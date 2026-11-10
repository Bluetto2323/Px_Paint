// PxPaint - selection engine tests (pure CPU, Block 10).
//
// Shapes, combine modes, inversion, antialiased edges, tile boundaries, the
// mask encoding, clipped strokes and erasing, clear / copy / cut / paste /
// duplicate, moving selected content, undo / redo of pixels AND selection,
// big canvases, memory and latency. "Unchanged outside" is checked pixel by
// pixel (every pixel of the canvas, colour and alpha, bit for bit). The GPU
// side (what is displayed on Vulkan and GLES) is host_test --suite brush (S14).
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <functional>
#include <random>
#include <string>
#include <vector>

#include "canvas/SelectionMask.h"
#include "core/Time.h"
#include "engine/CanvasDocument.h"
#include "engine/History.h"

using pe::CanvasDocument;
using pe::HistoryKind;
using pe::LayerError;
using pe::canvas::RectI;
using pe::canvas::Rgba8;
using pe::canvas::SelectionMask;
using Mode = SelectionMask::Mode;

namespace {

int gChecks = 0, gFailures = 0;
void check(bool ok, const std::string& what) {
    gChecks++;
    if (!ok) gFailures++;
    std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what.c_str());
    std::fflush(stdout);
}
void section(const char* s) { std::printf("-- %s\n", s); }
std::string fmt(const char* f, double a, double b = 0, double c = 0, double d = 0) {
    char buf[320];
    std::snprintf(buf, sizeof(buf), f, a, b, c, d);
    return buf;
}
double ms(int64_t t0) { return (pe::nowNs() - t0) / 1e6; }

std::vector<float> rect(double x0, double y0, double x1, double y1) {
    std::vector<float> p;
    SelectionMask::shapePolygon(false, x0, y0, x1, y1, p);
    return p;
}
std::vector<float> ellipse(double x0, double y0, double x1, double y1) {
    std::vector<float> p;
    SelectionMask::shapePolygon(true, x0, y0, x1, y1, p);
    return p;
}
LayerError sel(CanvasDocument& d, const std::vector<float>& p, Mode m = Mode::Replace, bool aa = true) {
    return d.select(p.data(), p.size() / 2, m, aa);
}

pe::brush::BrushSettings brush(float dia, uint32_t argb, float opacity = 1.0f, bool erase = false) {
    pe::brush::BrushSettings b;
    b.diameter = dia;
    b.argb = argb;
    b.opacity = opacity;
    b.erase = erase;
    return b;
}
void stroke(CanvasDocument& d, const pe::brush::BrushSettings& b, double x0, double y0, double x1, double y1, int n = 40) {
    std::vector<pe::brush::StrokePoint> p;
    for (int i = 0; i < n; ++i) {
        const double t = static_cast<double>(i) / (n - 1);
        p.push_back({x0 + (x1 - x0) * t, y0 + (y1 - y0) * t + 12 * std::sin(t * 6.28), i * 4.0});
    }
    d.strokeBegin(b, p[0]);
    d.strokeSamples(p.data() + 1, p.size() - 1);
    d.strokeEnd();
}
CanvasDocument& base(CanvasDocument& d, uint32_t w = 900, uint32_t h = 700) {
    d.open(w, h);
    d.setBackground(0xFFF3EBDDu, false);
    return d;
}
// Every pixel of a layer (premultiplied RGBA as stored).
std::vector<Rgba8> grab(CanvasDocument& d, uint32_t id, uint32_t w, uint32_t h) {
    std::vector<Rgba8> v(static_cast<size_t>(w) * h);
    for (uint32_t y = 0; y < h; ++y)
        for (uint32_t x = 0; x < w; ++x) v[y * w + x] = d.layerPixel(id, static_cast<int32_t>(x), static_cast<int32_t>(y));
    return v;
}
// Pixels where the selection is 0 identical; counts of changed pixels inside.
struct Diff {
    size_t outsideChanged = 0, insideChanged = 0, edgeChanged = 0;
};
Diff diff(CanvasDocument& d, const std::vector<Rgba8>& a, const std::vector<Rgba8>& b, uint32_t w, uint32_t h) {
    Diff r;
    for (uint32_t y = 0; y < h; ++y)
        for (uint32_t x = 0; x < w; ++x) {
            const size_t i = static_cast<size_t>(y) * w + x;
            if (a[i] == b[i]) continue;
            const uint8_t s = d.selectionAt(static_cast<int32_t>(x), static_cast<int32_t>(y));
            if (s == 0) r.outsideChanged++;
            else if (s == 255) r.insideChanged++;
            else r.edgeChanged++;
        }
    return r;
}

struct Snapshot {
    std::vector<uint32_t> ids;
    std::vector<uint64_t> pixels;
    uint64_t composite = 0, selection = 0;
    uint32_t active = 0;
    bool operator==(const Snapshot& o) const {
        return ids == o.ids && pixels == o.pixels && composite == o.composite && selection == o.selection && active == o.active;
    }
};
Snapshot snap(CanvasDocument& d) {
    Snapshot s;
    for (const auto& l : d.layers(s.active)) {
        s.ids.push_back(l.info.id);
        s.pixels.push_back(d.layerChecksum(l.info.id));
    }
    s.composite = d.compositeChecksum();
    s.selection = d.selectionChecksum();
    return s;
}

// ---------------------------------------------------------------- mask level
void testShapes() {
    section("S1 shapes: rectangle, ellipse, lasso, polygon; exact areas and antialiased edges");
    SelectionMask m;
    m.reset(1000, 800);
    auto r = rect(100.2, 50.4, 400.4, 250.6);  // rounds to 100,50 - 400,251
    m.setPolygon(r.data(), r.size() / 2, true);
    m.setActive(true);
    check(std::fabs(m.area() - 300.0 * 201.0) < 1e-6, fmt("S1: rectangle: area exactly 300 x 201 px (%.1f)", m.area()));
    bool binary = true;
    for (int y = 40; y < 260; ++y)
        for (int x = 90; x < 410; ++x) {
            const uint8_t v = m.at(x, y);
            binary &= v == 0 || v == 255;
        }
    check(binary && m.at(100, 50) == 255 && m.at(99, 50) == 0 && m.at(399, 250) == 255 && m.at(400, 250) == 0,
          "S1: rectangle on whole pixels: crisp edges (only 0 / 255), inclusive-exclusive bounds");
    RectI b;
    check(m.bounds(b) && b.x == 100 && b.y == 50 && b.w == 300 && b.h == 201, "S1: rectangle bounds exact");

    auto e = ellipse(200, 100, 800, 500);  // rx 300, ry 200
    m.setPolygon(e.data(), e.size() / 2, true);
    const double exact = 3.14159265358979 * 300 * 200;
    check(std::fabs(m.area() - exact) / exact < 0.0005, fmt("S1: ellipse area %.0f vs pi*a*b %.0f (< 0.05 %%)", m.area(), exact));
    // Along an oblique part of the edge the coverage takes intermediate values.
    int oblique = 0;
    for (int y = 120; y < 480; ++y)
        for (int x = 200; x < 500; ++x) {
            const uint8_t v = m.at(x, y);
            oblique += v > 0 && v < 255;
        }
    check(oblique > 300, fmt("S1: ellipse antialiased: %.0f partially covered edge pixels (left half)", oblique));
    m.setPolygon(e.data(), e.size() / 2, false);
    int nonBinary = 0;
    for (int y = 0; y < 800; ++y)
        for (int x = 0; x < 1000; ++x) {
            const uint8_t v = m.at(x, y);
            nonBinary += v != 0 && v != 255;
        }
    check(nonBinary == 0 && std::fabs(m.area() - exact) / exact < 0.005, "S1: ellipse without antialiasing: only 0 / 255, same area (< 0.5 %)");

    // Lasso: a self-crossing freehand star (nonzero winding: the centre stays selected).
    std::vector<float> star;
    for (int i = 0; i < 5; ++i) {
        const double a = -1.5708 + i * 2 * 2 * 3.14159265358979 / 5;
        star.push_back(static_cast<float>(500 + 300 * std::cos(a)));
        star.push_back(static_cast<float>(400 + 300 * std::sin(a)));
    }
    m.setPolygon(star.data(), 5, true);
    check(m.at(500, 400) == 255 && m.at(500, 130) == 255 && m.at(150, 100) == 0, "S1: self-crossing lasso: the inner pentagon stays selected (nonzero)");
    // Freehand lasso of 4000 jittered points around a circle.
    std::mt19937 rng(3);
    std::uniform_real_distribution<double> j(-1.5, 1.5);
    std::vector<float> lasso;
    for (int i = 0; i < 4000; ++i) {
        const double a = 2 * 3.14159265358979 * i / 4000;
        lasso.push_back(static_cast<float>(500 + 250 * std::cos(a) + j(rng)));
        lasso.push_back(static_cast<float>(400 + 250 * std::sin(a) + j(rng)));
    }
    const int64_t t0 = pe::nowNs();
    m.setPolygon(lasso.data(), 4000, true);
    const double lassoMs = ms(t0);
    const double circle = 3.14159265358979 * 250 * 250;
    check(std::fabs(m.area() - circle) / circle < 0.01, fmt("S1: 4000-point lasso: area within 1 %% of the circle (%.1f ms)", lassoMs));
    // Polygon: a triangle.
    const std::vector<float> tri = {100, 700, 500, 100, 900, 700};
    m.setPolygon(tri.data(), 3, true);
    check(std::fabs(m.area() - 0.5 * 800 * 600) < 2.0, fmt("S1: triangle polygon: area %.1f (exact 240000)", m.area()));
    // Degenerate: a line or a point selects nothing.
    const std::vector<float> line = {10, 10, 500, 10, 900, 10};
    m.setPolygon(line.data(), 3, true);
    check(m.area() == 0.0, "S1: a zero-area polygon selects nothing");
}

