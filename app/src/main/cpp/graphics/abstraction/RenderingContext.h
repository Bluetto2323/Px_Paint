// Paint Engine - backend-neutral command recording.
//
// A RenderingContext is obtained from GraphicsDevice::beginFrame() and is only
// valid until endFrame(). It is not thread-safe: it lives on the render thread.
#pragma once

#include "graphics/abstraction/BufferResource.h"
#include "graphics/abstraction/GraphicsTypes.h"
#include "graphics/abstraction/PipelineResource.h"
#include "graphics/abstraction/RenderTarget.h"

namespace pe::gfx {

struct RenderPassDesc {
    RenderTarget* target = nullptr;
    LoadOp colorLoad = LoadOp::Clear;
    ClearColor clearColor;
    // When false the contents may be discarded after the pass (tile GPUs such
    // as Mali skip the write-back). Backbuffers are always stored.
    bool storeColor = true;
    const char* debugLabel = nullptr;
};

struct Viewport {
    float x = 0, y = 0, width = 0, height = 0;
};

class RenderingContext {
public:
    virtual ~RenderingContext() = default;

    virtual void beginRenderPass(const RenderPassDesc& desc) = 0;
    virtual void endRenderPass() = 0;

    // Viewport and scissor default to the full target at beginRenderPass().
    virtual void setViewport(const Viewport& viewport) = 0;

    virtual void bindPipeline(PipelineResource& pipeline) = 0;
    virtual void bindVertexBuffer(BufferResource& buffer, uint64_t offset = 0) = 0;
    virtual void draw(uint32_t vertexCount, uint32_t firstVertex = 0) = 0;

    // Debug markers (no-ops when the driver has no debug-label support).
    virtual void pushDebugGroup(const char* name) = 0;
    virtual void popDebugGroup() = 0;
};

}  // namespace pe::gfx
