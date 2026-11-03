#include "engine/CanvasDocument.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>

#include "core/Time.h"

namespace pe {

using canvas::RasterStore;
using canvas::RectI;

namespace {
void putI32(std::vector<uint8_t>& out, int32_t v) {
    const uint32_t u = static_cast<uint32_t>(v);
    out.push_back(static_cast<uint8_t>(u));
    out.push_back(static_cast<uint8_t>(u >> 8));
    out.push_back(static_cast<uint8_t>(u >> 16));
    out.push_back(static_cast<uint8_t>(u >> 24));
}
int32_t getI32(const uint8_t* p) {
    return static_cast<int32_t>(static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
                                (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24));
}
void setI32(std::vector<uint8_t>& out, size_t at, int32_t v) {
    const uint32_t u = static_cast<uint32_t>(v);
    for (int i = 0; i < 4; ++i) out[at + i] = static_cast<uint8_t>(u >> (8 * i));
}
}  // namespace

const char* layerErrorName(LayerError e) {
    switch (e) {
        case LayerError::None: return "ok";
        case LayerError::NoCanvas: return "no_canvas";
        case LayerError::NotFound: return "not_found";
        case LayerError::LastLayer: return "last_layer";
        case LayerError::NoLayerBelow: return "no_layer_below";
        case LayerError::Locked: return "locked";
        case LayerError::Hidden: return "hidden";
        case LayerError::TooMany: return "too_many";
        case LayerError::Memory: return "memory";
        case LayerError::BadValue: return "bad_value";
    }
    return "?";
}

void CanvasDocument::open(uint32_t width, uint32_t height) {
    std::lock_guard<std::mutex> lock(mutex_);
    brush_.discard();
    layers_.clear();
    auto l = std::make_unique<Layer>();
    l->info.id = 1;
    l->info.name = "Layer 1";
    l->store.reset(width, height);
    layers_.push_back(std::move(l));
    activeId_ = 1;
    nextId_ = 2;
    composite_.reset(width, height);
    pending_.assign(composite_.tileCount(), 0);
    pendingCount_ = 0;
    open_ = true;
    refreshEditable();
    generation_.fetch_add(1, std::memory_order_acq_rel);
    bumpRevision();
}

void CanvasDocument::close() {
    std::lock_guard<std::mutex> lock(mutex_);
    brush_.discard();
    layers_.clear();
    composite_.reset(0, 0);
    open_ = false;
    refreshEditable();
    generation_.fetch_add(1, std::memory_order_acq_rel);
    bumpRevision();
}

LayerError CanvasDocument::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!open_) return LayerError::NoCanvas;
    settleInputLocked();
    Layer* a = activeLayer();
    if (!a) return LayerError::NotFound;
    if (a->info.locked) return LayerError::Locked;
    for (int32_t ty = 0; ty < a->store.rows(); ++ty)
        for (int32_t tx = 0; tx < a->store.columns(); ++tx)
            if (a->store.tileAllocated(tx, ty)) {
                // Freed; marked changed, so the next save writes it as empty.
                a->store.releaseTile(tx, ty);
                a->store.markChanged(a->store.tileRect(tx, ty));
            }
    bumpRevision();
    return LayerError::None;
}

bool CanvasDocument::isOpen() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return open_;
}

void CanvasDocument::setBackground(uint32_t paperArgb, bool transparent) {
    std::lock_guard<std::mutex> lock(mutex_);
    const canvas::Backdrop b = canvas::Backdrop::fromArgb(paperArgb, transparent);
    if (b.opaque == backdrop_.opaque && b.r == backdrop_.r && b.g == backdrop_.g && b.b == backdrop_.b) return;
    backdrop_ = b;
    invalidateAll();
}

void CanvasDocument::setMemoryBudget(uint64_t bytes) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (bytes > 0) budget_ = bytes;
}

int32_t CanvasDocument::indexOf(uint32_t id) const {
    for (size_t i = 0; i < layers_.size(); ++i)
        if (layers_[i]->info.id == id) return static_cast<int32_t>(i);
    return -1;
}

CanvasDocument::Layer* CanvasDocument::activeLayer() {
    const int32_t i = indexOf(activeId_);
    return i >= 0 ? layers_[static_cast<size_t>(i)].get() : nullptr;
}

