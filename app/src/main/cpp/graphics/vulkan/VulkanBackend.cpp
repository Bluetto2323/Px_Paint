#include "graphics/vulkan/VulkanBackend.h"

#include <algorithm>
#include <cstring>

#include "core/Log.h"
#include "graphics/vulkan/VulkanDevice.h"

namespace pe::vk {

namespace {
constexpr const char* kTag = "Vulkan";
constexpr const char* kValidationLayer = "VK_LAYER_KHRONOS_validation";

bool contains(const std::vector<std::string>& v, const char* s) {
    return std::find(v.begin(), v.end(), s) != v.end();
}

VKAPI_ATTR VkBool32 VKAPI_CALL debugMessenger(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                              VkDebugUtilsMessageTypeFlagsEXT,
                                              const VkDebugUtilsMessengerCallbackDataEXT* data, void*) {
    const char* msg = data && data->pMessage ? data->pMessage : "(null)";
    if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)
        PE_LOGE("Vulkan.validation", "%s", msg);
    else if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT)
        PE_LOGW("Vulkan.validation", "%s", msg);
    return VK_FALSE;
}

struct Candidate {
    VkPhysicalDevice device = VK_NULL_HANDLE;
    uint32_t queueFamily = 0;
    int score = -1;
    std::string name;
};
}  // namespace

std::unique_ptr<gfx::GraphicsBackend> createVulkanBackend() { return std::make_unique<VulkanBackend>(); }

VulkanBackend::~VulkanBackend() {
    if (messenger_ && vkDestroyDebugUtilsMessengerEXT) vkDestroyDebugUtilsMessengerEXT(instance_, messenger_, nullptr);
    if (instance_) {
        vkDestroyInstance(instance_, nullptr);
        PE_LOGI(kTag, "Instance destroyed");
    }
    loader::resetInstance();
}

bool VulkanBackend::initialize(const gfx::BackendInitOptions& options, std::string& error) {
    options_ = options;
    const char* loadError = nullptr;
    if (!loader::loadLibrary(loadError)) {
        error = loadError;
        return false;
    }

    if (vkEnumerateInstanceVersion) vkEnumerateInstanceVersion(&loaderVersion_);  // absent => 1.0
    // Never demand 1.1: request it only when the loader supports it.
    requestedApiVersion_ = loaderVersion_ >= VK_API_VERSION_1_1 ? VK_API_VERSION_1_1 : VK_API_VERSION_1_0;

    uint32_t count = 0;
    vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr);
    std::vector<VkExtensionProperties> exts(count);
    vkEnumerateInstanceExtensionProperties(nullptr, &count, exts.data());
    for (const auto& e : exts) instanceExtensions_.emplace_back(e.extensionName);

    count = 0;
    vkEnumerateInstanceLayerProperties(&count, nullptr);
    std::vector<VkLayerProperties> layers(count);
    vkEnumerateInstanceLayerProperties(&count, layers.data());
    for (const auto& l : layers) layers_.emplace_back(l.layerName);

    PE_LOGI(kTag, "Loader %s, %zu instance extensions, %zu layers", versionString(loaderVersion_).c_str(),
            instanceExtensions_.size(), layers_.size());

    // Surface extensions are mandatory for presenting.
    std::vector<const char*> enable;
    auto require = [&](const char* name) -> bool {
        if (!contains(instanceExtensions_, name)) {
            error = std::string("required instance extension missing: ") + name;
            return false;
        }
        enable.push_back(name);
        return true;
    };
    if (!require(VK_KHR_SURFACE_EXTENSION_NAME)) return false;
    if (options.windowKind == WindowKind::AndroidNativeWindow) {
        if (!require(VK_KHR_ANDROID_SURFACE_EXTENSION_NAME)) return false;
    } else {
        if (!require(VK_EXT_HEADLESS_SURFACE_EXTENSION_NAME)) return false;
    }
    // Needed to query subgroup properties on 1.0 instances.
    if (requestedApiVersion_ < VK_API_VERSION_1_1 &&
        contains(instanceExtensions_, VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME))
        enable.push_back(VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME);

    std::vector<const char*> enableLayers;
    if (options.enableValidation) {
        if (contains(layers_, kValidationLayer)) {
            enableLayers.push_back(kValidationLayer);
            validationEnabled_ = true;
        } else {
            PE_LOGW(kTag, "Validation requested but %s is not installed (package it in jniLibs)", kValidationLayer);
        }
        if (contains(instanceExtensions_, VK_EXT_DEBUG_UTILS_EXTENSION_NAME)) {
            enable.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
            debugUtils_ = true;
        }
    }

    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = options.applicationName;
    app.applicationVersion = VK_MAKE_VERSION(0, 1, 0);
    app.pEngineName = "PaintEngine";
    app.engineVersion = VK_MAKE_VERSION(0, 1, 0);
    app.apiVersion = requestedApiVersion_;

    VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ci.pApplicationInfo = &app;
    ci.enabledExtensionCount = static_cast<uint32_t>(enable.size());
    ci.ppEnabledExtensionNames = enable.data();
    ci.enabledLayerCount = static_cast<uint32_t>(enableLayers.size());
    ci.ppEnabledLayerNames = enableLayers.data();

    VkResult r = vkCreateInstance(&ci, nullptr, &instance_);
    if (r != VK_SUCCESS) {
        error = std::string("vkCreateInstance failed: ") + resultString(r);
        instance_ = VK_NULL_HANDLE;
        return false;
    }
    for (const char* e : enable) enabledExtensions_.emplace_back(e);

    const char* missing = nullptr;
    if (!loader::loadInstance(instance_, missing)) {
        error = std::string("instance function missing: ") + missing;
        return false;
    }

    if (debugUtils_ && vkCreateDebugUtilsMessengerEXT) {
        VkDebugUtilsMessengerCreateInfoEXT mi{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
        mi.messageSeverity =
            VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        mi.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                         VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                         VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
        mi.pfnUserCallback = &debugMessenger;
        PE_VK_CHECK(vkCreateDebugUtilsMessengerEXT(instance_, &mi, nullptr, &messenger_));
    }
    PE_LOGI(kTag, "Instance created (API %s requested%s)", versionString(requestedApiVersion_).c_str(),
            validationEnabled_ ? ", validation ON" : "");
    return true;
}

