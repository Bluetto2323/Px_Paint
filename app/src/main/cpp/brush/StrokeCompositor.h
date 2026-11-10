// PxPaint brush engine - stroke compositor.
//
// A stroke is accumulated in a coverage MASK (16-bit) and composited over the
// canvas pixels as they were before the stroke (the BASE):
//
//     mask  = max(mask, dab coverage)                  (per dab)
//     paint : out = colour * a + base * (1 - a),  a = opacity * colourAlpha * mask
//     erase : out = base * (1 - opacity * mask)       (premultiplied, all channels)
//
// Taking the max instead of stacking dabs means overlapping dabs of the same
// stroke never darken: a 40 % stroke is exactly 40 % everywhere, whatever
// the spacing or how often the finger crosses itself, and its antialiased
// edge is a smooth band. A second stroke composites over the first (40 % over
// 40 % = 64 %), as in every painting application. All arithmetic is on
// premultiplied values, so transparent canvases get no dark fringes.
//
// The base is a copy of every tile the stroke touches, taken the first time
// it touches it. It makes compositing exact (no accumulated rounding) and a
// cancelled stroke restores the canvas bit for bit. Stroke tiles are pooled:
// once warmed up, painting does not allocate.
//
// The provisional tail of the path (see StrokePath) lives in a second mask
// that is cleared and redrawn on every update.
//
// Block 5 brush model (BrushSettings::classicRound() == false): dabs go
// through rasterGeneric() - hardness falloff, roundness / angle, procedural
// tip stamps, canvas grain, per-dab jitter (deterministic: hash of the stroke
// seed and the dab index, so a redrawn tail and its committed dabs match),
// and "buildup" accumulation (mask = m + v * (1 - m)). Compositing can map
// the mask through a wet-edge transfer curve (watercolour rim). Classic
// round brushes keep the Block 3/4 rasteriser: same pixels, same cost.
//
// Wet media (Block 14, BrushSettings::wet != None): each committed dab is
// rasterised into the tile's scratch plane (the `tail` plane, unused by wet
// strokes) and applied at once to the store, reading the pixels as they are
// now - colour mixing (Mix) or carrying the pixels along (Smudge). The base
// copies still give exact cancel / undo. All arithmetic is premultiplied, so
// transparency is carried like colour (smudge) and transparent pixels never
// tint the paint (mix). The provisional tail is not drawn for wet strokes.
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

#include "brush/BrushDynamics.h"
#include "brush/BrushTextures.h"
#include "brush/BrushTypes.h"
#include "canvas/RasterStore.h"
#include "canvas/SelectionMask.h"

namespace pe::brush {

// Antialiased coverage of a round dab at a pixel centre distance `dist`
// (canvas px): a 1 px linear ramp centred on the radius. Brushes thinner
// than 1 px keep a 1 px footprint with proportionally lower coverage, so
// hairlines stay continuous instead of breaking into dots.
struct DabShape {
    double radius = 0.5;      // geometric radius (>= 0.5)
    double scale = 1.0;       // coverage multiplier (< 1 only for sub-pixel brushes)
    static DabShape forDiameter(double diameter);
    // Inclusive pixel bounds touched by a dab at (x, y).
    canvas::RectI bounds(double x, double y) const;
};

class StrokeCompositor {
public:
    // A tile the committed stroke changed: its pixels before the stroke
    // (`before`, null if the tile did not exist) and the region written.
    struct CommittedTile {
        int32_t tx = 0, ty = 0;
        const uint8_t* before = nullptr;
        canvas::RectI touched;
    };
    // Called by commit() with the store already holding the final pixels
    // (the history records the stroke from it). Not called for cancels.
    using CommitHook = std::function<void(const canvas::RasterStore&, const std::vector<CommittedTile>&, const BrushSettings&)>;
    void setCommitHook(CommitHook hook) { commitHook_ = std::move(hook); }
    /** Selection the strokes are clipped to (null or inactive: not clipped). Owned by the caller. */
    void setClip(const canvas::SelectionMask* clip) { clip_ = clip; }

    struct Stats {
        uint64_t dabs = 0;
        uint64_t compositedPixels = 0;
        uint32_t strokeTiles = 0;      // tiles touched by the current stroke
        uint32_t pooledTiles = 0;
    };

    // `hasPressure` / `hasTilt`: whether the stroke's samples carry these axes.
    void begin(canvas::RasterStore& store, const BrushSettings& settings, bool hasPressure = false,
               bool hasTilt = false);
    const BrushDynamics& dynamics() const { return dyn_; }
    // Committed dabs: rasterised into the stroke mask.
    void addDabs(canvas::RasterStore& store, const Dab* dabs, size_t count);
    // Replaces the provisional tail.
    void setTail(canvas::RasterStore& store, const Dab* dabs, size_t count);
    // Composites every changed area into the store and marks it changed.
    void flush(canvas::RasterStore& store);
    // Keeps the result (flushes first) and releases the stroke tiles.
    void commit(canvas::RasterStore& store);
    // Restores the canvas exactly as before the stroke.
    void cancel(canvas::RasterStore& store);
    // The store was reset/cleared under an active stroke: drop it silently.
    void abandon();