void CanvasDocument::refreshEditable() {
    const int32_t i = indexOf(activeId_);
    const bool ok = open_ && i >= 0 && layers_[static_cast<size_t>(i)]->info.visible &&
                    !layers_[static_cast<size_t>(i)]->info.locked;
    activeEditable_.store(ok, std::memory_order_release);
}

uint64_t CanvasDocument::layerBytes() const {
    uint64_t b = 0;
    for (const auto& l : layers_) b += l->store.bytesAllocated();
    return b;
}

std::string CanvasDocument::uniqueName(const std::string& base) const {
    auto taken = [&](const std::string& n) {
        for (const auto& l : layers_)
            if (l->info.name == n) return true;
        return false;
    };
    if (!taken(base)) return base;
    for (int i = 2;; ++i) {
        const std::string n = base + " " + std::to_string(i);
        if (!taken(n)) return n;
    }
}

void CanvasDocument::settleInputLocked() {
    Layer* a = activeLayer();
    if (!a) return;
    brush_.process(a->store);
    brush_.finishActive(a->store);
}

void CanvasDocument::markTilesOf(const RasterStore& st) {
    if (pending_.size() != composite_.tileCount()) pending_.assign(composite_.tileCount(), 0);
    for (int32_t ty = 0; ty < st.rows(); ++ty)
        for (int32_t tx = 0; tx < st.columns(); ++tx)
            if (st.tileAllocated(tx, ty)) {
                uint8_t& p = pending_[composite_.index(tx, ty)];
                if (!p) {
                    p = 1;
                    pendingCount_++;
                }
            }
    pendingComposite_.store(pendingCount_ > 0, std::memory_order_release);
}

void CanvasDocument::markAllContent() {
    for (const auto& l : layers_) markTilesOf(l->store);
    markTilesOf(composite_);
}

void CanvasDocument::updateLocked(bool complete) {
    Layer* a = activeLayer();
    if (a) brush_.process(a->store);
    const int64_t t0 = nowNs();
    viewScratch_.clear();
    for (const auto& l : layers_)
        if (l->info.visible && l->info.opacity > 0.0f) viewScratch_.push_back({&l->store, l->info.opacity, l->info.blend});
    uint64_t px = 0;
    for (const auto& l : layers_) {
        dirtyScratch_.clear();
        l->store.takeGpuDirty(dirtyScratch_);
        // A hidden layer's changes do not show; its content is composited when shown.
        if (!l->info.visible || dirtyScratch_.empty()) continue;
        if (l.get() == a) {
            // Strokes: exactly the changed rectangles, now (the stroke must show this frame).
            for (const auto& r : dirtyScratch_) {
                canvas::Compositor::composite(viewScratch_.data(), viewScratch_.size(), r, backdrop_, composite_);
                px += static_cast<uint64_t>(r.area());
            }
        } else {
            // Imports / duplicates / merges into other layers: whole tiles, deduplicated.
            if (pending_.size() != composite_.tileCount()) pending_.assign(composite_.tileCount(), 0);
            for (const auto& r : dirtyScratch_) {
                const size_t i = composite_.index(r.x / RasterStore::kTile, r.y / RasterStore::kTile);
                if (!pending_[i]) {
                    pending_[i] = 1;
                    pendingCount_++;
                }
            }
        }
    }
    if (pendingCount_ > 0) {
        // While the user paints the stale tiles get a small share of the frame.
        const int64_t budgetNs = complete ? INT64_MAX : (brush_.hasPending() || brush_.strokeActive() ? 3'000'000 : 8'000'000);
        for (size_t i = 0; i < pending_.size() && pendingCount_ > 0; ++i) {
            if (!pending_[i]) continue;
            const int32_t tx = static_cast<int32_t>(i % static_cast<size_t>(composite_.columns()));
            const int32_t ty = static_cast<int32_t>(i / static_cast<size_t>(composite_.columns()));
            const RectI r = composite_.tileRect(tx, ty);
            canvas::Compositor::composite(viewScratch_.data(), viewScratch_.size(), r, backdrop_, composite_);
            px += static_cast<uint64_t>(r.area());
            pending_[i] = 0;
            pendingCount_--;
            if (nowNs() - t0 > budgetNs) break;
        }
        pendingComposite_.store(pendingCount_ > 0, std::memory_order_release);
    }
    if (px > 0) {
        compositedPixels_ += px;
        lastCompositeMs_ = static_cast<double>(nowNs() - t0) / 1e6;
        maxCompositeMs_ = std::max(maxCompositeMs_, lastCompositeMs_);
    }
}

