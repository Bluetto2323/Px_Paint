#pragma once

#include <memory>

#include "graphics/abstraction/BufferResource.h"
#include "graphics/abstraction/PipelineResource.h"
#include "graphics/abstraction/RenderTarget.h"
#include "graphics/abstraction/ShaderResource.h"
#include "graphics/abstraction/TextureResource.h"
#include "graphics/vulkan/VulkanCommon.h"

namespace pe::vk {

class VulkanDevice;

// One VkDeviceMemory per resource. Adequate for Block 1 (a handful of
// objects); a sub-allocator replaces this before canvases/layers exist.
struct Allocation {
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkDeviceSize size = 0;
    uint32_t typeIndex = 0;
    void* mapped = nullptr;
    bool coherent = true;
};

class VulkanBuffer final : public gfx::BufferResource {
public:
    static std::unique_ptr<VulkanBuffer> create(VulkanDevice& device, const gfx::BufferDesc& desc, const void* data);
    ~VulkanBuffer() override;
    bool update(uint64_t offset, const void* data, uint64_t size) override;
    VkBuffer handle() const { return buffer_; }

private:
    VulkanBuffer(VulkanDevice& device, const gfx::BufferDesc& desc) : BufferResource(desc), device_(device) {}
    VulkanDevice& device_;
    VkBuffer buffer_ = VK_NULL_HANDLE;
    Allocation alloc_;
};

class VulkanTexture final : public gfx::TextureResource {
public:
    static std::unique_ptr<VulkanTexture> create(VulkanDevice& device, const gfx::TextureDesc& desc);
    ~VulkanTexture() override;
    VkImage image() const { return image_; }
    VkImageView view() const { return view_; }
    VkDescriptorSet descriptorSet() const { return descriptorSet_; }

private:
    VulkanTexture(VulkanDevice& device, const gfx::TextureDesc& desc) : TextureResource(desc), device_(device) {}
    VulkanDevice& device_;
    VkImage image_ = VK_NULL_HANDLE;
    VkImageView view_ = VK_NULL_HANDLE;
    VkDescriptorSet descriptorSet_ = VK_NULL_HANDLE;  // sampled textures only
    Allocation alloc_;
};

class VulkanShader final : public gfx::ShaderResource {
public:
    static std::unique_ptr<VulkanShader> create(VulkanDevice& device, const gfx::ShaderDesc& desc);
    ~VulkanShader() override;
    VkShaderModule module() const { return module_; }

private:
    VulkanShader(VulkanDevice& device, gfx::ShaderStage stage, std::string name)
        : ShaderResource(stage, std::move(name)), device_(device) {}
    VulkanDevice& device_;
    VkShaderModule module_ = VK_NULL_HANDLE;
};

class VulkanPipeline final : public gfx::PipelineResource {
public:
    static std::unique_ptr<VulkanPipeline> create(VulkanDevice& device, const gfx::PipelineDesc& desc);
    ~VulkanPipeline() override;
    VkPipeline handle() const { return pipeline_; }
    VkPipelineLayout layout() const { return layout_; }

private:
    VulkanPipeline(VulkanDevice& device, gfx::TextureFormat fmt, std::string name, uint32_t constantsSize)
        : PipelineResource(fmt, std::move(name), constantsSize), device_(device) {}
    VulkanDevice& device_;
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
};

// Swapchain image as a render target (owned by the swapchain).
class VulkanBackbuffer final : public gfx::RenderTarget {
public:
    gfx::Extent2D extent() const override { return extent_; }
    gfx::TextureFormat colorFormat() const override { return format_; }
    bool isBackbuffer() const override { return true; }

    gfx::Extent2D extent_;
    gfx::TextureFormat format_ = gfx::TextureFormat::Unknown;
    VkFormat vkFormat = VK_FORMAT_UNDEFINED;
    VkFramebuffer framebuffer = VK_NULL_HANDLE;
    VkImage image = VK_NULL_HANDLE;
};

}  // namespace pe::vk
