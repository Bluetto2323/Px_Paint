// PxPaint - undo / redo history tests (pure CPU, Block 7).
//
// Exactness is checked with checksums of every layer's pixels and of the
// composite (bit for bit, alpha included), plus the layer stack (ids, order,
// properties, active layer), after every undo / redo, including long cycles,
// tile boundaries, big canvases, the memory budget, save / reload, and
// interruptions. The GPU side (what is displayed after undo / redo on Vulkan
// and GLES) is in host_test --suite brush (S13).
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <random>
#include <string>
#include <vector>

#include "core/Time.h"
#include "engine/CanvasDocument.h"
#include "engine/History.h"
#include "PresetFile.h"

using pe::CanvasDocument;
using pe::HistoryKind;
using pe::LayerError;
using pe::LayerInfo;
using pe::canvas::BlendMode;
using pe::canvas::RectI;

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

// Everything that defines the document: stack, properties, active layer, pixels, composite.
struct Snapshot {
    uint32_t active = 0;
    std::vector<LayerInfo> infos;
    std::vector<uint64_t> pixels;
    uint64_t composite = 0;
    bool operator==(const Snapshot& o) const {
        if (active != o.active || infos.size() != o.infos.size() || pixels != o.pixels || composite != o.composite) return false;
        for (size_t i = 0; i < infos.size(); ++i) {
            const auto &a = infos[i], &b = o.infos[i];
            if (a.id != b.id || a.name != b.name || a.visible != b.visible || a.locked != b.locked || a.opacity != b.opacity ||
                a.blend != b.blend)
                return false;
        }
        return true;
    }
};
Snapshot snap(CanvasDocument& d) {
    Snapshot s;
    for (const auto& l : d.layers(s.active)) {
        s.infos.push_back(l.info);
        s.pixels.push_back(d.layerChecksum(l.info.id));
    }
    s.composite = d.compositeChecksum();
    return s;
}

pe::brush::BrushSettings brush(float d, uint32_t argb, float opacity = 1.0f, bool erase = false) {
    pe::brush::BrushSettings b;
    b.diameter = d;
    b.argb = argb;
    b.opacity = opacity;
    b.erase = erase;
    return b;
}
void stroke(CanvasDocument& d, const pe::brush::BrushSettings& b, double x0, double y0, double x1, double y1, int n = 30,
            bool end = true) {
    std::vector<pe::brush::StrokePoint> p;
    for (int i = 0; i < n; ++i) {
        const double t = static_cast<double>(i) / (n - 1);
        p.push_back({x0 + (x1 - x0) * t, y0 + (y1 - y0) * t + 15 * std::sin(t * 6.28), i * 4.0});
    }
    d.strokeBegin(b, p[0]);
    d.strokeSamples(p.data() + 1, p.size() - 1);
    if (end) d.strokeEnd();
}
CanvasDocument& base(CanvasDocument& d, uint32_t w = 900, uint32_t h = 700) {
    d.open(w, h);
    d.setBackground(0xFFF3EBDDu, false);
    return d;
}

// ---------------------------------------------------------------- tests
void testStrokes() {
    section("H1 strokes and erasing: undo / redo restore every pixel exactly (colour and alpha)");
    CanvasDocument d;
    base(d);
    d.fill({100, 100, 300, 300}, 0x80336699u);  // semi-transparent content under the strokes
    d.clearHistory();
    const Snapshot s0 = snap(d);
    stroke(d, brush(40, 0xFFE04020u, 0.7f), 50, 200, 800, 300);
    const Snapshot s1 = snap(d);
    check(!(s1 == s0) && d.historyStats().undoSteps == 1, "H1: one stroke = one history step (not one per dab)");
    check(d.undo() == 1 && snap(d) == s0, "H1: undo: layer and composite identical to before the stroke");
    check(d.redo() == 1 && snap(d) == s1, "H1: redo: identical to after the stroke");
    stroke(d, brush(60, 0xFF000000u, 0.6f, true), 50, 250, 800, 220);  // eraser
    const Snapshot s2 = snap(d);
    const auto e = d.historyEntries();
    check(e.size() == 2 && e.back().kind == HistoryKind::Erase, "H1: the eraser stroke is one 'erase' step");
    check(d.undo() == 1 && snap(d) == s1 && d.redo() == 1 && snap(d) == s2, "H1: eraser undo / redo exact (partially erased alpha restored)");
    check(d.undo() == 1 && d.undo() == 1 && snap(d) == s0 && d.undo() == 0, "H1: nothing more to undo after the first step");
    // A cancelled stroke and a refused stroke leave no step.
    stroke(d, brush(30, 0xFF00FF00u), 100, 600, 700, 600, 20, false);
    d.strokeCancel();
    const Snapshot sc = snap(d);
    check(sc == s0 && d.historyStats().undoSteps == 0, "H1: a cancelled stroke is not in the history");
}

