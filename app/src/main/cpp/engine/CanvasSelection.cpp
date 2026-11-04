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
    selection_.encode(move_.selectionBefore);
    move_.floating.reset(composite_.width(), composite_.height());
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
    // Nothing changes on screen yet: at offset 0 the layer is its original.
    move_.active = true;
    return LayerError::None;
}

// Layer tile `index` for the current offset: original at (0, 0); otherwise the
// hole (original x (1 - selection)) with the shifted floating pixels over it.
void CanvasDocument::composeMoveTileLocked(Layer& l, int32_t index, std::vector<uint8_t>& px) {
    RasterStore& ls = l.store;
    const int32_t tx = index % ls.columns(), ty = index / ls.columns();
    auto it = move_.orig.find(index);
    if (it == move_.orig.end()) {
        // First touch: keep the original.
        std::unique_ptr<uint8_t[]> keep;
        if (const uint8_t* cur = ls.tile(tx, ty)) {
            keep.reset(new uint8_t[kPx * 4]);
            std::memcpy(keep.get(), cur, kPx * 4);
        }
        it = move_.orig.emplace(index, std::move(keep)).first;
    }
    const uint8_t* orig = it->second.get();
    px.assign(kPx * 4, 0);
    if (orig) std::memcpy(px.data(), orig, kPx * 4);
    const int32_t dx = move_.dx, dy = move_.dy;
    if (dx != 0 || dy != 0) {
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
        // Shifted floating pixels over it (premultiplied src-over), clipped to the canvas.
        const RasterStore& f = move_.floating;
        const RectI r = ls.tileRect(tx, ty);   // canvas part of this tile
        for (int32_t y = r.y; y < r.bottom(); ++y) {
            const int32_t sy = y - dy;
            if (sy < 0 || sy >= static_cast<int32_t>(f.height())) continue;
            int32_t x = r.x;
            while (x < r.right()) {
                const int32_t sx = x - dx;
                if (sx < 0) { x += -sx; continue; }
                if (sx >= static_cast<int32_t>(f.width())) break;
                const int32_t stx = sx / kT, run = std::min(r.right() - x, kT - sx % kT);
                const uint8_t* sp = f.tile(stx, sy / kT);
                if (sp) {
                    const uint8_t* s = sp + (static_cast<size_t>(sy % kT) * kT + static_cast<size_t>(sx % kT)) * 4;
                    uint8_t* d = px.data() + (static_cast<size_t>(y - ty * kT) * kT + static_cast<size_t>(x - tx * kT)) * 4;
                    for (int32_t k = 0; k < run; ++k, s += 4, d += 4) {
                        const uint32_t sa = s[3];
                        if (sa == 0) continue;
                        if (sa == 255) { std::memcpy(d, s, 4); continue; }
                        for (int c = 0; c < 4; ++c)
                            d[c] = static_cast<uint8_t>(std::min<uint32_t>(255, s[c] + (d[c] * (255 - sa) + 127u) / 255u));
                    }
                }
                x += run;
            }
        }
    }
    if (tileEmpty(px.data())) ls.releaseTile(tx, ty);
    else std::memcpy(ls.tileForWrite(tx, ty), px.data(), kPx * 4);
    ls.markChanged(ls.tileRect(tx, ty));
}

LayerError CanvasDocument::moveTo(int32_t dx, int32_t dy) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!move_.active) return LayerError::BadValue;
    if (dx == move_.dx && dy == move_.dy) return LayerError::None;
    Layer* a = findLayer(move_.layerId);
    if (!a) return LayerError::NotFound;
    move_.dx = dx;
    move_.dy = dy;
    RasterStore& ls = a->store;
    const int32_t cols = ls.columns(), rows = ls.rows();
    // Tiles of the new offset: where the content was lifted from, and where it lands.
    std::vector<int32_t> next;
    if (dx != 0 || dy != 0) {
        const RasterStore& f = move_.floating;
        for (int32_t ty = 0; ty < f.rows(); ++ty)
            for (int32_t tx = 0; tx < f.columns(); ++tx) {
                if (!f.tileAllocated(tx, ty)) continue;
                next.push_back(ty * cols + tx);
                const int32_t x0 = tx * kT + dx, y0 = ty * kT + dy;
                const int32_t ax = static_cast<int32_t>(std::floor(x0 / static_cast<double>(kT)));
                const int32_t ay = static_cast<int32_t>(std::floor(y0 / static_cast<double>(kT)));
                for (int32_t j = ay; j <= ay + 1; ++j)
                    for (int32_t i = ax; i <= ax + 1; ++i)
                        if (i >= 0 && j >= 0 && i < cols && j < rows) next.push_back(j * cols + i);
            }
    }
    std::sort(next.begin(), next.end());
    next.erase(std::unique(next.begin(), next.end()), next.end());
    // Recomposed: the new set, plus tiles of the previous offset that it no longer covers (back to original).
    std::vector<int32_t> work = next;
    work.insert(work.end(), move_.shown.begin(), move_.shown.end());
    std::sort(work.begin(), work.end());
    work.erase(std::unique(work.begin(), work.end()), work.end());
    std::vector<uint8_t> scratch;
    for (int32_t i : work) composeMoveTileLocked(*a, i, scratch);
    move_.shown = std::move(next);
    bumpRevision();
    return LayerError::None;
}

void CanvasDocument::commitMoveLocked() {
    if (!move_.active) return;
    Layer* a = findLayer(move_.layerId);
    if (!a || (move_.dx == 0 && move_.dy == 0)) { cancelMoveLocked(); return; }
    RasterStore& ls = a->store;
    std::vector<int32_t> idx;
    for (const auto& kv : move_.orig) idx.push_back(kv.first);
    std::sort(idx.begin(), idx.end());
    PixelPatch patch;
    for (int32_t i : idx) {
        const int32_t tx = i % ls.columns(), ty = i / ls.columns();
        const uint8_t* before = move_.orig[i].get();
        const uint8_t* after = ls.tile(tx, ty);
        if (!before && !after) continue;
        if (before && after && std::memcmp(before, after, kPx * 4) == 0) continue;
        patch.addTile(tx, ty, ls.tileRect(tx, ty), before, after);
    }
    selection_.translate(move_.dx, move_.dy);
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
    if (Layer* a = findLayer(move_.layerId)) {
        RasterStore& ls = a->store;
        for (const auto& [i, px] : move_.orig) {
            const int32_t tx = i % ls.columns(), ty = i / ls.columns();
            if (px) std::memcpy(ls.tileForWrite(tx, ty), px.get(), kPx * 4);
            else ls.releaseTile(tx, ty);
            ls.markChanged(ls.tileRect(tx, ty));
        }
    }
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
    s.dx = move_.dx;
    s.dy = move_.dy;
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
