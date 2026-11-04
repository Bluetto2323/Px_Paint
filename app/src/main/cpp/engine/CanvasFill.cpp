// PxPaint - CanvasDocument: bucket fill and gradients (Block 13).
//
// Both work tile by tile on the active layer, inside the selection when one
// is active, and record one history step (tile delta patches; no full-canvas
// copies). The bucket fill reads either the active layer or the visible image
// (composited lazily, only the tiles the region reaches); the gradient is
// previewed non-destructively through the compositor's preview tiles, like a
// transform, and written only when confirmed.
#include <algorithm>
#include <cmath>
#include <cstring>

#include "engine/CanvasDocument.h"

namespace pe {

using canvas::RasterStore;
using canvas::RectI;
using canvas::SelectionMask;

namespace {
constexpr int32_t kT = RasterStore::kTile;
constexpr size_t kPx = static_cast<size_t>(kT) * kT;

// Premultiplied RGBA of straight ARGB.
void premul(uint32_t argb, float out[4]) {
    const float a = ((argb >> 24) & 0xFF) / 255.0f;
    out[0] = ((argb >> 16) & 0xFF) * a;
    out[1] = ((argb >> 8) & 0xFF) * a;
    out[2] = (argb & 0xFF) * a;
    out[3] = a * 255.0f;
}

// d = s * k + d * (1 - sa * k), premultiplied, k = coverage 0..1.
inline void overCov(uint8_t* d, const float s[4], float k) {
    if (k <= 0.0f) return;
    const float ia = 1.0f - (s[3] / 255.0f) * k;
    for (int c = 0; c < 4; ++c) {
        const float v = s[c] * k + d[c] * ia;
        d[c] = static_cast<uint8_t>(std::clamp(v + 0.5f, 0.0f, 255.0f));
    }
    for (int c = 0; c < 3; ++c) d[c] = std::min(d[c], d[3]);
}

bool tileEmpty(const uint8_t* px) {
    for (size_t i = 3; i < kPx * 4; i += 4)
        if (px[i]) return false;
    return true;
}

// Lazily allocated per-tile 8-bit masks over the canvas.
struct TileMask {
    int32_t cols = 0, rows = 0;
    std::vector<std::unique_ptr<uint8_t[]>> t;
    void reset(int32_t c, int32_t r) { cols = c; rows = r; t.clear(); t.resize(static_cast<size_t>(c) * r); }
    uint8_t get(int32_t x, int32_t y) const {
        const auto& p = t[static_cast<size_t>(y / kT) * cols + x / kT];
        return p ? p[static_cast<size_t>(y % kT) * kT + x % kT] : 0;
    }
    uint8_t* row(int32_t x, int32_t y) {
        auto& p = t[static_cast<size_t>(y / kT) * cols + x / kT];
        if (!p) { p.reset(new uint8_t[kPx]); std::memset(p.get(), 0, kPx); }
        return p.get() + static_cast<size_t>(y % kT) * kT;
    }
    void set(int32_t x, int32_t y, uint8_t v) { row(x, y)[x % kT] = v; }
};
}  // namespace

LayerError CanvasDocument::bucketFill(int32_t x, int32_t y, uint32_t argb, const FillOptions& o) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!open_) return LayerError::NoCanvas;
    settleInputLocked();
    Layer* a = activeLayer();
    if (!a) return LayerError::NotFound;
    if (a->info.locked) return LayerError::Locked;
    if (!a->info.visible) return LayerError::Hidden;
    const int32_t W = static_cast<int32_t>(composite_.width()), H = static_cast<int32_t>(composite_.height());
    if (x < 0 || y < 0 || x >= W || y >= H) return LayerError::BadValue;
    if (selection_.active() && selection_.at(x, y) == 0) return LayerError::BadValue;   // outside the selection: nothing
    const int32_t cols = composite_.columns(), rows = composite_.rows();

    // Reference pixels: the active layer, or the visible image (no paper) composited tile by tile on demand.
    RasterStore merged;
    std::vector<uint8_t> mergedDone;
    std::vector<canvas::LayerView> views;
    if (o.merged) {
        merged.reset(composite_.width(), composite_.height());
        mergedDone.assign(static_cast<size_t>(cols) * rows, 0);
        for (const auto& l : layers_)
            if (l->info.visible && l->info.opacity > 0.0f) views.push_back({&l->store, l->info.opacity, l->info.blend});
    }
    static const uint8_t kZero[4] = {0, 0, 0, 0};
    auto ref = [&](int32_t px, int32_t py) -> const uint8_t* {
        const int32_t tx = px / kT, ty = py / kT;
        const uint8_t* t;
        if (o.merged) {
            uint8_t& done = mergedDone[static_cast<size_t>(ty) * cols + tx];
            if (!done) {
                canvas::Compositor::composite(views.data(), views.size(), merged.tileRect(tx, ty), canvas::Backdrop{}, merged);
                done = 1;
            }
            t = merged.tile(tx, ty);
        } else {
            t = a->store.tile(tx, ty);
        }
        return t ? t + (static_cast<size_t>(py % kT) * kT + px % kT) * 4 : kZero;
    };
    uint8_t seed[4];
    std::memcpy(seed, ref(x, y), 4);
    const int32_t tol = std::clamp(o.tolerance, 0, 255);
    auto matches = [&](int32_t px, int32_t py) {
        if (selection_.active() && selection_.at(px, py) == 0) return false;
        const uint8_t* p = ref(px, py);
        for (int c = 0; c < 4; ++c)
            if (std::abs(int(p[c]) - int(seed[c])) > tol) return false;
        return true;
    };

    // Region (255 = filled), lazily allocated per tile; bounds tracked.
    TileMask region;
    region.reset(cols, rows);
    int32_t bx0 = W, by0 = H, bx1 = -1, by1 = -1;
    auto mark = [&](int32_t px0, int32_t px1, int32_t py) {   // [px0, px1] inclusive
        uint8_t* r = nullptr;
        for (int32_t px = px0; px <= px1; ++px) {
            if (!r || px % kT == 0) r = region.row(px, py);   // the tile's row (index by px % kT)
            r[px % kT] = 255;
        }
        bx0 = std::min(bx0, px0); bx1 = std::max(bx1, px1); by0 = std::min(by0, py); by1 = std::max(by1, py);
    };
    if (o.contiguous) {
        // Scanline flood fill with a span stack (4-connected).
        std::vector<std::pair<int32_t, int32_t>> stack;
        stack.push_back({x, y});
        while (!stack.empty()) {
            auto [sx, sy] = stack.back();
            stack.pop_back();
            if (region.get(sx, sy) || !matches(sx, sy)) continue;
            int32_t l = sx, r = sx;
            while (l > 0 && !region.get(l - 1, sy) && matches(l - 1, sy)) --l;
            while (r < W - 1 && !region.get(r + 1, sy) && matches(r + 1, sy)) ++r;
            mark(l, r, sy);
            for (int32_t ny : {sy - 1, sy + 1}) {
                if (ny < 0 || ny >= H) continue;
                bool inSpan = false;
                for (int32_t px = l; px <= r; ++px) {
                    const bool m = !region.get(px, ny) && matches(px, ny);
                    if (m && !inSpan) stack.push_back({px, ny});
                    inSpan = m;
                }
            }
        }
    } else {
        // Global: every matching pixel (of the selection's bounds when there is one).
        RectI area{0, 0, W, H};
        if (selection_.active()) selection_.bounds(area);
        for (int32_t py = area.y; py < area.bottom(); ++py) {
            int32_t runStart = -1;
            for (int32_t px = area.x; px <= area.right(); ++px) {
                const bool m = px < area.right() && matches(px, py);
                if (m && runStart < 0) runStart = px;
                if (!m && runStart >= 0) { mark(runStart, px - 1, py); runStart = -1; }
            }
        }
    }
    if (bx1 < 0) return LayerError::BadValue;

    // Expansion: grow the region by `expand` pixels (square dilation), within the canvas.
    const int32_t grow = std::clamp(o.expand, 0, 8);
    const int32_t aa = o.antialias ? 1 : 0;
    const int32_t ex0 = std::max(0, bx0 - grow - aa), ey0 = std::max(0, by0 - grow - aa);
    const int32_t ex1 = std::min(W - 1, bx1 + grow + aa), ey1 = std::min(H - 1, by1 + grow + aa);
    const int32_t bw = ex1 - ex0 + 1, bh = ey1 - ey0 + 1;
    // Work in a bounding-box buffer (only the region's bounds, never the whole canvas unless it is filled).
    std::vector<uint8_t> cov(static_cast<size_t>(bw) * bh, 0), tmp;
    for (int32_t py = by0; py <= by1; ++py)
        for (int32_t px = bx0; px <= bx1; ++px) cov[static_cast<size_t>(py - ey0) * bw + (px - ex0)] = region.get(px, py);
    region = {};
    if (grow > 0) {
        // Separable max filter: rows, then columns.
        tmp.assign(cov.size(), 0);
        for (int32_t yy = 0; yy < bh; ++yy)
            for (int32_t xx = 0; xx < bw; ++xx) {
                uint8_t m = 0;
                for (int32_t k = std::max(0, xx - grow); k <= std::min(bw - 1, xx + grow) && !m; ++k) m = cov[static_cast<size_t>(yy) * bw + k];
                tmp[static_cast<size_t>(yy) * bw + xx] = m;
            }
        for (int32_t yy = 0; yy < bh; ++yy)
            for (int32_t xx = 0; xx < bw; ++xx) {
                uint8_t m = 0;
                for (int32_t k = std::max(0, yy - grow); k <= std::min(bh - 1, yy + grow) && !m; ++k) m = tmp[static_cast<size_t>(k) * bw + xx];
                cov[static_cast<size_t>(yy) * bw + xx] = m;
            }
    }
    if (aa) {
        // Soft outer edge: a pixel next to the region gets the average of its 3x3 neighbourhood.
        tmp = cov;
        for (int32_t yy = 0; yy < bh; ++yy)
            for (int32_t xx = 0; xx < bw; ++xx) {
                if (tmp[static_cast<size_t>(yy) * bw + xx]) continue;
                int32_t sum = 0;
                for (int32_t j = -1; j <= 1; ++j)
                    for (int32_t i = -1; i <= 1; ++i) {
                        const int32_t u = xx + i, v = yy + j;
                        if (u >= 0 && v >= 0 && u < bw && v < bh) sum += tmp[static_cast<size_t>(v) * bw + u];
                    }
                cov[static_cast<size_t>(yy) * bw + xx] = static_cast<uint8_t>(sum / 9);
            }
    }

    // Paint, tile by tile, with the selection's coverage; one patch.
    float col[4];
    premul(argb, col);
    PixelPatch patch;
    std::vector<uint8_t> before(kPx * 4);
    for (int32_t ty = ey0 / kT; ty <= ey1 / kT; ++ty)
        for (int32_t tx = ex0 / kT; tx <= ex1 / kT; ++tx) {
            const RectI tr = a->store.tileRect(tx, ty);
            const int32_t x0 = std::max(tr.x, ex0), x1 = std::min(tr.right() - 1, ex1);
            const int32_t y0 = std::max(tr.y, ey0), y1 = std::min(tr.bottom() - 1, ey1);
            bool any = false;
            for (int32_t py = y0; py <= y1 && !any; ++py)
                for (int32_t px = x0; px <= x1; ++px)
                    if (cov[static_cast<size_t>(py - ey0) * bw + (px - ex0)]) { any = true; break; }
            if (!any) continue;
            const uint8_t* old = a->store.tile(tx, ty);
            if (old) std::memcpy(before.data(), old, before.size());
            uint8_t* d = a->store.tileForWrite(tx, ty);
            for (int32_t py = y0; py <= y1; ++py)
                for (int32_t px = x0; px <= x1; ++px) {
                    uint32_t k = cov[static_cast<size_t>(py - ey0) * bw + (px - ex0)];
                    if (!k) continue;
                    if (selection_.active()) k = k * selection_.at(px, py) / 255;
                    overCov(d + (static_cast<size_t>(py - ty * kT) * kT + (px - tx * kT)) * 4, col, k / 255.0f);
                }
            const uint8_t* after = d;
            if (tileEmpty(d)) { a->store.releaseTile(tx, ty); after = nullptr; }
            patch.addTile(tx, ty, tr, old ? before.data() : nullptr, after);
            a->store.markChanged(tr);
        }
    if (patch.empty()) return LayerError::BadValue;
    auto cmd = std::make_unique<PixelCommand>(HistoryKind::Fill, a->info.id, std::move(patch));
    cmd->setLabel(a->info.name);
    history_.push(std::move(cmd));
    bumpRevision();
    return LayerError::None;
}