void testSequences() {
    section("H2 consecutive strokes, undo then a new stroke (redo invalidated), chronological order");
    CanvasDocument d;
    base(d);
    std::vector<Snapshot> states{snap(d)};
    for (int i = 0; i < 10; ++i) {
        stroke(d, brush(20.0f + i * 4, 0xFF000000u | (0x151515u * (i + 1)), 0.8f), 40, 60 + i * 60, 860, 90 + i * 55);
        states.push_back(snap(d));
    }
    bool order = true;
    for (int i = 9; i >= 0; --i) order &= d.undo() == 1 && snap(d) == states[static_cast<size_t>(i)];
    check(order, "H2: 10 strokes undone one by one: each intermediate state exact, newest first");
    bool redo = true;
    for (int i = 1; i <= 10; ++i) redo &= d.redo() == 1 && snap(d) == states[static_cast<size_t>(i)];
    check(redo && d.redo() == 0, "H2: redone one by one in the original order");
    for (int i = 0; i < 4; ++i) d.undo();
    stroke(d, brush(50, 0xFF2080FFu), 100, 650, 800, 640);
    const auto st = d.historyStats();
    check(st.redoSteps == 0 && st.undoSteps == 7 && d.redo() == 0, "H2: a new stroke after undo discards the 4 undone steps (redo off)");
    check(d.undo() == 1 && snap(d) == states[6], "H2: undoing the new stroke returns to the state it was drawn on");
}

void testLayerOps() {
    section("H3 layer operations: create, delete, duplicate, reorder, rename, visibility, lock, opacity, blend");
    CanvasDocument d;
    base(d);
    d.fill({0, 0, 900, 700}, 0xFF204060u);
    uint32_t a = 0, b = 0, c = 0;
    d.addLayer(0, "A", a);
    stroke(d, brush(80, 0xFFFFC000u), 50, 100, 850, 500);
    d.addLayer(0, "B", b);
    d.fill({200, 200, 400, 300}, 0xA0C02060u);
    d.clearHistory();
    const Snapshot s0 = snap(d);
    struct Op { const char* name; std::function<void()> run; };
    std::vector<Op> ops = {
        {"add layer", [&] { d.addLayer(a, "C", c); }},
        {"delete a layer with content", [&] { d.deleteLayer(a); }},
        {"duplicate", [&] { uint32_t x; d.duplicateLayer(b, "", x); }},
        {"move", [&] { d.moveLayer(b, 0); }},
        {"rename", [&] { d.renameLayer(b, "Renamed"); }},
        {"hide", [&] { d.setVisible(b, false); }},
        {"lock", [&] { d.setLocked(1, true); }},
        {"opacity", [&] { d.setOpacity(b, 0.3f); }},
        {"blend", [&] { d.setBlend(b, BlendMode::Multiply); }},
    };
    for (auto& op : ops) {
        const Snapshot before = snap(d);
        op.run();
        const Snapshot after = snap(d);
        const bool changed = !(after == before);
        const bool u = d.undo() == 1 && snap(d) == before;
        const bool r = d.redo() == 1 && snap(d) == after;
        check(changed && u && r, std::string("H3: ") + op.name + ": undo restores, redo reapplies, exactly");
    }
    // Undo everything: back to the start.
    while (d.undo() == 1) {
    }
    check(snap(d) == s0, "H3: all nine operations undone: identical stack, properties, pixels, active layer");
    // Deleting restores the SAME layer (id, name, pixels), not a new one.
    const uint64_t pixelsA = d.layerChecksum(a);
    d.deleteLayer(a);
    d.undo();
    uint32_t act = 0;
    const auto l = d.layers(act);
    check(std::any_of(l.begin(), l.end(), [&](const pe::LayerSummary& x) { return x.info.id == a && x.info.name == "A"; }) &&
              d.layerChecksum(a) == pixelsA,
          "H3: undo delete: the same layer (id, name, pixels) is back at its place");
    // An opacity slider drag is one step.
    const size_t steps = d.historyStats().undoSteps;
    for (int i = 0; i <= 20; ++i) d.setOpacity(b, 1.0f - i * 0.03f);
    check(d.historyStats().undoSteps == steps + 1, "H3: 21 opacity changes in a row on one layer = one step");
    d.undo();
    check(snap(d) == s0, "H3: undoing the drag restores the original opacity");
    check(d.setActive(1) == LayerError::None && d.historyStats().redoSteps == 1, "H3: selecting a layer is not an edit (redo kept)");
    // Order: a stroke still queued when a property changes is recorded first.
    CanvasDocument o;
    base(o);
    for (auto op : {0, 1, 2}) {
        stroke(o, brush(30, 0xFF000000u), 50, 100 + op * 100, 850, 120 + op * 100);  // queued, not processed yet
        if (op == 0) o.setOpacity(1, 0.5f);
        if (op == 1) o.setBlend(1, BlendMode::Screen);
        if (op == 2) o.renameLayer(1, "Later");
    }
    const auto oe = o.historyEntries();
    const std::vector<HistoryKind> want = {HistoryKind::Stroke, HistoryKind::Opacity, HistoryKind::Stroke, HistoryKind::Blend,
                                           HistoryKind::Stroke, HistoryKind::RenameLayer};
    bool inOrder = oe.size() == want.size();
    for (size_t i = 0; inOrder && i < want.size(); ++i) inOrder = oe[i].kind == want[i];
    check(inOrder && oe[0].label == "Layer 1", "H3: queued strokes are recorded before a following opacity / blend / rename (chronological order)");
}

