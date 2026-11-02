// Paint Engine - base class of every GPU resource.
//
// Resources are handed out as Ref<T> (shared_ptr). When the last reference
// goes away the object is NOT destroyed immediately: it is retired into the
// ResourceManager, which destroys it only after the GPU finished every frame
// that could still use it. Concrete destructors perform the API deletion.
#pragma once

#include <cstdint>
#include <string>

namespace pe::gfx {

enum class ResourceKind : uint8_t { Buffer, Texture, Shader, Pipeline, RenderTarget, Count };
const char* toString(ResourceKind kind);

class GpuResource {
public:
    virtual ~GpuResource() = default;
    GpuResource(const GpuResource&) = delete;
    GpuResource& operator=(const GpuResource&) = delete;

    ResourceKind kind() const { return kind_; }
    const std::string& debugName() const { return debugName_; }
    // Bytes of GPU memory owned by this object (0 if not tracked / not owned).
    uint64_t gpuMemoryBytes() const { return gpuMemoryBytes_; }

protected:
    GpuResource(ResourceKind kind, std::string debugName)
        : kind_(kind), debugName_(std::move(debugName)) {}
    void setGpuMemoryBytes(uint64_t bytes) { gpuMemoryBytes_ = bytes; }

private:
    ResourceKind kind_;
    std::string debugName_;
    uint64_t gpuMemoryBytes_ = 0;
};

}  // namespace pe::gfx
