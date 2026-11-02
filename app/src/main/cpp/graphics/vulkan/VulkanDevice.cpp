#include "graphics/vulkan/VulkanDevice.h"

#include <cstring>

#include "core/Log.h"
#include "core/TestHooks.h"
#include "graphics/vulkan/VulkanBackend.h"
#include "platform/PlatformWindow.h"

namespace pe::vk {

namespace {
constexpr const char* kTag = "Vulkan";
constexpr uint64_t kFenceTimeoutNs = 3'000'000'000ull;  // GPU hang detection
}  // namespace

VulkanDevice::VulkanDevice(VulkanBackend& backend, VkPhysicalDevice physicalDevice, uint32_t queueFamily)
    : backend_(backend), physicalDevice_(physicalDevice), queueFamily_(queueFamily), context_(*this) {}

VkInstance VulkanDevice::instance() const { return backend_.instance(); }

VulkanDevice::~VulkanDevice() {
    if (!device_) {
        loader::resetDevice();
        return;
    }
    vkDeviceWaitIdle(device_);
    swapchain_.reset();
    destroyCaptureBuffer();
    resources_.shutdown();  // destroys retired objects, reports leaks
    for (auto& f : frames_) {
        if (f.fence) vkDestroyFence(device_, f.fence, nullptr);
        if (f.imageAvailable) vkDestroySemaphore(device_, f.imageAvailable, nullptr);
        if (f.pool) vkDestroyCommandPool(device_, f.pool, nullptr);
    }
    if (uploadPool_) vkDestroyCommandPool(device_, uploadPool_, nullptr);
    for (auto& [key, pass] : renderPasses_) vkDestroyRenderPass(device_, pass, nullptr);
    if (pipelineCache_) vkDestroyPipelineCache(device_, pipelineCache_, nullptr);
    if (allocationCount_ != 0) PE_LOGE(kTag, "%u device memory allocation(s) leaked", allocationCount_);
    vkDestroyDevice(device_, nullptr);
    loader::resetDevice();
    PE_LOGI(kTag, "Device destroyed");
}

bool VulkanDevice::initialize(std::string& error) {
    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(physicalDevice_, &props);
    deviceApiVersion_ = props.apiVersion;
    vkGetPhysicalDeviceMemoryProperties(physicalDevice_, &memoryProps_);

    queryCapabilities();  // needs only the physical device

    std::vector<const char*> extensions = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    const float priority = 1.0f;
    VkDeviceQueueCreateInfo qi{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qi.queueFamilyIndex = queueFamily_;
    qi.queueCount = 1;
    qi.pQueuePriorities = &priority;

    // No optional features are enabled in Block 1: nothing uses them yet.
    VkPhysicalDeviceFeatures features{};
    VkDeviceCreateInfo ci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    ci.queueCreateInfoCount = 1;
    ci.pQueueCreateInfos = &qi;
    ci.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
    ci.ppEnabledExtensionNames = extensions.data();
    ci.pEnabledFeatures = &features;

    VkResult r = vkCreateDevice(physicalDevice_, &ci, nullptr, &device_);
    if (r != VK_SUCCESS) {
        error = std::string("vkCreateDevice failed: ") + resultString(r);
        device_ = VK_NULL_HANDLE;
        return false;
    }
    for (const char* e : extensions) caps_.enabledExtensions.emplace_back(e);
    for (const auto& e : backend_.enabledInstanceExtensions()) caps_.enabledExtensions.push_back(e);

    const char* missing = nullptr;
    if (!loader::loadDevice(device_, missing)) {
        error = std::string("device function missing: ") + missing;
        return false;
    }
    debugLabels_ = backend_.debugUtilsEnabled() && vkCmdBeginDebugUtilsLabelEXT && vkCmdEndDebugUtilsLabelEXT;
    vkGetDeviceQueue(device_, queueFamily_, 0, &queue_);

    VkPipelineCacheCreateInfo pci{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
    PE_VK_CHECK(vkCreatePipelineCache(device_, &pci, nullptr, &pipelineCache_));

    for (auto& f : frames_) {
        VkCommandPoolCreateInfo pi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pi.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
        pi.queueFamilyIndex = queueFamily_;
        if (!PE_VK_CHECK(vkCreateCommandPool(device_, &pi, nullptr, &f.pool))) {
            error = "command pool creation failed";
            return false;
        }
        VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        ai.commandPool = f.pool;
        ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = 1;
        VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        if (!PE_VK_CHECK(vkAllocateCommandBuffers(device_, &ai, &f.cmd)) ||
            !PE_VK_CHECK(vkCreateFence(device_, &fi, nullptr, &f.fence)) ||
            !PE_VK_CHECK(vkCreateSemaphore(device_, &si, nullptr, &f.imageAvailable))) {
            error = "per-frame object creation failed";
            return false;
        }
    }
    VkCommandPoolCreateInfo upi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    upi.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    upi.queueFamilyIndex = queueFamily_;
    if (!PE_VK_CHECK(vkCreateCommandPool(device_, &upi, nullptr, &uploadPool_))) {
        error = "upload command pool creation failed";
        return false;
    }

    PE_LOGI(kTag, "Device ready: %s (%s), Vulkan %s, driver %s, queue family %u", caps_.deviceName.c_str(),
            caps_.vendorName.c_str(), caps_.apiVersion().c_str(), caps_.driverVersion.c_str(), queueFamily_);
    return true;
}

// ------------------------------------------------------------------ memory
bool VulkanDevice::allocate(const VkMemoryRequirements& req, VkMemoryPropertyFlags required,
                            VkMemoryPropertyFlags preferred, Allocation& out) {
    if (caps_.limits.maxMemoryAllocationCount && allocationCount_ >= caps_.limits.maxMemoryAllocationCount) {
        PE_LOGE(kTag, "maxMemoryAllocationCount (%u) reached", caps_.limits.maxMemoryAllocationCount);
        return false;
    }
    int best = -1;
    for (int pass = 0; pass < 2 && best < 0; ++pass) {
        const VkMemoryPropertyFlags want = pass == 0 ? (required | preferred) : required;
        for (uint32_t i = 0; i < memoryProps_.memoryTypeCount; ++i) {
            if (!(req.memoryTypeBits & (1u << i))) continue;
            if ((memoryProps_.memoryTypes[i].propertyFlags & want) == want) {
                best = static_cast<int>(i);
                break;
            }
        }
    }
    if (best < 0) return false;

    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = static_cast<uint32_t>(best);
    VkResult r = vkAllocateMemory(device_, &ai, nullptr, &out.memory);
    if (r != VK_SUCCESS) {
        PE_LOGE(kTag, "vkAllocateMemory(%llu bytes, type %d): %s", static_cast<unsigned long long>(req.size), best,
                resultString(r));
        out.memory = VK_NULL_HANDLE;
        return false;
    }
    allocationCount_++;
    out.size = req.size;
    out.typeIndex = static_cast<uint32_t>(best);
    const VkMemoryPropertyFlags flags = memoryProps_.memoryTypes[best].propertyFlags;
    out.coherent = (flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;
    if (flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) {
        if (!PE_VK_CHECK(vkMapMemory(device_, out.memory, 0, VK_WHOLE_SIZE, 0, &out.mapped))) {
            free(out);
            return false;
        }
    }
    return true;
}

void VulkanDevice::free(Allocation& a) {
    if (!a.memory) return;
    if (a.mapped) vkUnmapMemory(device_, a.memory);
    vkFreeMemory(device_, a.memory, nullptr);
    allocationCount_--;
    a = {};
}

bool VulkanDevice::submitImmediate(const std::function<void(VkCommandBuffer)>& record) {
    VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ai.commandPool = uploadPool_;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    if (!PE_VK_CHECK(vkAllocateCommandBuffers(device_, &ai, &cmd))) return false;
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &bi);
    record(cmd);
    vkEndCommandBuffer(cmd);
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    const bool ok = PE_VK_CHECK(vkQueueSubmit(queue_, 1, &si, VK_NULL_HANDLE)) && PE_VK_CHECK(vkQueueWaitIdle(queue_));
    vkFreeCommandBuffers(device_, uploadPool_, 1, &cmd);
    return ok;
}

VkRenderPass VulkanDevice::renderPass(VkFormat format, gfx::LoadOp load, VkImageLayout finalLayout) {
    const auto key = std::make_tuple(format, static_cast<int>(load), finalLayout);
    if (auto it = renderPasses_.find(key); it != renderPasses_.end()) return it->second;

    VkAttachmentDescription att{};
    att.format = format;
    att.samples = VK_SAMPLE_COUNT_1_BIT;
    switch (load) {
        case gfx::LoadOp::Clear: att.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; break;
        case gfx::LoadOp::Load: att.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD; break;
        case gfx::LoadOp::DontCare: att.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; break;
    }
    att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    att.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    att.initialLayout = load == gfx::LoadOp::Load ? finalLayout : VK_IMAGE_LAYOUT_UNDEFINED;
    att.finalLayout = finalLayout;

    VkAttachmentReference ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sub{};
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 1;
    sub.pColorAttachments = &ref;

    // Waits for the acquire semaphore (signalled at COLOR_ATTACHMENT_OUTPUT)
    // before the layout transition writes the image.
    VkSubpassDependency dep{};
    dep.srcSubpass = VK_SUBPASS_EXTERNAL;
    dep.dstSubpass = 0;
    dep.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.srcAccessMask = 0;
    dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT;

    VkRenderPassCreateInfo ci{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    ci.attachmentCount = 1;
    ci.pAttachments = &att;
    ci.subpassCount = 1;
    ci.pSubpasses = &sub;
    ci.dependencyCount = 1;
    ci.pDependencies = &dep;
    VkRenderPass pass = VK_NULL_HANDLE;
    if (!PE_VK_CHECK(vkCreateRenderPass(device_, &ci, nullptr, &pass))) return VK_NULL_HANDLE;
    renderPasses_[key] = pass;
    return pass;
}

void VulkanDevice::setObjectName(VkObjectType type, uint64_t handle, const char* name) {
    if (!debugLabels_ || !vkSetDebugUtilsObjectNameEXT || !name || !*name) return;
    VkDebugUtilsObjectNameInfoEXT ni{VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT};
    ni.objectType = type;
    ni.objectHandle = handle;
    ni.pObjectName = name;
    vkSetDebugUtilsObjectNameEXT(device_, &ni);
}

// ----------------------------------------------------------------- surface
bool VulkanDevice::attachSurface(PlatformWindow& window) {
    if (swapchain_) detachSurface();
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkResult r = backend_.createSurface(window, surface);
    if (r != VK_SUCCESS) {
        PE_LOGE(kTag, "Surface creation failed: %s", resultString(r));
        return false;
    }
    VkBool32 supported = VK_FALSE;
    vkGetPhysicalDeviceSurfaceSupportKHR(physicalDevice_, queueFamily_, surface, &supported);
    swapchain_ = std::make_unique<VulkanSwapchain>(*this, window, surface);  // owns surface now
    if (!supported) {
        PE_LOGE(kTag, "Queue family %u cannot present to this surface", queueFamily_);
        swapchain_.reset();
        return false;
    }
    const auto result = swapchain_->rebuild({window.width(), window.height()});
    if (result == VulkanSwapchain::BuildResult::Failed || result == VulkanSwapchain::BuildResult::SurfaceLost) {
        swapchain_.reset();
        return false;
    }
    if (result == VulkanSwapchain::BuildResult::ZeroExtent) swapchain_->markDirty();
    PE_LOGI(kTag, "Surface attached (%s)", toString(window.kind()));
    return true;
}

void VulkanDevice::detachSurface() {
    if (!swapchain_) return;
    if (inFrame_) {
        // Detach after an abandoned frame (exception mid-frame): its acquire
        // semaphore is signaled and its command buffer never submitted.
        PE_LOGW(kTag, "Detaching after an abandoned frame: recycling its sync objects");
        inFrame_ = false;
        captureRequested_ = false;
        captureRecorded_ = false;
        context_.begin(VK_NULL_HANDLE);
        recycleFrameSync(frames_[frameIndex_]);
    }
    // Every queued frame may reference swapchain images / semaphores; the
    // swapchain (and its VkSurfaceKHR) must be gone before the caller
    // releases the native window.
    vkDeviceWaitIdle(device_);
    swapchain_.reset();
    backbuffer_ = {};
    PE_LOGI(kTag, "Surface detached");
}

void VulkanDevice::notifySurfaceResized(gfx::Extent2D extent) {
    if (!swapchain_) return;
    if (extent != swapchain_->extent()) swapchain_->markDirty();
}

gfx::SurfaceState VulkanDevice::surfaceState() const {
    gfx::SurfaceState s;
    if (!swapchain_) return s;
    s.attached = true;
    s.extent = swapchain_->extent();
    s.format = swapchain_->format();
    s.imageCount = swapchain_->imageCount();
    s.presentMode = swapchain_->presentModeName();
    s.transform = swapchain_->transformName();
    s.recreateCount = swapchain_->recreateCount();
    s.captureSupported = swapchain_->captureSupported();
    return s;
}

// --------------------------------------------------------------- resources
gfx::Ref<gfx::BufferResource> VulkanDevice::createBuffer(const gfx::BufferDesc& desc, const void* data) {
    return resources_.track(VulkanBuffer::create(*this, desc, data));
}

gfx::Ref<gfx::TextureResource> VulkanDevice::createTexture(const gfx::TextureDesc& desc) {
    const auto& fs = caps_.format(desc.format);
    if (!fs.sampled && !fs.colorAttachment && !fs.depthAttachment) {
        PE_LOGE(kTag, "Texture '%s': format %s not supported on this device", desc.debugName.c_str(),
                gfx::toString(desc.format));
        return nullptr;
    }
    return resources_.track(VulkanTexture::create(*this, desc));
}

gfx::Ref<gfx::ShaderResource> VulkanDevice::createShader(const gfx::ShaderDesc& desc) {
    return resources_.track(VulkanShader::create(*this, desc));
}

gfx::Ref<gfx::PipelineResource> VulkanDevice::createPipeline(const gfx::PipelineDesc& desc) {
    return resources_.track(VulkanPipeline::create(*this, desc));
}

// ------------------------------------------------------------------ frames
gfx::FrameStatus VulkanDevice::statusFromResult(VkResult r, const char* what) {
    switch (r) {
        case VK_SUCCESS:
        case VK_SUBOPTIMAL_KHR: return gfx::FrameStatus::Ok;
        case VK_ERROR_OUT_OF_DATE_KHR:
            if (swapchain_) swapchain_->markDirty();
            return gfx::FrameStatus::SurfaceOutOfDate;
        case VK_TIMEOUT:
        case VK_NOT_READY:
            PE_LOGW(kTag, "%s: %s (frame skipped)", what, resultString(r));
            return gfx::FrameStatus::SurfaceOutOfDate;
        case VK_ERROR_SURFACE_LOST_KHR:
            PE_LOGE(kTag, "%s: surface lost", what);
            return gfx::FrameStatus::SurfaceLost;
        case VK_ERROR_DEVICE_LOST:
            PE_LOGE(kTag, "%s: DEVICE LOST", what);
            return gfx::FrameStatus::DeviceLost;
        default:
            PE_LOGE(kTag, "%s: %s", what, resultString(r));
            return gfx::FrameStatus::Error;
    }
}

bool VulkanDevice::recycleFrameSync(Frame& f) {
    // Used when a frame was abandoned after vkAcquireNextImageKHR (its
    // semaphore is signaled and will never be waited) or when a submit failed
    // (its fence was reset and will never be signaled). Neither object may be
    // reused in that state, so both are recreated once the GPU is idle.
    vkDeviceWaitIdle(device_);
    if (f.fence) vkDestroyFence(device_, f.fence, nullptr);
    if (f.imageAvailable) vkDestroySemaphore(device_, f.imageAvailable, nullptr);
    f.fence = VK_NULL_HANDLE;
    f.imageAvailable = VK_NULL_HANDLE;
    VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    const bool ok = PE_VK_CHECK(vkCreateFence(device_, &fi, nullptr, &f.fence)) &&
                    PE_VK_CHECK(vkCreateSemaphore(device_, &si, nullptr, &f.imageAvailable));
    // The acquired image is never presented: rebuilding the swapchain returns it.
    if (swapchain_) swapchain_->markDirty();
    return ok;
}

gfx::FrameStatus VulkanDevice::beginFrame(uint64_t serial, gfx::FrameContext& out) {
    if (!swapchain_) return gfx::FrameStatus::NoSurface;

    if (inFrame_) {
        // The caller never called endFrame() (e.g. it threw mid-frame).
        PE_LOGW(kTag, "Previous frame was abandoned without endFrame(): recycling its sync objects");
        inFrame_ = false;
        captureRequested_ = false;
        captureRecorded_ = false;
        context_.begin(VK_NULL_HANDLE);
        if (!recycleFrameSync(frames_[frameIndex_])) return gfx::FrameStatus::Error;
    }

    if (swapchain_->dirty() || !swapchain_->valid()) {
        // Every in-flight frame may reference swapchain images, views,
        // framebuffers or semaphores: wait before touching them.
        const VkResult idle = vkDeviceWaitIdle(device_);
        if (idle == VK_ERROR_DEVICE_LOST) return statusFromResult(idle, "vkDeviceWaitIdle (rebuild)");
        (void)PE_TEST_HOOK("surface.rebuild");  // timing hook only
        PlatformWindow& w = swapchain_->window();
        switch (swapchain_->rebuild({w.width(), w.height()})) {
            case VulkanSwapchain::BuildResult::Ok: break;
            case VulkanSwapchain::BuildResult::ZeroExtent:
                swapchain_->markDirty();
                return gfx::FrameStatus::SurfaceOutOfDate;
            case VulkanSwapchain::BuildResult::SurfaceLost: return gfx::FrameStatus::SurfaceLost;
            case VulkanSwapchain::BuildResult::Failed: return gfx::FrameStatus::Error;
        }
    }

    Frame& f = frames_[frameIndex_];
    VkResult r = vkWaitForFences(device_, 1, &f.fence, VK_TRUE, kFenceTimeoutNs);
    if (r == VK_TIMEOUT) {
        PE_LOGE(kTag, "GPU fence timeout (%.1f s): possible GPU hang", kFenceTimeoutNs / 1e9);
        return gfx::FrameStatus::DeviceLost;
    }
    if (r != VK_SUCCESS) return statusFromResult(r, "vkWaitForFences");
    // f.serial is the serial of the last SUCCESSFUL submit using this slot;
    // single queue => every earlier submission is complete too.
    if (f.serial > completedSerial_) completedSerial_ = f.serial;
    resources_.collect(completedSerial_);

    r = swapchain_->acquire(f.imageAvailable, imageIndex_);
    if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) {
        // No image acquired: the semaphore was not signaled, the fence is
        // still signaled -> the slot stays reusable as is.
        return statusFromResult(r, "vkAcquireNextImageKHR");
    }

    // The fence is reset in endFrame() right before vkQueueSubmit, so an
    // abandoned or failed frame never leaves an unsignaled, unsubmitted fence.
    vkResetCommandPool(device_, f.pool, 0);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(f.cmd, &bi);

    recordingSerial_ = serial;
    resources_.setRecordingSerial(serial);
    backbuffer_.extent_ = swapchain_->extent();
    backbuffer_.format_ = swapchain_->format();
    backbuffer_.vkFormat = swapchain_->vkFormat();
    backbuffer_.framebuffer = swapchain_->framebuffer(imageIndex_);
    backbuffer_.image = swapchain_->image(imageIndex_);
    context_.begin(f.cmd);
    inFrame_ = true;

    out.context = &context_;
    out.backbuffer = &backbuffer_;
    out.serial = serial;
    return gfx::FrameStatus::Ok;
}

gfx::FrameStatus VulkanDevice::endFrame() {
    if (!inFrame_) return gfx::FrameStatus::Error;
    inFrame_ = false;
    Frame& f = frames_[frameIndex_];
    context_.endRenderPass();  // tolerate a forgotten endRenderPass()

    if (captureRequested_) recordCapture(f.cmd);

    if (!PE_VK_CHECK(vkEndCommandBuffer(f.cmd))) {
        captureRecorded_ = false;
        recycleFrameSync(f);  // acquire semaphore is signaled and never waited
        return gfx::FrameStatus::Error;
    }

    const VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSemaphore signal = swapchain_->renderFinished(imageIndex_);
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.waitSemaphoreCount = 1;
    si.pWaitSemaphores = &f.imageAvailable;
    si.pWaitDstStageMask = &waitStage;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &f.cmd;
    si.signalSemaphoreCount = 1;
    si.pSignalSemaphores = &signal;
    vkResetFences(device_, 1, &f.fence);
    VkResult r = vkQueueSubmit(queue_, 1, &si, f.fence);
    if (r != VK_SUCCESS) {
        captureRecorded_ = false;
        auto status = statusFromResult(r, "vkQueueSubmit");
        if (status == gfx::FrameStatus::Ok) status = gfx::FrameStatus::Error;
        // A failed submit leaves the fence unsignaled and the semaphore
        // signaled: recycle them unless the device is gone anyway.
        if (status != gfx::FrameStatus::DeviceLost) recycleFrameSync(f);
        return status;
    }
    f.serial = recordingSerial_;  // only a submitted frame can complete

    r = swapchain_->present(queue_, imageIndex_);
    if (captureRecorded_) {
        // Diagnostics only: stall once for this frame instead of tracking the
        // copy across frame slots.
        if (vkWaitForFences(device_, 1, &f.fence, VK_TRUE, kFenceTimeoutNs) == VK_SUCCESS)
            finishCapture();
        else
            captureRecorded_ = false;
    }
    frameIndex_ = (frameIndex_ + 1) % kFramesInFlight;
    if (r == VK_SUBOPTIMAL_KHR) {
        // Android returns SUBOPTIMAL while preTransform != currentTransform
        // (rotated device). Rebuilding would not change that, so only size
        // changes (handled via notifySurfaceResized) trigger a rebuild.
        return gfx::FrameStatus::Ok;
    }
    // OUT_OF_DATE / SURFACE_LOST from present: the submit happened and the
    // present's semaphore wait still executes (spec), so no recycling needed.
    return statusFromResult(r, "vkQueuePresentKHR");
}

void VulkanDevice::waitIdle() {
    if (device_) vkDeviceWaitIdle(device_);
}

// ----------------------------------------------------------------- capture
bool VulkanDevice::requestCapture() {
    if (!swapchain_ || !swapchain_->captureSupported()) return false;
    const VkFormat fmt = swapchain_->vkFormat();
    const bool eightBit = fmt == VK_FORMAT_R8G8B8A8_UNORM || fmt == VK_FORMAT_B8G8R8A8_UNORM ||
                          fmt == VK_FORMAT_R8G8B8A8_SRGB || fmt == VK_FORMAT_B8G8R8A8_SRGB;
    if (!eightBit) return false;
    captureRequested_ = true;
    captureReady_ = false;
    return true;
}

void VulkanDevice::destroyCaptureBuffer() {
    if (captureBuffer_) vkDestroyBuffer(device_, captureBuffer_, nullptr);
    captureBuffer_ = VK_NULL_HANDLE;
    free(captureAlloc_);
}

void VulkanDevice::recordCapture(VkCommandBuffer cmd) {
    captureRequested_ = false;
    const gfx::Extent2D e = swapchain_->extent();
    const VkDeviceSize size = static_cast<VkDeviceSize>(e.width) * e.height * 4;

    destroyCaptureBuffer();
    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = size;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    if (!PE_VK_CHECK(vkCreateBuffer(device_, &bci, nullptr, &captureBuffer_))) return;
    VkMemoryRequirements req{};
    vkGetBufferMemoryRequirements(device_, captureBuffer_, &req);
    if (!allocate(req, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, VK_MEMORY_PROPERTY_HOST_CACHED_BIT, captureAlloc_) ||
        !PE_VK_CHECK(vkBindBufferMemory(device_, captureBuffer_, captureAlloc_.memory, 0))) {
        destroyCaptureBuffer();
        return;
    }

    VkImage image = swapchain_->image(imageIndex_);
    VkImageMemoryBarrier toTransfer{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    toTransfer.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    toTransfer.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    toTransfer.oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    toTransfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    toTransfer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toTransfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toTransfer.image = image;
    toTransfer.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0,
                         nullptr, 0, nullptr, 1, &toTransfer);

    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {e.width, e.height, 1};
    vkCmdCopyImageToBuffer(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, captureBuffer_, 1, &region);

    VkImageMemoryBarrier toPresent = toTransfer;
    toPresent.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    toPresent.dstAccessMask = 0;
    toPresent.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    toPresent.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    VkBufferMemoryBarrier hostRead{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
    hostRead.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    hostRead.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    hostRead.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    hostRead.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    hostRead.buffer = captureBuffer_;
    hostRead.size = VK_WHOLE_SIZE;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT | VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 1,
                         &hostRead, 1, &toPresent);

    captureExtent_ = e;
    captureFormat_ = swapchain_->vkFormat();
    captureRecorded_ = true;
}

void VulkanDevice::finishCapture() {
    // Called after the fence of the frame slot that recorded the copy.
    captureRecorded_ = false;
    if (!captureAlloc_.mapped) return;
    if (!captureAlloc_.coherent) {
        VkMappedMemoryRange range{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
        range.memory = captureAlloc_.memory;
        range.size = VK_WHOLE_SIZE;
        vkInvalidateMappedMemoryRanges(device_, 1, &range);
    }
    const size_t bytes = static_cast<size_t>(captureExtent_.width) * captureExtent_.height * 4;
    capture_.width = captureExtent_.width;
    capture_.height = captureExtent_.height;
    capture_.rgba.resize(bytes);
    std::memcpy(capture_.rgba.data(), captureAlloc_.mapped, bytes);
    if (captureFormat_ == VK_FORMAT_B8G8R8A8_UNORM || captureFormat_ == VK_FORMAT_B8G8R8A8_SRGB) {
        for (size_t i = 0; i < bytes; i += 4) std::swap(capture_.rgba[i], capture_.rgba[i + 2]);
    }
    destroyCaptureBuffer();
    captureReady_ = true;
}

bool VulkanDevice::takeCapture(gfx::CaptureResult& out) {
    if (!captureReady_) return false;
    out = std::move(capture_);
    capture_ = {};
    captureReady_ = false;
    return true;
}

}  // namespace pe::vk
