#include "graphics/vulkan/VulkanRenderingContext.h"

#include "core/Log.h"
#include "graphics/vulkan/VulkanDevice.h"
#include "graphics/vulkan/VulkanResources.h"

namespace pe::vk {

void VulkanRenderingContext::begin(VkCommandBuffer cmd) {
    cmd_ = cmd;
    inPass_ = false;
    pipeline_ = nullptr;
}

void VulkanRenderingContext::beginRenderPass(const gfx::RenderPassDesc& desc) {
    if (inPass_) {
        PE_LOGE("Vulkan", "beginRenderPass inside an open render pass");
        return;
    }
    auto* bb = desc.target && desc.target->isBackbuffer() ? static_cast<VulkanBackbuffer*>(desc.target) : nullptr;
    if (!bb || !bb->framebuffer) {
        PE_LOGE("Vulkan", "beginRenderPass: only the backbuffer is a valid target in this block");
        return;
    }
    if (desc.debugLabel) pushDebugGroup(desc.debugLabel);

    VkRenderPass pass = device_.renderPass(bb->vkFormat, desc.colorLoad, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
    VkClearValue clear{};
    clear.color = {{desc.clearColor.r, desc.clearColor.g, desc.clearColor.b, desc.clearColor.a}};
    VkRenderPassBeginInfo bi{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    bi.renderPass = pass;
    bi.framebuffer = bb->framebuffer;
    bi.renderArea = {{0, 0}, {bb->extent().width, bb->extent().height}};
    bi.clearValueCount = desc.colorLoad == gfx::LoadOp::Clear ? 1u : 0u;
    bi.pClearValues = &clear;
    vkCmdBeginRenderPass(cmd_, &bi, VK_SUBPASS_CONTENTS_INLINE);
    inPass_ = true;

    // Pipelines use dynamic viewport/scissor; default to the full target.
    const VkViewport vp{0.0f, 0.0f, static_cast<float>(bb->extent().width),
                        static_cast<float>(bb->extent().height), 0.0f, 1.0f};
    const VkRect2D scissor{{0, 0}, {bb->extent().width, bb->extent().height}};
    vkCmdSetViewport(cmd_, 0, 1, &vp);
    vkCmdSetScissor(cmd_, 0, 1, &scissor);
    if (desc.debugLabel) popDebugGroup();
}

void VulkanRenderingContext::endRenderPass() {
    if (!inPass_) return;
    vkCmdEndRenderPass(cmd_);
    inPass_ = false;
}

void VulkanRenderingContext::setViewport(const gfx::Viewport& v) {
    const VkViewport vp{v.x, v.y, v.width, v.height, 0.0f, 1.0f};
    vkCmdSetViewport(cmd_, 0, 1, &vp);
}

void VulkanRenderingContext::bindPipeline(gfx::PipelineResource& pipeline) {
    auto* p = static_cast<VulkanPipeline*>(&pipeline);
    if (p == pipeline_) return;
    pipeline_ = p;
    vkCmdBindPipeline(cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS, p->handle());
}

void VulkanRenderingContext::bindVertexBuffer(gfx::BufferResource& buffer, uint64_t offset) {
    VkBuffer b = static_cast<VulkanBuffer*>(&buffer)->handle();
    const VkDeviceSize off = offset;
    vkCmdBindVertexBuffers(cmd_, 0, 1, &b, &off);
}

void VulkanRenderingContext::draw(uint32_t vertexCount, uint32_t firstVertex) {
    if (!inPass_ || !pipeline_) {
        PE_LOGE("Vulkan", "draw() outside a render pass or without pipeline");
        return;
    }
    vkCmdDraw(cmd_, vertexCount, 1, firstVertex, 0);
}

void VulkanRenderingContext::setConstants(const void* data, uint32_t size) {
    if (!pipeline_ || size % 16 != 0 || size > pipeline_->constantsSize()) {
        PE_LOGE("Vulkan", "setConstants(%u bytes): no pipeline bound or size invalid", size);
        return;
    }
    vkCmdPushConstants(cmd_, pipeline_->layout(), VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, size,
                       data);
}

void VulkanRenderingContext::bindTexture(gfx::TextureResource& texture) {
    VkDescriptorSet set = static_cast<VulkanTexture&>(texture).descriptorSet();
    if (!pipeline_ || !set) {
        PE_LOGE("Vulkan", "bindTexture: no pipeline bound or texture not sampled");
        return;
    }
    vkCmdBindDescriptorSets(cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_->layout(), 0, 1, &set, 0, nullptr);
}

void VulkanRenderingContext::pushDebugGroup(const char* name) {
    if (!device_.debugLabels() || !vkCmdBeginDebugUtilsLabelEXT) return;
    VkDebugUtilsLabelEXT label{VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT};
    label.pLabelName = name;
    vkCmdBeginDebugUtilsLabelEXT(cmd_, &label);
}

void VulkanRenderingContext::popDebugGroup() {
    if (!device_.debugLabels() || !vkCmdEndDebugUtilsLabelEXT) return;
    vkCmdEndDebugUtilsLabelEXT(cmd_);
}

}  // namespace pe::vk
