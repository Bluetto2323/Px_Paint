// Paint Engine - hardware capabilities discovered at runtime.
//
// Filled by each backend from real queries (vkGetPhysicalDevice*, glGet*,
// eglQueryString). Nothing here is assumed from the device model.
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "graphics/abstraction/GraphicsTypes.h"

namespace pe::gfx {

struct FormatSupport {
    bool sampled = false;
    bool linearFilter = false;
    bool colorAttachment = false;
    bool blend = false;
    bool storage = false;
    bool depthAttachment = false;
};

struct GraphicsLimits {
    uint32_t maxTextureSize2D = 0;
    uint32_t maxFramebufferWidth = 0;
    uint32_t maxFramebufferHeight = 0;
    uint32_t maxColorAttachments = 0;
    uint32_t maxVertexAttributes = 0;
    uint32_t maxUniformBufferRange = 0;
    uint32_t maxPushConstantsSize = 0;      // Vulkan only
    uint32_t maxArrayTextureLayers = 0;
    uint32_t maxSamples = 0;                // color MSAA
    float maxSamplerAnisotropy = 0.0f;      // 0 = not supported
    uint32_t maxComputeWorkGroupInvocations = 0;  // 0 = no compute
    uint32_t maxMemoryAllocationCount = 0;  // Vulkan only
    uint64_t nonCoherentAtomSize = 0;       // Vulkan only
    double timestampPeriodNs = 0.0;         // 0 = no timestamps
};

struct MemoryInfo {
    uint64_t deviceLocalBytes = 0;   // sum of device-local heaps (Vulkan)
    uint64_t hostVisibleBytes = 0;   // heaps with host-visible types
    bool unifiedMemory = false;      // device-local + host-visible type exists
    bool known = false;              // GLES usually cannot report this
};

struct SubgroupInfo {
    bool queried = false;
    uint32_t size = 0;
    std::string supportedStages;
    std::string supportedOperations;
};

// Named, explicitly verified optional features. Each entry: name -> usable?
// plus a short reason so the diagnostics report explains *why*.
struct FeatureEntry {
    std::string name;
    bool usable = false;
    std::string detail;
};

struct GraphicsCapabilities {
    BackendType backend = BackendType::None;

    // Identity
    std::string deviceName;
    std::string vendorName;
    uint32_t vendorId = 0;
    uint32_t deviceId = 0;
    std::string deviceType;   // integrated / discrete / cpu ...
    std::string driverVersion;
    std::string driverInfo;   // GL_VERSION string / Vulkan driver name+info

    // API version actually in use (not just "available").
    uint32_t apiMajor = 0;
    uint32_t apiMinor = 0;
    uint32_t apiPatch = 0;
    std::string apiVersionString;
    std::string instanceVersionString;  // Vulkan loader version / EGL version
    std::string shadingLanguage;

    std::vector<std::string> instanceExtensions;  // Vulkan instance / EGL
    std::vector<std::string> deviceExtensions;    // Vulkan device / GL
    std::vector<std::string> layers;              // Vulkan layers available
    std::vector<std::string> enabledExtensions;   // what we actually enabled

    GraphicsLimits limits;
    MemoryInfo memory;
    SubgroupInfo subgroup;
    std::array<FormatSupport, static_cast<size_t>(TextureFormat::Count)> formats{};
    std::vector<FeatureEntry> features;
    std::vector<std::string> warnings;

    const FormatSupport& format(TextureFormat f) const { return formats[static_cast<size_t>(f)]; }
    FormatSupport& format(TextureFormat f) { return formats[static_cast<size_t>(f)]; }

    bool hasDeviceExtension(const std::string& name) const;
    bool hasInstanceExtension(const std::string& name) const;
    void addFeature(std::string name, bool usable, std::string detail = {});
    bool featureUsable(const std::string& name) const;

    std::string apiVersion() const;
};

// Requirements every backend must satisfy to be selected. Failing these makes
// the BackendSelector reject the backend (and try the next one).
struct MinimumRequirements {
    uint32_t minTextureSize2D = 2048;
    bool requireRGBA8Render = true;

    // Returns empty string when satisfied, otherwise the reason.
    std::string check(const GraphicsCapabilities& caps) const;
};

}  // namespace pe::gfx
