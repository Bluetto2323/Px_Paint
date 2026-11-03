// PxPaint canvas - GPU storage of one raster surface (render thread only).
//
// The surface holds premultiplied RGBA8 pixels in either
//   * one texture of the canvas size (StorageMode::SingleTexture), or
//   * square tiles allocated on first write (StorageMode::Tiled). Each tile
//     texture is tileSize + 2 px: a 1 px gutter on every side repeats the
//     neighbour's pixels (or, at the canvas border, the border pixel itself,
//     like CLAMP_TO_EDGE), so linear filtering is seamless across tiles.
//
// Writes are partial: only the textures (and texels) a rectangle touches are
// uploaded. The canvas content lives in the CPU RasterStore; this class is
// its GPU copy, refreshed through upload(store, dirty rects). Untouched tiles are never allocated and draw nothing (fully
// transparent). Block 2 has one surface per canvas; layers will own one each.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "canvas/CanvasMath.h"
#include "canvas/RasterStore.h"
#include "canvas/TileGrid.h"
#include "graphics/abstraction/GraphicsDevice.h"

namespace pe::canvas {

struct SurfaceStats {
    StorageMode mode = StorageMode::SingleTexture;
    uint32_t tileSize = 0;
    uint32_t tilesTotal = 0;      // grid size (1 in single-texture mode)
    uint32_t tilesAllocated = 0;  // textures that exist
    uint64_t bytesAllocated = 0;  // GPU texel memory of those textures
    uint64_t writes = 0;          // write() calls that uploaded something
    uint64_t textureUploads = 0;  // updateTexture() calls (one per touched tile)
    uint64_t bytesUploaded = 0;
    double lastWriteMs = 0.0;
    double maxWriteMs = 0.0;
};

class CanvasSurface {
public:
    // A piece of the surface to draw: one texture over a canvas rectangle.
    struct Piece {
        RectI canvasRect;
        float u0 = 0, v0 = 0, u1 = 1, v1 = 1;
        gfx::TextureResource* texture = nullptr;
    };

    CanvasSurface() = default;
    ~CanvasSurface() { destroy(); }
    CanvasSurface(const CanvasSurface&) = delete;
    CanvasSurface& operator=(const CanvasSurface&) = delete;

    // `plan` must come from StoragePolicy::choose() for this size.
    bool create(gfx::GraphicsDevice& device, uint32_t width, uint32_t height, const StoragePlan& plan,
                std::string debugName);
    // Releases every texture (deferred by the device until the GPU is done).
    void destroy();
    bool valid() const { return device_ != nullptr; }

    uint32_t width() const { return grid_.canvasWidth(); }
    uint32_t height() const { return grid_.canvasHeight(); }
    const TileGrid& grid() const { return grid_; }
    StorageMode mode() const { return stats_.mode; }
    const SurfaceStats& stats() const { return stats_; }
    bool tileAllocated(TileCoord t) const;

    // Writes tightly packed premultiplied RGBA8 pixels covering `rect`
    // (rect.w * rect.h * 4 bytes). The part outside the canvas is ignored.
    // Must be called outside a render pass (before recording the frame).
    bool write(const RectI& rect, const uint8_t* rgba);
    bool fill(const RectI& rect, Rgba8 color);
    // Refreshes the texels covering `dirty` (canvas rects) from the CPU store.
    // Regions are merged per GPU texture first: one upload per touched
    // texture, however many brush dabs or store tiles changed inside it.
    bool upload(const RasterStore& store, const std::vector<RectI>& dirty);

    // Pieces intersecting `visible` (canvas space) that hold content.
    // Returns the number of grid tiles intersecting `visible` (for culling stats).
    uint32_t collectVisible(const RectI& visible, std::vector<Piece>& out) const;

private:
    gfx::Ref<gfx::TextureResource> createTexture(uint32_t w, uint32_t h, const std::string& name);
    bool writeTile(TileCoord t, const RectI& clipped, const RectI& src, const uint8_t* rgba);
    // Copies canvas `region` (may extend 1 px past the border: clamped) from the store.
    static void readStore(const RasterStore& store, const RectI& region, uint8_t* dst);
    RectI withBorderGutter(const RectI& clipped) const;
    struct TileRegion {
        size_t index;
        RectI region;
    };
    std::vector<TileRegion> merged_;
    gfx::GraphicsDevice* device_ = nullptr;
    std::string name_;
    TileGrid grid_;
    std::vector<gfx::Ref<gfx::TextureResource>> textures_;  // 1 (single) or one per tile (tiled)
    std::vector<uint8_t> scratch_;
    SurfaceStats stats_;
};

}  // namespace pe::canvas
