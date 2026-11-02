// PxPaint canvas - geometry primitives.
//
// Coordinate spaces (both y-down, in pixels):
//   canvas space : document pixels, (0,0) = top-left of the canvas,
//                  independent of the device screen;
//   screen space : surface pixels of the render target.
// Doubles are used throughout so transforms stay exact enough at extreme
// zoom levels on large canvases.
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace pe::canvas {

constexpr double kPi = 3.14159265358979323846;

struct Vec2 {
    double x = 0.0;
    double y = 0.0;
    Vec2 operator+(Vec2 o) const { return {x + o.x, y + o.y}; }
    Vec2 operator-(Vec2 o) const { return {x - o.x, y - o.y}; }
    Vec2 operator*(double s) const { return {x * s, y * s}; }
    double length() const { return std::sqrt(x * x + y * y); }
};

// Integer pixel rectangle, half-open: [x, x+w) x [y, y+h).
struct RectI {
    int32_t x = 0, y = 0, w = 0, h = 0;
    bool empty() const { return w <= 0 || h <= 0; }
    int32_t right() const { return x + w; }
    int32_t bottom() const { return y + h; }
    int64_t area() const { return empty() ? 0 : static_cast<int64_t>(w) * h; }
    bool operator==(const RectI& o) const { return x == o.x && y == o.y && w == o.w && h == o.h; }
    bool contains(int32_t px, int32_t py) const { return px >= x && py >= y && px < right() && py < bottom(); }
    RectI intersect(const RectI& o) const {
        const int32_t l = std::max(x, o.x), t = std::max(y, o.y);
        const int32_t r = std::min(right(), o.right()), b = std::min(bottom(), o.bottom());
        return r > l && b > t ? RectI{l, t, r - l, b - t} : RectI{};
    }
};

struct RectD {
    double x = 0, y = 0, w = 0, h = 0;
    bool empty() const { return w <= 0 || h <= 0; }
    // Smallest integer rect covering this one.
    RectI roundOut() const {
        const double l = std::floor(x), t = std::floor(y), r = std::ceil(x + w), b = std::ceil(y + h);
        return {static_cast<int32_t>(l), static_cast<int32_t>(t), static_cast<int32_t>(r - l),
                static_cast<int32_t>(b - t)};
    }
};

// 2D affine transform:  x' = a*x + c*y + tx ;  y' = b*x + d*y + ty
struct Affine2 {
    double a = 1, b = 0, c = 0, d = 1, tx = 0, ty = 0;

    Vec2 apply(Vec2 p) const { return {a * p.x + c * p.y + tx, b * p.x + d * p.y + ty}; }
    double determinant() const { return a * d - b * c; }
    Affine2 inverse() const {
        const double det = determinant();
        const double ia = d / det, ib = -b / det, ic = -c / det, id = a / det;
        return {ia, ib, ic, id, -(ia * tx + ic * ty), -(ib * tx + id * ty)};
    }
    // (l * r)(p) = l(r(p))
    static Affine2 compose(const Affine2& l, const Affine2& r) {
        return {l.a * r.a + l.c * r.b,   l.b * r.a + l.d * r.b,   l.a * r.c + l.c * r.d,
                l.b * r.c + l.d * r.d,   l.a * r.tx + l.c * r.ty + l.tx, l.b * r.tx + l.d * r.ty + l.ty};
    }
    static Affine2 translate(double x, double y) { return {1, 0, 0, 1, x, y}; }
    static Affine2 scale(double s) { return {s, 0, 0, s, 0, 0}; }
    // Positive angle = clockwise on a y-down screen.
    static Affine2 rotate(double radians) {
        const double cs = std::cos(radians), sn = std::sin(radians);
        return {cs, sn, -sn, cs, 0, 0};
    }
};

inline double normalizeAngle(double radians) {
    double r = std::fmod(radians, 2.0 * kPi);
    if (r <= -kPi) r += 2.0 * kPi;
    if (r > kPi) r -= 2.0 * kPi;
    return r;
}

}  // namespace pe::canvas
