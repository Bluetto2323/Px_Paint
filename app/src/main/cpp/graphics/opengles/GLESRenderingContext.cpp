#include "graphics/opengles/GLESRenderingContext.h"

#include "core/Log.h"

namespace pe::gles {

void GLESRenderingContext::beginFrame() {
    pipeline_ = nullptr;
    vertexBuffer_ = nullptr;
    layoutDirty_ = true;
    inPass_ = false;
}

void GLESRenderingContext::beginRenderPass(const gfx::RenderPassDesc& desc) {
    if (inPass_) {
        PE_LOGE("GLES", "beginRenderPass inside an open render pass");
        return;
    }
    auto* bb = desc.target && desc.target->isBackbuffer() ? static_cast<GLESBackbuffer*>(desc.target) : nullptr;
    if (!bb) {
        PE_LOGE("GLES", "beginRenderPass: only the backbuffer is a valid target in this block");
        return;
    }
    inPass_ = true;
    storeColor_ = desc.storeColor;
    passExtent_ = bb->extent();
    if (desc.debugLabel) pushDebugGroup(desc.debugLabel);

    glBindFramebuffer(GL_FRAMEBUFFER, bb->framebuffer());
    glViewport(0, 0, static_cast<GLsizei>(passExtent_.width), static_cast<GLsizei>(passExtent_.height));
    glDisable(GL_SCISSOR_TEST);

    switch (desc.colorLoad) {
        case gfx::LoadOp::Clear:
            glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
            glClearColor(desc.clearColor.r, desc.clearColor.g, desc.clearColor.b, desc.clearColor.a);
            glClear(GL_COLOR_BUFFER_BIT);
            break;
        case gfx::LoadOp::DontCare: {
            // Tells tile-based GPUs (Mali, Adreno, PowerVR) not to load the
            // previous contents from memory.
            const GLenum attachment = bb->framebuffer() == 0 ? GL_COLOR : GL_COLOR_ATTACHMENT0;
            glInvalidateFramebuffer(GL_FRAMEBUFFER, 1, &attachment);
            break;
        }
        case gfx::LoadOp::Load: break;
    }
    if (desc.debugLabel) popDebugGroup();
}

void GLESRenderingContext::endRenderPass() {
    if (!inPass_) return;
    inPass_ = false;
    // Backbuffer contents are always presented; offscreen discard is handled
    // when offscreen targets exist (storeColor_ kept for that).
    (void)storeColor_;
}

void GLESRenderingContext::setViewport(const gfx::Viewport& v) {
    glViewport(static_cast<GLint>(v.x), static_cast<GLint>(v.y), static_cast<GLsizei>(v.width),
               static_cast<GLsizei>(v.height));
}

void GLESRenderingContext::bindPipeline(gfx::PipelineResource& pipeline) {
    auto* p = static_cast<GLESPipeline*>(&pipeline);
    if (p == pipeline_) return;
    pipeline_ = p;
    layoutDirty_ = true;
    glUseProgram(p->program());
    glBindVertexArray(p->vertexArray());
    if (p->blend() == gfx::BlendMode::PremultipliedAlpha) {
        glEnable(GL_BLEND);
        glBlendFuncSeparate(GL_ONE, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    } else {
        glDisable(GL_BLEND);
    }
}

void GLESRenderingContext::bindVertexBuffer(gfx::BufferResource& buffer, uint64_t offset) {
    auto* b = static_cast<GLESBuffer*>(&buffer);
    if (b == vertexBuffer_ && offset == vertexOffset_) return;
    vertexBuffer_ = b;
    vertexOffset_ = offset;
    layoutDirty_ = true;
}

void GLESRenderingContext::draw(uint32_t vertexCount, uint32_t firstVertex) {
    if (!inPass_ || !pipeline_) {
        PE_LOGE("GLES", "draw() outside a render pass or without pipeline");
        return;
    }
    if (layoutDirty_ && vertexBuffer_) {
        applyVertexLayout(pipeline_->layout(), vertexBuffer_->handle(), vertexOffset_);
        layoutDirty_ = false;
    }
    glDrawArrays(pipeline_->topology(), static_cast<GLint>(firstVertex), static_cast<GLsizei>(vertexCount));
}

void GLESRenderingContext::pushDebugGroup(const char* name) {
    if (ext_.pushDebugGroup) ext_.pushDebugGroup(GL_DEBUG_SOURCE_APPLICATION_KHR, 0, -1, name);
}

void GLESRenderingContext::popDebugGroup() {
    if (ext_.popDebugGroup) ext_.popDebugGroup();
}

}  // namespace pe::gles