    bool active() const { return active_; }
    const Stats& stats() const { return stats_; }
    const BrushSettings& settings() const { return settings_; }
    bool classic() const { return classic_; }
    // Wet-edge transfer at mask value m (0..1), exposed for tests.
    static double wetEdgeTransfer(double m, double wetEdge);

    static constexpr size_t kMaxPooledTiles = 48;  // ~24 MiB kept for the next stroke

private:
    struct StrokeTile {
        int32_t tx = 0, ty = 0;
        size_t slotIndex = 0;                   // index in slot_ (store tile index)
        bool storeHadTile = false;              // tile existed before the stroke
        std::unique_ptr<uint8_t[]> base;        // RasterStore::kTileBytes
        std::unique_ptr<uint16_t[]> mask;       // kTile * kTile
        std::unique_ptr<uint16_t[]> tail;       // kTile * kTile
        canvas::RectI dirty;                    // canvas coords, to composite
        canvas::RectI tailRect;                 // canvas coords covered by the tail mask
        canvas::RectI touched;                  // canvas coords written to the store so far
    };
    StrokeTile* tileFor(canvas::RasterStore& store, int32_t tx, int32_t ty, bool create);
    void rasterDab(canvas::RasterStore& store, const Dab& d, bool toTail, uint32_t index);
    void rasterEllipse(canvas::RasterStore& store, const Dab& d, const DabGeometry& g, bool toTail);
    void rasterGeneric(canvas::RasterStore& store, const Dab& d, bool toTail, uint32_t index);
    void composite(canvas::RasterStore& store, StrokeTile& st);
    void applyWet(canvas::RasterStore& store, const Dab& d);
    DabGeometry geometryAt(const Dab& d, uint32_t index) const;   // dynamics + start taper
    void releaseTiles();
    static void grow(canvas::RectI& r, const canvas::RectI& add);

    bool active_ = false;
    BrushSettings settings_;
    BrushDynamics dyn_;
    bool classic_ = true;                       // Block 3/4 rasteriser
    bool buildup_ = false;
    const Texture8* tipTex_ = nullptr;          // stamp tips
    const Texture8* grainTex_ = nullptr;        // canvas grain
    std::vector<uint16_t> wetLut_;              // mask -> coverage (kWetLutSize + 1), empty = identity
    uint32_t dabIndex_ = 0;                     // committed dabs of the stroke (jitter sequence)
    static constexpr int kWetLutBits = 10;
    // Paint constants (premultiplied colour * opacity, 0..255 scale).
    float paintA_ = 0.0f;                       // opacity * colour alpha
    float paintR_ = 0.0f, paintG_ = 0.0f, paintB_ = 0.0f;  // straight colour 0..255
    std::vector<int32_t> slot_;                 // per store tile: index in tiles_ or -1
    std::vector<std::unique_ptr<StrokeTile>> tiles_;
    std::vector<std::unique_ptr<StrokeTile>> pool_;
    Stats stats_;
    CommitHook commitHook_;
    const canvas::SelectionMask* clip_ = nullptr;
    std::vector<CommittedTile> committedScratch_;

    // Wet media state (per stroke; buffers reused between strokes).
    bool wet_ = false;
    bool smudge_ = false;
    float wetA_ = 1.0f;          // per-dab deposit (mix) / strength (smudge)
    float wetPick_ = 1.0f;       // per-dab pickup
    float brushLin_[3] = {};     // brush colour, linear light
    double brushMix_[4] = {};    // brush colour in the mixing space (ln r, ln g, ln b, luminance)
    double paintMix_[4] = {};    // colour loaded on the brush, same space
    bool paintLoaded_ = false;
    double chargeLen_ = 0.0;     // mix: distance over which the paint runs out (0 = never)
    double wetDist_ = 0.0;       // mix: distance painted in this stroke
    double wetLast_[2] = {};
    double taperStep_ = 1.0, taperLen_ = 0.0;   // start taper (px per dab, length; 0 = off)

    std::vector<float> carry_;   // smudge: carried premultiplied RGBA, 0..255 (alpha < 0 = empty)
    int32_t carryN_ = 0, carryCell_ = 1, carryHalf_ = 0;
public:
    // Smudge carry grid edge (cells), exposed for tests.
    static constexpr int32_t kMaxCarry = 512;
};

}  // namespace pe::brush