void testModes() {
    section("S2 replace, add, subtract, intersect, invert, select all, deselect");
    SelectionMask cur, sh;
    cur.reset(1000, 800);
    sh.reset(1000, 800);
    auto put = [&](const std::vector<float>& p, Mode mode) {
        sh.setPolygon(p.data(), p.size() / 2, true);
        cur.combine(sh, mode);
    };
    put(rect(0, 0, 400, 400), Mode::Subtract);
    check(!cur.active(), "S2: subtract with no selection: still nothing selected");
    put(rect(0, 0, 400, 400), Mode::Intersect);
    check(!cur.active(), "S2: intersect with no selection: nothing");
    put(rect(100, 100, 300, 300), Mode::Add);
    check(cur.active() && cur.area() == 40000.0, "S2: add with no selection = the shape");
    put(rect(500, 100, 700, 300), Mode::Add);
    check(cur.area() == 80000.0, "S2: add a disjoint rectangle: areas add up");
    put(rect(200, 100, 600, 300), Mode::Add);
    check(cur.area() == 120000.0, "S2: add an overlapping rectangle: union (overlap counted once)");
    put(rect(250, 0, 550, 800), Mode::Subtract);
    check(cur.area() == 120000.0 - 300.0 * 200.0 && cur.at(260, 200) == 0 && cur.at(240, 200) == 255,
          "S2: subtract: the band is removed, the rest kept");
    put(rect(0, 150, 1000, 250), Mode::Intersect);
    check(cur.area() == 60000.0 / 2 && cur.at(150, 120) == 0 && cur.at(150, 200) == 255, "S2: intersect: only the common part");
    put(rect(10, 10, 20, 20), Mode::Replace);
    check(cur.area() == 100.0, "S2: replace discards the previous selection");
    const uint64_t one = cur.checksum();
    cur.invert();
    check(cur.area() == 1000.0 * 800 - 100 && cur.at(15, 15) == 0 && cur.at(500, 500) == 255, "S2: invert: complement of the selection");
    cur.invert();
    check(cur.checksum() == one, "S2: inverting twice gives back the same mask");
    put(ellipse(100, 100, 700, 500), Mode::Replace);
    const double ea = cur.area();
    cur.invert();
    check(std::fabs(cur.area() - (800000.0 - ea)) < 0.5, "S2: inverted antialiased ellipse: area = canvas - ellipse (edges complement)");
    put(rect(0, 0, 1000, 800), Mode::Intersect);
    cur.selectAll();
    check(cur.area() == 800000.0 && cur.bytes() == 0, "S2: select all: whole canvas, no pixel memory (full tiles)");
    cur.invert();
    check(!cur.active(), "S2: inverting everything = nothing selected");
    cur.selectAll();
    cur.clear();
    check(!cur.active() && cur.at(5, 5) == 255, "S2: deselect: inactive (every pixel editable again)");
    put(rect(0, 0, 100, 100), Mode::Replace);
    put(rect(0, 0, 100, 100), Mode::Subtract);
    check(!cur.active(), "S2: subtracting everything selected = no selection");
}

void testTilesAndEncoding() {
    section("S3 tile boundaries, sparse storage, outline, encoding, translate");
    SelectionMask m;
    m.reset(3000, 2000);
    auto r = rect(250, 250, 1030, 770);  // crosses tile edges 256, 512, 768, 1024
    m.setPolygon(r.data(), 4, true);
    m.setActive(true);
    check(m.area() == 780.0 * 520.0, "S3: rectangle across 4 x 3 tile edges: exact area");
    bool edges = m.at(255, 300) == 255 && m.at(256, 300) == 255 && m.at(249, 300) == 0 && m.at(1029, 769) == 255 &&
                 m.at(1030, 769) == 0 && m.at(1023, 767) == 255 && m.at(1024, 768) == 255;
    check(edges, "S3: no seam at tile edges (pixels on both sides of x 256/1024, y 768)");
    check(m.state(2, 1) == SelectionMask::TileState::Full && m.state(5, 5) == SelectionMask::TileState::Empty,
          "S3: inner tiles FULL, outer tiles EMPTY (no memory)");
    check(m.bytes() == 14 * 65536, fmt("S3: only the 14 tiles crossed by an edge hold pixels: %.0f KiB", m.bytes() / 1024.0));
    std::vector<float> c;
    m.contours(c);
    check(c.size() == 2 + 8 && c[0] == 1 && c[1] == 4, "S3: outline of a rectangle: one loop of 4 corners (collinear points merged)");
    // Two separate shapes plus a hole: three loops.
    SelectionMask sh;
    sh.reset(3000, 2000);
    auto hole = rect(400, 400, 600, 600);
    sh.setPolygon(hole.data(), 4, true);
    m.combine(sh, Mode::Subtract);
    auto far = ellipse(2000, 1000, 2600, 1800);
    sh.setPolygon(far.data(), far.size() / 2, true);
    m.combine(sh, Mode::Add);
    m.contours(c);
    check(c.size() > 2 && c[0] == 3, fmt("S3: rectangle with a hole + an ellipse: %.0f outline loops (3 expected)", c.empty() ? 0 : c[0]));
    // Encoding round trip.
    std::vector<uint8_t> enc;
    m.encode(enc);
    SelectionMask back;
    back.reset(3000, 2000);
    check(back.decode(enc.data(), enc.size()) && back.checksum() == m.checksum() && back.active(),
          fmt("S3: encode / decode identical (%.1f KiB for a %.0f KiB mask)", enc.size() / 1024.0, m.bytes() / 1024.0));
    SelectionMask other;
    other.reset(100, 100);
    check(!other.decode(enc.data(), enc.size()) && !other.decode(enc.data(), enc.size() / 2), "S3: decode refuses another size / truncated data");
    // Translate by whole pixels; what leaves the canvas is dropped.
    SelectionMask t;
    t.reset(3000, 2000);
    auto small = rect(100, 100, 300, 200);
    t.setPolygon(small.data(), 4, true);
    t.setActive(true);
    t.translate(57, -20);
    check(t.area() == 200.0 * 100 && t.at(157, 80) == 255 && t.at(156, 80) == 0 && t.at(356, 179) == 255,
          "S3: translate (57, -20): same area at the new place (across a tile edge)");
    t.translate(-400, 0);
    check(!t.active() || t.area() == 0.0, "S3: moved off the canvas: nothing left selected");
    // Preview (for dimming): inverse coverage, downsampled.
    uint32_t pw = 0, ph = 0;
    std::vector<uint8_t> pv;
    m.preview(300, pw, ph, pv);
    check(pw == 300 && ph == 200 && pv.size() == 60000 && pv[static_cast<size_t>(60) * 300 + 70] < 10 && pv[5] == 255,
          "S3: preview 300 x 200: selected = 0, unselected = 255");
}

