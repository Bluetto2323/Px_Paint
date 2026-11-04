// PxPaint - CanvasDocument: selection operations (Block 10).
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

// px *= m / 255 (premultiplied: every channel), m per pixel or a constant.
void scaleTile(uint8_t* px, const uint8_t* m, uint8_t constant) {
    for (size_t i = 0; i < kPx; ++i) {
        const uint32_t k = m ? m[i] : constant;
        if (k == 255) continue;
        uint8_t* p = px + i * 4;
        for (int c = 0; c < 4; ++c) p[c] = static_cast<uint8_t>((p[c] * k + 127u) / 255u);
    }
}

bool tileEmpty(const uint8_t* px) {
    for (size_t i = 3; i < kPx * 4; i += 4)
        if (px[i]) return false;
    return true;
}
}  // namespace

bool CanvasDocument::restoreSelection(const std::vector<uint8_t>& encoded) {
    return selection_.decode(encoded.data(), encoded.size());
}

void CanvasDocument::recordSelection(const std::vector<uint8_t>& before) {
    std::vector<uint8_t> after;
    selection_.encode(after);
    if (after == before) return;
    history_.push(std::make_unique<SelectionCommand>(before, std::move(after)));
}

// ------------------------------------------------------------------ selecting
LayerError CanvasDocument::select(const float* xy, size_t points, SelectionMask::Mode mode, bool antialias) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!open_) return LayerError::NoCanvas;
    if (!xy || points < 3) return LayerError::BadValue;
    settleInputLocked();
    std::vector<uint8_t> before;
    selection_.encode(before);
    shape_.reset(selection_.width(), selection_.height());
    shape_.setPolygon(xy, points, antialias);
    selection_.combine(shape_, mode);
    recordSelection(before);
    return LayerError::None;
}

LayerError CanvasDocument::selectAll() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!open_) return LayerError::NoCanvas;
    settleInputLocked();
    std::vector<uint8_t> before;
    selection_.encode(before);
    selection_.selectAll();
    recordSelection(before);
    return LayerError::None;
}

LayerError CanvasDocument::deselect() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!open_) return LayerError::NoCanvas;
    settleInputLocked();
    std::vector<uint8_t> before;
    selection_.encode(before);
    selection_.clear();
    recordSelection(before);
    return LayerError::None;
}

LayerError CanvasDocument::invertSelection() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!open_) return LayerError::NoCanvas;
    if (!selection_.active()) return LayerError::BadValue;
    settleInputLocked();
    std::vector<uint8_t> before;
    selection_.encode(before);
    selection_.invert();
    recordSelection(before);
    return LayerError::None;
}

LayerError CanvasDocument::selectFromContent(SelectionMask::Mode mode) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!open_) return LayerError::NoCanvas;
    settleInputLocked();
    std::vector<uint8_t> before;
    selection_.encode(before);
    shape_.reset(selection_.width(), selection_.height());
    std::vector<uint8_t> cov(kPx);
    std::vector<const Layer*> vis;
    for (const auto& l : layers_)
        if (l->info.visible && l->info.opacity > 0.0f) vis.push_back(l.get());
    for (int32_t ty = 0; ty < shape_.rows(); ++ty)
        for (int32_t tx = 0; tx < shape_.columns(); ++tx) {
            // Union of the visible layers' alpha: 1 - prod(1 - a * opacity).
            std::vector<std::pair<const uint8_t*, float>> src;
            for (const Layer* l : vis)
                if (const uint8_t* px = l->store.tile(tx, ty)) src.push_back({px, l->info.opacity});
            if (src.empty()) continue;
            bool any = false;
            for (size_t i = 0; i < kPx; ++i) {
                float keep = 1.0f;
                for (const auto& [px, op] : src) keep *= 1.0f - (px[i * 4 + 3] / 255.0f) * op;
                const uint8_t v = static_cast<uint8_t>(std::lround((1.0f - keep) * 255.0f));
                cov[i] = v;
                any |= v != 0;
            }
            if (any) shape_.setTile(tx, ty, cov.data());
        }
    selection_.combine(shape_, mode);
    recordSelection(before);
    return LayerError::None;
}

