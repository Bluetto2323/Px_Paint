// Capability discovery for the Vulkan backend: everything is read from the
// selected VkPhysicalDevice. Optional structures (subgroup, driver properties)
// are only chained when the device version / extension allows it.
#include <algorithm>
#include <string>

#include "core/Log.h"
#include "graphics/vulkan/VulkanBackend.h"
#include "graphics/vulkan/VulkanDevice.h"

namespace pe::vk {

namespace {
std::string subgroupStages(VkShaderStageFlags s) {
    std::string out;
    auto add = [&](VkShaderStageFlags bit, const char* name) {
        if (s & bit) out += (out.empty() ? "" : "|") + std::string(name);
    };
    add(VK_SHADER_STAGE_VERTEX_BIT, "vertex");
    add(VK_SHADER_STAGE_FRAGMENT_BIT, "fragment");
    add(VK_SHADER_STAGE_COMPUTE_BIT, "compute");
    return out.empty() ? "none" : out;
}

std::string subgroupOps(VkSubgroupFeatureFlags f) {
    std::string out;
    auto add = [&](VkSubgroupFeatureFlags bit, const char* name) {
        if (f & bit) out += (out.empty() ? "" : "|") + std::string(name);
    };
    add(VK_SUBGROUP_FEATURE_BASIC_BIT, "basic");
    add(VK_SUBGROUP_FEATURE_VOTE_BIT, "vote");
    add(VK_SUBGROUP_FEATURE_ARITHMETIC_BIT, "arithmetic");
    add(VK_SUBGROUP_FEATURE_BALLOT_BIT, "ballot");
    add(VK_SUBGROUP_FEATURE_SHUFFLE_BIT, "shuffle");
    add(VK_SUBGROUP_FEATURE_SHUFFLE_RELATIVE_BIT, "shuffle_rel");
    add(VK_SUBGROUP_FEATURE_CLUSTERED_BIT, "clustered");
    add(VK_SUBGROUP_FEATURE_QUAD_BIT, "quad");
    return out.empty() ? "none" : out;
}

uint32_t maxSampleCount(VkSampleCountFlags f) {
    for (uint32_t s = 64; s > 1; s >>= 1)
        if (f & s) return s;
    return 1;
}
}  // namespace

void VulkanDevice::queryCapabilities() {
    auto& c = caps_;
    c = {};
    c.backend = gfx::BackendType::Vulkan;

    VkPhysicalDeviceProperties p{};
    vkGetPhysicalDeviceProperties(physicalDevice_, &p);
    VkPhysicalDeviceFeatures feat{};
    vkGetPhysicalDeviceFeatures(physicalDevice_, &feat);

    c.deviceName = p.deviceName;
    c.vendorId = p.vendorID;
    c.deviceId = p.deviceID;
    c.vendorName = vendorName(p.vendorID);
    c.deviceType = deviceTypeString(p.deviceType);
    c.driverVersion = driverVersionString(p.vendorID, p.driverVersion);

    // Usable API version = min(device version, version the instance requested).
    const uint32_t used = std::min(p.apiVersion, backend_.instanceApiVersion());
    c.apiMajor = VK_API_VERSION_MAJOR(p.apiVersion);
    c.apiMinor = VK_API_VERSION_MINOR(p.apiVersion);
    c.apiPatch = VK_API_VERSION_PATCH(p.apiVersion);
    c.apiVersionString = "Vulkan " + versionString(p.apiVersion) + " (device), " + versionString(used) + " usable";
    c.instanceVersionString = "loader " + versionString(backend_.loaderVersion()) + ", instance " +
                              versionString(backend_.instanceApiVersion());
    c.shadingLanguage = "SPIR-V 1.0";
    c.instanceExtensions = backend_.availableInstanceExtensions();
    c.layers = backend_.availableLayers();

    uint32_t extCount = 0;
    vkEnumerateDeviceExtensionProperties(physicalDevice_, nullptr, &extCount, nullptr);
    std::vector<VkExtensionProperties> exts(extCount);
    vkEnumerateDeviceExtensionProperties(physicalDevice_, nullptr, &extCount, exts.data());
    for (const auto& e : exts) c.deviceExtensions.emplace_back(e.extensionName);
    auto hasExt = [&](const char* n) { return c.hasDeviceExtension(n); };

    // ---- properties2 chain (subgroup = 1.1 core; driver props = extension)
    const bool device11 = p.apiVersion >= VK_API_VERSION_1_1;
    PFN_vkGetPhysicalDeviceProperties2 getProps2 = nullptr;
    if (backend_.instanceApiVersion() >= VK_API_VERSION_1_1 && vkGetPhysicalDeviceProperties2)
        getProps2 = vkGetPhysicalDeviceProperties2;
    else if (vkGetPhysicalDeviceProperties2KHR)
        getProps2 = vkGetPhysicalDeviceProperties2KHR;

    if (getProps2) {
        VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
        VkPhysicalDeviceSubgroupProperties sub{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES};
        VkPhysicalDeviceDriverPropertiesKHR drv{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES_KHR};
        void** next = &p2.pNext;
        if (device11) {
            *next = &sub;
            next = &sub.pNext;
        }
        const bool driverProps = hasExt(VK_KHR_DRIVER_PROPERTIES_EXTENSION_NAME);
        if (driverProps) {
            *next = &drv;
            next = &drv.pNext;
        }
        getProps2(physicalDevice_, &p2);
        if (device11) {
            c.subgroup.queried = true;
            c.subgroup.size = sub.subgroupSize;
            c.subgroup.supportedStages = subgroupStages(sub.supportedStages);
            c.subgroup.supportedOperations = subgroupOps(sub.supportedOperations);
        }
        if (driverProps) c.driverInfo = std::string(drv.driverName) + " / " + drv.driverInfo;
    } else {
        c.warnings.push_back("vkGetPhysicalDeviceProperties2 unavailable: subgroup/driver info not queried");
    }

    // ---- limits
    const auto& L = p.limits;
    auto& l = c.limits;
    l.maxTextureSize2D = L.maxImageDimension2D;
    l.maxFramebufferWidth = L.maxFramebufferWidth;
    l.maxFramebufferHeight = L.maxFramebufferHeight;
    l.maxColorAttachments = L.maxColorAttachments;
    l.maxVertexAttributes = L.maxVertexInputAttributes;
    l.maxUniformBufferRange = L.maxUniformBufferRange;
    l.maxPushConstantsSize = L.maxPushConstantsSize;
    l.maxArrayTextureLayers = L.maxImageArrayLayers;
    l.maxSamples = maxSampleCount(L.framebufferColorSampleCounts);
    l.maxSamplerAnisotropy = feat.samplerAnisotropy ? L.maxSamplerAnisotropy : 0.0f;
    l.maxComputeWorkGroupInvocations = L.maxComputeWorkGroupInvocations;
    l.maxMemoryAllocationCount = L.maxMemoryAllocationCount;
    l.nonCoherentAtomSize = L.nonCoherentAtomSize;
    l.timestampPeriodNs = L.timestampComputeAndGraphics ? L.timestampPeriod : 0.0;

    // ---- memory
    c.memory.known = true;
    for (uint32_t h = 0; h < memoryProps_.memoryHeapCount; ++h) {
        if (memoryProps_.memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)
            c.memory.deviceLocalBytes += memoryProps_.memoryHeaps[h].size;
    }
    std::vector<bool> hostHeap(memoryProps_.memoryHeapCount, false);
    for (uint32_t t = 0; t < memoryProps_.memoryTypeCount; ++t) {
        const auto flags = memoryProps_.memoryTypes[t].propertyFlags;
        if (flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) hostHeap[memoryProps_.memoryTypes[t].heapIndex] = true;
        if ((flags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) && (flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT))
            c.memory.unifiedMemory = true;
    }
    for (uint32_t h = 0; h < memoryProps_.memoryHeapCount; ++h)
        if (hostHeap[h]) c.memory.hostVisibleBytes += memoryProps_.memoryHeaps[h].size;

    // ---- formats (optimal tiling)
    for (size_t i = 1; i < static_cast<size_t>(gfx::TextureFormat::Count); ++i) {
        const auto tf = static_cast<gfx::TextureFormat>(i);
        const VkFormat vf = toVkFormat(tf);
        if (vf == VK_FORMAT_UNDEFINED) continue;
        VkFormatProperties fp{};
        vkGetPhysicalDeviceFormatProperties(physicalDevice_, vf, &fp);
        const auto f = fp.optimalTilingFeatures;
        auto& fs = c.format(tf);
        fs.sampled = f & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
        fs.linearFilter = f & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
        fs.colorAttachment = f & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT;
        fs.blend = f & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT;
        fs.storage = f & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT;
        fs.depthAttachment = f & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT;
    }

    // ---- verified optional features
    c.addFeature("Vulkan 1.1 device", device11, versionString(p.apiVersion));
    c.addFeature("Subgroup operations", c.subgroup.queried && c.subgroup.size > 0,
                 c.subgroup.queried ? "size " + std::to_string(c.subgroup.size) : "");
    c.addFeature("Anisotropic filtering", feat.samplerAnisotropy == VK_TRUE);
    c.addFeature("ASTC LDR textures", feat.textureCompressionASTC_LDR == VK_TRUE);
    c.addFeature("Fragment stores & atomics", feat.fragmentStoresAndAtomics == VK_TRUE);
    c.addFeature("Independent blend", feat.independentBlend == VK_TRUE);
    c.addFeature("GPU timestamps", L.timestampComputeAndGraphics == VK_TRUE);
    c.addFeature("Half-float render targets", c.format(gfx::TextureFormat::RGBA16_FLOAT).colorAttachment);
    c.addFeature("RGBA16F blending", c.format(gfx::TextureFormat::RGBA16_FLOAT).blend);
    c.addFeature("RGBA8 storage images", c.format(gfx::TextureFormat::RGBA8_UNORM).storage);
    c.addFeature("Unified memory (device-local + host-visible)", c.memory.unifiedMemory);
    c.addFeature("VK_KHR_dynamic_rendering", hasExt("VK_KHR_dynamic_rendering"));
    c.addFeature("VK_KHR_synchronization2", hasExt("VK_KHR_synchronization2"));
    c.addFeature("VK_KHR_timeline_semaphore", hasExt("VK_KHR_timeline_semaphore"));
    c.addFeature("Rasterization order attachment access (framebuffer fetch)",
                 hasExt("VK_EXT_rasterization_order_attachment_access") ||
                     hasExt("VK_ARM_rasterization_order_attachment_access"));
    c.addFeature("VK_EXT_memory_budget", hasExt("VK_EXT_memory_budget"));
    c.addFeature("VK_GOOGLE_display_timing", hasExt("VK_GOOGLE_display_timing"));
    c.addFeature("VK_KHR_incremental_present", hasExt("VK_KHR_incremental_present"));
    c.addFeature("Validation layer", backend_.validationEnabled());
    c.addFeature("Debug labels (VK_EXT_debug_utils)", backend_.debugUtilsEnabled());

    if (!device11) c.warnings.push_back("Vulkan 1.0 device: 1.1 features (subgroups) disabled");
    PE_LOGI("Vulkan", "Capabilities: %s, %zu device extensions, max texture %u, subgroup %u",
            c.apiVersionString.c_str(), c.deviceExtensions.size(), l.maxTextureSize2D, c.subgroup.size);
}

}  // namespace pe::vk