void testMergeFlatten() {
    section("H4 merge down and flatten: undo restores the original layers exactly");
    CanvasDocument d;
    base(d);
    d.fill({50, 50, 500, 400}, 0xFF3060A0u);
    d.setOpacity(1, 0.8f);
    uint32_t u = 0, h = 0;
    d.addLayer(0, "Upper", u);
    stroke(d, brush(70, 0xC0E03030u), 100, 300, 800, 200);
    d.setBlend(u, BlendMode::Screen);
    d.setOpacity(u, 0.6f);
    d.addLayer(0, "Hidden", h);
    d.fill({0, 600, 900, 100}, 0xFF00FF00u);
    d.setVisible(h, false);
    const Snapshot s0 = snap(d);
    check(d.mergeDown(u) == LayerError::None, "H4: merge down");
    const Snapshot merged = snap(d);
    check(d.undo() == 1 && snap(d) == s0, "H4: undo merge: both layers back (ids, modes, opacities, pixels)");
    check(d.redo() == 1 && snap(d) == merged, "H4: redo merge: identical merged result");
    d.undo();
    check(d.flatten("Flat") == LayerError::None, "H4: flatten (with a hidden layer)");
    const Snapshot flat = snap(d);
    check(d.undo() == 1 && snap(d) == s0, "H4: undo flatten: every layer back, hidden one included");
    check(d.redo() == 1 && snap(d) == flat, "H4: redo flatten: identical");
    d.undo();
    check(d.clear() == LayerError::None, "H4: clear layer");
    const Snapshot cleared = snap(d);
    check(d.undo() == 1 && snap(d) == s0 && d.redo() == 1 && snap(d) == cleared, "H4: clear layer undo / redo exact");
}

void testPresets() {
    section("H4b the 12 shipped brush presets (grain, jitter, buildup, wet edge): queued back to back, each one step, exact");
    const auto presets = pxtest::loadPresets(PE_BRUSH_ASSET);
    CanvasDocument d;
    base(d, 1600, 2400);
    std::vector<Snapshot> states{snap(d)};
    // Queued without processing in between, like the app's demo and fast drawing.
    for (size_t i = 0; i < presets.size(); ++i) {
        auto b = presets[i].s;
        b.diameter = std::min(presets[i].size, 100.0f);
        b.argb = 0xFF000000u | (0x1F2A44u + static_cast<uint32_t>(i) * 0x0A0B0Cu);
        const double y = 150 + static_cast<double>(i) * 180;
        stroke(d, b, 100, y, 1500, y + 40, 40);
    }
    const auto e = d.historyEntries();
    check(presets.size() == 12 && e.size() == 12, fmt("H4b: 12 queued preset strokes = 12 steps (%.0f)", static_cast<double>(e.size())));
    // Exactness per preset: undo one by one, compare with a document that draws only the first k strokes.
    bool exact = true;
    for (size_t k = presets.size(); k-- > 0;) {
        CanvasDocument ref;
        base(ref, 1600, 2400);
        for (size_t i = 0; i < k; ++i) {
            auto b = presets[i].s;
            b.diameter = std::min(presets[i].size, 100.0f);
            b.argb = 0xFF000000u | (0x1F2A44u + static_cast<uint32_t>(i) * 0x0A0B0Cu);
            const double y = 150 + static_cast<double>(i) * 180;
            stroke(ref, b, 100, y, 1500, y + 40, 40);
        }
        exact &= d.undo() == 1 && snap(d).pixels == snap(ref).pixels;
    }
    check(exact, "H4b: undoing preset by preset gives exactly the canvas drawn without the later presets");
    bool redo = true;
    while (d.redo() == 1) {
    }
    CanvasDocument all;
    base(all, 1600, 2400);
    for (size_t i = 0; i < presets.size(); ++i) {
        auto b = presets[i].s;
        b.diameter = std::min(presets[i].size, 100.0f);
        b.argb = 0xFF000000u | (0x1F2A44u + static_cast<uint32_t>(i) * 0x0A0B0Cu);
        const double y = 150 + static_cast<double>(i) * 180;
        stroke(all, b, 100, y, 1500, y + 40, 40);
    }
    redo &= snap(d).pixels == snap(all).pixels;
    check(redo, "H4b: redoing all of them gives exactly the full drawing");
}