// ---------------------------------------------------------------- document level
void testClippedPainting() {
    section("S4 strokes and erasing clipped to the selection; every pixel outside unchanged");
    CanvasDocument d;
    base(d);
    d.fill({0, 0, 900, 700}, 0x80305070u);  // semi-transparent content everywhere
    const uint32_t W = 900, H = 700;
    check(sel(d, rect(200, 150, 600, 450)) == LayerError::None, "S4: rectangle selected");
    auto before = grab(d, 1, W, H);
    stroke(d, brush(60, 0xFFE04020u), 50, 300, 850, 300);
    auto after = grab(d, 1, W, H);
    Diff df = diff(d, before, after, W, H);
    check(df.outsideChanged == 0 && df.insideChanged > 5000, fmt("S4: stroke across the canvas: %.0f px changed inside, %.0f outside", df.insideChanged, df.outsideChanged));
    sel(d, ellipse(100, 100, 800, 600));
    before = grab(d, 1, W, H);
    stroke(d, brush(80, 0xFF000000u, 1.0f, true), 30, 350, 870, 350);  // eraser
    after = grab(d, 1, W, H);
    df = diff(d, before, after, W, H);
    check(df.outsideChanged == 0 && df.insideChanged > 10000 && df.edgeChanged > 0,
          fmt("S4: eraser in an ellipse: %.0f inside, %.0f on the antialiased edge, %.0f outside", df.insideChanged, df.edgeChanged, df.outsideChanged));
    // Antialiased edge: where the stroke fully covers an edge pixel, alpha becomes a * (1 - s).
    int partialPx = 0, exactPx = 0;
    for (uint32_t y = 280; y < 420; ++y)
        for (uint32_t x = 0; x < W; ++x) {
            const uint8_t s = d.selectionAt(static_cast<int32_t>(x), static_cast<int32_t>(y));
            const Rgba8 a = before[y * W + x], b2 = after[y * W + x];
            if (s == 0 || s == 255 || b2.a == a.a) continue;
            partialPx += b2.a > 0 && b2.a < a.a;
            exactPx += std::abs(static_cast<int>(b2.a) - static_cast<int>(std::lround(a.a * (255 - s) / 255.0))) <= 1;
        }
    const bool partial = partialPx > 20;
    std::printf("     edge pixels partly erased: %d (of which a * (1 - s) +-1: %d)\n", partialPx, exactPx);
    check(partial, "S4: on the antialiased edge the eraser removes part of the alpha");
    d.deselect();
    before = grab(d, 1, W, H);
    stroke(d, brush(30, 0xFF2060FFu), 20, 650, 880, 650);
    after = grab(d, 1, W, H);
    size_t changed = 0;
    for (size_t i = 0; i < before.size(); ++i) changed += !(before[i] == after[i]);
    check(changed > 5000 && d.layerPixel(1, 20, 650).a > 0, "S4: deselected: strokes paint everywhere again");
    // History: a clipped stroke is one ordinary stroke step, undo exact.
    const auto e = d.historyEntries();
    check(e.size() >= 4 && e[e.size() - 2].kind == HistoryKind::Selection, "S4: deselect is a 'selection' step between the strokes");
}

void testClearCopyPaste() {
    section("S5 clear selected, copy, copy merged, cut, paste as a new layer, duplicate");
    const uint32_t W = 900, H = 700;
    CanvasDocument d;
    base(d);
    d.fill({100, 100, 400, 300}, 0x80FF0000u);  // semi-transparent red (alpha 128)
    uint32_t top = 0;
    d.addLayer(0, "Top", top);
    d.fill({300, 200, 100, 100}, 0xFF0000FFu);  // opaque blue on the top layer
    d.setActive(1);
    sel(d, ellipse(150, 150, 450, 350));
    auto before = grab(d, 1, W, H);
    const auto topBefore = d.layerChecksum(top);
    uint32_t ac = 0;
    const size_t layersBefore = d.layers(ac).size();
    check(d.clearSelected() == LayerError::None, "S5: clear selected");
    auto after = grab(d, 1, W, H);
    Diff df = diff(d, before, after, W, H);
    check(df.outsideChanged == 0 && df.insideChanged > 40000 && d.layerPixel(1, 300, 250).a == 0 && d.layerPixel(1, 120, 120).a == 128,
          fmt("S5: cleared inside (%.0f px), outside untouched (%.0f px changed)", df.insideChanged, df.outsideChanged));
    check(d.layers(ac).size() == layersBefore && d.layerChecksum(top) == topBefore, "S5: clearing is not deleting: the layer stays, other layers untouched");
    check(d.historyEntries().back().kind == HistoryKind::ClearSelected, "S5: one 'clear selection' step");
    d.undo();
    check(grab(d, 1, W, H) == before, "S5: undo clear: every pixel back");

    // Copy (active layer only) -> paste as a new layer.
    sel(d, rect(200, 150, 450, 350));
    check(d.copySelection(false) == LayerError::None, "S5: copy");
    uint32_t pasted = 0;
    check(d.paste(pasted) == LayerError::None && pasted != 0, "S5: paste as a new layer");
    uint32_t act = 0;
    const auto ls = d.layers(act);
    check(act == pasted && ls.size() == layersBefore + 1 && ls[1].info.id == pasted, "S5: the pasted layer is above the source layer and active");
    bool same = true, transparentOutside = true;
    for (int y = 90; y < 420; y += 3)
        for (int x = 90; x < 520; x += 3) {
            const bool in = x >= 200 && x < 450 && y >= 150 && y < 350;
            const Rgba8 p = d.layerPixel(pasted, x, y);
            if (in) same &= p == d.layerPixel(1, x, y);
            else transparentOutside &= p.a == 0;
        }
    check(same, "S5: pasted pixels identical to the source inside the selection, semi-transparent alpha (128) preserved");
    check(transparentOutside, "S5: pasted layer transparent outside the selection");
    check(d.historyEntries().back().kind == HistoryKind::Paste, "S5: one 'paste' step");
    d.undo();
    check(d.layers(act).size() == layersBefore, "S5: undo paste removes the pasted layer");

    // Copy merged: the visible image (both layers), no paper.
    check(d.copySelection(true) == LayerError::None && d.paste(pasted) == LayerError::None, "S5: copy merged, paste");
    const Rgba8 m = d.layerPixel(pasted, 350, 250), t = d.layerPixel(top, 350, 250);
    check(m == t && d.layerPixel(pasted, 250, 200).a == 128, "S5: merged copy holds the blue of the top layer and the red below");
    d.undo();

    // Cut: copies, then clears the active layer inside.
    d.setActive(1);
    before = grab(d, 1, W, H);
    check(d.cutSelection() == LayerError::None, "S5: cut");
    after = grab(d, 1, W, H);
    df = diff(d, before, after, W, H);
    check(df.outsideChanged == 0 && d.layerPixel(1, 250, 200).a == 0, "S5: cut cleared the selection only");
    d.paste(pasted);
    check(d.layerPixel(pasted, 250, 200).a == 128 && d.layerPixel(pasted, 120, 120).a == 0, "S5: the cut content pastes back with its transparency");
    d.undo();
    d.undo();
    check(grab(d, 1, W, H) == before, "S5: undo paste + undo cut: pixels back");

    // Duplicate selected content: a new layer, source unchanged.
    d.setActive(1);
    const uint64_t src = d.layerChecksum(1);
    uint32_t dup = 0;
    check(d.duplicateSelection(dup) == LayerError::None && d.layerChecksum(1) == src && d.layerPixel(dup, 250, 200).a == 128 &&
              d.layerPixel(dup, 120, 120).a == 0,
          "S5: duplicate selection: new layer with the selected part, source unchanged");
    check(d.historyEntries().back().kind == HistoryKind::DuplicateSelection, "S5: one 'duplicate selection' step");

    // Paste in place of an antialiased selection over its source: the same picture.
    {
        CanvasDocument p;
        base(p, 600, 400);
        p.fill({0, 0, 600, 400}, 0xFF3070B0u);
        stroke(p, brush(50, 0xFFD02020u), 20, 200, 580, 200);
        uint32_t s2 = 0;
        p.addLayer(0, "Band", s2);
        p.fill({250, 0, 100, 400}, 0x9020C040u);
        p.setActive(1);
        sel(p, ellipse(100, 60, 500, 340));
        std::vector<Rgba8> c0, c1;
        for (int y = 0; y < 400; ++y)
            for (int x = 0; x < 600; ++x) c0.push_back(p.pixel(x, y));
        uint32_t pid = 0;
        p.copySelection(false);
        p.paste(pid);
        int worst = 0;
        for (int y = 0; y < 400; ++y)
            for (int x = 0; x < 600; ++x) {
                const Rgba8 a = c0[static_cast<size_t>(y) * 600 + x], b = p.pixel(x, y);
                worst = std::max({worst, std::abs(a.r - b.r), std::abs(a.g - b.g), std::abs(a.b - b.b), std::abs(a.a - b.a)});
            }
        check(worst <= 1, fmt("S5: paste in place of an antialiased ellipse: composite max diff %.0f", worst));
    }

    // Refusals.
    d.setLocked(1, true);
    d.setActive(1);
    check(d.clearSelected() == LayerError::Locked && d.cutSelection() == LayerError::Locked && d.moveBegin() == LayerError::Locked,
          "S5: locked layer: clear / cut / move refused");
    d.setLocked(1, false);
    d.deselect();
    check(d.clearSelected() == LayerError::BadValue && d.copySelection(false) == LayerError::BadValue,
          "S5: without a selection: clear and copy do nothing (no layer cleared by mistake)");
    sel(d, rect(700, 500, 800, 600));
    check(d.copySelection(false) == LayerError::BadValue, "S5: copying an empty area is refused (clipboard kept)");
}

