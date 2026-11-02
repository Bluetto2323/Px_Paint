#pragma once

#include "graphics/abstraction/GpuResource.h"
#include "graphics/abstraction/GraphicsTypes.h"

namespace pe::gfx {

// A shader is described once for every backend: SPIR-V for Vulkan, GLSL ES
// 3.00 source for OpenGL ES. The device picks the representation it needs, so
// callers never branch on the backend.
struct ShaderDesc {
    ShaderStage stage = ShaderStage::Vertex;
    const uint32_t* spirv = nullptr;  // Vulkan
    size_t spirvSizeBytes = 0;
    const char* glslEs = nullptr;     // OpenGL ES ("#version 300 es")
    std::string debugName;
};

class ShaderResource : public GpuResource {
public:
    ShaderStage stage() const { return stage_; }

protected:
    ShaderResource(ShaderStage stage, std::string name)
        : GpuResource(ResourceKind::Shader, std::move(name)), stage_(stage) {}
    ShaderStage stage_;
};

}  // namespace pe::gfx
