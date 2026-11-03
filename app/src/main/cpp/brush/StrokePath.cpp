#include "brush/StrokePath.h"

#include <algorithm>
#include <cmath>

namespace pe::brush {

using canvas::Vec2;

namespace {
double knotStep(Vec2 a, Vec2 b) {
    // Centripetal parameterisation: sqrt of the chord length.
    return std::max(std::sqrt((b - a).length()), 1e-4);
}
Vec2 lerpK(Vec2 a, Vec2 b, double ta, double tb, double t) {
    const double s = (t - ta) / (tb - ta);
    return a * (1.0 - s) + b * s;
}
Vec2 mirror(Vec2 about, Vec2 other) { return about * 2.0 - other; }
}  // namespace

Vec2 catmullRom(Vec2 p0, Vec2 p1, Vec2 p2, Vec2 p3, double u) {
    const double t0 = 0.0;
    const double t1 = t0 + knotStep(p0, p1);
    const double t2 = t1 + knotStep(p1, p2);
    const double t3 = t2 + knotStep(p2, p3);
    const double t = t1 + (t2 - t1) * u;
    const Vec2 a1 = lerpK(p0, p1, t0, t1, t);
    const Vec2 a2 = lerpK(p1, p2, t1, t2, t);
    const Vec2 a3 = lerpK(p2, p3, t2, t3, t);
    const Vec2 b1 = lerpK(a1, a2, t0, t2, t);
    const Vec2 b2 = lerpK(a2, a3, t1, t3, t);
    return lerpK(b1, b2, t1, t2, t);
}

void StrokePath::reset() {
    active_ = false;
    count_ = 0;
    head_ = 0;
    walker_ = {};
    committed_.clear();
}

void StrokePath::begin(const StrokePoint& first, double spacingPx, const BrushDynamics* dynamics) {
    reset();
    active_ = true;
    spacing_ = std::max(spacingPx, BrushSettings::kMinSpacingPx);
    dyn_ = dynamics;
    const Vec2 p{first.x, first.y};
    pts_[0] = p;
    ax_[0] = first.axes;
    head_ = 1;
    count_ = 1;
    walker_.pos = p;
    walker_.axes = first.axes;
    walker_.carry = 0.0;
    walker_.lastDab = p;
    walker_.lastAxes = first.axes;
    // A dab oriented by the stroke waits for a direction (a tap still gets
    // its dot in finish()); every other brush starts with a dab right away.
    firstPending_ = dyn_ && dyn_->followsStroke();
    if (!firstPending_) committed_.push_back({p.x, p.y, first.axes});
}

bool StrokePath::add(const StrokePoint& s) {
    if (!active_) return false;
    const Vec2 p{s.x, s.y};
    if (!std::isfinite(p.x) || !std::isfinite(p.y)) return false;
    if ((p - pt(0)).length() < kMinStep) {
        // Same position: no geometry, but the latest axes (pressure going up
        // under a resting stylus) apply to the point.
        ax_[(head_ + 3) % 4] = s.axes;
        if (count_ == 1) walker_.axes = s.axes;
        return false;
    }
    pts_[head_] = p;
    ax_[head_] = s.axes;
    head_ = (head_ + 1) % 4;
    count_++;
    if (count_ >= 3) {
        // Segment pt(2) -> pt(1) is final now that pt(0) gives its end tangent.
        const Vec2 p1 = pt(2), p2 = pt(1), p3 = pt(0);
        const Vec2 p0 = count_ >= 4 ? pt(3) : mirror(p1, p2);
        walkSegment(p0, p1, p2, p3, ax(1), walker_, committed_);
    }
    return true;
}

void StrokePath::finish() {
    if (!active_) return;
    if (count_ >= 2) {
        const Vec2 p1 = pt(1), p2 = pt(0);
        const Vec2 p0 = count_ >= 3 ? pt(2) : mirror(p1, p2);
        walkSegment(p0, p1, p2, mirror(p2, p1), ax(0), walker_, committed_);
        // End exactly on the last sample.
        if ((walker_.lastDab - p2).length() > 1e-6) {
            committed_.push_back({p2.x, p2.y, ax(0), walker_.dir});
            walker_.lastDab = p2;
            walker_.lastAxes = ax(0);
            walker_.carry = 0.0;
        }
    } else if (firstPending_) {
        committed_.push_back({pt(0).x, pt(0).y, ax(0)});
    } else if (dyn_ && dyn_->usesPressure()) {
        // A tap: the dot takes the pressure reached before lifting.
        if (!committed_.empty()) committed_.back().axes = ax(0);
        else committed_.push_back({pt(0).x, pt(0).y, ax(0)});
    }
    active_ = false;
}

void StrokePath::takeCommitted(std::vector<Dab>& out) {
    out.insert(out.end(), committed_.begin(), committed_.end());
    committed_.clear();
}

void StrokePath::provisional(std::vector<Dab>& out) const {
    if (!active_ || count_ < 2) return;
    Walker w = walker_;
    const Vec2 p1 = pt(1), p2 = pt(0);
    const Vec2 p0 = count_ >= 3 ? pt(2) : mirror(p1, p2);
    walkSegment(p0, p1, p2, mirror(p2, p1), ax(0), w, out);
    if ((w.lastDab - p2).length() > 1e-6) out.push_back({p2.x, p2.y, ax(0), w.dir});  // tip under the finger
}

void StrokePath::walkSegment(Vec2 p0, Vec2 p1, Vec2 p2, Vec2 p3, const SampleAxes& a2, Walker& w,
                             std::vector<Dab>& out) const {
    const double chord = (p2 - p1).length();
    if (chord <= 0.0) return;
    // Flatten into pieces of <= ~1 canvas px (curve length <= ~1.6 x chord
    // for centripetal Catmull-Rom); points on the pieces lie on the curve.
    // Axes are interpolated linearly from the segment's start to its end.
    const SampleAxes a1 = w.axes;
    const int pieces = static_cast<int>(std::clamp(std::ceil(chord * 1.6), 1.0, 65536.0));
    for (int i = 1; i <= pieces; ++i) {
        const double u = static_cast<double>(i) / pieces;
        const Vec2 q = i == pieces ? p2 : catmullRom(p0, p1, p2, p3, u);
        walkLine(q, i == pieces ? a2 : lerpAxes(a1, a2, u), w, out);
    }
}

void StrokePath::walkLine(Vec2 to, const SampleAxes& toAxes, Walker& w, std::vector<Dab>& out) const {
    const Vec2 d = to - w.pos;
    const double len = d.length();
    if (len <= 0.0) {
        w.axes = toAxes;
        return;
    }
    w.dir = static_cast<float>(std::atan2(d.y, d.x));
    double s = spacingAt(w.lastAxes) - w.carry;  // distance along this piece to the next dab
    double last = -w.carry;                       // position of the last dab on this piece
    while (s <= len) {
        const double f = s / len;
        const Vec2 q = w.pos + d * f;
        const SampleAxes a = lerpAxes(w.axes, toAxes, f);
        out.push_back({q.x, q.y, a, w.dir});
        w.lastDab = q;
        w.lastAxes = a;
        last = s;
        s += spacingAt(a);
    }
    w.carry = len - last;
    w.pos = to;
    w.axes = toAxes;
}

}  // namespace pe::brush
