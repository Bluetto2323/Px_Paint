#include "brush/StrokeCompositor.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace pe::brush {

using canvas::RasterStore;
using canvas::RectI;

namespace {
constexpr int32_t kTile = RasterStore::kTile;
constexpr size_t kPlane = static_cast<size_t>(kTile) * kTile;
}  // namespace

DabShape DabShape::forDiameter(double diameter) {
    DabShape s;
    if (diameter >= 1.0) {
        s.radius = diameter * 0.5;
        s.scale = 1.0;
    } else {
        s.radius = 0.5;
        s.scale = std::max(diameter, 0.0);
    }
    return s;
}

RectI DabShape::bounds(double x, double y) const {
    const double e = radius + 0.5;
    const int32_t x0 = static_cast<int32_t>(std::floor(x - e));
    const int32_t y0 = static_cast<int32_t>(std::floor(y - e));
    const int32_t x1 = static_cast<int32_t>(std::ceil(x + e));
    const int32_t y1 = static_cast<int32_t>(std::ceil(y + e));
    return {x0, y0, x1 - x0, y1 - y0};
}

void StrokeCompositor::grow(RectI& r, const RectI& add) {
    if (add.empty()) return;
    if (r.empty()) {
        r = add;
        return;
    }
    const int32_t l = std::min(r.x, add.x), t = std::min(r.y, add.y);
    const int32_t rr = std::max(r.right(), add.right()), b = std::max(r.bottom(), add.bottom());
    r = {l, t, rr - l, b - t};
}

void StrokeCompositor::begin(RasterStore& store, const BrushSettings& settings) {
    if (active_) commit(store);
    settings_ = settings.sanitized();
    shape_ = DabShape::forDiameter(settings_.diameter);
    const float colourAlpha = static_cast<float>((settings_.argb >> 24) & 0xFF) / 255.0f;
    paintA_ = settings_.opacity * colourAlpha;
    paintR_ = static_cast<float>((settings_.argb >> 16) & 0xFF);
    paintG_ = static_cast<float>((settings_.argb >> 8) & 0xFF);
    paintB_ = static_cast<float>(settings_.argb & 0xFF);
    if (slot_.size() != store.tileCount()) slot_.assign(store.tileCount(), -1);
    active_ = true;
    stats_.strokeTiles = 0;
}

StrokeCompositor::StrokeTile* StrokeCompositor::tileFor(RasterStore& store, int32_t tx, int32_t ty, bool create) {
    const size_t idx = store.index(tx, ty);
    if (slot_[idx] >= 0) return tiles_[static_cast<size_t>(slot_[idx])].get();
    if (!create) return nullptr;
    const uint8_t* existing = store.tile(tx, ty);
    if (settings_.erase && !existing) return nullptr;  // nothing to erase there
    std::unique_ptr<StrokeTile> st;
    if (!pool_.empty()) {
        st = std::move(pool_.back());
        pool_.pop_back();
    } else {
        st = std::make_unique<StrokeTile>();
        st->base.reset(new uint8_t[RasterStore::kTileBytes]);
        st->mask.reset(new uint16_t[kPlane]);
        st->tail.reset(new uint16_t[kPlane]);
    }
    st->tx = tx;
    st->ty = ty;
    st->slotIndex = idx;
    st->storeHadTile = existing != nullptr;
    if (existing)
        std::memcpy(st->base.get(), existing, RasterStore::kTileBytes);
    else
        std::memset(st->base.get(), 0, RasterStore::kTileBytes);
    std::memset(st->mask.get(), 0, kPlane * sizeof(uint16_t));
    std::memset(st->tail.get(), 0, kPlane * sizeof(uint16_t));
    st->dirty = {};
    st->tailRect = {};
    if (!settings_.erase) store.tileForWrite(tx, ty);  // paint needs a destination tile
    slot_[idx] = static_cast<int32_t>(tiles_.size());
    tiles_.push_back(std::move(st));
    stats_.strokeTiles = static_cast<uint32_t>(tiles_.size());
    return tiles_.back().get();
}

