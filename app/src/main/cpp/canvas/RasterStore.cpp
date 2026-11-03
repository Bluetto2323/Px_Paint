#include "canvas/RasterStore.h"

#include <algorithm>
#include <cstring>

namespace pe::canvas {

Rgba8 Rgba8::fromArgb(uint32_t argb) {
    const uint32_t a = (argb >> 24) & 0xFF;
    auto pm = [a](uint32_t c) { return static_cast<uint8_t>((c * a + 127) / 255); };
    return {pm((argb >> 16) & 0xFF), pm((argb >> 8) & 0xFF), pm(argb & 0xFF), static_cast<uint8_t>(a)};
}

uint32_t Rgba8::toArgb() const {
    if (a == 0) return 0;
    auto un = [this](uint32_t c) { return std::min<uint32_t>(255, (c * 255 + a / 2) / a); };
    return (static_cast<uint32_t>(a) << 24) | (un(r) << 16) | (un(g) << 8) | un(b);
}

void RasterStore::reset(uint32_t width, uint32_t height) {
    width_ = width;
    height_ = height;
    cols_ = static_cast<int32_t>((width + kTile - 1) / kTile);
    rows_ = static_cast<int32_t>((height + kTile - 1) / kTile);
    tiles_.clear();
    tiles_.shrink_to_fit();
    tiles_.resize(static_cast<size_t>(cols_) * rows_);
    allocated_ = 0;
    gpuDirtyTiles_ = 0;
    version_++;
}

void RasterStore::clear() {
    for (auto& t : tiles_) t = Tile{};
    allocated_ = 0;
    gpuDirtyTiles_ = 0;
    version_++;
}

RectI RasterStore::tileRect(int32_t tx, int32_t ty) const {
    return RectI{tx * kTile, ty * kTile, kTile, kTile}.intersect(canvasRect());
}

bool RasterStore::tileRange(const RectI& r, int32_t& tx0, int32_t& ty0, int32_t& tx1, int32_t& ty1) const {
    const RectI c = r.intersect(canvasRect());
    if (c.empty()) return false;
    tx0 = c.x / kTile;
    ty0 = c.y / kTile;
    tx1 = (c.right() - 1) / kTile;
    ty1 = (c.bottom() - 1) / kTile;
    return true;
}

const uint8_t* RasterStore::tile(int32_t tx, int32_t ty) const {
    return validTile(tx, ty) ? tiles_[index(tx, ty)].px.get() : nullptr;
}

uint8_t* RasterStore::tileForWrite(int32_t tx, int32_t ty) {
    if (!validTile(tx, ty)) return nullptr;
    Tile& t = tiles_[index(tx, ty)];
    if (!t.px) {
        t.px.reset(new uint8_t[kTileBytes]);
        std::memset(t.px.get(), 0, kTileBytes);
        allocated_++;
    }
    return t.px.get();
}

void RasterStore::releaseTile(int32_t tx, int32_t ty) {
    if (!validTile(tx, ty)) return;
    Tile& t = tiles_[index(tx, ty)];
    if (!t.px) return;
    t.px.reset();
    allocated_--;
    version_++;
}

void RasterStore::copyFrom(const RasterStore& other) {
    reset(other.width_, other.height_);
    for (size_t i = 0; i < tiles_.size(); ++i) {
        if (!other.tiles_[i].px) continue;
        tiles_[i].px.reset(new uint8_t[kTileBytes]);
        std::memcpy(tiles_[i].px.get(), other.tiles_[i].px.get(), kTileBytes);
        tiles_[i].unsaved = true;
        allocated_++;
    }
    markAllGpuDirty();
    version_++;
}

void RasterStore::markChanged(const RectI& r) {
    int32_t tx0, ty0, tx1, ty1;
    if (!tileRange(r, tx0, ty0, tx1, ty1)) return;
    const RectI c = r.intersect(canvasRect());
    for (int32_t ty = ty0; ty <= ty1; ++ty) {
        for (int32_t tx = tx0; tx <= tx1; ++tx) {
            Tile& t = tiles_[index(tx, ty)];
            const RectI part = c.intersect(tileRect(tx, ty));
            if (t.gpuDirty.empty()) {
                t.gpuDirty = part;
                gpuDirtyTiles_++;
            } else {
                const int32_t l = std::min(t.gpuDirty.x, part.x), tp = std::min(t.gpuDirty.y, part.y);
                const int32_t rr = std::max(t.gpuDirty.right(), part.right());
                const int32_t b = std::max(t.gpuDirty.bottom(), part.bottom());
                t.gpuDirty = {l, tp, rr - l, b - tp};
            }
            t.unsaved = true;
        }
    }
    version_++;
}

void RasterStore::fill(const RectI& r, Rgba8 color) {
    int32_t tx0, ty0, tx1, ty1;
    if (!tileRange(r, tx0, ty0, tx1, ty1)) return;
    const RectI c = r.intersect(canvasRect());
    for (int32_t ty = ty0; ty <= ty1; ++ty) {
        for (int32_t tx = tx0; tx <= tx1; ++tx) {
            const RectI part = c.intersect(tileRect(tx, ty));
            if (color.a == 0 && !tile(tx, ty)) continue;  // transparent over nothing
            uint8_t* px = tileForWrite(tx, ty);
            for (int32_t y = part.y; y < part.bottom(); ++y) {
                uint8_t* row = px + static_cast<size_t>(y - ty * kTile) * kTileStride;
                for (int32_t x = part.x; x < part.right(); ++x) {
                    uint8_t* p = row + static_cast<size_t>(x - tx * kTile) * 4;
                    p[0] = color.r;
                    p[1] = color.g;
                    p[2] = color.b;
                    p[3] = color.a;
                }
            }
        }
    }
    markChanged(c);
}

Rgba8 RasterStore::pixel(int32_t x, int32_t y) const {
    if (!canvasRect().contains(x, y)) return {};
    const uint8_t* px = tile(x / kTile, y / kTile);
    if (!px) return {};
    const uint8_t* p = px + static_cast<size_t>(y % kTile) * kTileStride + static_cast<size_t>(x % kTile) * 4;
    return {p[0], p[1], p[2], p[3]};
}

void RasterStore::takeGpuDirty(std::vector<RectI>& out) {
    if (gpuDirtyTiles_ == 0) return;
    for (auto& t : tiles_) {
        if (t.gpuDirty.empty()) continue;
        out.push_back(t.gpuDirty);
        t.gpuDirty = {};
    }
    gpuDirtyTiles_ = 0;
}

void RasterStore::markAllGpuDirty() {
    gpuDirtyTiles_ = 0;
    for (int32_t ty = 0; ty < rows_; ++ty) {
        for (int32_t tx = 0; tx < cols_; ++tx) {
            Tile& t = tiles_[index(tx, ty)];
            t.gpuDirty = t.px ? tileRect(tx, ty) : RectI{};
            if (t.px) gpuDirtyTiles_++;
        }
    }
}

void RasterStore::unsavedTiles(std::vector<TileCoord>& out) const {
    for (int32_t ty = 0; ty < rows_; ++ty)
        for (int32_t tx = 0; tx < cols_; ++tx)
            if (tiles_[index(tx, ty)].unsaved) out.push_back({tx, ty});
}

void RasterStore::markSaved(int32_t tx, int32_t ty) {
    if (validTile(tx, ty)) tiles_[index(tx, ty)].unsaved = false;
}

void RasterStore::markAllUnsaved() {
    for (auto& t : tiles_)
        if (t.px) t.unsaved = true;
    version_++;
}

void RasterStore::markEverythingUnsaved() {
    for (auto& t : tiles_) t.unsaved = true;
    version_++;
}

bool RasterStore::tileEmpty(int32_t tx, int32_t ty) const {
    const uint8_t* px = tile(tx, ty);
    if (!px) return true;
    for (size_t i = 3; i < kTileBytes; i += 4)
        if (px[i] != 0) return false;
    return true;
}

bool RasterStore::importTile(int32_t tx, int32_t ty, const uint8_t* rgba) {
    if (!validTile(tx, ty) || !rgba) return false;
    uint8_t* px = tileForWrite(tx, ty);
    std::memcpy(px, rgba, kTileBytes);
    // Pixels beyond the canvas edge stay transparent whatever the file says.
    const RectI r = tileRect(tx, ty);
    const int32_t w = r.w, h = r.h;
    for (int32_t y = 0; y < kTile; ++y) {
        uint8_t* row = px + static_cast<size_t>(y) * kTileStride;
        if (y >= h) {
            std::memset(row, 0, kTileStride);
        } else if (w < kTile) {
            std::memset(row + static_cast<size_t>(w) * 4, 0, static_cast<size_t>(kTile - w) * 4);
        }
    }
    Tile& t = tiles_[index(tx, ty)];
    const bool wasDirty = !t.gpuDirty.empty();
    t.gpuDirty = r;
    if (!wasDirty) gpuDirtyTiles_++;
    t.unsaved = false;
    version_++;
    return true;
}

}  // namespace pe::canvas
