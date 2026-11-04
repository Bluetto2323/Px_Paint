// PxPaint - selection mask (Block 10).
//
// A tiled 8-bit coverage mask the size of the canvas, independent from the
// layers: 0 = not selected, 255 = selected, in between = partially selected
// (antialiased edges). Tiles are 256 x 256 like the raster tiles; a tile is
// EMPTY or FULL without any pixel memory, PARTIAL tiles hold 64 KiB. A
// selection is "active" when something is selected; no active selection
// means every pixel can be edited (the brush is not clipped).
//
// Shapes (rectangle, ellipse, lasso, polygon) are all rasterised as polygons
// with exact horizontal coverage on 4 sub-scanlines per pixel row (nonzero
// winding, so a self-crossing lasso stays filled), then combined with the
// current selection tile by tile (replace, add, subtract, intersect).
#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include "canvas/CanvasMath.h"

namespace pe::canvas {

class SelectionMask {
public:
    static constexpr int32_t kTile = 256;
    static constexpr size_t kTileBytes = static_cast<size_t>(kTile) * kTile;
    enum class TileState : uint8_t { Empty = 0, Full = 1, Partial = 2 };
    enum class Mode : uint8_t { Replace = 0, Add = 1, Subtract = 2, Intersect = 3 };

    void reset(uint32_t width, uint32_t height);   // no selection
    uint32_t width() const { return width_; }
    uint32_t height() const { return height_; }
    int32_t columns() const { return cols_; }
    int32_t rows() const { return rows_; }
    bool active() const { return active_; }
    uint64_t version() const { return version_; }

    TileState state(int32_t tx, int32_t ty) const;
    /** Pixels of a PARTIAL tile (row-major 256 x 256), null otherwise. */
    const uint8_t* tile(int32_t tx, int32_t ty) const;
    /** Coverage of a pixel: 255 everywhere when no selection is active. */
    uint8_t at(int32_t x, int32_t y) const;

    // --- edits (each bumps the version) -----------------------------------
    void clear();                         // deselect
    void selectAll();
    void invert();                        // no-op without an active selection
    /** Combines [shape] (same size; its active flag ignored) into this selection. */
    void combine(const SelectionMask& shape, Mode mode);
    /** Moves the selection by whole pixels (what falls outside is dropped). */
    void translate(int32_t dx, int32_t dy);
    /** Maps the selection through [m] (source -> destination), bilinear coverage (Block 12). */
    void transform(const Affine2& m);

    // --- shape building (on a scratch mask, then combine) ------------------
    /** Rasterises a closed polygon (x, y pairs, canvas pixels) into this mask (replaces it). */
    void setPolygon(const float* xy, size_t points, bool antialias);
    /**
     * Polygon of an axis-aligned rectangle (corners rounded to whole pixels:
     * crisp edges) or of the ellipse inscribed in it (chord error < 0.1 px),
     * between two canvas points in any order.
     */
    static void shapePolygon(bool ellipse, double x0, double y0, double x1, double y1, std::vector<float>& out);
    /** Direct write of a tile's coverage (from content); [px] null = empty. */
    void setTile(int32_t tx, int32_t ty, const uint8_t* px);
    void setActive(bool a) { active_ = a; version_++; }

    // --- queries ------------------------------------------------------------
    /** Tight bounds of the selected pixels; false if nothing is selected. */
    bool bounds(RectI& out) const;
    uint64_t bytes() const;               // pixel memory (partial tiles)
    uint64_t checksum() const;            // tests: equal = same selection
    /** Selected area in pixels (coverage / 255 summed). */
    double area() const;

    /**
     * Outline at 50 % coverage as closed loops along pixel edges, canvas
     * coordinates: [loopCount, n0, x, y, ... (n0 points), n1, ...]. Collinear
     * points are merged.
     */
    void contours(std::vector<float>& out) const;
    /**
     * Inverse coverage (255 = unselected) scaled down so the longer side is at
     * most [maxSide]; for dimming the unselected area on screen.
     */
    void preview(uint32_t maxSide, uint32_t& outW, uint32_t& outH, std::vector<uint8_t>& out) const;

    // --- compact copy (history) ----------------------------------------------
    void encode(std::vector<uint8_t>& out) const;   // states + RLE of partial tiles
    bool decode(const uint8_t* data, size_t size);

private:
    struct Tile {
        TileState state = TileState::Empty;
        std::unique_ptr<uint8_t[]> px;
    };
    size_t index(int32_t tx, int32_t ty) const { return static_cast<size_t>(ty) * static_cast<size_t>(cols_) + static_cast<size_t>(tx); }
    uint8_t* writable(Tile& t);           // makes a tile PARTIAL with its current value
    void normalize(Tile& t);              // PARTIAL tile that is uniform -> EMPTY / FULL
    bool anySelected() const;

    uint32_t width_ = 0, height_ = 0;
    int32_t cols_ = 0, rows_ = 0;
    std::vector<Tile> tiles_;
    bool active_ = false;
    uint64_t version_ = 1;
};

}  // namespace pe::canvas
