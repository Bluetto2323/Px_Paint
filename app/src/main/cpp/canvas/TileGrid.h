// PxPaint canvas - tile grid and storage strategy.
//
// A canvas is split into square tiles of `tileSize` canvas pixels; edge tiles
// are partial (clipped to the canvas). Each GPU tile texture also carries a
// 1-pixel gutter on every side holding its neighbours' pixels, so linear
// filtering never samples across a tile edge into garbage (no seams).
//
// StoragePlan decides, per canvas and per device, between a single texture
// (small/medium canvases) and tiles (large canvases), from the GPU's real
// limits and a memory budget - never from assumptions about the device.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "canvas/CanvasMath.h"

namespace pe::canvas {

struct TileCoord {
    int32_t tx = 0, ty = 0;
    bool operator==(const TileCoord& o) const { return tx == o.tx && ty == o.ty; }
};

class TileGrid {
public:
    static constexpr int32_t kGutter = 1;

    TileGrid() = default;
    TileGrid(uint32_t canvasWidth, uint32_t canvasHeight, uint32_t tileSize);

    uint32_t canvasWidth() const { return width_; }
    uint32_t canvasHeight() const { return height_; }
    uint32_t tileSize() const { return tileSize_; }
    int32_t columns() const { return cols_; }
    int32_t rows() const { return rows_; }
    size_t tileCount() const { return static_cast<size_t>(cols_) * static_cast<size_t>(rows_); }
    size_t index(TileCoord t) const { return static_cast<size_t>(t.ty) * cols_ + t.tx; }
    TileCoord coord(size_t index) const {
        return {static_cast<int32_t>(index % cols_), static_cast<int32_t>(index / cols_)};
    }

    RectI canvasRect() const { return {0, 0, static_cast<int32_t>(width_), static_cast<int32_t>(height_)}; }
    // Canvas pixels owned by a tile (clipped to the canvas: edge tiles are partial).
    RectI tileRect(TileCoord t) const;
    // Canvas area stored in the tile texture: tileRect grown by the gutter
    // (texture size is always tileSize + 2*gutter).
    RectI tileStorageRect(TileCoord t) const;
    uint32_t textureSize() const { return tileSize_ + 2 * kGutter; }

    // Tiles whose OWN pixels intersect `rect` (for drawing / culling).
    std::vector<TileCoord> tilesIntersecting(RectI rect) const;
    // Tiles whose STORAGE (incl. gutter) intersects `rect` (for writes:
    // neighbours' gutters must be updated too).
    std::vector<TileCoord> tilesStoring(RectI rect) const;

private:
    uint32_t width_ = 0, height_ = 0, tileSize_ = 256;
    int32_t cols_ = 0, rows_ = 0;
};

enum class StorageMode : uint8_t { SingleTexture, Tiled };
const char* toString(StorageMode mode);

struct StorageLimits {
    uint32_t maxTextureSize = 0;     // GPU limit for one 2D texture
    uint64_t memoryBudgetBytes = 0;  // what the canvas may use on this device
};

struct StoragePlan {
    bool ok = false;
    StorageMode mode = StorageMode::SingleTexture;
    uint32_t tileSize = 0;              // tiled mode
    uint64_t fullCanvasBytes = 0;       // if every pixel were allocated
    uint64_t maxAllocatedBytes = 0;     // worst case incl. tile gutters
    std::string reason;
};

class StoragePolicy {
public:
    static constexpr uint32_t kTileSize = 256;
    // A single texture is used only up to this size, even if the GPU allows
    // more: beyond it tiles give partial updates/culling for free.
    static constexpr uint32_t kSingleTextureMaxSide = 2048;
    static constexpr uint32_t kMaxCanvasSide = 16384;

    static StoragePlan choose(uint32_t width, uint32_t height, const StorageLimits& limits);
};

}  // namespace pe::canvas
