// PxPaint brush engine - pressure and tilt dynamics (Block 4).
//
// PressureFilter: adaptive low-pass ("1-euro" filter, Casiez et al. 2012) on
// the normalised pressure of one stroke. Its cutoff rises with the speed of
// the pressure change, so slow jitter is smoothed while a fast press or
// release goes through within a sample or two. It runs on sample time (the
// MotionEvent timestamps), not on frame time, so batching does not change
// the result. The first sample is taken as is (no ramp-in at the stroke start).
//
// BrushDynamics: per-dab size, opacity ("flow") and shape from the
// interpolated axes of the dab:
//   c        = response(pressure)            (app curve + sensitivity, LUT)
//   diameter = D * (minSize + (1 - minSize) * c)      if pressure -> size
//   flow     = minOpacity + (1 - minOpacity) * c      if pressure -> opacity
//   ellipse  : major = r * (1 + 1.5 * strength * sin(tilt)), along the
//              stylus orientation; minor = r          if tilt shapes the dab
// minSize / minOpacity have hard floors (2 % / 5 %): no pressure value can
// make a stroke vanish. A stroke whose samples carry no pressure (finger,
// mouse, passive stylus) uses c = 1: exactly the Block 3 brush. Tilt needs
// both tilt and orientation; without them the dab stays round (nothing is
// simulated).
#pragma once

#include <algorithm>
#include <cmath>

#include "brush/BrushTypes.h"

namespace pe::brush {

class PressureFilter {
public:
    // smoothing 0 = pass-through, 1 = strongest.
    void reset(float smoothing);
    // `p` normalised 0..1 (NaN passes through), `tMs` sample time.
    float filter(float p, double tMs);

    static constexpr double kMaxMinCutoffHz = 30.0;   // smoothing 0+
    static constexpr double kMinMinCutoffHz = 1.5;    // smoothing 1
    static constexpr double kBeta = 1.0;              // cutoff gain per (pressure/s)
    static constexpr double kDerivativeCutoffHz = 10.0;

private:
    static double alpha(double cutoffHz, double dtS);
    bool bypass_ = true;
    bool primed_ = false;
    double minCutoff_ = kMaxMinCutoffHz;
    double x_ = 0.0, dx_ = 0.0, lastT_ = 0.0;
};

struct DabGeometry {
    double radius = 0.5;     // circular radius / ellipse semi-minor axis (>= 0.5)
    double major = 0.5;      // ellipse semi-major axis (== radius when round)
    double angle = 0.0;      // major axis direction (radians from +x, canvas)
    double scale = 1.0;      // coverage multiplier for sub-pixel brushes
    double flow = 1.0;       // pressure opacity (0..1]
    bool elliptical = false;
    double reach() const { return major; }
};

class BrushDynamics {
public:
    // `hasPressure` / `hasTilt`: what the stroke's first sample carries.
    void configure(const BrushSettings& settings, bool hasPressure, bool hasTilt);
    // Pressure response c in [0, 1] (1 when the stroke has no pressure).
    double response(const SampleAxes& a) const;
    double diameter(const SampleAxes& a) const;
    // Dab distance: from the smaller dab dimension (a flat nib needs the
    // spacing of its thin side; roundness 1 = the Block 3/4 spacing).
    double spacingPx(const SampleAxes& a) const {
        const double d = diameter(a);
        return settings_.spacingPx(settings_.roundness < 1.0f ? d * std::max(0.2f, settings_.roundness) : d);
    }
    DabGeometry geometry(const SampleAxes& a) const;
    bool usesPressure() const { return hasPressure_ && (settings_.pressureSize || settings_.pressureOpacity); }
    bool hasPressure() const { return hasPressure_; }
    // The dab is oriented by the stroke direction (unknown at the first sample).
    bool followsStroke() const {
        return settings_.angleFollowsStroke && (settings_.tip != BrushTip::Round || settings_.roundness < 1.0f);
    }
    bool usesTilt() const { return hasTilt_ && settings_.tiltShape && settings_.tiltStrength > 0.0f; }

private:
    BrushSettings settings_;
    bool hasPressure_ = false;
    bool hasTilt_ = false;
};

}  // namespace pe::brush