// ------------------------------------------------------------------ pixel operations
template <typename Fn>
LayerError CanvasDocument::editSelectedLocked(HistoryKind kind, Fn&& op) {
    if (!selection_.active()) return LayerError::BadValue;
    Layer* a = activeLayer();
    if (!a) return LayerError::NotFound;
    if (a->info.locked) return LayerError::Locked;
    if (!a->info.visible) return LayerError::Hidden;
    RectI b;
    if (!selection_.bounds(b)) return LayerError::BadValue;
    PixelPatch patch;
    std::vector<uint8_t> before(kPx * 4);
    const int32_t tx0 = b.x / kT, ty0 = b.y / kT, tx1 = (b.right() - 1) / kT, ty1 = (b.bottom() - 1) / kT;
    for (int32_t ty = ty0; ty <= ty1; ++ty)
        for (int32_t tx = tx0; tx <= tx1; ++tx) {
            const auto st = selection_.state(tx, ty);
            if (st == SelectionMask::TileState::Empty) continue;
            uint8_t* px = const_cast<uint8_t*>(a->store.tile(tx, ty));
            if (!px) continue;
            std::memcpy(before.data(), px, before.size());
            op(px, selection_.tile(tx, ty));
            const RectI r = a->store.tileRect(tx, ty);
            if (tileEmpty(px)) {
                patch.addTile(tx, ty, r, before.data(), nullptr);
                a->store.releaseTile(tx, ty);
            } else {
                patch.addTile(tx, ty, r, before.data(), px);
            }
            a->store.markChanged(r);
        }
    if (!patch.empty()) {
        auto cmd = std::make_unique<PixelCommand>(kind, a->info.id, std::move(patch));
        cmd->setLabel(a->info.name);
        history_.push(std::move(cmd));
        bumpRevision();
    }
    return LayerError::None;
}

LayerError CanvasDocument::clearSelected() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!open_) return LayerError::NoCanvas;
    settleInputLocked();
    return editSelectedLocked(HistoryKind::ClearSelected, [](uint8_t* px, const uint8_t* m) {
        // Keep (1 - selection): fully selected tiles become empty, edges fade.
        if (!m) { std::memset(px, 0, kPx * 4); return; }
        std::vector<uint8_t> inv(kPx);
        for (size_t i = 0; i < kPx; ++i) inv[i] = static_cast<uint8_t>(255 - m[i]);
        scaleTile(px, inv.data(), 255);
    });
}

bool CanvasDocument::copySelectedLocked(bool merged, RasterStore& out) {
    RectI b;
    if (!selection_.bounds(b)) return false;
    Layer* a = activeLayer();
    if (!merged && !a) return false;
    out.reset(selection_.width(), selection_.height());
    // Merged: the visible layers composited over transparency (no paper).
    RasterStore scratch;
    std::vector<canvas::LayerView> views;
    if (merged) {
        scratch.reset(selection_.width(), selection_.height());
        for (const auto& l : layers_)
            if (l->info.visible && l->info.opacity > 0.0f) views.push_back({&l->store, l->info.opacity, l->info.blend});
    }
    bool any = false;
    const int32_t tx0 = b.x / kT, ty0 = b.y / kT, tx1 = (b.right() - 1) / kT, ty1 = (b.bottom() - 1) / kT;
    for (int32_t ty = ty0; ty <= ty1; ++ty)
        for (int32_t tx = tx0; tx <= tx1; ++tx) {
            if (selection_.state(tx, ty) == SelectionMask::TileState::Empty) continue;
            const uint8_t* src = nullptr;
            if (merged) {
                canvas::Compositor::composite(views.data(), views.size(), scratch.tileRect(tx, ty), canvas::Backdrop{}, scratch);
                src = scratch.tile(tx, ty);
            } else {
                src = a->store.tile(tx, ty);
            }
            if (!src) continue;
            uint8_t* dst = out.tileForWrite(tx, ty);
            std::memcpy(dst, src, kPx * 4);
            scaleTile(dst, selection_.tile(tx, ty), 255);
            if (tileEmpty(dst)) out.releaseTile(tx, ty);
            else any = true;
        }
    return any;
}