void CanvasDocument::fill(const RectI& rect, uint32_t argb) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!open_) return;
    settleInputLocked();  // keep the order: queued strokes first
    Layer* a = activeLayer();
    if (!a) return;
    a->store.fill(rect, canvas::Rgba8::fromArgb(argb));
    bumpRevision();
}

size_t CanvasDocument::loadLegacy(const std::vector<ContentOp>& ops) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!open_) return 0;
    brush_.discard();
    const uint32_t w = composite_.width(), h = composite_.height();
    layers_.clear();
    auto l = std::make_unique<Layer>();
    l->info.id = 1;
    l->info.name = "Layer 1";
    l->store.reset(w, h);
    size_t kept = 0;
    for (const auto& op : ops) {
        const RectI r = RectI{op.x, op.y, op.w, op.h}.intersect(l->store.canvasRect());
        if (r.empty()) continue;
        l->store.fill(r, canvas::Rgba8::fromArgb(op.argb));
        kept++;
    }
    // Imported content is not on disk in the new format yet.
    l->store.markAllUnsaved();
    layers_.push_back(std::move(l));
    activeId_ = 1;
    nextId_ = 2;
    composite_.reset(w, h);
    pending_.assign(composite_.tileCount(), 0);
    pendingCount_ = 0;
    markAllContent();
    refreshEditable();
    generation_.fetch_add(1, std::memory_order_acq_rel);
    bumpRevision();
    return kept;
}

bool CanvasDocument::strokeBegin(const brush::BrushSettings& settings, const brush::StrokePoint& first) {
    // Hidden or locked active layer: nothing is painted (the UI says why).
    if (!activeEditable()) return false;
    brush_.begin(settings, first);
    bumpRevision();
    return true;
}

void CanvasDocument::strokeSamples(const brush::StrokePoint* points, size_t count) {
    if (count == 0) return;
    brush_.addSamples(points, count);
    bumpRevision();
}

void CanvasDocument::strokeEnd() {
    brush_.end();
    bumpRevision();
}

void CanvasDocument::strokeCancel() {
    brush_.cancel();
    bumpRevision();
}

// ------------------------------------------------------------------ layers
std::vector<LayerSummary> CanvasDocument::layers(uint32_t& activeId) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<LayerSummary> out;
    for (const auto& l : layers_) out.push_back({l->info, l->store.tilesAllocated(), l->store.version()});
    activeId = activeId_;
    return out;
}

LayerError CanvasDocument::addLayer(uint32_t aboveId, const std::string& name, uint32_t& newId) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!open_) return LayerError::NoCanvas;
    if (layers_.size() >= kMaxLayers) return LayerError::TooMany;
    int32_t at = static_cast<int32_t>(layers_.size());
    if (aboveId != 0) {
        const int32_t i = indexOf(aboveId);
        if (i < 0) return LayerError::NotFound;
        at = i + 1;
    }
    settleInputLocked();
    auto l = std::make_unique<Layer>();
    l->info.id = nextId_++;
    l->info.name = uniqueName(name.empty() ? "Layer " + std::to_string(l->info.id) : name);
    l->store.reset(composite_.width(), composite_.height());
    newId = l->info.id;
    layers_.insert(layers_.begin() + at, std::move(l));
    activeId_ = newId;
    refreshEditable();
    bumpRevision();  // an empty layer changes no pixel: no recomposite
    return LayerError::None;
}

LayerError CanvasDocument::deleteLayer(uint32_t id) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!open_) return LayerError::NoCanvas;
    const int32_t i = indexOf(id);
    if (i < 0) return LayerError::NotFound;
    if (layers_.size() <= 1) return LayerError::LastLayer;
    settleInputLocked();
    markTilesOf(layers_[static_cast<size_t>(i)]->store);
    layers_.erase(layers_.begin() + i);
    if (activeId_ == id) activeId_ = layers_[static_cast<size_t>(std::max(0, i - 1))]->info.id;
    refreshEditable();
    bumpRevision();
    return LayerError::None;
}

