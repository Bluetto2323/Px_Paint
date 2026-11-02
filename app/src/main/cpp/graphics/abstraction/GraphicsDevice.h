// Paint Engine - logical GPU device, independent of the graphics API.
//
// Lifetime rules (enforced by the Engine):
//  * created, used and destroyed on the render thread only;
//  * survives surface loss: only the swapchain / EGLSurface is torn down when
//    Android destroys the window, resources stay valid;
//  * every Ref<> created by the device must be released before destruction
//    (ResourceManager reports leaks).
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "graphics/abstraction/BufferResource.h"
#include "graphics/abstraction/GraphicsCapabilities.h"
#include "graphics/abstraction/GraphicsTypes.h"
#include "graphics/abstraction/PipelineResource.h"
#include "graphics/abstraction/RenderTarget.h"
#include "graphics/abstraction/RenderingContext.h"
#include "graphics/abstraction/ResourceManager.h"
#include "graphics/abstraction/ShaderResource.h"
#include "graphics/abstraction/TextureResource.h"

namespace pe {
class PlatformWindow;
}

namespace pe::gfx {

struct SurfaceState {
    bool attached = false;
    Extent2D extent;
    TextureFormat format = TextureFormat::Unknown;
    uint32_t imageCount = 0;
    std::string presentMode;
    std::string transform;          // pre-transform the engine renders with
    std::string currentTransform;   // what the display currently expects
    uint32_t recreateCount = 0;
    double lastRebuildMs = 0.0;     // duration of the last swapchain / EGLSurface (re)build
    bool captureSupported = false;
};

struct FrameContext {
    RenderingContext* context = nullptr;
    RenderTarget* backbuffer = nullptr;
    uint64_t serial = 0;
};

// RGBA8, top-left origin, tightly packed.
struct CaptureResult {
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<uint8_t> rgba;
};

class GraphicsDevice {
public:
    virtual ~GraphicsDevice() = default;

    virtual BackendType backendType() const = 0;
    virtual const GraphicsCapabilities& capabilities() const = 0;

    // --- Surface lifecycle ------------------------------------------------
    // The window must stay alive until detachSurface() returns.
    virtual bool attachSurface(PlatformWindow& window) = 0;
    virtual void detachSurface() = 0;  // waits for GPU work on the surface
    virtual void notifySurfaceResized(Extent2D newExtent) = 0;
    virtual SurfaceState surfaceState() const = 0;

    // --- Resources ----------------------------------------------------------
    virtual Ref<BufferResource> createBuffer(const BufferDesc& desc, const void* initialData) = 0;
    virtual Ref<TextureResource> createTexture(const TextureDesc& desc) = 0;
    virtual Ref<ShaderResource> createShader(const ShaderDesc& desc) = 0;
    virtual Ref<PipelineResource> createPipeline(const PipelineDesc& desc) = 0;
    virtual ResourceManager& resources() = 0;

    // --- Frames -----------------------------------------------------------
    // `serial` is the FrameScheduler serial of this frame.
    virtual FrameStatus beginFrame(uint64_t serial, FrameContext& out) = 0;
    virtual FrameStatus endFrame() = 0;
    virtual void waitIdle() = 0;

    // --- Diagnostics --------------------------------------------------------
    // Asks the device to copy the backbuffer of the next presented frame.
    // Returns false if the surface does not allow readback.
    virtual bool requestCapture() = 0;
    // Moves a completed capture into `out`. False if none is ready.
    virtual bool takeCapture(CaptureResult& out) = 0;
};

}  // namespace pe::gfx