LayerError CanvasDocument::copySelection(bool merged) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!open_) return LayerError::NoCanvas;
    if (!selection_.active()) return LayerError::BadValue;
    settleInputLocked();
    if (merged) updateLocked(true);
    RasterStore c;
    if (!copySelectedLocked(merged, c)) return LayerError::BadValue;   // nothing there to copy
    clipboard_ = std::move(c);
    hasClipboard_ = true;
    return LayerError::None;
}

LayerError CanvasDocument::cutSelection() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!open_) return LayerError::NoCanvas;
    if (!selection_.active()) return LayerError::BadValue;
    settleInputLocked();
    Layer* a = activeLayer();
    if (!a) return LayerError::NotFound;
    if (a->info.locked) return LayerError::Locked;
    if (!a->info.visible) return LayerError::Hidden;
    RasterStore c;
    if (!copySelectedLocked(false, c)) return LayerError::BadValue;
    clipboard_ = std::move(c);
    hasClipboard_ = true;
    return editSelectedLocked(HistoryKind::Cut, [](uint8_t* px, const uint8_t* m) {
        if (!m) { std::memset(px, 0, kPx * 4); return; }
        std::vector<uint8_t> inv(kPx);
        for (size_t i = 0; i < kPx; ++i) inv[i] = static_cast<uint8_t>(255 - m[i]);
        scaleTile(px, inv.data(), 255);
    });
}

LayerError CanvasDocument::insertStoreAsLayerLocked(const RasterStore& src, const std::string& name, HistoryKind kind, uint32_t& newId) {
    if (layers_.size() >= kMaxLayers) return LayerError::TooMany;
    if (layerBytes() + src.bytesAllocated() > budget_) return LayerError::Memory;
    const int32_t i = indexOf(activeId_);
    auto l = std::make_unique<Layer>();
    l->info.id = nextId_++;
    l->info.name = uniqueName(name);
    l->store.reset(composite_.width(), composite_.height());
    // Tile by tile (a clipboard from another canvas size keeps what fits).
    for (int32_t ty = 0; ty < std::min(src.rows(), l->store.rows()); ++ty)
        for (int32_t tx = 0; tx < std::min(src.columns(), l->store.columns()); ++tx)
            if (const uint8_t* px = src.tile(tx, ty)) {
                std::memcpy(l->store.tileForWrite(tx, ty), px, kPx * 4);
                l->store.markChanged(l->store.tileRect(tx, ty));
            }
    l->store.markEverythingUnsaved();
    dirtyScratch_.clear();
    l->store.takeGpuDirty(dirtyScratch_);
    markTilesOf(l->store);
    newId = l->info.id;
    const std::string label = l->info.name;
    const int32_t at = i < 0 ? static_cast<int32_t>(layers_.size()) : i + 1;
    layers_.insert(layers_.begin() + at, std::move(l));
    auto cmd = std::make_unique<InsertLayerCommand>(kind, newId, at, activeId_);
    cmd->setLabel(label);
    history_.push(std::move(cmd));
    activeId_ = newId;
    refreshEditable();
    bumpRevision();
    return LayerError::None;
}

LayerError CanvasDocument::paste(uint32_t& newId) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!open_) return LayerError::NoCanvas;
    if (!hasClipboard_) return LayerError::BadValue;
    settleInputLocked();
    return insertStoreAsLayerLocked(clipboard_, "Pasted", HistoryKind::Paste, newId);
}

LayerError CanvasDocument::duplicateSelection(uint32_t& newId) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!open_) return LayerError::NoCanvas;
    if (!selection_.active()) return LayerError::BadValue;
    settleInputLocked();
    Layer* a = activeLayer();
    if (!a) return LayerError::NotFound;
    RasterStore c;
    if (!copySelectedLocked(false, c)) return LayerError::BadValue;
    return insertStoreAsLayerLocked(c, a->info.name + " selection", HistoryKind::DuplicateSelection, newId);
}

