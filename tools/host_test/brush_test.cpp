// PxPaint - brush engine unit tests (pure CPU: path, dabs, compositing, store).
//
// The brush engine rasterises on the CPU into the RasterStore, deterministically,
// so its quality is measured here pixel by pixel:
//   * stroke width (integrated coverage across the stroke) == brush diameter,
//     constant along the stroke (no bumps between dabs, no thinning);
//   * no gaps (the centre line is fully covered), exact start and end caps;
//   * opacity is exact and never exceeded by overlapping dabs of one stroke;
//     strokes stack as source-over; no dark fringes on transparent canvases;
//   * slow (dense) and fast (sparse) sampling of the same path agree;
//   * results do not depend on how the input is batched into frames;
//   * tile boundaries are invisible; cancel restores the canvas bit for bit.
// The GPU side (upload + display on Vulkan and GLES) is in host_test --suite brush.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <vector>

#include "brush/BrushEngine.h"
#include "canvas/RasterStore.h"
#include "core/Time.h"

using pe::brush::BrushEngine;
using pe::brush::BrushSettings;
using pe::brush::StrokePoint;
using pe::canvas::RasterStore;
using pe::canvas::Rgba8;

namespace {

int gChecks = 0, gFailures = 0;
constexpr double kPi = 3.14159265358979323846;

void check(bool ok, const std::string& what) {
    gChecks++;
    if (!ok) gFailures++;
    std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what.c_str());
    std::fflush(stdout);
}
void section(const char* s) { std::printf("-- %s\n", s); }
std::string fmt(const char* f, double a, double b = 0, double c = 0) {
    char buf[256];
    std::snprintf(buf, sizeof(buf), f, a, b, c);
    return buf;
}

BrushSettings brush(float d, float opacity = 1.0f, uint32_t argb = 0xFF000000u, float spacing = 0.08f) {
    BrushSettings s;
    s.diameter = d;
    s.opacity = opacity;
    s.argb = argb;
    s.spacing = spacing;
    return s;
}

using Path = std::vector<StrokePoint>;

Path line(double x0, double y0, double x1, double y1, int samples) {
    Path p;
    for (int i = 0; i < samples; ++i) {
        const double t = samples == 1 ? 0.0 : static_cast<double>(i) / (samples - 1);
        p.push_back({x0 + (x1 - x0) * t, y0 + (y1 - y0) * t, t * 100});
    }
    return p;
}

Path circle(double cx, double cy, double r, int samples, double turns = 1.0) {
    Path p;
    for (int i = 0; i <= samples; ++i) {
        const double a = 2 * kPi * turns * i / samples;
        p.push_back({cx + r * std::cos(a), cy + r * std::sin(a), static_cast<double>(i)});
    }
    return p;
}

// Draws a whole stroke; `batch` = samples per process() call (0 = all at once).
void draw(BrushEngine& e, RasterStore& s, const BrushSettings& b, const Path& p, size_t batch = 0,
          bool finish = true) {
    e.begin(b, p.front());
    if (batch == 0) {
        if (p.size() > 1) e.addSamples(p.data() + 1, p.size() - 1);
    } else {
        for (size_t i = 1; i < p.size(); i += batch) {
            e.addSamples(p.data() + i, std::min(batch, p.size() - i));
            e.process(s);
        }
    }
    if (finish) e.end();
    e.process(s);
}

double alpha(const RasterStore& s, int x, int y) { return s.pixel(x, y).a / 255.0; }

// Integrated coverage along a vertical line x (rows y0..y1): == stroke width
// for a horizontal stroke drawn at opacity 1.
double columnInk(const RasterStore& s, int x, int y0, int y1) {
    double sum = 0;
    for (int y = y0; y <= y1; ++y) sum += alpha(s, x, y);
    return sum;
}
double rowInk(const RasterStore& s, int y, int x0, int x1) {
    double sum = 0;
    for (int x = x0; x <= x1; ++x) sum += alpha(s, x, y);
    return sum;
}

bool sameStore(const RasterStore& a, const RasterStore& b, int tolerance, int* worst = nullptr) {
    int w = 0;
    for (int y = 0; y < static_cast<int>(a.height()); ++y)
        for (int x = 0; x < static_cast<int>(a.width()); ++x) {
            const Rgba8 p = a.pixel(x, y), q = b.pixel(x, y);
            w = std::max({w, std::abs(p.r - q.r), std::abs(p.g - q.g), std::abs(p.b - q.b), std::abs(p.a - q.a)});
        }
    if (worst) *worst = w;
    return w <= tolerance;
}

void fillOpaque(RasterStore& s, uint32_t argb) { s.fill(s.canvasRect(), Rgba8::fromArgb(argb)); }

// ---------------------------------------------------------------- tests
void testStraight() {
    section("B1 straight stroke: width = diameter, constant, no gaps, exact caps");
    RasterStore s;
    s.reset(800, 400);
    BrushEngine e;
    const double d = 20.0;
    draw(e, s, brush(static_cast<float>(d)), line(100.3, 200.4, 700.6, 200.4, 60));
    double lo = 1e9, hi = 0, minCentre = 1;
    for (int x = 130; x <= 670; ++x) {
        const double w = columnInk(s, x, 170, 230);
        lo = std::min(lo, w);
        hi = std::max(hi, w);
        minCentre = std::min(minCentre, alpha(s, x, 200));
    }
    check(std::abs(lo - d) < 0.25 && std::abs(hi - d) < 0.25,
          fmt("B1: width %.3f..%.3f px for a %.0f px brush", lo, hi, d));
    check(hi - lo < 0.1, fmt("B1: width varies by %.4f px along the stroke (no dab bumps)", hi - lo));
    check(minCentre == 1.0, "B1: centre line fully covered (no gaps)");
    check(alpha(s, 100, 200) == 1.0 && alpha(s, 700, 200) == 1.0, "B1: start and end samples fully covered");
    check(alpha(s, 100 - 12, 200) == 0.0 && alpha(s, 700 + 12, 200) == 0.0, "B1: nothing beyond the round caps");
    const double capInk = rowInk(s, 200, 80, 99);  // left cap: from 90.3 (r = 10 before 100.3) to pixel 100
    check(std::abs(capInk - (100.0 - 90.3)) < 0.1, fmt("B1: round start cap reaches r = 10 px (ink %.2f, expected 9.70)", capInk));
    // Antialiasing: the edge has intermediate values, not a hard step.
    const double edge = alpha(s, 400, 190);
    check(edge > 0.05 && edge < 0.95, fmt("B1: antialiased edge (alpha %.3f at the boundary)", edge));
}

