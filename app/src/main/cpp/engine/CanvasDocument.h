// PxPaint - canvas document: the content of the open canvas, shared between
// the UI side (strokes, persistence) and the render thread (GPU sync).
//
//   CanvasDocument
//     RasterStore  : authoritative pixels (CPU, tiled, premultiplied RGBA8)
//     BrushEngine  : stroke input queue + processing into the store
//
// Threading: every method is thread-safe. Stroke input only touches the
// brush engine's queue (never blocks on rendering). Store access is guarded
// by one mutex held briefly: by the render thread while it processes queued
// input and copies dirty regions to the GPU, and per tile by exports and
// thumbnails, so a save never stalls drawing for more than one tile copy.
//
// generation() changes when the document is replaced (open/close/clear) so
// the render thread knows its GPU copy must be rebuilt from scratch.
#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <vector>

#include "brush/BrushEngine.h"
#include "canvas/RasterStore.h"

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
    size_t tilesAllocated = 0;     // CPU raster tiles
    uint64_t bytesAllocated = 0;
    brush::BrushStats brush;
};

class CanvasDocument {
public:
    // Export / import record (little-endian):
    //   int32 count, then per tile: int32 tx, int32 ty, int32 flags,
    //   followed by RasterStore::kTileBytes of premultiplied RGBA8 unless
    //   flags & kTileEmpty (the tile is transparent / can be dropped).
    static constexpr int32_t kTileEmpty = 1;
    static constexpr size_t kRecordHeader = 12;

    // --- Lifecycle (any thread) ---------------------------------------------
    void open(uint32_t width, uint32_t height);
    void close();
    void clear();
    bool isOpen() const;
    uint64_t generation() const { return generation_.load(std::memory_order_acquire); }
    // Increases with every content change request (strokes, fills, loads).
    uint64_t revision() const { return revision_.load(std::memory_order_acquire); }

    // --- Edits (any thread) ---------------------------------------------------
    void fill(const canvas::RectI& rect, uint32_t argb);
    // Replaces the content with legacy rectangle ops (clipped); returns ops kept.
    size_t loadLegacy(const std::vector<ContentOp>& ops);
    void strokeBegin(const brush::BrushSettings& settings, const brush::StrokePoint& first);
    void strokeSamples(const brush::StrokePoint* points, size_t count);
    void strokeEnd();
    void strokeCancel();
    bool hasPendingInput() const { return brush_.hasPending(); }

    // --- Persistence (any thread) --------------------------------------------
    // Applies queued input, then appends up to `maxTiles` tiles (changed since
    // their last export if `changedOnly`, else every allocated tile) to `out`
    // in the record format, marking them saved. Returns the number of tiles.
    size_t exportTiles(bool changedOnly, size_t maxTiles, std::vector<uint8_t>& out);
    // Imports records (format above). Returns tiles imported, or -1 if the
    // data is malformed (nothing after the malformed record is applied).
    long importTiles(const uint8_t* data, size_t size);
    // Box-filtered preview of the content (straight ARGB, transparent where
    // nothing is painted). False if no canvas is open.
    bool thumbnail(uint32_t maxSide, uint32_t& width, uint32_t& height, std::vector<uint32_t>& argb);

    // --- Render thread ----------------------------------------------------------
    // Applies queued input; then, if the document is still `generation`,
    // calls fn(store) under the lock (to copy dirty regions to the GPU).
    // Returns false if the generation changed (caller must rebuild).
    template <typename Fn>
    bool syncGpu(uint64_t generation, Fn&& fn) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (open_) brush_.process(store_);
        if (generation != generation_.load(std::memory_order_relaxed) || !open_) return false;
        fn(store_);
        return true;
    }
    // See BrushEngine::takeConsumedInputTime (call after syncGpu, render thread).
    int64_t takeConsumedInputTime() {
        std::lock_guard<std::mutex> lock(mutex_);
        return brush_.takeConsumedInputTime();
    }
    // Size of the document of `generation` (false if it is not current).
    bool size(uint64_t generation, uint32_t& width, uint32_t& height) const;

    DocumentStats stats() const;
    // Pixel of the store (tests).
    canvas::Rgba8 pixel(int32_t x, int32_t y) const;

private:
    void bumpRevision() { revision_.fetch_add(1, std::memory_order_acq_rel); }

    mutable std::mutex mutex_;
    canvas::RasterStore store_;   // guarded by mutex_
    bool open_ = false;           // guarded by mutex_
    brush::BrushEngine brush_;    // input side self-synchronised; processing under mutex_
    std::atomic<uint64_t> generation_{0};
    std::atomic<uint64_t> revision_{0};
};

}  // namespace pe