void testTilesAndLarge() {
    section("H5 tile boundaries and large canvases: exact, memory proportional to the change");
    CanvasDocument d;
    base(d, 8192, 8192);
    const Snapshot s0 = snap(d);
    // Long diagonal stroke over ~32 tiles, crossing many tile corners.
    stroke(d, brush(48, 0xFF101010u, 0.9f), 100, 100, 8000, 8000, 200);
    const Snapshot s1 = snap(d);
    const auto st = d.historyStats();
    check(d.undo() == 1 && snap(d) == s0 && d.redo() == 1 && snap(d) == s1, "H5: 8192 x 8192: a stroke across ~60 tiles undone / redone exactly");
    const double canvasMiB = 8192.0 * 8192 * 4 / 1048576.0;
    std::printf("    stroke step: %.2f MiB of history (the canvas is %.0f MiB)\n", st.bytes / 1048576.0, canvasMiB);
    check(st.bytes < 4u << 20, "H5: the step stores the changed pixels only (< 4 MiB, no canvas snapshot)");
    // Big brush.
    CanvasDocument e;
    base(e, 2048, 2048);
    const Snapshot e0 = snap(e);
    stroke(e, brush(400, 0xFFC08030u, 0.5f), 200, 1000, 1850, 1000, 40);
    const Snapshot e1 = snap(e);
    check(e.undo() == 1 && snap(e) == e0 && e.redo() == 1 && snap(e) == e1, "H5: 400 px brush undone / redone exactly");
    std::printf("    400 px stroke: %.2f MiB of history\n", e.historyStats().bytes / 1048576.0);
}

void testBudget() {
    section("H6 memory budget and step limit: oldest steps dropped, the canvas never touched");
    CanvasDocument d;
    base(d, 2048, 2048);
    d.setHistoryBudget(3u << 20, 100);  // 3 MiB
    std::vector<Snapshot> states{snap(d)};
    for (int i = 0; i < 30; ++i) {
        stroke(d, brush(120, 0xFF000000u | (0x080808u * i), 0.9f), 50, 60 + i * 65, 2000, 80 + i * 64, 40);
        states.push_back(snap(d));
    }
    const auto st = d.historyStats();
    std::printf("    30 strokes of 120 px: %zu steps kept, %.2f MiB (budget 3 MiB), %llu dropped\n", st.undoSteps,
                st.bytes / 1048576.0, static_cast<unsigned long long>(st.dropped));
    check(st.bytes <= st.budget && st.dropped > 0 && st.undoSteps >= 1, "H6: history stays within its budget by dropping the oldest steps");
    check(snap(d) == states.back(), "H6: the current canvas is untouched by the trimming");
    size_t n = 0;
    bool exact = true;
    while (d.undo() == 1) {
        ++n;
        exact &= snap(d) == states[states.size() - 1 - n];
    }
    check(exact && n == st.undoSteps, "H6: every kept step still undoes exactly; then nothing more (oldest gone)");
    // Step limit.
    CanvasDocument e;
    base(e, 512, 512);
    e.setHistoryBudget(1ull << 30, 10);
    for (int i = 0; i < 25; ++i) stroke(e, brush(10, 0xFF000000u), 10, 10 + i * 20, 500, 10 + i * 20, 10);
    check(e.historyStats().undoSteps == 10, "H6: step limit (10) respected");
    // A single step larger than the budget is still kept (the latest step is always undoable).
    CanvasDocument f;
    base(f, 2048, 2048);
    f.setHistoryBudget(1024, 100);
    stroke(f, brush(300, 0xFF000000u), 100, 1000, 1900, 1000, 40);
    check(f.historyStats().undoSteps == 1 && f.undo() == 1, "H6: the newest step is kept even beyond the budget");
}

