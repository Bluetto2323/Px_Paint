// Paint Engine - Block 1 test scene: one colored triangle.
//
// Uses only the backend-neutral API. It also knows exactly what it draws, so
// it can verify a captured backbuffer (self-test): this catches a black
// screen, a wrong Y orientation or swapped R/B channels on a real device.
#pragma once

#include <string>

#include "diagnostics/SelfTest.h"
#include "graphics/abstraction/GraphicsDevice.h"

namespace pe {

class TestScene {
public:
    static gfx::ClearColor clearColor() { return {0.08f, 0.08f, 0.10f, 1.0f}; }

    bool isReadyFor(gfx::TextureFormat targetFormat) const;
    bool create(gfx::GraphicsDevice& device, gfx::TextureFormat targetFormat);
    void destroy();
    void render(gfx::RenderingContext& ctx, gfx::RenderTarget& target);

    static SelfTestResult verify(const gfx::CaptureResult& capture);

private:
    gfx::Ref<gfx::BufferResource> vertices_;
    gfx::Ref<gfx::ShaderResource> vertexShader_;
    gfx::Ref<gfx::ShaderResource> fragmentShader_;
    gfx::Ref<gfx::PipelineResource> pipeline_;
    gfx::TextureFormat format_ = gfx::TextureFormat::Unknown;
};

}  // namespace pe
