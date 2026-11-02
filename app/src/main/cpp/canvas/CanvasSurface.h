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
// uploaded. Untouched tiles are never allocated and draw nothing (fully
// transparent). Block 2 has one surface per canvas; layers will own one each.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "canvas/CanvasMath.h"
#include "canvas/TileGrid.h"
#include "graphics/abstraction/GraphicsDevice.h"

namespace pe::canvas {

// Premultiplied RGBA8.
struct Rgba8 {
    uint8_t r = 0, g = 0, b = 0, a = 0;
    bool operator==(const Rgba8& o) const { return r == o.r && g == o.g && b == o.b && a == o.a; }
    // Straight (non-premultiplied) 0xAARRGGBB, as used by Android.
    static Rgba8 fromArgb(uint32_t argb);
};

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

    // Pieces intersecting `visible` (canvas space) that hold content.
    // Returns the number of grid tiles intersecting `visible` (for culling stats).
    uint32_t collectVisible(const RectI& visible, std::vector<Piece>& out) const;

private:
    gfx::Ref<gfx::TextureResource> createTexture(uint32_t w, uint32_t h, const std::string& name);
    bool writeTile(TileCoord t, const RectI& clipped, const RectI& src, const uint8_t* rgba);

    gfx::GraphicsDevice* device_ = nullptr;
    std::string name_;
    TileGrid grid_;
    std::vector<gfx::Ref<gfx::TextureResource>> textures_;  // 1 (single) or one per tile (tiled)
    std::vector<uint8_t> scratch_;
    SurfaceStats stats_;
};

}  // namespace pe::canvas