void testCycles() {
    section("H7 repeated undo / redo cycles: no drift");
    CanvasDocument d;
    base(d);
    uint32_t l2 = 0;
    stroke(d, brush(30, 0x90FF0000u, 0.5f), 50, 100, 850, 600);
    d.addLayer(0, "", l2);
    d.setBlend(l2, BlendMode::Multiply);
    stroke(d, brush(90, 0xC00000FFu, 0.7f), 50, 600, 850, 100);
    stroke(d, brush(40, 0xFF000000u, 1.0f, true), 400, 50, 400, 650);
    const Snapshot end = snap(d);
    while (d.undo() == 1) {
    }
    const Snapshot start = snap(d);
    bool same = true;
    for (int i = 0; i < 100; ++i) {
        while (d.redo() == 1) {
        }
        same &= snap(d) == end;
        while (d.undo() == 1) {
        }
        same &= snap(d) == start;
    }
    check(same, "H7: 100 full undo / redo cycles (strokes, layer, blend, eraser): start and end states bit-identical every time");
}

void testSaveLoad() {
    section("H8 save / load after undo and after redo: the saved project is the current state");
    auto saveLoad = [](CanvasDocument& d) {
        uint32_t active = 0;
        const auto layers = d.layers(active);
        CanvasDocument r;
        base(r);
        std::vector<LayerInfo> infos;
        for (const auto& l : layers) infos.push_back(l.info);
        r.setLayerStack(infos, active);
        for (const auto& l : layers) {
            std::vector<uint8_t> rec;
            d.exportLayerTiles(l.info.id, false, 100000, rec);
            r.importLayerTiles(l.info.id, rec.data(), rec.size());
        }
        return snap(r);
    };
    CanvasDocument d;
    base(d);
    stroke(d, brush(40, 0xFF802020u), 50, 100, 850, 200);
    uint32_t l2 = 0;
    d.addLayer(0, "Top", l2);
    stroke(d, brush(60, 0xFF208020u), 50, 400, 850, 300);
    d.undo();
    check(saveLoad(d) == snap(d), "H8: saved after undo: reload equals the undone state");
    d.redo();
    check(saveLoad(d) == snap(d), "H8: saved after redo: reload equals the redone state");
    // Incremental save path: tiles changed by undo are re-exported as changed (and emptied ones as empty).
    std::vector<uint8_t> rec;
    d.exportLayerTiles(l2, true, 100000, rec);  // "saved"
    d.undo();
    rec.clear();
    const size_t n = d.exportLayerTiles(l2, true, 100000, rec);
    check(n > 0, fmt("H8: the tiles an undo changed are re-exported by the next incremental save (%.0f tiles)", n));
    // Loading clears the history.
    CanvasDocument r;
    base(r);
    stroke(r, brush(20, 0xFF000000u), 10, 10, 800, 600);
    uint32_t act = 0;
    auto layers = r.layers(act);
    std::vector<LayerInfo> infos;
    for (const auto& l : layers) infos.push_back(l.info);
    r.setLayerStack(infos, act);
    check(r.historyStats().undoSteps == 0 && r.undo() == 0, "H8: a loaded document starts with an empty history");
}

// The app's incremental save, as WorkspaceScreen.save does it: per layer only the
// tiles changed since the last export (empty ones remove the tile from the file),
// files of layers no longer in the stack deleted. Reloading = new document from "disk".
struct Disk {
    std::vector<LayerInfo> stack;
    uint32_t active = 0;
    std::map<uint32_t, std::map<std::pair<int32_t, int32_t>, std::vector<uint8_t>>> files;
    void save(CanvasDocument& d) {
        stack.clear();
        for (const auto& l : d.layers(active)) stack.push_back(l.info);
        for (const auto& info : stack) {
            auto& f = files[info.id];
            std::vector<uint8_t> rec;
            d.exportLayerTiles(info.id, true, 1000000, rec);
            int32_t n = 0;
            std::memcpy(&n, rec.data(), 4);
            size_t off = 4;
            for (int32_t i = 0; i < n; ++i) {
                int32_t h[3];
                std::memcpy(h, rec.data() + off, 12);
                off += 12;
                if (h[2] & 1) {
                    f.erase({h[0], h[1]});
                } else {
                    f[{h[0], h[1]}].assign(rec.data() + off, rec.data() + off + pe::canvas::RasterStore::kTileBytes);
                    off += pe::canvas::RasterStore::kTileBytes;
                }
            }
        }
        for (auto it = files.begin(); it != files.end();) {
            const bool present = std::any_of(stack.begin(), stack.end(), [&](const LayerInfo& i) { return i.id == it->first; });
            it = present ? std::next(it) : files.erase(it);
        }
    }
    Snapshot reload(uint32_t w, uint32_t h) {
        CanvasDocument r;
        base(r, w, h);
        r.setLayerStack(stack, active);
        for (const auto& info : stack) {
            std::vector<uint8_t> rec(4);
            int32_t n = 0;
            for (const auto& [k, px] : files[info.id]) {
                const int32_t hd[3] = {k.first, k.second, 0};
                rec.insert(rec.end(), reinterpret_cast<const uint8_t*>(hd), reinterpret_cast<const uint8_t*>(hd) + 12);
                rec.insert(rec.end(), px.begin(), px.end());
                n++;
            }
            std::memcpy(rec.data(), &n, 4);
            r.importLayerTiles(info.id, rec.data(), rec.size());
        }
        return snap(r);
    }
};