// ------------------------------------------------------------------ gradient
LayerError CanvasDocument::gradientSet(const GradientSpec& g) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!open_) return LayerError::NoCanvas;
    if (!std::isfinite(g.x0) || !std::isfinite(g.y0) || !std::isfinite(g.x1) || !std::isfinite(g.y1)) return LayerError::BadValue;
    if (!grad_.active) {
        settleInputLocked();
        Layer* a = activeLayer();
        if (!a) return LayerError::NotFound;
        if (a->info.locked) return LayerError::Locked;
        if (!a->info.visible) return LayerError::Hidden;
        grad_ = {};
        grad_.active = true;
        grad_.layerId = a->info.id;
        grad_.preview.reset(composite_.width(), composite_.height());
        grad_.previewTiles.assign(composite_.tileCount(), 0);
    }
    Layer* a = findLayer(grad_.layerId);
    if (!a) { cancelGradientLocked(); return LayerError::NotFound; }
    grad_.spec = g;
    const RasterStore& ls = a->store;
    // Tiles: the whole layer, or the selection's bounds.
    RectI area = ls.canvasRect();
    if (selection_.active() && !selection_.bounds(area)) return LayerError::BadValue;
    float c0[4], c1[4];
    premul(g.argb0, c0);
    premul(g.argb1, c1);
    const double dx = g.x1 - g.x0, dy = g.y1 - g.y0, len2 = dx * dx + dy * dy;
    const double radius = std::sqrt(len2);
    // Colour ramp: 1024 premultiplied steps (interpolated in premultiplied space, so a
    // transparency gradient keeps its colour); pixels index it by their position t.
    constexpr int kSteps = 1024;
    std::vector<uint8_t> ramp(kSteps * 4);
    for (int i = 0; i < kSteps; ++i) {
        const float f = static_cast<float>(i) / (kSteps - 1);
        for (int c = 0; c < 4; ++c) ramp[i * 4 + c] = static_cast<uint8_t>(std::clamp(c0[c] + (c1[c] - c0[c]) * f + 0.5f, 0.0f, 255.0f));
        for (int c = 0; c < 3; ++c) ramp[i * 4 + c] = std::min(ramp[i * 4 + c], ramp[i * 4 + 3]);
    }
    std::vector<uint8_t> px(kPx * 4);
    std::vector<int32_t> now;
    for (int32_t ty = area.y / kT; ty <= (area.bottom() - 1) / kT; ++ty)
        for (int32_t tx = area.x / kT; tx <= (area.right() - 1) / kT; ++tx) {
            const auto st = selection_.active() ? selection_.state(tx, ty) : SelectionMask::TileState::Full;
            if (st == SelectionMask::TileState::Empty) continue;
            const uint8_t* mask = selection_.active() ? selection_.tile(tx, ty) : nullptr;
            const RectI tr = ls.tileRect(tx, ty);
            const uint8_t* orig = ls.tile(tx, ty);
            if (orig) std::memcpy(px.data(), orig, px.size());
            else std::fill(px.begin(), px.end(), 0);
            for (int32_t y = tr.y; y < tr.bottom(); ++y) {
                // Linear: t grows by a constant step along a row.
                const double rowT = len2 < 1e-9 ? 1.0 : ((tr.x + 0.5 - g.x0) * dx + (y + 0.5 - g.y0) * dy) / len2;
                const double stepT = len2 < 1e-9 ? 0.0 : dx / len2;
                const double ry2 = (y + 0.5 - g.y0) * (y + 0.5 - g.y0);
                for (int32_t x = tr.x; x < tr.right(); ++x) {
                    const size_t i = static_cast<size_t>(y - ty * kT) * kT + (x - tx * kT);
                    const uint32_t k = mask ? mask[i] : 255u;
                    if (!k) continue;
                    double t;
                    if (len2 < 1e-9) t = 1.0;
                    else if (g.radial) { const double ddx = x + 0.5 - g.x0; t = std::sqrt(ddx * ddx + ry2) / radius; }
                    else t = rowT + stepT * (x - tr.x);
                    const int idx = static_cast<int>(std::clamp(t, 0.0, 1.0) * (kSteps - 1) + 0.5);
                    const uint8_t* sc = &ramp[static_cast<size_t>(idx) * 4];
                    uint8_t* d = px.data() + i * 4;
                    // src-over with coverage k, integer premultiplied.
                    const uint32_t sa = (sc[3] * k + 127u) / 255u;
                    if (sa == 255) { std::memcpy(d, sc, 4); continue; }
                    for (int c = 0; c < 4; ++c)
                        d[c] = static_cast<uint8_t>(std::min<uint32_t>(255, (sc[c] * k + 127u) / 255u + (d[c] * (255u - sa) + 127u) / 255u));
                }
            }
            const int32_t idx = ty * ls.columns() + tx;
            if (tileEmpty(px.data())) grad_.preview.releaseTile(tx, ty);
            else std::memcpy(grad_.preview.tileForWrite(tx, ty), px.data(), px.size());
            grad_.previewTiles[static_cast<size_t>(idx)] = 1;
            grad_.dirty.push_back(tr);
            now.push_back(idx);
        }
    // (The tile set only changes with the selection, which cannot change during a preview.)
    grad_.shown = std::move(now);
    bumpRevision();
    return LayerError::None;
}

