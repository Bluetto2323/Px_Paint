#pragma once

#include "graphics/abstraction/GraphicsTypes.h"

namespace pe::gfx {

// Something a render pass can draw into. In Block 1 the only render target is
// the swapchain backbuffer (owned by the device, valid for one frame);
// offscreen targets built on TextureResource will implement the same
// interface later without changing the RenderingContext API.
class RenderTarget {
public:
    virtual ~RenderTarget() = default;
    virtual Extent2D extent() const = 0;
    virtual TextureFormat colorFormat() const = 0;
    virtual bool isBackbuffer() const = 0;
};

}  // namespace pe::gfx