void testMove() {
    section("S6 moving selected content (floating content, transform integration point)");
    const uint32_t W = 900, H = 700;
    CanvasDocument d;
    base(d);
    d.fill({0, 0, 900, 700}, 0xFF203040u);
    d.fill({100, 100, 200, 200}, 0xFFFFC000u);
    uint32_t other = 0;
    d.addLayer(0, "Other", other);
    d.fill({600, 400, 100, 100}, 0xFF00FF00u);
    d.setActive(1);
    sel(d, rect(150, 150, 260, 260));
    d.clearHistory();
    const Snapshot s0 = snap(d);
    const auto before = grab(d, 1, W, H);
    const uint64_t otherPx = d.layerChecksum(other);
    check(d.moveBegin() == LayerError::None, "S6: move begin (content lifted)");
    uint32_t act = 0;
    check(d.layers(act).size() == 2 && d.selectionInfo().moving, "S6: the floating content is not a layer in the list");
    check(d.compositeChecksum() == s0.composite, "S6: lifted, not moved yet: the picture is unchanged");
    for (int i = 1; i <= 30; ++i) d.moveTo(i * 10, i * 5);  // a drag
    check(d.moveTo(300, 150) == LayerError::None, "S6: move to (300, 150)");
    check(d.historyStats().undoSteps == 0, "S6: dragging records nothing yet");
    check(d.pixel(455, 305) == Rgba8::fromArgb(0xFFFFC000u) && d.pixel(200, 200) != Rgba8::fromArgb(0xFFFFC000u),
          "S6: during the drag the composite shows the content at the new place and the hole at the old one");
    check(d.moveCommit() == LayerError::None, "S6: commit");
    const auto after = grab(d, 1, W, H);
    // Content moved: the old place shows the layer with the hole, the new place the yellow.
    const Rgba8 yellow = Rgba8::fromArgb(0xFFFFC000u), bg = Rgba8::fromArgb(0xFF203040u);
    check(d.layerPixel(1, 455, 305) == yellow && d.layerPixel(1, 200, 200).a == 0 && d.layerPixel(1, 120, 120) == yellow,
          "S6: moved pixels at the new place, hole at the old place, unselected neighbours kept");
    size_t unrelated = 0;
    for (uint32_t y = 0; y < H; ++y)
        for (uint32_t x = 0; x < W; ++x) {
            const bool oldPlace = x >= 150 && x < 260 && y >= 150 && y < 260;
            const bool newPlace = x >= 450 && x < 560 && y >= 300 && y < 410;
            if (!oldPlace && !newPlace) unrelated += !(before[y * W + x] == after[y * W + x]);
        }
    check(unrelated == 0 && d.layerChecksum(other) == otherPx, "S6: every unrelated pixel unchanged (this layer and the others)");
    check(d.layerPixel(1, 455, 305) == yellow && d.selectionAt(455, 305) == 255 && d.selectionAt(155, 155) == 0,
          "S6: the selection moved with the content");
    (void)bg;
    const auto e = d.historyEntries();
    check(e.size() == 1 && e[0].kind == HistoryKind::MoveSelection, "S6: the whole move is ONE history step");
    const Snapshot s1 = snap(d);
    check(d.undo() == 1 && snap(d) == s0, "S6: undo: pixels AND selection back exactly");
    check(d.redo() == 1 && snap(d) == s1, "S6: redo: moved again, selection moved");
    // Cancel restores exactly.
    d.moveBegin();
    d.moveTo(-80, 40);
    d.moveCancel();
    check(snap(d) == s1 && d.historyStats().undoSteps == 1, "S6: cancelled move: nothing changed, no step");
    // Moved off the canvas partly: what leaves is cut.
    d.moveBegin();
    d.moveTo(400, 0);
    d.moveCommit();
    check(d.layerPixel(1, 899, 305) == yellow && d.selectionAt(899, 305) == 255, "S6: moved to the canvas edge: the part beyond it is dropped");
    d.undo();
    check(snap(d) == s1, "S6: undo the edge move: exact");
    // A stroke started during a move: the move lands first, then the stroke (two steps).
    d.moveBegin();
    d.moveTo(-100, -100);
    stroke(d, brush(20, 0xFFFFFFFFu), 100, 255, 800, 255);  // through the selection (where it lands: 350..460)
    (void)d.pixel(0, 0);
    const auto e2 = d.historyEntries();
    check(!d.selectionInfo().moving && e2.size() >= 3 && e2[e2.size() - 2].kind == HistoryKind::MoveSelection &&
              e2.back().kind == HistoryKind::Stroke,
          "S6: painting during a move commits the move first (move step, then stroke step)");
    d.undo();
    d.undo();
    check(snap(d) == s1, "S6: both undone: exact");
    // Moving the content of a semi-transparent layer with a blend mode keeps it.
    d.setOpacity(1, 0.5f);
    d.setBlend(1, pe::canvas::BlendMode::Multiply);
    const uint64_t comp = d.compositeChecksum();
    d.moveBegin();
    check(d.compositeChecksum() == comp, "S6: floating content uses the layer's opacity and blend mode (picture unchanged when lifted)");
    d.moveCancel();
}

void testTransform() {
    section("S11 transform (Block 12): scale, rotate, flip, skew; non-destructive preview; one undo step");
    const uint32_t W = 800, H = 600;
    CanvasDocument d;
    base(d, W, H);
    d.fill({0, 0, 800, 600}, 0xFF203040u);
    d.fill({100, 100, 100, 100}, 0xFFFFC000u);   // yellow square 100..200
    uint32_t other = 0;
    d.addLayer(0, "Other", other);
    d.fill({600, 400, 100, 100}, 0xFF00FF00u);
    d.setActive(1);
    sel(d, rect(100, 100, 200, 200));
    d.clearHistory();
    const Snapshot s0 = snap(d);
    const uint64_t layer0 = d.layerChecksum(1), otherPx = d.layerChecksum(other);
    using pe::canvas::Affine2;
    // Scale 2x about the square's centre (150, 150): 50..250.
    const Affine2 sc = Affine2::compose(Affine2::translate(150, 150), Affine2::compose(Affine2::scale(2.0), Affine2::translate(-150, -150)));
    check(d.moveBegin() == LayerError::None && d.transformTo(sc) == LayerError::None, "S11: scale 2x");
    const Rgba8 yellow = Rgba8::fromArgb(0xFFFFC000u);
    check(d.pixel(60, 60) == yellow && d.pixel(240, 240) == yellow && d.pixel(45, 150) != yellow,
          "S11: the preview shows the square scaled to 50..250");
    check(d.layerChecksum(1) == layer0 && d.historyStats().undoSteps == 0,
          "S11: non-destructive preview: the layer's pixels are untouched, nothing recorded");
    // Rotate 90 degrees, flip, skew: each replaces the previous transform.
    const Affine2 rot = Affine2::compose(Affine2::translate(400, 300), Affine2::compose(Affine2::rotate(1.5707963267948966), Affine2::translate(-150, -150)));
    check(d.transformTo(rot) == LayerError::None && d.pixel(400, 300) == yellow && d.pixel(100 + 5, 100 + 5) != yellow,
          "S11: rotate 90 degrees about the square's centre to (400, 300)");
    const Affine2 flip{-1, 0, 0, 1, 300, 0};   // horizontal flip about x = 150
    check(d.transformTo(flip) == LayerError::None && d.pixel(150, 150) == yellow && d.layerChecksum(1) == layer0, "S11: flip horizontally");
    const Affine2 skew{1, 0, 0.5, 1, -75, 0};  // horizontal skew: x' = x + 0.5 (y - 150)
    check(d.transformTo(skew) == LayerError::None && d.pixel(215, 195) == yellow && d.pixel(190, 105) != yellow && d.pixel(85, 105) == yellow,
          "S11: skew horizontally (bottom edge shifted right, top edge left)");
    check(d.transformTo(Affine2{0, 0, 0, 0, 0, 0}) == LayerError::BadValue, "S11: a degenerate (zero-area) transform is refused");
    // Cancel: exactly the original picture, nothing recorded.
    d.moveCancel();
    check(snap(d) == s0 && d.historyStats().undoSteps == 0, "S11: cancel: identical picture and selection, no step");
    // Confirm a scale + rotation: one step; outside unchanged; selection transformed.
    const auto before = grab(d, 1, W, H);
    const Affine2 sr = Affine2::compose(Affine2::translate(450, 300), Affine2::compose(Affine2::rotate(0.5),
                       Affine2::compose(Affine2::scale(1.5), Affine2::translate(-150, -150))));
    d.moveBegin();
    d.transformTo(sr);
    const uint64_t shown = d.compositeChecksum();
    check(d.moveCommit() == LayerError::None, "S11: confirm a scale 1.5 + rotation 0.5 rad");
    check(d.compositeChecksum() == shown, "S11: the confirmed picture is exactly the preview");
    const auto after = grab(d, 1, W, H);
    // Unrelated pixels: outside both the old square and the new footprint (radius ~ 106 px around (450, 300)).
    size_t unrelated = 0;
    for (uint32_t y = 0; y < H; ++y)
        for (uint32_t x = 0; x < W; ++x) {
            const bool oldPlace = x >= 99 && x < 201 && y >= 99 && y < 201;
            const double ddx = x + 0.5 - 450, ddy = y + 0.5 - 300;
            const bool newPlace = ddx * ddx + ddy * ddy < 110.0 * 110.0;
            if (!oldPlace && !newPlace) unrelated += !(before[y * W + x] == after[y * W + x]);
        }
    check(unrelated == 0 && d.layerChecksum(other) == otherPx, "S11: every unrelated pixel unchanged (this layer and the others)");
    check(d.selectionAt(450, 300) == 255 && d.selectionAt(150, 150) == 0 && d.selectionAt(450 + 100, 300) == 0,
          "S11: the selection follows the transform");
    bool transparentEdge = false;
    for (int x = 450; x < 600 && !transparentEdge; ++x) {
        const Rgba8 p = d.layerPixel(1, x, 300);
        transparentEdge = p.a > 0 && p.a < 255;
    }
    (void)transparentEdge;
    const auto e = d.historyEntries();
    check(e.size() == 1 && e[0].kind == HistoryKind::MoveSelection, "S11: the whole transform is ONE history step");
    const Snapshot s1 = snap(d);
    check(d.undo() == 1 && snap(d) == s0, "S11: undo: pixels and selection back exactly");
    check(d.redo() == 1 && snap(d) == s1, "S11: redo: transformed again");
    // Transparency is kept: a half-transparent square scaled keeps alpha 128 inside.
    CanvasDocument t;
    base(t, 400, 400);
    t.fill({50, 50, 100, 100}, 0x80FF0000u);
    sel(t, rect(50, 50, 150, 150));
    t.moveBegin();
    t.transformTo(Affine2::compose(Affine2::translate(250, 250), Affine2::compose(Affine2::scale(1.3), Affine2::translate(-100, -100))));
    t.moveCommit();
    check(t.layerPixel(1, 250, 250).a == 128 && t.layerPixel(1, 100, 100).a == 0, "S11: transparency preserved (alpha 128 stays 128)");
    // Timing: a 1024 px selection rotated on a 4096 canvas.
    CanvasDocument big;
    base(big, 4096, 4096);
    big.fill({1000, 1000, 1024, 1024}, 0xFF3366CCu);
    sel(big, rect(1000, 1000, 2024, 2024));
    big.moveBegin();
    double worst = 0;
    for (int i = 1; i <= 10; ++i) {
        const int64_t t0 = pe::nowNs();
        big.transformTo(Affine2::compose(Affine2::translate(2048, 2048), Affine2::compose(Affine2::rotate(i * 0.1), Affine2::translate(-1512, -1512))));
        worst = std::max(worst, ms(t0));
    }
    const int64_t c0 = pe::nowNs();
    big.moveCommit();
    std::printf("     1024 px rotated on 4096: transform step max %.1f ms, confirm %.1f ms\n", worst, ms(c0));
    check(worst < 2000, "S11: rotation steps of a 1024 px selection complete");
}

