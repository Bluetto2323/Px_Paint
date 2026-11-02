#pragma once

#include <array>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <tuple>
#include <vector>

#include "graphics/abstraction/GraphicsDevice.h"
#include "graphics/vulkan/VulkanCommon.h"
#include "graphics/vulkan/VulkanRenderingContext.h"
#include "graphics/vulkan/VulkanResources.h"
#include "graphics/vulkan/VulkanSwapchain.h"

namespace pe::vk {

class VulkanBackend;

class VulkanDevice final : public gfx::GraphicsDevice {
public:
    static constexpr uint32_t kFramesInFlight = 2;

    VulkanDevice(VulkanBackend& backend, VkPhysicalDevice physicalDevice, uint32_t queueFamily);
    ~VulkanDevice() override;

    bool initialize(std::string& error);

    // GraphicsDevice
    gfx::BackendType backendType() const override { return gfx::BackendType::Vulkan; }
    const gfx::GraphicsCapabilities& capabilities() const override { return caps_; }

    bool attachSurface(PlatformWindow& window) override;
    void detachSurface() override;
    void notifySurfaceResized(gfx::Extent2D extent) override;
    gfx::SurfaceState surfaceState() const override;
    bool needsFrame() const override;

    gfx::Ref<gfx::BufferResource> createBuffer(const gfx::BufferDesc& desc, const void* data) override;
    gfx::Ref<gfx::TextureResource> createTexture(const gfx::TextureDesc& desc) override;
    gfx::Ref<gfx::ShaderResource> createShader(const gfx::ShaderDesc& desc) override;
    gfx::Ref<gfx::PipelineResource> createPipeline(const gfx::PipelineDesc& desc) override;
    gfx::ResourceManager& resources() override { return resources_; }

    gfx::FrameStatus beginFrame(uint64_t serial, gfx::FrameContext& out) override;
    gfx::FrameStatus endFrame() override;
    void waitIdle() override;

    bool requestCapture() override;
    bool takeCapture(gfx::CaptureResult& out) override;

    // Internal API used by resources / swapchain / context.
    VkDevice device() const { return device_; }
    VkPhysicalDevice physicalDevice() const { return physicalDevice_; }
    VkInstance instance() const;
    VkPipelineCache pipelineCache() const { return pipelineCache_; }
    bool debugLabels() const { return debugLabels_; }

    bool allocate(const VkMemoryRequirements& req, VkMemoryPropertyFlags required, VkMemoryPropertyFlags preferred,
                  Allocation& out);
    void free(Allocation& allocation);
    // Records and submits a one-off command buffer, then waits for it.
    // Initialization-time uploads only.
    bool submitImmediate(const std::function<void(VkCommandBuffer)>& record);
    // Render passes are cached; all passes with the same format are
    // compatible, so pipelines / framebuffers can be built with any of them.
    VkRenderPass renderPass(VkFormat format, gfx::LoadOp load, VkImageLayout finalLayout);
    void setObjectName(VkObjectType type, uint64_t handle, const char* name);

private:
    struct Frame {
        VkCommandPool pool = VK_NULL_HANDLE;
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        VkFence fence = VK_NULL_HANDLE;
        VkSemaphore imageAvailable = VK_NULL_HANDLE;
        uint64_t serial = 0;
    };

    void queryCapabilities();
    bool recycleFrameSync(Frame& f);
    gfx::FrameStatus statusFromResult(VkResult r, const char* what);
    void recordCapture(VkCommandBuffer cmd);
    void finishCapture();
    void destroyCaptureBuffer();

    VulkanBackend& backend_;
    VkPhysicalDevice physicalDevice_;
    uint32_t queueFamily_;
    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue queue_ = VK_NULL_HANDLE;
    VkPipelineCache pipelineCache_ = VK_NULL_HANDLE;
    VkCommandPool uploadPool_ = VK_NULL_HANDLE;
    VkPhysicalDeviceMemoryProperties memoryProps_{};
    uint32_t deviceApiVersion_ = 0;
    bool debugLabels_ = false;
    uint32_t allocationCount_ = 0;

    gfx::GraphicsCapabilities caps_;
    gfx::ResourceManager resources_;

    std::array<Frame, kFramesInFlight> frames_{};
    uint32_t frameIndex_ = 0;
    uint64_t completedSerial_ = 0;
    uint64_t recordingSerial_ = 0;
    // KI-1: window size from surfaceChanged, chased until the swapchain has it.
    gfx::Extent2D desiredExtent_;
    uint32_t resizeChaseFrames_ = 0;
    void chaseResize();
    bool inFrame_ = false;
    uint32_t imageIndex_ = 0;

    std::unique_ptr<VulkanSwapchain> swapchain_;
    VulkanRenderingContext context_;
    VulkanBackbuffer backbuffer_;
    std::map<std::tuple<VkFormat, int, VkImageLayout>, VkRenderPass> renderPasses_;

    // Capture (diagnostics self-test)
    bool captureRequested_ = false;
    bool captureRecorded_ = false;
    bool captureReady_ = false;
    VkBuffer captureBuffer_ = VK_NULL_HANDLE;
    Allocation captureAlloc_;
    gfx::Extent2D captureExtent_;
    VkFormat captureFormat_ = VK_FORMAT_UNDEFINED;
    gfx::CaptureResult capture_;
};

}  // namespace pe::vk
