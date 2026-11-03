#include "canvas/Compositor.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace pe::canvas {

namespace {
constexpr int32_t kTile = RasterStore::kTile;
constexpr float kInv255 = 1.0f / 255.0f;

inline float clamp01(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }
// 8-bit -> 0..1 without a multiply per channel.
struct U8ToFloat {
    float v[256];
    U8ToFloat() {
        for (int i = 0; i < 256; ++i) v[i] = static_cast<float>(i) / 255.0f;
    }
};
const U8ToFloat kU8;
inline uint8_t to8(float v) { return static_cast<uint8_t>(clamp01(v) * 255.0f + 0.5f); }
}  // namespace

const char* blendModeName(BlendMode m) {
    switch (m) {
        case BlendMode::Normal: return "normal";
        case BlendMode::Multiply: return "multiply";
        case BlendMode::Screen: return "screen";
        case BlendMode::Overlay: return "overlay";
        case BlendMode::Darken: return "darken";
        case BlendMode::Lighten: return "lighten";
        case BlendMode::Add: return "add";
        case BlendMode::Difference: return "difference";
    }
    return "normal";
}

Backdrop Backdrop::fromArgb(uint32_t argb, bool transparent) {
    Backdrop b;
    b.opaque = !transparent;
    b.r = static_cast<float>((argb >> 16) & 0xFF) * kInv255;
    b.g = static_cast<float>((argb >> 8) & 0xFF) * kInv255;
    b.b = static_cast<float>(argb & 0xFF) * kInv255;
    return b;
}

float blendChannel(BlendMode m, float cb, float cs) {
    switch (m) {
        case BlendMode::Normal: return cs;
        case BlendMode::Multiply: return cb * cs;
        case BlendMode::Screen: return cb + cs - cb * cs;
        case BlendMode::Overlay:  // HardLight with the layers swapped
            return cb <= 0.5f ? 2.0f * cs * cb : 1.0f - 2.0f * (1.0f - cs) * (1.0f - cb);
        case BlendMode::Darken: return std::min(cb, cs);
        case BlendMode::Lighten: return std::max(cb, cs);
        case BlendMode::Add: return std::min(1.0f, cb + cs);
        case BlendMode::Difference: return std::fabs(cb - cs);
    }
    return cs;
}

void blendPixel(float* d, const float* s, float opacity, BlendMode m) {
    const float as = s[3] * opacity;
    if (!(as > 0.0f)) return;
    const float ab = d[3];
    const float ao = as + ab * (1.0f - as);
    if (m == BlendMode::Normal || !(ab > 0.0f)) {
        // Source-over (every mode reduces to it over a transparent backdrop).
        const float k = 1.0f - as;
        d[0] = s[0] * opacity + d[0] * k;
        d[1] = s[1] * opacity + d[1] * k;
        d[2] = s[2] * opacity + d[2] * k;
        d[3] = ao;
        return;
    }
    const float invAs = 1.0f / as, invAb = 1.0f / ab;
    for (int c = 0; c < 3; ++c) {
        const float cs = s[c] * opacity;
        const float Cs = clamp01(cs * invAs), Cb = clamp01(d[c] * invAb);
        const float co = cs * (1.0f - ab) + d[c] * (1.0f - as) + as * ab * blendChannel(m, Cb, Cs);
        d[c] = std::clamp(co, 0.0f, ao);
    }
    d[3] = ao;
}

