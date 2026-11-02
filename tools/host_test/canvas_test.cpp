// PxPaint - canvas core unit tests (pure CPU: viewport math, tiles, storage).
//
// Property-style: thousands of random viewports / screens / canvases are
// checked against invariants (exact inverse, no distortion, focal points
// fixed under zoom/rotation/gestures, fit keeps the canvas on screen...).
#include <cmath>
#include <cstdio>
#include <random>
#include <string>
#include <vector>

#include "canvas/TileGrid.h"
#include "canvas/Viewport.h"

using namespace pe::canvas;

namespace {

int gChecks = 0, gFailures = 0;

void check(bool ok, const std::string& what) {
    gChecks++;
    if (!ok) gFailures++;
    std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what.c_str());
}

double dist(Vec2 a, Vec2 b) { return (a - b).length(); }

struct Rng {
    std::mt19937_64 gen{0x5eedULL};
    double uniform(double lo, double hi) { return std::uniform_real_distribution<double>(lo, hi)(gen); }
    uint32_t integer(uint32_t lo, uint32_t hi) { return std::uniform_int_distribution<uint32_t>(lo, hi)(gen); }
};

// Screens: portrait/landscape phones (incl. A33-like 1080x2400), tablets, tiny.
const std::vector<std::pair<uint32_t, uint32_t>> kScreens = {
    {1080, 2400}, {2400, 1080}, {720, 1600}, {1600, 720}, {2560, 1600}, {1600, 2560}, {320, 240}, {64, 900}};
// Canvases: square, portrait, landscape, odd, huge, 1-pixel.
const std::vector<std::pair<uint32_t, uint32_t>> kCanvases = {
    {2048, 2048}, {2480, 3508}, {3840, 2160}, {1000, 777}, {16384, 16384}, {1, 1}, {4096, 300}};

void testAffine() {
    std::printf("-- affine\n");
    Rng rng;
    double worst = 0;
    for (int i = 0; i < 1000; ++i) {
        const Affine2 m = Affine2::compose(
            Affine2::translate(rng.uniform(-5e3, 5e3), rng.uniform(-5e3, 5e3)),
            Affine2::compose(Affine2::rotate(rng.uniform(-10, 10)), Affine2::scale(rng.uniform(0.01, 64))));
        const Vec2 p{rng.uniform(-1e4, 1e4), rng.uniform(-1e4, 1e4)};
        worst = std::max(worst, dist(m.inverse().apply(m.apply(p)), p));
    }
    check(worst < 1e-6, "inverse(apply(p)) == p over 1000 random transforms (worst " + std::to_string(worst) + ")");
}

void testRoundTripAndAspect() {
    std::printf("-- viewport: round trip + no distortion\n");
    Rng rng;
    double worstRoundTrip = 0, worstAspect = 0, worstAngle = 0;
    for (auto [sw, sh] : kScreens) {
        for (auto [cw, ch] : kCanvases) {
            Viewport v;
            v.setScreenSize(sw, sh);
            v.setCanvasSize(cw, ch);
            for (int i = 0; i < 60; ++i) {
                ViewportState s;
                s.zoom = rng.uniform(v.minZoom(), Viewport::kMaxZoom);
                s.rotation = rng.uniform(-kPi, kPi);
                s.center = {rng.uniform(-1.0 * sw, 2.0 * sw), rng.uniform(-1.0 * sh, 2.0 * sh)};
                v.setState(s);
                for (int k = 0; k < 8; ++k) {
                    const Vec2 sp{rng.uniform(0, sw), rng.uniform(0, sh)};
                    const double err = dist(v.toScreen(v.toCanvas(sp)), sp);
                    worstRoundTrip = std::max(worstRoundTrip, err);
                }
                const auto c = v.canvasCornersOnScreen();
                const double wPx = dist(c[1], c[0]), hPx = dist(c[3], c[0]);
                worstAspect = std::max(worstAspect, std::abs(wPx / hPx - double(cw) / ch) / (double(cw) / ch));
                // edges stay perpendicular (no shear)
                const Vec2 e1 = c[1] - c[0], e2 = c[3] - c[0];
                worstAngle = std::max(worstAngle, std::abs(e1.x * e2.x + e1.y * e2.y) / (wPx * hPx));
            }
        }
    }
    check(worstRoundTrip < 1e-6, "screen->canvas->screen exact (worst " + std::to_string(worstRoundTrip) + " px)");
    check(worstAspect < 1e-9, "canvas aspect ratio preserved for every screen/zoom/rotation (rel err " +
                                  std::to_string(worstAspect) + ")");
    check(worstAngle < 1e-9, "canvas corners stay perpendicular (no shear)");
}