// ------------------------------------------------------------------ moving
LayerError CanvasDocument::moveBegin() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!open_) return LayerError::NoCanvas;
    if (move_.active) return LayerError::None;
    if (!selection_.active()) return LayerError::BadValue;
    settleInputLocked();
    Layer* a = activeLayer();
    if (!a) return LayerError::NotFound;
    if (a->info.locked) return LayerError::Locked;
    if (!a->info.visible) return LayerError::Hidden;
    RectI b;
    if (!selection_.bounds(b)) return LayerError::BadValue;
    move_ = {};
    move_.layerId = a->info.id;
    move_.bounds = b;
    selection_.encode(move_.selectionBefore);
    move_.floating.reset(composite_.width(), composite_.height());
    move_.preview.reset(composite_.width(), composite_.height());
    move_.previewTiles.assign(composite_.tileCount(), 0);
    const int32_t tx0 = b.x / kT, ty0 = b.y / kT, tx1 = (b.right() - 1) / kT, ty1 = (b.bottom() - 1) / kT;
    for (int32_t ty = ty0; ty <= ty1; ++ty)
        for (int32_t tx = tx0; tx <= tx1; ++tx) {
            if (selection_.state(tx, ty) == SelectionMask::TileState::Empty) continue;
            const uint8_t* px = a->store.tile(tx, ty);
            if (!px) continue;
            uint8_t* f = move_.floating.tileForWrite(tx, ty);
            std::memcpy(f, px, kPx * 4);
            scaleTile(f, selection_.tile(tx, ty), 255);
            if (tileEmpty(f)) move_.floating.releaseTile(tx, ty);
        }
    // Nothing changes on screen yet: untransformed, the preview is the layer itself.
    move_.active = true;
    return LayerError::None;
}

namespace {
// Premultiplied src-over of `s` onto `d`.
inline void over(uint8_t* d, const uint8_t* s) {
    const uint32_t sa = s[3];
    if (sa == 0) return;
    if (sa == 255) { std::memcpy(d, s, 4); return; }
    for (int c = 0; c < 4; ++c) d[c] = static_cast<uint8_t>(std::min<uint32_t>(255, s[c] + (d[c] * (255 - sa) + 127u) / 255u));
}
// Integer translation: (dx, dy) when `m` is one, exactly.
bool integerShift(const canvas::Affine2& m, int32_t& dx, int32_t& dy) {
    if (m.a != 1.0 || m.b != 0.0 || m.c != 0.0 || m.d != 1.0) return false;
    if (m.tx != std::floor(m.tx) || m.ty != std::floor(m.ty)) return false;
    dx = static_cast<int32_t>(m.tx);
    dy = static_cast<int32_t>(m.ty);
    return true;
}
}  // namespace

