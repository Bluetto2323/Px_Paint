#include "brush/BrushTextures.h"

#include <algorithm>
#include <cmath>
#include <mutex>

namespace pe::brush {

namespace {

constexpr double kPi = 3.14159265358979323846;

uint32_t hash3(uint32_t x, uint32_t y, uint32_t s) {
    uint32_t h = x * 0x8da6b343u ^ y * 0xd8163841u ^ s * 0xcb1ab31fu;
    h ^= h >> 16;
    h *= 0x7feb352du;
    h ^= h >> 15;
    h *= 0x846ca68bu;
    h ^= h >> 16;
    return h;
}
double rand01(uint32_t x, uint32_t y, uint32_t s) { return (hash3(x, y, s) >> 8) * (1.0 / 16777216.0); }

double smooth5(double t) { return t * t * t * (t * (t * 6.0 - 15.0) + 10.0); }
double smoothstep(double e0, double e1, double x) {
    const double t = std::clamp((x - e0) / (e1 - e0), 0.0, 1.0);
    return t * t * (3.0 - 2.0 * t);
}

struct Octave {
    int32_t cell;
    double weight;
};

// Weighted sum of tileable value-noise octaves (lattice cells of `cell` px,
// cell divides n), normalised to [0, 1] over the image.
std::vector<double> fbm(int32_t n, uint32_t seed, std::initializer_list<Octave> octaves) {
    std::vector<double> f(static_cast<size_t>(n) * n, 0.0);
    std::vector<double> lattice, wx(static_cast<size_t>(n));
    std::vector<int32_t> ix(static_cast<size_t>(n));
    uint32_t k = 0;
    for (const Octave& o : octaves) {
        ++k;
        const int32_t period = n / o.cell;
        lattice.resize(static_cast<size_t>(period) * period);
        for (int32_t y = 0; y < period; ++y)
            for (int32_t x = 0; x < period; ++x)
                lattice[static_cast<size_t>(y) * period + x] =
                    rand01(static_cast<uint32_t>(x), static_cast<uint32_t>(y), seed * 31u + k);
        // Pixel centre -> lattice cell and smooth weight, per axis.
        for (int32_t i = 0; i < n; ++i) {
            const double fx = (i + 0.5) / o.cell - 0.5;
            const int32_t i0 = static_cast<int32_t>(std::floor(fx));
            ix[static_cast<size_t>(i)] = i0;
            wx[static_cast<size_t>(i)] = smooth5(fx - i0);
        }
        auto wrap = [period](int32_t v) { return ((v % period) + period) % period; };
        for (int32_t y = 0; y < n; ++y) {
            const int32_t y0 = wrap(ix[static_cast<size_t>(y)]), y1 = wrap(ix[static_cast<size_t>(y)] + 1);
            const double ty = wx[static_cast<size_t>(y)];
            const double* r0 = &lattice[static_cast<size_t>(y0) * period];
            const double* r1 = &lattice[static_cast<size_t>(y1) * period];
            double* out = &f[static_cast<size_t>(y) * n];
            for (int32_t x = 0; x < n; ++x) {
                const int32_t x0 = wrap(ix[static_cast<size_t>(x)]), x1 = wrap(ix[static_cast<size_t>(x)] + 1);
                const double tx = wx[static_cast<size_t>(x)];
                const double a = r0[x0] + (r0[x1] - r0[x0]) * tx;
                const double b = r1[x0] + (r1[x1] - r1[x0]) * tx;
                out[x] += o.weight * (a + (b - a) * ty);
            }
        }
    }
    const auto [mn, mx] = std::minmax_element(f.begin(), f.end());
    const double lo = *mn, span = std::max(*mx - *mn, 1e-9);
    for (double& v : f) v = (v - lo) / span;
    return f;
}

Texture8 make(int32_t n) {
    Texture8 t;
    t.size = n;
    t.mask = static_cast<uint32_t>(n - 1);
    while ((1 << t.shift) < n) t.shift++;
    t.px.assign(static_cast<size_t>(n) * n, 0);
    return t;
}

uint8_t u8(double v) { return static_cast<uint8_t>(std::lround(std::clamp(v, 0.0, 1.0) * 255.0)); }

// ---------------------------------------------------------------- grains
// Values: 1 = paper peak (takes pigment), 0 = valley (stays bare).
Texture8 buildGrain(BrushGrain g) {
    const int32_t n = kGrainSize;
    Texture8 t = make(n);
    switch (g) {
        case BrushGrain::None:
            return Texture8{};
        case BrushGrain::Paper: {
            // Fine drawing paper: small isotropic tooth.
            auto f = fbm(n, 11, {{2, 0.50}, {4, 0.32}, {8, 0.18}});
            for (size_t i = 0; i < f.size(); ++i) t.px[i] = u8(smoothstep(0.25, 0.75, f[i]));
            break;
        }
        case BrushGrain::Rough: {
            // Pastel / charcoal paper: coarser, high-contrast tooth.
            auto f = fbm(n, 23, {{2, 0.36}, {4, 0.34}, {8, 0.20}, {16, 0.10}});
            for (size_t i = 0; i < f.size(); ++i) t.px[i] = u8(smoothstep(0.38, 0.62, f[i]));
            break;
        }
        case BrushGrain::Canvas: {
            // Plain weave, 8 px threads: the thread on top alternates per cell.
            auto f = fbm(n, 37, {{2, 0.5}, {4, 0.5}});
            for (int32_t y = 0; y < n; ++y) {
                for (int32_t x = 0; x < n; ++x) {
                    const int32_t bx = x / 8, by = y / 8;
                    const double pv = std::sin(kPi * ((x % 8) + 0.5) / 8.0);
                    const double ph = std::sin(kPi * ((y % 8) + 0.5) / 8.0);
                    const double thread = ((bx + by) & 1) ? pv : ph;
                    const size_t i = static_cast<size_t>(y) * n + x;
                    t.px[i] = u8(0.12 + 0.88 * std::pow(thread, 0.6) * (0.82 + 0.18 * f[i]));
                }
            }
            break;
        }
        case BrushGrain::Wash: {
            // Watercolour: broad pigment variations plus fine granules.
            auto f = fbm(n, 53, {{64, 0.40}, {32, 0.22}, {8, 0.16}, {2, 0.22}});
            for (size_t i = 0; i < f.size(); ++i) t.px[i] = u8(smoothstep(0.12, 0.88, f[i]));
            break;
        }
    }
    return t;
}

// ------------------------------------------------------------------ tips
// Periodic 1-D noise around the circle (`cells` lattice points).
double angularNoise(double theta, int32_t cells, uint32_t seed) {
    const double f = (theta / (2.0 * kPi) + 0.5) * cells;
    const int32_t i0 = static_cast<int32_t>(std::floor(f));
    const double t = smooth5(f - i0);
    auto at = [&](int32_t i) { return rand01(static_cast<uint32_t>(((i % cells) + cells) % cells), 0, seed); };
    return at(i0) + (at(i0 + 1) - at(i0)) * t;
}

Texture8 buildTip(BrushTip tip) {
    const int32_t n = kTipSize;
    if (tip == BrushTip::Round) return Texture8{};
    Texture8 t = make(n);
    auto coord = [](int32_t i) { return (i + 0.5) / (kTipSize * 0.5) - 1.0; };
    switch (tip) {
        case BrushTip::Round:
            break;
        case BrushTip::Chalk: {
            // Speckled disc, broken edge: the stick catches only some of the tooth.
            auto speck = fbm(n, 71, {{2, 0.45}, {4, 0.35}, {8, 0.20}});
            for (int32_t y = 0; y < n; ++y) {
                for (int32_t x = 0; x < n; ++x) {
                    const double nx = coord(x), ny = coord(y), r = std::hypot(nx, ny);
                    const double edgeR = 0.84 + 0.12 * (angularNoise(std::atan2(ny, nx), 28, 72) - 0.5);
                    const double edge = 1.0 - smoothstep(edgeR - 0.10, edgeR, r);
                    const double s = smoothstep(0.30, 0.62, speck[static_cast<size_t>(y) * n + x]);
                    t.px[static_cast<size_t>(y) * n + x] = u8(edge * (0.25 + 0.75 * s));
                }
            }
            break;
        }
        case BrushTip::Charcoal: {
            // Irregular blob with a dense core and a soft, uneven border.
            auto body = fbm(n, 83, {{8, 0.5}, {16, 0.3}, {4, 0.2}});
            for (int32_t y = 0; y < n; ++y) {
                for (int32_t x = 0; x < n; ++x) {
                    const double nx = coord(x), ny = coord(y), r = std::hypot(nx, ny);
                    const double edgeR = 0.80 + 0.32 * (angularNoise(std::atan2(ny, nx), 11, 84) - 0.5);
                    const double edge = 1.0 - smoothstep(edgeR - 0.32, std::min(edgeR, 0.98), r);
                    const double inner = 0.62 + 0.38 * body[static_cast<size_t>(y) * n + x];
                    t.px[static_cast<size_t>(y) * n + x] = u8(edge * inner);
                }
            }
            break;
        }
        case BrushTip::Bristle: {
            // Bristle marks spread across the stroke (v), short along it (u):
            // dense dabs turn each one into a streak parallel to the stroke.
            struct Hair { double u, v, wu, wv, k; };
            Hair hairs[44];
            for (uint32_t i = 0; i < 44; ++i) {
                const double v = -0.92 + 1.84 * rand01(i, 1, 91);
                const double wv = 0.020 + 0.050 * rand01(i, 2, 91);
                hairs[i] = {0.30 * (rand01(i, 3, 91) - 0.5), v, wv * 3.0, wv, 0.35 + 0.65 * rand01(i, 4, 91)};
            }
            for (int32_t y = 0; y < n; ++y) {
                for (int32_t x = 0; x < n; ++x) {
                    const double nu = coord(x), nv = coord(y);
                    double v = 0.0;
                    for (const Hair& h : hairs) {
                        const double du = (nu - h.u) / h.wu, dv = (nv - h.v) / h.wv;
                        v = std::max(v, h.k * std::exp(-(du * du + dv * dv)));
                    }
                    const double r = std::hypot(nu, nv);
                    // A thin film of paint between the hairs.
                    v = std::max(v, 0.16 * (1.0 - smoothstep(0.55, 0.9, std::abs(nv))) * (1.0 - smoothstep(0.15, 0.4, std::abs(nu))));
                    t.px[static_cast<size_t>(y) * n + x] = u8(v * (1.0 - smoothstep(0.86, 1.0, r)));
                }
            }
            break;
        }
    }
    return t;
}

struct Cache {
    std::once_flag grainOnce[kBrushGrainCount];
    Texture8 grain[kBrushGrainCount];
    std::once_flag tipOnce[kBrushTipCount];
    Texture8 tip[kBrushTipCount];
};
Cache& cache() {
    static Cache c;
    return c;
}

}  // namespace

float Texture8::sample(float u, float v) const {
    u -= 0.5f;
    v -= 0.5f;
    const float fu = std::floor(u), fv = std::floor(v);
    const int32_t x0 = static_cast<int32_t>(fu), y0 = static_cast<int32_t>(fv);
    const float tx = u - fu, ty = v - fv;
    auto at = [&](int32_t x, int32_t y) -> float {
        if (x < 0 || y < 0 || x >= size || y >= size) return 0.0f;
        return px[static_cast<size_t>(y) * size + x];
    };
    const float a = at(x0, y0), b = at(x0 + 1, y0), c = at(x0, y0 + 1), d = at(x0 + 1, y0 + 1);
    return (a + (b - a) * tx) * (1.0f - ty) + (c + (d - c) * tx) * ty;
}

double Texture8::mean() const {
    if (px.empty()) return 0.0;
    double s = 0.0;
    for (uint8_t v : px) s += v;
    return s / (255.0 * static_cast<double>(px.size()));
}

const Texture8& grainTexture(BrushGrain g) {
    const int i = static_cast<int>(g);
    Cache& c = cache();
    if (i < 0 || i >= kBrushGrainCount) return c.grain[0];
    std::call_once(c.grainOnce[i], [&] { c.grain[i] = buildGrain(g); });
    return c.grain[i];
}

const Texture8& tipTexture(BrushTip t) {
    const int i = static_cast<int>(t);
    Cache& c = cache();
    if (i < 0 || i >= kBrushTipCount) return c.tip[0];
    std::call_once(c.tipOnce[i], [&] { c.tip[i] = buildTip(t); });
    return c.tip[i];
}

void prewarmBrushTextures() {
    for (int i = 1; i < kBrushGrainCount; ++i) grainTexture(static_cast<BrushGrain>(i));
    for (int i = 1; i < kBrushTipCount; ++i) tipTexture(static_cast<BrushTip>(i));
}

}  // namespace pe::brush