void testHistory() {
    section("S7 undo / redo: every selection step, mixed with pixel steps, long sequences");
    const uint32_t W = 700, H = 500;
    CanvasDocument d;
    base(d, W, H);
    d.fill({50, 50, 550, 350}, 0xC0406080u);
    d.clearHistory();
    std::vector<Snapshot> states{snap(d)};
    std::vector<HistoryKind> kinds;
    struct Op { const char* name; std::function<void()> run; HistoryKind kind; };
    uint32_t id = 0;
    std::vector<Op> ops = {
        {"rectangle", [&] { sel(d, rect(100, 100, 300, 250)); }, HistoryKind::Selection},
        {"add ellipse", [&] { sel(d, ellipse(250, 150, 450, 350), Mode::Add); }, HistoryKind::Selection},
        {"subtract", [&] { sel(d, rect(200, 0, 260, 500), Mode::Subtract); }, HistoryKind::Selection},
        {"stroke inside", [&] { stroke(d, brush(40, 0xFFFF00FFu), 50, 200, 650, 220); }, HistoryKind::Stroke},
        {"invert", [&] { d.invertSelection(); }, HistoryKind::Selection},
        {"clear", [&] { d.clearSelected(); }, HistoryKind::ClearSelected},
        {"invert back", [&] { d.invertSelection(); }, HistoryKind::Selection},
        {"intersect", [&] { sel(d, rect(0, 120, 700, 300), Mode::Intersect); }, HistoryKind::Selection},
        {"duplicate", [&] { d.duplicateSelection(id); }, HistoryKind::DuplicateSelection},
        {"select all", [&] { d.selectAll(); }, HistoryKind::Selection},
        {"from content", [&] { d.selectFromContent(Mode::Replace); }, HistoryKind::Selection},
        {"cut", [&] { d.cutSelection(); }, HistoryKind::Cut},
        {"paste", [&] { d.paste(id); }, HistoryKind::Paste},
        {"lasso", [&] { const std::vector<float> p = {100, 100, 600, 120, 500, 450, 120, 400, 300, 250}; sel(d, p); }, HistoryKind::Selection},
        {"move", [&] { d.moveBegin(); d.moveTo(30, -20); d.moveCommit(); }, HistoryKind::MoveSelection},
        {"deselect", [&] { d.deselect(); }, HistoryKind::Selection},
    };
    bool oneEach = true;
    for (auto& op : ops) {
        const size_t n = d.historyStats().undoSteps;
        op.run();
        const auto e = d.historyEntries();
        const bool ok = d.historyStats().undoSteps == n + 1 && e.back().kind == op.kind;
        if (!ok) std::printf("     (%s: %zu -> %zu steps)\n", op.name, n, d.historyStats().undoSteps);
        oneEach &= ok;
        states.push_back(snap(d));
    }
    check(oneEach, "S7: each of the 16 operations = exactly one step of the expected kind");
    bool undo = true;
    for (int i = static_cast<int>(ops.size()) - 1; i >= 0; --i) {
        const bool ok = d.undo() == 1 && snap(d) == states[static_cast<size_t>(i)];
        if (!ok) std::printf("     (undo of %s differs)\n", ops[static_cast<size_t>(i)].name);
        undo &= ok;
    }
    check(undo, "S7: undone one by one: layers, pixels, composite and selection exact at every step");
    bool redo = true;
    for (size_t i = 1; i < states.size(); ++i) redo &= d.redo() == 1 && snap(d) == states[i];
    check(redo, "S7: redone one by one: exact at every step");
    // Repeating the same selection is not a step.
    sel(d, rect(100, 100, 200, 200));
    const size_t n = d.historyStats().undoSteps;
    sel(d, rect(100, 100, 200, 200));
    check(d.historyStats().undoSteps == n, "S7: re-selecting the identical shape adds no step");
    // Cycles.
    bool cycles = true;
    for (int c = 0; c < 20; ++c) {
        cycles &= d.undo() == 1;
        cycles &= d.redo() == 1;
    }
    check(cycles && d.selectionChecksum() == snap(d).selection, "S7: 20 undo / redo cycles");
    // A new document has no selection; the clipboard stays for the session.
    d.copySelection(false);
    d.open(400, 300);
    check(!d.selectionInfo().active && d.selectionInfo().clipboard, "S7: new document: no selection, clipboard kept");
    uint32_t p = 0;
    check(d.paste(p) == LayerError::None && d.layerPixel(p, 150, 150).a > 0, "S7: paste into another (smaller) document keeps what fits");
}

