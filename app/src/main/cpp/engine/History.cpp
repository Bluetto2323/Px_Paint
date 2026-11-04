#include "engine/History.h"

#include <algorithm>
#include <cstring>

#include "core/Time.h"

namespace pe {

using canvas::RasterStore;
using canvas::RectI;

namespace {
constexpr int32_t kTile = RasterStore::kTile;

inline uint32_t wordAt(const uint8_t* tile, int32_t lx, int32_t ly) {
    if (!tile) return 0;
    uint32_t w;
    std::memcpy(&w, tile + (static_cast<size_t>(ly) * kTile + lx) * 4, 4);
    return w;
}
}  // namespace

const char* historyKindName(HistoryKind k) {
    switch (k) {
        case HistoryKind::Stroke: return "stroke";
        case HistoryKind::Erase: return "erase";
        case HistoryKind::Clear: return "clear";
        case HistoryKind::Fill: return "fill";
        case HistoryKind::AddLayer: return "add_layer";
        case HistoryKind::DeleteLayer: return "delete_layer";
        case HistoryKind::DuplicateLayer: return "duplicate_layer";
        case HistoryKind::MoveLayer: return "move_layer";
        case HistoryKind::RenameLayer: return "rename_layer";
        case HistoryKind::Visibility: return "visibility";
        case HistoryKind::Lock: return "lock";
        case HistoryKind::Opacity: return "opacity";
        case HistoryKind::Blend: return "blend";
        case HistoryKind::MergeDown: return "merge_down";
        case HistoryKind::Flatten: return "flatten";
        case HistoryKind::Selection: return "selection";
        case HistoryKind::ClearSelected: return "clear_selected";
        case HistoryKind::Cut: return "cut";
        case HistoryKind::Paste: return "paste";
        case HistoryKind::DuplicateSelection: return "duplicate_selection";
        case HistoryKind::MoveSelection: return "move_selection";
        case HistoryKind::Gradient: return "gradient";
        case HistoryKind::Smudge: return "smudge";
    }
    return "?";
}

// ----------------------------------------------------------------- group
uint64_t GroupCommand::bytes() const {
    uint64_t b = sizeof(*this);
    for (const auto& p : parts_) b += p->bytes();
    return b;
}

bool GroupCommand::undo(HistoryTarget& t) {
    for (auto it = parts_.rbegin(); it != parts_.rend(); ++it)
        if (!(*it)->undo(t)) return false;
    return true;
}

bool GroupCommand::redo(HistoryTarget& t) {
    for (auto& p : parts_)
        if (!p->redo(t)) return false;
    return true;
}

// ----------------------------------------------------------------- patch
uint64_t PixelPatch::hashRect(const uint8_t* tile, const RectI& rect, int32_t tx, int32_t ty) {
    uint64_t h = 1469598103934665603ull;  // FNV-1a, 64 bit, over 32-bit words
    for (int32_t y = rect.y; y < rect.bottom(); ++y)
        for (int32_t x = rect.x; x < rect.right(); ++x) {
            h ^= wordAt(tile, x - tx * kTile, y - ty * kTile);
            h *= 1099511628211ull;
        }
    return h;
}

void PixelPatch::addTile(int32_t tx, int32_t ty, const RectI& rect, const uint8_t* before, const uint8_t* after) {
    if (rect.empty()) return;
    TileDelta d;
    d.tx = tx;
    d.ty = ty;
    d.rect = rect;
    d.existedBefore = before != nullptr;
    d.existedAfter = after != nullptr;
    d.hashBefore = hashRect(before, rect, tx, ty);
    d.hashAfter = hashRect(after, rect, tx, ty);
    // XOR of before and after, row by row, as runs: zeros count, literal count, literals.
    uint32_t zeros = 0;
    std::vector<uint32_t> lit;
    auto flush = [&] {
        d.rle.push_back(zeros);
        d.rle.push_back(static_cast<uint32_t>(lit.size()));
        d.rle.insert(d.rle.end(), lit.begin(), lit.end());
        zeros = 0;
        lit.clear();
    };
    bool any = false;
    for (int32_t y = rect.y; y < rect.bottom(); ++y)
        for (int32_t x = rect.x; x < rect.right(); ++x) {
            const uint32_t v = wordAt(before, x - tx * kTile, y - ty * kTile) ^ wordAt(after, x - tx * kTile, y - ty * kTile);
            if (v == 0) {
                if (!lit.empty()) flush();
                zeros++;
            } else {
                lit.push_back(v);
                any = true;
            }
        }
    if (!any && d.existedBefore == d.existedAfter) return;  // nothing changed in this tile
    if (zeros || !lit.empty()) flush();
    d.rle.shrink_to_fit();
    bytes_ += d.bytes();
    tiles_.push_back(std::move(d));
}

bool PixelPatch::apply(RasterStore& store, bool undo, RectI* changed) const {
    // Verify the whole patch first: nothing is modified unless every tile is
    // exactly in the state this direction starts from.
    for (const auto& d : tiles_) {
        const uint64_t expect = undo ? d.hashAfter : d.hashBefore;
        if (PixelPatch::hashRect(store.tile(d.tx, d.ty), d.rect, d.tx, d.ty) != expect) return false;
    }
    for (const auto& d : tiles_) {
        uint8_t* px = store.tileForWrite(d.tx, d.ty);
        if (!px) return false;  // invalid tile: cannot happen for a verified patch
        int32_t x = d.rect.x, y = d.rect.y;
        auto advance = [&] {
            if (++x >= d.rect.right()) {
                x = d.rect.x;
                ++y;
            }
        };
        size_t i = 0;
        while (i + 1 < d.rle.size()) {
            const uint32_t zeros = d.rle[i], n = d.rle[i + 1];
            i += 2;
            for (uint32_t k = 0; k < zeros; ++k) advance();
            for (uint32_t k = 0; k < n && i < d.rle.size(); ++k, ++i) {
                uint8_t* p = px + (static_cast<size_t>(y - d.ty * kTile) * kTile + (x - d.tx * kTile)) * 4;
                uint32_t w;
                std::memcpy(&w, p, 4);
                w ^= d.rle[i];
                std::memcpy(p, &w, 4);
                advance();
            }
        }
        store.markChanged(d.rect);
        // A tile that did not exist in the target state goes away again (only
        // if nothing is left in it: content outside the rectangle is never lost).
        if (!(undo ? d.existedBefore : d.existedAfter) && store.tileEmpty(d.tx, d.ty)) store.releaseTile(d.tx, d.ty);
        if (changed) {
            if (changed->empty()) *changed = d.rect;
            else {
                const int32_t l = std::min(changed->x, d.rect.x), t = std::min(changed->y, d.rect.y);
                const int32_t r = std::max(changed->right(), d.rect.right()), b = std::max(changed->bottom(), d.rect.bottom());
                *changed = {l, t, r - l, b - t};
            }
        }
    }
    return true;
}

// -------------------------------------------------------------- commands
bool PixelCommand::apply(HistoryTarget& t, bool undo) {
    LayerData* l = t.findLayer(layer_);
    if (!l) return false;
    RectI changed;
    if (!patch_.apply(l->store, undo, &changed)) return false;
    t.layerPixelsChanged(layer_, changed);
    return true;
}

namespace {
uint64_t layerBytes(const std::unique_ptr<LayerData>& l) { return l ? l->store.bytesAllocated() + sizeof(LayerData) : 0; }
}  // namespace

uint64_t InsertLayerCommand::bytes() const { return sizeof(*this) + layerBytes(kept_); }

bool InsertLayerCommand::undo(HistoryTarget& t) {
    kept_ = t.removeLayer(id_);
    if (!kept_) return false;
    t.setActiveLayer(activeBefore_);
    return true;
}

bool InsertLayerCommand::redo(HistoryTarget& t) {
    if (!kept_) return false;
    t.insertLayer(index_, std::move(kept_));
    t.setActiveLayer(id_);
    return true;
}

uint64_t DeleteLayerCommand::bytes() const { return sizeof(*this) + layerBytes(kept_); }

bool DeleteLayerCommand::undo(HistoryTarget& t) {
    if (!kept_) return false;
    t.insertLayer(index_, std::move(kept_));  // the same layer: id, properties, tiles
    t.setActiveLayer(activeBefore_);
    return true;
}

bool DeleteLayerCommand::redo(HistoryTarget& t) {
    kept_ = t.removeLayer(id_);
    if (!kept_) return false;
    t.setActiveLayer(activeAfter_);
    return true;
}

bool MoveLayerCommand::undo(HistoryTarget& t) {
    if (t.layerIndex(id_) != to_) return false;
    t.moveLayerTo(id_, from_);
    return true;
}

bool MoveLayerCommand::redo(HistoryTarget& t) {
    if (t.layerIndex(id_) != from_) return false;
    t.moveLayerTo(id_, to_);
    return true;
}

bool PropertyCommand::set(HistoryTarget& t, const LayerInfo& v) {
    LayerData* l = t.findLayer(v.id);
    if (!l) return false;
    l->info.name = v.name;
    l->info.visible = v.visible;
    l->info.locked = v.locked;
    l->info.opacity = v.opacity;
    l->info.blend = v.blend;
    t.layerPropertiesChanged(v.id);
    return true;
}

bool PropertyCommand::absorb(const HistoryCommand& next) {
    // A continuous opacity drag on one layer is one step.
    if (kind_ != HistoryKind::Opacity || next.kind() != HistoryKind::Opacity) return false;
    const auto& n = static_cast<const PropertyCommand&>(next);
    if (n.after_.id != after_.id) return false;
    after_ = n.after_;
    return true;
}

uint64_t MergeDownCommand::bytes() const { return sizeof(*this) + layerBytes(upper_) + otherStore_.bytesAllocated(); }

bool MergeDownCommand::undo(HistoryTarget& t) {
    LayerData* lower = t.findLayer(lowerId_);
    if (!lower || !upper_) return false;
    upperId_ = upper_->info.id;
    std::swap(lower->store, otherStore_);  // original pixels back; merged ones kept for redo
    lower->info.opacity = lowerOpacity_;
    lower->store.markEverythingUnsaved();
    t.insertLayer(upperIndex_, std::move(upper_));
    t.stackChanged();
    t.setActiveLayer(activeBefore_);
    return true;
}

bool MergeDownCommand::redo(HistoryTarget& t) {
    LayerData* lower = t.findLayer(lowerId_);
    if (!lower) return false;
    upper_ = t.removeLayer(upperId_);
    if (!upper_) return false;
    std::swap(lower->store, otherStore_);
    lower->info.opacity = 1.0f;
    lower->store.markEverythingUnsaved();
    t.stackChanged();
    t.setActiveLayer(activeAfter_);
    return true;
}

uint64_t FlattenCommand::bytes() const {
    uint64_t b = sizeof(*this) + layerBytes(result_);
    for (const auto& l : before_) b += layerBytes(l);
    return b;
}

bool FlattenCommand::undo(HistoryTarget& t) {
    if (before_.empty()) return false;
    result_ = t.removeLayer(resultId_);
    if (!result_) return false;
    for (size_t i = 0; i < before_.size(); ++i) t.insertLayer(static_cast<int32_t>(i), std::move(before_[i]));
    before_.clear();
    t.stackChanged();
    t.setActiveLayer(activeBefore_);
    return true;
}

bool FlattenCommand::redo(HistoryTarget& t) {
    if (!result_ || !before_.empty()) return false;
    // Undo restored exactly these layers (later steps were undone first).
    for (uint32_t id : idsBefore_)
        if (!t.findLayer(id)) return false;
    for (uint32_t id : idsBefore_) before_.push_back(t.removeLayer(id));
    t.insertLayer(0, std::move(result_));
    t.stackChanged();
    t.setActiveLayer(resultId_);
    return true;
}

// --------------------------------------------------------------- history
void History::push(std::unique_ptr<HistoryCommand> cmd) {
    if (!cmd) return;
    const bool canAbsorb = redo_.empty() && !undo_.empty();
    redo_.clear();  // a new operation after undo: the undone steps are gone
    if (canAbsorb && undo_.back()->absorb(*cmd)) {
        trim();
        return;
    }
    undo_.push_back(std::move(cmd));
    trim();
}

void History::trim() {
    auto total = [&] {
        uint64_t b = 0;
        for (const auto& c : undo_) b += c->bytes();
        for (const auto& c : redo_) b += c->bytes();
        return b;
    };
    bytes_ = total();
    // The newest step always stays: the budget only ever drops old steps.
    while (undo_.size() > 1 && (undo_.size() > maxSteps_ || bytes_ > budget_)) {
        bytes_ -= undo_.front()->bytes();
        undo_.pop_front();
        dropped_++;
    }
    publish();
}

void History::publish() {
    counts_.store((static_cast<uint64_t>(undo_.size()) << 32) | static_cast<uint64_t>(redo_.size()), std::memory_order_release);
}

int History::undo(HistoryTarget& t) {
    if (undo_.empty()) return 0;
    const int64_t t0 = nowNs();
    std::unique_ptr<HistoryCommand> c = std::move(undo_.back());
    undo_.pop_back();
    if (!c->undo(t)) {
        refused_++;
        clear();
        return -1;
    }
    redo_.push_back(std::move(c));
    trim();
    lastUndoMs_ = static_cast<double>(nowNs() - t0) / 1e6;
    return 1;
}

int History::redo(HistoryTarget& t) {
    if (redo_.empty()) return 0;
    const int64_t t0 = nowNs();
    std::unique_ptr<HistoryCommand> c = std::move(redo_.back());
    redo_.pop_back();
    if (!c->redo(t)) {
        refused_++;
        clear();
        return -1;
    }
    undo_.push_back(std::move(c));
    trim();
    lastRedoMs_ = static_cast<double>(nowNs() - t0) / 1e6;
    return 1;
}

void History::clear() {
    undo_.clear();
    redo_.clear();
    bytes_ = 0;
    publish();
}

void History::setBudget(uint64_t bytes, size_t maxSteps) {
    if (bytes > 0) budget_ = bytes;
    if (maxSteps > 0) maxSteps_ = maxSteps;
    trim();
}

std::vector<HistoryEntry> History::entries() const {
    std::vector<HistoryEntry> out;
    for (const auto& c : undo_) out.push_back({c->kind(), c->label(), false});
    for (auto it = redo_.rbegin(); it != redo_.rend(); ++it) out.push_back({(*it)->kind(), (*it)->label(), true});
    return out;
}

HistoryStats History::stats() const {
    HistoryStats s;
    s.undoSteps = undo_.size();
    s.redoSteps = redo_.size();
    s.bytes = bytes_;
    s.budget = budget_;
    s.maxSteps = maxSteps_;
    s.dropped = dropped_;
    s.refused = refused_;
    s.lastUndoMs = lastUndoMs_;
    s.lastRedoMs = lastRedoMs_;
    s.lastRecordMs = lastRecordMs_;
    return s;
}

}  // namespace pe
