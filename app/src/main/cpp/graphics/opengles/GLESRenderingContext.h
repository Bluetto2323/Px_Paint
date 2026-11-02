#pragma once

#include "graphics/abstraction/RenderingContext.h"
#include "graphics/opengles/GLESCommon.h"
#include "graphics/opengles/GLESResources.h"

namespace pe::gles {

class GLESRenderingContext final : public gfx::RenderingContext {
public:
    explicit GLESRenderingContext(const GLExtFunctions& ext) : ext_(ext) {}

    void beginFrame();  // resets cached state

    void beginRenderPass(const gfx::RenderPassDesc& desc) override;
    void endRenderPass() override;
    void setViewport(const gfx::Viewport& viewport) override;
    void bindPipeline(gfx::PipelineResource& pipeline) override;
    void bindVertexBuffer(gfx::BufferResource& buffer, uint64_t offset) override;
    void draw(uint32_t vertexCount, uint32_t firstVertex) override;
    void setConstants(const void* data, uint32_t size) override;
    void bindTexture(gfx::TextureResource& texture) override;
    void pushDebugGroup(const char* name) override;
    void popDebugGroup() override;

private:
    const GLExtFunctions& ext_;
    GLESPipeline* pipeline_ = nullptr;
    GLESBuffer* vertexBuffer_ = nullptr;
    uint64_t vertexOffset_ = 0;
    bool layoutDirty_ = true;
    bool inPass_ = false;
    bool storeColor_ = true;
    gfx::Extent2D passExtent_;
};

}  // namespace pe::gles
