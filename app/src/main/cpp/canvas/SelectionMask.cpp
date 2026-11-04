#include "canvas/SelectionMask.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <unordered_map>

namespace pe::canvas {

namespace {
constexpr int32_t kT = SelectionMask::kTile;
constexpr size_t kTB = SelectionMask::kTileBytes;
inline uint8_t clamp255(float v) { return static_cast<uint8_t>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f)); }
}  // namespace

void SelectionMask::reset(uint32_t width, uint32_t height) {
    width_ = width;
    height_ = height;
    cols_ = static_cast<int32_t>((width + kT - 1) / kT);
    rows_ = static_cast<int32_t>((height + kT - 1) / kT);
    tiles_.clear();
    tiles_.resize(static_cast<size_t>(cols_) * static_cast<size_t>(rows_));
    active_ = false;
    version_++;
}

SelectionMask::TileState SelectionMask::state(int32_t tx, int32_t ty) const {
    if (tx < 0 || ty < 0 || tx >= cols_ || ty >= rows_) return TileState::Empty;
    return tiles_[index(tx, ty)].state;
}

const uint8_t* SelectionMask::tile(int32_t tx, int32_t ty) const {
    if (tx < 0 || ty < 0 || tx >= cols_ || ty >= rows_) return nullptr;
    const Tile& t = tiles_[index(tx, ty)];
    return t.state == TileState::Partial ? t.px.get() : nullptr;
}

uint8_t SelectionMask::at(int32_t x, int32_t y) const {
    if (!active_) return 255;
    if (x < 0 || y < 0 || x >= static_cast<int32_t>(width_) || y >= static_cast<int32_t>(height_)) return 0;
    const Tile& t = tiles_[index(x / kT, y / kT)];
    switch (t.state) {
        case TileState::Empty: return 0;
        case TileState::Full: return 255;
        case TileState::Partial: return t.px[static_cast<size_t>(y % kT) * kT + static_cast<size_t>(x % kT)];
    }
    return 0;
}

uint8_t* SelectionMask::writable(Tile& t) {
    if (t.state != TileState::Partial) {
        const uint8_t v = t.state == TileState::Full ? 255 : 0;
        t.px.reset(new uint8_t[kTB]);
        std::memset(t.px.get(), v, kTB);
        t.state = TileState::Partial;
    }
    return t.px.get();
}

void SelectionMask::normalize(Tile& t) {
    if (t.state != TileState::Partial) return;
    const uint8_t first = t.px[0];
    if (first != 0 && first != 255) return;
    for (size_t i = 0; i < kTB; ++i)
        if (t.px[i] != first) return;
    t.px.reset();
    t.state = first == 255 ? TileState::Full : TileState::Empty;
}

bool SelectionMask::anySelected() const {
    for (const auto& t : tiles_)
        if (t.state != TileState::Empty) return true;   // a partial tile always holds some coverage
    return false;
}

void SelectionMask::clear() {
    for (auto& t : tiles_) {
        t.px.reset();
        t.state = TileState::Empty;
    }
    active_ = false;
    version_++;
}

void SelectionMask::selectAll() {
    for (auto& t : tiles_) {
        t.px.reset();
        t.state = TileState::Full;
    }
    active_ = !tiles_.empty();
    version_++;
}

void SelectionMask::invert() {
    if (!active_) return;
    for (auto& t : tiles_) {
        if (t.state == TileState::Empty) t.state = TileState::Full;
        else if (t.state == TileState::Full) t.state = TileState::Empty;
        else {
            for (size_t i = 0; i < kTB; ++i) t.px[i] = static_cast<uint8_t>(255 - t.px[i]);
            normalize(t);
        }
    }
    active_ = anySelected();
    version_++;
}

