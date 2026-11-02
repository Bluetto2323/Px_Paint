#pragma once

#include "graphics/abstraction/GpuResource.h"
#include "graphics/abstraction/GraphicsTypes.h"

namespace pe::gfx {

struct BufferDesc {
    uint64_t size = 0;
    uint32_t usage = 0;  // BufferUsage bits
    MemoryUsage memory = MemoryUsage::GpuOnly;
    std::string debugName;
};

class BufferResource : public GpuResource {
public:
    const BufferDesc& desc() const { return desc_; }

    // Updates a CpuToGpu buffer. Returns false for GpuOnly buffers (those are
    // written once at creation in this block).
    virtual bool update(uint64_t offset, const void* data, uint64_t size) = 0;

protected:
    explicit BufferResource(const BufferDesc& desc)
        : GpuResource(ResourceKind::Buffer, desc.debugName), desc_(desc) {}
    BufferDesc desc_;
};

}  // namespace pe::gfx
