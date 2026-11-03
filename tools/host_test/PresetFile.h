// PxPaint host tests - reads the SHIPPED brush preset file
// (app/src/main/assets/brushes/builtin.pxb) into engine BrushSettings, with
// the same key mapping as the app (core/BrushPresets.kt + NativeEngine.Brush):
// the engine is tested with exactly the presets users get.
#pragma once

#include <cmath>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

#include "brush/BrushTypes.h"

namespace pxtest {

struct TestPreset {
    std::string id;
    std::string name;
    std::string category;
    pe::brush::BrushSettings s;
    float size = 24.0f;
};

inline std::string trim(const std::string& v) {
    const size_t a = v.find_first_not_of(" \t\r");
    if (a == std::string::npos) return "";
    const size_t b = v.find_last_not_of(" \t\r");
    return v.substr(a, b - a + 1);
}

inline void applyKey(TestPreset& p, const std::string& k, const std::string& v) {
    using namespace pe::brush;
    BrushSettings& s = p.s;
    const float f = static_cast<float>(std::atof(v.c_str()));
    const bool b = v == "true";
    if (k == "name") p.name = v;
    else if (k == "category") p.category = v;
    else if (k == "size") { p.size = f; s.diameter = f; }
    else if (k == "opacity") s.opacity = f;
    else if (k == "flow") s.flow = f;
    else if (k == "spacing") s.spacing = f;
    else if (k == "hardness") s.hardness = f;
    else if (k == "roundness") s.roundness = f;
    else if (k == "angle") s.angle = static_cast<float>(f * 3.14159265358979323846 / 180.0);
    else if (k == "angle_mode") s.angleFollowsStroke = v == "stroke";
    else if (k == "size_jitter") s.sizeJitter = f;
    else if (k == "scatter") s.scatter = f;
    else if (k == "angle_jitter") s.angleJitter = f;
    else if (k == "flow_jitter") s.flowJitter = f;
    else if (k == "grain_depth") s.grainDepth = f;
    else if (k == "grain_pressure") s.grainPressure = f;
    else if (k == "wet_edge") s.wetEdge = f;
    else if (k == "pressure_size") s.pressureSize = b;
    else if (k == "min_size") s.minSizeFraction = f;
    else if (k == "pressure_opacity") s.pressureOpacity = b;
    else if (k == "min_opacity") s.minOpacityFraction = f;
    else if (k == "tilt") s.tiltShape = b;
    else if (k == "tilt_strength") s.tiltStrength = f;
    else if (k == "accumulation") s.accumulation = v == "buildup" ? Accumulation::Buildup : Accumulation::StrokeMax;
    else if (k == "tip") {
        s.tip = v == "chalk" ? BrushTip::Chalk : v == "charcoal" ? BrushTip::Charcoal : v == "bristle" ? BrushTip::Bristle : BrushTip::Round;
    } else if (k == "grain") {
        s.grain = v == "paper" ? BrushGrain::Paper : v == "rough" ? BrushGrain::Rough : v == "canvas" ? BrushGrain::Canvas
                  : v == "wash" ? BrushGrain::Wash : BrushGrain::None;
    }
}

inline std::vector<TestPreset> loadPresets(const char* path, bool* headerOk = nullptr) {
    std::vector<TestPreset> out;
    std::ifstream in(path);
    std::string line;
    bool header = false;
    while (std::getline(in, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#') continue;
        if (line == "pxbrushes 1") { header = true; continue; }
        if (line.rfind("[preset ", 0) == 0 && line.back() == ']') {
            TestPreset p;
            p.id = trim(line.substr(8, line.size() - 9));
            p.s.pressureSize = false;  // app defaults (BrushParams)
            p.s.minSizeFraction = 0.25f;
            p.s.minOpacityFraction = 0.3f;
            out.push_back(p);
            continue;
        }
        const size_t eq = line.find('=');
        if (eq == std::string::npos || out.empty()) continue;
        applyKey(out.back(), trim(line.substr(0, eq)), trim(line.substr(eq + 1)));
    }
    if (headerOk) *headerOk = header;
    return out;
}

inline const TestPreset* find(const std::vector<TestPreset>& v, const std::string& id) {
    for (const auto& p : v) if (p.id == id) return &p;
    return nullptr;
}

}  // namespace pxtest