void SelectionMask::combine(const SelectionMask& shape, Mode mode) {
    if (shape.cols_ != cols_ || shape.rows_ != rows_) return;
    if (mode == Mode::Replace || (mode == Mode::Add && !active_)) {
        for (size_t i = 0; i < tiles_.size(); ++i) {
            const Tile& s = shape.tiles_[i];
            Tile& t = tiles_[i];
            t.state = s.state;
            if (s.state == TileState::Partial) {
                t.px.reset(new uint8_t[kTB]);
                std::memcpy(t.px.get(), s.px.get(), kTB);
            } else {
                t.px.reset();
            }
        }
        active_ = anySelected();
        version_++;
        return;
    }
    if (!active_) return;   // subtract from / intersect with nothing selected: still nothing
    for (size_t i = 0; i < tiles_.size(); ++i) {
        const Tile& s = shape.tiles_[i];
        Tile& t = tiles_[i];
        switch (mode) {
            case Mode::Add:
                if (s.state == TileState::Empty || t.state == TileState::Full) break;
                if (s.state == TileState::Full) { t.px.reset(); t.state = TileState::Full; break; }
                if (t.state == TileState::Empty) { t.px.reset(new uint8_t[kTB]); std::memcpy(t.px.get(), s.px.get(), kTB); t.state = TileState::Partial; break; }
                for (size_t k = 0; k < kTB; ++k) t.px[k] = std::max(t.px[k], s.px[k]);
                normalize(t);
                break;
            case Mode::Subtract:
                if (s.state == TileState::Empty || t.state == TileState::Empty) break;
                if (s.state == TileState::Full) { t.px.reset(); t.state = TileState::Empty; break; }
                {
                    uint8_t* p = writable(t);
                    for (size_t k = 0; k < kTB; ++k) p[k] = std::min<uint8_t>(p[k], static_cast<uint8_t>(255 - s.px[k]));
                }
                normalize(t);
                break;
            case Mode::Intersect:
                if (t.state == TileState::Empty || s.state == TileState::Full) break;
                if (s.state == TileState::Empty) { t.px.reset(); t.state = TileState::Empty; break; }
                if (t.state == TileState::Full) { t.px.reset(new uint8_t[kTB]); std::memcpy(t.px.get(), s.px.get(), kTB); t.state = TileState::Partial; break; }
                for (size_t k = 0; k < kTB; ++k) t.px[k] = std::min(t.px[k], s.px[k]);
                normalize(t);
                break;
            case Mode::Replace: break;
        }
    }
    active_ = anySelected();
    version_++;
}

void SelectionMask::translate(int32_t dx, int32_t dy) {
    if (!active_ || (dx == 0 && dy == 0)) return;
    std::vector<Tile> out(tiles_.size());
    for (int32_t ty = 0; ty < rows_; ++ty)
        for (int32_t tx = 0; tx < cols_; ++tx) {
            // Source rectangle of this destination tile.
            const int32_t sx0 = tx * kT - dx, sy0 = ty * kT - dy;
            const int32_t stx0 = static_cast<int32_t>(std::floor(sx0 / static_cast<double>(kT)));
            const int32_t sty0 = static_cast<int32_t>(std::floor(sy0 / static_cast<double>(kT)));
            // Fast path: all overlapped source tiles uniform with one state, inside the canvas.
            bool uniform = true;
            TileState st = state(stx0, sty0);
            for (int32_t j = 0; j < 2 && uniform; ++j)
                for (int32_t i = 0; i < 2 && uniform; ++i) {
                    const bool aligned = (i == 1 && sx0 % kT == 0) || (j == 1 && sy0 % kT == 0);
                    if (aligned) continue;
                    const int32_t ax = stx0 + i, ay = sty0 + j;
                    const bool inside = ax >= 0 && ay >= 0 && ax < cols_ && ay < rows_;
                    const TileState s2 = inside ? tiles_[index(ax, ay)].state : TileState::Empty;
                    if (s2 == TileState::Partial || s2 != st) uniform = false;
                }
            Tile& d = out[index(tx, ty)];
            if (uniform && st != TileState::Partial) {
                d.state = st;
                continue;
            }
            d.px.reset(new uint8_t[kTB]);
            d.state = TileState::Partial;
            for (int32_t y = 0; y < kT; ++y)
                for (int32_t x = 0; x < kT; ++x) d.px[static_cast<size_t>(y) * kT + x] = at(sx0 + x, sy0 + y);
            normalize(d);
        }
    // Pixels beyond the canvas edge are not part of the canvas: clear them in edge tiles.
    tiles_ = std::move(out);
    for (int32_t ty = 0; ty < rows_; ++ty)
        for (int32_t tx = 0; tx < cols_; ++tx) {
            const int32_t vw = std::min<int32_t>(kT, static_cast<int32_t>(width_) - tx * kT);
            const int32_t vh = std::min<int32_t>(kT, static_cast<int32_t>(height_) - ty * kT);
            if (vw == kT && vh == kT) continue;
            Tile& t = tiles_[index(tx, ty)];
            if (t.state == TileState::Empty) continue;
            uint8_t* p = writable(t);
            for (int32_t y = 0; y < kT; ++y)
                for (int32_t x = 0; x < kT; ++x)
                    if (x >= vw || y >= vh) p[static_cast<size_t>(y) * kT + x] = 0;
            normalize(t);
        }
    active_ = anySelected();
    version_++;
}