void testFillGradient() {
    section("S12 bucket fill and gradients (Block 13)");
    const uint32_t W = 600, H = 400;
    CanvasDocument d;
    base(d, W, H);
    // A closed black ring (antialiased stroke) on a transparent layer, a second layer elsewhere.
    std::vector<pe::brush::StrokePoint> ring;
    for (int i = 0; i <= 64; ++i) {
        const double t = 2 * 3.14159265358979 * i / 64;
        ring.push_back({300 + 100 * std::cos(t), 200 + 100 * std::sin(t), i * 4.0});
    }
    d.strokeBegin(brush(8, 0xFF000000u), ring[0]);
    d.strokeSamples(ring.data() + 1, ring.size() - 1);
    d.strokeEnd();
    uint32_t other = 0;
    d.addLayer(0, "Other", other);
    d.fill({0, 0, 50, 50}, 0xFF00FF00u);
    d.setActive(1);
    d.clearHistory();
    const uint64_t otherPx = d.layerChecksum(other);
    const auto before = grab(d, 1, W, H);
    CanvasDocument::FillOptions o;
    o.tolerance = 40;
    o.expand = 2;
    const int64_t t0 = pe::nowNs();
    check(d.bucketFill(300, 200, 0xFFE04020u, o) == LayerError::None, fmt("S12: fill inside the ring (%.1f ms)", ms(t0)));
    const Rgba8 red = Rgba8::fromArgb(0xFFE04020u);
    check(d.layerPixel(1, 300, 200) == red && d.layerPixel(1, 220, 200) == red, "S12: the inside is filled");
    check(d.layerPixel(1, 10, 10).a == 0 && d.layerPixel(1, 590, 390).a == 0, "S12: the outside is not (the ring stops the fill)");
    // Expansion: no transparent or light gap between the fill and the line.
    int gap = 0;
    for (int x = 300; x < 420; ++x) {
        const Rgba8 p = d.layerPixel(1, x, 200);
        if (p.a < 250 && x < 395) gap++;
    }
    check(gap == 0, "S12: expansion closes the gap under the antialiased line (no pixel below full alpha inside)");
    const auto after = grab(d, 1, W, H);
    size_t changedFar = 0;
    for (uint32_t y = 0; y < H; ++y)
        for (uint32_t x = 0; x < W; ++x) {
            const double r = std::hypot(x + 0.5 - 300, y + 0.5 - 200);
            if (r > 112 && !(before[y * W + x] == after[y * W + x])) changedFar++;
        }
    check(changedFar == 0 && d.layerChecksum(other) == otherPx, "S12: pixels outside the region and other layers unchanged");
    check(d.historyEntries().size() == 1 && d.historyEntries()[0].kind == HistoryKind::Fill, "S12: one 'fill' step");
    check(d.undo() == 1 && grab(d, 1, W, H) == before && d.redo() == 1 && grab(d, 1, W, H) == after, "S12: undo / redo exact");
    // Antialiasing: partial alpha on the outer edge only when on.
    CanvasDocument e;
    base(e, 200, 200);
    e.fill({50, 50, 100, 100}, 0xFF0000FFu);
    CanvasDocument::FillOptions hard;
    hard.tolerance = 0; hard.expand = 0; hard.antialias = false;
    e.bucketFill(100, 100, 0xFFFFFF00u, hard);
    check(e.layerPixel(1, 49, 100).a == 0 && e.layerPixel(1, 50, 100) == Rgba8::fromArgb(0xFFFFFF00u), "S12: no antialiasing: hard edge on the region");
    e.undo();
    CanvasDocument::FillOptions soft = hard;
    soft.antialias = true;
    e.bucketFill(100, 100, 0xFFFFFF00u, soft);
    const Rgba8 edge = e.layerPixel(1, 49, 100);
    check(edge.a > 0 && edge.a < 255, fmt("S12: antialiasing: soft outer edge (alpha %.0f)", edge.a));
    // Global replacement: both separate squares of the same colour change.
    CanvasDocument g;
    base(g, 300, 200);
    g.fill({10, 10, 40, 40}, 0xFF0000FFu);
    g.fill({200, 100, 40, 40}, 0xFF0000FFu);
    CanvasDocument::FillOptions glob = hard;
    glob.contiguous = false;
    g.bucketFill(20, 20, 0xFF00FF00u, glob);
    check(g.layerPixel(1, 210, 110) == Rgba8::fromArgb(0xFF00FF00u) && g.layerPixel(1, 150, 150).a == 0,
          "S12: global fill replaces every matching pixel, nothing else");
    // Transparent region + selection: filled only inside the selection; tolerance.
    CanvasDocument t;
    base(t, 300, 300);
    sel(t, rect(100, 100, 200, 200));
    t.bucketFill(150, 150, 0x80FF0000u, hard);
    check(t.layerPixel(1, 150, 150).a == 128 && t.layerPixel(1, 50, 50).a == 0, "S12: transparent area filled inside the selection only, colour alpha kept");
    // Merged mode: the line on another layer stops the fill of an empty layer.
    CanvasDocument m;
    base(m, 400, 300);
    m.fill({195, 0, 10, 300}, 0xFF000000u);   // a wall on layer 1
    uint32_t paint = 0;
    m.addLayer(0, "Paint", paint);
    CanvasDocument::FillOptions mg = hard;
    mg.merged = true;
    m.bucketFill(50, 50, 0xFF00FF00u, mg);
    check(m.layerPixel(paint, 50, 50).a == 255 && m.layerPixel(paint, 300, 50).a == 0 && m.layerPixel(1, 50, 50).a == 0,
          "S12: merged mode: the visible wall stops the fill, which goes to the active layer only");

    // Gradients: preview is non-destructive, commit is one step.
    CanvasDocument r;
    base(r, 400, 200);
    r.fill({0, 0, 400, 200}, 0xFFFFFFFFu);
    r.clearHistory();
    const uint64_t layer0 = r.layerChecksum(1);
    CanvasDocument::GradientSpec gs;
    gs.x0 = 0; gs.y0 = 100; gs.x1 = 400; gs.y1 = 100;
    gs.argb0 = 0xFF000000u; gs.argb1 = 0xFFFFFFFFu;
    check(r.gradientSet(gs) == LayerError::None && r.layerChecksum(1) == layer0 && r.historyStats().undoSteps == 0,
          "S12: gradient preview leaves the layer untouched");
    const Rgba8 l = r.pixel(10, 100), mid = r.pixel(200, 100), rr = r.pixel(390, 100);
    check(l.r < 15 && std::abs(mid.r - 128) <= 2 && rr.r > 240, fmt("S12: linear gradient shown: %.0f / %.0f / %.0f", l.r, mid.r, rr.r));
    gs.radial = true; gs.x0 = 200; gs.y0 = 100; gs.x1 = 300; gs.y1 = 100;
    gs.argb0 = 0xFFFF0000u; gs.argb1 = 0x00FF0000u;   // transparency gradient
    r.gradientSet(gs);
    const uint64_t shown = r.compositeChecksum();
    check(r.gradientCommit() == LayerError::None && r.compositeChecksum() == shown, "S12: radial transparency gradient committed exactly as previewed");
    const Rgba8 c = r.layerPixel(1, 200, 100), out = r.layerPixel(1, 350, 100), half = r.layerPixel(1, 250, 100);
    check(c.r == 255 && c.g < 3 && out == Rgba8::fromArgb(0xFFFFFFFFu) && half.g > 100 && half.g < 160,
          "S12: red at the centre, untouched beyond the radius, blended half way");
    check(r.historyEntries().size() == 1 && r.historyEntries()[0].kind == HistoryKind::Gradient && r.undo() == 1 &&
              r.layerChecksum(1) == layer0,
          "S12: one 'gradient' step; undo restores the layer");
    r.gradientSet(gs);
    r.gradientCancel();
    check(r.layerChecksum(1) == layer0 && r.compositeChecksum() != shown && r.historyStats().redoSteps == 1, "S12: cancelled gradient: nothing changed");
    // Timing on a large canvas.
    CanvasDocument big;
    base(big, 4096, 4096);
    CanvasDocument::GradientSpec bg;
    bg.x1 = 4096; bg.y1 = 4096;
    const int64_t g0 = pe::nowNs();
    big.gradientSet(bg);
    const double setMs = ms(g0);
    const int64_t g1 = pe::nowNs();
    big.gradientCommit();
    const double commitMs = ms(g1);
    big.clearHistory();
    const int64_t f0 = pe::nowNs();
    CanvasDocument::FillOptions bf;
    big.bucketFill(10, 10, 0xFF102030u, bf);   // floods the whole gradient-free? no: tolerance 32 of a smooth gradient
    std::printf("     4096 px: gradient preview %.1f ms, commit %.1f ms; bucket fill %.1f ms\n", setMs, commitMs, ms(f0));
    CanvasDocument empty;
    base(empty, 4096, 4096);
    CanvasDocument::FillOptions ef;
    ef.expand = 2;
    const int64_t e0 = pe::nowNs();
    check(empty.bucketFill(2000, 2000, 0xFF336699u, ef) == LayerError::None && empty.layerPixel(1, 4095, 4095).a == 255,
          fmt("S12: whole empty 4096 x 4096 layer filled (%.0f ms)", ms(e0)));
}

void testFromContent() {
    section("S8 selection from visible content: alpha, layer opacity, hidden layers");
    CanvasDocument d;
    base(d, 600, 400);
    d.fill({50, 50, 100, 100}, 0xFF000000u);
    d.fill({200, 50, 100, 100}, 0x80000000u);
    uint32_t b = 0, c = 0;
    d.addLayer(0, "B", b);
    d.fill({350, 50, 100, 100}, 0xFF000000u);
    d.setOpacity(b, 0.5f);
    d.addLayer(0, "C", c);
    d.fill({50, 250, 100, 100}, 0xFF000000u);
    d.setVisible(c, false);
    d.selectFromContent(Mode::Replace);
    check(d.selectionAt(100, 100) == 255 && std::abs(d.selectionAt(250, 100) - 128) <= 1 && std::abs(d.selectionAt(400, 100) - 128) <= 1 &&
              d.selectionAt(100, 300) == 0 && d.selectionAt(500, 300) == 0,
          "S8: opaque = 255, alpha 50 % = 128, layer opacity 50 % = 128, hidden layer and empty area = 0");
    d.selectFromContent(Mode::Subtract);
    check(d.selectionAt(100, 100) == 0 && std::abs(d.selectionAt(250, 100) - 127) <= 1,
          "S8: subtracting the content from itself: opaque parts gone, half-covered parts min(a, 1 - a)");
}