VkResult VulkanBackend::createSurface(PlatformWindow& window, VkSurfaceKHR& out) const {
    out = VK_NULL_HANDLE;
    if (window.kind() == WindowKind::AndroidNativeWindow) {
        if (!vkCreateAndroidSurfaceKHR) return VK_ERROR_EXTENSION_NOT_PRESENT;
        VkAndroidSurfaceCreateInfoKHR ci{VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR};
        ci.window = static_cast<ANativeWindow*>(window.nativeHandle());
        return vkCreateAndroidSurfaceKHR(instance_, &ci, nullptr, &out);
    }
    if (!vkCreateHeadlessSurfaceEXT) return VK_ERROR_EXTENSION_NOT_PRESENT;
    VkHeadlessSurfaceCreateInfoEXT ci{VK_STRUCTURE_TYPE_HEADLESS_SURFACE_CREATE_INFO_EXT};
    return vkCreateHeadlessSurfaceEXT(instance_, &ci, nullptr, &out);
}

std::unique_ptr<gfx::GraphicsDevice> VulkanBackend::createDevice(PlatformWindow* window, std::string& error) {
    uint32_t count = 0;
    VkResult r = vkEnumeratePhysicalDevices(instance_, &count, nullptr);
    if (r != VK_SUCCESS || count == 0) {
        error = std::string("no Vulkan physical device (") + resultString(r) + ")";
        return nullptr;
    }
    std::vector<VkPhysicalDevice> devices(count);
    vkEnumeratePhysicalDevices(instance_, &count, devices.data());

    VkSurfaceKHR probeSurface = VK_NULL_HANDLE;
    if (window) {
        r = createSurface(*window, probeSurface);
        if (r != VK_SUCCESS) {
            error = std::string("surface creation failed: ") + resultString(r);
            return nullptr;
        }
    }

    Candidate best;
    for (VkPhysicalDevice pd : devices) {
        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(pd, &props);
        Candidate c;
        c.device = pd;
        c.name = props.deviceName;

        uint32_t extCount = 0;
        vkEnumerateDeviceExtensionProperties(pd, nullptr, &extCount, nullptr);
        std::vector<VkExtensionProperties> exts(extCount);
        vkEnumerateDeviceExtensionProperties(pd, nullptr, &extCount, exts.data());
        const bool swapchain = std::any_of(exts.begin(), exts.end(), [](const VkExtensionProperties& e) {
            return std::strcmp(e.extensionName, VK_KHR_SWAPCHAIN_EXTENSION_NAME) == 0;
        });
        if (!swapchain) {
            PE_LOGW(kTag, "GPU '%s' skipped: no VK_KHR_swapchain", c.name.c_str());
            continue;
        }

        uint32_t qCount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(pd, &qCount, nullptr);
        std::vector<VkQueueFamilyProperties> queues(qCount);
        vkGetPhysicalDeviceQueueFamilyProperties(pd, &qCount, queues.data());
        bool found = false;
        for (uint32_t i = 0; i < qCount && !found; ++i) {
            if (!(queues[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) || queues[i].queueCount == 0) continue;
            VkBool32 present = VK_TRUE;
            if (probeSurface) vkGetPhysicalDeviceSurfaceSupportKHR(pd, i, probeSurface, &present);
            if (present) {
                c.queueFamily = i;
                found = true;
            }
        }
        if (!found) {
            PE_LOGW(kTag, "GPU '%s' skipped: no graphics queue able to present", c.name.c_str());
            continue;
        }

        switch (props.deviceType) {
            case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU: c.score = 400; break;
            case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: c.score = 300; break;
            case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU: c.score = 200; break;
            default: c.score = 100; break;
        }
        c.score += static_cast<int>(VK_API_VERSION_MINOR(props.apiVersion));
        PE_LOGI(kTag, "GPU candidate '%s' (%s, Vulkan %s) score %d", c.name.c_str(),
                deviceTypeString(props.deviceType), versionString(props.apiVersion).c_str(), c.score);
        if (c.score > best.score) best = c;
    }
    if (probeSurface) vkDestroySurfaceKHR(instance_, probeSurface, nullptr);

    if (!best.device) {
        error = "no physical device with graphics + present + swapchain";
        return nullptr;
    }
    auto device = std::make_unique<VulkanDevice>(*this, best.device, best.queueFamily);
    if (!device->initialize(error)) return nullptr;
    return device;
}

std::string VulkanBackend::describe() const {
    return "Vulkan loader " + versionString(loaderVersion_) + ", instance API " + versionString(requestedApiVersion_) +
           (validationEnabled_ ? ", validation" : "");
}

}  // namespace pe::vk
