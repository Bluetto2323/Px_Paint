#include "graphics/abstraction/GraphicsTypes.h"

#include <algorithm>
#include <cctype>

namespace pe::gfx {

const char* toString(BackendType type) {
    switch (type) {
        case BackendType::None: return "None";
        case BackendType::Vulkan: return "Vulkan";
        case BackendType::OpenGLES: return "OpenGL ES";
    }
    return "?";
}

const char* toString(BackendPreference pref) {
    switch (pref) {
        case BackendPreference::Auto: return "auto";
        case BackendPreference::ForceVulkan: return "vulkan";
        case BackendPreference::ForceOpenGLES: return "gles";
    }
    return "?";
}

BackendPreference backendPreferenceFromString(const std::string& s) {
    std::string v = s;
    std::transform(v.begin(), v.end(), v.begin(), [](unsigned char c) { return std::tolower(c); });
    if (v == "vulkan" || v == "vk") return BackendPreference::ForceVulkan;
    if (v == "gles" || v == "opengles" || v == "gl") return BackendPreference::ForceOpenGLES;
    return BackendPreference::Auto;
}

const char* toString(TextureFormat format) {
    switch (format) {
        case TextureFormat::Unknown: return "Unknown";
        case TextureFormat::RGBA8_UNORM: return "RGBA8_UNORM";
        case TextureFormat::RGBA8_SRGB: return "RGBA8_SRGB";
        case TextureFormat::BGRA8_UNORM: return "BGRA8_UNORM";
        case TextureFormat::BGRA8_SRGB: return "BGRA8_SRGB";
        case TextureFormat::R8_UNORM: return "R8_UNORM";
        case TextureFormat::RG8_UNORM: return "RG8_UNORM";
        case TextureFormat::RGBA16_FLOAT: return "RGBA16_FLOAT";
        case TextureFormat::R16_FLOAT: return "R16_FLOAT";
        case TextureFormat::R32_FLOAT: return "R32_FLOAT";
        case TextureFormat::RGB10A2_UNORM: return "RGB10A2_UNORM";
        case TextureFormat::D24_UNORM_S8_UINT: return "D24_UNORM_S8_UINT";
        case TextureFormat::D32_FLOAT: return "D32_FLOAT";
        case TextureFormat::Count: break;
    }
    return "?";
}

uint32_t bytesPerPixel(TextureFormat format) {
    switch (format) {
        case TextureFormat::R8_UNORM: return 1;
        case TextureFormat::RG8_UNORM:
        case TextureFormat::R16_FLOAT: return 2;
        case TextureFormat::RGBA16_FLOAT: return 8;
        case TextureFormat::Unknown:
        case TextureFormat::Count: return 0;
        default: return 4;
    }
}

bool isDepthFormat(TextureFormat format) {
    return format == TextureFormat::D24_UNORM_S8_UINT || format == TextureFormat::D32_FLOAT;
}

uint32_t vertexFormatSize(VertexFormat format) {
    switch (format) {
        case VertexFormat::Float2: return 8;
        case VertexFormat::Float3: return 12;
        case VertexFormat::Float4: return 16;
        case VertexFormat::UByte4Norm: return 4;
    }
    return 0;
}

const char* toString(ShaderStage stage) {
    return stage == ShaderStage::Vertex ? "vertex" : "fragment";
}

const char* toString(FrameStatus status) {
    switch (status) {
        case FrameStatus::Ok: return "Ok";
        case FrameStatus::NoSurface: return "NoSurface";
        case FrameStatus::SurfaceOutOfDate: return "SurfaceOutOfDate";
        case FrameStatus::SurfaceLost: return "SurfaceLost";
        case FrameStatus::DeviceLost: return "DeviceLost";
        case FrameStatus::Error: return "Error";
    }
    return "?";
}

}  // namespace pe::gfx