LayerError CanvasDocument::duplicateLayer(uint32_t id, const std::string& name, uint32_t& newId) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!open_) return LayerError::NoCanvas;
    const int32_t i = indexOf(id);
    if (i < 0) return LayerError::NotFound;
    if (layers_.size() >= kMaxLayers) return LayerError::TooMany;
    const Layer& src = *layers_[static_cast<size_t>(i)];
    if (layerBytes() + src.store.bytesAllocated() > budget_) return LayerError::Memory;
    settleInputLocked();
    auto l = std::make_unique<Layer>();
    l->info = src.info;
    l->info.id = nextId_++;
    l->info.locked = false;
    l->info.name = uniqueName(name.empty() ? src.info.name + " copy" : name);
    l->store.copyFrom(src.store);  // every tile unsaved (to save)
    dirtyScratch_.clear();
    l->store.takeGpuDirty(dirtyScratch_);
    markTilesOf(l->store);         // recomposited progressively, not in one frame
    newId = l->info.id;
    layers_.insert(layers_.begin() + i + 1, std::move(l));
    activeId_ = newId;
    refreshEditable();
    bumpRevision();
    return LayerError::None;
}

LayerError CanvasDocument::renameLayer(uint32_t id, const std::string& name) {
    std::lock_guard<std::mutex> lock(mutex_);
    const int32_t i = indexOf(id);
    if (i < 0) return open_ ? LayerError::NotFound : LayerError::NoCanvas;
    if (name.empty() || name.size() > 200) return LayerError::BadValue;
    layers_[static_cast<size_t>(i)]->info.name = name;
    bumpRevision();
    return LayerError::None;
}

LayerError CanvasDocument::moveLayer(uint32_t id, int32_t index) {
    std::lock_guard<std::mutex> lock(mutex_);
    const int32_t i = indexOf(id);
    if (i < 0) return open_ ? LayerError::NotFound : LayerError::NoCanvas;
    if (index < 0 || index >= static_cast<int32_t>(layers_.size())) return LayerError::BadValue;
    if (index == i) return LayerError::None;
    settleInputLocked();
    markTilesOf(layers_[static_cast<size_t>(i)]->store);  // order only matters where it has content
    auto l = std::move(layers_[static_cast<size_t>(i)]);
    layers_.erase(layers_.begin() + i);
    layers_.insert(layers_.begin() + index, std::move(l));
    bumpRevision();
    return LayerError::None;
}

LayerError CanvasDocument::setVisible(uint32_t id, bool visible) {
    std::lock_guard<std::mutex> lock(mutex_);
    const int32_t i = indexOf(id);
    if (i < 0) return open_ ? LayerError::NotFound : LayerError::NoCanvas;
    Layer& l = *layers_[static_cast<size_t>(i)];
    if (l.info.visible == visible) return LayerError::None;
    settleInputLocked();
    l.info.visible = visible;
    markTilesOf(l.store);
    refreshEditable();
    bumpRevision();
    return LayerError::None;
}

LayerError CanvasDocument::setLocked(uint32_t id, bool locked) {
    std::lock_guard<std::mutex> lock(mutex_);
    const int32_t i = indexOf(id);
    if (i < 0) return open_ ? LayerError::NotFound : LayerError::NoCanvas;
    settleInputLocked();
    layers_[static_cast<size_t>(i)]->info.locked = locked;
    refreshEditable();
    bumpRevision();
    return LayerError::None;
}

LayerError CanvasDocument::setOpacity(uint32_t id, float opacity) {
    std::lock_guard<std::mutex> lock(mutex_);
    const int32_t i = indexOf(id);
    if (i < 0) return open_ ? LayerError::NotFound : LayerError::NoCanvas;
    if (!std::isfinite(opacity)) return LayerError::BadValue;
    Layer& l = *layers_[static_cast<size_t>(i)];
    opacity = std::clamp(opacity, 0.0f, 1.0f);
    if (l.info.opacity == opacity) return LayerError::None;
    l.info.opacity = opacity;
    if (l.info.visible) markTilesOf(l.store);
    bumpRevision();
    return LayerError::None;
}