void SelectionMask::setTile(int32_t tx, int32_t ty, const uint8_t* px) {
    if (tx < 0 || ty < 0 || tx >= cols_ || ty >= rows_) return;
    Tile& t = tiles_[index(tx, ty)];
    if (!px) {
        t.px.reset();
        t.state = TileState::Empty;
    } else {
        uint8_t* p = writable(t);
        std::memcpy(p, px, kTB);
        normalize(t);
    }
    version_++;
}

// ------------------------------------------------------------------ polygon
void SelectionMask::setPolygon(const float* xy, size_t points, bool antialias) {
    for (auto& t : tiles_) {
        t.px.reset();
        t.state = TileState::Empty;
    }
    version_++;
    active_ = false;
    if (!xy || points < 3) return;
    struct Edge {
        float x0, y0, x1, y1, dxdy;
        int dir;
    };
    std::vector<Edge> edges;
    edges.reserve(points);
    float minY = 1e30f, maxY = -1e30f, minX = 1e30f, maxX = -1e30f;
    for (size_t i = 0; i < points; ++i) {
        const float ax = xy[2 * i], ay = xy[2 * i + 1];
        const size_t j = (i + 1) % points;
        const float bx = xy[2 * j], by = xy[2 * j + 1];
        if (!std::isfinite(ax) || !std::isfinite(ay) || !std::isfinite(bx) || !std::isfinite(by)) continue;
        minX = std::min(minX, ax); maxX = std::max(maxX, ax);
        minY = std::min(minY, ay); maxY = std::max(maxY, ay);
        if (ay == by) continue;
        Edge e;
        if (ay < by) { e = {ax, ay, bx, by, 0, +1}; }
        else { e = {bx, by, ax, ay, 0, -1}; }
        e.dxdy = (e.x1 - e.x0) / (e.y1 - e.y0);
        edges.push_back(e);
    }
    if (edges.empty()) return;
    const int32_t W = static_cast<int32_t>(width_), H = static_cast<int32_t>(height_);
    const int32_t by0 = std::clamp(static_cast<int32_t>(std::floor(minY)), 0, H);
    const int32_t by1 = std::clamp(static_cast<int32_t>(std::ceil(maxY)), 0, H);
    const int32_t bx0 = std::clamp(static_cast<int32_t>(std::floor(minX)), 0, W);
    const int32_t bx1 = std::clamp(static_cast<int32_t>(std::ceil(maxX)), 0, W);
    if (by1 <= by0 || bx1 <= bx0) return;
    std::sort(edges.begin(), edges.end(), [](const Edge& a, const Edge& b) { return a.y0 < b.y0; });
    const int32_t n = bx1 - bx0;
    std::vector<float> cov(static_cast<size_t>(n) + 2), dlt(static_cast<size_t>(n) + 2);
    std::vector<const Edge*> active;
    std::vector<std::pair<float, int>> xs;
    size_t next = 0;
    const int S = antialias ? 4 : 1;
    const float w = 1.0f / static_cast<float>(S);
    auto addSpan = [&](float xa, float xb) {
        xa = std::max(xa, static_cast<float>(bx0));
        xb = std::min(xb, static_cast<float>(bx1));
        if (xb <= xa) return;
        if (!antialias) {
            // Pixel centres inside the span.
            const int32_t ia = static_cast<int32_t>(std::ceil(xa - 0.5f)), ib = static_cast<int32_t>(std::ceil(xb - 0.5f));
            if (ib > ia) { dlt[static_cast<size_t>(ia - bx0)] += 1.0f; dlt[static_cast<size_t>(ib - bx0)] -= 1.0f; }
            return;
        }
        const int32_t ia = static_cast<int32_t>(std::floor(xa)), ib = static_cast<int32_t>(std::floor(xb));
        if (ia == ib) {
            cov[static_cast<size_t>(ia - bx0)] += (xb - xa) * w;
            return;
        }
        cov[static_cast<size_t>(ia - bx0)] += (static_cast<float>(ia + 1) - xa) * w;
        dlt[static_cast<size_t>(ia + 1 - bx0)] += w;
        dlt[static_cast<size_t>(ib - bx0)] -= w;
        if (ib < bx1) cov[static_cast<size_t>(ib - bx0)] += (xb - static_cast<float>(ib)) * w;
    };
    // Rows are written band by band (one tile row). A tile whose rows are all
    // uniform (0 or 255) is never allocated: only tiles crossed by an edge
    // hold pixels, so a big shape costs memory along its outline only.
    enum class Run : uint8_t { None, Zero, Full, Mixed };
    std::vector<Run> runs(static_cast<size_t>(cols_));
    std::vector<uint8_t> row(static_cast<size_t>(n));
    const int32_t tx0 = bx0 / kT, tx1 = (bx1 - 1) / kT;
    auto vwOf = [&](int32_t tx) { return std::min<int32_t>(kT, W - tx * kT); };
    // Allocates tile tx of band ty, filling rows [r0, r1) with the uniform value seen so far.
    auto materialize = [&](int32_t tx, int32_t ty, int32_t r0, int32_t r1) -> uint8_t* {
        Tile& t = tiles_[index(tx, ty)];
        t.px.reset(new uint8_t[kTB]);
        std::memset(t.px.get(), 0, kTB);
        t.state = TileState::Partial;
        if (runs[static_cast<size_t>(tx)] == Run::Full)
            for (int32_t r = r0; r < r1; ++r) std::memset(t.px.get() + static_cast<size_t>(r - ty * kT) * kT, 255, static_cast<size_t>(vwOf(tx)));
        runs[static_cast<size_t>(tx)] = Run::Mixed;
        return t.px.get();
    };
    for (int32_t ty = by0 / kT; ty <= (by1 - 1) / kT; ++ty) {
        const int32_t bandTop = ty * kT, bandEnd = std::min(bandTop + kT, H);
        const int32_t r0 = std::max(bandTop, by0), r1 = std::min(bandEnd, by1);
        for (int32_t tx = tx0; tx <= tx1; ++tx) runs[static_cast<size_t>(tx)] = r0 > bandTop ? Run::Zero : Run::None;
        for (int32_t y = r0; y < r1; ++y) {
            std::fill(cov.begin(), cov.end(), 0.0f);
            std::fill(dlt.begin(), dlt.end(), 0.0f);
            bool any = false;
            for (int s = 0; s < S; ++s) {
                const float ys = static_cast<float>(y) + (static_cast<float>(s) + 0.5f) * w;
                while (next < edges.size() && edges[next].y0 <= ys) active.push_back(&edges[next++]);
                active.erase(std::remove_if(active.begin(), active.end(), [&](const Edge* e) { return e->y1 <= ys; }), active.end());
                xs.clear();
                for (const Edge* e : active)
                    if (e->y0 <= ys && ys < e->y1) xs.push_back({e->x0 + (ys - e->y0) * e->dxdy, e->dir});
                if (xs.size() < 2) continue;
                std::sort(xs.begin(), xs.end());
                int winding = 0;
                float start = 0;
                for (const auto& [x, dir] : xs) {
                    const int before = winding;
                    winding += dir;
                    if (before == 0 && winding != 0) start = x;
                    else if (before != 0 && winding == 0) { addSpan(start, x); any = true; }
                }
            }
            if (any) {
                float acc = 0;
                for (int32_t i = 0; i < n; ++i) {
                    acc += dlt[static_cast<size_t>(i)];
                    row[static_cast<size_t>(i)] = clamp255(cov[static_cast<size_t>(i)] + acc);
                }
            } else {
                std::fill(row.begin(), row.end(), 0);
            }
            const size_t rowOff = static_cast<size_t>(y - bandTop) * kT;
            for (int32_t tx = tx0; tx <= tx1; ++tx) {
                // This tile's part of the row: [a, b) inside the shape's bounds; the rest of the tile is 0.
                const int32_t tl = tx * kT, tr = tl + vwOf(tx);
                const int32_t a = std::max(tl, bx0), b = std::min(tr, bx1);
                const uint8_t* src = row.data() + (a - bx0);
                const size_t len = static_cast<size_t>(b - a);
                const uint8_t first = src[0];
                bool uniform = (first == 0 || (first == 255 && a == tl && b == tr));
                for (size_t k = 1; k < len && uniform; ++k) uniform = src[k] == first;
                Run& run = runs[static_cast<size_t>(tx)];
                const Run kind = uniform ? (first == 0 ? Run::Zero : Run::Full) : Run::Mixed;
                if (run == Run::None) run = kind;
                if (kind == run && run != Run::Mixed) continue;  // still uniform: nothing stored
                uint8_t* px = run == Run::Mixed && tiles_[index(tx, ty)].state == TileState::Partial
                                  ? tiles_[index(tx, ty)].px.get()
                                  : materialize(tx, ty, r0, y);
                std::memcpy(px + rowOff + static_cast<size_t>(a - tl), src, len);
            }
        }
        // End of the band: uniform tiles become EMPTY / FULL; a full run followed by
        // rows below the shape (zeros) is stored.
        for (int32_t tx = tx0; tx <= tx1; ++tx) {
            Run& run = runs[static_cast<size_t>(tx)];
            if (run == Run::Full) {
                if (r1 < bandEnd) materialize(tx, ty, r0, r1);
                else tiles_[index(tx, ty)].state = TileState::Full;
            }
            if (run == Run::Mixed) normalize(tiles_[index(tx, ty)]);
        }
    }
    active_ = anySelected();
}