void testFit() {
    std::printf("-- viewport: fit to screen\n");
    bool allInside = true, allTight = true, centered = true;
    for (auto [sw, sh] : kScreens) {
        for (auto [cw, ch] : kCanvases) {
            for (double deg : {0.0, 30.0, 90.0, 137.0, -45.0, 180.0}) {
                Viewport v;
                v.setScreenSize(sw, sh);
                v.setCanvasSize(cw, ch);
                v.setRotationAt(deg * kPi / 180.0, {sw * 0.5, sh * 0.5});
                const double margin = std::min(24.0, std::min(sw, sh) / 4.0);
                v.fitToScreen(margin);
                double minX = 1e18, minY = 1e18, maxX = -1e18, maxY = -1e18;
                for (auto p : v.canvasCornersOnScreen()) {
                    minX = std::min(minX, p.x), minY = std::min(minY, p.y);
                    maxX = std::max(maxX, p.x), maxY = std::max(maxY, p.y);
                }
                const double eps = 1e-6;
                if (minX < margin - eps || minY < margin - eps || maxX > sw - margin + eps || maxY > sh - margin + eps)
                    allInside = false;
                const bool tightX = std::abs(minX - margin) < 1e-6 && std::abs(maxX - (sw - margin)) < 1e-6;
                const bool tightY = std::abs(minY - margin) < 1e-6 && std::abs(maxY - (sh - margin)) < 1e-6;
                if (!tightX && !tightY && v.zoom() > Viewport::kMinZoomFloor && v.zoom() < Viewport::kMaxZoom)
                    allTight = false;
                if (dist(v.toScreen({cw * 0.5, ch * 0.5}), {sw * 0.5, sh * 0.5}) > 1e-6) centered = false;
            }
        }
    }
    check(allInside, "fit: all canvas corners inside the screen margins (any rotation)");
    check(allTight, "fit: canvas touches the margins on one axis (largest possible)");
    check(centered, "fit: canvas centre at screen centre");
}

void testFocalInvariance() {
    std::printf("-- viewport: zoom / rotate / gesture keep the focal point\n");
    Rng rng;
    double worstZoom = 0, worstRot = 0, worstGesture = 0;
    bool clamped = true;
    Viewport v;
    v.setScreenSize(1080, 2400);
    v.setCanvasSize(2480, 3508);
    for (int i = 0; i < 5000; ++i) {
        const Vec2 f{rng.uniform(0, 1080), rng.uniform(0, 2400)};
        const Vec2 before = v.toCanvas(f);
        const double z0 = v.zoom();
        v.zoomAt(rng.uniform(0.5, 2.0), f);
        if (v.zoom() != z0) worstZoom = std::max(worstZoom, dist(v.toCanvas(f), before));
        if (v.zoom() < v.minZoom() - 1e-12 || v.zoom() > Viewport::kMaxZoom + 1e-12) clamped = false;

        const Vec2 f2{rng.uniform(0, 1080), rng.uniform(0, 2400)};
        const Vec2 b2 = v.toCanvas(f2);
        v.rotateAt(rng.uniform(-0.5, 0.5), f2);
        worstRot = std::max(worstRot, dist(v.toCanvas(f2), b2));

        const Vec2 p0{rng.uniform(0, 1080), rng.uniform(0, 2400)};
        const Vec2 p1{rng.uniform(0, 1080), rng.uniform(0, 2400)};
        const Vec2 anchor = v.toCanvas(p0);
        v.applyGesture(p0, p1, rng.uniform(0.9, 1.1), rng.uniform(-0.1, 0.1));
        worstGesture = std::max(worstGesture, dist(v.toScreen(anchor), p1));
        if (v.zoom() >= Viewport::kMaxZoom * 0.9 || v.zoom() <= v.minZoom() * 1.1) v.fitToScreen();
    }
    check(worstZoom < 1e-6, "zoomAt: canvas point under the focal point does not move (worst " +
                                std::to_string(worstZoom) + " canvas px)");
    check(worstRot < 1e-6, "rotateAt: canvas point under the focal point does not move");
    check(worstGesture < 1e-6, "applyGesture: canvas point under the fingers follows them exactly");
    check(clamped, "zoom always within [minZoom, maxZoom]");
}

