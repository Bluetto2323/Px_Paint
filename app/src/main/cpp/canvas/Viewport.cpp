#include "canvas/Viewport.h"

#include <cmath>
#include <limits>

namespace pe::canvas {

void Viewport::setCanvasSize(uint32_t width, uint32_t height) {
    canvasW_ = width;
    canvasH_ = height;
    if (valid()) fitToScreen();
    touch();
}

void Viewport::setScreenSize(uint32_t width, uint32_t height) {
    if (width == screenW_ && height == screenH_) return;
    const bool hadScreen = screenW_ > 0 && screenH_ > 0;
    const Vec2 oldCenter{screenW_ * 0.5, screenH_ * 0.5};
    const Vec2 anchor = hadScreen && canvasW_ ? toCanvas(oldCenter) : Vec2{canvasW_ * 0.5, canvasH_ * 0.5};
    screenW_ = width;
    screenH_ = height;
    if (!valid()) {
        touch();
        return;
    }
    if (!hadScreen || state_.fitted) {
        fitToScreen();
        return;
    }
    // Keep the canvas point that was in the middle of the screen in the middle.
    const Vec2 newCenter{screenW_ * 0.5, screenH_ * 0.5};
    state_.center = state_.center + (newCenter - toScreen(anchor));
    state_.zoom = clampZoom(state_.zoom);
    touch();
}

double Viewport::fitZoom(double marginPx) const {
    const double cs = std::abs(std::cos(state_.rotation)), sn = std::abs(std::sin(state_.rotation));
    const double bw = canvasW_ * cs + canvasH_ * sn;  // rotated bounding box
    const double bh = canvasW_ * sn + canvasH_ * cs;
    const double aw = std::max(1.0, screenW_ - 2.0 * marginPx);
    const double ah = std::max(1.0, screenH_ - 2.0 * marginPx);
    return std::min(aw / bw, ah / bh);
}

double Viewport::minZoom() const {
    if (!valid()) return kMinZoomFloor;
    return std::max(kMinZoomFloor, std::min(1.0, fitZoom(0.0) * 0.5));
}

double Viewport::clampZoom(double z) const { return std::clamp(z, minZoom(), kMaxZoom); }

Affine2 Viewport::canvasToScreen() const {
    // T(center) * R * S * T(-canvasCenter)
    // Note: -(0.5 * w), never -w * 0.5 on an unsigned (that negates the integer).
    const Affine2 toOrigin = Affine2::translate(-0.5 * canvasW_, -0.5 * canvasH_);
    const Affine2 rs = Affine2::compose(Affine2::rotate(state_.rotation), Affine2::scale(state_.zoom));
    return Affine2::compose(Affine2::translate(state_.center.x, state_.center.y), Affine2::compose(rs, toOrigin));
}

Affine2 Viewport::canvasToNdc() const {
    // screen (px, y down) -> NDC (-1..1, y up)
    const Affine2 screenToNdc{2.0 / screenW_, 0.0, 0.0, -2.0 / screenH_, -1.0, 1.0};
    return Affine2::compose(screenToNdc, canvasToScreen());
}

void Viewport::fitToScreen(double marginPx) {
    if (!valid()) return;
    state_.zoom = std::clamp(fitZoom(marginPx), kMinZoomFloor, kMaxZoom);
    state_.center = {screenW_ * 0.5, screenH_ * 0.5};
    state_.fitted = true;
    touch();
}

void Viewport::centerCanvas() {
    state_.center = {screenW_ * 0.5, screenH_ * 0.5};
    touch();
}

void Viewport::panBy(double dx, double dy) {
    if (dx == 0.0 && dy == 0.0) return;
    state_.center = state_.center + Vec2{dx, dy};
    state_.fitted = false;
    touch();
}

void Viewport::setZoomAt(double zoom, Vec2 focal) {
    const double z = clampZoom(zoom);
    if (z == state_.zoom) return;
    // focal' = focal  =>  center' = focal + (center - focal) * (z / zoom)
    state_.center = focal + (state_.center - focal) * (z / state_.zoom);
    state_.zoom = z;
    state_.fitted = false;
    touch();
}

void Viewport::zoomAt(double factor, Vec2 focal) {
    if (factor > 0.0) setZoomAt(state_.zoom * factor, focal);
}

void Viewport::setRotationAt(double radians, Vec2 focal) {
    rotateAt(normalizeAngle(radians) - state_.rotation, focal);
}

void Viewport::rotateAt(double radians, Vec2 focal) {
    if (radians == 0.0) return;
    // Rotate the canvas centre around the focal point by the same angle.
    const Affine2 r = Affine2::rotate(radians);
    state_.center = focal + r.apply(state_.center - focal);
    state_.rotation = normalizeAngle(state_.rotation + radians);
    state_.fitted = false;
    touch();
}

void Viewport::applyGesture(Vec2 prevFocal, Vec2 newFocal, double scale, double rotation) {
    if (!valid()) return;
    const Vec2 anchor = toCanvas(prevFocal);            // canvas point under the fingers
    const double z = clampZoom(state_.zoom * (scale > 0.0 ? scale : 1.0));
    state_.zoom = z;
    state_.rotation = normalizeAngle(state_.rotation + rotation);
    state_.center = {0.0, 0.0};
    const Vec2 placed = toScreen(anchor);               // where it lands with center = 0
    state_.center = newFocal - placed;                  // move it under the fingers
    state_.fitted = false;
    touch();
}

void Viewport::setState(const ViewportState& s) {
    state_ = s;
    state_.zoom = clampZoom(s.zoom);
    state_.rotation = normalizeAngle(s.rotation);
    touch();
}

std::array<Vec2, 4> Viewport::canvasCornersOnScreen() const {
    const double w = canvasW_, h = canvasH_;
    return {toScreen({0, 0}), toScreen({w, 0}), toScreen({w, h}), toScreen({0, h})};
}

RectD Viewport::visibleCanvasBounds() const {
    const Affine2 inv = screenToCanvas();
    const double sw = screenW_, sh = screenH_;
    const Vec2 pts[4] = {inv.apply({0, 0}), inv.apply({sw, 0}), inv.apply({sw, sh}), inv.apply({0, sh})};
    double minX = std::numeric_limits<double>::max(), minY = minX;
    double maxX = -minX, maxY = -minX;
    for (const auto& p : pts) {
        minX = std::min(minX, p.x);
        minY = std::min(minY, p.y);
        maxX = std::max(maxX, p.x);
        maxY = std::max(maxY, p.y);
    }
    return {minX, minY, maxX - minX, maxY - minY};
}

}  // namespace pe::canvas