// Preview tile `index`: the layer's tile with the hole, the transformed floating pixels over it.
void CanvasDocument::composeMoveTileLocked(Layer& l, int32_t index, std::vector<uint8_t>& px) {
    const RasterStore& ls = l.store;
    const int32_t tx = index % ls.columns(), ty = index / ls.columns();
    px.resize(kPx * 4);
    if (const uint8_t* orig = ls.tile(tx, ty)) std::memcpy(px.data(), orig, kPx * 4);
    else std::fill(px.begin(), px.end(), 0);
    // Hole.
    const auto st = selection_.state(tx, ty);
    if (st == SelectionMask::TileState::Full) std::fill(px.begin(), px.end(), 0);
    else if (st == SelectionMask::TileState::Partial) {
        const uint8_t* m = selection_.tile(tx, ty);
        for (size_t i = 0; i < kPx; ++i) {
            const uint32_t k = 255u - m[i];
            if (k == 255) continue;
            uint8_t* p = px.data() + i * 4;
            for (int c = 0; c < 4; ++c) p[c] = static_cast<uint8_t>((p[c] * k + 127u) / 255u);
        }
    }
    const RasterStore& f = move_.floating;
    const RectI r = ls.tileRect(tx, ty);   // canvas part of this tile
    const int32_t FW = static_cast<int32_t>(f.width()), FH = static_cast<int32_t>(f.height());
    int32_t dx = 0, dy = 0;
    if (integerShift(move_.xf, dx, dy)) {
        // Exact copy of the shifted pixels, row runs within one source tile.
        for (int32_t y = r.y; y < r.bottom(); ++y) {
            const int32_t sy = y - dy;
            if (sy < 0 || sy >= FH) continue;
            int32_t x = r.x;
            while (x < r.right()) {
                const int32_t sx = x - dx;
                if (sx < 0) { x += -sx; continue; }
                if (sx >= FW) break;
                const int32_t run = std::min(r.right() - x, kT - sx % kT);
                if (const uint8_t* sp = f.tile(sx / kT, sy / kT)) {
                    const uint8_t* s = sp + (static_cast<size_t>(sy % kT) * kT + static_cast<size_t>(sx % kT)) * 4;
                    uint8_t* d = px.data() + (static_cast<size_t>(y - ty * kT) * kT + static_cast<size_t>(x - tx * kT)) * 4;
                    for (int32_t k = 0; k < run; ++k, s += 4, d += 4) over(d, s);
                }
                x += run;
            }
        }
    } else {
        // Bilinear resampling of the premultiplied floating pixels (transparent outside them).
        const canvas::Affine2 inv = move_.xf.inverse();
        const RectI& b = move_.bounds;
        auto texel = [&](int32_t x, int32_t y) -> const uint8_t* {
            if (x < b.x || y < b.y || x >= b.right() || y >= b.bottom()) return nullptr;
            const uint8_t* t = f.tile(x / kT, y / kT);
            return t ? t + (static_cast<size_t>(y % kT) * kT + static_cast<size_t>(x % kT)) * 4 : nullptr;
        };
        for (int32_t y = r.y; y < r.bottom(); ++y)
            for (int32_t x = r.x; x < r.right(); ++x) {
                const canvas::Vec2 s = inv.apply({x + 0.5, y + 0.5});
                const double fx = s.x - 0.5, fy = s.y - 0.5;
                if (fx < b.x - 1 || fy < b.y - 1 || fx >= b.right() || fy >= b.bottom()) continue;
                const int32_t ix = static_cast<int32_t>(std::floor(fx)), iy = static_cast<int32_t>(std::floor(fy));
                const float ax = static_cast<float>(fx - ix), ay = static_cast<float>(fy - iy);
                const uint8_t* t00 = texel(ix, iy);
                const uint8_t* t10 = texel(ix + 1, iy);
                const uint8_t* t01 = texel(ix, iy + 1);
                const uint8_t* t11 = texel(ix + 1, iy + 1);
                if (!t00 && !t10 && !t01 && !t11) continue;
                const float w00 = (1 - ax) * (1 - ay), w10 = ax * (1 - ay), w01 = (1 - ax) * ay, w11 = ax * ay;
                uint8_t sp[4];
                for (int c = 0; c < 4; ++c) {
                    const float v = (t00 ? t00[c] * w00 : 0.0f) + (t10 ? t10[c] * w10 : 0.0f) + (t01 ? t01[c] * w01 : 0.0f) +
                                    (t11 ? t11[c] * w11 : 0.0f);
                    sp[c] = static_cast<uint8_t>(std::min(255.0f, v + 0.5f));
                }
                // Premultiplied: no colour channel above alpha after rounding.
                for (int c = 0; c < 3; ++c) sp[c] = std::min(sp[c], sp[3]);
                over(px.data() + (static_cast<size_t>(y - ty * kT) * kT + static_cast<size_t>(x - tx * kT)) * 4, sp);
            }
    }
    RasterStore& pv = move_.preview;
    if (tileEmpty(px.data())) pv.releaseTile(tx, ty);
    else std::memcpy(pv.tileForWrite(tx, ty), px.data(), kPx * 4);
    move_.previewTiles[static_cast<size_t>(index)] = 1;
    move_.dirty.push_back(ls.tileRect(tx, ty));
}

LayerError CanvasDocument::moveTo(int32_t dx, int32_t dy) {
    return transformTo(canvas::Affine2::translate(dx, dy));
}

