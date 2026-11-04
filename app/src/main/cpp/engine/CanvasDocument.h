// PxPaint - canvas document: the content of the open canvas, shared between
// the UI side (strokes, layers, persistence) and the render thread (GPU sync).
//
//   CanvasDocument
//     layers_      : the layer stack, bottom first. Each layer owns its own
//                    tiled RasterStore (premultiplied RGBA8, sparse: only
//                    tiles with content exist) and its properties (id, name,
//                    visibility, lock, opacity, blend mode).
//     composite_   : the visible layers composited over the backdrop
//                    (canvas/Compositor). This is what the GPU displays: the
//                    render thread uploads its dirty regions, exactly as it
//                    uploaded the single raster of Blocks 2-5.
//     BrushEngine  : stroke input queue + processing into the ACTIVE layer.
//
// The composite is a cache: only regions whose layers changed are
// recomposited. Strokes on the active layer recomposite their dirty
// rectangles at once; property changes (visibility, opacity, blend, order,
// delete, merge) mark only the tiles where the affected layer has content,
// background changes every tile with content; those stale tiles are
// recomposited by the render thread within a few milliseconds per frame, so
// big documents stay responsive. Nothing is ever flattened implicitly.
//
// Threading: every method is thread-safe. Stroke input only touches the
// brush engine's queue (never blocks on rendering). Everything else is
// guarded by one mutex held briefly. Layer operations first apply queued
// strokes (and commit one still in progress) so they happen in input order.
//
// generation() changes when the document is replaced (open/close) so the
// render thread knows its GPU copy must be rebuilt from scratch.
#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "brush/BrushEngine.h"
#include "canvas/Compositor.h"
#include "canvas/RasterStore.h"
#include "canvas/SelectionMask.h"
#include "engine/History.h"
#include "engine/LayerTypes.h"

namespace pe {

// Legacy content (Block 2 projects): a rectangle filled - replaced, not
// blended - with a straight ARGB colour. Still imported, never written.
struct ContentOp {
    int32_t x = 0, y = 0, w = 0, h = 0;
    uint32_t argb = 0;
};


struct DocumentStats {
    bool open = false;
    uint32_t width = 0, height = 0;
    uint64_t generation = 0;
    uint64_t revision = 0;
    size_t tilesAllocated = 0;     // content tiles of all layers
    uint64_t bytesAllocated = 0;   // CPU raster memory: layers + composite
    size_t layerCount = 0;
    size_t layerTiles = 0;         // tiles of all layers
    size_t compositeTiles = 0;
    double lastCompositeMs = 0;    // last composite update (render thread)
    double maxCompositeMs = 0;
    uint64_t compositedPixels = 0;
    brush::BrushStats brush;
    HistoryStats history;
};

class CanvasDocument : private HistoryTarget {
public:
    CanvasDocument();

    // Export / import record (little-endian):
    //   int32 count, then per tile: int32 tx, int32 ty, int32 flags,
    //   followed by RasterStore::kTileBytes of premultiplied RGBA8 unless
    //   flags & kTileEmpty (the tile is transparent / can be dropped).
    static constexpr int32_t kTileEmpty = 1;
    static constexpr size_t kRecordHeader = 12;
    static constexpr size_t kMaxLayers = 100;

    // --- Lifecycle (any thread) ---------------------------------------------
    // A new canvas has one empty layer ("Layer 1", id 1).
    void open(uint32_t width, uint32_t height);
    void close();
    // Clears the ACTIVE layer.
    LayerError clear();
    bool isOpen() const;
    uint64_t generation() const { return generation_.load(std::memory_order_acquire); }
    // Increases with every content change request (strokes, fills, loads, layer edits).
    uint64_t revision() const { return revision_.load(std::memory_order_acquire); }
    // Paper the bottom layer blends with (opaque) or transparency.
    void setBackground(uint32_t paperArgb, bool transparent);
    // CPU raster budget for layer operations that allocate (duplicate, merge).
    void setMemoryBudget(uint64_t bytes);

    // --- Edits (any thread) ---------------------------------------------------
    void fill(const canvas::RectI& rect, uint32_t argb);  // active layer
    // Replaces the content with legacy rectangle ops (clipped) as one layer; returns ops kept.
    size_t loadLegacy(const std::vector<ContentOp>& ops);
    // False (stroke ignored) if the active layer is hidden or locked.
    bool strokeBegin(const brush::BrushSettings& settings, const brush::StrokePoint& first);
    void strokeSamples(const brush::StrokePoint* points, size_t count);
    void strokeEnd();
    void strokeCancel();
    bool hasPendingInput() const { return brush_.hasPending(); }
    bool activeEditable() const { return activeEditable_.load(std::memory_order_acquire); }
    // Tiles still to recomposite after a property change (the render thread
    // works through them a few milliseconds per frame).
    bool hasPendingComposite() const { return pendingComposite_.load(std::memory_order_acquire); }