void testIncrementalSaves() {
    section("H8b incremental saves between every operation, undo and redo: the reloaded project is always the current state");
    CanvasDocument d;
    base(d, 700, 600);
    Disk disk;
    disk.save(d);
    bool all = true;
    int n = 0;
    auto step = [&](const char* what, const std::function<void()>& f) {
        f();
        disk.save(d);
        const bool ok = disk.reload(700, 600) == snap(d);
        if (!ok) std::printf("    mismatch after: %s\n", what);
        all &= ok;
        n++;
    };
    uint32_t a = 0, b = 0;
    step("stroke", [&] { stroke(d, brush(50, 0xFF3060C0u), 20, 100, 680, 300); });
    step("add layer", [&] { d.addLayer(0, "A", a); });
    step("stroke on A", [&] { stroke(d, brush(70, 0xC0E04020u), 20, 400, 680, 200); });
    step("duplicate A", [&] { d.duplicateLayer(a, "", b); });
    step("erase on the copy", [&] { stroke(d, brush(90, 0xFF000000u, 1.0f, true), 300, 0, 300, 600); });
    step("delete A", [&] { d.deleteLayer(a); });
    step("undo delete", [&] { d.undo(); });
    step("redo delete", [&] { d.redo(); });
    step("undo delete again", [&] { d.undo(); });
    step("merge copy down", [&] { d.mergeDown(b); });
    step("undo merge", [&] { d.undo(); });
    step("redo merge", [&] { d.redo(); });
    step("undo merge again", [&] { d.undo(); });
    step("flatten", [&] { d.flatten("F"); });
    step("undo flatten", [&] { d.undo(); });
    step("clear the copy", [&] { d.setActive(b); d.clear(); });
    step("undo clear", [&] { d.undo(); });
    step("undo erase .. everything", [&] { while (d.undo() == 1) { } });
    step("redo everything", [&] { while (d.redo() == 1) { } });
    check(all, fmt("H8b: %.0f save points (strokes, erase, add, duplicate, delete, merge, flatten, clear and their undo / redo): reload identical every time", n));
}

void testInterruption() {
    section("H9 interruptions: a stroke in progress, refused patches, layer switches mid-stroke");
    CanvasDocument d;
    base(d);
    // Interrupted stroke: before a stroke ends it is not a step...
    stroke(d, brush(30, 0xFF000000u), 50, 100, 850, 120, 30, false);
    check(d.historyStats().undoSteps == 0, "H9: a stroke still in progress is not in the history yet");
    // ...the app is paused: the pause-save commits what was drawn so far as one complete step,
    // so the saved file and the history agree; the rest of the gesture adds nothing.
    std::vector<uint8_t> rec;
    d.exportLayerTiles(1, true, 1000, rec);
    const Snapshot drawn = snap(d);
    check(d.historyStats().undoSteps == 1, "H9: the pause-save commits the interrupted stroke as one step");
    stroke(d, brush(30, 0xFF000000u), 0, 0, 0, 0, 2, false);  // (a new gesture would be a new stroke)
    d.strokeCancel();
    d.strokeEnd();
    check(d.historyStats().undoSteps == 1 && snap(d) == drawn, "H9: still one step, with exactly what was drawn");
    check(d.undo() == 1 && d.redo() == 1 && snap(d) == drawn, "H9: the interrupted stroke undoes / redoes exactly");
    // Undo during a stroke: the stroke is committed first, then undone.
    const Snapshot beforeSecond = snap(d);
    stroke(d, brush(30, 0xFF0000FFu), 50, 400, 850, 420, 30, false);
    check(d.undo() == 1 && snap(d) == beforeSecond, "H9: undo while a stroke is in progress removes that stroke");
    d.strokeEnd();
    check(snap(d) == beforeSecond, "H9: the rest of that stroke paints nothing");
    // A change outside the history makes an undo refuse, without touching the canvas.
    CanvasDocument e;
    base(e);
    stroke(e, brush(40, 0xFF000000u), 50, 100, 850, 120);
    std::vector<uint8_t> outside;
    {
        CanvasDocument other;
        base(other);
        other.fill({0, 0, 900, 700}, 0xFFFF00FFu);
        other.exportLayerTiles(1, false, 1000, outside);
    }
    e.importLayerTiles(1, outside.data(), outside.size());  // e.g. a reload into the open document
    const Snapshot imported = snap(e);
    check(e.historyStats().undoSteps == 0 && e.undo() == 0 && snap(e) == imported,
          "H9: pixels replaced from outside clear the history: no stale undo can damage them");
}