LayerError CanvasDocument::setBlend(uint32_t id, canvas::BlendMode mode) {
    std::lock_guard<std::mutex> lock(mutex_);
    const int32_t i = indexOf(id);
    if (i < 0) return open_ ? LayerError::NotFound : LayerError::NoCanvas;
    if (static_cast<int>(mode) >= canvas::kBlendModeCount) return LayerError::BadValue;
    Layer& l = *layers_[static_cast<size_t>(i)];
    if (l.info.blend == mode) return LayerError::None;
    l.info.blend = mode;
    if (l.info.visible) markTilesOf(l.store);
    bumpRevision();
    return LayerError::None;
}

LayerError CanvasDocument::setActive(uint32_t id) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (indexOf(id) < 0) return open_ ? LayerError::NotFound : LayerError::NoCanvas;
    if (activeId_ == id) return LayerError::None;
    settleInputLocked();  // strokes already drawn stay on the layer they were drawn on
    activeId_ = id;
    refreshEditable();
    return LayerError::None;
}

LayerError CanvasDocument::mergeDown(uint32_t id) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!open_) return LayerError::NoCanvas;
    const int32_t i = indexOf(id);
    if (i < 0) return LayerError::NotFound;
    if (i == 0) return LayerError::NoLayerBelow;
    Layer& upper = *layers_[static_cast<size_t>(i)];
    Layer& lower = *layers_[static_cast<size_t>(i - 1)];
    if (upper.info.locked || lower.info.locked) return LayerError::Locked;
    if (!upper.info.visible || !lower.info.visible) return LayerError::Hidden;
    // The result needs at most the tiles of both, alive next to them until the swap.
    if (layerBytes() + upper.store.bytesAllocated() + lower.store.bytesAllocated() > budget_) return LayerError::Memory;
    settleInputLocked();
    // The pair as an isolated group over transparency: lower (its opacity
    // baked, Normal), then upper with its own opacity and blend mode.
    canvas::RasterStore merged;
    merged.reset(composite_.width(), composite_.height());
    const canvas::LayerView pair[2] = {{&lower.store, lower.info.opacity, canvas::BlendMode::Normal},
                                       {&upper.store, upper.info.opacity, upper.info.blend}};
    canvas::Compositor::flattenInto(pair, 2, merged.canvasRect(), merged);
    merged.markAllUnsaved();
    // Tiles the lower layer had but the result does not: saved as empty.
    for (int32_t ty = 0; ty < merged.rows(); ++ty)
        for (int32_t tx = 0; tx < merged.columns(); ++tx)
            if (lower.store.tileAllocated(tx, ty) && !merged.tileAllocated(tx, ty)) {
                merged.tileForWrite(tx, ty);
                merged.markChanged(merged.tileRect(tx, ty));
            }
    merged.markAllUnsaved();
    markTilesOf(upper.store);
    markTilesOf(lower.store);
    std::swap(lower.store, merged);
    lower.store.takeGpuDirty(dirtyScratch_);  // covered by the marks above
    dirtyScratch_.clear();
    lower.info.opacity = 1.0f;
    if (activeId_ == upper.info.id) activeId_ = lower.info.id;
    layers_.erase(layers_.begin() + i);
    refreshEditable();
    bumpRevision();
    return LayerError::None;
}