    // --- Layers (any thread) --------------------------------------------------
    std::vector<LayerSummary> layers(uint32_t& activeId) const;
    // New empty layer above `aboveId` (0: on top); it becomes active.
    LayerError addLayer(uint32_t aboveId, const std::string& name, uint32_t& newId);
    LayerError deleteLayer(uint32_t id);
    // Copy above the original (pixels and properties); it becomes active.
    LayerError duplicateLayer(uint32_t id, const std::string& name, uint32_t& newId);
    LayerError renameLayer(uint32_t id, const std::string& name);
    // Moves to `index` (0 = bottom) in the stack.
    LayerError moveLayer(uint32_t id, int32_t index);
    LayerError setVisible(uint32_t id, bool visible);
    LayerError setLocked(uint32_t id, bool locked);
    LayerError setOpacity(uint32_t id, float opacity);
    LayerError setBlend(uint32_t id, canvas::BlendMode mode);
    LayerError setActive(uint32_t id);
    // Composites `id` onto the layer below (the result keeps the lower layer's
    // id, name, blend mode; both opacities are baked in: opacity 100 %).
    LayerError mergeDown(uint32_t id);
    // Every visible layer composited into one layer; hidden layers are removed.
    LayerError flatten(const std::string& name);
    // Replaces the stack with empty layers with these properties (project
    // load; content follows with importLayerTiles). False if invalid.
    bool setLayerStack(const std::vector<LayerInfo>& stack, uint32_t activeId);

    // --- History (any thread; Block 7) ------------------------------------------
    // Strokes, erasing, clear / fill and every layer operation are recorded
    // when they complete (a stroke in progress, cancelled or refused is not).
    // Selecting the active layer is not an edit and is not recorded. Loading
    // or replacing content clears the history.
    // 1 done, 0 nothing to undo / redo, -1 refused (history cleared).
    int undo();
    int redo();
    void clearHistory();
    void setHistoryBudget(uint64_t bytes, size_t maxSteps);
    // Both consume queued input first (an ended stroke is a step as soon as it
    // is queued); a stroke still in progress is not a step yet.
    std::vector<HistoryEntry> historyEntries();
    HistoryStats historyStats();
    // (undo steps << 32) | redo steps; lock-free, does not consume input (UI buttons).
    uint64_t historyCounts() const { return history_.counts(); }

    // --- Selection (Block 10; any thread) --------------------------------------
    // The selection is a coverage mask independent from the layers (canvas/
    // SelectionMask). With a selection, strokes and erasing are clipped to it.
    // Every change is one history step; pixel operations act on the active
    // layer only (copyMerged reads the visible image). Queued strokes are
    // applied first, and a move in progress is committed first.
    struct SelectionInfo {
        bool active = false;
        canvas::RectI bounds;
        double area = 0;          // selected pixels (coverage summed)
        uint64_t bytes = 0;       // mask pixel memory (partial tiles)
        uint64_t clipboardBytes = 0;
        uint64_t version = 0;     // changes with the mask (outline caches)
        bool moving = false;
        int32_t dx = 0, dy = 0;   // offset of the content being moved
        bool clipboard = false;
        canvas::RectI clipboardBounds;
    };
    LayerError select(const float* canvasXY, size_t points, canvas::SelectionMask::Mode mode, bool antialias);
    LayerError selectAll();
    LayerError deselect();
    LayerError invertSelection();
    // Visible content (all visible layers, layer opacity applied) as a shape.
    LayerError selectFromContent(canvas::SelectionMask::Mode mode);
    LayerError clearSelected();
    LayerError copySelection(bool merged);
    LayerError cutSelection();
    LayerError paste(uint32_t& newId);
    LayerError duplicateSelection(uint32_t& newId);
    // Moving the selected pixels of the active layer (whole canvas pixels):
    // begin lifts them into a floating layer above it, moveTo offsets that,
    // commit drops them (one history step with the selection), cancel restores.
    // A future transform engine extends this floating content.
    LayerError moveBegin();
    LayerError moveTo(int32_t dx, int32_t dy);
    // Any affine transform of the lifted content (floating -> canvas): move,
    // scale, rotate, flip, skew (Block 12). Integer translations copy pixels
    // exactly; other transforms sample bilinearly.
    LayerError transformTo(const canvas::Affine2& m);
    LayerError moveCommit();
    void moveCancel();
    SelectionInfo selectionInfo();
    void selectionContours(std::vector<float>& out);
    void selectionPreview(uint32_t maxSide, uint32_t& w, uint32_t& h, std::vector<uint8_t>& out);
    uint8_t selectionAt(int32_t x, int32_t y);
    uint64_t selectionChecksum();
    // (version << 1) | active: cheap, for the UI to notice changes (undo / redo included).
    uint64_t selectionVersion() const;

