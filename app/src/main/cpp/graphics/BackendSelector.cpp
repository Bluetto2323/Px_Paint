#include "graphics/BackendSelector.h"

#include <algorithm>

#include "core/Log.h"
#include "core/Time.h"
#include "graphics/opengles/GLESBackend.h"
#include "graphics/vulkan/VulkanBackend.h"

namespace pe::gfx {

namespace {
constexpr const char* kTag = "BackendSelector";

bool isExcluded(const SelectionOptions& o, BackendType t) {
    return std::find(o.excluded.begin(), o.excluded.end(), t) != o.excluded.end();
}
}  // namespace

std::unique_ptr<GraphicsBackend> BackendSelector::create(BackendType type) {
    switch (type) {
        case BackendType::Vulkan: return vk::createVulkanBackend();
        case BackendType::OpenGLES: return gles::createGLESBackend();
        case BackendType::None: break;
    }
    return nullptr;
}

std::vector<BackendType> BackendSelector::candidateOrder(const SelectionOptions& o) {
    std::vector<BackendType> order;
    switch (o.preference) {
        case BackendPreference::Auto:
            order = {BackendType::Vulkan, BackendType::OpenGLES};
            break;
        case BackendPreference::ForceVulkan:
            order = {BackendType::Vulkan};
            if (o.allowFallback) order.push_back(BackendType::OpenGLES);
            break;
        case BackendPreference::ForceOpenGLES:
            order = {BackendType::OpenGLES};
            if (o.allowFallback) order.push_back(BackendType::Vulkan);
            break;
    }
    order.erase(std::remove_if(order.begin(), order.end(), [&](BackendType t) { return isExcluded(o, t); }),
                order.end());
    return order;
}

BackendSelection BackendSelector::select(const SelectionOptions& options, PlatformWindow& window) {
    BackendSelection result;
    const auto order = candidateOrder(options);
    PE_LOGI(kTag, "Selecting backend: preference=%s fallback=%s validation=%s candidates=%zu",
            toString(options.preference), options.allowFallback ? "on" : "off",
            options.enableValidation ? "on" : "off", order.size());

    for (BackendType type : order) {
        BackendAttempt attempt;
        attempt.type = type;
        const int64_t t0 = nowNs();
        std::string error;

        auto backend = create(type);
        BackendInitOptions init;
        init.enableValidation = options.enableValidation;
        init.windowKind = window.kind();

        std::unique_ptr<GraphicsDevice> device;
        if (!backend) {
            attempt.failedStage = "create";
            attempt.reason = "backend not compiled in";
        } else if (!backend->initialize(init, error)) {
            attempt.failedStage = "initialize";
            attempt.reason = error;
        } else if (!(device = backend->createDevice(&window, error))) {
            attempt.failedStage = "createDevice";
            attempt.reason = error;
        } else if (auto why = options.requirements.check(device->capabilities()); !why.empty()) {
            attempt.failedStage = "requirements";
            attempt.reason = why;
        } else if (!device->attachSurface(window)) {
            attempt.failedStage = "attachSurface";
            attempt.reason = "could not create swapchain / window surface (see log)";
        } else {
            attempt.success = true;
        }
        attempt.durationMs = nsToMs(nowNs() - t0);

        if (attempt.success) {
            PE_LOGI(kTag, "Selected %s in %.1f ms", toString(type), attempt.durationMs);
            result.attempts.push_back(attempt);
            result.backend = std::move(backend);
            result.device = std::move(device);
            return result;
        }

        PE_LOGW(kTag, "%s rejected at stage '%s': %s", toString(type), attempt.failedStage.c_str(),
                attempt.reason.c_str());
        result.attempts.push_back(attempt);
        device.reset();   // device before backend
        backend.reset();
    }

    PE_LOGE(kTag, "No usable graphics backend (%zu attempt(s))", result.attempts.size());
    return result;
}

}  // namespace pe::gfx