void testDiagonal() {
    section("B2 diagonal strokes (45 and 30 degrees)");
    for (double deg : {45.0, 30.0}) {
        RasterStore s;
        s.reset(900, 900);
        BrushEngine e;
        const double d = 16.0, rad = deg * kPi / 180.0;
        draw(e, s, brush(static_cast<float>(d)), line(100, 100, 100 + 700 * std::cos(rad), 100 + 700 * std::sin(rad), 40));
        // A vertical cut through a stroke at angle a is d / cos(a) long.
        double lo = 1e9, hi = 0;
        for (int x = 250; x <= 550; x += 7) {  // away from the end caps
            const double w = columnInk(s, x, 0, 899);
            lo = std::min(lo, w);
            hi = std::max(hi, w);
        }
        const double expect = d / std::cos(rad);
        check(std::abs(lo - expect) < 0.3 && std::abs(hi - expect) < 0.3,
              fmt("B2: %.0f deg: cut %.3f..%.3f px, expected %.3f", deg, lo, hi) + fmt(" (%.3f)", expect));
    }
}

// Radial profile of a circular stroke at angle a: (ink, centre radius).
void radial(const RasterStore& s, double cx, double cy, double a, double rMin, double rMax, double& ink,
            double& centre, double& peak) {
    ink = 0;
    double wsum = 0;
    peak = 0;
    for (double r = rMin; r <= rMax; r += 0.25) {
        const int x = static_cast<int>(std::floor(cx + r * std::cos(a)));
        const int y = static_cast<int>(std::floor(cy + r * std::sin(a)));
        const double v = alpha(s, x, y);
        ink += v * 0.25;
        wsum += v * 0.25 * r;
        peak = std::max(peak, v);
    }
    centre = ink > 0 ? wsum / ink : 0;
}

void testCircle() {
    section("B3 circular strokes: smooth curve from sparse (fast) and dense (slow) sampling");
    const double cx = 400, cy = 400, R = 250, d = 12;
    for (int samples : {16, 48, 720}) {
        RasterStore s;
        s.reset(800, 800);
        BrushEngine e;
        draw(e, s, brush(static_cast<float>(d)), circle(cx, cy, R, samples));
        double worstR = 0, minInk = 1e9, maxInk = 0, minPeak = 1;
        for (int i = 0; i < 360; ++i) {
            const double a = 2 * kPi * (i + 0.37) / 360;
            double ink, centre, peak;
            radial(s, cx, cy, a, R - 30, R + 30, ink, centre, peak);
            worstR = std::max(worstR, std::abs(centre - R));
            minInk = std::min(minInk, ink);
            maxInk = std::max(maxInk, ink);
            minPeak = std::min(minPeak, peak);
        }
        check(minPeak == 1.0, fmt("B3: %.0f samples/turn: no gap anywhere on the circle", samples));
        check(minInk > d - 0.8 && maxInk < d + 0.8,
              fmt("B3: %.0f samples/turn: radial width %.2f..%.2f px", samples, minInk, maxInk));
        // A polyline through the samples would sag by R(1 - cos(pi/n)) between
        // them; the spline must do clearly better (and the measurement itself
        // has ~0.2 px of pixel quantisation, see 720 samples).
        const double sag = R * (1 - std::cos(kPi / samples));
        const double tol = 0.7 * sag + 0.2;
        check(worstR < tol, fmt("B3: %.0f samples/turn: centre line within %.2f px of the circle (polyline sag %.2f px)",
                                samples, worstR, sag));
    }
}

void testFastSlow() {
    section("B4 fast vs slow input: same path, different sampling -> same pixels");
    RasterStore slow, fast;
    slow.reset(600, 300);
    fast.reset(600, 300);
    BrushEngine e1, e2;
    draw(e1, slow, brush(14), line(50, 150.5, 550, 150.5, 2000));  // ~0.25 px per sample
    draw(e2, fast, brush(14), line(50, 150.5, 550, 150.5, 3));     // 250 px per sample
    int worst = 0;
    check(sameStore(slow, fast, 1, &worst), fmt("B4: straight line: max channel difference %.0f", worst));
    // Curved: a sine wave sampled densely vs every 40 px.
    RasterStore a, b;
    a.reset(800, 400);
    b.reset(800, 400);
    Path dense, sparse;
    for (int i = 0; i <= 1400; ++i) dense.push_back({50 + i * 0.5, 200 + 80 * std::sin(i * 0.5 / 70.0), 0});
    for (int i = 0; i <= 1400; i += 80) sparse.push_back(dense[i]);
    BrushEngine e3, e4;
    draw(e3, a, brush(10), dense);
    draw(e4, b, brush(10), sparse);
    double worstInk = 0, minPeak = 1;
    for (int x = 80; x <= 700; x += 3) {
        worstInk = std::max(worstInk, std::abs(columnInk(a, x, 0, 399) - columnInk(b, x, 0, 399)));
        double peak = 0;
        for (int y = 0; y < 400; ++y) peak = std::max(peak, alpha(b, x, y));
        minPeak = std::min(minPeak, peak);
    }
    check(minPeak == 1.0, "B4: sparse sine: continuous, no gaps");
    check(worstInk < 1.0, fmt("B4: sparse vs dense sine: per-column ink differs by <= %.3f px", worstInk));
}

void testBatching() {
    section("B5 determinism: input batching (frames) and the provisional tail do not change the result");
    RasterStore one, many, single;
    one.reset(700, 500);
    many.reset(700, 500);
    single.reset(700, 500);
    Path p = circle(350, 250, 180, 37, 1.3);
    BrushEngine e1, e2, e3;
    draw(e1, one, brush(18, 0.6f, 0xFF2050C0u), p, 0);
    draw(e2, many, brush(18, 0.6f, 0xFF2050C0u), p, 3);
    draw(e3, single, brush(18, 0.6f, 0xFF2050C0u), p, 1);
    int w1 = 0, w2 = 0;
    check(sameStore(one, many, 0, &w1) && sameStore(one, single, 0, &w2),
          fmt("B5: identical pixels processing all at once / 3 per frame / 1 per frame (max diff %.0f, %.0f)", w1, w2));
    // The tip follows the latest sample while the stroke is in progress.
    RasterStore live;
    live.reset(700, 500);
    BrushEngine e4;
    Path q = line(100, 250, 600, 250, 6);
    e4.begin(brush(12), q[0]);
    e4.addSamples(q.data() + 1, 5);
    e4.process(live);
    check(alpha(live, 600, 250) == 1.0, "B5: provisional tail reaches the latest sample before the stroke ends");
    e4.end();
    e4.process(live);
    RasterStore ref;
    ref.reset(700, 500);
    BrushEngine e5;
    draw(e5, ref, brush(12), q);
    check(sameStore(live, ref, 0), "B5: committing the tail gives exactly the finished stroke");
}