LayerError CanvasDocument::transformTo(const canvas::Affine2& m) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!move_.active) return LayerError::BadValue;
    const double det = m.determinant();
    if (!std::isfinite(det) || std::fabs(det) < 1e-6 || !std::isfinite(m.tx) || !std::isfinite(m.ty)) return LayerError::BadValue;
    const canvas::Affine2& o = move_.xf;
    if (m.a == o.a && m.b == o.b && m.c == o.c && m.d == o.d && m.tx == o.tx && m.ty == o.ty) return LayerError::None;
    Layer* a = findLayer(move_.layerId);
    if (!a) return LayerError::NotFound;
    move_.xf = m;
    move_.identity = m.a == 1.0 && m.b == 0.0 && m.c == 0.0 && m.d == 1.0 && m.tx == 0.0 && m.ty == 0.0;
    const RasterStore& ls = a->store;
    const int32_t cols = ls.columns(), rows = ls.rows();
    // Tiles of the new transform: where the content was lifted from, and where it lands.
    // (Scratch vectors of the move are reused: no allocation per drag step once warm.)
    std::vector<int32_t>& next = move_.next;
    next.clear();
    if (!move_.identity) {
        const RasterStore& f = move_.floating;
        for (int32_t ty = 0; ty < f.rows(); ++ty)
            for (int32_t tx = 0; tx < f.columns(); ++tx)
                if (f.tileAllocated(tx, ty)) next.push_back(ty * cols + tx);
        const RectI& b = move_.bounds;
        double x0 = 1e30, y0 = 1e30, x1 = -1e30, y1 = -1e30;
        const double cx[4] = {double(b.x), double(b.right()), double(b.right()), double(b.x)};
        const double cy[4] = {double(b.y), double(b.y), double(b.bottom()), double(b.bottom())};
        for (int k = 0; k < 4; ++k) {
            const canvas::Vec2 p = m.apply({cx[k], cy[k]});
            x0 = std::min(x0, p.x); y0 = std::min(y0, p.y); x1 = std::max(x1, p.x); y1 = std::max(y1, p.y);
        }
        const int32_t W = static_cast<int32_t>(ls.width()), H = static_cast<int32_t>(ls.height());
        const int32_t px0 = std::clamp(static_cast<int32_t>(std::floor(x0)) - 1, 0, W), py0 = std::clamp(static_cast<int32_t>(std::floor(y0)) - 1, 0, H);
        const int32_t px1 = std::clamp(static_cast<int32_t>(std::ceil(x1)) + 1, 0, W), py1 = std::clamp(static_cast<int32_t>(std::ceil(y1)) + 1, 0, H);
        if (px1 > px0 && py1 > py0)
            for (int32_t ty = py0 / kT; ty <= std::min(rows - 1, (py1 - 1) / kT); ++ty)
                for (int32_t tx = px0 / kT; tx <= std::min(cols - 1, (px1 - 1) / kT); ++tx) next.push_back(ty * cols + tx);
    }
    std::sort(next.begin(), next.end());
    next.erase(std::unique(next.begin(), next.end()), next.end());
    // Recomposed: the new set; tiles of the previous transform it no longer covers show the layer again.
    for (int32_t i : move_.shown)
        if (!std::binary_search(next.begin(), next.end(), i)) {
            move_.previewTiles[static_cast<size_t>(i)] = 0;
            move_.preview.releaseTile(i % cols, i / cols);
            move_.dirty.push_back(ls.tileRect(i % cols, i / cols));
        }
    for (int32_t i : next) composeMoveTileLocked(*a, i, move_.scratch);
    move_.shown.assign(next.begin(), next.end());
    bumpRevision();
    return LayerError::None;
}

