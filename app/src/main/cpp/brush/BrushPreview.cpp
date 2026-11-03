#include "brush/BrushPreview.h"

#include <cmath>
#include <vector>

#include "brush/BrushEngine.h"
#include "brush/StrokeCompositor.h"
#include "brush/StrokePath.h"
#include "canvas/RasterStore.h"

namespace pe::brush {

bool renderBrushPreview(const BrushSettings& brush, const PreviewStroke* strokes, size_t strokeCount, int32_t width,
                        int32_t height, uint32_t backgroundArgb, uint32_t* out) {
    if (!out || width <= 0 || height <= 0 || width > kMaxPreviewSide || height > kMaxPreviewSide) return false;
    canvas::RasterStore store;
    store.reset(static_cast<uint32_t>(width), static_cast<uint32_t>(height));
    StrokeCompositor comp;
    StrokePath path;
    std::vector<Dab> dabs;
    for (size_t si = 0; si < strokeCount; ++si) {
        const PreviewStroke& ps = strokes[si];
        if (!ps.points || ps.count == 0) continue;
        BrushSettings s = brush;
        if (ps.diameter > 0.0f) s.diameter = ps.diameter;
        if (ps.opacity >= 0.0f) s.opacity = ps.opacity;
        s.seed = BrushEngine::strokeSeed(ps.points[0]);
        // As BrushEngine: what the first sample carries decides the dynamics;
        // axes the stroke does not use are cleared.
        const bool hasP = std::isfinite(ps.points[0].axes.pressure);
        const bool hasT = std::isfinite(ps.points[0].axes.tilt) && std::isfinite(ps.points[0].axes.orientation);
        auto prep = [&](StrokePoint p) {
            if (!hasP) p.axes.pressure = kNoValue;
            if (!hasT) p.axes.tilt = p.axes.orientation = kNoValue;
            return p;
        };
        comp.begin(store, s, hasP, hasT);
        path.begin(prep(ps.points[0]), comp.settings().spacingPx(), &comp.dynamics());
        for (size_t i = 1; i < ps.count; ++i) {
            path.add(prep(ps.points[i]));
            dabs.clear();
            path.takeCommitted(dabs);
            comp.addDabs(store, dabs.data(), dabs.size());
        }
        path.finish();
        dabs.clear();
        path.takeCommitted(dabs);
        comp.addDabs(store, dabs.data(), dabs.size());
        comp.commit(store);
    }
    const float br = static_cast<float>((backgroundArgb >> 16) & 0xFF);
    const float bg = static_cast<float>((backgroundArgb >> 8) & 0xFF);
    const float bb = static_cast<float>(backgroundArgb & 0xFF);
    for (int32_t ty = 0; ty < store.rows(); ++ty) {
        for (int32_t tx = 0; tx < store.columns(); ++tx) {
            const canvas::RectI r = store.tileRect(tx, ty).intersect(store.canvasRect());
            const uint8_t* t = store.tile(tx, ty);
            for (int32_t y = r.y; y < r.bottom(); ++y) {
                uint32_t* o = out + static_cast<size_t>(y) * width;
                for (int32_t x = r.x; x < r.right(); ++x) {
                    if (!t) {
                        o[x] = 0xFF000000u | (backgroundArgb & 0xFFFFFFu);
                        continue;
                    }
                    constexpr int32_t kT = canvas::RasterStore::kTile;
                    const uint8_t* p = t + (static_cast<size_t>(y - ty * kT) * kT + (x - tx * kT)) * 4;
                    const float k = 1.0f - p[3] / 255.0f;  // premultiplied over
                    const uint32_t R = static_cast<uint32_t>(std::lround(p[0] + br * k));
                    const uint32_t G = static_cast<uint32_t>(std::lround(p[1] + bg * k));
                    const uint32_t B = static_cast<uint32_t>(std::lround(p[2] + bb * k));
                    o[x] = 0xFF000000u | (std::min(R, 255u) << 16) | (std::min(G, 255u) << 8) | std::min(B, 255u);
                }
            }
        }
    }
    return true;
}

}  // namespace pe::brush