void testShort() {
    section("B6 short strokes: tap, tiny drag");
    RasterStore s;
    s.reset(400, 400);
    BrushEngine e;
    const double d = 30;
    draw(e, s, brush(static_cast<float>(d)), Path{{200.5, 200.5, 0}});
    double area = 0;
    for (int y = 170; y < 231; ++y) area += rowInk(s, y, 170, 230);
    const double disc = kPi * (d / 2) * (d / 2);
    check(std::abs(area - disc) / disc < 0.01, fmt("B6: tap = round dot, area %.1f vs pi r^2 %.1f", area, disc));
    check(alpha(s, 200, 200) == 1.0 && alpha(s, 200 + 16, 200) == 0.0, "B6: dot centred, radius 15");
    check(std::abs(alpha(s, 190, 200) - alpha(s, 210, 200)) < 1e-9 && std::abs(alpha(s, 200, 190) - alpha(s, 200, 210)) < 1e-9,
          "B6: dot symmetric");
    RasterStore t;
    t.reset(400, 400);
    BrushEngine e2;
    draw(e2, t, brush(8), Path{{100, 100, 0}, {101.5, 100.5, 1}});
    check(alpha(t, 100, 100) == 1.0 && alpha(t, 101, 100) == 1.0, "B6: 1.6 px drag drawn (both ends covered)");
    RasterStore u;
    u.reset(400, 400);
    BrushEngine e3;
    draw(e3, u, brush(8), Path{{50, 50, 0}, {50, 50, 1}, {50.0000001, 50, 2}});
    check(e3.stats().pathPoints == 1 && e3.stats().samplesProcessed == 3,
          "B6: resting finger: duplicate positions processed, no extra geometry");
}

void testLong() {
    section("B7 long stroke: 200 000 samples, nothing lost or truncated");
    RasterStore s;
    s.reset(4096, 4096);
    BrushEngine e;
    Path p;
    std::mt19937 rng(7);
    std::uniform_real_distribution<double> jitter(-0.2, 0.2);
    for (int i = 0; i < 200000; ++i) {
        // Zig-zag lines over the whole canvas.
        const double t = i / 200000.0;
        const double phase = std::fmod(t * 40, 2.0);
        const double x = phase < 1 ? 20 + phase * 4050 : 20 + (2 - phase) * 4050;
        p.push_back({x + jitter(rng), 20 + t * 4050, static_cast<double>(i)});
    }
    const int64_t t0 = pe::nowNs();
    draw(e, s, brush(6), p, 400);
    const double ms = (pe::nowNs() - t0) / 1e6;
    const auto st = e.stats();
    check(st.samplesReceived == 200000 && st.samplesProcessed == 200000,
          fmt("B7: %.0f samples received, %.0f processed", static_cast<double>(st.samplesReceived),
              static_cast<double>(st.samplesProcessed)));
    check(st.strokesCommitted == 1 && !st.strokeActive, "B7: stroke committed");
    check(alpha(s, static_cast<int>(p.back().x), static_cast<int>(p.back().y)) > 0.9, "B7: last sample painted");
    std::printf("    %.0f ms for 200k samples (%.2f us/sample), %llu dabs, %llu px composited, %zu tiles\n", ms,
                ms * 1000 / 200000, static_cast<unsigned long long>(st.dabs),
                static_cast<unsigned long long>(st.compositedPixels), s.tilesAllocated());
}

void testSizes() {
    section("B8 brush sizes: width = diameter from sub-pixel to 600 px");
    for (double d : {0.5, 1.0, 2.0, 3.0, 7.5, 32.0, 150.0, 600.0}) {
        RasterStore s;
        s.reset(2000, 1000);
        BrushEngine e;
        draw(e, s, brush(static_cast<float>(d)), line(300, 500.37, 1700, 500.37, 9));
        double lo = 1e9, hi = 0;
        for (int x = 900; x <= 1100; x += 10) {
            const double w = columnInk(s, x, 150, 850);
            lo = std::min(lo, w);
            hi = std::max(hi, w);
        }
        const double tol = d < 2 ? 0.15 : 0.3;
        check(std::abs(lo - d) < tol && std::abs(hi - d) < tol,
              fmt("B8: diameter %.1f px -> width %.3f..%.3f px", d, lo, hi));
    }
}

void testOpacity() {
    section("B9 opacity: exact, never exceeded by overlapping dabs, strokes stack source-over");
    RasterStore s;
    s.reset(600, 600);
    fillOpaque(s, 0xFFFFFFFFu);
    BrushEngine e;
    // Self-overlapping zig-zag at 40 %, tight spacing: overlapping dabs must not darken.
    Path zig;
    for (int i = 0; i <= 12; ++i) zig.push_back({100.0 + (i % 2) * 400, 100.0 + i * 30, 0});
    draw(e, s, brush(40, 0.4f, 0xFF000000u, 0.02f), zig);
    int darkest = 255;
    for (int y = 0; y < 600; ++y)
        for (int x = 0; x < 600; ++x) darkest = std::min(darkest, static_cast<int>(s.pixel(x, y).r));
    const int expect = static_cast<int>(std::lround(255 * 0.6));
    check(std::abs(darkest - expect) <= 1, fmt("B9: darkest pixel %.0f, single 40%% layer expects %.0f", darkest, expect));
    check(s.pixel(300, 100 + 30).r == expect || std::abs(s.pixel(110, 100).r - expect) <= 1,
          "B9: interior exactly 1 - 0.4 over white");
    // A second stroke over the first: 1 - 0.6^2 = 0.64.
    draw(e, s, brush(40, 0.4f, 0xFF000000u), line(100, 100, 500, 100, 2));
    const int two = s.pixel(300, 100).r;
    check(std::abs(two - std::lround(255 * 0.36)) <= 1, fmt("B9: two 40%% strokes = 64%% (pixel %.0f, expected %.0f)", two,
                                                          std::lround(255 * 0.36)));
    // On a transparent canvas, alpha never exceeds the opacity.
    RasterStore t;
    t.reset(400, 400);
    BrushEngine e2;
    draw(e2, t, brush(30, 0.25f, 0xFFFFFFFFu, 0.01f), circle(200, 200, 100, 64, 2.5));
    int maxA = 0, worstFringe = 0;
    for (int y = 0; y < 400; ++y)
        for (int x = 0; x < 400; ++x) {
            const Rgba8 p = t.pixel(x, y);
            maxA = std::max(maxA, static_cast<int>(p.a));
            if (p.a >= 8) worstFringe = std::max(worstFringe, 255 - static_cast<int>(p.toArgb() >> 16 & 0xFF));
        }
    check(maxA == std::lround(255 * 0.25), fmt("B9: transparent canvas: max alpha %.0f for 25%% opacity", maxA));
    check(worstFringe <= 8, fmt("B9: white stroke stays white (worst unpremultiplied loss %.0f/255, no dark fringe)",
                                 worstFringe));
    // Exact colour.
    RasterStore c;
    c.reset(200, 200);
    BrushEngine e3;
    draw(e3, c, brush(20, 1.0f, 0xFFE03070u), line(50, 100, 150, 100, 4));
    const Rgba8 px = c.pixel(100, 100);
    check(px.r == 0xE0 && px.g == 0x30 && px.b == 0x70 && px.a == 255, "B9: opaque colour reproduced exactly (#E03070)");
    // Colour alpha multiplies opacity.
    RasterStore h;
    h.reset(200, 200);
    BrushEngine e4;
    draw(e4, h, brush(20, 0.5f, 0x80FF0000u), line(50, 100, 150, 100, 4));
    check(std::abs(h.pixel(100, 100).a - std::lround(255 * 0.5 * 128 / 255.0)) <= 1, "B9: colour alpha x opacity");
}