void testLarge() {
    section("S9 large canvas: latency and memory");
    for (uint32_t size : {4096u, 8192u}) {
        CanvasDocument d;
        base(d, size, size);
        uint32_t id = 0;
        for (int i = 0; i < 4; ++i) {
            if (i) d.addLayer(0, "", id);
            d.fill({static_cast<int32_t>(i * 300), 200, static_cast<int32_t>(size / 2), static_cast<int32_t>(size / 2)}, 0xFF102030u + i * 0x302010u);
        }
        const double S = size;
        auto t = [&](const char* what, const std::function<void()>& f) {
            const int64_t t0 = pe::nowNs();
            f();
            return std::make_pair(std::string(what), ms(t0));
        };
        std::vector<std::pair<std::string, double>> r;
        r.push_back(t("select all", [&] { d.selectAll(); }));
        r.push_back(t("rectangle", [&] { sel(d, rect(S * 0.1, S * 0.1, S * 0.9, S * 0.8)); }));
        r.push_back(t("ellipse", [&] { sel(d, ellipse(S * 0.05, S * 0.05, S * 0.95, S * 0.95)); }));
        std::vector<float> lasso;
        std::mt19937 rng(11);
        std::uniform_real_distribution<double> j(-3, 3);
        for (int i = 0; i < 20000; ++i) {
            const double a = 2 * 3.14159265358979 * i / 20000, rr = S * (0.3 + 0.1 * std::sin(a * 9));
            lasso.push_back(static_cast<float>(S / 2 + rr * std::cos(a) + j(rng)));
            lasso.push_back(static_cast<float>(S / 2 + rr * std::sin(a) + j(rng)));
        }
        r.push_back(t("20000-point lasso, add", [&] { sel(d, lasso, Mode::Add); }));
        r.push_back(t("invert", [&] { d.invertSelection(); }));
        r.push_back(t("invert back", [&] { d.invertSelection(); }));
        std::vector<float> c;
        r.push_back(t("outline", [&] { d.selectionContours(c); }));
        uint32_t pw, ph;
        std::vector<uint8_t> pv;
        r.push_back(t("preview 512", [&] { d.selectionPreview(512, pw, ph, pv); }));
        r.push_back(t("from content", [&] { d.selectFromContent(Mode::Replace); }));
        sel(d, ellipse(S * 0.2, S * 0.2, S * 0.6, S * 0.6));
        r.push_back(t("copy", [&] { d.copySelection(false); }));
        r.push_back(t("paste", [&] { d.paste(id); }));
        r.push_back(t("clear", [&] { d.clearSelected(); }));
        r.push_back(t("move begin", [&] { d.moveBegin(); }));
        double moveMax = 0;
        for (int i = 1; i <= 20; ++i) moveMax = std::max(moveMax, t("", [&] { d.moveTo(i * 13, i * 7); }).second);
        r.push_back({"move step (max of 20)", moveMax});
        r.push_back(t("move commit", [&] { d.moveCommit(); }));
        r.push_back(t("undo move", [&] { d.undo(); }));
        r.push_back(t("undo clear", [&] { d.undo(); }));
        double worst = 0;
        for (const auto& [n, v] : r) {
            std::printf("     %u px: %-24s %8.2f ms\n", size, n.c_str(), v);
            worst = std::max(worst, v);
        }
        const auto info = d.selectionInfo();
        std::printf("     %u px: mask %.2f MiB (%.0f %% of a full 8-bit mask), outline %zu floats, history %.2f MiB\n", size,
                    (info.bytes - 0) / 1048576.0, 100.0 * info.bytes / (S * S), c.size(), d.historyStats().bytes / 1048576.0);
        check(worst < (size == 4096 ? 2000.0 : 8000.0), fmt("S9: %.0f px canvas: every operation done (slowest %.0f ms)", S, worst));
        sel(d, ellipse(S * 0.2, S * 0.2, S * 0.6, S * 0.6));
        const double maskMiB = d.selectionInfo().bytes / 1048576.0;
        check(maskMiB < S * S / 1048576.0 / 4, fmt("S9: ellipse mask %.2f MiB (a full mask would be %.0f MiB): edge tiles only", maskMiB, S * S / 1048576.0));
    }
}

void testBrushLatency() {
    section("S10 brush cost with and without a selection (no latency regression)");
    auto run = [](bool withSelection, bool partialTiles) {
        CanvasDocument d;
        base(d, 2048, 1536);
        if (withSelection) {
            if (partialTiles) sel(d, ellipse(10, 10, 2040, 1530));
            else d.selectAll();
        }
        std::mt19937 rng(5);
        std::uniform_real_distribution<double> X(50, 2000), Y(50, 1480);
        const int64_t t0 = pe::nowNs();
        for (int i = 0; i < 150; ++i) {
            stroke(d, brush(30, 0xFF000000u | static_cast<uint32_t>(rng()), 0.8f), X(rng), Y(rng), X(rng), Y(rng), 30);
            (void)d.pixel(0, 0);
        }
        return ms(t0);
    };
    run(false, false);  // warm-up
    const double none = run(false, false), all = run(true, false), ell = run(true, true);
    std::printf("     150 strokes: no selection %.1f ms, select all %.1f ms, ellipse (antialiased edge tiles) %.1f ms\n", none, all, ell);
    check(all < none * 1.25 + 20 && ell < none * 1.5 + 20, fmt("S10: clipped painting cost %.0f %% / %.0f %% of unclipped", 100 * all / none, 100 * ell / none));
}

// ---------------------------------------------------------------- Block 14
pe::brush::BrushSettings smudger(float dia, float strength, float pickup = 1.0f) {
    pe::brush::BrushSettings b = brush(dia, 0xFF000000u, strength);
    b.hardness = 0.5f;
    b.wet = pe::brush::WetMode::Smudge;
    b.pickup = pickup;
    return b;
}
pe::brush::BrushSettings mixer(float dia, uint32_t argb, float mix, float pickup = 0.5f) {
    pe::brush::BrushSettings b = brush(dia, argb);
    b.wet = pe::brush::WetMode::Mix;
    b.mix = mix;
    b.pickup = pickup;
    return b;
}
void straight(CanvasDocument& d, const pe::brush::BrushSettings& b, double x0, double y0, double x1, double y1, int n = 60) {
    std::vector<pe::brush::StrokePoint> p;
    for (int i = 0; i < n; ++i) {
        const double t = static_cast<double>(i) / (n - 1);
        p.push_back({x0 + (x1 - x0) * t, y0 + (y1 - y0) * t, i * 4.0});
    }
    d.strokeBegin(b, p[0]);
    d.strokeSamples(p.data() + 1, p.size() - 1);
    d.strokeEnd();
    (void)d.layerPixel(1, 0, 0);   // processes the queued input
}
// White layer with a blue block (x 100..400, y 200..500).
void whiteBlue(CanvasDocument& d) {
    base(d, 900, 700);
    d.bucketFill(5, 5, 0xFFFFFFFFu, {});
    sel(d, rect(100, 200, 400, 500), Mode::Replace, false);
    d.bucketFill(200, 300, 0xFF2050E0u, {});
    d.deselect();
}
double changedSum(const std::vector<Rgba8>& a, const std::vector<Rgba8>& b) {
    double s = 0;
    for (size_t i = 0; i < a.size(); ++i)
        s += std::abs(a[i].r - b[i].r) + std::abs(a[i].g - b[i].g) + std::abs(a[i].b - b[i].b) + std::abs(a[i].a - b[i].a);
    return s;
}

