// PxPaint canvas - draws a canvas into the backbuffer (render thread only).
//
// Per frame: clear to the workspace colour, a soft shadow around the canvas,
// the paper (solid colour, or a checkerboard for a transparent background),
// then the surface's visible pieces (premultiplied alpha). Only tiles that
// intersect the visible part of the canvas are drawn (culling).
//
// Both backends run the same draws with the same per-draw constants; the
// only backend difference lives in the shaders (Vulkan flips clip-space Y).
#pragma once

#include <vector>

#include "canvas/CanvasSurface.h"
#include "canvas/Viewport.h"
#include "graphics/abstraction/GraphicsDevice.h"
#include "graphics/abstraction/RenderingContext.h"

namespace pe::canvas {

struct CanvasAppearance {
    gfx::ClearColor workspace{0.106f, 0.110f, 0.125f, 1.0f};  // straight, opaque
    Rgba8 paper{255, 255, 255, 255};                           // premultiplied
    bool transparentBackground = false;                        // checkerboard instead of paper
    Rgba8 checkerA{255, 255, 255, 255};
    Rgba8 checkerB{204, 204, 208, 255};
    float checkerCellPx = 12.0f;                               // screen pixels
    bool shadow = true;
};

struct CanvasFrameStats {
    uint32_t tilesVisible = 0;  // grid tiles intersecting the visible area
    uint32_t piecesDrawn = 0;   // textures actually drawn (allocated + visible)
    uint32_t drawCalls = 0;
    uint32_t tilesTotal = 0;
};

class CanvasRenderer {
public:
    bool isReadyFor(gfx::TextureFormat targetFormat) const;
    bool create(gfx::GraphicsDevice& device, gfx::TextureFormat targetFormat);
    void destroy();

    // `viewport` maps canvas pixels to the target's pixels (its screen size
    // must be the size the target is displayed at). `surface` may be null
    // (paper only) and must be fully written before this call.
    void render(gfx::RenderingContext& ctx, gfx::RenderTarget& target, const Viewport& viewport,
                const CanvasAppearance& look, const CanvasSurface* surface, CanvasFrameStats& stats);
    // Workspace only (no canvas open).
    void renderEmpty(gfx::RenderingContext& ctx, gfx::RenderTarget& target, const CanvasAppearance& look);

private:
    struct QuadConstants;

    gfx::Ref<gfx::ShaderResource> vertexShader_;
    gfx::Ref<gfx::ShaderResource> texFragment_;
    gfx::Ref<gfx::ShaderResource> solidFragment_;
    gfx::Ref<gfx::PipelineResource> texPipeline_;
    gfx::Ref<gfx::PipelineResource> solidPipeline_;
    gfx::TextureFormat format_ = gfx::TextureFormat::Unknown;
    std::vector<CanvasSurface::Piece> pieces_;
};

}  // namespace pe::canvas
