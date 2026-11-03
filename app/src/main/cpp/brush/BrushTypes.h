// PxPaint brush engine - shared types.
//
// The brush engine is independent of the UI and of the GPU: it turns stroke
// samples (canvas coordinates) into antialiased circular dabs composited into
// the CPU raster store (canvas/RasterStore). The GPU only displays the store.
//
// Units: canvas pixels. A brush therefore has the same size on the document
// whatever the zoom or rotation of the view (the UI converts screen samples
// to canvas coordinates through the viewport before they get here).
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace pe::brush {

struct BrushSettings {
    float diameter = 24.0f;     // canvas px
    float opacity = 1.0f;       // stroke opacity [0, 1] (overlapping dabs never exceed it)
    uint32_t argb = 0xFF000000; // straight sRGB colour (alpha multiplies the opacity)
    float spacing = 0.08f;      // dab distance as a fraction of the diameter
    bool erase = false;         // removes paint instead of adding colour

    static constexpr float kMinDiameter = 0.5f;
    static constexpr float kMaxDiameter = 2000.0f;
    static constexpr float kMinSpacing = 0.01f;
    static constexpr float kMaxSpacing = 2.0f;
    // Below this, dabs would be denser than useful for antialiasing.
    static constexpr double kMinSpacingPx = 0.1;

    BrushSettings sanitized() const {
        BrushSettings s = *this;
        auto finite = [](float v, float fallback) { return std::isfinite(v) ? v : fallback; };
        s.diameter = std::clamp(finite(diameter, 24.0f), kMinDiameter, kMaxDiameter);
        s.opacity = std::clamp(finite(opacity, 1.0f), 0.0f, 1.0f);
        s.spacing = std::clamp(finite(spacing, 0.08f), kMinSpacing, kMaxSpacing);
        return s;
    }
    // Spacing at or below this fraction means "a continuous stroke": the
    // engine then also keeps the edge ripple between dabs (sagitta s^2/8r)
    // under kMaxEdgeRipplePx, so big brushes have perfectly straight edges.
    // Above it the user asked for visible dabs and gets exactly that spacing.
    static constexpr float kContinuousSpacing = 0.25f;
    static constexpr double kMaxEdgeRipplePx = 0.1;

    // Distance between dab centres along the stroke, canvas px.
    double spacingPx() const {
        double s = static_cast<double>(spacing) * diameter;
        if (spacing <= kContinuousSpacing) {
            const double r = diameter * 0.5;
            s = std::min(s, std::sqrt(8.0 * r * kMaxEdgeRipplePx));
        }
        return std::max(kMinSpacingPx, s);
    }
};

// One input sample, canvas coordinates. `t` (ms) is kept for later blocks
// (velocity, pressure); Block 3 does not use it.
struct StrokePoint {
    double x = 0.0, y = 0.0;
    double t = 0.0;
};

struct Dab {
    double x = 0.0, y = 0.0;
};

}  // namespace pe::brush
