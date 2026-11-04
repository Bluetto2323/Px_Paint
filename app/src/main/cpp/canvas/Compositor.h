// PxPaint - layer compositor (Block 6).
//
// Composites the visible layers of a document, bottom to top, into the
// composite raster that the GPU displays (CanvasSurface uploads its dirty
// regions exactly as it uploaded the single raster before). Compositing is
// done on the CPU, in one place, so Vulkan and OpenGL ES show bit-identical
// results and no GPU readback is ever needed.
//
// Colour space: layers store 8-bit sRGB-encoded premultiplied RGBA (as the
// brush engine writes them). Blending is done on those sRGB values, as in
// common painting applications (Photoshop's and Procreate's defaults), in
// 32-bit float with a single rounding per output pixel.
//
// Model: W3C Compositing and Blending Level 1. A layer pixel with
// premultiplied colour cs and alpha as (both already multiplied by the
// layer opacity) over a backdrop (cb, ab):
//
//     co = cs * (1 - ab) + cb * (1 - as) + as * ab * B(Cb, Cs)
//     ao = as + ab * (1 - as)
//
// where Cs = cs / as and Cb = cb / ab are the un-premultiplied colours and
// B is the blend function of the mode (Normal: B = Cs, which reduces to
// source-over). Where the backdrop is transparent the layer shows as is,
// where the layer is transparent the backdrop is unchanged: transparent
// pixels never darken or tint anything.
//
// The backdrop of the bottom layer is the paper colour when the canvas has
// an opaque background (so Multiply etc. blend with the paper exactly as
// shown), transparent otherwise.
#pragma once

#include <cstddef>
#include <cstdint>

#include "canvas/RasterStore.h"

namespace pe::canvas {

enum class BlendMode : uint8_t {
    Normal = 0,
    Multiply = 1,
    Screen = 2,
    Overlay = 3,
    Darken = 4,
    Lighten = 5,
    Add = 6,
    Difference = 7,
};
constexpr int kBlendModeCount = 8;
const char* blendModeName(BlendMode m);

struct LayerView {
    const RasterStore* store = nullptr;
    float opacity = 1.0f;
    BlendMode mode = BlendMode::Normal;
    // Non-destructive transform preview (Block 12): where previewTiles[tile] is
    // set, the tile is read from `preview` (absent there = transparent) instead
    // of `store`, which stays untouched until the transform is confirmed.
    const RasterStore* preview = nullptr;
    const uint8_t* previewTiles = nullptr;
};

struct Backdrop {
    bool opaque = false;
    float r = 1.0f, g = 1.0f, b = 1.0f;  // paper colour 0..1 (used when opaque)
    static Backdrop fromArgb(uint32_t argb, bool transparent);
};

// Blend function B(Cb, Cs) of a separable mode on un-premultiplied values.
float blendChannel(BlendMode m, float cb, float cs);
// One pixel: `dst` = premultiplied backdrop RGBA (0..1), `src` = premultiplied
// layer RGBA (0..1, opacity not applied), result in `dst`.
void blendPixel(float* dst, const float* src, float opacity, BlendMode m);

class Compositor {
public:
    // Recomposites `rect` (canvas px) of `layers` (bottom first, visible only)
    // over `backdrop` into `out`, then marks it changed (GPU upload). Tiles
    // where no layer has content are left unallocated / released: the
    // renderer's paper shows there, which is exactly the backdrop.
    static void composite(const LayerView* layers, size_t count, const RectI& rect, const Backdrop& backdrop,
                          RasterStore& out);
    // Composites `layers` over a transparent backdrop into `out` (merge /
    // flatten: the result is new layer content). Every pixel of `rect`.
    static void flattenInto(const LayerView* layers, size_t count, const RectI& rect, RasterStore& out);

private:
    static void compositeTile(const LayerView* layers, size_t count, const RectI& part, const Backdrop& backdrop,
                              RasterStore& out, bool keepEmpty);
};

}  // namespace pe::canvas