void StrokeCompositor::rasterDab(RasterStore& store, const Dab& d, bool toTail) {
    if (!std::isfinite(d.x) || !std::isfinite(d.y)) return;
    const RectI box = shape_.bounds(d.x, d.y).intersect(store.canvasRect());
    int32_t tx0, ty0, tx1, ty1;
    if (!store.tileRange(box, tx0, ty0, tx1, ty1)) return;
    const double r = shape_.radius;
    const double inner = r - 0.5;
    const double inner2 = inner > 0 ? inner * inner : -1.0;
    const double outer2 = (r + 0.5) * (r + 0.5);
    const double scale = shape_.scale;
    const uint16_t full = static_cast<uint16_t>(std::lround(65535.0 * scale));
    for (int32_t ty = ty0; ty <= ty1; ++ty) {
        for (int32_t tx = tx0; tx <= tx1; ++tx) {
            const RectI part = box.intersect(store.tileRect(tx, ty));
            if (part.empty()) continue;
            StrokeTile* st = tileFor(store, tx, ty, true);
            if (!st) continue;
            uint16_t* plane = toTail ? st->tail.get() : st->mask.get();
            const int32_t ox = tx * kTile, oy = ty * kTile;
            for (int32_t y = part.y; y < part.bottom(); ++y) {
                const double dy = (y + 0.5) - d.y;
                const double dy2 = dy * dy;
                if (dy2 >= outer2) continue;
                uint16_t* row = plane + static_cast<size_t>(y - oy) * kTile - ox;
                // Pixel centres x+0.5 with (x+0.5-cx)^2 < outer2 - dy2: the row span.
                const double halfOuter = std::sqrt(outer2 - dy2);
                const int32_t xs = std::max(part.x, static_cast<int32_t>(std::floor(d.x - halfOuter - 0.5)));
                const int32_t xe = std::min(part.right(), static_cast<int32_t>(std::ceil(d.x + halfOuter - 0.5)) + 1);
                // Fully covered core of the row (d2 <= inner2): one fill.
                int32_t cs = xe, ce = xe;
                if (inner2 > dy2) {
                    const double halfInner = std::sqrt(inner2 - dy2);
                    cs = std::max(xs, static_cast<int32_t>(std::ceil(d.x - halfInner - 0.5)));
                    ce = std::min(xe, static_cast<int32_t>(std::floor(d.x + halfInner - 0.5)) + 1);
                    if (ce < cs) cs = ce = xe;
                }
                auto ring = [&](int32_t x0, int32_t x1) {
                    for (int32_t x = x0; x < x1; ++x) {
                        const double dx = (x + 0.5) - d.x;
                        const double d2 = dx * dx + dy2;
                        if (d2 >= outer2) continue;
                        const double cov = std::clamp(r + 0.5 - std::sqrt(d2), 0.0, 1.0) * scale;
                        const uint16_t v = d2 <= inner2 ? full : static_cast<uint16_t>(cov * 65535.0 + 0.5);
                        uint16_t& m = row[x];
                        if (v > m) m = v;
                    }
                };
                if (cs < ce) {
                    ring(xs, cs);
                    uint16_t* p = row + cs;
                    for (int32_t i = 0, n = ce - cs; i < n; ++i) p[i] = std::max(p[i], full);
                    ring(ce, xe);
                } else {
                    ring(xs, xe);
                }
            }
            grow(st->dirty, part);
            if (toTail) grow(st->tailRect, part);
        }
    }
    stats_.dabs++;
}

void StrokeCompositor::addDabs(RasterStore& store, const Dab* dabs, size_t count) {
    if (!active_) return;
    for (size_t i = 0; i < count; ++i) rasterDab(store, dabs[i], false);
}

void StrokeCompositor::setTail(RasterStore& store, const Dab* dabs, size_t count) {
    if (!active_) return;
    for (auto& st : tiles_) {
        if (st->tailRect.empty()) continue;
        const int32_t ox = st->tx * kTile, oy = st->ty * kTile;
        for (int32_t y = st->tailRect.y; y < st->tailRect.bottom(); ++y) {
            std::memset(st->tail.get() + static_cast<size_t>(y - oy) * kTile + (st->tailRect.x - ox), 0,
                        static_cast<size_t>(st->tailRect.w) * sizeof(uint16_t));
        }
        grow(st->dirty, st->tailRect);  // recomposite where the old tail was
        st->tailRect = {};
    }
    for (size_t i = 0; i < count; ++i) rasterDab(store, dabs[i], true);
}

