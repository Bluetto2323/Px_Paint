// PxPaint brush engine - offscreen brush previews (Block 5).
//
// Renders sample strokes with the REAL stroke pipeline (StrokePath +
// BrushDynamics + StrokeCompositor, the same code that paints the canvas)
// into a private, CPU-only raster store, then flattens it over a background
// colour. Used by the brush library thumbnails and the brush editor. No GPU,
// no engine instance: safe on any thread (textures are built once, thread-
// safely, see BrushTextures).
#pragma once

#include <cstddef>
#include <cstdint>

#include "brush/BrushTypes.h"

namespace pe::brush {

struct PreviewStroke {
    const StrokePoint* points = nullptr;  // preview pixel coordinates, t in ms
    size_t count = 0;
    float diameter = 0.0f;  // > 0 overrides the brush diameter
    float opacity = -1.0f;  // >= 0 overrides the brush opacity
};

// `out`: width * height straight ARGB (opaque: the strokes over `backgroundArgb`).
bool renderBrushPreview(const BrushSettings& brush, const PreviewStroke* strokes, size_t strokeCount, int32_t width,
                        int32_t height, uint32_t backgroundArgb, uint32_t* out);

constexpr int32_t kMaxPreviewSide = 2048;

}  // namespace pe::brush
