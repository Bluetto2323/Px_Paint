#include "graphics/abstraction/GraphicsCapabilities.h"

#include <algorithm>

namespace pe::gfx {

namespace {
bool contains(const std::vector<std::string>& list, const std::string& name) {
    return std::find(list.begin(), list.end(), name) != list.end();
}
}  // namespace

bool GraphicsCapabilities::hasDeviceExtension(const std::string& name) const {
    return contains(deviceExtensions, name);
}

bool GraphicsCapabilities::hasInstanceExtension(const std::string& name) const {
    return contains(instanceExtensions, name);
}

void GraphicsCapabilities::addFeature(std::string name, bool usable, std::string detail) {
    for (auto& f : features) {
        if (f.name == name) {
            f.usable = usable;
            f.detail = std::move(detail);
            return;
        }
    }
    features.push_back({std::move(name), usable, std::move(detail)});
}

bool GraphicsCapabilities::featureUsable(const std::string& name) const {
    for (const auto& f : features)
        if (f.name == name) return f.usable;
    return false;
}

std::string GraphicsCapabilities::apiVersion() const {
    if (!apiVersionString.empty()) return apiVersionString;
    return std::to_string(apiMajor) + "." + std::to_string(apiMinor) + "." + std::to_string(apiPatch);
}

std::string MinimumRequirements::check(const GraphicsCapabilities& caps) const {
    if (caps.limits.maxTextureSize2D < minTextureSize2D) {
        return "max 2D texture size " + std::to_string(caps.limits.maxTextureSize2D) + " < required " +
               std::to_string(minTextureSize2D);
    }
    if (requireRGBA8Render) {
        const auto& rgba = caps.format(TextureFormat::RGBA8_UNORM);
        const auto& bgra = caps.format(TextureFormat::BGRA8_UNORM);
        const bool ok = (rgba.sampled && rgba.colorAttachment) || (bgra.sampled && bgra.colorAttachment);
        if (!ok) return "no renderable+sampleable 8-bit RGBA/BGRA format";
    }
    return {};
}

}  // namespace pe::gfx