    // --- Persistence (any thread) --------------------------------------------
    // Applies queued input, then appends up to `maxTiles` tiles of layer `id`
    // (changed since their last export if `changedOnly`, else every allocated
    // tile) to `out` in the record format, marking them saved. Returns the
    // number of tiles (0 also when the layer does not exist).
    size_t exportLayerTiles(uint32_t id, bool changedOnly, size_t maxTiles, std::vector<uint8_t>& out);
    // Imports records into layer `id`. Returns tiles imported, or -1 if the
    // data is malformed / the layer does not exist.
    long importLayerTiles(uint32_t id, const uint8_t* data, size_t size);
    // Single-layer API of Blocks 3-5: the active layer.
    size_t exportTiles(bool changedOnly, size_t maxTiles, std::vector<uint8_t>& out);
    long importTiles(const uint8_t* data, size_t size);
    // Box-filtered preview (straight ARGB, transparent where nothing shows):
    // of the composite, or of one layer's own content (visibility ignored).
    bool thumbnail(uint32_t maxSide, uint32_t& width, uint32_t& height, std::vector<uint32_t>& argb);
    bool layerThumbnail(uint32_t id, uint32_t maxSide, uint32_t& width, uint32_t& height, std::vector<uint32_t>& argb);

    // --- Render thread ----------------------------------------------------------
    // Applies queued input, updates the composite; then, if the document is
    // still `generation`, calls fn(composite) under the lock (to copy dirty
    // regions to the GPU). Returns false if the generation changed.
    template <typename Fn>
    bool syncGpu(uint64_t generation, Fn&& fn) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (open_) updateLocked(false);
        if (generation != generation_.load(std::memory_order_relaxed) || !open_) return false;
        fn(composite_);
        return true;
    }
    int64_t takeConsumedInputTime() {
        std::lock_guard<std::mutex> lock(mutex_);
        return brush_.takeConsumedInputTime();
    }
    bool size(uint64_t generation, uint32_t& width, uint32_t& height) const;

    DocumentStats stats() const;
    // Composite pixel (what is displayed, paper included where opaque) and
    // a layer's own pixel (tests).
    canvas::Rgba8 pixel(int32_t x, int32_t y);
    // Eyedropper: straight (non-premultiplied) opaque ARGB of what is shown
    // at canvas pixel (x, y), paper included; false outside the canvas or
    // where nothing is painted over a transparent background. Reads the
    // composite as displayed (no compositing work: callable from the UI).
    bool sampleColor(int32_t x, int32_t y, uint32_t& argb) const;
    canvas::Rgba8 layerPixel(uint32_t id, int32_t x, int32_t y);
    // Exact fingerprint of a layer's pixels / of the composite (transparent
    // tiles count as absent): equal checksums = identical pictures (tests).
    uint64_t layerChecksum(uint32_t id);
    uint64_t compositeChecksum();

private:
    using Layer = LayerData;
    void bumpRevision() { revision_.fetch_add(1, std::memory_order_acq_rel); }
    // Under mutex_: strokes into the active layer, then the composite. With
    // `complete` false, stale tiles are recomposited within a time budget
    // (the rest at the next frames); true finishes them (exports, tests).
    void updateLocked(bool complete = true);
    void markTilesOf(const canvas::RasterStore& s);
    void markAllContent();
    // Under mutex_: queued strokes applied and a stroke in progress committed.
    void settleInputLocked();
    // Under mutex_: queued strokes applied (a move in progress lands first).
    void processInputLocked();
    int32_t indexOf(uint32_t id) const;
    Layer* activeLayer();
    void refreshEditable();
    void invalidateAll() { markAllContent(); }
    uint64_t layerBytes() const;
    std::string uniqueName(const std::string& base) const;
    bool thumbnailOf(uint32_t layerId, bool composite, uint32_t maxSide, uint32_t& w, uint32_t& h,
                     std::vector<uint32_t>& argb);

