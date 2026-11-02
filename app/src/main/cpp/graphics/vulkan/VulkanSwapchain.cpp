#include "graphics/vulkan/VulkanSwapchain.h"

#include <algorithm>

#include "core/Log.h"
#include "core/Time.h"
#include "graphics/vulkan/VulkanDevice.h"
#include "platform/PlatformWindow.h"

namespace pe::vk {

namespace {
constexpr const char* kTag = "Vulkan.Swapchain";

VkSurfaceFormatKHR chooseFormat(const std::vector<VkSurfaceFormatKHR>& formats) {
    // UNORM first: the engine controls color encoding explicitly, and it
    // matches what the GLES backend gets from EGL, so both look identical.
    const VkFormat preferred[] = {VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_R8G8B8A8_SRGB,
                                  VK_FORMAT_B8G8R8A8_SRGB};
    for (VkFormat p : preferred)
        for (const auto& f : formats)
            if (f.format == p && f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) return f;
    return formats.front();
}
}  // namespace

VulkanSwapchain::VulkanSwapchain(VulkanDevice& device, PlatformWindow& window, VkSurfaceKHR surface)
    : device_(device), window_(window), surface_(surface) {}

// Destruction order (owner guarantees vkDeviceWaitIdle first):
// framebuffers -> image views -> swapchain (owns the images) -> semaphores
// -> VkSurfaceKHR. The window reference is released by the Engine only after
// this destructor ran.
VulkanSwapchain::~VulkanSwapchain() {
    destroyImageResources();
    if (swapchain_) vkDestroySwapchainKHR(device_.device(), swapchain_, nullptr);
    for (auto s : renderFinished_) vkDestroySemaphore(device_.device(), s, nullptr);
    renderFinished_.clear();
    if (surface_) vkDestroySurfaceKHR(device_.instance(), surface_, nullptr);
}

void VulkanSwapchain::destroyImageResources() {
    VkDevice d = device_.device();
    for (auto fb : framebuffers_) vkDestroyFramebuffer(d, fb, nullptr);
    for (auto v : views_) vkDestroyImageView(d, v, nullptr);
    framebuffers_.clear();
    views_.clear();
    images_.clear();
    // renderFinished_ is NOT destroyed here: without
    // VK_EXT_swapchain_maintenance1 there is no way to know that the
    // presentation engine finished waiting on a semaphore, so rebuilds reuse
    // them and they are destroyed only with the swapchain object.
}

VulkanSwapchain::BuildResult VulkanSwapchain::rebuild(gfx::Extent2D requested) {
    const int64_t t0 = nowNs();
    VkPhysicalDevice pd = device_.physicalDevice();
    VkDevice d = device_.device();

    VkSurfaceCapabilitiesKHR caps{};
    VkResult r = vkGetPhysicalDeviceSurfaceCapabilitiesKHR(pd, surface_, &caps);
    if (r == VK_ERROR_SURFACE_LOST_KHR) return BuildResult::SurfaceLost;
    if (!PE_VK_CHECK(r)) return BuildResult::Failed;

    gfx::Extent2D extent;
    if (caps.currentExtent.width != 0xFFFFFFFFu) {
        extent = {caps.currentExtent.width, caps.currentExtent.height};
    } else {
        extent = requested.empty() ? gfx::Extent2D{window_.width(), window_.height()} : requested;
        extent.width = std::clamp(extent.width, caps.minImageExtent.width, caps.maxImageExtent.width);
        extent.height = std::clamp(extent.height, caps.minImageExtent.height, caps.maxImageExtent.height);
    }
    if (extent.empty()) return BuildResult::ZeroExtent;

    uint32_t count = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(pd, surface_, &count, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(count);
    vkGetPhysicalDeviceSurfaceFormatsKHR(pd, surface_, &count, formats.data());
    if (formats.empty()) {
        PE_LOGE(kTag, "Surface reports no formats");
        return BuildResult::Failed;
    }
    const VkSurfaceFormatKHR sf = chooseFormat(formats);

    // FIFO is the only mode guaranteed by the spec and is vsync-paced.
    presentMode_ = VK_PRESENT_MODE_FIFO_KHR;

    uint32_t desiredImages = std::max(caps.minImageCount + 1, 2u);
    if (caps.maxImageCount > 0) desiredImages = std::min(desiredImages, caps.maxImageCount);

    // Pre-rotation is not implemented yet: ask for IDENTITY and let the
    // compositor rotate. On Android this may return VK_SUBOPTIMAL_KHR while the
    // device is rotated; that is accepted (see present()).
    currentTransform_ = caps.currentTransform;
    transform_ = (caps.supportedTransforms & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR)
                     ? VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR
                     : caps.currentTransform;

    VkCompositeAlphaFlagBitsKHR alpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    for (VkCompositeAlphaFlagBitsKHR a :
         {VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR, VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR,
          VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR, VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR}) {
        if (caps.supportedCompositeAlpha & a) {
            alpha = a;
            break;
        }
    }

    transferSrc_ = (caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) != 0;
    VkImageUsageFlags usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    if (transferSrc_) usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;

    VkSwapchainCreateInfoKHR ci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    ci.surface = surface_;
    ci.minImageCount = desiredImages;
    ci.imageFormat = sf.format;
    ci.imageColorSpace = sf.colorSpace;
    ci.imageExtent = {extent.width, extent.height};
    ci.imageArrayLayers = 1;
    ci.imageUsage = usage;
    ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.preTransform = transform_;
    ci.compositeAlpha = alpha;
    ci.presentMode = presentMode_;
    ci.clipped = VK_TRUE;
    ci.oldSwapchain = swapchain_;

    VkSwapchainKHR newSwapchain = VK_NULL_HANDLE;
    r = vkCreateSwapchainKHR(d, &ci, nullptr, &newSwapchain);
    // The old swapchain is retired either way.
    destroyImageResources();
    if (swapchain_) vkDestroySwapchainKHR(d, swapchain_, nullptr);
    swapchain_ = VK_NULL_HANDLE;
    if (r == VK_ERROR_SURFACE_LOST_KHR || r == VK_ERROR_NATIVE_WINDOW_IN_USE_KHR) {
        PE_LOGE(kTag, "vkCreateSwapchainKHR: %s", resultString(r));
        return BuildResult::SurfaceLost;
    }
    if (!PE_VK_CHECK(r)) return BuildResult::Failed;
    swapchain_ = newSwapchain;
    // From here on, any failure must leave NO half-built swapchain behind
    // (valid() false), otherwise beginFrame would index missing framebuffers.
    auto fail = [&]() {
        destroyImageResources();
        vkDestroySwapchainKHR(d, swapchain_, nullptr);
        swapchain_ = VK_NULL_HANDLE;
        return BuildResult::Failed;
    };
    format_ = sf.format;
    colorSpace_ = sf.colorSpace;
    extent_ = extent;

    count = 0;
    vkGetSwapchainImagesKHR(d, swapchain_, &count, nullptr);
    images_.resize(count);
    vkGetSwapchainImagesKHR(d, swapchain_, &count, images_.data());

    if (count == 0) return fail();

    VkRenderPass compatiblePass = device_.renderPass(format_, gfx::LoadOp::Clear, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
    if (!compatiblePass) return fail();

    // One "render finished" semaphore per image (reused across rebuilds).
    while (renderFinished_.size() < images_.size()) {
        VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        VkSemaphore sem = VK_NULL_HANDLE;
        if (!PE_VK_CHECK(vkCreateSemaphore(d, &si, nullptr, &sem))) return fail();
        renderFinished_.push_back(sem);
    }

    for (VkImage image : images_) {
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = image;
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = format_;
        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkImageView view = VK_NULL_HANDLE;
        if (!PE_VK_CHECK(vkCreateImageView(d, &vi, nullptr, &view))) return fail();
        views_.push_back(view);

        VkFramebufferCreateInfo fi{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        fi.renderPass = compatiblePass;
        fi.attachmentCount = 1;
        fi.pAttachments = &view;
        fi.width = extent.width;
        fi.height = extent.height;
        fi.layers = 1;
        VkFramebuffer fb = VK_NULL_HANDLE;
        if (!PE_VK_CHECK(vkCreateFramebuffer(d, &fi, nullptr, &fb))) return fail();
        framebuffers_.push_back(fb);
    }

    dirty_ = false;
    recreateCount_++;
    lastRebuildMs_ = nsToMs(nowNs() - t0);
    PE_LOGI(kTag, "Swapchain #%u: %ux%u %s, %u images, %s, pre-transform %s (display %s), readback %s, %.2f ms",
            recreateCount_, extent.width, extent.height, gfx::toString(format()), imageCount(),
            presentModeName().c_str(), transformName().c_str(), currentTransformName().c_str(),
            transferSrc_ ? "yes" : "no", lastRebuildMs_);
    return BuildResult::Ok;
}

bool VulkanSwapchain::queryCurrent(gfx::Extent2D& extent, VkSurfaceTransformFlagBitsKHR& transform) const {
    VkSurfaceCapabilitiesKHR caps{};
    if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(device_.physicalDevice(), surface_, &caps) != VK_SUCCESS) return false;
    extent = caps.currentExtent.width == 0xFFFFFFFFu ? gfx::Extent2D{} : gfx::Extent2D{caps.currentExtent.width, caps.currentExtent.height};
    transform = caps.currentTransform;
    return true;
}

bool VulkanSwapchain::matches(gfx::Extent2D desired) const {
    if (desired.empty() || extent_ == desired) return true;
    const bool quarterTurn = currentTransform_ == VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR ||
                             currentTransform_ == VK_SURFACE_TRANSFORM_ROTATE_270_BIT_KHR;
    return quarterTurn && extent_.width == desired.height && extent_.height == desired.width;
}

VkResult VulkanSwapchain::acquire(VkSemaphore signal, uint32_t& imageIndex) {
    // Bounded wait: a stuck compositor must not hold the render thread, which
    // must stay responsive to surfaceDestroyed. With FIFO and minImageCount+1
    // images a normal acquire waits at most ~1 vsync; a timeout skips a frame.
    constexpr uint64_t kTimeoutNs = 100'000'000ull;
    return vkAcquireNextImageKHR(device_.device(), swapchain_, kTimeoutNs, signal, VK_NULL_HANDLE, &imageIndex);
}

VkResult VulkanSwapchain::present(VkQueue queue, uint32_t imageIndex) {
    VkSemaphore wait = renderFinished_[imageIndex];
    VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &wait;
    pi.swapchainCount = 1;
    pi.pSwapchains = &swapchain_;
    pi.pImageIndices = &imageIndex;
    return vkQueuePresentKHR(queue, &pi);
}

std::string VulkanSwapchain::presentModeName() const {
    switch (presentMode_) {
        case VK_PRESENT_MODE_FIFO_KHR: return "FIFO";
        case VK_PRESENT_MODE_MAILBOX_KHR: return "MAILBOX";
        case VK_PRESENT_MODE_IMMEDIATE_KHR: return "IMMEDIATE";
        case VK_PRESENT_MODE_FIFO_RELAXED_KHR: return "FIFO_RELAXED";
        default: return "other";
    }
}

namespace {
std::string transformString(VkSurfaceTransformFlagBitsKHR t) {
    switch (t) {
        case VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR: return "IDENTITY";
        case VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR: return "ROTATE_90";
        case VK_SURFACE_TRANSFORM_ROTATE_180_BIT_KHR: return "ROTATE_180";
        case VK_SURFACE_TRANSFORM_ROTATE_270_BIT_KHR: return "ROTATE_270";
        default: return "other";
    }
}
}  // namespace

std::string VulkanSwapchain::currentTransformName() const { return transformString(currentTransform_); }

std::string VulkanSwapchain::transformName() const {
    switch (transform_) {
        case VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR: return "IDENTITY";
        case VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR: return "ROTATE_90";
        case VK_SURFACE_TRANSFORM_ROTATE_180_BIT_KHR: return "ROTATE_180";
        case VK_SURFACE_TRANSFORM_ROTATE_270_BIT_KHR: return "ROTATE_270";
        default: return "other";
    }
}

}  // namespace pe::vk
