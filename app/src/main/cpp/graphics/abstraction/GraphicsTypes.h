// Paint Engine - API-independent graphics types.
//
// Nothing in this header may reference Vulkan, EGL or GLES. Backends translate
// these enums into their native equivalents.
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace pe::gfx {

template <class T>
using Ref = std::shared_ptr<T>;

enum class BackendType : uint8_t { None = 0, Vulkan, OpenGLES };
const char* toString(BackendType type);

enum class BackendPreference : uint8_t {
    Auto,           // Vulkan if it passes checks, else OpenGL ES
    ForceVulkan,    // only Vulkan (fallback optional, see EngineConfig)
    ForceOpenGLES,  // only OpenGL ES
};
const char* toString(BackendPreference pref);
BackendPreference backendPreferenceFromString(const std::string& s);

enum class TextureFormat : uint8_t {
    Unknown = 0,
    RGBA8_UNORM,
    RGBA8_SRGB,
    BGRA8_UNORM,
    BGRA8_SRGB,
    R8_UNORM,
    RG8_UNORM,
    RGBA16_FLOAT,
    R16_FLOAT,
    R32_FLOAT,
    RGB10A2_UNORM,
    D24_UNORM_S8_UINT,
    D32_FLOAT,
    Count
};
const char* toString(TextureFormat format);
uint32_t bytesPerPixel(TextureFormat format);
bool isDepthFormat(TextureFormat format);

enum class VertexFormat : uint8_t { Float2, Float3, Float4, UByte4Norm };
uint32_t vertexFormatSize(VertexFormat format);

enum class ShaderStage : uint8_t { Vertex, Fragment };
const char* toString(ShaderStage stage);

enum class PrimitiveTopology : uint8_t { TriangleList, TriangleStrip, LineList };

enum class LoadOp : uint8_t { Load, Clear, DontCare };

namespace BufferUsage {
enum : uint32_t {
    Vertex = 1u << 0,
    Index = 1u << 1,
    Uniform = 1u << 2,
    TransferSrc = 1u << 3,
    TransferDst = 1u << 4,
};
}

namespace TextureUsage {
enum : uint32_t {
    Sampled = 1u << 0,
    ColorAttachment = 1u << 1,
    TransferSrc = 1u << 2,
    TransferDst = 1u << 3,
    Storage = 1u << 4,
};
}

enum class MemoryUsage : uint8_t {
    GpuOnly,   // device local, written once (staging if needed)
    CpuToGpu,  // host visible, frequently updated by CPU
    GpuToCpu,  // host visible + cached, readback
};

struct ClearColor {
    float r = 0, g = 0, b = 0, a = 1;
};

struct Extent2D {
    uint32_t width = 0;
    uint32_t height = 0;
    bool operator==(const Extent2D& o) const { return width == o.width && height == o.height; }
    bool operator!=(const Extent2D& o) const { return !(*this == o); }
    bool empty() const { return width == 0 || height == 0; }
};

// Result of a frame operation. Everything except Ok means "nothing was
// presented"; the engine decides whether to skip, rebuild or fall back.
enum class FrameStatus : uint8_t {
    Ok,
    NoSurface,         // no surface attached (app in background)
    SurfaceOutOfDate,  // swapchain must be rebuilt / zero-sized window; skip
    SurfaceLost,       // native window gone: detach and wait for a new one
    DeviceLost,        // GPU device/context lost: device must be recreated
    Error,             // unexpected API error (already logged)
};
const char* toString(FrameStatus status);

}  // namespace pe::gfx
