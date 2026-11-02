#include "canvas/CanvasRenderer.h"

#include <algorithm>
#include <cstring>

#include "core/Log.h"
#include "shaders/generated/CanvasShaders.h"

namespace pe::canvas {

namespace {
constexpr const char* kTag = "CanvasRenderer";
}

// Layout shared with shaders/canvas*.vert|frag (6 x vec4 = 96 bytes).
struct CanvasRenderer::QuadConstants {
    float row0[4];    // ndc.x = row0 . (x, y, 1)  (quad-local canvas pixels)
    float row1[4];    // ndc.y = row1 . (x, y, 1)  (GL convention, y up)
    float rect[4];    // quad origin (always 0,0: folded into the rows) and size
    float uvOrColorB[4];
    float color[4];
    float params[4];  // mode (0 solid, 1 checker), checker cell px
};
static_assert(sizeof(float) * 24 == 96, "push constant layout");

namespace {
// canvas->NDC affine, re-based on the quad origin in double precision so the
// GPU only ever sees small coordinates (exact even on 16k canvases at 64x).
void quadRows(const Affine2& m, double ox, double oy, float row0[4], float row1[4]) {
    row0[0] = static_cast<float>(m.a);
    row0[1] = static_cast<float>(m.c);
    row0[2] = static_cast<float>(m.a * ox + m.c * oy + m.tx);
    row0[3] = 0.0f;
    row1[0] = static_cast<float>(m.b);
    row1[1] = static_cast<float>(m.d);
    row1[2] = static_cast<float>(m.b * ox + m.d * oy + m.ty);
    row1[3] = 0.0f;
}

void toFloat(Rgba8 c, float out[4]) {
    out[0] = c.r / 255.0f;
    out[1] = c.g / 255.0f;
    out[2] = c.b / 255.0f;
    out[3] = c.a / 255.0f;
}
}  // namespace

bool CanvasRenderer::isReadyFor(gfx::TextureFormat targetFormat) const {
    return texPipeline_ && solidPipeline_ && format_ == targetFormat;
}

bool CanvasRenderer::create(gfx::GraphicsDevice& device, gfx::TextureFormat targetFormat) {
    if (!vertexShader_) {
        gfx::ShaderDesc vs;
        vs.stage = gfx::ShaderStage::Vertex;
        vs.spirv = shaders::kCanvasVertSpv;
        vs.spirvSizeBytes = sizeof(shaders::kCanvasVertSpv);
        vs.glslEs = shaders::kCanvasVertEs;
        vs.debugName = "canvas.vert";
        vertexShader_ = device.createShader(vs);

        gfx::ShaderDesc tf;
        tf.stage = gfx::ShaderStage::Fragment;
        tf.spirv = shaders::kCanvasTexFragSpv;
        tf.spirvSizeBytes = sizeof(shaders::kCanvasTexFragSpv);
        tf.glslEs = shaders::kCanvasTexFragEs;
        tf.debugName = "canvas_tex.frag";
        texFragment_ = device.createShader(tf);

        gfx::ShaderDesc sf;
        sf.stage = gfx::ShaderStage::Fragment;
        sf.spirv = shaders::kCanvasSolidFragSpv;
        sf.spirvSizeBytes = sizeof(shaders::kCanvasSolidFragSpv);
        sf.glslEs = shaders::kCanvasSolidFragEs;
        sf.debugName = "canvas_solid.frag";
        solidFragment_ = device.createShader(sf);
    }
    if (!vertexShader_ || !texFragment_ || !solidFragment_) {
        PE_LOGE(kTag, "Shader creation failed");
        destroy();
        return false;
    }
    gfx::PipelineDesc pd;
    pd.vertexShader = vertexShader_;
    pd.topology = gfx::PrimitiveTopology::TriangleStrip;
    pd.blend = gfx::BlendMode::PremultipliedAlpha;
    pd.colorFormat = targetFormat;
    pd.constantsSize = sizeof(QuadConstants);

    pd.fragmentShader = texFragment_;
    pd.sampledTexture = true;
    pd.debugName = "canvas.textured";
    texPipeline_ = device.createPipeline(pd);

    pd.fragmentShader = solidFragment_;
    pd.sampledTexture = false;
    pd.debugName = "canvas.solid";
    solidPipeline_ = device.createPipeline(pd);
    if (!texPipeline_ || !solidPipeline_) {
        PE_LOGE(kTag, "Pipeline creation failed for %s", gfx::toString(targetFormat));
        texPipeline_.reset();
        solidPipeline_.reset();
        return false;
    }
    format_ = targetFormat;
    PE_LOGI(kTag, "Canvas pipelines ready for %s", gfx::toString(targetFormat));
    return true;
}

void CanvasRenderer::destroy() {
    texPipeline_.reset();
    solidPipeline_.reset();
    solidFragment_.reset();
    texFragment_.reset();
    vertexShader_.reset();
    pieces_.clear();
    format_ = gfx::TextureFormat::Unknown;
}

void CanvasRenderer::renderEmpty(gfx::RenderingContext& ctx, gfx::RenderTarget& target,
                                 const CanvasAppearance& look) {
    gfx::RenderPassDesc pass;
    pass.target = &target;
    pass.colorLoad = gfx::LoadOp::Clear;
    pass.clearColor = look.workspace;
    pass.debugLabel = "Canvas(empty)";
    ctx.beginRenderPass(pass);
    ctx.endRenderPass();
}

void CanvasRenderer::render(gfx::RenderingContext& ctx, gfx::RenderTarget& target, const Viewport& viewport,
                            const CanvasAppearance& look, const CanvasSurface* surface, CanvasFrameStats& stats) {
    stats = {};
    gfx::RenderPassDesc pass;
    pass.target = &target;
    pass.colorLoad = gfx::LoadOp::Clear;
    pass.clearColor = look.workspace;
    pass.debugLabel = "Canvas";
    ctx.beginRenderPass(pass);
    if (!viewport.valid()) {
        ctx.endRenderPass();
        return;
    }
    const Affine2 ndc = viewport.canvasToNdc();
    const double cw = viewport.canvasWidth(), ch = viewport.canvasHeight();

    QuadConstants q{};
    auto solidQuad = [&](double x, double y, double w, double h, const float color[4], bool checker,
                         const float colorB[4]) {
        quadRows(ndc, x, y, q.row0, q.row1);
        q.rect[0] = 0.0f;
        q.rect[1] = 0.0f;
        q.rect[2] = static_cast<float>(w);
        q.rect[3] = static_cast<float>(h);
        std::memcpy(q.color, color, sizeof(q.color));
        if (colorB) std::memcpy(q.uvOrColorB, colorB, sizeof(q.uvOrColorB));
        q.params[0] = checker ? 1.0f : 0.0f;
        q.params[1] = look.checkerCellPx;
        ctx.setConstants(&q, sizeof(q));
        ctx.draw(4, 0);
        stats.drawCalls++;
    };

    ctx.bindPipeline(*solidPipeline_);
    if (look.shadow) {
        // Soft shadow: three premultiplied black rings, sized in screen pixels.
        const double inv = 1.0 / std::max(viewport.zoom(), 1e-9);
        const struct { double grow; float alpha; } rings[] = {{9.0, 0.035f}, {5.0, 0.06f}, {2.0, 0.10f}};
        for (const auto& r : rings) {
            const double g = r.grow * inv;
            const float c[4] = {0.0f, 0.0f, 0.0f, r.alpha};
            solidQuad(-g, -g + 1.5 * inv, cw + 2 * g, ch + 2 * g, c, false, nullptr);
        }
    }
    if (look.transparentBackground) {
        float a[4], b[4];
        toFloat(look.checkerA, a);
        toFloat(look.checkerB, b);
        solidQuad(0, 0, cw, ch, a, true, b);
    } else {
        float paper[4];
        toFloat(look.paper, paper);
        solidQuad(0, 0, cw, ch, paper, false, nullptr);
    }

    if (surface && surface->valid()) {
        pieces_.clear();
        const RectI visible = viewport.visibleCanvasBounds().roundOut();
        stats.tilesVisible = surface->collectVisible(visible, pieces_);
        stats.tilesTotal = surface->stats().tilesTotal;
        if (!pieces_.empty()) {
            ctx.bindPipeline(*texPipeline_);
            const float white[4] = {1.0f, 1.0f, 1.0f, 1.0f};
            for (const auto& p : pieces_) {
                quadRows(ndc, p.canvasRect.x, p.canvasRect.y, q.row0, q.row1);
                q.rect[0] = 0.0f;
                q.rect[1] = 0.0f;
                q.rect[2] = static_cast<float>(p.canvasRect.w);
                q.rect[3] = static_cast<float>(p.canvasRect.h);
                q.uvOrColorB[0] = p.u0;
                q.uvOrColorB[1] = p.v0;
                q.uvOrColorB[2] = p.u1;
                q.uvOrColorB[3] = p.v1;
                std::memcpy(q.color, white, sizeof(q.color));
                q.params[0] = 0.0f;
                ctx.bindTexture(*p.texture);
                ctx.setConstants(&q, sizeof(q));
                ctx.draw(4, 0);
                stats.drawCalls++;
                stats.piecesDrawn++;
            }
        }
    }
    ctx.endRenderPass();
}

}  // namespace pe::canvas