void CanvasDocument::commitMoveLocked() {
    if (!move_.active) return;
    Layer* a = findLayer(move_.layerId);
    if (!a || move_.identity) { cancelMoveLocked(); return; }
    RasterStore& ls = a->store;
    PixelPatch patch;
    for (int32_t i : move_.shown) {
        const int32_t tx = i % ls.columns(), ty = i / ls.columns();
        const uint8_t* before = ls.tile(tx, ty);
        const uint8_t* after = move_.preview.tile(tx, ty);
        if (!before && !after) continue;
        if (before && after && std::memcmp(before, after, kPx * 4) == 0) continue;
        patch.addTile(tx, ty, ls.tileRect(tx, ty), before, after);
        // The confirmed pixels: what the preview showed.
        if (after) std::memcpy(ls.tileForWrite(tx, ty), after, kPx * 4);
        else ls.releaseTile(tx, ty);
        ls.markChanged(ls.tileRect(tx, ty));
    }
    int32_t dx = 0, dy = 0;
    if (integerShift(move_.xf, dx, dy)) selection_.translate(dx, dy);
    else selection_.transform(move_.xf);
    std::vector<std::unique_ptr<HistoryCommand>> parts;
    if (!patch.empty()) parts.push_back(std::make_unique<PixelCommand>(HistoryKind::MoveSelection, a->info.id, std::move(patch)));
    std::vector<uint8_t> after;
    selection_.encode(after);
    parts.push_back(std::make_unique<SelectionCommand>(std::move(move_.selectionBefore), std::move(after)));
    auto cmd = std::make_unique<GroupCommand>(HistoryKind::MoveSelection, std::move(parts));
    cmd->setLabel(a->info.name);
    history_.push(std::move(cmd));
    move_ = {};
    bumpRevision();
}

void CanvasDocument::cancelMoveLocked() {
    if (!move_.active) return;
    // The layer was never changed: the composite shows it again where the preview was.
    if (pending_.size() != composite_.tileCount()) pending_.assign(composite_.tileCount(), 0);
    for (int32_t i : move_.shown) {
        uint8_t& p = pending_[static_cast<size_t>(i)];
        if (!p) { p = 1; pendingCount_++; }
    }
    pendingComposite_.store(pendingCount_ > 0, std::memory_order_release);
    move_ = {};
    bumpRevision();
}

LayerError CanvasDocument::moveCommit() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!move_.active) return LayerError::BadValue;
    commitMoveLocked();
    return LayerError::None;
}

void CanvasDocument::moveCancel() {
    std::lock_guard<std::mutex> lock(mutex_);
    cancelMoveLocked();
}

// ------------------------------------------------------------------ queries
CanvasDocument::SelectionInfo CanvasDocument::selectionInfo() {
    std::lock_guard<std::mutex> lock(mutex_);
    SelectionInfo s;
    s.active = selection_.active();
    if (s.active) {
        selection_.bounds(s.bounds);
        s.area = selection_.area();
    }
    s.bytes = selection_.bytes();
    s.clipboardBytes = clipboard_.bytesAllocated();
    s.version = selection_.version();
    s.moving = move_.active;
    s.dx = static_cast<int32_t>(std::lround(move_.xf.tx));
    s.dy = static_cast<int32_t>(std::lround(move_.xf.ty));
    s.clipboard = hasClipboard_;
    if (hasClipboard_) {
        int32_t x0 = INT32_MAX, y0 = INT32_MAX, x1 = INT32_MIN, y1 = INT32_MIN;
        for (int32_t ty = 0; ty < clipboard_.rows(); ++ty)
            for (int32_t tx = 0; tx < clipboard_.columns(); ++tx)
                if (clipboard_.tileAllocated(tx, ty)) {
                    const RectI r = clipboard_.tileRect(tx, ty);
                    x0 = std::min(x0, r.x); y0 = std::min(y0, r.y); x1 = std::max(x1, r.right()); y1 = std::max(y1, r.bottom());
                }
        if (x1 > x0) s.clipboardBounds = {x0, y0, x1 - x0, y1 - y0};
    }
    return s;
}

void CanvasDocument::selectionContours(std::vector<float>& out) {
    std::lock_guard<std::mutex> lock(mutex_);
    selection_.contours(out);
}

void CanvasDocument::selectionPreview(uint32_t maxSide, uint32_t& w, uint32_t& h, std::vector<uint8_t>& out) {
    std::lock_guard<std::mutex> lock(mutex_);
    selection_.preview(maxSide, w, h, out);
}

uint8_t CanvasDocument::selectionAt(int32_t x, int32_t y) {
    std::lock_guard<std::mutex> lock(mutex_);
    return selection_.at(x, y);
}

uint64_t CanvasDocument::selectionVersion() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return (selection_.version() << 1) | (selection_.active() ? 1u : 0u);
}

uint64_t CanvasDocument::selectionChecksum() {
    std::lock_guard<std::mutex> lock(mutex_);
    return selection_.checksum();
}

}  // namespace pe
