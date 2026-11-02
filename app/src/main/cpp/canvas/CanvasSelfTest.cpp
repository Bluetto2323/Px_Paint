#include "canvas/CanvasSelfTest.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace pe::canvas {

namespace {

struct PatternRect {
    RectI rect;
    Rgba8 color;  // premultiplied
};

// Painted in this order (a later write replaces earlier pixels).
constexpr PatternRect kBase[] = {
    {{100, 100, 300, 200}, {220, 40, 40, 255}},   // red: crosses x=256 and y=256
    {{450, 50, 200, 400}, {40, 170, 70, 255}},    // green: crosses x=512 and y=256
    {{50, 350, 250, 130}, {40, 80, 220, 255}},    // blue: crosses x=256
    {{226, 226, 60, 60}, {250, 210, 30, 255}},    // yellow: corner shared by 4 tiles
    {{420, 460, 200, 35}, {0, 0, 0, 128}},        // 50 % black: premultiplied blending, crosses x=512
    {{660, 0, 40, 500}, {240, 130, 20, 255}},     // orange: right/top/bottom canvas border (outer gutters)
};
constexpr PatternRect kLate = {{480, 100, 80, 80}, {230, 40, 200, 255}};  // magenta over green, crosses x=512

constexpr int kTolerance = 12;
constexpr double kOutsideMarginPx = 12.0;  // shadow + edge antialiasing outside the canvas

bool nearEdge(const RectI& r, double x, double y, double m) {
    const bool inX = x > r.x - m && x < r.right() + m;
    const bool inY = y > r.y - m && y < r.bottom() + m;
    if (!inX || !inY) return false;
    return std::abs(x - r.x) < m || std::abs(x - r.right()) < m || std::abs(y - r.y) < m ||
           std::abs(y - r.bottom()) < m;
}

bool inside(const RectI& r, double x, double y) { return x >= r.x && x < r.right() && y >= r.y && y < r.bottom(); }

Rgba8 straightClear(const gfx::ClearColor& c) {
    auto u = [](float v) { return static_cast<uint8_t>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f)); };
    return {u(c.r), u(c.g), u(c.b), 255};
}

}  // namespace

CanvasAppearance CanvasSelfTest::appearance() {
    CanvasAppearance a;
    a.workspace = {0.106f, 0.110f, 0.125f, 1.0f};
    a.paper = {255, 255, 255, 255};
    a.transparentBackground = false;
    a.shadow = true;
    return a;
}

bool CanvasSelfTest::paintBase(CanvasSurface& surface) {
    bool ok = true;
    for (const auto& p : kBase) ok = surface.fill(p.rect, p.color) && ok;
    return ok;
}

bool CanvasSelfTest::paintLate(CanvasSurface& surface) { return surface.fill(kLate.rect, kLate.color); }

Viewport CanvasSelfTest::viewportFor(int step, uint32_t w, uint32_t h) {
    Viewport vp;
    vp.setCanvasSize(kWidth, kHeight);
    vp.setScreenSize(w, h);
    vp.fitToScreen();
    const double fit = vp.zoom();
    const Vec2 c{w * 0.5, h * 0.5};
    switch (step) {
        case 1:
            vp.setRotationAt(kPi / 6.0, c);
            vp.setZoomAt(fit * 1.37, c);
            vp.panBy(13.0, -7.0);
            break;
        case 2: {
            vp.setRotationAt(kPi / 2.0, c);
            vp.setZoomAt(fit * 3.0, c);
            const Vec2 s = vp.toScreen({256.0, 256.0});
            vp.panBy(c.x - s.x, c.y - s.y);
            break;
        }
        case 3:
            vp.setRotationAt(-kPi / 4.0, c);
            vp.setZoomAt(fit * 0.8, c);
            break;
        default: break;
    }
    return vp;
}

std::optional<Rgba8> CanvasSelfTest::expectedCanvas(double x, double y, double m, bool late) {
    const RectI canvas{0, 0, static_cast<int32_t>(kWidth), static_cast<int32_t>(kHeight)};
    if (!inside(canvas, x, y) || nearEdge(canvas, x, y, m)) return std::nullopt;
    const PatternRect* top = nullptr;
    for (const auto& p : kBase) {
        if (nearEdge(p.rect, x, y, m)) return std::nullopt;
        if (inside(p.rect, x, y)) top = &p;
    }
    if (late) {
        if (nearEdge(kLate.rect, x, y, m)) return std::nullopt;
        if (inside(kLate.rect, x, y)) top = &kLate;
    }
    const Rgba8 paper = appearance().paper;
    if (!top) return paper;
    // Premultiplied "over" onto opaque paper.
    const Rgba8 s = top->color;
    auto over = [&](uint8_t sc, uint8_t dc) {
        return static_cast<uint8_t>(std::lround(sc + dc * (255.0 - s.a) / 255.0));
    };
    return Rgba8{over(s.r, paper.r), over(s.g, paper.g), over(s.b, paper.b), 255};
}

