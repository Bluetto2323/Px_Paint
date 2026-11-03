// PxPaint brush engine - stroke path: interpolation + dab placement.
//
// Input samples are joined by a centripetal Catmull-Rom spline (it passes
// through EVERY sample, has no cusps or overshoot on uneven sampling), and
// dabs are placed along it at a constant arc-length distance (`spacing`),
// carrying the remainder from one segment to the next. The dab rhythm is
// therefore independent of how fast the finger moves or how often samples
// arrive: slow and fast strokes produce the same line.
//
// Committed vs provisional
//   The spline segment between samples i-1 and i needs sample i+1 for its
//   end tangent, so it is final ("committed") only once the next sample is
//   known. The last segment is still drawn immediately as a provisional
//   tail (same curve with a mirrored end tangent) which the caller redraws
//   every update. finish() commits the tail exactly as it was shown and puts
//   a final dab on the last sample, so the stroke ends where the finger
//   lifted. There is no lag between the finger and the drawn line.
#pragma once

#include <cstddef>
#include <vector>

#include "brush/BrushDynamics.h"
#include "brush/BrushTypes.h"
#include "canvas/CanvasMath.h"

namespace pe::brush {

class StrokePath {
public:
    // Starts a stroke; the first dab is placed on `first` immediately (a tap
    // makes a round dot). With `dynamics`, the distance to the next dab is
    // the spacing of the current dab's (pressure-dependent) size; it must
    // outlive the stroke.
    void begin(const StrokePoint& first, double spacingPx, const BrushDynamics* dynamics = nullptr);
    // Adds a sample. Samples closer than kMinStep to the previous one are
    // merged (a resting finger) - they carry no geometry. Returns true if it
    // was accepted as a new path point.
    bool add(const StrokePoint& p);
    // Commits the remaining path, ending with a dab on the last sample.
    void finish();
    void reset();

    bool active() const { return active_; }
    size_t pointCount() const { return count_; }

    // Committed dabs produced since the last call (moved out).
    void takeCommitted(std::vector<Dab>& out);
    // Dabs of the provisional tail (recomputed from the current state).
    void provisional(std::vector<Dab>& out) const;

    static constexpr double kMinStep = 1e-3;  // canvas px

private:
    struct Walker {
        canvas::Vec2 pos;      // end of the committed polyline
        SampleAxes axes;       // axes at `pos`
        double carry = 0.0;    // distance travelled since the last dab
        canvas::Vec2 lastDab;
        SampleAxes lastAxes;
    };
    // Appends dabs along the spline segment p1->p2 (p0, p3: neighbours);
    // the axes go linearly from the walker's to `a2`.
    void walkSegment(canvas::Vec2 p0, canvas::Vec2 p1, canvas::Vec2 p2, canvas::Vec2 p3, const SampleAxes& a2,
                     Walker& w, std::vector<Dab>& out) const;
    void walkLine(canvas::Vec2 to, const SampleAxes& toAxes, Walker& w, std::vector<Dab>& out) const;
    double spacingAt(const SampleAxes& a) const {
        return dyn_ ? std::max(BrushSettings::kMinSpacingPx, dyn_->spacingPx(a)) : spacing_;
    }
    canvas::Vec2 pt(size_t fromEnd) const { return pts_[(head_ + 4 - 1 - fromEnd) % 4]; }  // 0 = newest
    const SampleAxes& ax(size_t fromEnd) const { return ax_[(head_ + 4 - 1 - fromEnd) % 4]; }

    bool active_ = false;
    double spacing_ = 1.0;
    canvas::Vec2 pts_[4];  // ring of the last 4 accepted points
    SampleAxes ax_[4];     // their axes
    const BrushDynamics* dyn_ = nullptr;
    size_t head_ = 0;      // next write slot
    size_t count_ = 0;     // accepted points so far
    Walker walker_;
    std::vector<Dab> committed_;
};

// Centripetal Catmull-Rom point on segment p1->p2 at u in [0,1] (exposed for tests).
canvas::Vec2 catmullRom(canvas::Vec2 p0, canvas::Vec2 p1, canvas::Vec2 p2, canvas::Vec2 p3, double u);

}  // namespace pe::brush
