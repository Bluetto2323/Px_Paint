// PxPaint brush engine - procedural brush textures (Block 5).
//
// Two kinds of 8-bit textures, generated on the CPU from fixed seeds (no
// asset files, identical on every device and run):
//
//   grain  512 x 512, tileable, anchored to CANVAS pixels: the paper tooth
//          that modulates dab coverage (pencil, charcoal, chalk, canvas,
//          watercolour granulation). Sampled 1:1 (nearest), so the grain has
//          the same scale on the document at every zoom.
//   tip    128 x 128 stamp, centred, zero outside the unit disc: the dab shape
//          of the non-round tips, scaled / rotated per dab (bilinear).
//
// Each texture is built once per process, on first use (or by prewarm()),
// behind a std::call_once: brush selection never regenerates anything, and
// the preview renderer can run on another thread. Total: 5 x 256 KiB grain +
// 3 x 16 KiB tips = 1.3 MiB, CPU only (the GPU never sees them: the result
// is in the raster tiles).
#pragma once

#include <cstdint>
#include <vector>

#include "brush/BrushTypes.h"

namespace pe::brush {

struct Texture8 {
    int32_t size = 0;
    uint32_t mask = 0;   // size - 1 (power of two)
    uint32_t shift = 0;  // log2(size)
    std::vector<uint8_t> px;

    bool empty() const { return px.empty(); }
    // Wrapping lookup (grain).
    uint8_t wrap(int32_t x, int32_t y) const {
        return px[((static_cast<uint32_t>(y) & mask) << shift) | (static_cast<uint32_t>(x) & mask)];
    }
    // Bilinear sample at texel coordinates (pixel centres at i + 0.5), 0 outside (tips).
    float sample(float u, float v) const;
    double mean() const;
};

// Grain texture (None -> an empty texture).
const Texture8& grainTexture(BrushGrain g);
// Tip stamp (Round -> an empty texture).
const Texture8& tipTexture(BrushTip t);
// Builds every texture now (engine start, off the drawing path).
void prewarmBrushTextures();

constexpr int32_t kGrainSize = 512;
constexpr int32_t kTipSize = 128;

}  // namespace pe::brush