// ------------------------------------------------------------------ queries
bool SelectionMask::bounds(RectI& out) const {
    if (!active_) return false;
    int32_t x0 = INT32_MAX, y0 = INT32_MAX, x1 = INT32_MIN, y1 = INT32_MIN;
    for (int32_t ty = 0; ty < rows_; ++ty)
        for (int32_t tx = 0; tx < cols_; ++tx) {
            const Tile& t = tiles_[index(tx, ty)];
            if (t.state == TileState::Empty) continue;
            const int32_t vw = std::min<int32_t>(kT, static_cast<int32_t>(width_) - tx * kT);
            const int32_t vh = std::min<int32_t>(kT, static_cast<int32_t>(height_) - ty * kT);
            if (t.state == TileState::Full) {
                x0 = std::min(x0, tx * kT); y0 = std::min(y0, ty * kT);
                x1 = std::max(x1, tx * kT + vw); y1 = std::max(y1, ty * kT + vh);
                continue;
            }
            for (int32_t y = 0; y < vh; ++y)
                for (int32_t x = 0; x < vw; ++x)
                    if (t.px[static_cast<size_t>(y) * kT + x]) {
                        x0 = std::min(x0, tx * kT + x); y0 = std::min(y0, ty * kT + y);
                        x1 = std::max(x1, tx * kT + x + 1); y1 = std::max(y1, ty * kT + y + 1);
                    }
        }
    if (x1 <= x0 || y1 <= y0) return false;
    out = {x0, y0, x1 - x0, y1 - y0};
    return true;
}

