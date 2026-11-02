// PxPaint canvas - viewport (how the canvas is placed on the screen).
//
//   screen = center + R(rotation) * zoom * (canvas - canvasSize/2)
//
// zoom     : screen pixels per canvas pixel;
// rotation : radians, positive = clockwise on screen;
// center   : screen position of the canvas centre (pan).
//
// Every operation keeps the transform exact and invertible; gestures keep
// the canvas point under the user's fingers fixed. Thread-safety is the
// owner's job (the Engine guards it with a mutex: the UI thread edits it,
// the render thread reads a snapshot per frame).
#pragma once

#include <array>
#include <cstdint>

#include "canvas/CanvasMath.h"

namespace pe::canvas {

struct ViewportState {
    double zoom = 1.0;
    double rotation = 0.0;
    Vec2 center;
    bool fitted = false;  // still in "fit to screen" (refit on screen resize)
};

class Viewport {
public:
    static constexpr double kMinZoomFloor = 0.001;  // fit must always fit (16384 px on a tiny view)
    static constexpr double kMaxZoom = 64.0;

    void setCanvasSize(uint32_t width, uint32_t height);
    // Resize of the render surface. A fitted view is refitted; otherwise the
    // canvas point at the old screen centre stays at the new screen centre.
    void setScreenSize(uint32_t width, uint32_t height);

    uint32_t canvasWidth() const { return canvasW_; }
    uint32_t canvasHeight() const { return canvasH_; }
    uint32_t screenWidth() const { return screenW_; }
    uint32_t screenHeight() const { return screenH_; }
    bool valid() const { return canvasW_ > 0 && canvasH_ > 0 && screenW_ > 0 && screenH_ > 0; }

    const ViewportState& state() const { return state_; }
    double zoom() const { return state_.zoom; }
    double rotation() const { return state_.rotation; }
    double minZoom() const;  // a little below "fit", never below kMinZoomFloor
    uint64_t revision() const { return revision_; }

    Affine2 canvasToScreen() const;
    Affine2 screenToCanvas() const { return canvasToScreen().inverse(); }
    Vec2 toScreen(Vec2 canvasPoint) const { return canvasToScreen().apply(canvasPoint); }
    Vec2 toCanvas(Vec2 screenPoint) const { return screenToCanvas().apply(screenPoint); }

    // --- operations -------------------------------------------------------
    void fitToScreen(double marginPx = 24.0);      // keeps the rotation
    void centerCanvas();                           // keeps zoom and rotation
    void panBy(double dx, double dy);
    void zoomAt(double factor, Vec2 focalScreen);  // clamped to [minZoom, kMaxZoom]
    void setZoomAt(double zoom, Vec2 focalScreen);
    void rotateAt(double radians, Vec2 focalScreen);
    void setRotationAt(double radians, Vec2 focalScreen);
    // Combined two-finger gesture: the canvas point under prevFocal ends up
    // under newFocal after scaling by `scale` and rotating by `rotation`.
    void applyGesture(Vec2 prevFocal, Vec2 newFocal, double scale, double rotation);
    void setState(const ViewportState& s);

    // --- queries ----------------------------------------------------------
    std::array<Vec2, 4> canvasCornersOnScreen() const;  // TL, TR, BR, BL
    // Axis-aligned canvas-space bounds of what the screen shows (for culling).
    RectD visibleCanvasBounds() const;
    // Clip-space transform for the GPU (OpenGL convention, y up):
    // ndc = canvasToNdc(canvas). Backends flip y in their shader if needed.
    Affine2 canvasToNdc() const;

private:
    void touch() { revision_++; }
    double clampZoom(double z) const;
    double fitZoom(double marginPx) const;

    uint32_t canvasW_ = 0, canvasH_ = 0;
    uint32_t screenW_ = 0, screenH_ = 0;
    ViewportState state_;
    uint64_t revision_ = 0;
};

}  // namespace pe::canvas
