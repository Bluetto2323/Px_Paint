#include "graphics/vulkan/VulkanCommon.h"

#include <cstdio>

#include "core/Log.h"

namespace pe::vk {

const char* resultString(VkResult r) {
    switch (r) {
#define PE_CASE(x) \
    case x: return #x;
        PE_CASE(VK_SUCCESS)
        PE_CASE(VK_NOT_READY)
        PE_CASE(VK_TIMEOUT)
        PE_CASE(VK_EVENT_SET)
        PE_CASE(VK_EVENT_RESET)
        PE_CASE(VK_INCOMPLETE)
        PE_CASE(VK_ERROR_OUT_OF_HOST_MEMORY)
        PE_CASE(VK_ERROR_OUT_OF_DEVICE_MEMORY)
        PE_CASE(VK_ERROR_INITIALIZATION_FAILED)
        PE_CASE(VK_ERROR_DEVICE_LOST)
        PE_CASE(VK_ERROR_MEMORY_MAP_FAILED)
        PE_CASE(VK_ERROR_LAYER_NOT_PRESENT)
        PE_CASE(VK_ERROR_EXTENSION_NOT_PRESENT)
        PE_CASE(VK_ERROR_FEATURE_NOT_PRESENT)
        PE_CASE(VK_ERROR_INCOMPATIBLE_DRIVER)
        PE_CASE(VK_ERROR_TOO_MANY_OBJECTS)
        PE_CASE(VK_ERROR_FORMAT_NOT_SUPPORTED)
        PE_CASE(VK_ERROR_FRAGMENTED_POOL)
        PE_CASE(VK_ERROR_OUT_OF_POOL_MEMORY)
        PE_CASE(VK_ERROR_SURFACE_LOST_KHR)
        PE_CASE(VK_ERROR_NATIVE_WINDOW_IN_USE_KHR)
        PE_CASE(VK_SUBOPTIMAL_KHR)
        PE_CASE(VK_ERROR_OUT_OF_DATE_KHR)
        PE_CASE(VK_ERROR_VALIDATION_FAILED_EXT)
#undef PE_CASE
        default: return "VK_RESULT_UNKNOWN";
    }
}

bool check(VkResult result, const char* what) {
    if (result == VK_SUCCESS) return true;
    PE_LOGE("Vulkan", "%s -> %s (%d)", what, resultString(result), static_cast<int>(result));
    return false;
}

std::string versionString(uint32_t v) {
    return std::to_string(VK_API_VERSION_MAJOR(v)) + "." + std::to_string(VK_API_VERSION_MINOR(v)) + "." +
           std::to_string(VK_API_VERSION_PATCH(v));
}

const char* deviceTypeString(VkPhysicalDeviceType t) {
    switch (t) {
        case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: return "integrated GPU";
        case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU: return "discrete GPU";
        case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU: return "virtual GPU";
        case VK_PHYSICAL_DEVICE_TYPE_CPU: return "CPU (software)";
        default: return "other";
    }
}

std::string vendorName(uint32_t id) {
    switch (id) {
        case 0x13B5: return "ARM";
        case 0x5143: return "Qualcomm";
        case 0x1010: return "Imagination";
        case 0x144D: return "Samsung";
        case 0x10DE: return "NVIDIA";
        case 0x1002: return "AMD";
        case 0x8086: return "Intel";
        case 0x1AE0: return "Google";
        case 0x10005: return "Mesa";
        default: {
            char buf[16];
            std::snprintf(buf, sizeof(buf), "0x%04X", id);
            return buf;
        }
    }
}

std::string driverVersionString(uint32_t vendorId, uint32_t v) {
    if (vendorId == 0x10DE) {  // NVIDIA: 10.8.8.6
        return std::to_string((v >> 22) & 0x3ff) + "." + std::to_string((v >> 14) & 0xff) + "." +
               std::to_string((v >> 6) & 0xff) + "." + std::to_string(v & 0x3f);
    }
    // ARM (Mali rNpM -> N.M.0), Qualcomm, Imagination, Mesa use the standard
    // VK_MAKE_VERSION packing.
    return std::to_string(VK_VERSION_MAJOR(v)) + "." + std::to_string(VK_VERSION_MINOR(v)) + "." +
           std::to_string(VK_VERSION_PATCH(v));
}

VkFormat toVkFormat(gfx::TextureFormat f) {
    using F = gfx::TextureFormat;
    switch (f) {
        case F::RGBA8_UNORM: return VK_FORMAT_R8G8B8A8_UNORM;
        case F::RGBA8_SRGB: return VK_FORMAT_R8G8B8A8_SRGB;
        case F::BGRA8_UNORM: return VK_FORMAT_B8G8R8A8_UNORM;
        case F::BGRA8_SRGB: return VK_FORMAT_B8G8R8A8_SRGB;
        case F::R8_UNORM: return VK_FORMAT_R8_UNORM;
        case F::RG8_UNORM: return VK_FORMAT_R8G8_UNORM;
        case F::RGBA16_FLOAT: return VK_FORMAT_R16G16B16A16_SFLOAT;
        case F::R16_FLOAT: return VK_FORMAT_R16_SFLOAT;
        case F::R32_FLOAT: return VK_FORMAT_R32_SFLOAT;
        case F::RGB10A2_UNORM: return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
        case F::D24_UNORM_S8_UINT: return VK_FORMAT_D24_UNORM_S8_UINT;
        case F::D32_FLOAT: return VK_FORMAT_D32_SFLOAT;
        default: return VK_FORMAT_UNDEFINED;
    }
}

gfx::TextureFormat fromVkFormat(VkFormat f) {
    for (size_t i = 1; i < static_cast<size_t>(gfx::TextureFormat::Count); ++i) {
        const auto tf = static_cast<gfx::TextureFormat>(i);
        if (toVkFormat(tf) == f) return tf;
    }
    return gfx::TextureFormat::Unknown;
}

}  // namespace pe::vk