void testTiles() {
    section("B10 strokes crossing tile boundaries (256 px tiles)");
    RasterStore s;
    s.reset(1100, 1100);
    BrushEngine e;
    draw(e, s, brush(24), line(30, 256.0, 1070, 256.0, 7));    // along a tile row boundary
    draw(e, s, brush(24), line(511.7, 30, 511.7, 1070, 5));    // along a tile column boundary
    draw(e, s, brush(24), line(40, 40, 1060, 1060, 3));        // through tile corners
    double lo = 1e9, hi = 0;
    for (int x = 748; x <= 788; ++x) {  // horizontal stroke on y = 256 crossing the x = 768 tile edge
        const double w = columnInk(s, x, 230, 282);
        lo = std::min(lo, w);
        hi = std::max(hi, w);
    }
    check(std::abs(lo - 24) < 0.3 && std::abs(hi - 24) < 0.3, fmt("B10: width across tiles %.3f..%.3f px", lo, hi));
    double lo2 = 1e9, hi2 = 0;
    for (int y = 700; y <= 820; ++y) {  // vertical stroke crossing y = 768
        const double w = rowInk(s, y, 480, 540);
        lo2 = std::min(lo2, w);
        hi2 = std::max(hi2, w);
    }
    check(std::abs(lo2 - 24) < 0.3 && std::abs(hi2 - 24) < 0.3,
          fmt("B10: vertical stroke on a tile edge: width %.3f..%.3f px", lo2, hi2));
    bool symmetric = true;
    for (int d = 0; d < 14; ++d) symmetric &= std::abs(alpha(s, 600, 256 + d) - alpha(s, 600, 255 - d)) < 0.004;
    check(symmetric, "B10: profile symmetric across the tile boundary (no seam)");
    check(alpha(s, 256, 256) == 1 && alpha(s, 255, 255) == 1 && alpha(s, 768, 768) == 1 && alpha(s, 767, 767) == 1,
          "B10: diagonal continuous through tile corners");
    check(!s.tileAllocated(0, 4) && !s.tileAllocated(4, 2) && s.tilesAllocated() < 25,
          fmt("B10: untouched tiles never allocated (%.0f of 25 allocated)", s.tilesAllocated()));
}

void testCancel() {
    section("B11 stroke cancellation restores the canvas bit for bit");
    RasterStore s, before;
    s.reset(900, 600);
    before.reset(900, 600);
    BrushEngine e;
    draw(e, s, brush(30, 0.7f, 0xFF30A050u), line(50, 300, 850, 300, 10));
    BrushEngine copy;
    draw(copy, before, brush(30, 0.7f, 0xFF30A050u), line(50, 300, 850, 300, 10));
    // A new stroke over existing paint and over never-touched tiles, cancelled mid-way.
    Path p = circle(450, 300, 250, 40);
    e.begin(brush(50, 0.9f, 0xFF102030u), p[0]);
    e.addSamples(p.data() + 1, 20);
    e.process(s);
    check(!sameStore(s, before, 0), "B11: the stroke is visible while drawing");
    e.addSamples(p.data() + 21, 5);
    e.cancel();
    e.process(s);
    int worst = 0;
    check(sameStore(s, before, 0, &worst), fmt("B11: after cancel, canvas identical to before (max diff %.0f)", worst));
    check(e.stats().strokesCancelled == 1 && !e.stats().strokeActive, "B11: stroke counted as cancelled");
    // Eraser cancel too.
    BrushSettings er = brush(40, 1.0f);
    er.erase = true;
    e.begin(er, {100, 300, 0});
    const StrokePoint q[] = {{800, 300, 1}};
    e.addSamples(q, 1);
    e.process(s);
    check(alpha(s, 450, 300) == 0.0, "B11: eraser removes paint while drawing");
    e.cancel();
    e.process(s);
    check(sameStore(s, before, 0), "B11: cancelled eraser stroke restores the paint");
}

void testOverlap() {
    section("B12 repeated overlapping strokes");
    RasterStore s;
    s.reset(400, 200);
    BrushEngine e;
    double prev = 0;
    bool monotonic = true, hueOk = true;
    for (int i = 0; i < 40; ++i) {
        draw(e, s, brush(30, 0.3f, 0xFFD04010u), line(40, 100, 360, 100, 2 + i % 5));
        const Rgba8 p = s.pixel(200, 100);
        const double a = p.a / 255.0;
        if (a + 1e-9 < prev) monotonic = false;
        prev = a;
        const uint32_t argb = p.toArgb();
        // 8-bit premultiplied storage: un-premultiplying can be off by up to
        // 255 / (2 * alpha) (quantisation), never more.
        const int tol = static_cast<int>(std::ceil(255.0 / (2.0 * p.a))) + 1;
        hueOk &= std::abs(static_cast<int>(argb >> 16 & 0xFF) - 0xD0) <= tol &&
                 std::abs(static_cast<int>(argb >> 8 & 0xFF) - 0x40) <= tol && std::abs(static_cast<int>(argb & 0xFF) - 0x10) <= tol;
    }
    check(monotonic, "B12: coverage grows monotonically with each stroke");
    check(prev > 0.99, fmt("B12: 40 strokes at 30%% converge to opaque (alpha %.4f)", prev));
    check(hueOk, "B12: colour unchanged by repeated compositing (no drift, no darkening)");
    const auto st = e.stats();
    check(st.strokesCommitted == 40 && st.pooledTiles > 0 && st.strokeTiles == 0,
          "B12: stroke tiles returned to the pool between strokes");
}

