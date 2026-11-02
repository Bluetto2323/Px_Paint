#pragma once

#include "graphics/abstraction/RenderingContext.h"
#include "graphics/vulkan/VulkanCommon.h"

namespace pe::vk {

class VulkanDevice;
class VulkanPipeline;

class VulkanRenderingContext final : public gfx::RenderingContext {
public:
    explicit VulkanRenderingContext(VulkanDevice& device) : device_(device) {}

    void begin(VkCommandBuffer cmd);
    VkCommandBuffer commandBuffer() const { return cmd_; }
    bool insideRenderPass() const { return inPass_; }

    void beginRenderPass(const gfx::RenderPassDesc& desc) override;
    void endRenderPass() override;
    void setViewport(const gfx::Viewport& viewport) override;
    void bindPipeline(gfx::PipelineResource& pipeline) override;
    void bindVertexBuffer(gfx::BufferResource& buffer, uint64_t offset) override;
    void draw(uint32_t vertexCount, uint32_t firstVertex) override;
    void pushDebugGroup(const char* name) override;
    void popDebugGroup() override;

private:
    VulkanDevice& device_;
    VkCommandBuffer cmd_ = VK_NULL_HANDLE;
    bool inPass_ = false;
    VulkanPipeline* pipeline_ = nullptr;
};

}  // namespace pe::vk
