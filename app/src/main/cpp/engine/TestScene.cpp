#include "engine/TestScene.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <utility>

#include "core/Log.h"
#include "shaders/generated/TriangleShaders.h"

namespace pe {

namespace {
constexpr const char* kTag = "TestScene";

struct Vertex {
    float x, y;
    float r, g, b;
};

// Y-up normalized device coordinates (the Vulkan shader flips Y).
constexpr Vertex kTriangle[3] = {
    {0.0f, 0.6f, 1.0f, 0.0f, 0.0f},    // top: red
    {-0.6f, -0.6f, 0.0f, 1.0f, 0.0f},  // bottom-left: green
    {0.6f, -0.6f, 0.0f, 0.0f, 1.0f},   // bottom-right: blue
};

bool barycentric(float px, float py, float& w0, float& w1, float& w2) {
    const Vertex &a = kTriangle[0], &b = kTriangle[1], &c = kTriangle[2];
    const float det = (b.y - c.y) * (a.x - c.x) + (c.x - b.x) * (a.y - c.y);
    w0 = ((b.y - c.y) * (px - c.x) + (c.x - b.x) * (py - c.y)) / det;
    w1 = ((c.y - a.y) * (px - c.x) + (a.x - c.x) * (py - c.y)) / det;
    w2 = 1.0f - w0 - w1;
    return w0 >= 0 && w1 >= 0 && w2 >= 0;
}
}  // namespace

bool TestScene::isReadyFor(gfx::TextureFormat targetFormat) const {
    return pipeline_ && format_ == targetFormat;
}

bool TestScene::create(gfx::GraphicsDevice& device, gfx::TextureFormat targetFormat) {
    if (!vertices_) {
        gfx::BufferDesc bd;
        bd.size = sizeof(kTriangle);
        bd.usage = gfx::BufferUsage::Vertex;
        bd.memory = gfx::MemoryUsage::GpuOnly;
        bd.debugName = "triangle.vertices";
        vertices_ = device.createBuffer(bd, kTriangle);

        gfx::ShaderDesc vs;
        vs.stage = gfx::ShaderStage::Vertex;
        vs.spirv = shaders::kTriangleVertSpv;
        vs.spirvSizeBytes = sizeof(shaders::kTriangleVertSpv);
        vs.glslEs = shaders::kTriangleVertEs;
        vs.debugName = "triangle.vert";
        vertexShader_ = device.createShader(vs);

        gfx::ShaderDesc fs;
        fs.stage = gfx::ShaderStage::Fragment;
        fs.spirv = shaders::kTriangleFragSpv;
        fs.spirvSizeBytes = sizeof(shaders::kTriangleFragSpv);
        fs.glslEs = shaders::kTriangleFragEs;
        fs.debugName = "triangle.frag";
        fragmentShader_ = device.createShader(fs);
    }
    if (!vertices_ || !vertexShader_ || !fragmentShader_) {
        PE_LOGE(kTag, "Resource creation failed");
        destroy();
        return false;
    }

    gfx::PipelineDesc pd;
    pd.vertexShader = vertexShader_;
    pd.fragmentShader = fragmentShader_;
    pd.vertexLayout.stride = sizeof(Vertex);
    pd.vertexLayout.attributes = {{0, gfx::VertexFormat::Float2, 0},
                                  {1, gfx::VertexFormat::Float3, static_cast<uint32_t>(2 * sizeof(float))}};
    pd.colorFormat = targetFormat;
    pd.debugName = "triangle.pipeline";
    pipeline_ = device.createPipeline(pd);  // old pipeline (if any) is retired safely
    if (!pipeline_) {
        PE_LOGE(kTag, "Pipeline creation failed for %s", gfx::toString(targetFormat));
        return false;
    }
    format_ = targetFormat;
    PE_LOGI(kTag, "Scene ready for target format %s", gfx::toString(targetFormat));
    return true;
}

void TestScene::destroy() {
    pipeline_.reset();
    fragmentShader_.reset();
    vertexShader_.reset();
    vertices_.reset();
    format_ = gfx::TextureFormat::Unknown;
}

void TestScene::render(gfx::RenderingContext& ctx, gfx::RenderTarget& target) {
    gfx::RenderPassDesc pass;
    pass.target = &target;
    pass.colorLoad = gfx::LoadOp::Clear;
    pass.clearColor = clearColor();
    pass.debugLabel = "TestScene";
    ctx.beginRenderPass(pass);
    ctx.bindPipeline(*pipeline_);
    ctx.bindVertexBuffer(*vertices_, 0);
    ctx.draw(3, 0);
    ctx.endRenderPass();
}

SelfTestResult TestScene::verify(const gfx::CaptureResult& cap) {
    SelfTestResult result;
    if (cap.width < 16 || cap.height < 16 || cap.rgba.size() < static_cast<size_t>(cap.width) * cap.height * 4) {
        result.status = SelfTestStatus::Failed;
        result.detail = "capture too small or incomplete";
        return result;
    }
    const auto clear = clearColor();
    constexpr int kTolerance = 40;  // mediump precision, dithering, sRGB rounding

    struct Probe {
        const char* name;
        float x, y;  // NDC, Y up
    };
    const Vertex &a = kTriangle[0], &b = kTriangle[1], &c = kTriangle[2];
    const float cx = (a.x + b.x + c.x) / 3.0f, cy = (a.y + b.y + c.y) / 3.0f;
    auto toward = [&](const Vertex& v, float t) { return std::make_pair(v.x + (cx - v.x) * t, v.y + (cy - v.y) * t); };
    const auto pa = toward(a, 0.2f), pb = toward(b, 0.2f), pc = toward(c, 0.2f);
    const Probe probes[] = {
        {"centroid", cx, cy},           {"near-top(red)", pa.first, pa.second},
        {"near-left(green)", pb.first, pb.second}, {"near-right(blue)", pc.first, pc.second},
        {"corner-tl", -0.95f, 0.95f},   {"corner-tr", 0.95f, 0.95f},
        {"corner-bl", -0.95f, -0.95f},  {"corner-br", 0.95f, -0.95f},
    };

    int failures = 0;
    std::string detail;
    for (const auto& p : probes) {
        const uint32_t px = std::min(cap.width - 1, static_cast<uint32_t>((p.x + 1.0f) * 0.5f * cap.width));
        const uint32_t py = std::min(cap.height - 1, static_cast<uint32_t>((1.0f - p.y) * 0.5f * cap.height));
        const uint8_t* rgba = &cap.rgba[(static_cast<size_t>(py) * cap.width + px) * 4];

        float w0, w1, w2;
        float er, eg, eb;
        if (barycentric(p.x, p.y, w0, w1, w2)) {
            er = w0 * a.r + w1 * b.r + w2 * c.r;
            eg = w0 * a.g + w1 * b.g + w2 * c.g;
            eb = w0 * a.b + w1 * b.b + w2 * c.b;
        } else {
            er = clear.r;
            eg = clear.g;
            eb = clear.b;
        }
        const int exp[3] = {static_cast<int>(std::lround(er * 255)), static_cast<int>(std::lround(eg * 255)),
                            static_cast<int>(std::lround(eb * 255))};
        const bool ok = std::abs(rgba[0] - exp[0]) <= kTolerance && std::abs(rgba[1] - exp[1]) <= kTolerance &&
                        std::abs(rgba[2] - exp[2]) <= kTolerance;
        if (!ok) {
            failures++;
            char buf[160];
            std::snprintf(buf, sizeof(buf), "%s@(%u,%u) got %d,%d,%d expected %d,%d,%d; ", p.name, px, py, rgba[0],
                          rgba[1], rgba[2], exp[0], exp[1], exp[2]);
            detail += buf;
        }
    }
    result.status = failures == 0 ? SelfTestStatus::Passed : SelfTestStatus::Failed;
    result.detail = failures == 0 ? "8/8 probes match (" + std::to_string(cap.width) + "x" +
                                        std::to_string(cap.height) + ")"
                                  : detail;
    return result;
}

}  // namespace pe
