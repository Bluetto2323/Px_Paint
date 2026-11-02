#pragma once

#include <vector>

#include "graphics/abstraction/GpuResource.h"
#include "graphics/abstraction/GraphicsTypes.h"
#include "graphics/abstraction/ShaderResource.h"

namespace pe::gfx {

struct VertexAttribute {
    uint32_t location = 0;
    VertexFormat format = VertexFormat::Float2;
    uint32_t offset = 0;
};

struct VertexLayout {
    uint32_t stride = 0;
    std::vector<VertexAttribute> attributes;
};

enum class BlendMode : uint8_t { Opaque, PremultipliedAlpha };

// Graphics pipeline state. Not in the original component list but required to
// draw anything in a backend-neutral way (Vulkan needs baked pipelines; the
// GLES backend emulates them with program + VAO + state).
struct PipelineDesc {
    Ref<ShaderResource> vertexShader;
    Ref<ShaderResource> fragmentShader;
    VertexLayout vertexLayout;
    PrimitiveTopology topology = PrimitiveTopology::TriangleList;
    BlendMode blend = BlendMode::Opaque;
    TextureFormat colorFormat = TextureFormat::RGBA8_UNORM;  // target format
    uint32_t constantsSize = 0;   // bytes of per-draw constants (see RenderingContext::setConstants)
    bool sampledTexture = false;  // one combined texture+sampler at binding 0
    std::string debugName;
};

class PipelineResource : public GpuResource {
public:
    TextureFormat colorFormat() const { return colorFormat_; }
    uint32_t constantsSize() const { return constantsSize_; }

protected:
    PipelineResource(TextureFormat colorFormat, std::string name, uint32_t constantsSize = 0)
        : GpuResource(ResourceKind::Pipeline, std::move(name)), colorFormat_(colorFormat), constantsSize_(constantsSize) {}
    TextureFormat colorFormat_;
    uint32_t constantsSize_ = 0;
};

}  // namespace pe::gfx
