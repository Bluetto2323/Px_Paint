#pragma once

#include <memory>
#include <string>

#include "graphics/abstraction/GraphicsDevice.h"
#include "graphics/opengles/GLESCommon.h"
#include "graphics/opengles/GLESRenderingContext.h"
#include "graphics/opengles/GLESResources.h"

namespace pe::gles {

class GLESBackend;

// One EGL context, current on the render thread for the device's lifetime.
// When no window is attached the context stays current on a surfaceless (or
// 1x1 pbuffer) binding so resources remain usable.
class GLESDevice final : public gfx::GraphicsDevice {
public:
    explicit GLESDevice(GLESBackend& backend);
    ~GLESDevice() override;

    bool initialize(std::string& error);

    gfx::BackendType backendType() const override { return gfx::BackendType::OpenGLES; }
    const gfx::GraphicsCapabilities& capabilities() const override { return caps_; }

    bool attachSurface(PlatformWindow& window) override;
    void detachSurface() override;
    void notifySurfaceResized(gfx::Extent2D extent) override;
    gfx::SurfaceState surfaceState() const override;

    gfx::Ref<gfx::BufferResource> createBuffer(const gfx::BufferDesc& desc, const void* data) override;
    gfx::Ref<gfx::TextureResource> createTexture(const gfx::TextureDesc& desc) override;
    gfx::Ref<gfx::ShaderResource> createShader(const gfx::ShaderDesc& desc) override;
    gfx::Ref<gfx::PipelineResource> createPipeline(const gfx::PipelineDesc& desc) override;
    gfx::ResourceManager& resources() override { return resources_; }

    gfx::FrameStatus beginFrame(uint64_t serial, gfx::FrameContext& out) override;
    gfx::FrameStatus endFrame() override;
    void waitIdle() override;

    bool requestCapture() override;
    bool takeCapture(gfx::CaptureResult& out) override;

private:
    bool makeIdleCurrent();
    bool createWindowSurface();
    void destroyWindowSurface();
    void queryCapabilities();
    void installDebugCallback();
    void resolveRobustness();
    // True (and latches contextLost_) if EGL/GL report the context as lost.
    bool checkContextLost(const char* where);
    gfx::Extent2D querySurfaceExtent() const;

    GLESBackend& backend_;
    EGLDisplay display_;
    EGLContext context_ = EGL_NO_CONTEXT;
    EGLSurface idleSurface_ = EGL_NO_SURFACE;  // 1x1 pbuffer if no surfaceless ctx
    bool surfaceless_ = false;
    bool debugContext_ = false;
    bool robustContext_ = false;   // LOSE_CONTEXT_ON_RESET requested and accepted
    bool contextLost_ = false;     // latched: device must be recreated

    PlatformWindow* window_ = nullptr;
    EGLSurface windowSurface_ = EGL_NO_SURFACE;
    gfx::Extent2D requestedExtent_;  // headless only
    bool recreateSurface_ = false;
    uint32_t recreateCount_ = 0;
    double lastRebuildMs_ = 0.0;
    EGLint swapInterval_ = 1;

    gfx::GraphicsCapabilities caps_;
    GLExtFunctions ext_;
    gfx::ResourceManager resources_;
    GLESRenderingContext context2d_{ext_};
    GLESBackbuffer backbuffer_;
    uint64_t frameSerial_ = 0;
    bool inFrame_ = false;
    uint64_t framesSinceErrorCheck_ = 0;

    bool captureRequested_ = false;
    bool captureReady_ = false;
    gfx::CaptureResult capture_;
};

}  // namespace pe::gles