LayerError CanvasDocument::flatten(const std::string& name) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!open_) return LayerError::NoCanvas;
    for (const auto& l : layers_)
        if (l->info.visible && l->info.locked) return LayerError::Locked;
    settleInputLocked();
    viewScratch_.clear();
    for (const auto& l : layers_)
        if (l->info.visible) viewScratch_.push_back({&l->store, l->info.opacity, l->info.blend});
    auto result = std::make_unique<Layer>();
    result->info.id = layers_.front()->info.id;  // the bottom layer's id survives
    result->info.name = name.empty() ? layers_.front()->info.name : name;
    result->store.reset(composite_.width(), composite_.height());
    canvas::Compositor::flattenInto(viewScratch_.data(), viewScratch_.size(), result->store.canvasRect(), result->store);
    // Every tile the old bottom layer had is rewritten (empty where nothing is left).
    for (int32_t ty = 0; ty < result->store.rows(); ++ty)
        for (int32_t tx = 0; tx < result->store.columns(); ++tx)
            if (layers_.front()->store.tileAllocated(tx, ty) && !result->store.tileAllocated(tx, ty))
                result->store.tileForWrite(tx, ty);
    result->store.markAllUnsaved();
    markAllContent();
    layers_.clear();
    activeId_ = result->info.id;
    dirtyScratch_.clear();
    result->store.takeGpuDirty(dirtyScratch_);
    layers_.push_back(std::move(result));
    markAllContent();
    refreshEditable();
    bumpRevision();
    return LayerError::None;
}

bool CanvasDocument::setLayerStack(const std::vector<LayerInfo>& stack, uint32_t activeId) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!open_ || stack.empty() || stack.size() > kMaxLayers) return false;
    uint32_t maxId = 0;
    bool activeFound = false;
    for (size_t i = 0; i < stack.size(); ++i) {
        if (stack[i].id == 0 || static_cast<int>(stack[i].blend) >= canvas::kBlendModeCount) return false;
        for (size_t j = 0; j < i; ++j)
            if (stack[j].id == stack[i].id) return false;
        maxId = std::max(maxId, stack[i].id);
        activeFound |= stack[i].id == activeId;
    }
    brush_.discard();
    layers_.clear();
    for (const auto& info : stack) {
        auto l = std::make_unique<Layer>();
        l->info = info;
        l->info.opacity = std::isfinite(info.opacity) ? std::clamp(info.opacity, 0.0f, 1.0f) : 1.0f;
        if (l->info.name.empty()) l->info.name = "Layer " + std::to_string(info.id);
        l->store.reset(composite_.width(), composite_.height());
        layers_.push_back(std::move(l));
    }
    activeId_ = activeFound ? activeId : layers_.back()->info.id;
    nextId_ = maxId + 1;
    markAllContent();  // the old content's composite tiles become empty
    refreshEditable();
    bumpRevision();
    return true;
}

bool CanvasDocument::size(uint64_t generation, uint32_t& width, uint32_t& height) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!open_ || generation != generation_.load(std::memory_order_relaxed)) return false;
    width = composite_.width();
    height = composite_.height();
    return true;
}

size_t CanvasDocument::exportLayerTiles(uint32_t id, bool changedOnly, size_t maxTiles, std::vector<uint8_t>& out) {
    std::vector<canvas::TileCoord> list;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const int32_t i = indexOf(id);
        if (!open_ || i < 0) return 0;
        settleInputLocked();
        const RasterStore& st = layers_[static_cast<size_t>(i)]->store;
        if (changedOnly) {
            st.unsavedTiles(list);
        } else {
            for (int32_t ty = 0; ty < st.rows(); ++ty)
                for (int32_t tx = 0; tx < st.columns(); ++tx)
                    if (st.tileAllocated(tx, ty)) list.push_back({tx, ty});
        }
    }
    const size_t countAt = out.size();
    putI32(out, 0);
    size_t n = 0;
    const uint64_t gen = generation();
    for (const auto& t : list) {
        if (n >= maxTiles) break;
        // One tile per lock: a save never holds up the render thread longer.
        std::lock_guard<std::mutex> lock(mutex_);
        const int32_t i = indexOf(id);
        if (!open_ || i < 0 || generation_.load(std::memory_order_relaxed) != gen) break;
        RasterStore& st = layers_[static_cast<size_t>(i)]->store;
        const bool empty = st.tileEmpty(t.tx, t.ty);
        putI32(out, t.tx);
        putI32(out, t.ty);
        putI32(out, empty ? kTileEmpty : 0);
        if (!empty) {
            const uint8_t* px = st.tile(t.tx, t.ty);
            out.insert(out.end(), px, px + RasterStore::kTileBytes);
        } else if (st.tileAllocated(t.tx, t.ty)) {
            st.releaseTile(t.tx, t.ty);  // nothing left there: free it
        }
        st.markSaved(t.tx, t.ty);
        n++;
    }
    setI32(out, countAt, static_cast<int32_t>(n));
    return n;
}