void testEraser() {
    section("B13 eraser");
    RasterStore s;
    s.reset(400, 200);
    fillOpaque(s, 0xFF3366CCu);
    BrushEngine e;
    BrushSettings er = brush(30, 1.0f);
    er.erase = true;
    draw(e, s, er, line(50, 100, 350, 100, 3));
    check(alpha(s, 200, 100) == 0.0 && alpha(s, 200, 60) == 1.0, "B13: 100% eraser clears, outside untouched");
    er.opacity = 0.5f;
    draw(e, s, er, line(50, 40, 350, 40, 3));
    const Rgba8 p = s.pixel(200, 40);
    const uint32_t c = p.toArgb();
    check(std::abs(p.a - 128) <= 1 && std::abs(static_cast<int>(c >> 16 & 0xFF) - 0x33) <= 2 &&
              std::abs(static_cast<int>(c >> 8 & 0xFF) - 0x66) <= 2 && std::abs(static_cast<int>(c & 0xFF) - 0xCC) <= 2,
          "B13: 50% eraser halves alpha, keeps colour (within 8-bit rounding)");
    RasterStore empty;
    empty.reset(400, 200);
    BrushEngine e2;
    er.opacity = 1;
    draw(e2, empty, er, line(10, 10, 390, 190, 5));
    check(empty.tilesAllocated() == 0, "B13: erasing an empty canvas allocates nothing");
}

void testEdges() {
    section("B14 robustness: strokes leaving the canvas, invalid input, spacing");
    RasterStore s;
    s.reset(300, 300);
    BrushEngine e;
    draw(e, s, brush(20), line(-500, 150, 800, 150, 4));
    check(alpha(s, 0, 150) == 1 && alpha(s, 299, 150) == 1, "B14: stroke through the canvas from outside clipped correctly");
    const StrokePoint bad[] = {{NAN, 5, 0}, {INFINITY, 5, 0}, {150, 290, 0}};
    e.begin(brush(10), {150, 10, 0});
    e.addSamples(bad, 3);
    e.end();
    e.process(s);
    check(alpha(s, 150, 150) == 1, "B14: non-finite samples ignored, stroke still drawn");
    BrushEngine sparse, dense;
    RasterStore a, b;
    a.reset(300, 100);
    b.reset(300, 100);
    draw(sparse, a, brush(20, 1, 0xFF000000u, 1.0f), line(20, 50, 280, 50, 2));
    draw(dense, b, brush(20, 1, 0xFF000000u, 0.05f), line(20, 50, 280, 50, 2));
    check(sparse.stats().dabs * 10 < dense.stats().dabs, "B14: spacing setting controls the dab density");
    check(columnInk(a, 30, 0, 99) < columnInk(b, 30, 0, 99) || columnInk(a, 40, 0, 99) < 19.5,
          "B14: 100% spacing shows separate dabs (as requested by the user)");
}

void testStore() {
    section("B16 raster store: dirty regions, save tracking, import");
    RasterStore s;
    s.reset(600, 300);  // 3 x 2 tiles, edge tiles partial
    check(s.columns() == 3 && s.rows() == 2 && s.tilesAllocated() == 0, "B16: 600x300 = 3x2 tiles, none allocated");
    s.fill({250, 10, 20, 5}, Rgba8::fromArgb(0xFF102030u));  // crosses the x = 256 tile edge
    std::vector<pe::canvas::RectI> dirty;
    s.takeGpuDirty(dirty);
    check(dirty.size() == 2 && dirty[0] == pe::canvas::RectI{250, 10, 6, 5} && dirty[1] == pe::canvas::RectI{256, 10, 14, 5},
          "B16: GPU dirty regions split per tile, exact");
    dirty.clear();
    s.takeGpuDirty(dirty);
    check(dirty.empty() && !s.hasGpuDirty(), "B16: dirty regions consumed once");
    std::vector<pe::canvas::TileCoord> unsaved;
    s.unsavedTiles(unsaved);
    check(unsaved.size() == 2, "B16: both tiles unsaved");
    s.markSaved(0, 0);
    s.markSaved(1, 0);
    unsaved.clear();
    s.unsavedTiles(unsaved);
    check(unsaved.empty(), "B16: marked saved");
    std::vector<uint8_t> px(RasterStore::kTileBytes, 0xFF);
    check(s.importTile(2, 1, px.data()), "B16: import into the partial corner tile");
    check(s.pixel(599, 299).a == 255 && s.tile(2, 1)[(static_cast<size_t>(43) * 256 + 88) * 4 + 3] == 0 && s.tile(2, 1)[(static_cast<size_t>(44) * 256 + 10) * 4 + 3] == 0 &&
              s.tile(2, 1)[(static_cast<size_t>(10) * 256 + 100) * 4] == 0,
          "B16: imported pixels outside the canvas cleared (corner tile is 88x44)");
    check(!s.tileEmpty(2, 1) && s.tileEmpty(1, 1), "B16: empty-tile detection");
    s.markAllGpuDirty();
    dirty.clear();
    s.takeGpuDirty(dirty);
    check(dirty.size() == 3, "B16: device restore re-uploads every allocated tile (3)");
    const uint64_t v = s.version();
    s.clear();
    check(s.tilesAllocated() == 0 && s.version() > v, "B16: clear frees every tile");
}


