// PxPaint brush engine - stroke lifecycle and input queue.
//
//   UI / JNI thread                      render thread (or a flushing caller)
//   ---------------                      ------------------------------------
//   begin / addSamples / end / cancel -> [input queue] -> process(store):
//   (canvas coordinates, never blocks       StrokePath  -> dabs
//    on rendering, no GPU access)           StrokeCompositor -> RasterStore
//                                           (dirty regions for the GPU)
//
// Input collection is decoupled from rendering: samples are appended to a
// mutex-guarded queue (amortised, no allocation once warmed up) and consumed
// in order, all of them, whenever the next frame is prepared. Nothing is
// dropped or merged on the way except exact-duplicate positions (a finger
// resting in place carries no geometry).
//
// process() must be called with the store protected by the caller
// (CanvasDocument's lock).
#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <vector>

#include "brush/BrushDynamics.h"
#include "brush/BrushTypes.h"
#include "brush/StrokeCompositor.h"
#include "brush/StrokePath.h"
#include "canvas/RasterStore.h"

namespace pe::brush {

struct BrushStats {
    uint64_t strokesBegun = 0;
    uint64_t strokesCommitted = 0;
    uint64_t strokesCancelled = 0;
    uint64_t samplesReceived = 0;    // queued by the UI (incl. the first point)
    uint64_t samplesProcessed = 0;   // consumed by process()
    uint64_t pathPoints = 0;         // samples that added geometry
    uint64_t dabs = 0;               // committed dabs rasterised
    uint64_t compositedPixels = 0;
    uint32_t strokeTiles = 0;        // tiles touched by the active stroke
    uint32_t pooledTiles = 0;
    uint32_t pendingEvents = 0;      // queued, not processed yet
    double lastProcessMs = 0.0;      // last process() call that did work
    double maxProcessMs = 0.0;
    bool strokeActive = false;
};

class BrushEngine {
public:
    BrushEngine();

    // --- Input side: any thread, cheap, never waits for rendering ----------
    void begin(const BrushSettings& settings, const StrokePoint& first);
    void addSamples(const StrokePoint* points, size_t count);
    void end();
    void cancel();
    bool hasPending() const { return pending_.load(std::memory_order_acquire); }

    // --- Processing side: caller holds the store's lock --------------------
    // Consumes every queued event. Returns true if the store changed.
    bool process(canvas::RasterStore& store);
    // Commits a stroke still in progress into `store` (its later samples are
    // ignored until the next begin): the target layer is about to change.
    void finishActive(canvas::RasterStore& store);
    bool strokeActive() const { return comp_.active(); }
    // Receives every committed stroke (history). Called under the caller's lock.
    void setCommitHook(StrokeCompositor::CommitHook hook) { comp_.setCommitHook(std::move(hook)); }
    void setClip(const canvas::SelectionMask* clip) { comp_.setClip(clip); }
    // Time (nowNs) the oldest event consumed by the last process() call was
    // queued, then 0 until another call consumes input. Input-to-display
    // latency is measured from it when that frame is presented.
    int64_t takeConsumedInputTime() {
        const int64_t t = consumedQueuedNs_;
        consumedQueuedNs_ = 0;
        return t;
    }
    // The canvas was replaced/cleared: forget any stroke without touching it
    // (also drops queued events that belonged to the old canvas).
    void discard();

    BrushStats stats() const;

private:
    enum class Ev : uint8_t { Begin, Sample, End, Cancel };
    struct Event {
        Ev type = Ev::Sample;
        StrokePoint p;
    };
    void push(const Event& e);

    mutable std::mutex inputMutex_;
    std::vector<Event> queue_;       // guarded by inputMutex_
    std::vector<BrushSettings> beginSettings_;  // one per queued Begin, in order (guarded)
    uint64_t received_ = 0;          // guarded by inputMutex_
    int64_t queuedSinceNs_ = 0;      // guarded by inputMutex_: oldest event in queue_
    int64_t consumedQueuedNs_ = 0;   // processing side
    std::atomic<bool> pending_{false};

    // Processing state (caller-synchronised)
    std::vector<Event> work_;
    std::vector<BrushSettings> workSettings_;
    PressureFilter pressure_;
    bool strokeHasPressure_ = false, strokeHasTilt_ = false;
    // Prepares a sample for the active stroke: smoothed pressure; axes the
    // stroke does not use are cleared (no mixing finger / stylus data).
    StrokePoint prepare(const StrokePoint& p);
public:
    // Jitter seed of a stroke, from its first sample (deterministic replay).
    static uint32_t strokeSeed(const StrokePoint& p);
private:
    std::vector<Dab> dabs_;
    StrokePath path_;
    StrokeCompositor comp_;
    BrushStats st_;
};

}  // namespace pe::brush