uint64_t SelectionMask::bytes() const {
    uint64_t b = 0;
    for (const auto& t : tiles_)
        if (t.state == TileState::Partial) b += kTB;
    return b;
}

uint64_t SelectionMask::checksum() const {
    uint64_t h = 1469598103934665603ull;
    auto mix = [&](uint64_t v) { h ^= v; h *= 1099511628211ull; };
    mix(active_ ? 1 : 0);
    if (!active_) return h;
    for (int32_t ty = 0; ty < rows_; ++ty)
        for (int32_t tx = 0; tx < cols_; ++tx) {
            const Tile& t = tiles_[index(tx, ty)];
            mix(static_cast<uint64_t>(t.state));
            if (t.state == TileState::Partial)
                for (size_t i = 0; i < kTB; i += 8) {
                    uint64_t w;
                    std::memcpy(&w, t.px.get() + i, 8);
                    mix(w);
                }
        }
    return h;
}

double SelectionMask::area() const {
    if (!active_) return 0;
    double a = 0;
    for (int32_t ty = 0; ty < rows_; ++ty)
        for (int32_t tx = 0; tx < cols_; ++tx) {
            const Tile& t = tiles_[index(tx, ty)];
            const int32_t vw = std::min<int32_t>(kT, static_cast<int32_t>(width_) - tx * kT);
            const int32_t vh = std::min<int32_t>(kT, static_cast<int32_t>(height_) - ty * kT);
            if (t.state == TileState::Full) a += static_cast<double>(vw) * vh;
            else if (t.state == TileState::Partial) {
                uint64_t s = 0;
                for (int32_t y = 0; y < vh; ++y)
                    for (int32_t x = 0; x < vw; ++x) s += t.px[static_cast<size_t>(y) * kT + x];
                a += static_cast<double>(s) / 255.0;
            }
        }
    return a;
}