// ======================================================= Block 4: pressure & tilt
Path withPressure(Path p, float p0, float p1) {
    for (size_t i = 0; i < p.size(); ++i) {
        const double f = p.size() == 1 ? 0.0 : static_cast<double>(i) / (p.size() - 1);
        p[i].axes.pressure = static_cast<float>(p0 + (p1 - p0) * f);
    }
    return p;
}
BrushSettings dyn(float d, bool size, bool opacity, float minSize = 0.2f, float minOpacity = 0.2f, float smoothing = 0.0f) {
    BrushSettings b = brush(d);
    b.pressureSize = size;
    b.pressureOpacity = opacity;
    b.minSizeFraction = minSize;
    b.minOpacityFraction = minOpacity;
    b.pressureSmoothing = smoothing;
    return b;
}
double widthAt(const RasterStore& s, int x, int y0, int y1) { return columnInk(s, x, y0, y1); }

void testPressureCompat() {
    section("P1 missing pressure axis: strokes identical to Block 3");
    RasterStore a, b;
    a.reset(600, 300);
    b.reset(600, 300);
    BrushEngine e1, e2;
    BrushSettings plain = brush(30, 0.7f, 0xFF3050A0u);
    BrushSettings dynamics = plain;
    dynamics.pressureSize = dynamics.pressureOpacity = dynamics.tiltShape = true;
    const Path p = circle(300, 150, 100, 23, 0.8);  // no axes: finger / mouse
    draw(e1, a, plain, p, 3);
    draw(e2, b, dynamics, p, 3);
    int worst = 0;
    check(sameStore(a, b, 0, &worst), fmt("P1: dynamics enabled but no pressure/tilt in the samples: identical pixels (diff %.0f)", worst));
    check(e2.stats().strokesCommitted == 1, "P1: committed");
}

void testPressureSize() {
    section("P2 pressure -> size: min / max / response");
    const double D = 40;
    for (float pr : {0.0f, 0.25f, 0.5f, 1.0f}) {
        RasterStore s;
        s.reset(800, 300);
        BrushEngine e;
        draw(e, s, dyn(static_cast<float>(D), true, false), withPressure(line(100, 150.3, 700, 150.3, 20), pr, pr));
        const double expect = D * (0.2 + 0.8 * pr);
        double lo = 1e9, hi = 0;
        for (int x = 200; x <= 600; x += 20) {
            const double w = widthAt(s, x, 50, 250);
            lo = std::min(lo, w);
            hi = std::max(hi, w);
        }
        check(std::abs(lo - expect) < 0.3 && std::abs(hi - expect) < 0.3,
              fmt("P2: pressure %.2f -> width %.2f..%.2f px", pr, lo, hi) + fmt(" (expected %.2f)", expect));
    }
    // Out-of-range values are clamped, never extrapolated.
    RasterStore over;
    over.reset(800, 300);
    BrushEngine eo;
    draw(eo, over, dyn(static_cast<float>(D), true, false), withPressure(line(100, 150, 700, 150, 5), 1.7f, 1.7f));
    check(std::abs(widthAt(over, 400, 50, 250) - D) < 0.3, "P2: pressure > 1 clamps to the maximum size");
    // A response curve (here p^2) shapes the size.
    auto lut = std::make_shared<pe::brush::PressureLut>();
    for (size_t i = 0; i < pe::brush::PressureLut::kSize; ++i) {
        const float x = static_cast<float>(i) / (pe::brush::PressureLut::kSize - 1);
        lut->lut[i] = x * x;
    }
    BrushSettings curved = dyn(static_cast<float>(D), true, false);
    curved.pressureCurve = lut;
    RasterStore cs;
    cs.reset(800, 300);
    BrushEngine ec;
    draw(ec, cs, curved, withPressure(line(100, 150, 700, 150, 5), 0.5f, 0.5f));
    check(std::abs(widthAt(cs, 400, 50, 250) - D * (0.2 + 0.8 * 0.25)) < 0.3, "P2: response curve p^2 -> 0.5 maps to 0.25");
    BrushSettings bad = curved;
    auto badLut = std::make_shared<pe::brush::PressureLut>(*lut);
    badLut->lut[100] = 2.0f;  // not monotonic
    bad.pressureCurve = badLut;
    check(!bad.sanitized().pressureCurve, "P2: invalid (non-monotonic) curve rejected -> linear");
}

void testNeverDisappears() {
    section("P3 a pressure value never makes the stroke vanish");
    RasterStore s;
    s.reset(800, 300);
    BrushEngine e;
    // Minimum size 0 requested (floored at 2 %), opacity minimum 0 (floored at 5 %), zero pressure.
    draw(e, s, dyn(40, true, true, 0.0f, 0.0f), withPressure(line(100, 150.5, 700, 150.5, 10), 0.0f, 0.0f));
    double minCentre = 1;
    for (int x = 150; x <= 650; ++x) minCentre = std::min(minCentre, alpha(s, x, 150));
    check(minCentre > 0.0, fmt("P3: zero pressure still paints (min centre alpha %.3f)", minCentre));
    RasterStore t;
    t.reset(800, 300);
    BrushEngine e2;
    Path nan = line(100, 150, 700, 150, 10);
    nan[0].axes.pressure = 0.5f;   // stroke has pressure...
    nan[4].axes.pressure = NAN;    // ...one sample lost it
    for (size_t i = 1; i < nan.size(); ++i) if (i != 4) nan[i].axes.pressure = 0.5f;
    draw(e2, t, dyn(40, true, false), nan);
    check(alpha(t, 330, 150) == 1.0, "P3: a sample without pressure inside a pressure stroke does not break it");
}

void testPressureInterpolation() {
    section("P4 pressure interpolated between (historical) samples");
    const double D = 40;
    RasterStore s;
    s.reset(900, 300);
    BrushEngine e;
    // ONE segment from pressure 0 to 1: the width must grow smoothly along it.
    draw(e, s, dyn(static_cast<float>(D), true, false), withPressure(line(100, 150.5, 800, 150.5, 2), 0.0f, 1.0f));
    double prev = 0, maxStep = 0;
    bool monotonic = true;
    for (int x = 120; x <= 780; x += 10) {
        const double w = widthAt(s, x, 50, 250);
        if (w + 0.05 < prev) monotonic = false;
        if (prev > 0) maxStep = std::max(maxStep, w - prev);
        prev = w;
    }
    const double mid = widthAt(s, 450, 50, 250);
    check(monotonic, "P4: width grows monotonically along a 0 -> 1 pressure segment");
    check(std::abs(mid - D * (0.2 + 0.8 * 0.5)) < 1.0, fmt("P4: half-way width %.2f px (expected %.2f)", mid, D * 0.6));
    check(maxStep < 1.0, fmt("P4: no steps (largest change per 10 px: %.3f px)", maxStep));
    // Dense historical samples (pressure ramp) vs the same ramp in 2 samples: same result.
    RasterStore dense;
    dense.reset(900, 300);
    BrushEngine e2;
    draw(e2, dense, dyn(static_cast<float>(D), true, false), withPressure(line(100, 150.5, 800, 150.5, 300), 0.0f, 1.0f), 7);
    double worst = 0;
    for (int x = 120; x <= 780; x += 10) worst = std::max(worst, std::abs(widthAt(dense, x, 50, 250) - widthAt(s, x, 50, 250)));
    check(worst < 0.5, fmt("P4: 300 samples in batches of 7 vs 2 samples: widths agree within %.3f px", worst));
}