void StrokeCompositor::composite(RasterStore& store, StrokeTile& st) {
    const RectI region = st.dirty.intersect(store.tileRect(st.tx, st.ty));
    st.dirty = {};
    if (region.empty()) return;
    uint8_t* dst = settings_.erase ? const_cast<uint8_t*>(store.tile(st.tx, st.ty)) : store.tileForWrite(st.tx, st.ty);
    if (!dst) return;
    const int32_t ox = st.tx * kTile, oy = st.ty * kTile;
    const float inv = 1.0f / 65535.0f;
    for (int32_t y = region.y; y < region.bottom(); ++y) {
        const size_t rowOff = static_cast<size_t>(y - oy) * kTile;
        const uint16_t* m0 = st.mask.get() + rowOff;
        const uint16_t* m1 = st.tail.get() + rowOff;
        const uint8_t* b = st.base.get() + rowOff * 4;
        uint8_t* o = dst + rowOff * 4;
        for (int32_t x = region.x - ox; x < region.right() - ox; ++x) {
            const uint16_t m = std::max(m0[x], m1[x]);
            const uint8_t* bp = b + static_cast<size_t>(x) * 4;
            uint8_t* op = o + static_cast<size_t>(x) * 4;
            if (m == 0) {
                std::memcpy(op, bp, 4);
                continue;
            }
            if (settings_.erase) {
                const float k = 1.0f - settings_.opacity * (static_cast<float>(m) * inv);
                op[0] = static_cast<uint8_t>(bp[0] * k + 0.5f);
                op[1] = static_cast<uint8_t>(bp[1] * k + 0.5f);
                op[2] = static_cast<uint8_t>(bp[2] * k + 0.5f);
                op[3] = static_cast<uint8_t>(bp[3] * k + 0.5f);
            } else {
                const float a = paintA_ * (static_cast<float>(m) * inv);
                const float k = 1.0f - a;
                op[0] = static_cast<uint8_t>(paintR_ * a + bp[0] * k + 0.5f);
                op[1] = static_cast<uint8_t>(paintG_ * a + bp[1] * k + 0.5f);
                op[2] = static_cast<uint8_t>(paintB_ * a + bp[2] * k + 0.5f);
                op[3] = static_cast<uint8_t>(255.0f * a + bp[3] * k + 0.5f);
            }
        }
    }
    stats_.compositedPixels += static_cast<uint64_t>(region.area());
    store.markChanged(region);
}

void StrokeCompositor::flush(RasterStore& store) {
    if (!active_) return;
    for (auto& st : tiles_)
        if (!st->dirty.empty()) composite(store, *st);
}

void StrokeCompositor::commit(RasterStore& store) {
    if (!active_) return;
    // The tail is part of the result only if finish() committed it; a
    // leftover provisional tail is removed.
    setTail(store, nullptr, 0);
    flush(store);
    releaseTiles();
    active_ = false;
}

void StrokeCompositor::cancel(RasterStore& store) {
    if (!active_) return;
    for (auto& st : tiles_) {
        if (st->storeHadTile) {
            uint8_t* px = store.tileForWrite(st->tx, st->ty);
            std::memcpy(px, st->base.get(), RasterStore::kTileBytes);
        } else if (uint8_t* px = const_cast<uint8_t*>(store.tile(st->tx, st->ty))) {
            std::memset(px, 0, RasterStore::kTileBytes);
        }
        store.markChanged(store.tileRect(st->tx, st->ty));
    }
    releaseTiles();
    active_ = false;
}

void StrokeCompositor::abandon() {
    tiles_.clear();
    slot_.clear();
    active_ = false;
    stats_.strokeTiles = 0;
}

void StrokeCompositor::releaseTiles() {
    for (auto& st : tiles_) {
        if (st->slotIndex < slot_.size()) slot_[st->slotIndex] = -1;
        if (pool_.size() < kMaxPooledTiles) pool_.push_back(std::move(st));
    }
    tiles_.clear();
    stats_.strokeTiles = 0;
    stats_.pooledTiles = static_cast<uint32_t>(pool_.size());
}

}  // namespace pe::brush