void SelectionMask::contours(std::vector<float>& out) const {
    out.clear();
    out.push_back(0.0f);
    if (!active_) return;
    const int32_t W = static_cast<int32_t>(width_), H = static_cast<int32_t>(height_);
    auto sel = [&](int32_t x, int32_t y) { return at(x, y) >= 128; };
    // Directed pixel-edge segments, selected region on the right (clockwise in y-down space).
    std::unordered_multimap<uint64_t, uint64_t> next;
    const uint64_t stride = static_cast<uint64_t>(W) + 1;
    auto key = [&](int32_t x, int32_t y) { return static_cast<uint64_t>(y) * stride + static_cast<uint64_t>(x); };
    auto visit = [&](int32_t x, int32_t y) {
        if (!sel(x, y)) return;
        if (!sel(x, y - 1)) next.emplace(key(x, y), key(x + 1, y));
        if (!sel(x + 1, y)) next.emplace(key(x + 1, y), key(x + 1, y + 1));
        if (!sel(x, y + 1)) next.emplace(key(x + 1, y + 1), key(x, y + 1));
        if (!sel(x - 1, y)) next.emplace(key(x, y + 1), key(x, y));
    };
    for (int32_t ty = 0; ty < rows_; ++ty)
        for (int32_t tx = 0; tx < cols_; ++tx) {
            const TileState s = state(tx, ty);
            const int32_t vw = std::min<int32_t>(kT, W - tx * kT), vh = std::min<int32_t>(kT, H - ty * kT);
            if (s == TileState::Empty) continue;
            if (s == TileState::Full) {
                // Only its border pixels can have an edge, and only next to a tile that is not full
                // (or the canvas edge). Each border pixel is visited once.
                const bool edge = tx == 0 || ty == 0 || tx * kT + vw >= W || ty * kT + vh >= H ||
                                  state(tx - 1, ty) != TileState::Full || state(tx + 1, ty) != TileState::Full ||
                                  state(tx, ty - 1) != TileState::Full || state(tx, ty + 1) != TileState::Full;
                if (!edge) continue;
                for (int32_t y = 0; y < vh; ++y) {
                    if (y == 0 || y == vh - 1) {
                        for (int32_t x = 0; x < vw; ++x) visit(tx * kT + x, ty * kT + y);
                    } else {
                        visit(tx * kT, ty * kT + y);
                        if (vw > 1) visit(tx * kT + vw - 1, ty * kT + y);
                    }
                }
                continue;
            }
            for (int32_t y = 0; y < vh; ++y)
                for (int32_t x = 0; x < vw; ++x) visit(tx * kT + x, ty * kT + y);
        }
    // Follow the segments into closed loops, merging straight runs.
    uint32_t loops = 0;
    while (!next.empty()) {
        auto it = next.begin();
        const uint64_t start = it->first;
        uint64_t cur = start;
        const size_t countAt = out.size();
        out.push_back(0.0f);
        uint32_t n = 0;
        int32_t pdx = 0, pdy = 0;
        bool first = true;
        while (true) {
            auto e = next.find(cur);
            if (e == next.end()) break;
            const uint64_t to = e->second;
            next.erase(e);
            const int32_t cx = static_cast<int32_t>(cur % stride), cy = static_cast<int32_t>(cur / stride);
            const int32_t nx = static_cast<int32_t>(to % stride), ny = static_cast<int32_t>(to / stride);
            const int32_t dx = nx - cx, dy = ny - cy;
            if (first || dx != pdx || dy != pdy) {
                out.push_back(static_cast<float>(cx));
                out.push_back(static_cast<float>(cy));
                n++;
            }
            first = false;
            pdx = dx; pdy = dy;
            cur = to;
            if (cur == start) break;
        }
        out[countAt] = static_cast<float>(n);
        loops++;
    }
    out[0] = static_cast<float>(loops);
}

