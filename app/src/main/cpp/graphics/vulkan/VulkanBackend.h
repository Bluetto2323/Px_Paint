#pragma once

#include <memory>
#include <string>
#include <vector>

#include "graphics/abstraction/GraphicsBackend.h"
#include "graphics/vulkan/VulkanCommon.h"

namespace pe::vk {

// VkInstance owner. Baseline is Vulkan 1.0: the instance requests 1.1 only if
// the loader reports it, and every 1.1 / extension feature is gated on what
// the selected physical device really exposes.
class VulkanBackend final : public gfx::GraphicsBackend {
public:
    VulkanBackend() = default;
    ~VulkanBackend() override;

    gfx::BackendType type() const override { return gfx::BackendType::Vulkan; }
    bool initialize(const gfx::BackendInitOptions& options, std::string& error) override;
    std::unique_ptr<gfx::GraphicsDevice> createDevice(PlatformWindow* window, std::string& error) override;
    std::string describe() const override;

    VkInstance instance() const { return instance_; }
    uint32_t instanceApiVersion() const { return requestedApiVersion_; }
    uint32_t loaderVersion() const { return loaderVersion_; }
    bool debugUtilsEnabled() const { return debugUtils_; }
    bool validationEnabled() const { return validationEnabled_; }
    const std::vector<std::string>& availableInstanceExtensions() const { return instanceExtensions_; }
    const std::vector<std::string>& enabledInstanceExtensions() const { return enabledExtensions_; }
    const std::vector<std::string>& availableLayers() const { return layers_; }

    // Creates a VkSurfaceKHR for the given window (Android or headless).
    VkResult createSurface(PlatformWindow& window, VkSurfaceKHR& out) const;

private:
    gfx::BackendInitOptions options_;
    VkInstance instance_ = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT messenger_ = VK_NULL_HANDLE;
    uint32_t loaderVersion_ = VK_API_VERSION_1_0;
    uint32_t requestedApiVersion_ = VK_API_VERSION_1_0;
    bool debugUtils_ = false;
    bool validationEnabled_ = false;
    std::vector<std::string> instanceExtensions_;
    std::vector<std::string> enabledExtensions_;
    std::vector<std::string> layers_;
};

std::unique_ptr<gfx::GraphicsBackend> createVulkanBackend();

}  // namespace pe::vk
