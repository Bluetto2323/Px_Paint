#include "brush/BrushDynamics.h"

#include <algorithm>

namespace pe::brush {

namespace {
constexpr double kPi = 3.14159265358979323846;
constexpr double kDefaultDtS = 1.0 / 240.0;  // used when timestamps do not advance
}  // namespace

void PressureFilter::reset(float smoothing) {
    bypass_ = !(smoothing > 0.0f);
    const double s = std::clamp(static_cast<double>(smoothing), 0.0, 1.0);
    // Log interpolation of the resting cutoff: 30 Hz (barely any smoothing) .. 1.5 Hz.
    minCutoff_ = kMaxMinCutoffHz * std::pow(kMinMinCutoffHz / kMaxMinCutoffHz, s);
    primed_ = false;
    x_ = dx_ = lastT_ = 0.0;
}

double PressureFilter::alpha(double cutoffHz, double dtS) {
    const double tau = 1.0 / (2.0 * kPi * cutoffHz);
    return 1.0 / (1.0 + tau / dtS);
}

float PressureFilter::filter(float p, double tMs) {
    if (!std::isfinite(p)) return p;
    if (bypass_) return p;
    if (!primed_) {
        primed_ = true;
        x_ = p;
        dx_ = 0.0;
        lastT_ = tMs;
        return p;
    }
    double dt = (tMs - lastT_) / 1000.0;
    if (!(dt > 0.0)) dt = kDefaultDtS;
    lastT_ = tMs;
    const double rawDx = (p - x_) / dt;
    dx_ += alpha(kDerivativeCutoffHz, dt) * (rawDx - dx_);
    const double cutoff = minCutoff_ + kBeta * std::abs(dx_);
    x_ += alpha(cutoff, dt) * (p - x_);
    return static_cast<float>(std::clamp(x_, 0.0, 1.0));
}

void BrushDynamics::configure(const BrushSettings& settings, bool hasPressure, bool hasTilt) {
    settings_ = settings;
    hasPressure_ = hasPressure;
    hasTilt_ = hasTilt;
}

double BrushDynamics::response(const SampleAxes& a) const {
    if (!hasPressure_ || !std::isfinite(a.pressure)) return 1.0;
    const float p = std::clamp(a.pressure, 0.0f, 1.0f);
    return settings_.pressureCurve ? settings_.pressureCurve->apply(p) : p;
}

double BrushDynamics::diameter(const SampleAxes& a) const {
    const double d = settings_.diameter;
    if (!hasPressure_ || !settings_.pressureSize) return d;
    const double mn = settings_.minSizeFraction;
    return std::max<double>(BrushSettings::kMinDiameter, d * (mn + (1.0 - mn) * response(a)));
}

DabGeometry BrushDynamics::geometry(const SampleAxes& a) const {
    DabGeometry g;
    const double d = diameter(a);
    if (d >= 1.0) {
        g.radius = d * 0.5;
        g.scale = 1.0;
    } else {
        // Sub-pixel: keep a 1 px footprint, lower the coverage (hairlines stay continuous).
        g.radius = 0.5;
        g.scale = std::max(d, 0.0);
    }
    g.major = g.radius;
    if (hasPressure_ && settings_.pressureOpacity) {
        const double mn = settings_.minOpacityFraction;
        g.flow = mn + (1.0 - mn) * response(a);
    }
    if (usesTilt() && std::isfinite(a.tilt) && std::isfinite(a.orientation)) {
        const double tilt = std::clamp(static_cast<double>(a.tilt), 0.0, kPi / 2);
        const double stretch = 1.0 + 1.5 * settings_.tiltStrength * std::sin(tilt);
        if (stretch > 1.0005 && g.radius >= 1.0) {
            g.elliptical = true;
            g.major = g.radius * stretch;
            // Orientation 0 points to -y; the major axis lies along the stylus.
            g.angle = static_cast<double>(a.orientation) - kPi / 2;
        }
    }
    return g;
}

}  // namespace pe::brush