void testPatch() {
    section("H11 pixel patches: XOR+RLE deltas, partial tiles, verification before applying");
    using pe::PixelPatch;
    using pe::canvas::RasterStore;
    RasterStore st;
    st.reset(600, 600);
    // Tile (0,0) exists with a gradient; (1,0) does not exist before.
    uint8_t* t0 = st.tileForWrite(0, 0);
    for (size_t i = 0; i < RasterStore::kTileBytes; ++i) t0[i] = static_cast<uint8_t>(i * 7 + (i >> 10));
    std::vector<uint8_t> before0(t0, t0 + RasterStore::kTileBytes);
    const uint64_t h0 = PixelPatch::hashRect(t0, st.tileRect(0, 0), 0, 0);
    // Change a 40x30 region of tile 0 and create tile 1 with a small dot.
    const RectI r0{100, 50, 40, 30};
    for (int y = r0.y; y < r0.bottom(); ++y)
        for (int x = r0.x; x < r0.right(); ++x) std::memset(t0 + (static_cast<size_t>(y) * RasterStore::kTile + x) * 4, 0x55, 4);
    uint8_t* t1 = st.tileForWrite(1, 0);
    std::memset(t1, 0, RasterStore::kTileBytes);
    const RectI r1{256 + 10, 10, 8, 8};
    for (int y = r1.y; y < r1.bottom(); ++y)
        for (int x = 10; x < 18; ++x) std::memset(t1 + (static_cast<size_t>(y) * RasterStore::kTile + x) * 4, 0xC0, 4);
    std::vector<uint8_t> after0(t0, t0 + RasterStore::kTileBytes), after1(t1, t1 + RasterStore::kTileBytes);
    PixelPatch p;
    p.addTile(0, 0, r0, before0.data(), t0);
    p.addTile(1, 0, r1, nullptr, t1);
    std::printf("    patch: 2 tiles, %llu bytes (one raw tile = %zu)\n", static_cast<unsigned long long>(p.bytes()),
                RasterStore::kTileBytes);
    check(p.bytes() < 8000, "H11: the patch holds only the changed rectangles, compressed");
    RectI changed;
    check(p.apply(st, true, &changed) && std::memcmp(st.tile(0, 0), before0.data(), RasterStore::kTileBytes) == 0 &&
              !st.tileAllocated(1, 0) && PixelPatch::hashRect(st.tile(0, 0), st.tileRect(0, 0), 0, 0) == h0,
          "H11: undo: tile 0 bit-identical to before, tile 1 (new) released again");
    check(p.apply(st, false, &changed) && std::memcmp(st.tile(0, 0), after0.data(), RasterStore::kTileBytes) == 0 &&
              st.tileAllocated(1, 0) && std::memcmp(st.tile(1, 0), after1.data(), RasterStore::kTileBytes) == 0,
          "H11: redo: both tiles bit-identical to after");
    // Tamper with one pixel inside the recorded rectangle: the patch must refuse, changing nothing.
    st.tileForWrite(0, 0)[(static_cast<size_t>(60) * RasterStore::kTile + 110) * 4 + 3] ^= 1;
    std::vector<uint8_t> tampered(st.tile(0, 0), st.tile(0, 0) + RasterStore::kTileBytes);
    check(!p.apply(st, true, &changed) && std::memcmp(st.tile(0, 0), tampered.data(), RasterStore::kTileBytes) == 0 &&
              std::memcmp(st.tile(1, 0), after1.data(), RasterStore::kTileBytes) == 0,
          "H11: a store not in the recorded state is refused and left untouched (never a wrong 'restore')");
}