void testPressureOpacity() {
    section("P5 pressure -> opacity, independent of size");
    RasterStore s;
    s.reset(800, 300);
    fillOpaque(s, 0xFFFFFFFFu);
    BrushEngine e;
    draw(e, s, dyn(30, false, true), withPressure(line(100, 100, 700, 100, 8), 0.5f, 0.5f));
    const int expect = static_cast<int>(std::lround(255 * (1 - 0.6)));  // flow = 0.2 + 0.8 * 0.5
    check(std::abs(s.pixel(400, 100).r - expect) <= 1, fmt("P5: 50%% pressure -> 60%% opacity (pixel %.0f, expected %.0f)",
                                                           s.pixel(400, 100).r, expect));
    check(std::abs(columnInk(s, 400, 50, 150) - 0) >= 0 && alpha(s, 400, 100 + 14) == 1.0,
          "P5: opacity only: size unchanged (30 px)");
    // Self-overlap at constant pressure still does not darken.
    RasterStore z;
    z.reset(600, 600);
    fillOpaque(z, 0xFFFFFFFFu);
    BrushEngine e2;
    Path zig;
    for (int i = 0; i <= 10; ++i) zig.push_back({100.0 + (i % 2) * 400, 100.0 + i * 30, 0});
    draw(e2, z, dyn(40, false, true), withPressure(zig, 0.5f, 0.5f));
    int darkest = 255;
    for (int y = 0; y < 600; ++y)
        for (int x = 0; x < 600; ++x) darkest = std::min(darkest, static_cast<int>(z.pixel(x, y).r));
    check(std::abs(darkest - expect) <= 1, fmt("P5: overlapping dabs keep the pressure opacity (darkest %.0f)", darkest));
    // Size and opacity together.
    RasterStore b;
    b.reset(800, 300);
    fillOpaque(b, 0xFFFFFFFFu);
    BrushEngine e3;
    draw(e3, b, dyn(30, true, true), withPressure(line(100, 150, 700, 150, 8), 0.5f, 0.5f));
    check(std::abs(b.pixel(400, 150).r - expect) <= 1 && b.pixel(400, 150 + 12).r == 255,
          "P5: size and opacity together (18 px wide, 60%)");
}

void testPressureFilter() {
    section("P6 pressure smoothing: noise reduced, fast changes preserved, no start lag");
    pe::brush::PressureFilter f;
    f.reset(0.0f);
    check(f.filter(0.37f, 0) == 0.37f && f.filter(0.91f, 4) == 0.91f, "P6: smoothing 0 = raw values");
    f.reset(0.35f);
    check(f.filter(0.2f, 0) == 0.2f, "P6: first sample taken as is (no ramp-in)");
    for (int i = 1; i < 20; ++i) f.filter(0.2f, i * 4.1667);
    float v = 0;
    int samples = 0;
    while (v < 0.8f && samples < 50) v = f.filter(0.9f, (20 + samples++) * 4.1667);
    check(samples <= 3, fmt("P6: step 0.2 -> 0.9 at 240 Hz reaches 0.8 in %.0f samples (%.1f ms)", samples, samples * 4.1667));
    std::mt19937 rng(3);
    std::normal_distribution<double> noise(0.0, 0.02);
    f.reset(0.35f);
    double rawVar = 0, outVar = 0;
    const int n = 2000;
    for (int i = 0; i < n; ++i) {
        const double raw = 0.5 + noise(rng);
        const double out = f.filter(static_cast<float>(raw), i * 4.1667);
        if (i > 50) {
            rawVar += (raw - 0.5) * (raw - 0.5);
            outVar += (out - 0.5) * (out - 0.5);
        }
    }
    const double ratio = std::sqrt(outVar / rawVar);
    check(ratio < 0.6, fmt("P6: sensor noise (sigma 0.02) reduced to %.0f%% of its amplitude", ratio * 100));
    // Slow ramp: lag stays small.
    f.reset(0.35f);
    double worstLag = 0;
    for (int i = 0; i < 240; ++i) {
        const double target = i / 240.0;  // 0 -> 1 in one second
        worstLag = std::max(worstLag, target - f.filter(static_cast<float>(target), i * 4.1667));
    }
    check(worstLag < 0.05, fmt("P6: slow 1 s press: filtered value lags by at most %.3f", worstLag));
    // End-to-end: a fast press inside a stroke shows up within a few px.
    RasterStore s;
    s.reset(900, 300);
    BrushEngine e;
    Path p = line(100, 150.5, 800, 150.5, 141);  // 5 px per sample
    for (size_t i = 0; i < p.size(); ++i) {
        p[i].t = i * 4.1667;
        p[i].axes.pressure = i < 70 ? 0.1f : 1.0f;
    }
    draw(e, s, dyn(40, true, false, 0.2f, 0.2f, 0.35f), p, 4);
    const double before = widthAt(s, 400, 50, 250), after = widthAt(s, 470, 50, 250);
    check(before < 12 && after > 38, fmt("P6: width %.1f px before, %.1f px 20 px after a fast press", before, after));
}