    mutable std::mutex mutex_;
    std::vector<std::unique_ptr<Layer>> layers_;  // bottom first; guarded by mutex_
    uint32_t activeId_ = 0;
    uint32_t nextId_ = 1;
    canvas::RasterStore composite_;               // guarded by mutex_
    canvas::Backdrop backdrop_;
    std::vector<uint8_t> pending_;                // per tile: needs recompositing
    size_t pendingCount_ = 0;
    bool open_ = false;
    uint64_t budget_ = 1024ull << 20;
    double lastCompositeMs_ = 0, maxCompositeMs_ = 0;
    uint64_t compositedPixels_ = 0;
    std::vector<canvas::RectI> dirtyScratch_;
    std::vector<canvas::LayerView> viewScratch_;
    brush::BrushEngine brush_;    // input side self-synchronised; processing under mutex_
    History history_;             // guarded by mutex_
    // Block 10 (guarded by mutex_)
    canvas::SelectionMask selection_;
    canvas::SelectionMask shape_;                 // scratch: the shape being combined
    canvas::RasterStore clipboard_;               // copied / cut pixels (session, any document)
    bool hasClipboard_ = false;
    // Moving / transforming selected content (Blocks 10, 12). The selected
    // pixels of the active layer are lifted into `floating` (selection coverage
    // applied). The preview is non-destructive: affected tiles are recomposed
    // into `preview` (hole = original x (1 - selection), the transformed
    // floating pixels drawn over it) and the compositor reads them instead of
    // the layer's, which is untouched until the transform is confirmed. What
    // is displayed is exactly what the confirmation writes, with the layer's
    // opacity and blend mode.
    struct Move {
        bool active = false;
        uint32_t layerId = 0;                     // the active layer being edited
        canvas::RasterStore floating;             // lifted pixels, untransformed
        canvas::RectI bounds;                     // selection bounds when lifted
        canvas::Affine2 xf;                       // floating -> canvas
        bool identity = true;
        canvas::RasterStore preview;              // recomposed tiles (only those affected)
        std::vector<uint8_t> previewTiles;        // per tile: read from `preview`
        std::vector<int32_t> shown;               // tiles in the preview now
        std::vector<int32_t> next, work;          // scratch (reused while dragging)
        std::vector<uint8_t> scratch;
        std::vector<canvas::RectI> dirty;         // preview regions to composite at the next frame
        std::vector<uint8_t> selectionBefore;
    } move_;
    void composeMoveTileLocked(Layer& l, int32_t index, std::vector<uint8_t>& scratch);
    void commitMoveLocked();
    void cancelMoveLocked();
    void recordSelection(const std::vector<uint8_t>& before);
    // Applies `op` to every tile of the active layer that the selection touches
    // (op(px, mask px or null when fully selected) modifies px in place) as one step.
    template <typename Fn>
    LayerError editSelectedLocked(HistoryKind kind, Fn&& op);
    bool copySelectedLocked(bool merged, canvas::RasterStore& out);
    LayerError insertStoreAsLayerLocked(const canvas::RasterStore& src, const std::string& name, HistoryKind kind, uint32_t& newId);

    // HistoryTarget (called by history commands under mutex_).
    LayerData* findLayer(uint32_t id) override;
    int32_t layerIndex(uint32_t id) const override { return indexOf(id); }
    void insertLayer(int32_t index, std::unique_ptr<LayerData> layer) override;
    std::unique_ptr<LayerData> removeLayer(uint32_t id) override;
    void moveLayerTo(uint32_t id, int32_t index) override;
    void setActiveLayer(uint32_t id) override;
    void layerPixelsChanged(uint32_t id, const canvas::RectI& rect) override;
    void layerPropertiesChanged(uint32_t id) override;
    void stackChanged() override { markAllContent(); }
    bool restoreSelection(const std::vector<uint8_t>& encoded) override;
    // Records a property change of layer `id` made by `apply`.
    template <typename Fn>
    void recordProperty(HistoryKind kind, Layer& l, Fn&& apply) {
        // Strokes already ended are steps before this one (chronological order);
        // a stroke still in progress continues (an opacity drag does not cut it).
        commitMoveLocked();
        if (Layer* a = activeLayer()) brush_.process(a->store);
        const LayerInfo before = l.info;
        apply();
        auto cmd = std::make_unique<PropertyCommand>(kind, before, l.info);
        cmd->setLabel(l.info.name);
        history_.push(std::move(cmd));
    }
    void onStrokeCommitted(const canvas::RasterStore& store, const std::vector<brush::StrokeCompositor::CommittedTile>& tiles,
                           bool erase);
    std::atomic<bool> activeEditable_{false};
    std::atomic<bool> pendingComposite_{false};
    std::atomic<uint64_t> generation_{0};
    std::atomic<uint64_t> revision_{0};
};

}  // namespace pe
