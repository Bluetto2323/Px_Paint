#pragma once

#include <string>
#include <vector>

#include "graphics/abstraction/GraphicsTypes.h"
#include "graphics/vulkan/VulkanCommon.h"

namespace pe {
class PlatformWindow;
}

namespace pe::vk {

class VulkanDevice;

// VkSurfaceKHR + VkSwapchainKHR + per-image views, framebuffers and
// "render finished" semaphores. The owner must make the device idle before
// rebuild() or destruction.
class VulkanSwapchain {
public:
    VulkanSwapchain(VulkanDevice& device, PlatformWindow& window, VkSurfaceKHR surface);
    ~VulkanSwapchain();

    VulkanSwapchain(const VulkanSwapchain&) = delete;
    VulkanSwapchain& operator=(const VulkanSwapchain&) = delete;

    enum class BuildResult { Ok, ZeroExtent, Failed, SurfaceLost };
    BuildResult rebuild(gfx::Extent2D requestedExtent);

    VkResult acquire(VkSemaphore signal, uint32_t& imageIndex);
    VkResult present(VkQueue queue, uint32_t imageIndex);

    void markDirty() { dirty_ = true; }
    bool dirty() const { return dirty_; }
    bool valid() const { return swapchain_ != VK_NULL_HANDLE; }

    gfx::Extent2D extent() const { return extent_; }
    VkFormat vkFormat() const { return format_; }
    gfx::TextureFormat format() const { return fromVkFormat(format_); }
    uint32_t imageCount() const { return static_cast<uint32_t>(images_.size()); }
    VkImage image(uint32_t i) const { return images_[i]; }
    VkFramebuffer framebuffer(uint32_t i) const { return framebuffers_[i]; }
    VkSemaphore renderFinished(uint32_t i) const { return renderFinished_[i]; }
    bool captureSupported() const { return transferSrc_; }
    uint32_t recreateCount() const { return recreateCount_; }
    std::string presentModeName() const;
    std::string transformName() const;
    std::string currentTransformName() const;
    double lastRebuildMs() const { return lastRebuildMs_; }
    PlatformWindow& window() const { return window_; }

private:
    void destroyImageResources();

    VulkanDevice& device_;
    PlatformWindow& window_;
    VkSurfaceKHR surface_;
    VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
    VkFormat format_ = VK_FORMAT_UNDEFINED;
    VkColorSpaceKHR colorSpace_ = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    VkPresentModeKHR presentMode_ = VK_PRESENT_MODE_FIFO_KHR;
    VkSurfaceTransformFlagBitsKHR transform_ = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    VkSurfaceTransformFlagBitsKHR currentTransform_ = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    gfx::Extent2D extent_;
    bool transferSrc_ = false;
    bool dirty_ = false;
    uint32_t recreateCount_ = 0;
    double lastRebuildMs_ = 0.0;
    std::vector<VkImage> images_;
    std::vector<VkImageView> views_;
    std::vector<VkFramebuffer> framebuffers_;
    std::vector<VkSemaphore> renderFinished_;
};

}  // namespace pe::vk