void testBenchmarks() {
    section("H12 benchmarks (informational, this machine)");
    for (int count : {100, 1000}) {
        CanvasDocument d;
        base(d, 2048, 1536);
        d.setHistoryBudget(256ull << 20, 2000);
        std::mt19937 rng(7);
        std::uniform_real_distribution<double> X(50, 2000), Y(50, 1480);
        double recordMax = 0, recordSum = 0;
        const int64_t t0 = pe::nowNs();
        for (int i = 0; i < count; ++i) {
            stroke(d, brush(8 + (i % 5) * 12.0f, 0xFF000000u | static_cast<uint32_t>(rng()), 0.8f), X(rng), Y(rng), X(rng), Y(rng), 24);
            (void)d.pixel(0, 0);  // processed, as by a frame
            const double r = d.historyStats().lastRecordMs;
            recordMax = std::max(recordMax, r);
            recordSum += r;
        }
        const double drawMs = (pe::nowNs() - t0) / 1e6;
        const auto st = d.historyStats();
        double undoMax = 0, undoSum = 0, redoMax = 0, redoSum = 0;
        int n = 0;
        while (true) {
            const int64_t a = pe::nowNs();
            if (d.undo() != 1) break;
            (void)d.pixel(0, 0);  // + recomposite, as the frame after it
            const double ms = (pe::nowNs() - a) / 1e6;
            undoMax = std::max(undoMax, ms);
            undoSum += ms;
            n++;
        }
        int m = 0;
        while (true) {
            const int64_t a = pe::nowNs();
            if (d.redo() != 1) break;
            (void)d.pixel(0, 0);
            const double ms = (pe::nowNs() - a) / 1e6;
            redoMax = std::max(redoMax, ms);
            redoSum += ms;
            m++;
        }
        std::printf("    %4d strokes on 2048x1536: history %.1f MiB (%zu steps) | recording at stroke end avg %.3f ms, max %.3f | "
                    "undo avg %.3f ms, max %.3f | redo avg %.3f ms, max %.3f | total drawing %.0f ms\n",
                    count, st.bytes / 1048576.0, st.undoSteps, recordSum / count, recordMax, undoSum / std::max(1, n), undoMax,
                    redoSum / std::max(1, m), redoMax, drawMs);
    }
    // Large brush, large canvas.
    {
        CanvasDocument d;
        base(d, 4096, 4096);
        stroke(d, brush(500, 0xFF336699u, 0.8f), 200, 2000, 3900, 2100, 60);
        (void)d.pixel(0, 0);
        const auto st = d.historyStats();
        int64_t a = pe::nowNs();
        d.undo();
        (void)d.pixel(0, 0);
        const double u = (pe::nowNs() - a) / 1e6;
        a = pe::nowNs();
        d.redo();
        (void)d.pixel(0, 0);
        const double r = (pe::nowNs() - a) / 1e6;
        std::printf("    500 px stroke across a 4096 canvas: %.1f MiB of history, recorded in %.2f ms, undo %.2f ms, redo %.2f ms\n",
                    st.bytes / 1048576.0, st.lastRecordMs, u, r);
    }
    // Many layers: undo of layer operations.
    for (int layers : {20, 50}) {
        CanvasDocument d;
        base(d, 2048, 1536);
        d.setMemoryBudget(8ull << 30);
        for (int i = 0; i < layers; ++i) {
            uint32_t id = 1;
            if (i > 0) d.addLayer(0, "", id);
            d.fill({0, static_cast<int32_t>((1536 * i) / (layers + 2)), 2048, 512}, 0xA0000000u | (0x0F1F2Fu * (i + 1)));
        }
        (void)d.pixel(0, 0);
        uint32_t act = 0;
        auto l = d.layers(act);
        auto time = [&](auto&& f) {
            const int64_t a = pe::nowNs();
            f();
            (void)d.pixel(0, 0);
            return (pe::nowNs() - a) / 1e6;
        };
        const double del = time([&] { d.deleteLayer(l[layers / 2].info.id); });
        const double undel = time([&] { d.undo(); });
        const double merge = time([&] { d.mergeDown(l[layers - 1].info.id); });
        const double unmerge = time([&] { d.undo(); });
        const double flat = time([&] { d.flatten("F"); });
        const double unflat = time([&] { d.undo(); });
        const double reflat = time([&] { d.redo(); });
        std::printf("    %d layers 2048x1536 (operation / undo incl. recomposite): delete %.1f / %.1f ms | merge %.1f / %.1f ms | "
                    "flatten %.1f / %.1f ms, redo %.1f ms | history %.1f MiB\n",
                    layers, del, undel, merge, unmerge, flat, unflat, reflat, d.historyStats().bytes / 1048576.0);
    }
    check(true, "H12: benchmarks printed");
}

}  // namespace

int main() {
    std::printf("=== history tests (CPU) ===\n");
    testStrokes();
    testSequences();
    testLayerOps();
    testMergeFlatten();
    testPresets();
    testTilesAndLarge();
    testBudget();
    testCycles();
    testSaveLoad();
    testIncrementalSaves();
    testInterruption();
    testPatch();
    testBenchmarks();
    std::printf("=== %d/%d checks passed ===\n", gChecks - gFailures, gChecks);
    return gFailures == 0 ? 0 : 1;
}