void testScreenResize() {
    std::printf("-- viewport: screen resize / device rotation\n");
    Viewport v;
    v.setScreenSize(1080, 2400);
    v.setCanvasSize(3000, 2000);
    v.setScreenSize(2400, 1080);  // rotate the phone while fitted
    bool inside = true;
    for (auto p : v.canvasCornersOnScreen())
        if (p.x < -1e-6 || p.y < -1e-6 || p.x > 2400 + 1e-6 || p.y > 1080 + 1e-6) inside = false;
    check(v.state().fitted && inside, "fitted view is refitted when the screen rotates");

    v.zoomAt(3.0, {700, 300});  // user zoomed: no longer fitted
    const Vec2 mid = v.toCanvas({1200, 540});
    v.setScreenSize(1080, 2400);
    check(!v.state().fitted && dist(v.toCanvas({540, 1200}), mid) < 1e-6,
          "zoomed view keeps the canvas point at the screen centre across a resize");
    const double z = v.zoom();
    v.setScreenSize(1080, 1800);
    check(std::abs(v.zoom() - z) < 1e-12, "resize does not change the zoom of a non-fitted view");
}

void testNdcAndBounds() {
    std::printf("-- viewport: clip space and visible bounds\n");
    Viewport v;
    v.setScreenSize(1080, 2400);
    v.setCanvasSize(2048, 2048);
    v.rotateAt(0.7, {300, 900});
    v.zoomAt(2.5, {500, 1000});
    const Affine2 toNdc = v.canvasToNdc();
    const Affine2 inv = v.screenToCanvas();
    const Vec2 tl = toNdc.apply(inv.apply({0, 0})), br = toNdc.apply(inv.apply({1080, 2400}));
    check(dist(tl, {-1, 1}) < 1e-9 && dist(br, {1, -1}) < 1e-9, "screen corners map to NDC (-1,1) / (1,-1)");
    const RectD b = v.visibleCanvasBounds();
    bool contains = true;
    for (Vec2 s : {Vec2{0, 0}, Vec2{1080, 0}, Vec2{1080, 2400}, Vec2{0, 2400}, Vec2{540, 1200}}) {
        const Vec2 c = inv.apply(s);
        if (c.x < b.x - 1e-6 || c.y < b.y - 1e-6 || c.x > b.x + b.w + 1e-6 || c.y > b.y + b.h + 1e-6) contains = false;
    }
    check(contains, "visible canvas bounds contain every screen point (culling is conservative)");
}