void SelectionMask::preview(uint32_t maxSide, uint32_t& outW, uint32_t& outH, std::vector<uint8_t>& out) const {
    const uint32_t longest = std::max(width_, height_);
    const double s = longest > maxSide && maxSide > 0 ? static_cast<double>(maxSide) / longest : 1.0;
    outW = std::max<uint32_t>(1, static_cast<uint32_t>(std::lround(width_ * s)));
    outH = std::max<uint32_t>(1, static_cast<uint32_t>(std::lround(height_ * s)));
    out.assign(static_cast<size_t>(outW) * outH, 0);
    if (!active_) return;
    for (uint32_t y = 0; y < outH; ++y)
        for (uint32_t x = 0; x < outW; ++x) {
            const int32_t sx = static_cast<int32_t>((x + 0.5) / s), sy = static_cast<int32_t>((y + 0.5) / s);
            out[static_cast<size_t>(y) * outW + x] = static_cast<uint8_t>(255 - at(sx, sy));
        }
}

// ------------------------------------------------------------------ compact copy
void SelectionMask::encode(std::vector<uint8_t>& out) const {
    out.clear();
    auto put32 = [&](uint32_t v) { for (int i = 0; i < 4; ++i) out.push_back(static_cast<uint8_t>(v >> (8 * i))); };
    put32(width_);
    put32(height_);
    out.push_back(active_ ? 1 : 0);
    for (const auto& t : tiles_) {
        out.push_back(static_cast<uint8_t>(t.state));
        if (t.state != TileState::Partial) continue;
        size_t i = 0;
        while (i < kTB) {
            const uint8_t v = t.px[i];
            size_t run = 1;
            while (i + run < kTB && run < 255 && t.px[i + run] == v) run++;
            out.push_back(static_cast<uint8_t>(run));
            out.push_back(v);
            i += run;
        }
    }
}

bool SelectionMask::decode(const uint8_t* data, size_t size) {
    if (!data || size < 9) return false;
    auto get32 = [&](size_t o) { uint32_t v = 0; for (int i = 0; i < 4; ++i) v |= static_cast<uint32_t>(data[o + i]) << (8 * i); return v; };
    if (get32(0) != width_ || get32(4) != height_) return false;
    size_t off = 8;
    const bool act = data[off++] != 0;
    std::vector<Tile> tiles(tiles_.size());
    for (auto& t : tiles) {
        if (off >= size) return false;
        t.state = static_cast<TileState>(data[off++]);
        if (t.state != TileState::Partial) continue;
        t.px.reset(new uint8_t[kTB]);
        size_t i = 0;
        while (i < kTB) {
            if (off + 2 > size) return false;
            const size_t run = data[off];
            const uint8_t v = data[off + 1];
            off += 2;
            if (run == 0 || i + run > kTB) return false;
            std::memset(t.px.get() + i, v, run);
            i += run;
        }
    }
    tiles_ = std::move(tiles);
    active_ = act;
    version_++;
    return true;
}

void SelectionMask::shapePolygon(bool ellipse, double x0, double y0, double x1, double y1, std::vector<float>& out) {
    out.clear();
    double l = std::min(x0, x1), r = std::max(x0, x1), t = std::min(y0, y1), b = std::max(y0, y1);
    if (!ellipse) {
        // Whole pixels: a rectangle selection has crisp edges.
        l = std::round(l); r = std::round(r); t = std::round(t); b = std::round(b);
        const float p[8] = {static_cast<float>(l), static_cast<float>(t), static_cast<float>(r), static_cast<float>(t),
                            static_cast<float>(r), static_cast<float>(b), static_cast<float>(l), static_cast<float>(b)};
        out.assign(p, p + 8);
        return;
    }
    const double cx = (l + r) * 0.5, cy = (t + b) * 0.5, rx = (r - l) * 0.5, ry = (b - t) * 0.5;
    // Segments so the chord error stays under ~0.1 px.
    const double rmax = std::max(rx, ry);
    const int n = std::clamp(static_cast<int>(std::ceil(3.14159265358979 / std::acos(std::max(-1.0, 1.0 - 0.1 / std::max(rmax, 0.1))))), 16, 2048);
    out.reserve(static_cast<size_t>(n) * 2);
    for (int i = 0; i < n; ++i) {
        const double a = 2.0 * 3.14159265358979 * i / n;
        out.push_back(static_cast<float>(cx + rx * std::cos(a)));
        out.push_back(static_cast<float>(cy + ry * std::sin(a)));
    }
}

}  // namespace pe::canvas