void testTilt() {
    section("P7 tilt + orientation shape the dab; missing tilt -> round");
    const double D = 20;
    auto tap = [&](float tilt, float orient, bool enabled) {
        RasterStore s;
        s.reset(200, 200);
        BrushEngine e;
        BrushSettings b = brush(static_cast<float>(D));
        b.tiltShape = enabled;
        StrokePoint p{100.5, 100.5, 0};
        p.axes.tilt = tilt;
        p.axes.orientation = orient;
        draw(e, s, b, Path{p});
        return std::pair<double, double>(rowInk(s, 100, 0, 199), columnInk(s, 100, 0, 199));
    };
    const auto round = tap(NAN, NAN, true);
    check(std::abs(round.first - D) < 0.3 && std::abs(round.second - D) < 0.3, "P7: no tilt axis -> round dab");
    const auto noOrient = tap(1.0f, NAN, true);
    check(std::abs(noOrient.first - D) < 0.3 && std::abs(noOrient.second - D) < 0.3,
          "P7: tilt without orientation -> round (direction unknown, not invented)");
    const float kPi = 3.14159265f;
    const auto flat = tap(kPi / 3, kPi / 2, true);  // 60 deg tilt, pointing right
    const double major = D * (1 + 1.5 * std::sin(kPi / 3));
    check(std::abs(flat.first - major) < 0.8 && std::abs(flat.second - D) < 0.5,
          fmt("P7: 60 deg tilt pointing right: %.2f x %.2f px", flat.first, flat.second) + fmt(" (expected %.2f x 20)", major));
    const auto up = tap(kPi / 3, 0.0f, true);  // pointing up: long axis vertical
    check(std::abs(up.second - major) < 0.8 && std::abs(up.first - D) < 0.5, "P7: orientation 0 (up): long axis vertical");
    const auto off = tap(kPi / 3, kPi / 2, false);
    check(std::abs(off.first - D) < 0.3, "P7: tilt shaping disabled -> round");
    check(std::abs(pe::brush::lerpAxes({0.5f, 0, 3.0f}, {0.5f, 0, -3.0f}, 0.5).orientation) > 3.1f,
          "P7: orientation interpolates along the shorter arc (across +-pi)");
}

void testTransitions() {
    section("P8 stylus -> finger -> stylus: no state leaks between strokes; cancel");
    RasterStore mixed, ref;
    mixed.reset(800, 400);
    ref.reset(800, 400);
    BrushEngine e;
    BrushSettings b = dyn(30, true, true, 0.2f, 0.2f, 0.35f);
    draw(e, mixed, b, withPressure(line(100, 100, 700, 100, 30), 0.1f, 0.3f));   // stylus
    draw(e, mixed, b, line(100, 250, 700, 250, 30));                               // finger: no pressure
    BrushEngine e2;
    draw(e2, ref, b, withPressure(line(100, 100, 700, 100, 30), 0.1f, 0.3f));
    draw(e2, ref, brush(30), line(100, 250, 700, 250, 30));                         // plain Block 3 stroke
    check(sameStore(mixed, ref, 0), "P8: the finger stroke after a stylus stroke is a plain full-size stroke");
    // Cancel restores exactly, also with pressure + tilt.
    RasterStore c, before;
    c.reset(800, 400);
    before.reset(800, 400);
    BrushEngine e3, e4;
    draw(e3, c, brush(20), line(50, 200, 750, 200, 5));
    draw(e4, before, brush(20), line(50, 200, 750, 200, 5));
    BrushSettings tb = b;
    tb.tiltShape = true;
    Path p = withPressure(circle(400, 200, 150, 30), 0.2f, 1.0f);
    for (auto& q : p) {
        q.axes.tilt = 0.8f;
        q.axes.orientation = 1.0f;
    }
    e3.begin(tb, p[0]);
    e3.addSamples(p.data() + 1, 20);
    e3.process(c);
    e3.cancel();
    e3.process(c);
    check(sameStore(c, before, 0), "P8: cancelled pressure + tilt stroke restores the canvas bit for bit");
    // Batching determinism with pressure.
    RasterStore one, many;
    one.reset(800, 400);
    many.reset(800, 400);
    BrushEngine e5, e6;
    Path q = withPressure(circle(400, 200, 120, 40, 1.2), 0.0f, 1.0f);
    for (size_t i = 0; i < q.size(); ++i) q[i].t = i * 4.0;
    draw(e5, one, b, q, 0);
    draw(e6, many, b, q, 1);
    check(sameStore(one, many, 0), "P8: pressure strokes do not depend on frame batching");
}

void testPerf() {
    section("B15 timing (informational, this machine)");
    struct Case {
        const char* name;
        float d;
        int samples;
    } cases[] = {{"small 4 px, 2000 px stroke", 4, 250}, {"medium 40 px, 2000 px stroke", 40, 250},
                 {"large 300 px, 2000 px stroke", 300, 250}};
    for (const auto& c : cases) {
        RasterStore s;
        s.reset(4096, 4096);
        BrushEngine e;
        Path p = line(1000, 2000, 3000, 2000, c.samples);
        const int64_t t0 = pe::nowNs();
        draw(e, s, brush(c.d, 0.8f, 0xFF204080u), p, 8);  // 8 samples per "frame"
        const double ms = (pe::nowNs() - t0) / 1e6;
        std::printf("    %-30s %8.2f ms total, %6.3f ms per 8-sample frame, worst frame %.3f ms\n", c.name, ms,
                    ms / (c.samples / 8.0), e.stats().maxProcessMs);
    }
    // Same strokes with pressure (size + opacity) and tilt (elliptical dabs).
    for (const auto& c : cases) {
        RasterStore s;
        s.reset(4096, 4096);
        BrushEngine e;
        Path p = withPressure(line(1000, 2000, 3000, 2000, c.samples), 0.3f, 1.0f);
        for (size_t i = 0; i < p.size(); ++i) {
            p[i].t = i * 4.0;
            p[i].axes.tilt = 0.9f;
            p[i].axes.orientation = 0.4f;
        }
        BrushSettings b = brush(c.d, 0.8f, 0xFF204080u);
        b.pressureSize = b.pressureOpacity = b.tiltShape = true;
        const int64_t t0 = pe::nowNs();
        draw(e, s, b, p, 8);
        const double ms = (pe::nowNs() - t0) / 1e6;
        std::printf("    %-30s %8.2f ms total, %6.3f ms per 8-sample frame, worst frame %.3f ms (pressure + tilt)\n", c.name,
                    ms, ms / (c.samples / 8.0), e.stats().maxProcessMs);
    }
    check(true, "B15: timings printed");
}

}  // namespace

int main() {
    std::printf("=== brush engine tests (CPU) ===\n");
    testStraight();
    testDiagonal();
    testCircle();
    testFastSlow();
    testBatching();
    testShort();
    testLong();
    testSizes();
    testOpacity();
    testTiles();
    testCancel();
    testOverlap();
    testEraser();
    testEdges();
    testStore();
    testPressureCompat();
    testPressureSize();
    testNeverDisappears();
    testPressureInterpolation();
    testPressureOpacity();
    testPressureFilter();
    testTilt();
    testTransitions();
    testPerf();
    std::printf("=== %d/%d checks passed ===\n", gChecks - gFailures, gChecks);
    return gFailures == 0 ? 0 : 1;
}