long CanvasDocument::importLayerTiles(uint32_t id, const uint8_t* data, size_t size) {
    if (!data || size < 4) return -1;
    const int32_t count = getI32(data);
    if (count < 0) return -1;
    size_t off = 4;
    long imported = 0;
    std::lock_guard<std::mutex> lock(mutex_);
    if (!open_) return 0;
    const int32_t li = indexOf(id);
    if (li < 0) return -1;
    RasterStore& st = layers_[static_cast<size_t>(li)]->store;
    for (int32_t i = 0; i < count; ++i) {
        if (off + kRecordHeader > size) return -1;
        const int32_t tx = getI32(data + off), ty = getI32(data + off + 4), flags = getI32(data + off + 8);
        off += kRecordHeader;
        if (flags & kTileEmpty) continue;
        if (off + RasterStore::kTileBytes > size) return -1;
        if (!st.validTile(tx, ty)) return -1;
        st.importTile(tx, ty, data + off);
        off += RasterStore::kTileBytes;
        imported++;
    }
    bumpRevision();
    return imported;
}

size_t CanvasDocument::exportTiles(bool changedOnly, size_t maxTiles, std::vector<uint8_t>& out) {
    uint32_t id;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        id = activeId_;
    }
    return exportLayerTiles(id, changedOnly, maxTiles, out);
}

long CanvasDocument::importTiles(const uint8_t* data, size_t size) {
    uint32_t id;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        id = activeId_;
    }
    return importLayerTiles(id, data, size);
}

bool CanvasDocument::thumbnail(uint32_t maxSide, uint32_t& outW, uint32_t& outH, std::vector<uint32_t>& argb) {
    return thumbnailOf(0, true, maxSide, outW, outH, argb);
}

bool CanvasDocument::layerThumbnail(uint32_t id, uint32_t maxSide, uint32_t& outW, uint32_t& outH,
                                    std::vector<uint32_t>& argb) {
    return thumbnailOf(id, false, maxSide, outW, outH, argb);
}

