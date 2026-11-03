// PxPaint - history engine (undo / redo), Block 7.
//
// Command-based: every editing operation of the document becomes one
// HistoryCommand that can undo and redo itself exactly on a HistoryTarget
// (the document). The History keeps the undo and redo stacks, chronological
// order, redo invalidation, coalescing (an opacity slider drag is one step)
// and the memory budget. It knows nothing about the UI or the GPU: the GPU
// only ever sees the composite, which the document rebuilds after undo/redo
// like after any other change (no GPU resource is ever held by history).
//
// Pixels: a PixelPatch records, per tile and only over the rectangle that
// changed, the XOR of the pixels before and after the change, run-length
// encoded (unchanged pixels XOR to zero and cost almost nothing). Undo and
// redo apply the same XOR: exact, bit for bit, alpha included. Before
// touching anything, the current pixels of every rectangle are checked
// against the hash recorded for the state the patch expects; if anything
// changed outside the history the step is refused and the history cleared,
// so the canvas is never corrupted by a stale patch.
//
// Layers: commands that remove a layer (delete, merge down, flatten) keep
// the removed layer itself - id, properties and tiles - so undoing puts the
// very same layer back. Their memory is the memory the layer already used;
// it is counted in the budget and released when the step is discarded.
#pragma once

#include <cstdint>
#include <atomic>
#include <deque>
#include <memory>
#include <string>
#include <vector>

#include "engine/LayerTypes.h"

namespace pe {

enum class HistoryKind : int32_t {
    Stroke = 0,
    Erase = 1,
    Clear = 2,
    Fill = 3,
    AddLayer = 4,
    DeleteLayer = 5,
    DuplicateLayer = 6,
    MoveLayer = 7,
    RenameLayer = 8,
    Visibility = 9,
    Lock = 10,
    Opacity = 11,
    Blend = 12,
    MergeDown = 13,
    Flatten = 14,
};
const char* historyKindName(HistoryKind k);

// What commands act on (implemented by CanvasDocument, under its lock).
class HistoryTarget {
public:
    virtual ~HistoryTarget() = default;
    virtual LayerData* findLayer(uint32_t id) = 0;
    virtual int32_t layerIndex(uint32_t id) const = 0;
    // Inserts at `index` (0 = bottom); the layer's tiles are all marked to save.
    virtual void insertLayer(int32_t index, std::unique_ptr<LayerData> layer) = 0;
    virtual std::unique_ptr<LayerData> removeLayer(uint32_t id) = 0;
    virtual void moveLayerTo(uint32_t id, int32_t index) = 0;
    virtual void setActiveLayer(uint32_t id) = 0;
    // A layer's pixels / properties changed (composite and save follow).
    virtual void layerPixelsChanged(uint32_t id, const canvas::RectI& rect) = 0;
    virtual void layerPropertiesChanged(uint32_t id) = 0;
    // Every tile of every layer recomposited (structure changed).
    virtual void stackChanged() = 0;
};

// XOR delta of one tile over the rectangle that changed.
struct TileDelta {
    int32_t tx = 0, ty = 0;
    canvas::RectI rect;              // canvas coordinates, inside the tile
    bool existedBefore = false;      // the tile was allocated before the change
    bool existedAfter = false;
    uint64_t hashBefore = 0, hashAfter = 0;  // of the rect's pixels
    std::vector<uint32_t> rle;       // [zeros, n, n words] ...
    uint64_t bytes() const { return rle.capacity() * sizeof(uint32_t) + sizeof(TileDelta); }
};

class PixelPatch {
public:
    // Records one tile: `before` = previous pixels of the whole tile (null:
    // the tile did not exist), `after` = current tile (null: does not exist).
    void addTile(int32_t tx, int32_t ty, const canvas::RectI& rect, const uint8_t* before, const uint8_t* after);
    bool empty() const { return tiles_.empty(); }
    uint64_t bytes() const { return bytes_; }
    size_t tileCount() const { return tiles_.size(); }
    // Applies the patch to `store` in the direction given; false (nothing
    // changed) if the store is not in the expected state.
    bool apply(canvas::RasterStore& store, bool undo, canvas::RectI* changed) const;