void testTiles() {
    std::printf("-- tile grid\n");
    bool coverage = true, partialEdges = true;
    for (auto [w, h] : std::vector<std::pair<uint32_t, uint32_t>>{
             {1, 1}, {255, 255}, {256, 256}, {257, 257}, {1000, 777}, {4096, 4096}, {16384, 300}}) {
        const TileGrid g(w, h, 256);
        int64_t area = 0;
        for (size_t i = 0; i < g.tileCount(); ++i) {
            const RectI r = g.tileRect(g.coord(i));
            area += r.area();
            if (r.empty() || r.right() > static_cast<int32_t>(w) || r.bottom() > static_cast<int32_t>(h)) coverage = false;
        }
        if (area != static_cast<int64_t>(w) * h) coverage = false;
        const RectI last = g.tileRect({g.columns() - 1, g.rows() - 1});
        if (w % 256 && last.w != static_cast<int32_t>(w % 256)) partialEdges = false;
        if (h % 256 && last.h != static_cast<int32_t>(h % 256)) partialEdges = false;
    }
    check(coverage, "tiles cover every canvas pixel exactly once (7 sizes incl. 1x1, 257, 16384x300)");
    check(partialEdges, "edge tiles are clipped to the canvas");

    const TileGrid g(1000, 777, 256);
    check(g.columns() == 4 && g.rows() == 4, "1000x777 -> 4x4 tiles");
    auto ti = g.tilesIntersecting({250, 250, 12, 12});
    check(ti.size() == 4, "rect across a tile corner touches 4 tiles");
    check(g.tilesIntersecting({0, 0, 256, 256}).size() == 1, "rect exactly one tile -> 1 tile");
    check(g.tilesIntersecting({-50, -50, 40, 40}).empty(), "rect outside the canvas -> no tiles");
    check(g.tilesIntersecting({800, 770, 500, 500}).size() == 1, "rect beyond the edge clipped to the last tile");
    check(g.tilesIntersecting({900, 700, 500, 500}).size() == 2, "rect beyond the edge spanning two rows -> 2 tiles");
    auto st = g.tilesStoring({255, 10, 1, 1});
    check(st.size() == 2, "write at x=255 also updates the neighbour's gutter (2 tiles)");
    check(g.tilesStoring({254, 10, 1, 1}).size() == 1, "write at x=254 touches only its own tile");
    check(g.tilesStoring({256, 256, 1, 1}).size() == 4, "write at a tile corner pixel updates 4 tiles");
    const RectI sr = g.tileStorageRect({1, 0});
    check(sr.x == 255 && sr.w == 258 && g.textureSize() == 258, "tile storage = tile + 1px gutter (258x258)");
}

void testStorage() {
    std::printf("-- storage policy\n");
    StorageLimits big{16384, 512ull << 20};
    auto p = StoragePolicy::choose(1024, 1024, big);
    check(p.ok && p.mode == StorageMode::SingleTexture, "1024x1024 -> single texture: " + p.reason);
    p = StoragePolicy::choose(2048, 2048, big);
    check(p.ok && p.mode == StorageMode::SingleTexture, "2048x2048 -> single texture");
    p = StoragePolicy::choose(2049, 1000, big);
    check(p.ok && p.mode == StorageMode::Tiled && p.tileSize == 256, "2049x1000 -> tiled: " + p.reason);
    p = StoragePolicy::choose(1500, 1500, {1024, 512ull << 20});
    check(p.ok && p.mode == StorageMode::Tiled, "GPU max texture 1024 -> 1500x1500 must be tiled");
    p = StoragePolicy::choose(16385, 100, big);
    check(!p.ok, "side above 16384 rejected: " + p.reason);
    p = StoragePolicy::choose(8192, 8192, {16384, 64ull << 20});
    check(!p.ok, "8192x8192 rejected with a 64 MiB budget: " + p.reason);
    p = StoragePolicy::choose(4096, 4096, big);
    const uint64_t expected = 16ull * 16 * 258 * 258 * 4;
    check(p.ok && p.maxAllocatedBytes == expected, "4096x4096 worst case = 256 tiles x 258^2 x 4 B (" +
                                                       std::to_string(p.maxAllocatedBytes >> 20) + " MiB)");
    p = StoragePolicy::choose(100, 100, {200, 0});
    check(!p.ok, "GPU limit smaller than one tile rejected");
}

}  // namespace

int main() {
    std::printf("=== canvas core tests ===\n");
    testAffine();
    testRoundTripAndAspect();
    testFit();
    testFocalInvariance();
    testScreenResize();
    testNdcAndBounds();
    testTiles();
    testStorage();
    std::printf("=== %d/%d checks passed ===\n", gChecks - gFailures, gChecks);
    return gFailures == 0 ? 0 : 1;
}