void testWetMedia() {
    section("S13 smudge and colour mixing (Block 14)");
    constexpr uint32_t W = 900, H = 700;
    {   // Smudge drags the blue into the white along the stroke, no new colours.
        CanvasDocument d;
        whiteBlue(d);
        const auto before = grab(d, 1, W, H);
        const Snapshot s0 = snap(d);
        const int64_t t0 = pe::nowNs();
        straight(d, smudger(60, 0.85f), 250, 350, 650, 350);
        const double t = ms(t0);
        const auto after = grab(d, 1, W, H);
        const Snapshot s1 = snap(d);
        const Rgba8 p = after[350 * W + 460], q = before[350 * W + 460];
        check(q.r == 255 && p.b > p.r + 40, fmt("S13: smudged: white pixel 60 px past the blue edge is now blue-ish (r %.0f b %.0f)", p.r, p.b));
        bool hull = true, premul = true;
        size_t changed = 0, farChanged = 0;
        for (uint32_t y = 0; y < H; ++y)
            for (uint32_t x = 0; x < W; ++x) {
                const Rgba8 a = after[y * W + x];
                if (a.r > a.a || a.g > a.a || a.b > a.a) premul = false;
                if (a == before[y * W + x]) continue;
                ++changed;
                if (x > 700 || y < 300 || y > 400) ++farChanged;
                if (a.r < 0x20 - 1 || a.g < 0x50 - 1 || a.b < 0xE0 - 1) hull = false;   // between blue and white only
            }
        check(changed > 3000 && farChanged == 0, fmt("S13: %.0f px changed, all under the stroke (%.0f elsewhere)", changed, farChanged));
        check(hull && premul, "S13: smudge adds no colour (every result between the blue and the white), valid premultiplied");
        const auto e = d.historyEntries();
        check(!e.empty() && e.back().kind == HistoryKind::Smudge, "S13: one 'smudge' history step");
        check(d.undo() == 1 && snap(d) == s0, "S13: undo restores every pixel exactly");
        check(d.redo() == 1 && snap(d) == s1, "S13: redo gives the same smudge back");
        std::printf("     smudge stroke, 60 px, 400 px long: %.1f ms\n", t);
        // Strength: a weak smudge moves less paint.
        d.undo();
        straight(d, smudger(60, 0.2f), 250, 350, 650, 350);
        const double weak = changedSum(before, grab(d, 1, W, H));
        const double strong = changedSum(before, after);
        check(weak < strong * 0.6, fmt("S13: strength 20 %% changes %.0f %% of what 85 %% does", 100.0 * weak / strong));
        // The spacing does not change the result (rates are per distance, not per dab).
        auto spaced = [&](float sp) {
            d.undo();
            auto b = smudger(60, 0.6f);
            b.spacing = sp;
            straight(d, b, 250, 350, 650, 350);
            return changedSum(before, grab(d, 1, W, H));
        };
        const double sDense = spaced(0.04f), sSparse = spaced(0.16f);
        check(std::abs(sDense - sSparse) < 0.2 * sDense, fmt("S13: spacing 4 %% vs 16 %%: %.0f %% difference", 100.0 * std::abs(sDense - sSparse) / sDense));
        // Pickup: a low pickup keeps the first colour on the brush longer (drags it further).
        auto blueAt = [&](float pickup, int x) {
            d.undo();
            straight(d, smudger(60, 0.5f, pickup), 250, 350, 650, 350);
            const Rgba8 c = d.layerPixel(1, x, 350);
            return c.b - c.r;
        };
        const int hi = blueAt(1.0f, 470), lo = blueAt(0.15f, 470);
        check(lo > hi + 10, fmt("S13: 70 px past the edge: blue excess %.0f with pickup 15 %%, %.0f with 100 %%", lo, hi));
    }
    {   // The default smudge brush (soft airbrush: flow 12 %, pressure -> opacity down to 10 %) with a
        // lightly pressed pen still smudges: the strength is the tool's, not the preset's flow.
        CanvasDocument d;
        whiteBlue(d);
        auto b = smudger(120, 0.8f, 0.5f);
        b.hardness = 0.0f;
        b.flow = 0.12f;
        b.accumulation = pe::brush::Accumulation::Buildup;
        b.pressureOpacity = true;
        b.minOpacityFraction = 0.1f;
        std::vector<pe::brush::StrokePoint> p;
        for (int i = 0; i < 60; ++i) {
            pe::brush::StrokePoint q{250.0 + 400.0 * i / 59, 350.0, i * 4.0};
            q.axes.pressure = 0.3f;
            p.push_back(q);
        }
        d.strokeBegin(b, p[0]);
        d.strokeSamples(p.data() + 1, p.size() - 1);
        d.strokeEnd();
        const Rgba8 c = d.layerPixel(1, 470, 350);
        check(c.b > c.r + 60, fmt("S13: soft airbrush + light pen pressure still drags the blue 70 px (r %.0f b %.0f)", c.r, c.b));
    }
    {   // Selection: nothing changes outside it.
        CanvasDocument d;
        whiteBlue(d);
        sel(d, ellipse(300, 250, 600, 450));
        const auto before = grab(d, 1, W, H);
        straight(d, smudger(80, 1.0f), 150, 350, 750, 350);
        const Diff df = diff(d, before, grab(d, 1, W, H), W, H);
        check(df.outsideChanged == 0 && df.insideChanged > 2000,
              fmt("S13: smudge in a selection: %.0f px changed inside, %.0f outside", df.insideChanged, df.outsideChanged));
        d.deselect();
    }
    {   // Transparency: smudging out of an opaque blob on a transparent layer.
        CanvasDocument d;
        base(d, W, H);
        sel(d, rect(100, 200, 400, 500), Mode::Replace, false);
        d.bucketFill(200, 300, 0xFFE03020u, {});
        d.deselect();
        straight(d, smudger(60, 0.6f), 250, 350, 650, 350);
        const Rgba8 out = d.layerPixel(1, 460, 350), in = d.layerPixel(1, 300, 350);
        bool premul = true;
        for (uint32_t y = 300; y < 400; ++y)
            for (uint32_t x = 200; x < 700; ++x) {
                const Rgba8 a = d.layerPixel(1, x, y);
                if (a.r > a.a || a.g > a.a || a.b > a.a) premul = false;
            }
        check(out.a > 30 && out.a < 255 && premul, fmt("S13: smudged into transparency: partial alpha %.0f, premultiplied valid", out.a));
        check(in.a == 255, "S13: dragging starts inside the paint: the paint behind stays opaque");
        // Back the other way: transparency dragged into the paint thins it.
        straight(d, smudger(60, 0.9f), 700, 420, 250, 420);
        check(d.layerPixel(1, 380, 420).a < 250, fmt("S13: transparency smudged into paint lowers its alpha (%.0f)", d.layerPixel(1, 380, 420).a));
    }
    {   // Colour mixing.
        CanvasDocument d;
        whiteBlue(d);
        const uint32_t yellow = 0xFFF0D020u;
        straight(d, mixer(40, yellow, 0.0f), 150, 260, 750, 260);
        const Rgba8 plain = d.layerPixel(1, 250, 260);
        check(plain.r == 0xF0 && plain.g == 0xD0 && plain.b == 0x20, "S13: mix 0: the brush colour exactly (classic stroke)");
        d.undo();
        straight(d, mixer(40, yellow, 0.6f), 150, 300, 750, 300);
        const Rgba8 m = d.layerPixel(1, 300, 300), w = d.layerPixel(1, 700, 300);
        check(m.a == 255 && m.g > m.r && m.g > m.b, fmt("S13: yellow paint over blue mixes to an opaque green (r %.0f g %.0f b %.0f)", m.r, m.g, m.b));
        check(w.a == 255 && w.b < m.b && w.r > m.r, "S13: past the blue, the paint drifts back towards the brush colour + white");
        int jump = 0;
        for (int x = 160; x < 740; ++x) {
            const Rgba8 a = d.layerPixel(1, x, 300), b = d.layerPixel(1, x + 1, 300);
            jump = std::max({jump, std::abs(a.r - b.r), std::abs(a.g - b.g), std::abs(a.b - b.b)});
        }
        check(jump <= 24, fmt("S13: smooth colour transition along the stroke (largest step %.0f / 255)", jump));
        const auto e = d.historyEntries();
        check(e.back().kind == HistoryKind::Stroke && d.undo() == 1 && d.layerPixel(1, 300, 300) == Rgba8{0x20, 0x50, 0xE0, 0xFF},
              "S13: a mixing stroke is one 'stroke' step, undo exact");
        // On a transparent layer the paint keeps its colour (transparent pixels add none).
        uint32_t top = 0;
        d.addLayer(1, "Empty", top);
        straight(d, mixer(40, yellow, 0.6f), 150, 300, 750, 300);
        const Rgba8 t = d.layerPixel(top, 300, 300);
        check(t.a == 255 && t.r == 0xF0 && t.b == 0x20, "S13: mixing on a transparent layer: the brush colour, opaque");
    }
    {   // Block 15: paint charge, dilution, start taper.
        CanvasDocument d;
        base(d, W, H);
        auto dry = mixer(40, 0xFFE03020u, 0.0f);
        dry.charge = 0.1f;
        straight(d, dry, 100, 200, 800, 200, 120);
        const int a0 = d.layerPixel(1, 130, 200).a, a1 = d.layerPixel(1, 760, 200).a;
        check(a0 > 180 && a1 < a0 / 2, fmt("S13: low charge: the paint runs out along the stroke (alpha %.0f -> %.0f)", a0, a1));
        auto thin = mixer(40, 0xFFE03020u, 0.0f);
        thin.dilution = 0.8f;
        straight(d, thin, 100, 320, 800, 320, 120);
        const int at = d.layerPixel(1, 450, 320).a;
        check(at > 40 && at < 140, fmt("S13: dilution 80 %%: thin paint (alpha %.0f)", at));
        auto tap = brush(40, 0xFF000000u);
        tap.taper = 0.5f;
        straight(d, tap, 100, 450, 800, 450, 120);
        auto width = [&](int x) { int n = 0; for (int y = 400; y < 500; ++y) n += d.layerPixel(1, x, y).a > 127; return n; };
        check(width(104) < 16 && width(160) > width(104) + 6 && width(400) >= 38,
              fmt("S13: start taper: %.0f px wide at the start, %.0f at 60 px, %.0f later", width(104), width(160), width(400)));
        check(d.undo() == 1 && d.undo() == 1 && d.undo() == 1 && d.layerPixel(1, 450, 320).a == 0, "S13: each is one undo step");
    }
    {   // Cost: large smudge brush.
        CanvasDocument d;
        whiteBlue(d);
        const int64_t t0 = pe::nowNs();
        straight(d, smudger(300, 0.8f), 100, 350, 800, 350, 120);
        const double t = ms(t0);
        std::printf("     smudge stroke, 300 px brush, 700 px long: %.1f ms\n", t);
        check(t < 1500, fmt("S13: large smudge stroke %.0f ms", t));
    }
}

}  // namespace

int main() {
    std::printf("=== selection tests (CPU) ===\n");
    testShapes();
    testModes();
    testTilesAndEncoding();
    testClippedPainting();
    testClearCopyPaste();
    testMove();
    testHistory();
    testTransform();
    testFillGradient();
    testFromContent();
    testLarge();
    testBrushLatency();
    testWetMedia();
    std::printf("=== %d/%d checks passed ===\n", gChecks - gFailures, gChecks);
    return gFailures == 0 ? 0 : 1;
}
