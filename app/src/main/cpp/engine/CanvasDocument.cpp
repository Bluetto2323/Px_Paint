#include "engine/CanvasDocument.h"

#include <algorithm>
#include <cmath>
#include <cstring>

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

void CanvasDocument::open(uint32_t width, uint32_t height) {
    std::lock_guard<std::mutex> lock(mutex_);
    brush_.discard();
    store_.reset(width, height);
    open_ = true;
    generation_.fetch_add(1, std::memory_order_acq_rel);
    bumpRevision();
}

void CanvasDocument::close() {
    std::lock_guard<std::mutex> lock(mutex_);
    brush_.discard();
    store_.reset(0, 0);
    open_ = false;
    generation_.fetch_add(1, std::memory_order_acq_rel);
    bumpRevision();
}

void CanvasDocument::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    brush_.discard();
    store_.clear();
    generation_.fetch_add(1, std::memory_order_acq_rel);  // GPU storage rebuilt empty
    bumpRevision();
}

bool CanvasDocument::isOpen() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return open_;
}

void CanvasDocument::fill(const RectI& rect, uint32_t argb) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!open_) return;
    brush_.process(store_);  // keep the order: queued strokes first
    store_.fill(rect, canvas::Rgba8::fromArgb(argb));
    bumpRevision();
}

size_t CanvasDocument::loadLegacy(const std::vector<ContentOp>& ops) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!open_) return 0;
    brush_.discard();
    store_.clear();
    size_t kept = 0;
    for (const auto& op : ops) {
        const RectI r = RectI{op.x, op.y, op.w, op.h}.intersect(store_.canvasRect());
        if (r.empty()) continue;
        store_.fill(r, canvas::Rgba8::fromArgb(op.argb));
        kept++;
    }
    // Imported content is not on disk in the new format yet.
    store_.markAllUnsaved();
    generation_.fetch_add(1, std::memory_order_acq_rel);
    bumpRevision();
    return kept;
}

void CanvasDocument::strokeBegin(const brush::BrushSettings& settings, const brush::StrokePoint& first) {
    brush_.begin(settings, first);
    bumpRevision();
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

bool CanvasDocument::size(uint64_t generation, uint32_t& width, uint32_t& height) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!open_ || generation != generation_.load(std::memory_order_relaxed)) return false;
    width = store_.width();
    height = store_.height();
    return true;
}

size_t CanvasDocument::exportTiles(bool changedOnly, size_t maxTiles, std::vector<uint8_t>& out) {
    std::vector<canvas::TileCoord> list;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!open_) return 0;
        brush_.process(store_);
        if (changedOnly) {
            store_.unsavedTiles(list);
        } else {
            for (int32_t ty = 0; ty < store_.rows(); ++ty)
                for (int32_t tx = 0; tx < store_.columns(); ++tx)
                    if (store_.tileAllocated(tx, ty)) list.push_back({tx, ty});
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
        if (!open_ || generation_.load(std::memory_order_relaxed) != gen) break;
        const bool empty = store_.tileEmpty(t.tx, t.ty);
        putI32(out, t.tx);
        putI32(out, t.ty);
        putI32(out, empty ? kTileEmpty : 0);
        if (!empty) {
            const uint8_t* px = store_.tile(t.tx, t.ty);
            out.insert(out.end(), px, px + RasterStore::kTileBytes);
        }
        store_.markSaved(t.tx, t.ty);
        n++;
    }
    setI32(out, countAt, static_cast<int32_t>(n));
    return n;
}

long CanvasDocument::importTiles(const uint8_t* data, size_t size) {
    if (!data || size < 4) return -1;
    const int32_t count = getI32(data);
    if (count < 0) return -1;
    size_t off = 4;
    long imported = 0;
    std::lock_guard<std::mutex> lock(mutex_);
    if (!open_) return 0;
    for (int32_t i = 0; i < count; ++i) {
        if (off + kRecordHeader > size) return -1;
        const int32_t tx = getI32(data + off), ty = getI32(data + off + 4), flags = getI32(data + off + 8);
        off += kRecordHeader;
        if (flags & kTileEmpty) continue;
        if (off + RasterStore::kTileBytes > size) return -1;
        if (!store_.validTile(tx, ty)) return -1;
        store_.importTile(tx, ty, data + off);
        off += RasterStore::kTileBytes;
        imported++;
    }
    bumpRevision();
    return imported;
}

bool CanvasDocument::thumbnail(uint32_t maxSide, uint32_t& outW, uint32_t& outH, std::vector<uint32_t>& argb) {
    uint32_t w, h;
    int32_t cols, rows;
    uint64_t gen;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!open_ || store_.width() == 0) return false;
        brush_.process(store_);
        w = store_.width();
        h = store_.height();
        cols = store_.columns();
        rows = store_.rows();
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
            const uint8_t* px = store_.tile(tx, ty);
            if (!px) continue;
            const RectI r = store_.tileRect(tx, ty);
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
    s.width = store_.width();
    s.height = store_.height();
    s.generation = generation_.load(std::memory_order_relaxed);
    s.revision = revision_.load(std::memory_order_relaxed);
    s.tilesAllocated = store_.tilesAllocated();
    s.bytesAllocated = store_.bytesAllocated();
    s.brush = brush_.stats();
    return s;
}

canvas::Rgba8 CanvasDocument::pixel(int32_t x, int32_t y) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return store_.pixel(x, y);
}

}  // namespace pe
