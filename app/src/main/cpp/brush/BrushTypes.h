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

// Block 5 brush model. Every field has a default that reproduces the Block 3
// round brush exactly (BrushSettings::classicRound()).
enum class BrushTip : uint8_t {
    Round = 0,     // analytic round / elliptical dab (hardness, roundness)
    Chalk = 1,     // procedural stamp: speckled disc with a broken edge
    Charcoal = 2,  // procedural stamp: irregular blob, soft dense core
    Bristle = 3,   // procedural stamp: row of bristle marks across the stroke
};
constexpr int kBrushTipCount = 4;

// Canvas-anchored paper texture that modulates dab coverage (the "tooth").
enum class BrushGrain : uint8_t {
    None = 0,
    Paper = 1,   // fine drawing paper
    Rough = 2,   // coarse, high-contrast tooth (pastel / charcoal paper)
    Canvas = 3,  // woven canvas
    Wash = 4,    // low-frequency pigment granulation (watercolour paper)
};
constexpr int kBrushGrainCount = 5;

// How the dabs of ONE stroke combine in its coverage mask.
enum class Accumulation : uint8_t {
    StrokeMax = 0,  // max: a stroke never darkens itself (Block 3)
    Buildup = 1,    // over: overlapping dabs build up (airbrush, layered graphite)
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

    // --- Brush model (Block 5).
    BrushTip tip = BrushTip::Round;
    float hardness = 1.0f;       // Round tip: 1 = 1 px antialiased edge (Block 3) .. 0 = fully soft
    float flow = 1.0f;           // coverage of one dab (0.01..1); the stroke ceiling is `opacity`
    float roundness = 1.0f;      // minor / major axis of the dab (0.05..1)
    float angle = 0.0f;          // major axis direction, radians from +x (canvas space)
    bool angleFollowsStroke = false;  // angle is relative to the stroke direction
    float sizeJitter = 0.0f;     // 0..1: dab size varies by up to +-100 %
    float scatter = 0.0f;        // 0..2: dab centre displaced by up to scatter * diameter
    float angleJitter = 0.0f;    // 0..1: dab angle varies by up to +-180 deg
    float flowJitter = 0.0f;     // 0..1: dab coverage reduced by up to 100 %
    BrushGrain grain = BrushGrain::None;
    float grainDepth = 0.0f;     // 0..1: how much of the coverage the texture can remove
    float grainPressure = 0.0f;  // 0..1: stylus pressure fills the grain (needs pressure)
    Accumulation accumulation = Accumulation::StrokeMax;
    float wetEdge = 0.0f;        // 0..1: pigment collects at the stroke edge (watercolour look)
    uint32_t seed = 0;           // jitter sequence (set per stroke from its first sample)

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
        if (static_cast<int>(tip) >= kBrushTipCount) s.tip = BrushTip::Round;
        if (static_cast<int>(grain) >= kBrushGrainCount) s.grain = BrushGrain::None;
        if (accumulation != Accumulation::Buildup) s.accumulation = Accumulation::StrokeMax;
        s.hardness = std::clamp(finite(hardness, 1.0f), 0.0f, 1.0f);
        s.flow = std::clamp(finite(flow, 1.0f), 0.01f, 1.0f);
        s.roundness = std::clamp(finite(roundness, 1.0f), 0.05f, 1.0f);
        s.angle = finite(angle, 0.0f);
        s.sizeJitter = std::clamp(finite(sizeJitter, 0.0f), 0.0f, 1.0f);
        s.scatter = std::clamp(finite(scatter, 0.0f), 0.0f, 2.0f);
        s.angleJitter = std::clamp(finite(angleJitter, 0.0f), 0.0f, 1.0f);
        s.flowJitter = std::clamp(finite(flowJitter, 0.0f), 0.0f, 1.0f);
        s.grainDepth = std::clamp(finite(grainDepth, 0.0f), 0.0f, 1.0f);
        s.grainPressure = std::clamp(finite(grainPressure, 0.0f), 0.0f, 1.0f);
        s.wetEdge = std::clamp(finite(wetEdge, 0.0f), 0.0f, 1.0f);
        return s;
    }

    // True when the Block 5 model adds nothing: the Block 3/4 rasteriser
    // (exactly the same pixels) is used.
    bool classicRound() const {
        return tip == BrushTip::Round && hardness >= 1.0f && flow >= 1.0f && roundness >= 1.0f && sizeJitter == 0.0f &&
               scatter == 0.0f && angleJitter == 0.0f && flowJitter == 0.0f &&
               (grain == BrushGrain::None || grainDepth == 0.0f) && accumulation == Accumulation::StrokeMax &&
               wetEdge == 0.0f;
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
    SampleAxes axes;    // interpolated between the samples around the dab
    float dir = kNoValue;  // stroke direction at the dab (radians, canvas), NaN = unknown
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