    static uint64_t hashRect(const uint8_t* tile, const canvas::RectI& rect, int32_t tx, int32_t ty);

private:
    std::vector<TileDelta> tiles_;
    uint64_t bytes_ = 0;
};

class HistoryCommand {
public:
    virtual ~HistoryCommand() = default;
    virtual HistoryKind kind() const = 0;
    virtual uint64_t bytes() const = 0;
    // False: refused (the document was not in the state the command expects).
    virtual bool undo(HistoryTarget& t) = 0;
    virtual bool redo(HistoryTarget& t) = 0;
    // Absorbs `next` (same kind of continuous edit); true if it did.
    virtual bool absorb(const HistoryCommand& /*next*/) { return false; }
    const std::string& label() const { return label_; }  // layer name, for the history list
    void setLabel(std::string l) { label_ = std::move(l); }

private:
    std::string label_;
};

// ----------------------------------------------------------- commands
class PixelCommand : public HistoryCommand {
public:
    PixelCommand(HistoryKind k, uint32_t layerId, PixelPatch patch) : kind_(k), layer_(layerId), patch_(std::move(patch)) {}
    HistoryKind kind() const override { return kind_; }
    uint64_t bytes() const override { return patch_.bytes() + sizeof(*this); }
    bool undo(HistoryTarget& t) override { return apply(t, true); }
    bool redo(HistoryTarget& t) override { return apply(t, false); }
    const PixelPatch& patch() const { return patch_; }

private:
    bool apply(HistoryTarget& t, bool undo);
    HistoryKind kind_;
    uint32_t layer_;
    PixelPatch patch_;
};

// Add / duplicate: undo takes the layer out (kept for redo).
class InsertLayerCommand : public HistoryCommand {
public:
    InsertLayerCommand(HistoryKind k, uint32_t id, int32_t index, uint32_t activeBefore)
        : kind_(k), id_(id), index_(index), activeBefore_(activeBefore) {}
    HistoryKind kind() const override { return kind_; }
    uint64_t bytes() const override;
    bool undo(HistoryTarget& t) override;
    bool redo(HistoryTarget& t) override;

private:
    HistoryKind kind_;
    uint32_t id_;
    int32_t index_;
    uint32_t activeBefore_;
    std::unique_ptr<LayerData> kept_;
};

// Delete: keeps the layer itself (same id, properties, tiles).
class DeleteLayerCommand : public HistoryCommand {
public:
    DeleteLayerCommand(std::unique_ptr<LayerData> layer, int32_t index, uint32_t activeBefore, uint32_t activeAfter)
        : id_(layer->info.id), index_(index), activeBefore_(activeBefore), activeAfter_(activeAfter), kept_(std::move(layer)) {}
    HistoryKind kind() const override { return HistoryKind::DeleteLayer; }
    uint64_t bytes() const override;
    bool undo(HistoryTarget& t) override;
    bool redo(HistoryTarget& t) override;

private:
    uint32_t id_;
    int32_t index_;
    uint32_t activeBefore_, activeAfter_;
    std::unique_ptr<LayerData> kept_;
};

class MoveLayerCommand : public HistoryCommand {
public:
    MoveLayerCommand(uint32_t id, int32_t from, int32_t to) : id_(id), from_(from), to_(to) {}
    HistoryKind kind() const override { return HistoryKind::MoveLayer; }
    uint64_t bytes() const override { return sizeof(*this); }
    bool undo(HistoryTarget& t) override;
    bool redo(HistoryTarget& t) override;

private:
    uint32_t id_;
    int32_t from_, to_;
};

// Rename, visibility, lock, opacity, blend: the properties before / after.
class PropertyCommand : public HistoryCommand {
public:
    PropertyCommand(HistoryKind k, const LayerInfo& before, const LayerInfo& after) : kind_(k), before_(before), after_(after) {}
    HistoryKind kind() const override { return kind_; }
    uint64_t bytes() const override { return sizeof(*this) + before_.name.size() + after_.name.size(); }
    bool undo(HistoryTarget& t) override { return set(t, before_); }
    bool redo(HistoryTarget& t) override { return set(t, after_); }
    bool absorb(const HistoryCommand& next) override;

private:
    bool set(HistoryTarget& t, const LayerInfo& v);
    HistoryKind kind_;
    LayerInfo before_, after_;
};

// Merge down: keeps the upper layer and the lower layer's previous pixels
// and opacity; the merged pixels are kept while undone (for redo).
class MergeDownCommand : public HistoryCommand {
public:
    MergeDownCommand(std::unique_ptr<LayerData> upper, int32_t upperIndex, uint32_t lowerId, canvas::RasterStore lowerBefore,
                     float lowerOpacityBefore, uint32_t activeBefore, uint32_t activeAfter)
        : upperIndex_(upperIndex), lowerId_(lowerId), lowerOpacity_(lowerOpacityBefore), activeBefore_(activeBefore),
          activeAfter_(activeAfter), upper_(std::move(upper)), otherStore_(std::move(lowerBefore)) {
        upperId_ = upper_ ? upper_->info.id : 0;
    }
    HistoryKind kind() const override { return HistoryKind::MergeDown; }
    uint64_t bytes() const override;
    bool undo(HistoryTarget& t) override;
    bool redo(HistoryTarget& t) override;

private:
    int32_t upperIndex_;
    uint32_t lowerId_;
    float lowerOpacity_;
    uint32_t activeBefore_, activeAfter_;
    std::unique_ptr<LayerData> upper_;
    canvas::RasterStore otherStore_;  // undone: merged pixels; done: the lower layer's original pixels
    uint32_t upperId_ = 0;
};

// Flatten: keeps every original layer; the flattened layer while undone.
class FlattenCommand : public HistoryCommand {
public:
    FlattenCommand(std::vector<std::unique_ptr<LayerData>> before, uint32_t resultId, uint32_t activeBefore)
        : before_(std::move(before)), resultId_(resultId), activeBefore_(activeBefore) {
        for (const auto& l : before_) idsBefore_.push_back(l->info.id);
    }
    HistoryKind kind() const override { return HistoryKind::Flatten; }
    uint64_t bytes() const override;
    bool undo(HistoryTarget& t) override;
    bool redo(HistoryTarget& t) override;

private:
    std::vector<std::unique_ptr<LayerData>> before_;
    std::vector<uint32_t> idsBefore_;  // bottom first
    std::unique_ptr<LayerData> result_;
    uint32_t resultId_;
    uint32_t activeBefore_;
};

struct HistoryEntry {
    HistoryKind kind;
    std::string label;
    bool undone;  // in the redo part
};

struct HistoryStats {
    size_t undoSteps = 0, redoSteps = 0;
    uint64_t bytes = 0, budget = 0;
    size_t maxSteps = 0;
    uint64_t dropped = 0;    // oldest steps discarded for the budget / step limit
    uint64_t refused = 0;    // steps refused (document not in the expected state)
    double lastUndoMs = 0, lastRedoMs = 0, lastRecordMs = 0;
};

class History {
public:
    // Records a completed operation: clears the redo part, may absorb into
    // the previous step, then trims the oldest steps to the budget (the
    // newest step is always kept: the current canvas is never touched).
    void push(std::unique_ptr<HistoryCommand> cmd);
    // 1 done, 0 nothing to do, -1 refused (history cleared).
    int undo(HistoryTarget& t);
    int redo(HistoryTarget& t);
    void clear();
    bool canUndo() const { return !undo_.empty(); }
    bool canRedo() const { return !redo_.empty(); }
    void setBudget(uint64_t bytes, size_t maxSteps);
    std::vector<HistoryEntry> entries() const;  // oldest first, then the undone steps (next redo first)
    HistoryStats stats() const;
    void noteRecordTime(double ms) { lastRecordMs_ = ms; }
    // (undo steps << 32) | redo steps, lock-free (the UI's button states).
    uint64_t counts() const { return counts_.load(std::memory_order_acquire); }

    static constexpr uint64_t kDefaultBudget = 128ull << 20;
    static constexpr size_t kDefaultMaxSteps = 100;

private:
    void trim();
    void publish();
    std::atomic<uint64_t> counts_{0};
    std::deque<std::unique_ptr<HistoryCommand>> undo_;
    std::vector<std::unique_ptr<HistoryCommand>> redo_;  // back = next redo
    uint64_t bytes_ = 0;
    uint64_t budget_ = kDefaultBudget;
    size_t maxSteps_ = kDefaultMaxSteps;
    uint64_t dropped_ = 0, refused_ = 0;
    double lastUndoMs_ = 0, lastRedoMs_ = 0, lastRecordMs_ = 0;
};

}  // namespace pe
