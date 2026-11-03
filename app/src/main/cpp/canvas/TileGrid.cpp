#include "canvas/TileGrid.h"

#include <algorithm>

namespace pe::canvas {

TileGrid::TileGrid(uint32_t canvasWidth, uint32_t canvasHeight, uint32_t tileSize)
    : width_(canvasWidth), height_(canvasHeight), tileSize_(tileSize > 0 ? tileSize : 256) {
    cols_ = static_cast<int32_t>((width_ + tileSize_ - 1) / tileSize_);
    rows_ = static_cast<int32_t>((height_ + tileSize_ - 1) / tileSize_);
}

RectI TileGrid::tileRect(TileCoord t) const {
    const int32_t ts = static_cast<int32_t>(tileSize_);
    return RectI{t.tx * ts, t.ty * ts, ts, ts}.intersect(canvasRect());
}

RectI TileGrid::tileStorageRect(TileCoord t) const {
    const int32_t ts = static_cast<int32_t>(tileSize_);
    return {t.tx * ts - kGutter, t.ty * ts - kGutter, ts + 2 * kGutter, ts + 2 * kGutter};
}

std::vector<TileCoord> TileGrid::tilesIntersecting(RectI rect) const {
    std::vector<TileCoord> out;
    const RectI r = rect.intersect(canvasRect());
    if (r.empty()) return out;
    const int32_t ts = static_cast<int32_t>(tileSize_);
    const int32_t x0 = r.x / ts, y0 = r.y / ts;
    const int32_t x1 = (r.right() - 1) / ts, y1 = (r.bottom() - 1) / ts;
    out.reserve(static_cast<size_t>(x1 - x0 + 1) * static_cast<size_t>(y1 - y0 + 1));
    for (int32_t ty = y0; ty <= y1; ++ty)
        for (int32_t tx = x0; tx <= x1; ++tx) out.push_back({tx, ty});
    return out;
}

std::vector<TileCoord> TileGrid::tilesStoring(RectI rect) const {
    // Storage rects overlap their neighbours by the gutter: grow the query.
    const RectI grown{rect.x - kGutter, rect.y - kGutter, rect.w + 2 * kGutter, rect.h + 2 * kGutter};
    return tilesIntersecting(grown);
}

const char* toString(StorageMode mode) {
    return mode == StorageMode::SingleTexture ? "single texture" : "tiled";
}

StoragePlan StoragePolicy::choose(uint32_t width, uint32_t height, const StorageLimits& limits) {
    StoragePlan plan;
    plan.fullCanvasBytes = static_cast<uint64_t>(width) * height * 4u;
    if (width == 0 || height == 0) {
        plan.reason = "empty canvas";
        return plan;
    }
    if (width > kMaxCanvasSide || height > kMaxCanvasSide) {
        plan.reason = "canvas side above " + std::to_string(kMaxCanvasSide) + " px";
        return plan;
    }
    if (limits.maxTextureSize < kTileSize + 2u * TileGrid::kGutter) {
        plan.reason = "GPU max texture size " + std::to_string(limits.maxTextureSize) + " too small";
        return plan;
    }
    const uint32_t singleMax = std::min(limits.maxTextureSize, kSingleTextureMaxSide);
    const TileGrid grid(width, height, kTileSize);
    // The CPU raster (authoritative pixels, see RasterStore) uses whole
    // 256 px tiles whatever the GPU strategy.
    plan.maxCpuBytes = static_cast<uint64_t>(grid.tileCount()) * kTileSize * kTileSize * 4u;
    if (width <= singleMax && height <= singleMax && !limits.forceTiled) {
        plan.mode = StorageMode::SingleTexture;
        plan.maxGpuBytes = plan.fullCanvasBytes;
    } else {
        plan.mode = StorageMode::Tiled;
        plan.tileSize = kTileSize;
        const uint64_t tex = grid.textureSize();
        plan.maxGpuBytes = static_cast<uint64_t>(grid.tileCount()) * tex * tex * 4u;
    }
    plan.maxAllocatedBytes = plan.maxGpuBytes + plan.maxCpuBytes;
    if (limits.memoryBudgetBytes > 0 && plan.maxAllocatedBytes > limits.memoryBudgetBytes) {
        plan.reason = "needs up to " + std::to_string(plan.maxAllocatedBytes >> 20) + " MiB, budget " +
                      std::to_string(limits.memoryBudgetBytes >> 20) + " MiB";
        return plan;
    }
    plan.ok = true;
    plan.reason = std::string(toString(plan.mode)) +
                  (plan.mode == StorageMode::Tiled ? " (" + std::to_string(kTileSize) + " px tiles, allocated on demand)"
                                                   : " (fits one texture)");
    return plan;
}

}  // namespace pe::canvas
