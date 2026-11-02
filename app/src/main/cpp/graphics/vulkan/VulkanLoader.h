// Paint Engine - dynamic Vulkan loader.
//
// libvulkan is opened with dlopen() instead of being linked, so the app starts
// even where Vulkan is missing or broken and can fall back to OpenGL ES.
// Entry points are resolved in three tiers:
//   global   -> vkGetInstanceProcAddr(NULL, ...)
//   instance -> vkGetInstanceProcAddr(instance, ...)
//   device   -> vkGetDeviceProcAddr(device, ...) (skips loader trampolines)
// Optional entry points may stay null and MUST be checked before use.
//
// The engine owns at most one VkInstance/VkDevice at a time, which is why the
// pointers are process-global.
#pragma once

#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
// Type definitions only; the Android surface entry point is resolved at
// runtime and only called for WindowKind::AndroidNativeWindow.
#ifndef VK_USE_PLATFORM_ANDROID_KHR
#define VK_USE_PLATFORM_ANDROID_KHR
#endif
#include <vulkan/vulkan.h>

namespace pe::vk {

#define PE_VK_GLOBAL_FUNCTIONS(X)            \
    X(vkCreateInstance)                      \
    X(vkEnumerateInstanceExtensionProperties) \
    X(vkEnumerateInstanceLayerProperties)

#define PE_VK_GLOBAL_OPTIONAL_FUNCTIONS(X) X(vkEnumerateInstanceVersion)

#define PE_VK_INSTANCE_FUNCTIONS(X)                \
    X(vkDestroyInstance)                           \
    X(vkEnumeratePhysicalDevices)                  \
    X(vkGetPhysicalDeviceProperties)               \
    X(vkGetPhysicalDeviceFeatures)                 \
    X(vkGetPhysicalDeviceQueueFamilyProperties)    \
    X(vkGetPhysicalDeviceMemoryProperties)         \
    X(vkGetPhysicalDeviceFormatProperties)         \
    X(vkEnumerateDeviceExtensionProperties)        \
    X(vkCreateDevice)                              \
    X(vkGetDeviceProcAddr)

#define PE_VK_INSTANCE_OPTIONAL_FUNCTIONS(X)       \
    X(vkDestroySurfaceKHR)                         \
    X(vkGetPhysicalDeviceSurfaceSupportKHR)        \
    X(vkGetPhysicalDeviceSurfaceCapabilitiesKHR)   \
    X(vkGetPhysicalDeviceSurfaceFormatsKHR)        \
    X(vkGetPhysicalDeviceSurfacePresentModesKHR)   \
    X(vkCreateAndroidSurfaceKHR)                   \
    X(vkCreateHeadlessSurfaceEXT)                  \
    X(vkGetPhysicalDeviceProperties2)              \
    X(vkGetPhysicalDeviceProperties2KHR)           \
    X(vkCreateDebugUtilsMessengerEXT)              \
    X(vkDestroyDebugUtilsMessengerEXT)             \
    X(vkCmdBeginDebugUtilsLabelEXT)                \
    X(vkCmdEndDebugUtilsLabelEXT)                  \
    X(vkSetDebugUtilsObjectNameEXT)

#define PE_VK_DEVICE_FUNCTIONS(X)        \
    X(vkDestroyDevice)                   \
    X(vkGetDeviceQueue)                  \
    X(vkDeviceWaitIdle)                  \
    X(vkQueueSubmit)                     \
    X(vkQueueWaitIdle)                   \
    X(vkCreateSwapchainKHR)              \
    X(vkDestroySwapchainKHR)             \
    X(vkGetSwapchainImagesKHR)           \
    X(vkAcquireNextImageKHR)             \
    X(vkQueuePresentKHR)                 \
    X(vkCreateImage)                     \
    X(vkDestroyImage)                    \
    X(vkCreateImageView)                 \
    X(vkDestroyImageView)                \
    X(vkGetImageMemoryRequirements)      \
    X(vkBindImageMemory)                 \
    X(vkCreateBuffer)                    \
    X(vkDestroyBuffer)                   \
    X(vkGetBufferMemoryRequirements)     \
    X(vkBindBufferMemory)                \
    X(vkAllocateMemory)                  \
    X(vkFreeMemory)                      \
    X(vkMapMemory)                       \
    X(vkUnmapMemory)                     \
    X(vkFlushMappedMemoryRanges)         \
    X(vkInvalidateMappedMemoryRanges)    \
    X(vkCreateRenderPass)                \
    X(vkDestroyRenderPass)               \
    X(vkCreateFramebuffer)               \
    X(vkDestroyFramebuffer)              \
    X(vkCreateShaderModule)              \
    X(vkDestroyShaderModule)             \
    X(vkCreatePipelineLayout)            \
    X(vkDestroyPipelineLayout)           \
    X(vkCreatePipelineCache)             \
    X(vkDestroyPipelineCache)            \
    X(vkCreateGraphicsPipelines)         \
    X(vkDestroyPipeline)                 \
    X(vkCreateCommandPool)               \
    X(vkDestroyCommandPool)              \
    X(vkResetCommandPool)                \
    X(vkAllocateCommandBuffers)          \
    X(vkFreeCommandBuffers)              \
    X(vkBeginCommandBuffer)              \
    X(vkEndCommandBuffer)                \
    X(vkCreateFence)                     \
    X(vkDestroyFence)                    \
    X(vkWaitForFences)                   \
    X(vkResetFences)                     \
    X(vkCreateSemaphore)                 \
    X(vkDestroySemaphore)                \
    X(vkCmdBeginRenderPass)              \
    X(vkCmdEndRenderPass)                \
    X(vkCmdBindPipeline)                 \
    X(vkCmdBindVertexBuffers)            \
    X(vkCmdDraw)                         \
    X(vkCmdSetViewport)                  \
    X(vkCmdSetScissor)                   \
    X(vkCmdPipelineBarrier)              \
    X(vkCmdCopyBuffer)                   \
    X(vkCmdCopyImageToBuffer)

#define PE_VK_DECLARE(name) extern PFN_##name name;
PE_VK_DECLARE(vkGetInstanceProcAddr)
PE_VK_GLOBAL_FUNCTIONS(PE_VK_DECLARE)
PE_VK_GLOBAL_OPTIONAL_FUNCTIONS(PE_VK_DECLARE)
PE_VK_INSTANCE_FUNCTIONS(PE_VK_DECLARE)
PE_VK_INSTANCE_OPTIONAL_FUNCTIONS(PE_VK_DECLARE)
PE_VK_DEVICE_FUNCTIONS(PE_VK_DECLARE)
#undef PE_VK_DECLARE

namespace loader {
// Opens libvulkan and resolves global functions. Idempotent.
bool loadLibrary(const char*& errorOut);
// Resolves instance-level functions; returns false if a required one is null.
bool loadInstance(VkInstance instance, const char*& missingOut);
// Resolves device-level functions; returns false if a required one is null.
bool loadDevice(VkDevice device, const char*& missingOut);
// Clears instance/device pointers (library stays mapped; unloading drivers
// mid-process is known to be unsafe on several Android vendors).
void resetInstance();
void resetDevice();
}  // namespace loader

}  // namespace pe::vk