CanvasSelfTest::StepResult CanvasSelfTest::verify(int step, const gfx::CaptureResult& cap, const Viewport& vp) {
    StepResult r;
    if (cap.width < 32 || cap.height < 32 || cap.rgba.size() < static_cast<size_t>(cap.width) * cap.height * 4) {
        r.detail = "capture too small or incomplete";
        return r;
    }
    if (cap.width != vp.screenWidth() || cap.height != vp.screenHeight()) {
        r.detail = "capture " + std::to_string(cap.width) + "x" + std::to_string(cap.height) +
                   " does not match the viewport target";
        return r;
    }
    const bool late = step >= 2;
    const bool tiled = tiledStep(step);
    const double zoom = vp.zoom();
    const double margin = 1.5 / zoom + 1.0;  // linear filtering + pixel-centre offset
    const Rgba8 workspace = straightClear(appearance().workspace);
    const Affine2 toCanvas = vp.screenToCanvas();
    const RectI canvas{0, 0, static_cast<int32_t>(kWidth), static_cast<int32_t>(kHeight)};
    std::string firstErrors;

    auto compare = [&](uint32_t px, uint32_t py, const Rgba8& e, double cx, double cy, bool seam) {
        const uint8_t* p = &cap.rgba[(static_cast<size_t>(py) * cap.width + px) * 4];
        r.probes++;
        if (seam) r.seamProbes++;
        if (std::abs(p[0] - e.r) <= kTolerance && std::abs(p[1] - e.g) <= kTolerance &&
            std::abs(p[2] - e.b) <= kTolerance)
            return;
        if (r.mismatches++ < 4) {
            char buf[200];
            std::snprintf(buf, sizeof(buf), "%s(%u,%u)=canvas(%.1f,%.1f) got %d,%d,%d expected %d,%d,%d; ",
                          seam ? "seam " : "", px, py, cx, cy, p[0], p[1], p[2], e.r, e.g, e.b);
            firstErrors += buf;
        }
    };

    // 1. Screen grid: canvas interior and the workspace around it.
    constexpr uint32_t kStride = 5;
    for (uint32_t py = 2; py < cap.height; py += kStride) {
        for (uint32_t px = 2; px < cap.width; px += kStride) {
            const Vec2 c = toCanvas.apply({px + 0.5, py + 0.5});
            if (inside(canvas, c.x, c.y)) {
                if (const auto e = expectedCanvas(c.x, c.y, margin, late)) compare(px, py, *e, c.x, c.y, false);
            } else {
                const double dx = std::max({0.0, -c.x, c.x - kWidth});
                const double dy = std::max({0.0, -c.y, c.y - kHeight});
                if (std::sqrt(dx * dx + dy * dy) * zoom > kOutsideMarginPx) compare(px, py, workspace, c.x, c.y, false);
            }
        }
    }
    // 2. Tile boundaries: points just either side of every internal edge.
    if (tiled) {
        const Affine2 toScreen = vp.canvasToScreen();
        auto probeLine = [&](bool vertical, double at) {
            const double len = vertical ? kHeight : kWidth;
            for (double t = 1.5; t < len - 1.5; t += 2.0) {
                for (const double side : {-0.3, 0.3}) {
                    const double cx = vertical ? at + side : t, cy = vertical ? t : at + side;
                    const Vec2 s = toScreen.apply({cx, cy});
                    if (s.x < 0 || s.y < 0 || s.x >= cap.width || s.y >= cap.height) continue;
                    const auto px = static_cast<uint32_t>(s.x), py = static_cast<uint32_t>(s.y);
                    // Judge the pixel by its own centre (the probe point is only where we look).
                    const Vec2 c = toCanvas.apply({px + 0.5, py + 0.5});
                    if (const auto e = expectedCanvas(c.x, c.y, margin, late)) compare(px, py, *e, c.x, c.y, true);
                }
            }
        };
        for (uint32_t x = 256; x < kWidth; x += 256) probeLine(true, x);
        for (uint32_t y = 256; y < kHeight; y += 256) probeLine(false, y);
    }

    const uint32_t minProbes = 200, minSeam = tiled ? 20u : 0u;
    r.passed = r.mismatches == 0 && r.probes >= minProbes && r.seamProbes >= minSeam;
    char head[200];
    std::snprintf(head, sizeof(head), "step %d (%s, zoom %.3f, rot %.0f deg): %u probes, %u on tile seams, %u mismatches",
                  step, tiled ? "tiled" : "single texture", zoom, vp.rotation() * 180.0 / kPi, r.probes,
                  r.seamProbes, r.mismatches);
    r.detail = head;
    if (r.mismatches) r.detail += " - " + firstErrors;
    else if (r.probes < minProbes || r.seamProbes < minSeam) r.detail += " - too few probes on screen";
    return r;
}

}  // namespace pe::canvas
