// PxPaint canvas - CPU raster store (the authoritative canvas pixels).
//
// Premultiplied RGBA8 pixels in 256 x 256 tiles, allocated on first write.
// It is the source of truth for the canvas content:
//   * the brush engine paints into it (CPU, deterministic, antialiased);
//   * the GPU storage (CanvasSurface) is a cache refreshed from it through
//     dirty rectangles (partial uploads, never the whole canvas per sample);
//   * persistence exports/imports its tiles (no GPU readback anywhere);
//   * a lost device / backend switch simply re-uploads every tile.
//
// Not synchronized: CanvasDocument guards it with its mutex.
#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include "canvas/CanvasMath.h"
#include "canvas/TileGrid.h"

namespace pe::canvas {

// Premultiplied RGBA8.
struct Rgba8 {
    uint8_t r = 0, g = 0, b = 0, a = 0;
    bool operator==(const Rgba8& o) const { return r == o.r && g == o.g && b == o.b && a == o.a; }
    bool operator!=(const Rgba8& o) const { return !(*this == o); }
    // Straight (non-premultiplied) 0xAARRGGBB, as used by Android.
    static Rgba8 fromArgb(uint32_t argb);
    // Back to straight 0xAARRGGBB (rounded).
    uint32_t toArgb() const;
};

class RasterStore {
public:
    static constexpr int32_t kTile = 256;
    static constexpr size_t kTileStride = static_cast<size_t>(kTile) * 4;
    static constexpr size_t kTileBytes = static_cast<size_t>(kTile) * kTile * 4;

    RasterStore() = default;
    RasterStore(const RasterStore&) = delete;
    RasterStore& operator=(const RasterStore&) = delete;
    RasterStore(RasterStore&&) = default;
    RasterStore& operator=(RasterStore&&) = default;

    // New empty canvas (every tile freed).
    void reset(uint32_t width, uint32_t height);
    // Frees every tile (content becomes transparent).
    void clear();

    uint32_t width() const { return width_; }
    uint32_t height() const { return height_; }
    int32_t columns() const { return cols_; }
    int32_t rows() const { return rows_; }
    size_t tileCount() const { return tiles_.size(); }
    bool validTile(int32_t tx, int32_t ty) const { return tx >= 0 && ty >= 0 && tx < cols_ && ty < rows_; }
    size_t index(int32_t tx, int32_t ty) const { return static_cast<size_t>(ty) * cols_ + tx; }
    RectI canvasRect() const { return {0, 0, static_cast<int32_t>(width_), static_cast<int32_t>(height_)}; }
    // Canvas pixels of a tile (edge tiles clipped to the canvas).
    RectI tileRect(int32_t tx, int32_t ty) const;
    // Range of tiles intersecting `r` (inclusive, clipped to the grid). False if none.
    bool tileRange(const RectI& r, int32_t& tx0, int32_t& ty0, int32_t& tx1, int32_t& ty1) const;

    // Pixels of a tile (kTile x kTile, row stride kTileStride); null if never written.
    const uint8_t* tile(int32_t tx, int32_t ty) const;
    // Allocates (transparent) on first use. Null only for an invalid tile.
    uint8_t* tileForWrite(int32_t tx, int32_t ty);
    bool tileAllocated(int32_t tx, int32_t ty) const { return tile(tx, ty) != nullptr; }
    size_t tilesAllocated() const { return allocated_; }
    // Frees a tile (it reads as transparent again). Caller marks the change.
    void releaseTile(int32_t tx, int32_t ty);
    // Copies every tile of `other` (same size); this store becomes a copy.
    void copyFrom(const RasterStore& other);
    uint64_t bytesAllocated() const { return static_cast<uint64_t>(allocated_) * kTileBytes; }

    // Records a content change of `r`: GPU dirty region, unsaved tile, version.
    void markChanged(const RectI& r);
    uint64_t version() const { return version_; }

    // Replace-fill (legacy rectangle content, diagnostics). Clipped.
    void fill(const RectI& r, Rgba8 color);
    Rgba8 pixel(int32_t x, int32_t y) const;  // transparent outside / unallocated

    // --- GPU synchronisation (render thread, under the document lock) -----
    // Moves out the dirty rectangle of every changed tile (one per tile).
    void takeGpuDirty(std::vector<RectI>& out);
    // Every allocated tile dirty (new GPU storage / device restore).
    void markAllGpuDirty();
    bool hasGpuDirty() const { return gpuDirtyTiles_ > 0; }

    // --- Persistence -------------------------------------------------------
    // Tiles changed since they were last exported (sorted by index).
    void unsavedTiles(std::vector<TileCoord>& out) const;
    void markSaved(int32_t tx, int32_t ty);
    void markAllUnsaved();  // e.g. after a legacy import
    // Every tile position unsaved, allocated or not: the next save rewrites
    // the whole layer (empty records drop what a file had there).
    void markEverythingUnsaved();
    bool tileEmpty(int32_t tx, int32_t ty) const;  // unallocated or all alpha == 0
    // Replaces a tile with `rgba` (kTileBytes, premultiplied). Pixels outside
    // the canvas are cleared. Does not mark it unsaved (it came from disk).
    bool importTile(int32_t tx, int32_t ty, const uint8_t* rgba);

private:
    struct Tile {
        std::unique_ptr<uint8_t[]> px;
        RectI gpuDirty;          // canvas coordinates, empty if clean
        bool unsaved = false;
    };
    uint32_t width_ = 0, height_ = 0;
    int32_t cols_ = 0, rows_ = 0;
    std::vector<Tile> tiles_;
    size_t allocated_ = 0;
    size_t gpuDirtyTiles_ = 0;
    uint64_t version_ = 0;
};

}  // namespace pe::canvas