void CanvasDocument::commitGradientLocked() {
    if (!grad_.active) return;
    Layer* a = findLayer(grad_.layerId);
    if (!a) { cancelGradientLocked(); return; }
    RasterStore& ls = a->store;
    PixelPatch patch;
    for (int32_t i : grad_.shown) {
        const int32_t tx = i % ls.columns(), ty = i / ls.columns();
        const uint8_t* before = ls.tile(tx, ty);
        const uint8_t* after = grad_.preview.tile(tx, ty);
        if (!before && !after) continue;
        if (before && after && std::memcmp(before, after, kPx * 4) == 0) continue;
        patch.addTile(tx, ty, ls.tileRect(tx, ty), before, after);
        if (after) std::memcpy(ls.tileForWrite(tx, ty), after, kPx * 4);
        else ls.releaseTile(tx, ty);
        ls.markChanged(ls.tileRect(tx, ty));
    }
    if (!patch.empty()) {
        auto cmd = std::make_unique<PixelCommand>(HistoryKind::Gradient, a->info.id, std::move(patch));
        cmd->setLabel(a->info.name);
        history_.push(std::move(cmd));
    }
    grad_ = {};
    bumpRevision();
}

void CanvasDocument::cancelGradientLocked() {
    if (!grad_.active) return;
    if (pending_.size() != composite_.tileCount()) pending_.assign(composite_.tileCount(), 0);
    for (int32_t i : grad_.shown) {
        uint8_t& p = pending_[static_cast<size_t>(i)];
        if (!p) { p = 1; pendingCount_++; }
    }
    pendingComposite_.store(pendingCount_ > 0, std::memory_order_release);
    grad_ = {};
    bumpRevision();
}

LayerError CanvasDocument::gradientCommit() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!grad_.active) return LayerError::BadValue;
    commitGradientLocked();
    return LayerError::None;
}

void CanvasDocument::gradientCancel() {
    std::lock_guard<std::mutex> lock(mutex_);
    cancelGradientLocked();
}

bool CanvasDocument::gradientActive() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return grad_.active;
}

}  // namespace pe
