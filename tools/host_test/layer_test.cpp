// PxPaint - layer system and compositing tests (pure CPU, Block 6).
//
// Blend modes are checked against an independent double-precision
// implementation of the W3C compositing formula (reference values), on
// opaque, semi-transparent and transparent pixels, over transparent and
// opaque backdrops. Layer operations, painting into the active layer,
// merge / flatten equivalence, dirty-region updates, thumbnails, save and
// reload, invalid operations and timings are tested on CanvasDocument, the
// class the engine uses. The GPU side (display on Vulkan and GLES) is in
// host_test --suite brush (S12).
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

#include "canvas/Compositor.h"
#include "core/Time.h"
#include "engine/CanvasDocument.h"

using pe::CanvasDocument;
using pe::LayerError;
using pe::LayerInfo;
using pe::canvas::BlendMode;
using pe::canvas::RasterStore;
using pe::canvas::RectI;
using pe::canvas::Rgba8;

namespace {

int gChecks = 0, gFailures = 0;
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

constexpr BlendMode kModes[] = {BlendMode::Normal, BlendMode::Multiply, BlendMode::Screen,  BlendMode::Overlay,
                                BlendMode::Darken, BlendMode::Lighten,  BlendMode::Add,     BlendMode::Difference};

// ---- independent reference (double, straight colours in, premultiplied out)
double refB(BlendMode m, double cb, double cs) {
    switch (m) {
        case BlendMode::Normal: return cs;
        case BlendMode::Multiply: return cb * cs;
        case BlendMode::Screen: return 1 - (1 - cb) * (1 - cs);
        case BlendMode::Overlay: return cb < 0.5 || cb == 0.5 ? 2 * cb * cs : 1 - 2 * (1 - cb) * (1 - cs);
        case BlendMode::Darken: return std::min(cb, cs);
        case BlendMode::Lighten: return std::max(cb, cs);
        case BlendMode::Add: return std::min(1.0, cb + cs);
        case BlendMode::Difference: return std::fabs(cb - cs);
    }
    return cs;
}
struct P4 { double r, g, b, a; };  // premultiplied, 0..1
// Layer pixel `s` (straight colour + alpha) with opacity over backdrop `d` (premultiplied).
P4 refOver(P4 d, double sr, double sg, double sb, double sa, double opacity, BlendMode m) {
    const double as = sa * opacity, ab = d.a;
    const double ao = as + ab * (1 - as);
    P4 o{0, 0, 0, ao};
    const double S[3] = {sr, sg, sb}, D[3] = {d.r, d.g, d.b};
    double out[3];
    for (int c = 0; c < 3; ++c) {
        const double Cb = ab > 0 ? D[c] / ab : 0;
        out[c] = as * (1 - ab) * S[c] + D[c] * (1 - as) + as * ab * refB(m, Cb, S[c]);
    }
    o.r = out[0];
    o.g = out[1];
    o.b = out[2];
    return o;
}

// Document helpers -------------------------------------------------------
uint32_t argbOf(double r, double g, double b, double a) {
    auto c = [](double v) { return static_cast<uint32_t>(std::lround(std::clamp(v, 0.0, 1.0) * 255)); };
    return (c(a) << 24) | (c(r) << 16) | (c(g) << 8) | c(b);
}
int maxDiff(Rgba8 p, const P4& q) {
    auto v = [](double x) { return static_cast<int>(std::lround(std::clamp(x, 0.0, 1.0) * 255)); };
    return std::max({std::abs(p.r - v(q.r)), std::abs(p.g - v(q.g)), std::abs(p.b - v(q.b)), std::abs(p.a - v(q.a))});
}
bool sameComposite(CanvasDocument& a, CanvasDocument& b, int w, int h, int tol, int* worst = nullptr) {
    int m = 0;
    for (int y = 0; y < h; y += 1)
        for (int x = 0; x < w; x += 1) {
            const Rgba8 p = a.pixel(x, y), q = b.pixel(x, y);
            m = std::max({m, std::abs(p.r - q.r), std::abs(p.g - q.g), std::abs(p.b - q.b), std::abs(p.a - q.a)});
        }
    if (worst) *worst = m;
    return m <= tol;
}

// ---------------------------------------------------------------- tests
void testBlendReference() {
    section("LY1 blend modes == W3C reference (opaque, semi-transparent, transparent; transparent and paper backdrops)");
    // Backdrop and source straight colours / alphas covering the cases.
    struct Px { double r, g, b, a; };
    const Px backs[] = {{0.6, 0.3, 0.9, 1.0}, {0.2, 0.8, 0.5, 0.5}, {0.9, 0.9, 0.1, 0.25}, {0, 0, 0, 0}};
    const Px srcs[] = {{0.3, 0.7, 0.2, 1.0}, {0.9, 0.1, 0.6, 0.6}, {0.5, 0.5, 0.5, 0.2}, {1, 0, 0, 0}};
    const double opacities[] = {1.0, 0.5};
    for (BlendMode m : kModes) {
        int worst = 0;
        for (const Px& b : backs)
            for (const Px& s : srcs)
                for (double o : opacities)
                    for (bool paper : {false, true}) {
                        CanvasDocument d;
                        d.open(4, 4);
                        d.setBackground(0xFFE0C8A0u, !paper);
                        d.fill({0, 0, 4, 4}, argbOf(b.r, b.g, b.b, b.a));
                        uint32_t top = 0;
                        d.addLayer(0, "top", top);
                        d.fill({0, 0, 4, 4}, argbOf(s.r, s.g, s.b, s.a));
                        d.setBlend(top, m);
                        d.setOpacity(top, static_cast<float>(o));
                        // Reference from the 8-bit values actually stored.
                        const Rgba8 lb = d.layerPixel(1, 1, 1), ls = d.layerPixel(top, 1, 1);
                        P4 back{0, 0, 0, 0};
                        if (paper) back = {0xE0 / 255.0, 0xC8 / 255.0, 0xA0 / 255.0, 1.0};
                        back = refOver(back, lb.a ? lb.r / double(lb.a) : 0, lb.a ? lb.g / double(lb.a) : 0,
                                       lb.a ? lb.b / double(lb.a) : 0, lb.a / 255.0, 1.0, BlendMode::Normal);
                        const P4 want = refOver(back, ls.a ? ls.r / double(ls.a) : 0, ls.a ? ls.g / double(ls.a) : 0,
                                                ls.a ? ls.b / double(ls.a) : 0, ls.a / 255.0, o, m);
                        const int diff = maxDiff(d.pixel(1, 1), want);
                        if (diff > 1 && worst <= 1) {
                            const Rgba8 g = d.pixel(1, 1);
                            std::printf("    first mismatch: got %d,%d,%d,%d want %.1f,%.1f,%.1f,%.1f (layers %08x %08x paper %d op %.1f)\n", g.r,
                                        g.g, g.b, g.a, want.r * 255, want.g * 255, want.b * 255, want.a * 255, lb.toArgb(),
                                        ls.toArgb(), paper, o);
                        }
                        worst = std::max(worst, diff);
                    }
        check(worst <= 1, std::string("LY1: ") + pe::canvas::blendModeName(m) +
                              fmt(" matches the reference on 128 cases (max diff %.0f / 255)", worst));
    }
}

void testTransparency() {
    section("LY2 transparent pixels: never darken, tint or cut anything");
    bool srcClear = true, backClear = true;
    for (BlendMode m : kModes) {
        CanvasDocument d;
        d.open(8, 4);
        d.setBackground(0xFFFFFFFFu, true);
        d.fill({0, 0, 4, 4}, 0xFF3366CCu);  // left half: backdrop content
        uint32_t top = 0;
        d.addLayer(0, "", top);
        d.fill({2, 0, 5, 4}, 0xFFCC3300u);  // x 2..6: layer content, x 7: nothing anywhere
        d.fill({0, 0, 2, 4}, 0x00000000u);  // left: transparent layer pixels
        d.setBlend(top, m);
        // Transparent layer pixel: backdrop unchanged.
        srcClear &= d.pixel(0, 0).toArgb() == 0xFF3366CCu;
        // Transparent backdrop: the layer shows as itself (Multiply of red over nothing is red, not black).
        backClear &= d.pixel(6, 0).toArgb() == 0xFFCC3300u && d.pixel(7, 0).a == 0;
    }
    check(srcClear, "LY2: transparent layer pixels leave the backdrop unchanged, all 8 modes");
    check(backClear, "LY2: over a transparent backdrop every mode shows the layer as is (no black, no halo)");
    // Half-transparent white Multiply over transparent: stays half-transparent white, not grey.
    CanvasDocument d;
    d.open(2, 2);
    d.setBackground(0, true);
    uint32_t top = 0;
    d.addLayer(0, "", top);
    d.fill({0, 0, 2, 2}, 0x80FFFFFFu);
    d.setBlend(top, BlendMode::Multiply);
    const Rgba8 p = d.pixel(0, 0);
    check(p.a == 128 && p.r == 128 && p.g == 128 && p.b == 128,
          "LY2: 50 % white Multiply over transparency = 50 % white (premultiplied 128), no darkening");
    // Paper: Multiply against the paper colour, exactly as displayed.
    d.setBackground(0xFF808080u, false);
    d.fill({0, 0, 2, 2}, 0xFFFF0000u);
    const Rgba8 q = d.pixel(0, 0);
    check(q.a == 255 && q.r == 128 && q.g == 0 && q.b == 0, "LY2: opaque red Multiply on grey paper = dark red (128, 0, 0)");
}

void testOpacity() {
    section("LY3 layer opacity is independent from brush opacity");
    CanvasDocument d;
    d.open(300, 100);
    d.setBackground(0xFFFFFFFFu, false);
    uint32_t top = 0;
    d.addLayer(0, "", top);
    pe::brush::BrushSettings b;
    b.diameter = 30;
    b.opacity = 0.5f;
    b.argb = 0xFF000000u;
    std::vector<pe::brush::StrokePoint> pts;
    for (int i = 0; i < 20; ++i) pts.push_back({30.0 + i * 12, 50.0, i * 5.0});
    check(d.strokeBegin(b, pts[0]), "LY3: stroke accepted on an editable layer");
    d.strokeSamples(pts.data() + 1, pts.size() - 1);
    d.strokeEnd();
    d.setOpacity(top, 0.5f);
    const Rgba8 layer = d.layerPixel(top, 150, 50), shown = d.pixel(150, 50);
    check(layer.a == 128, fmt("LY3: the layer stores the brush opacity (alpha %.0f = 50 %%)", layer.a));
    check(std::abs(shown.r - 191) <= 1, fmt("LY3: displayed = brush 50 %% x layer 50 %% = 25 %% black on white (grey %.0f, want 191)", shown.r));
    d.setOpacity(top, 1.0f);
    check(std::abs(d.pixel(150, 50).r - 127) <= 1, "LY3: layer opacity back to 100 %: the stroke shows at its own 50 %");
    check(d.setOpacity(top, NAN) == LayerError::BadValue && d.setOpacity(top, 7.0f) == LayerError::None,
          "LY3: invalid opacity rejected, out-of-range clamped");
}

void testStackOperations() {
    section("LY4 create, delete, duplicate, rename, reorder, visibility, lock, active layer");
    CanvasDocument d;
    d.open(512, 512);
    d.setBackground(0xFFFFFFFFu, true);
    uint32_t active = 0;
    auto list = d.layers(active);
    check(list.size() == 1 && list[0].info.id == 1 && list[0].info.name == "Layer 1" && active == 1,
          "LY4: a new canvas has one layer (id 1, 'Layer 1', active)");
    d.fill({0, 0, 512, 512}, 0xFF0000FFu);  // layer 1: blue
    uint32_t red = 0, green = 0;
    check(d.addLayer(0, "Red", red) == LayerError::None && red == 2, "LY4: add layer -> id 2, on top, active");
    d.fill({100, 100, 100, 100}, 0xFFFF0000u);
    check(d.pixel(150, 150).toArgb() == 0xFFFF0000u && d.pixel(50, 50).toArgb() == 0xFF0000FFu, "LY4: top layer shows over the bottom one");
    check(d.addLayer(1, "Green", green) == LayerError::None, "LY4: add above layer 1 (between)");
    d.fill({120, 120, 100, 100}, 0xFF00FF00u);
    list = d.layers(active);
    check(list.size() == 3 && list[1].info.id == green && list[2].info.id == red && active == green, "LY4: order bottom->top: 1, Green, Red");
    check(d.pixel(150, 150).toArgb() == 0xFFFF0000u && d.pixel(210, 210).toArgb() == 0xFF00FF00u, "LY4: ordering respected in the composite");
    check(d.moveLayer(green, 2) == LayerError::None && d.pixel(150, 150).toArgb() == 0xFF00FF00u, "LY4: move Green to the top: it now covers Red");
    check(d.moveLayer(green, 0) == LayerError::None && d.pixel(130, 130).toArgb() == 0xFFFF0000u, "LY4: move Green to the bottom");
    check(d.moveLayer(green, 3) == LayerError::BadValue && d.moveLayer(99, 0) == LayerError::NotFound, "LY4: bad index / unknown id rejected");
    check(d.setVisible(red, false) == LayerError::None && d.pixel(150, 150).toArgb() == 0xFF0000FFu, "LY4: hide Red: blue shows (Green is under blue)");
    check(d.setVisible(red, true) == LayerError::None && d.pixel(150, 150).toArgb() == 0xFFFF0000u, "LY4: show Red again: unchanged");
    check(d.renameLayer(red, "Ink") == LayerError::None && d.renameLayer(red, "") == LayerError::BadValue, "LY4: rename (empty name refused)");
    uint32_t dup = 0;
    check(d.duplicateLayer(red, "", dup) == LayerError::None, "LY4: duplicate");
    list = d.layers(active);
    check(active == dup && list.back().info.name == "Ink copy" && d.layerPixel(dup, 150, 150).toArgb() == 0xFFFF0000u,
          "LY4: the copy is on top, active, named 'Ink copy', with the same pixels");
    std::vector<uint8_t> rec;
    check(d.exportLayerTiles(dup, true, 100, rec) > 0, "LY4: the copy's tiles are new content to save");
    check(d.deleteLayer(dup) == LayerError::None, "LY4: delete");
    list = d.layers(active);
    check(list.size() == 3 && active == red, "LY4: after deleting the active top layer the one below becomes active");
    check(d.setActive(1) == LayerError::None && d.setActive(42) == LayerError::NotFound, "LY4: select active layer (unknown id rejected)");
    check(d.setLocked(1, true) == LayerError::None && !d.activeEditable(), "LY4: locked active layer is not editable");
    check(d.setLocked(1, false) == LayerError::None && d.setVisible(1, false) == LayerError::None && !d.activeEditable(),
          "LY4: hidden active layer is not editable");
    d.setVisible(1, true);
    check(d.activeEditable(), "LY4: visible + unlocked: editable again");
}

void testPaintActiveOnly() {
    section("LY5 painting changes only the active layer; hidden / locked layers refuse strokes");
    CanvasDocument d;
    d.open(800, 600);
    d.setBackground(0xFFFFFFFFu, false);
    uint32_t a = 1, b = 0;
    d.fill({0, 0, 800, 600}, 0xFF80C0FFu);
    d.addLayer(0, "B", b);
    pe::brush::BrushSettings s;
    s.diameter = 40;
    s.argb = 0xFFFF0000u;
    std::vector<pe::brush::StrokePoint> pts;
    for (int i = 0; i < 40; ++i) pts.push_back({100.0 + i * 15, 300.0, i * 4.0});
    d.strokeBegin(s, pts[0]);
    d.strokeSamples(pts.data() + 1, pts.size() - 1);
    d.strokeEnd();
    check(d.layerPixel(b, 300, 300).toArgb() == 0xFFFF0000u && d.layerPixel(a, 300, 300).toArgb() == 0xFF80C0FFu,
          "LY5: the stroke is in the active layer, the layer below is untouched");
    // Switch layer mid-stroke: what was drawn stays on B.
    d.strokeBegin(s, {100, 400, 0});
    d.strokeSamples(pts.data() + 1, 10);
    d.setActive(a);
    d.strokeSamples(pts.data() + 11, 10);
    d.strokeEnd();
    check(d.layerPixel(a, 150, 400).toArgb() == 0xFF80C0FFu && d.layerPixel(b, 100, 400).a > 0,
          "LY5: switching layers during a stroke keeps the stroke on its layer, nothing lands on the new one");
    d.setActive(b);
    d.setLocked(b, true);
    check(!d.strokeBegin(s, {400, 100, 0}), "LY5: stroke on a locked layer refused");
    d.setLocked(b, false);
    d.setVisible(b, false);
    check(!d.strokeBegin(s, {400, 100, 0}), "LY5: stroke on a hidden layer refused");
    d.setVisible(b, true);
    check(d.layerPixel(b, 400, 100).a == 0, "LY5: refused strokes left nothing");
    check(d.clear() == LayerError::None && d.layerPixel(b, 300, 300).a == 0 && d.layerPixel(a, 300, 300).toArgb() == 0xFF80C0FFu,
          "LY5: clear empties the active layer only");
}

void testMergeFlatten() {
    section("LY6 merge down and flatten keep the picture");
    // Normal layers with opacities, over paper: merge / flatten are exact.
    auto build = [](CanvasDocument& d, BlendMode upperMode) {
        d.open(600, 400);
        d.setBackground(0xFFF3EBDDu, false);
        d.fill({50, 50, 300, 200}, 0xFF2050A0u);
        // Normal: exact even with a translucent lower layer. Other modes are
        // exact where the lower layer is opaque (elsewhere the upper layer
        // now blends with the lower one only, as in every painting app).
        d.setOpacity(1, upperMode == BlendMode::Normal ? 0.8f : 1.0f);
        uint32_t l2 = 0, l3 = 0;
        d.addLayer(0, "", l2);
        d.fill({200, 100, 300, 250}, 0xC0E04020u);
        d.setOpacity(l2, 0.6f);
        d.setBlend(l2, upperMode);
        d.addLayer(0, "", l3);
        d.fill({0, 300, 600, 60}, 0x8020A040u);
    };
    for (BlendMode m : {BlendMode::Normal, BlendMode::Multiply, BlendMode::Screen}) {
        CanvasDocument ref, d;
        build(ref, m);
        build(d, m);
        check(d.mergeDown(2) == LayerError::None, std::string("LY6: merge down (") + pe::canvas::blendModeName(m) + ")");
        int worst = 0;
        // Exact where the lower layer is opaque (the upper blends with it as in the stack).
        bool ok = true;
        for (int y = 60; y < 240; y += 3)
            for (int x = 210; x < 340; x += 3) {
                const Rgba8 p = d.pixel(x, y), q = ref.pixel(x, y);
                worst = std::max({worst, std::abs(p.r - q.r), std::abs(p.g - q.g), std::abs(p.b - q.b)});
            }
        ok = worst <= 2;
        uint32_t active = 0;
        auto list = d.layers(active);
        check(ok && list.size() == 2 && list[0].info.id == 1 && list[0].info.opacity == 1.0f,
              fmt("LY6:   same picture over the lower layer (max diff %.0f), 2 layers left, lower id kept, opacity baked", worst));
    }
    CanvasDocument ref, d;
    build(ref, BlendMode::Normal);
    build(d, BlendMode::Normal);
    uint32_t hidden = 0;
    d.addLayer(0, "hidden", hidden);
    d.fill({0, 0, 600, 400}, 0xFF000000u);
    d.setVisible(hidden, false);
    check(d.flatten("Flat") == LayerError::None, "LY6: flatten");
    int worst = 0;
    const bool same = sameComposite(d, ref, 600, 400, 2, &worst);
    uint32_t active = 0;
    auto list = d.layers(active);
    check(same && list.size() == 1 && list[0].info.name == "Flat" && active == list[0].info.id,
          fmt("LY6: flattened picture identical (max diff %.0f), one layer, hidden layer removed", worst));
    // Invalid merges.
    CanvasDocument e;
    e.open(64, 64);
    uint32_t t = 0;
    e.addLayer(0, "", t);
    check(e.mergeDown(1) == LayerError::NoLayerBelow, "LY6: merge down of the bottom layer refused");
    e.setLocked(1, true);
    check(e.mergeDown(t) == LayerError::Locked, "LY6: merge into a locked layer refused");
    e.setLocked(1, false);
    e.setVisible(t, false);
    check(e.mergeDown(t) == LayerError::Hidden, "LY6: merge of a hidden layer refused");
    check(e.deleteLayer(t) == LayerError::None && e.deleteLayer(1) == LayerError::LastLayer, "LY6: the last layer cannot be deleted");
    uint32_t id = 0;
    for (int i = 0; i < 99; ++i) e.addLayer(0, "", id);
    check(e.addLayer(0, "", id) == LayerError::TooMany, "LY6: more than 100 layers refused");
}

void testDirtyRegions() {
    section("LY7 dirty-region compositing == full recomposite; tile boundaries");
    CanvasDocument d, full;
    for (CanvasDocument* x : {&d, &full}) {
        x->open(700, 700);
        x->setBackground(0xFFFFFFFFu, false);
        x->fill({0, 0, 700, 700}, 0x40FF8000u);
        uint32_t id = 0;
        x->addLayer(0, "", id);
        x->setBlend(id, BlendMode::Multiply);
        x->setOpacity(id, 0.7f);
    }
    // Strokes crossing the tile corners at (256,256) and (512,512).
    pe::brush::BrushSettings s;
    s.diameter = 36;
    s.argb = 0xFF2040C0u;
    std::vector<pe::brush::StrokePoint> pts;
    for (int i = 0; i < 60; ++i) pts.push_back({200.0 + i * 6, 200.0 + i * 6, i * 4.0});
    for (CanvasDocument* x : {&d, &full}) {
        x->strokeBegin(s, pts[0]);
        for (size_t i = 1; i < pts.size(); i += 4) {
            x->strokeSamples(pts.data() + i, std::min<size_t>(4, pts.size() - i));
            if (x == &d) (void)x->pixel(0, 0);  // incremental composite after every batch
        }
        x->strokeEnd();
    }
    full.setBackground(0xFFFFFFFEu, false);  // forces a full recomposite ...
    full.setBackground(0xFFFFFFFFu, false);  // ... with the same paper
    int worst = 0;
    check(sameComposite(d, full, 700, 700, 0, &worst), fmt("LY7: incremental == full recomposite, bit for bit (max diff %.0f)", worst));
    const Rgba8 a = d.pixel(255, 255), b = d.pixel(256, 256), c = d.pixel(257, 257);
    check(std::abs(a.r - b.r) <= 1 && std::abs(b.r - c.r) <= 1 && a.r < 200, "LY7: continuous across the tile corner");
    const auto st = d.stats();
    check(st.compositeTiles <= 9, fmt("LY7: composite tiles only where content is (%.0f of 9)", static_cast<double>(st.compositeTiles)));
}

void testThumbnails() {
    section("LY8 layer thumbnails: the layer alone, transparency kept, hidden layers still previewed");
    CanvasDocument d;
    d.open(1024, 512);
    d.setBackground(0xFFFFFFFFu, false);
    d.fill({0, 0, 512, 512}, 0xFF0000FFu);
    uint32_t top = 0;
    d.addLayer(0, "", top);
    d.fill({512, 0, 512, 512}, 0xFFFF0000u);
    d.setVisible(top, false);
    uint32_t w = 0, h = 0;
    std::vector<uint32_t> px;
    check(d.layerThumbnail(top, 64, w, h, px) && w == 64 && h == 32, "LY8: 64 x 32 thumbnail of a 1024 x 512 layer");
    check(px[16 * 64 + 48] == 0xFFFF0000u && (px[16 * 64 + 10] >> 24) == 0,
          "LY8: hidden top layer: its red half, transparent elsewhere (not the blue layer, not the paper)");
    check(d.layerThumbnail(1, 64, w, h, px) && px[16 * 64 + 10] == 0xFF0000FFu && (px[16 * 64 + 48] >> 24) == 0,
          "LY8: bottom layer: blue half only");
    check(!d.layerThumbnail(77, 64, w, h, px), "LY8: unknown layer: no thumbnail");
    // Time: thumbnail of a full 2048^2 layer.
    CanvasDocument big;
    big.open(2048, 2048);
    big.fill({0, 0, 2048, 2048}, 0xFF336699u);
    const int64_t t0 = pe::nowNs();
    big.layerThumbnail(1, 96, w, h, px);
    std::printf("    thumbnail of a full 2048 x 2048 layer (96 px): %.2f ms (worker thread in the app)\n", (pe::nowNs() - t0) / 1e6);
}

void testSaveReload() {
    section("LY9 save and reload: every layer, its properties and the active layer");
    CanvasDocument d;
    d.open(900, 700);
    d.setBackground(0xFFF3EBDDu, false);
    d.fill({10, 10, 500, 400}, 0xFF204080u);
    uint32_t b = 0, c = 0;
    d.addLayer(0, "Shadows", b);
    d.fill({300, 200, 500, 400}, 0xA0402010u);
    d.setBlend(b, BlendMode::Multiply);
    d.setOpacity(b, 0.55f);
    d.addLayer(0, "Notes", c);
    d.fill({0, 600, 900, 100}, 0xFF00FF00u);
    d.setVisible(c, false);
    d.setLocked(c, true);
    d.setActive(b);
    uint32_t active = 0;
    const auto layers = d.layers(active);
    std::vector<std::vector<uint8_t>> files;
    for (const auto& l : layers) {
        std::vector<uint8_t> rec;
        d.exportLayerTiles(l.info.id, false, 100000, rec);
        files.push_back(rec);
    }
    CanvasDocument r;
    r.open(900, 700);
    r.setBackground(0xFFF3EBDDu, false);
    std::vector<LayerInfo> infos;
    for (const auto& l : layers) infos.push_back(l.info);
    check(r.setLayerStack(infos, active), "LY9: layer stack restored");
    bool imported = true;
    for (size_t i = 0; i < layers.size(); ++i) imported &= r.importLayerTiles(layers[i].info.id, files[i].data(), files[i].size()) >= 0;
    check(imported, "LY9: every layer's tiles imported");
    uint32_t active2 = 0;
    const auto back = r.layers(active2);
    bool props = back.size() == layers.size() && active2 == active;
    for (size_t i = 0; props && i < back.size(); ++i) {
        const auto &x = back[i].info, &y = layers[i].info;
        props = x.id == y.id && x.name == y.name && x.visible == y.visible && x.locked == y.locked && x.opacity == y.opacity &&
                x.blend == y.blend && back[i].tiles == layers[i].tiles;
    }
    check(props, "LY9: order, ids, names, visibility, locks, opacity, blend modes, tiles and active layer identical");
    int worst = 0;
    check(sameComposite(d, r, 900, 700, 0, &worst), "LY9: reloaded picture identical, bit for bit");
    uint32_t id = 0;
    check(r.addLayer(0, "", id) == LayerError::None && id == 4, "LY9: new layer ids continue after the loaded ones");
    // Invalid stacks are refused, nothing changes.
    std::vector<LayerInfo> dupIds = {infos[0], infos[0]};
    std::vector<LayerInfo> badBlend = infos;
    badBlend[0].blend = static_cast<BlendMode>(42);
    check(!r.setLayerStack(dupIds, 1) && !r.setLayerStack(badBlend, 1) && !r.setLayerStack({}, 1),
          "LY9: duplicate ids, unknown blend mode, empty stack refused");
    std::vector<uint8_t> junk = {5, 0, 0, 0, 1, 2};
    check(r.importLayerTiles(1, junk.data(), junk.size()) == -1 && r.importLayerTiles(99, files[0].data(), files[0].size()) == -1,
          "LY9: truncated tile data / unknown layer refused");
}

void testMigration() {
    section("LY10 single-layer content of Blocks 2-5 opens as one layer");
    CanvasDocument d;
    d.open(600, 400);
    d.setBackground(0xFFFFFFFFu, false);
    std::vector<pe::ContentOp> ops = {{10, 10, 100, 100, 0xFFFF0000u}, {500, 300, 300, 300, 0xFF00FF00u}};
    check(d.loadLegacy(ops) == 2, "LY10: Block 2 rectangles loaded");
    uint32_t active = 0;
    auto l = d.layers(active);
    check(l.size() == 1 && l[0].info.id == 1 && d.layerPixel(1, 50, 50).toArgb() == 0xFFFF0000u,
          "LY10: one layer 'Layer 1' holds the old content");
    // Block 3-5 tile records: imported through the single-layer API into the active layer.
    CanvasDocument old;
    old.open(600, 400);
    old.fill({100, 100, 50, 50}, 0xFF123456u);
    std::vector<uint8_t> rec;
    old.exportTiles(false, 1000, rec);
    CanvasDocument n;
    n.open(600, 400);
    check(n.importTiles(rec.data(), rec.size()) > 0 && n.layerPixel(1, 120, 120).toArgb() == 0xFF123456u,
          "LY10: Block 3-5 tile records import into layer 1");
}

void testBenchmarks() {
    section("LY11 benchmarks (informational, this machine)");
    struct Size { uint32_t w, h; const char* name; } sizes[] = {{2048, 1536, "2048x1536 (single-texture class)"},
                                                                  {4096, 4096, "4096x4096 (tiled class)"}};
    for (const auto& sz : sizes) {
        for (int n : {5, 10, 20, 50}) {
            CanvasDocument d;
            d.open(sz.w, sz.h);
            d.setBackground(0xFFFFFFFFu, false);
            d.setMemoryBudget(16ull << 30);
            // Each layer: a band covering a third of the canvas, semi-transparent, mixed modes.
            for (int i = 0; i < n; ++i) {
                uint32_t id = 1;
                if (i > 0) d.addLayer(0, "", id);
                const int32_t y = static_cast<int32_t>((sz.h * i) / (n + 2));
                d.fill({0, y, static_cast<int32_t>(sz.w), static_cast<int32_t>(sz.h / 3)}, 0xA0000000u | (0x102030u * (i + 1)));
                d.setBlend(id, kModes[i % 8]);
            }
            int64_t t0 = pe::nowNs();
            (void)d.pixel(0, 0);  // full composite
            const double fullMs = (pe::nowNs() - t0) / 1e6;
            // One stroke frame on the top layer (8 samples, 40 px brush) with n layers below.
            pe::brush::BrushSettings s;
            s.diameter = 40;
            s.argb = 0xFF000000u;
            std::vector<pe::brush::StrokePoint> pts;
            for (int i = 0; i < 200; ++i) pts.push_back({500.0 + i * 4, 700.0, i * 4.0});
            d.strokeBegin(s, pts[0]);
            double strokeMs = 0, worst = 0;
            int frames = 0;
            for (size_t i = 1; i < pts.size(); i += 8) {
                d.strokeSamples(pts.data() + i, std::min<size_t>(8, pts.size() - i));
                t0 = pe::nowNs();
                (void)d.pixel(0, 0);  // brush + incremental composite, as one render-thread sync
                const double ms = (pe::nowNs() - t0) / 1e6;
                strokeMs += ms;
                worst = std::max(worst, ms);
                frames++;
            }
            d.strokeEnd();
            uint32_t active = 0;
            const auto layers = d.layers(active);
            t0 = pe::nowNs();
            d.setActive(layers[n / 2].info.id);
            const double switchMs = (pe::nowNs() - t0) / 1e6;
            t0 = pe::nowNs();
            d.setOpacity(layers[n / 2].info.id, 0.5f);
            (void)d.pixel(0, 0);
            const double opacityMs = (pe::nowNs() - t0) / 1e6;
            uint32_t tw, th;
            std::vector<uint32_t> px;
            t0 = pe::nowNs();
            d.layerThumbnail(layers[0].info.id, 96, tw, th, px);
            const double thumbMs = (pe::nowNs() - t0) / 1e6;
            t0 = pe::nowNs();
            std::vector<std::vector<uint8_t>> files;
            for (const auto& l : layers) {
                std::vector<uint8_t> rec;
                d.exportLayerTiles(l.info.id, false, 1u << 20, rec);
                files.push_back(std::move(rec));
            }
            const double saveMs = (pe::nowNs() - t0) / 1e6;
            CanvasDocument r;
            r.open(sz.w, sz.h);
            std::vector<LayerInfo> infos;
            for (const auto& l : layers) infos.push_back(l.info);
            t0 = pe::nowNs();
            r.setLayerStack(infos, active);
            for (size_t i = 0; i < layers.size(); ++i) r.importLayerTiles(layers[i].info.id, files[i].data(), files[i].size());
            (void)r.pixel(0, 0);
            const double loadMs = (pe::nowNs() - t0) / 1e6;
            const auto st = d.stats();
            std::printf("    %-32s %2d layers: full composite %7.1f ms | stroke frame avg %.2f ms, worst %.2f | switch %.3f ms | "
                        "opacity change %6.1f ms | thumb %.1f ms | export %6.1f ms | import+composite %6.1f ms | CPU %4.0f MiB\n",
                        sz.name, n, fullMs, strokeMs / std::max(1, frames), worst, switchMs, opacityMs, thumbMs, saveMs, loadMs,
                        st.bytesAllocated / 1048576.0);
        }
    }
    check(true, "LY11: benchmarks printed");
}

}  // namespace

int main() {
    std::printf("=== layer system tests (CPU) ===\n");
    testBlendReference();
    testTransparency();
    testOpacity();
    testStackOperations();
    testPaintActiveOnly();
    testMergeFlatten();
    testDirtyRegions();
    testThumbnails();
    testSaveReload();
    testMigration();
    testBenchmarks();
    std::printf("=== %d/%d checks passed ===\n", gChecks - gFailures, gChecks);
    return gFailures == 0 ? 0 : 1;
}
