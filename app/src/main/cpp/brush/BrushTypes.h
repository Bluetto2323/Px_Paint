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
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>

namespace pe::brush {

constexpr float kNoValue = std::numeric_limits<float>::quiet_NaN();

// Pressure response (sensitivity + curve) sampled by the app into a table:
// lut[i] = response(i / (kSize - 1)), monotonic, lut[0] = 0, lut[last] = 1.
// Linear when absent.
struct PressureLut {
    static constexpr size_t kSize = 257;
    std::array<float, kSize> lut{};
    float apply(float p) const {
        const float x = std::clamp(p, 0.0f, 1.0f) * static_cast<float>(kSize - 1);
        const size_t i = std::min(static_cast<size_t>(x), kSize - 2);
        const float f = x - static_cast<float>(i);
        return lut[i] + (lut[i + 1] - lut[i]) * f;
    }
    // Monotonic, finite, from 0 to 1 (anything else is rejected -> linear).
    bool valid() const {
        if (!(std::abs(lut[0]) < 1e-4f) || !(std::abs(lut[kSize - 1] - 1.0f) < 1e-4f)) return false;
        for (size_t i = 1; i < kSize; ++i)
            if (!std::isfinite(lut[i]) || lut[i] < lut[i - 1] - 1e-6f) return false;
        return true;
    }
};

struct BrushSettings {
    float diameter = 24.0f;     // canvas px
    float opacity = 1.0f;       // stroke opacity [0, 1] (overlapping dabs never exceed it)
    uint32_t argb = 0xFF000000; // straight sRGB colour (alpha multiplies the opacity)
    float spacing = 0.08f;      // dab distance as a fraction of the diameter
    bool erase = false;         // removes paint instead of adding colour

    // --- Dynamics (Block 4). They act only on strokes whose samples carry
    // the axis (stylus pressure / tilt); without it a stroke is drawn exactly
    // as in Block 3: full size, full opacity, round dabs.
    bool pressureSize = false;
    float minSizeFraction = 0.2f;     // diameter at zero pressure, fraction of `diameter`
    bool pressureOpacity = false;
    float minOpacityFraction = 0.2f;  // dab opacity at zero pressure, fraction of `opacity`
    bool tiltShape = false;           // tilt + orientation stretch the dab into an ellipse
    float tiltStrength = 1.0f;        // 0..1: major axis up to 1 + 1.5 * strength at full tilt
    float pressureSmoothing = 0.35f;  // 0 = raw .. 1 = strongest (adaptive, see PressureFilter)
    std::shared_ptr<const PressureLut> pressureCurve;  // null = linear

    // Floors that keep a stroke visible whatever the pressure.
    static constexpr float kMinSizeFraction = 0.02f;
    static constexpr float kMinOpacityFraction = 0.05f;

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
        s.minSizeFraction = std::clamp(finite(minSizeFraction, 0.2f), kMinSizeFraction, 1.0f);
        s.minOpacityFraction = std::clamp(finite(minOpacityFraction, 0.2f), kMinOpacityFraction, 1.0f);
        s.tiltStrength = std::clamp(finite(tiltStrength, 1.0f), 0.0f, 1.0f);
        s.pressureSmoothing = std::clamp(finite(pressureSmoothing, 0.35f), 0.0f, 1.0f);
        if (s.pressureCurve && !s.pressureCurve->valid()) s.pressureCurve.reset();
        return s;
    }
    // Spacing at or below this fraction means "a continuous stroke": the
    // engine then also keeps the edge ripple between dabs (sagitta s^2/8r)
    // under kMaxEdgeRipplePx, so big brushes have perfectly straight edges.
    // Above it the user asked for visible dabs and gets exactly that spacing.
    static constexpr float kContinuousSpacing = 0.25f;
    static constexpr double kMaxEdgeRipplePx = 0.1;

    // Distance between dab centres along the stroke, canvas px, for a dab
    // of diameter `d` (the brush diameter unless pressure changes it).
    double spacingPx(double d) const {
        double s = static_cast<double>(spacing) * d;
        if (spacing <= kContinuousSpacing) {
            const double r = d * 0.5;
            s = std::min(s, std::sqrt(8.0 * r * kMaxEdgeRipplePx));
        }
        return std::max(kMinSpacingPx, s);
    }
    double spacingPx() const { return spacingPx(diameter); }
};

// Per-sample stylus axes. NaN = the device does not report the axis (never
// replaced by an invented value).
//   pressure    : normalised 0..1 (the app maps the device's range)
//   tilt        : radians, 0 = perpendicular to the screen, pi/2 = flat
//   orientation : radians, canvas space, 0 = pointing to -y (up), clockwise
struct SampleAxes {
    float pressure = kNoValue;
    float tilt = kNoValue;
    float orientation = kNoValue;
};

// One input sample, canvas coordinates; `t` in ms.
struct StrokePoint {
    double x = 0.0, y = 0.0;
    double t = 0.0;
    SampleAxes axes;
};

struct Dab {
    double x = 0.0, y = 0.0;
    SampleAxes axes;  // interpolated between the samples around the dab
};

// Linear interpolation of the axes (orientation along the shorter arc).
inline SampleAxes lerpAxes(const SampleAxes& a, const SampleAxes& b, double f) {
    auto lerp = [f](float x, float y) {
        if (!std::isfinite(x)) return y;
        if (!std::isfinite(y)) return x;
        return static_cast<float>(x + (y - x) * f);
    };
    SampleAxes r;
    r.pressure = lerp(a.pressure, b.pressure);
    r.tilt = lerp(a.tilt, b.tilt);
    if (std::isfinite(a.orientation) && std::isfinite(b.orientation)) {
        constexpr double kTwoPi = 6.283185307179586;
        double d = std::fmod(static_cast<double>(b.orientation) - a.orientation, kTwoPi);
        if (d > kTwoPi / 2) d -= kTwoPi;
        if (d < -kTwoPi / 2) d += kTwoPi;
        r.orientation = static_cast<float>(a.orientation + d * f);
    } else {
        r.orientation = lerp(a.orientation, b.orientation);
    }
    return r;
}

}  // namespace pe::brush