void Compositor::compositeTile(const LayerView* layers, size_t count, const RectI& dirty, const Backdrop& bd,
                               RasterStore& out, bool keepEmpty) {
    const int32_t tx = dirty.x / kTile, ty = dirty.y / kTile;
    // A composite tile that does not exist yet is computed whole (its other
    // pixels are backdrop, not "transparent").
    const RectI part = out.tileAllocated(tx, ty) ? dirty : out.tileRect(tx, ty);
    // Layers with content in this tile (and a visible opacity).
    const uint8_t* src[256];
    float opacity[256];
    BlendMode mode[256];
    size_t n = 0;
    for (size_t i = 0; i < count && n < 256; ++i) {
        if (!layers[i].store || !(layers[i].opacity > 0.0f)) continue;
        const uint8_t* t = layers[i].store->tile(tx, ty);
        if (!t) continue;
        src[n] = t;
        opacity[n] = std::min(1.0f, layers[i].opacity);
        mode[n] = layers[i].mode;
        n++;
    }
    if (n == 0 && !keepEmpty) {
        // Nothing but the backdrop: the renderer's paper is exactly that.
        if (out.tileAllocated(tx, ty)) {
            out.releaseTile(tx, ty);
            out.markChanged(out.tileRect(tx, ty));
        }
        return;
    }
    uint8_t* dst = out.tileForWrite(tx, ty);
    if (!dst) return;
    float acc[kTile * 4];
    const int32_t x0 = part.x - tx * kTile, w = part.w;
    for (int32_t y = part.y; y < part.bottom(); ++y) {
        const size_t rowOff = static_cast<size_t>(y - ty * kTile) * RasterStore::kTileStride + static_cast<size_t>(x0) * 4;
        // Backdrop.
        if (bd.opaque) {
            for (int32_t i = 0; i < w; ++i) {
                acc[i * 4 + 0] = bd.r;
                acc[i * 4 + 1] = bd.g;
                acc[i * 4 + 2] = bd.b;
                acc[i * 4 + 3] = 1.0f;
            }
        } else {
            std::memset(acc, 0, sizeof(float) * 4 * static_cast<size_t>(w));
        }
        for (size_t l = 0; l < n; ++l) {
            const uint8_t* p = src[l] + rowOff;
            const float o = opacity[l];
            if (mode[l] == BlendMode::Normal) {
                const float* f = kU8.v;
                if (o >= 1.0f) {
                    for (int32_t i = 0; i < w; ++i, p += 4) {
                        const uint8_t a8 = p[3];
                        if (a8 == 0) continue;
                        float* d = acc + i * 4;
                        if (a8 == 255) {  // opaque: replaces the backdrop
                            d[0] = f[p[0]];
                            d[1] = f[p[1]];
                            d[2] = f[p[2]];
                            d[3] = 1.0f;
                            continue;
                        }
                        const float k = 1.0f - f[a8];
                        d[0] = f[p[0]] + d[0] * k;
                        d[1] = f[p[1]] + d[1] * k;
                        d[2] = f[p[2]] + d[2] * k;
                        d[3] = f[a8] + d[3] * k;
                    }
                } else {
                    for (int32_t i = 0; i < w; ++i, p += 4) {
                        if (p[3] == 0) continue;
                        const float as = f[p[3]] * o, k = 1.0f - as;
                        float* d = acc + i * 4;
                        d[0] = f[p[0]] * o + d[0] * k;
                        d[1] = f[p[1]] * o + d[1] * k;
                        d[2] = f[p[2]] * o + d[2] * k;
                        d[3] = as + d[3] * k;
                    }
                }
            } else {
                for (int32_t i = 0; i < w; ++i, p += 4) {
                    if (p[3] == 0) continue;
                    const float s[4] = {kU8.v[p[0]], kU8.v[p[1]], kU8.v[p[2]], kU8.v[p[3]]};
                    blendPixel(acc + i * 4, s, o, mode[l]);
                }
            }
        }
        uint8_t* o8 = dst + rowOff;
        for (int32_t i = 0; i < w; ++i, o8 += 4) {
            const float* d = acc + i * 4;
            const float a = clamp01(d[3]);
            o8[3] = to8(a);
            // Keep premultiplied colour <= alpha after rounding.
            o8[0] = std::min(to8(d[0]), o8[3]);
            o8[1] = std::min(to8(d[1]), o8[3]);
            o8[2] = std::min(to8(d[2]), o8[3]);
        }
    }
    out.markChanged(part);
}

void Compositor::composite(const LayerView* layers, size_t count, const RectI& rect, const Backdrop& backdrop,
                           RasterStore& out) {
    int32_t tx0, ty0, tx1, ty1;
    if (!out.tileRange(rect, tx0, ty0, tx1, ty1)) return;
    const RectI c = rect.intersect(out.canvasRect());
    for (int32_t ty = ty0; ty <= ty1; ++ty)
        for (int32_t tx = tx0; tx <= tx1; ++tx) {
            const RectI part = c.intersect(out.tileRect(tx, ty));
            if (!part.empty()) compositeTile(layers, count, part, backdrop, out, false);
        }
}

void Compositor::flattenInto(const LayerView* layers, size_t count, const RectI& rect, RasterStore& out) {
    Backdrop none;
    none.opaque = false;
    composite(layers, count, rect, none, out);
}

}  // namespace pe::canvas
