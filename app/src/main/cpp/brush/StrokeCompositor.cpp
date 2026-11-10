#include "brush/StrokeCompositor.h"

#include <algorithm>
#include <array>
#include <limits>
#include <cmath>
#include <cstring>

namespace pe::brush {

using canvas::RasterStore;
using canvas::RectI;

namespace {
constexpr int32_t kTile = RasterStore::kTile;
constexpr size_t kPlane = static_cast<size_t>(kTile) * kTile;

// sRGB <-> linear light (colour mixing happens in linear light: no muddy,
// darkened midpoints between saturated colours).
const float* srgbToLinearTable() {
    static const auto table = [] {
        std::array<float, 256> t{};
        for (int i = 0; i < 256; ++i) {
            const double c = i / 255.0;
            t[static_cast<size_t>(i)] = static_cast<float>(c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4));
        }
        return t;
    }();
    return table.data();
}
// Pigment-like mixing: hue and saturation from the weighted geometric mean of
// the linear reflectances (subtractive: yellow + blue -> green, not grey),
// luminance from the additive mix (no muddy darkening).
struct PaintColour {
    double logc[3] = {0, 0, 0};   // ln(linear channel)
    double lum = 0;               // linear luminance
};
constexpr double kLogFloor = 1e-3;
PaintColour paintFromLinear(const float* c) {
    PaintColour p;
    for (int i = 0; i < 3; ++i) p.logc[i] = std::log(std::max(kLogFloor, static_cast<double>(c[i])));
    p.lum = 0.2126 * c[0] + 0.7152 * c[1] + 0.0722 * c[2];
    return p;
}
void mixInto(PaintColour& p, const PaintColour& q, double t) {
    for (int i = 0; i < 3; ++i) p.logc[i] += (q.logc[i] - p.logc[i]) * t;
    p.lum += (q.lum - p.lum) * t;
}
void paintToLinear(const PaintColour& p, double* out) {
    double y = 0;
    for (int i = 0; i < 3; ++i) out[i] = std::exp(p.logc[i]);
    y = 0.2126 * out[0] + 0.7152 * out[1] + 0.0722 * out[2];
    const double k = y > 1e-6 ? p.lum / y : 0.0;
    for (int i = 0; i < 3; ++i) out[i] = std::min(1.0, out[i] * k);
}
uint8_t linearToSrgb8(double v) {
    v = std::clamp(v, 0.0, 1.0);
    const double c = v <= 0.0031308 ? v * 12.92 : 1.055 * std::pow(v, 1.0 / 2.4) - 0.055;
    return static_cast<uint8_t>(std::lround(std::clamp(c, 0.0, 1.0) * 255.0));
}
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

void StrokeCompositor::begin(RasterStore& store, const BrushSettings& settings, bool hasPressure, bool hasTilt) {
    if (active_) commit(store);
    settings_ = settings.sanitized();
    dyn_.configure(settings_, hasPressure, hasTilt);
    const float colourAlpha = static_cast<float>((settings_.argb >> 24) & 0xFF) / 255.0f;
    paintA_ = settings_.opacity * colourAlpha;
    paintR_ = static_cast<float>((settings_.argb >> 16) & 0xFF);
    paintG_ = static_cast<float>((settings_.argb >> 8) & 0xFF);
    paintB_ = static_cast<float>(settings_.argb & 0xFF);
    classic_ = settings_.classicRound();
    buildup_ = settings_.accumulation == Accumulation::Buildup;
    tipTex_ = settings_.tip != BrushTip::Round ? &tipTexture(settings_.tip) : nullptr;
    grainTex_ = settings_.grain != BrushGrain::None && settings_.grainDepth > 0.0f ? &grainTexture(settings_.grain) : nullptr;
    if (tipTex_ && tipTex_->empty()) tipTex_ = nullptr;
    if (grainTex_ && grainTex_->empty()) grainTex_ = nullptr;
    wetLut_.clear();
    if (settings_.wetEdge > 0.0f) {
        constexpr int n = 1 << kWetLutBits;
        wetLut_.resize(n + 1);
        for (int i = 0; i <= n; ++i)
            wetLut_[static_cast<size_t>(i)] = static_cast<uint16_t>(
                std::lround(std::clamp(wetEdgeTransfer(static_cast<double>(i) / n, settings_.wetEdge), 0.0, 1.0) * 65535.0));
    }
    dabIndex_ = 0;
    taperStep_ = settings_.spacingPx(settings_.diameter);
    taperLen_ = settings_.taper > 0.0f ? settings_.taper * 6.0 * settings_.diameter : 0.0;
    wet_ = settings_.wet != WetMode::None;
    smudge_ = settings_.wet == WetMode::Smudge;
    if (wet_) {
        // Per-dab rates from per-diameter ones: the result does not depend on the spacing.
        const double dia = settings_.diameter;
        const double step = std::clamp(settings_.spacingPx(dia) / dia, 1e-3, 2.0);
        if (smudge_) {
            // Per dab the carried image (the pixels one step back) replaces `strength` of the pixels:
            // the paint travels at `strength` x the finger's speed whatever the spacing, so it is
            // left behind after about strength / (1 - strength) brush diameters (100 %: dragged along).
            wetA_ = settings_.opacity;
            wetPick_ = static_cast<float>(1.0 - std::pow(1.0 - std::max(0.02, static_cast<double>(settings_.pickup)), step));
            // Carry grid: the largest footprint around the dab centre (size jitter, tilt, scatter).
            const double major = dia * 0.5 * (1.0 + settings_.sizeJitter) * (settings_.tiltShape ? 1.0 + 1.5 * settings_.tiltStrength : 1.0);
            const double extent = major * (1.0 + 2.0 * settings_.scatter) + 3.0;
            const int32_t need = 2 * static_cast<int32_t>(std::ceil(extent)) + 1;
            carryCell_ = std::max<int32_t>(1, (need + kMaxCarry - 1) / kMaxCarry);
            carryN_ = (need + carryCell_ - 1) / carryCell_ + 2;
            carryHalf_ = carryN_ / 2;
            carry_.assign(static_cast<size_t>(carryN_) * carryN_ * 4, 0.0f);  // reuses the capacity
            for (size_t i = 3; i < carry_.size(); i += 4) carry_[i] = -1.0f;  // nothing picked up yet
        } else {
            // The stroke keeps its coverage mask exactly as a plain stroke would (same edges,
            // textures, opacity ceiling, buildup, wet edge); each dab deposits, in its own
            // colour, only the opacity it adds to that mask.
            wetA_ = std::clamp(paintA_, 0.0f, 1.0f) * (1.0f - 0.85f * settings_.dilution);
            chargeLen_ = settings_.charge < 1.0f ? dia * (1.5 + 40.0 * settings_.charge * settings_.charge) : 0.0;
            wetDist_ = 0.0;
            wetLast_[0] = wetLast_[1] = std::numeric_limits<double>::quiet_NaN();
            wetPick_ = static_cast<float>(1.0 - std::pow(1.0 - static_cast<double>(settings_.pickup), step));
            const float* lin = srgbToLinearTable();
            brushLin_[0] = lin[static_cast<size_t>(paintR_)];
            brushLin_[1] = lin[static_cast<size_t>(paintG_)];
            brushLin_[2] = lin[static_cast<size_t>(paintB_)];
            const PaintColour b = paintFromLinear(brushLin_);
            for (int i = 0; i < 3; ++i) brushMix_[i] = b.logc[i];
            brushMix_[3] = b.lum;
            paintLoaded_ = false;
        }
    }
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
    st->touched = {};
    if (!settings_.erase) store.tileForWrite(tx, ty);  // paint needs a destination tile
    slot_[idx] = static_cast<int32_t>(tiles_.size());
    tiles_.push_back(std::move(st));
    stats_.strokeTiles = static_cast<uint32_t>(tiles_.size());
    return tiles_.back().get();
}

double StrokeCompositor::wetEdgeTransfer(double m, double e) {
    // Rises fast to a rim at m ~ 0.2 (the stroke's soft border), then eases
    // down to 1 - e/2 in the fully covered interior: pigment collected at
    // the edge, lighter wash inside. e = 0 is the identity.
    auto ss = [](double a, double b, double x) {
        const double t = std::clamp((x - a) / (b - a), 0.0, 1.0);
        return t * t * (3.0 - 2.0 * t);
    };
    m = std::clamp(m, 0.0, 1.0);
    const double w = ss(0.0, 0.2, m) * (1.0 - 0.5 * ss(0.2, 0.9, m));
    return (1.0 - e) * m + e * w;
}

DabGeometry StrokeCompositor::geometryAt(const Dab& d, uint32_t index) const {
    DabGeometry g = dyn_.geometry(d.axes);
    if (taperLen_ > 0.0) {
        const double t = index * taperStep_ / taperLen_;
        if (t < 1.0) {
            const double f = 0.15 + 0.85 * t * t * (3.0 - 2.0 * t);
            g.radius = std::max(0.5, g.radius * f);
            g.major = std::max(g.radius, g.major * f);
        }
    }
    return g;
}

void StrokeCompositor::rasterDab(RasterStore& store, const Dab& d, bool toTail, uint32_t index) {
    if (!std::isfinite(d.x) || !std::isfinite(d.y)) return;
    if (!classic_) {
        rasterGeneric(store, d, toTail, index);
        return;
    }
    const DabGeometry g = geometryAt(d, index);
    if (g.elliptical) {
        rasterEllipse(store, d, g, toTail);
        return;
    }
    DabShape shape;
    shape.radius = g.radius;
    shape.scale = g.scale;
    const RectI box = shape.bounds(d.x, d.y).intersect(store.canvasRect());
    int32_t tx0, ty0, tx1, ty1;
    if (!store.tileRange(box, tx0, ty0, tx1, ty1)) return;
    const double r = g.radius;
    const double inner = r - 0.5;
    const double inner2 = inner > 0 ? inner * inner : -1.0;
    const double outer2 = (r + 0.5) * (r + 0.5);
    const double scale = g.scale * g.flow;  // pressure opacity scales the dab's coverage
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

void StrokeCompositor::rasterEllipse(RasterStore& store, const Dab& d, const DabGeometry& g, bool toTail) {
    // Antialiased ellipse: normalised radius rho = |(u/a, v/b)|; the distance
    // to the edge is approximated by (rho - 1) / |grad rho| (exact on the
    // axes, within a few percent elsewhere), then the same 1 px ramp as the
    // round dab.
    const double a = g.major, b = g.radius;
    const double e = a + 1.0;
    const RectI box = RectI{static_cast<int32_t>(std::floor(d.x - e)), static_cast<int32_t>(std::floor(d.y - e)),
                            static_cast<int32_t>(std::ceil(2 * e)) + 1, static_cast<int32_t>(std::ceil(2 * e)) + 1}
                          .intersect(store.canvasRect());
    int32_t tx0, ty0, tx1, ty1;
    if (!store.tileRange(box, tx0, ty0, tx1, ty1)) return;
    const double c = std::cos(g.angle), sn = std::sin(g.angle);
    const double ia2 = 1.0 / (a * a), ib2 = 1.0 / (b * b);
    const double scale = g.scale * g.flow;
    const uint16_t full = static_cast<uint16_t>(std::lround(65535.0 * scale));
    // Outer limit: 1 px beyond the edge along the minor axis (and more along
    // the major one). Core: the ellipse scaled so it stays >= 0.5 px inside
    // the edge everywhere (min distance of homothetic ellipses is along the
    // minor axis): coverage is exactly 1 there.
    const double outerR2 = (1.0 + 1.0 / b) * (1.0 + 1.0 / b);
    const double k = std::max(0.0, 1.0 - 0.5 / b);
    const double coreR2 = k * k;
    for (int32_t ty = ty0; ty <= ty1; ++ty) {
        for (int32_t tx = tx0; tx <= tx1; ++tx) {
            const RectI part = box.intersect(store.tileRect(tx, ty));
            if (part.empty()) continue;
            StrokeTile* st = tileFor(store, tx, ty, true);
            if (!st) continue;
            uint16_t* plane = toTail ? st->tail.get() : st->mask.get();
            const int32_t ox = tx * kTile, oy = ty * kTile;
            bool touched = false;
            for (int32_t y = part.y; y < part.bottom(); ++y) {
                const double dy = (y + 0.5) - d.y;
                // rho^2(dx) = A dx^2 + B dx + C on this row: solve for the
                // span inside the outer limit and the fully covered core.
                const double A = c * c * ia2 + sn * sn * ib2;
                const double B = 2.0 * dy * c * sn * (ia2 - ib2);
                const double C = dy * dy * (sn * sn * ia2 + c * c * ib2);
                auto span = [&](double r2, double& lo, double& hi) {
                    const double disc = B * B - 4.0 * A * (C - r2);
                    if (disc < 0) return false;
                    const double sq = std::sqrt(disc);
                    lo = (-B - sq) / (2.0 * A);
                    hi = (-B + sq) / (2.0 * A);
                    return true;
                };
                double lo, hi;
                if (!span(outerR2, lo, hi)) continue;
                const int32_t xs = std::max(part.x, static_cast<int32_t>(std::floor(d.x + lo - 0.5)));
                const int32_t xe = std::min(part.right(), static_cast<int32_t>(std::ceil(d.x + hi - 0.5)) + 1);
                if (xs >= xe) continue;
                int32_t cs = xe, ce = xe;
                double clo, chi;
                if (span(coreR2, clo, chi)) {
                    cs = std::max(xs, static_cast<int32_t>(std::ceil(d.x + clo - 0.5)));
                    ce = std::min(xe, static_cast<int32_t>(std::floor(d.x + chi - 0.5)) + 1);
                    if (ce < cs) cs = ce = xe;
                }
                uint16_t* row = plane + static_cast<size_t>(y - oy) * kTile - ox;
                auto ring = [&](int32_t x0, int32_t x1) {
                    for (int32_t x = x0; x < x1; ++x) {
                        const double dx = (x + 0.5) - d.x;
                        const double u = dx * c + dy * sn, v = -dx * sn + dy * c;
                        const double q = u * u * ia2 + v * v * ib2;  // rho^2
                        if (q >= outerR2) continue;
                        const double rho = std::sqrt(q);
                        double cov = 1.0;
                        if (rho >= 0.5) {
                            const double gu = u * ia2, gv = v * ib2;
                            const double grad = std::sqrt(gu * gu + gv * gv) / rho;
                            cov = std::clamp(0.5 - (rho - 1.0) / grad, 0.0, 1.0);
                        }
                        if (cov <= 0.0) continue;
                        const uint16_t val = static_cast<uint16_t>(cov * scale * 65535.0 + 0.5);
                        uint16_t& m = row[x];
                        if (val > m) m = val;
                    }
                };
                ring(xs, cs);
                for (int32_t x = cs; x < ce; ++x) row[x] = std::max(row[x], full);
                ring(ce, xe);
                touched = true;
            }
            if (touched) {
                grow(st->dirty, part);
                if (toTail) grow(st->tailRect, part);
            }
        }
    }
    stats_.dabs++;
}

namespace {
// Uniform [0, 1) from (stroke seed, dab index, channel): the same dab always
// gets the same jitter, whatever the batching or tail redraws.
inline double jitterRand(uint32_t seed, uint32_t index, uint32_t k) {
    uint32_t h = seed ^ (index * 0x9E3779B1u) ^ (k * 0x85EBCA77u);
    h ^= h >> 16;
    h *= 0x7feb352du;
    h ^= h >> 15;
    h *= 0x846ca68bu;
    h ^= h >> 16;
    return (h >> 8) * (1.0 / 16777216.0);
}
constexpr double kPiD = 3.14159265358979323846;
}  // namespace

void StrokeCompositor::rasterGeneric(RasterStore& store, const Dab& d, bool toTail, uint32_t index) {
    const BrushSettings& s = settings_;
    const DabGeometry g = geometryAt(d, index);
    // Semi-axes: major along `angle`, minor squashed by the roundness.
    double a = g.major, b = g.radius * s.roundness;
    double angle;
    if (g.elliptical) angle = g.angle + (s.roundness < 1.0f ? s.angle : 0.0);
    else if (s.angleFollowsStroke && std::isfinite(d.dir)) angle = static_cast<double>(d.dir) + s.angle;
    else angle = s.angle;
    double cov = g.scale * g.flow * s.flow;
    double cx = d.x, cy = d.y;
    if (s.sizeJitter > 0.0f) {
        const double f = std::max(0.05, 1.0 + s.sizeJitter * (2.0 * jitterRand(s.seed, index, 1) - 1.0));
        a *= f;
        b *= f;
    }
    if (s.angleJitter > 0.0f) angle += s.angleJitter * kPiD * (2.0 * jitterRand(s.seed, index, 2) - 1.0);
    if (s.scatter > 0.0f) {
        const double rr = s.scatter * 2.0 * g.major * std::sqrt(jitterRand(s.seed, index, 3));
        const double th = 2.0 * kPiD * jitterRand(s.seed, index, 4);
        cx += rr * std::cos(th);
        cy += rr * std::sin(th);
    }
    if (s.flowJitter > 0.0f) cov *= 1.0 - s.flowJitter * jitterRand(s.seed, index, 5);
    // Thinner than a pixel: keep a 1 px footprint with proportional coverage.
    if (b < 0.5) {
        cov *= b / 0.5;
        b = 0.5;
    }
    a = std::max(a, b);
    if (cov <= 0.0) return;
    // Stamps need a few pixels to show their shape; below that a soft round dab.
    const Texture8* tip = (tipTex_ && b >= 1.5) ? tipTex_ : nullptr;
    const double hard = tip ? 1.0 : (tipTex_ ? 0.5 : static_cast<double>(s.hardness));
    const double w = (1.0 - hard) * b;          // falloff width, px
    const bool soft = w >= 0.01;
    double grainA = 1.0, grainB = 0.0;
    if (grainTex_) {
        double depth = s.grainDepth;
        // Pressure presses the pigment into the valleys (stylus strokes only).
        if (s.grainPressure > 0.0f && dyn_.hasPressure()) depth *= 1.0 - s.grainPressure * dyn_.response(d.axes);
        grainA = 1.0 - depth;
        grainB = depth / 255.0;
    }
    const bool grain = grainTex_ && grainB > 0.0;

    const double e = a + 1.5;
    const RectI box = RectI{static_cast<int32_t>(std::floor(cx - e)), static_cast<int32_t>(std::floor(cy - e)),
                            static_cast<int32_t>(std::ceil(2 * e)) + 1, static_cast<int32_t>(std::ceil(2 * e)) + 1}
                          .intersect(store.canvasRect());
    int32_t tx0, ty0, tx1, ty1;
    if (!store.tileRange(box, tx0, ty0, tx1, ty1)) return;
    const double c = std::cos(angle), sn = std::sin(angle);
    const double ia2 = 1.0 / (a * a), ib2 = 1.0 / (b * b);
    const bool circle = std::abs(a - b) < 1e-9;
    const double outerR2 = (1.0 + 1.0 / b) * (1.0 + 1.0 / b);
    // Core of exact full coverage (no texture involved): one fill per row.
    double coreR2 = -1.0;
    if (!tip && !grain) {
        const double k = 1.0 - (w + 0.5) / b;
        if (k > 0.0) coreR2 = k * k;
    }
    const uint16_t full = static_cast<uint16_t>(std::lround(65535.0 * std::min(cov, 1.0)));
    const float tipHalf = tip ? 0.5f * static_cast<float>(tip->size) : 0.0f;
    const double ia = 1.0 / a, ib = 1.0 / b;
    const double A = c * c * ia2 + sn * sn * ib2;
    const double invW1 = 1.0 / (w + 1.0);
    const bool buildup = buildup_;
    auto write = [buildup](uint16_t& m, uint32_t v) {
        if (buildup) m = static_cast<uint16_t>(m + (v * (65535u - m) + 32767u) / 65535u);
        else if (v > m) m = static_cast<uint16_t>(v);
    };
    for (int32_t ty = ty0; ty <= ty1; ++ty) {
        for (int32_t tx = tx0; tx <= tx1; ++tx) {
            const RectI part = box.intersect(store.tileRect(tx, ty));
            if (part.empty()) continue;
            StrokeTile* st = tileFor(store, tx, ty, true);
            if (!st) continue;
            uint16_t* plane = toTail ? st->tail.get() : st->mask.get();
            const int32_t ox = tx * kTile, oy = ty * kTile;
            bool touched = false;
            for (int32_t y = part.y; y < part.bottom(); ++y) {
                const double dy = (y + 0.5) - cy;
                const double B = 2.0 * dy * c * sn * (ia2 - ib2);
                const double C = dy * dy * (sn * sn * ia2 + c * c * ib2);
                auto span = [&](double r2, double& lo, double& hi) {
                    const double disc = B * B - 4.0 * A * (C - r2);
                    if (disc < 0) return false;
                    const double sq = std::sqrt(disc);
                    lo = (-B - sq) / (2.0 * A);
                    hi = (-B + sq) / (2.0 * A);
                    return true;
                };
                double lo, hi;
                if (!span(outerR2, lo, hi)) continue;
                const int32_t xs = std::max(part.x, static_cast<int32_t>(std::floor(cx + lo - 0.5)));
                const int32_t xe = std::min(part.right(), static_cast<int32_t>(std::ceil(cx + hi - 0.5)) + 1);
                if (xs >= xe) continue;
                int32_t cs = xe, ce = xe;
                double clo, chi;
                if (coreR2 > 0.0 && span(coreR2, clo, chi)) {
                    cs = std::max(xs, static_cast<int32_t>(std::ceil(cx + clo - 0.5)));
                    ce = std::min(xe, static_cast<int32_t>(std::floor(cx + chi - 0.5)) + 1);
                    if (ce < cs) cs = ce = xe;
                }
                uint16_t* row = plane + static_cast<size_t>(y - oy) * kTile - ox;
                auto ring = [&](int32_t x0, int32_t x1) {
                    for (int32_t x = x0; x < x1; ++x) {
                        const double dx = (x + 0.5) - cx;
                        const double u = dx * c + dy * sn, v = -dx * sn + dy * c;
                        double k;
                        if (tip) {
                            const float tu = static_cast<float>((u * ia + 1.0) * tipHalf);
                            const float tv = static_cast<float>((v * ib + 1.0) * tipHalf);
                            k = tip->sample(tu, tv) * (1.0 / 255.0);
                        } else {
                            const double q = u * u * ia2 + v * v * ib2;  // rho^2
                            if (q >= outerR2) continue;
                            const double rho = std::sqrt(q);
                            double dist;  // px inside the edge
                            if (circle) {
                                dist = b - rho * b;
                            } else if (rho > 1e-9) {
                                const double gu = u * ia2, gv = v * ib2;
                                dist = (1.0 - rho) * rho / std::sqrt(gu * gu + gv * gv);
                            } else {
                                dist = b;
                            }
                            const double t = std::clamp((dist + 0.5) * invW1, 0.0, 1.0);
                            k = soft ? t * t * (3.0 - 2.0 * t) : t;
                        }
                        if (grain) k *= grainA + grainB * grainTex_->wrap(x, y);
                        const uint32_t val = static_cast<uint32_t>(k * cov * 65535.0 + 0.5);
                        if (val == 0) continue;
                        write(row[x], std::min<uint32_t>(val, 65535u));
                    }
                };
                ring(xs, cs);
                for (int32_t x = cs; x < ce; ++x) write(row[x], full);
                ring(ce, xe);
                touched = true;
            }
            if (touched) {
                grow(st->dirty, part);
                if (toTail) grow(st->tailRect, part);
            }
        }
    }
    stats_.dabs++;
}

void StrokeCompositor::addDabs(RasterStore& store, const Dab* dabs, size_t count) {
    if (!active_) return;
    if (wet_) {
        for (size_t i = 0; i < count; ++i) {
            if (!std::isfinite(dabs[i].x) || !std::isfinite(dabs[i].y)) continue;
            rasterDab(store, dabs[i], true, dabIndex_++);   // this dab's coverage -> scratch plane
            applyWet(store, dabs[i]);
        }
        return;
    }
    for (size_t i = 0; i < count; ++i) rasterDab(store, dabs[i], false, dabIndex_++);
}

void StrokeCompositor::applyWet(RasterStore& store, const Dab& d) {
    constexpr float kInv16 = 1.0f / 65535.0f;
    const bool clipped = clip_ && clip_->active();
    uint8_t paint[4] = {0, 0, 0, 255};
    float fade = 1.0f;   // mix: deposit left when the paint has run out (on bare canvas)
    if (!smudge_) {
        if (std::isfinite(wetLast_[0])) wetDist_ += std::hypot(d.x - wetLast_[0], d.y - wetLast_[1]);
        wetLast_[0] = d.x;
        wetLast_[1] = d.y;
        const double remaining = chargeLen_ > 0.0 ? std::exp(-wetDist_ / chargeLen_) : 1.0;
        // Colour under the dab as it was before this stroke (the stroke's own fresh paint is
        // not picked up again): coverage-weighted, transparent pixels add no colour.
        const float* lin = srgbToLinearTable();
        const int32_t stride = std::max<int32_t>(1, static_cast<int32_t>(settings_.diameter / 24.0f));
        double sw = 0.0, swa = 0.0, sc[3] = {0.0, 0.0, 0.0};
        for (const auto& stp : tiles_) {
            const StrokeTile& st = *stp;
            if (st.tailRect.empty()) continue;
            const uint8_t* px = st.storeHadTile ? st.base.get() : nullptr;
            const int32_t ox = st.tx * kTile, oy = st.ty * kTile;
            for (int32_t y = st.tailRect.y; y < st.tailRect.bottom(); y += stride) {
                const size_t row = static_cast<size_t>(y - oy) * kTile;
                for (int32_t x = st.tailRect.x; x < st.tailRect.right(); x += stride) {
                    const size_t i = row + static_cast<size_t>(x - ox);
                    const uint16_t m = st.tail[i];
                    if (!m) continue;
                    const double w = m * kInv16;
                    sw += w;
                    if (!px || !px[i * 4 + 3]) continue;
                    const uint8_t* p = px + i * 4;
                    const double wa = w * p[3] / 255.0;
                    swa += wa;
                    const float k = 255.0f / p[3];
                    for (int c = 0; c < 3; ++c)
                        sc[c] += wa * lin[std::min(255, static_cast<int>(p[c] * k + 0.5f))];
                }
            }
        }
        PaintColour loaded, brushC;
        for (int c = 0; c < 3; ++c) brushC.logc[c] = brushMix_[c];
        brushC.lum = brushMix_[3];
        if (!paintLoaded_) {
            for (int c = 0; c < 4; ++c) paintMix_[c] = brushMix_[c];
            paintLoaded_ = true;
        }
        for (int c = 0; c < 3; ++c) loaded.logc[c] = paintMix_[c];
        loaded.lum = paintMix_[3];
        if (sw > 0.0 && swa > 1e-9) {
            float under[3];
            for (int c = 0; c < 3; ++c) under[c] = static_cast<float>(sc[c] / swa);
            // The first dab loads the colour it lands on at once; later ones at the pickup rate.
            const double pick = (dabIndex_ <= 1 ? 1.0 : wetPick_) * (swa / sw);
            mixInto(loaded, paintFromLinear(under), pick);
            for (int c = 0; c < 3; ++c) paintMix_[c] = loaded.logc[c];
            paintMix_[3] = loaded.lum;
        }
        // Paint running out: the dab is more and more what the brush picked up, and where
        // there was nothing to pick up it fades.
        const double cover = sw > 0.0 ? swa / sw : 0.0;
        fade = static_cast<float>(remaining + (1.0 - remaining) * cover);
        PaintColour dabC = brushC;
        mixInto(dabC, loaded, 1.0 - (1.0 - settings_.mix) * remaining);
        double rgb[3];
        paintToLinear(dabC, rgb);
        for (int c = 0; c < 3; ++c) paint[c] = linearToSrgb8(rgb[c]);
    }
    const int32_t ix = static_cast<int32_t>(std::floor(d.x)), iy = static_cast<int32_t>(std::floor(d.y));
    const int32_t off = carryHalf_ * carryCell_;
    // Stroke mask -> opacity fraction (the wet-edge transfer of plain strokes).
    auto transfer = [&](uint32_t m) -> float {
        if (!wetLut_.empty() && m != 0) {
            constexpr int kShift = 16 - kWetLutBits;
            const uint32_t i = m >> kShift, f = m & ((1u << kShift) - 1u);
            m = (wetLut_[i] * ((1u << kShift) - f) + wetLut_[std::min<uint32_t>(i + 1, 1u << kWetLutBits)] * f) >> kShift;
        }
        return static_cast<float>(m) * kInv16;
    };
    for (auto& stp : tiles_) {
        StrokeTile& st = *stp;
        const RectI r = st.tailRect;
        if (r.empty()) continue;
        st.tailRect = {};
        st.dirty = {};
        const int32_t ox = st.tx * kTile, oy = st.ty * kTile;
        uint16_t* plane = st.tail.get();
        uint16_t* strokeMask = st.mask.get();
        const auto clipState = clipped ? clip_->state(st.tx, st.ty) : canvas::SelectionMask::TileState::Full;
        if (clipState == canvas::SelectionMask::TileState::Empty && !smudge_) {
            for (int32_t y = r.y; y < r.bottom(); ++y)
                std::memset(plane + static_cast<size_t>(y - oy) * kTile + (r.x - ox), 0, static_cast<size_t>(r.w) * sizeof(uint16_t));
            continue;
        }
        const uint8_t* clipPx = clipped && clipState != canvas::SelectionMask::TileState::Empty ? clip_->tile(st.tx, st.ty) : nullptr;
        const bool noWrite = clipState == canvas::SelectionMask::TileState::Empty;
        uint8_t* dst = store.tileForWrite(st.tx, st.ty);
        for (int32_t y = r.y; y < r.bottom(); ++y) {
            const size_t row = static_cast<size_t>(y - oy) * kTile;
            const int32_t cy = (y - iy + off) / carryCell_;
            for (int32_t x = r.x; x < r.right(); ++x) {
                const size_t i = row + static_cast<size_t>(x - ox);
                const uint16_t m = plane[i];
                if (!m) continue;
                plane[i] = 0;
                const float cov = m * kInv16;
                float covW = noWrite ? 0.0f : cov;   // deposit: inside the selection only
                if (clipPx) covW *= clipPx[i] * (1.0f / 255.0f);
                uint8_t* o = dst + i * 4;
                if (smudge_) {
                    const int32_t cx = (x - ix + off) / carryCell_;
                    if (cx < 0 || cy < 0 || cx >= carryN_ || cy >= carryN_) continue;
                    float* k = carry_.data() + (static_cast<size_t>(cy) * carryN_ + cx) * 4;
                    if (k[3] < 0.0f) {   // first touch: pick up what is there
                        for (int c = 0; c < 4; ++c) k[c] = o[c];
                        continue;
                    }
                    // Soft tips: the falloff is steepened (1 - (1 - c)^2) so the body of a 0 %
                    // hardness brush still drags at close to the set strength.
                    const float a = wetA_ * covW * (2.0f - cov);
                    const float pk = wetPick_ * cov;
                    for (int c = 0; c < 4; ++c) {
                        const float out = o[c] + (k[c] - o[c]) * a;
                        o[c] = static_cast<uint8_t>(std::min(255.0f, out + 0.5f));
                        k[c] += (out - k[c]) * pk;
                    }
                } else {
                    if (covW <= 0.0f) continue;
                    const uint32_t m0 = strokeMask[i];
                    const uint32_t m1 = buildup_ ? m0 + (static_cast<uint32_t>(m) * (65535u - m0) + 32767u) / 65535u : std::max<uint32_t>(m0, m);
                    if (m1 == m0) continue;
                    strokeMask[i] = static_cast<uint16_t>(m1);
                    const float c0 = wetA_ * transfer(m0), c1 = wetA_ * transfer(m1);
                    if (c1 <= c0 || c0 >= 1.0f) continue;
                    const float a = (c1 - c0) / (1.0f - c0) * (covW / cov) * fade;   // times the selection
                    for (int c = 0; c < 4; ++c) o[c] = static_cast<uint8_t>(std::min(255.0f, o[c] + (paint[c] - o[c]) * a + 0.5f));
                }
            }
        }
        grow(st.touched, r);
        store.markChanged(r);
        stats_.compositedPixels += static_cast<uint64_t>(r.area());
    }
}

void StrokeCompositor::setTail(RasterStore& store, const Dab* dabs, size_t count) {
    if (!active_ || wet_) return;   // wet strokes: no provisional tail (dabs read the canvas)
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
    for (size_t i = 0; i < count; ++i) rasterDab(store, dabs[i], true, dabIndex_ + static_cast<uint32_t>(i));
}

void StrokeCompositor::composite(RasterStore& store, StrokeTile& st) {
    const RectI region = st.dirty.intersect(store.tileRect(st.tx, st.ty));
    st.dirty = {};
    if (region.empty()) return;
    grow(st.touched, region);
    uint8_t* dst = settings_.erase ? const_cast<uint8_t*>(store.tile(st.tx, st.ty)) : store.tileForWrite(st.tx, st.ty);
    if (!dst) return;
    const int32_t ox = st.tx * kTile, oy = st.ty * kTile;
    const float inv = 1.0f / 65535.0f;
    // Selection (Block 10): the stroke's coverage is multiplied by the selection's, so nothing
    // changes outside it and its antialiased edge fades the paint. Unselected tile: base kept.
    const bool clipped = clip_ && clip_->active();
    const auto clipState = clipped ? clip_->state(st.tx, st.ty) : canvas::SelectionMask::TileState::Full;
    const uint8_t* clipPx = clipped ? clip_->tile(st.tx, st.ty) : nullptr;
    for (int32_t y = region.y; y < region.bottom(); ++y) {
        const size_t rowOff = static_cast<size_t>(y - oy) * kTile;
        if (clipState == canvas::SelectionMask::TileState::Empty) {
            std::memcpy(dst + rowOff * 4 + static_cast<size_t>(region.x - ox) * 4, st.base.get() + rowOff * 4 + static_cast<size_t>(region.x - ox) * 4,
                        static_cast<size_t>(region.w) * 4);
            continue;
        }
        const uint16_t* m0 = st.mask.get() + rowOff;
        const uint16_t* m1 = st.tail.get() + rowOff;
        const uint8_t* b = st.base.get() + rowOff * 4;
        uint8_t* o = dst + rowOff * 4;
        for (int32_t x = region.x - ox; x < region.right() - ox; ++x) {
            uint32_t m = buildup_ ? m0[x] + (static_cast<uint32_t>(m1[x]) * (65535u - m0[x]) + 32767u) / 65535u
                                  : std::max(m0[x], m1[x]);
            if (!wetLut_.empty() && m != 0) {
                // Linear interpolation in the transfer table.
                constexpr int kShift = 16 - kWetLutBits;
                const uint32_t i = m >> kShift, f = m & ((1u << kShift) - 1u);
                m = (wetLut_[i] * ((1u << kShift) - f) + wetLut_[std::min<uint32_t>(i + 1, 1u << kWetLutBits)] * f) >> kShift;
            }
            if (clipPx) m = (m * clipPx[rowOff + static_cast<size_t>(x)] + 127u) / 255u;
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
    if (!active_ || wet_) return;   // wet dabs are applied as they come
    for (auto& st : tiles_)
        if (!st->dirty.empty()) composite(store, *st);
}

void StrokeCompositor::commit(RasterStore& store) {
    if (!active_) return;
    // The tail is part of the result only if finish() committed it; a
    // leftover provisional tail is removed.
    setTail(store, nullptr, 0);
    flush(store);
    if (commitHook_) {
        committedScratch_.clear();
        for (const auto& st : tiles_)
            if (!st->touched.empty())
                committedScratch_.push_back({st->tx, st->ty, st->storeHadTile ? st->base.get() : nullptr, st->touched});
        if (!committedScratch_.empty()) commitHook_(store, committedScratch_, settings_);
    }
    releaseTiles();
    active_ = false;
}

void StrokeCompositor::cancel(RasterStore& store) {
    if (!active_) return;
    for (auto& st : tiles_) {
        if (st->storeHadTile) {
            uint8_t* px = store.tileForWrite(st->tx, st->ty);
            std::memcpy(px, st->base.get(), RasterStore::kTileBytes);
        } else {
            store.releaseTile(st->tx, st->ty);  // it did not exist before the stroke: gone again
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
