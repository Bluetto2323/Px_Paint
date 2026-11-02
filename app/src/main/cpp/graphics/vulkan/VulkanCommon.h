#pragma once

#include <cstdint>
#include <string>

#include "graphics/abstraction/GraphicsTypes.h"
#include "graphics/vulkan/VulkanLoader.h"

namespace pe::vk {

const char* resultString(VkResult result);
std::string versionString(uint32_t apiVersion);
const char* deviceTypeString(VkPhysicalDeviceType type);
std::string vendorName(uint32_t vendorId);
// Driver version encoding is vendor specific (NVIDIA, Qualcomm, ARM...).
std::string driverVersionString(uint32_t vendorId, uint32_t driverVersion);

VkFormat toVkFormat(gfx::TextureFormat format);
gfx::TextureFormat fromVkFormat(VkFormat format);

// Logs non-success results. Returns true on VK_SUCCESS.
bool check(VkResult result, const char* what);

}  // namespace pe::vk

#define PE_VK_CHECK(expr) ::pe::vk::check((expr), #expr)