bool CanvasDocument::thumbnailOf(uint32_t layerId, bool composite, uint32_t maxSide, uint32_t& outW, uint32_t& outH,
                                 std::vector<uint32_t>& argb) {
    // The store to read, looked up again under every lock (layers may change between tiles).
    auto storeLocked = [&]() -> const RasterStore* {
        if (composite) return &composite_;
        const int32_t i = indexOf(layerId);
        return i >= 0 ? &layers_[static_cast<size_t>(i)]->store : nullptr;
    };
    uint32_t w, h;
    int32_t cols, rows;
    uint64_t gen;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!open_ || composite_.width() == 0) return false;
        updateLocked();
        const RasterStore* st = storeLocked();
        if (!st) return false;
        w = st->width();
        h = st->height();
        cols = st->columns();
        rows = st->rows();
        gen = generation_.load(std::memory_order_relaxed);
    }
    maxSide = std::max<uint32_t>(1, maxSide);
    const double scale = std::min(1.0, static_cast<double>(maxSide) / std::max(w, h));
    const uint32_t tw = std::max<uint32_t>(1, static_cast<uint32_t>(std::lround(w * scale)));
    const uint32_t th = std::max<uint32_t>(1, static_cast<uint32_t>(std::lround(h * scale)));
    // Canvas pixel -> thumbnail pixel; on big canvases only every `step`-th
    // pixel is read (still >= 2 samples per thumbnail pixel and axis).
    const int32_t step = std::max(1, static_cast<int32_t>(std::floor(0.5 / scale)));
    std::vector<int32_t> mapX(w), mapY(h);
    for (uint32_t x = 0; x < w; ++x) mapX[x] = std::min<int32_t>(tw - 1, static_cast<int32_t>(x * tw / w));
    for (uint32_t y = 0; y < h; ++y) mapY[y] = std::min<int32_t>(th - 1, static_cast<int32_t>(y * th / h));
    std::vector<uint32_t> countX(tw, 0), countY(th, 0);
    for (uint32_t x = 0; x < w; x += step) countX[mapX[x]]++;
    for (uint32_t y = 0; y < h; y += step) countY[mapY[y]]++;
    std::vector<float> acc(static_cast<size_t>(tw) * th * 4, 0.0f);
    for (int32_t ty = 0; ty < rows; ++ty) {
        for (int32_t tx = 0; tx < cols; ++tx) {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!open_ || generation_.load(std::memory_order_relaxed) != gen) return false;
            const RasterStore* st = storeLocked();
            if (!st) return false;
            const uint8_t* px = st->tile(tx, ty);
            if (!px) continue;
            const RectI r = st->tileRect(tx, ty);
            const int32_t y0 = ((r.y + step - 1) / step) * step, x0 = ((r.x + step - 1) / step) * step;
            for (int32_t y = y0; y < r.bottom(); y += step) {
                const uint8_t* row = px + static_cast<size_t>(y - r.y) * RasterStore::kTileStride;
                float* dstRow = acc.data() + static_cast<size_t>(mapY[y]) * tw * 4;
                for (int32_t x = x0; x < r.right(); x += step) {
                    const uint8_t* p = row + static_cast<size_t>(x - r.x) * 4;
                    float* d = dstRow + static_cast<size_t>(mapX[x]) * 4;
                    d[0] += p[0];
                    d[1] += p[1];
                    d[2] += p[2];
                    d[3] += p[3];
                }
            }
        }
    }
    argb.assign(static_cast<size_t>(tw) * th, 0);
    for (uint32_t y = 0; y < th; ++y) {
        for (uint32_t x = 0; x < tw; ++x) {
            const float n = static_cast<float>(countX[x]) * static_cast<float>(countY[y]);
            if (n <= 0) continue;
            const float* d = acc.data() + (static_cast<size_t>(y) * tw + x) * 4;
            canvas::Rgba8 p{static_cast<uint8_t>(std::lround(d[0] / n)), static_cast<uint8_t>(std::lround(d[1] / n)),
                            static_cast<uint8_t>(std::lround(d[2] / n)), static_cast<uint8_t>(std::lround(d[3] / n))};
            argb[static_cast<size_t>(y) * tw + x] = p.toArgb();
        }
    }
    outW = tw;
    outH = th;
    return true;
}

DocumentStats CanvasDocument::stats() const {
    std::lock_guard<std::mutex> lock(mutex_);
    DocumentStats s;
    s.open = open_;
    s.width = composite_.width();
    s.height = composite_.height();
    s.generation = generation_.load(std::memory_order_relaxed);
    s.revision = revision_.load(std::memory_order_relaxed);
    s.layerCount = layers_.size();
    for (const auto& l : layers_) s.layerTiles += l->store.tilesAllocated();
    s.compositeTiles = composite_.tilesAllocated();
    s.tilesAllocated = s.layerTiles;  // content
    s.bytesAllocated = static_cast<uint64_t>(s.layerTiles + s.compositeTiles) * RasterStore::kTileBytes;  // memory
    s.lastCompositeMs = lastCompositeMs_;
    s.maxCompositeMs = maxCompositeMs_;
    s.compositedPixels = compositedPixels_;
    s.brush = brush_.stats();
    return s;
}

canvas::Rgba8 CanvasDocument::pixel(int32_t x, int32_t y) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!open_) return {};
    updateLocked();
    // A composite tile that does not exist shows the backdrop (the renderer's paper).
    if (backdrop_.opaque && x >= 0 && y >= 0 && x < static_cast<int32_t>(composite_.width()) &&
        y < static_cast<int32_t>(composite_.height()) && !composite_.tileAllocated(x / RasterStore::kTile, y / RasterStore::kTile)) {
        auto c8 = [](float v) { return static_cast<uint8_t>(std::lround(v * 255.0f)); };
        return {c8(backdrop_.r), c8(backdrop_.g), c8(backdrop_.b), 255};
    }
    return composite_.pixel(x, y);
}

canvas::Rgba8 CanvasDocument::layerPixel(uint32_t id, int32_t x, int32_t y) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (open_) updateLocked();
    const int32_t i = indexOf(id);
    return i >= 0 ? layers_[static_cast<size_t>(i)]->store.pixel(x, y) : canvas::Rgba8{};
}

}  // namespace pe
