#include "graphics/vulkan/VulkanLoader.h"

#include <dlfcn.h>

namespace pe::vk {

#define PE_VK_DEFINE(name) PFN_##name name = nullptr;
PE_VK_DEFINE(vkGetInstanceProcAddr)
PE_VK_GLOBAL_FUNCTIONS(PE_VK_DEFINE)
PE_VK_GLOBAL_OPTIONAL_FUNCTIONS(PE_VK_DEFINE)
PE_VK_INSTANCE_FUNCTIONS(PE_VK_DEFINE)
PE_VK_INSTANCE_OPTIONAL_FUNCTIONS(PE_VK_DEFINE)
PE_VK_DEVICE_FUNCTIONS(PE_VK_DEFINE)
#undef PE_VK_DEFINE

namespace loader {

namespace {
void* gLibrary = nullptr;
}

bool loadLibrary(const char*& errorOut) {
    if (vkGetInstanceProcAddr) return true;
#if defined(__ANDROID__)
    const char* candidates[] = {"libvulkan.so"};
#else
    const char* candidates[] = {"libvulkan.so.1", "libvulkan.so"};
#endif
    for (const char* name : candidates) {
        gLibrary = dlopen(name, RTLD_NOW | RTLD_LOCAL);
        if (gLibrary) break;
    }
    if (!gLibrary) {
        errorOut = "libvulkan not found (dlopen failed)";
        return false;
    }
    vkGetInstanceProcAddr = reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(gLibrary, "vkGetInstanceProcAddr"));
    if (!vkGetInstanceProcAddr) {
        errorOut = "vkGetInstanceProcAddr not exported by libvulkan";
        return false;
    }
#define PE_VK_LOAD_GLOBAL(name) name = reinterpret_cast<PFN_##name>(vkGetInstanceProcAddr(nullptr, #name));
    PE_VK_GLOBAL_FUNCTIONS(PE_VK_LOAD_GLOBAL)
    PE_VK_GLOBAL_OPTIONAL_FUNCTIONS(PE_VK_LOAD_GLOBAL)
#undef PE_VK_LOAD_GLOBAL
#define PE_VK_CHECK_GLOBAL(name)               \
    if (!name) {                               \
        errorOut = "missing global " #name;    \
        vkGetInstanceProcAddr = nullptr;       \
        return false;                          \
    }
    PE_VK_GLOBAL_FUNCTIONS(PE_VK_CHECK_GLOBAL)
#undef PE_VK_CHECK_GLOBAL
    return true;
}

bool loadInstance(VkInstance instance, const char*& missingOut) {
#define PE_VK_LOAD_INSTANCE(name) name = reinterpret_cast<PFN_##name>(vkGetInstanceProcAddr(instance, #name));
    PE_VK_INSTANCE_FUNCTIONS(PE_VK_LOAD_INSTANCE)
    PE_VK_INSTANCE_OPTIONAL_FUNCTIONS(PE_VK_LOAD_INSTANCE)
#undef PE_VK_LOAD_INSTANCE
#define PE_VK_CHECK_INSTANCE(name) \
    if (!name) {                   \
        missingOut = #name;        \
        return false;              \
    }
    PE_VK_INSTANCE_FUNCTIONS(PE_VK_CHECK_INSTANCE)
#undef PE_VK_CHECK_INSTANCE
    return true;
}

bool loadDevice(VkDevice device, const char*& missingOut) {
#define PE_VK_LOAD_DEVICE(name) name = reinterpret_cast<PFN_##name>(vkGetDeviceProcAddr(device, #name));
    PE_VK_DEVICE_FUNCTIONS(PE_VK_LOAD_DEVICE)
#undef PE_VK_LOAD_DEVICE
#define PE_VK_CHECK_DEVICE(name) \
    if (!name) {                 \
        missingOut = #name;      \
        return false;            \
    }
    PE_VK_DEVICE_FUNCTIONS(PE_VK_CHECK_DEVICE)
#undef PE_VK_CHECK_DEVICE
    return true;
}

void resetInstance() {
#define PE_VK_RESET(name) name = nullptr;
    PE_VK_INSTANCE_FUNCTIONS(PE_VK_RESET)
    PE_VK_INSTANCE_OPTIONAL_FUNCTIONS(PE_VK_RESET)
#undef PE_VK_RESET
}

void resetDevice() {
#define PE_VK_RESET(name) name = nullptr;
    PE_VK_DEVICE_FUNCTIONS(PE_VK_RESET)
#undef PE_VK_RESET
}

}  // namespace loader
}  // namespace pe::vk
