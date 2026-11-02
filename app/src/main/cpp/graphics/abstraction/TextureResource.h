#pragma once

#include "graphics/abstraction/GpuResource.h"
#include "graphics/abstraction/GraphicsTypes.h"

namespace pe::gfx {

struct TextureDesc {
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t mipLevels = 1;
    TextureFormat format = TextureFormat::RGBA8_UNORM;
    uint32_t usage = TextureUsage::Sampled;  // TextureUsage bits
    bool linearFilter = true;                // sampler filter (clamp-to-edge addressing)
    std::string debugName;
};
// Sampled textures start fully transparent (all channels 0), on every backend.

// Block 1 scope: allocation + view only. Uploads, layout transitions and
// sampling are added together with the systems that need them.
class TextureResource : public GpuResource {
public:
    const TextureDesc& desc() const { return desc_; }

protected:
    explicit TextureResource(const TextureDesc& desc)
        : GpuResource(ResourceKind::Texture, desc.debugName), desc_(desc) {}
    TextureDesc desc_;
};

}  // namespace pe::gfx
