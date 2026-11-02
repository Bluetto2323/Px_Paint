// PxPaint canvas - GPU self-test (both backends, on the real device).
//
// Draws a known calibration canvas through the real canvas path (storage,
// partial uploads, tiles + gutters, culling, viewport transform, blending)
// and checks the captured backbuffer pixel by pixel against the expected
// image computed on the CPU from the same viewport math.
//
// Calibration canvas: 700 x 500 px, white paper, coloured rectangles that
// straddle tile boundaries (256 px), a yellow patch over the corner shared by
// four tiles, a 50 % black patch (premultiplied blending), and - from step 2
// on - a magenta rectangle written AFTER the canvas was already drawn
// (partial update of textures in use).
//
// Steps (each one frame + capture):
//   0  tiled storage, fit to screen
//   1  tiled, zoom 1.37, rotation +30 deg, off-centre
//   2  tiled, zoom 3, rotation 90 deg, magnified tile corner (seams) + late partial update
//   3  single-texture storage, zoom 0.8, rotation -45 deg (same pattern)
#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include "canvas/CanvasRenderer.h"
#include "canvas/CanvasSurface.h"
#include "canvas/Viewport.h"
#include "graphics/abstraction/GraphicsDevice.h"

namespace pe::canvas {

class CanvasSelfTest {
public:
    static constexpr uint32_t kWidth = 700;
    static constexpr uint32_t kHeight = 500;
    static constexpr int kSteps = 4;

    struct StepResult {
        bool passed = false;
        uint32_t probes = 0;          // pixels compared
        uint32_t seamProbes = 0;      // ... of which on tile boundaries
        uint32_t mismatches = 0;
        std::string detail;
    };

    static CanvasAppearance appearance();
    static bool tiledStep(int step) { return step != 3; }
    // Writes the base pattern (partial uploads, several tiles each).
    static bool paintBase(CanvasSurface& surface);
    // The late partial update applied before step 2.
    static bool paintLate(CanvasSurface& surface);
    // Viewport of a step for a target of the given size.
    static Viewport viewportFor(int step, uint32_t targetWidth, uint32_t targetHeight);
    // Expected straight RGB at a canvas point; nullopt where it is ambiguous
    // (within `margin` canvas px of a colour edge).
    static std::optional<Rgba8> expectedCanvas(double x, double y, double margin, bool late);

    static StepResult verify(int step, const gfx::CaptureResult& capture, const Viewport& viewport);
};

}  // namespace pe::canvas
