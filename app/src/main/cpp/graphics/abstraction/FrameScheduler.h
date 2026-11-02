// Paint Engine - frame scheduling and timing.
//
// Decides *whether* a frame should be produced (continuous vs on-demand) and
// measures frames. Presentation pacing itself is done by the backend (FIFO
// present / eglSwapInterval(1)), which blocks on vsync.
//
// Timing is split so start-up costs never pollute steady-state numbers:
//   * first-frame latency : surface attached -> first presented frame
//                           (includes pipeline creation, swapchain build);
//   * warm-up frames      : the first kWarmupFrames after an attach/resize are
//                           excluded from steady-state statistics;
//   * steady state        : CPU time per frame (record + submit + present).
// Rate statistics (fps, intervals) are only meaningful while rendering
// continuously: in on-demand mode they are reported as not applicable, and
// gaps longer than kMaxIntervalMs (idle, paused) are never counted.
#pragma once

#include <cstdint>
#include <deque>

#include "graphics/abstraction/GraphicsTypes.h"

namespace pe::gfx {

enum class RenderMode : uint8_t {
    Continuous,  // render every vsync (measurements, animations)
    OnDemand,    // render only when requested (default for a paint app)
};
const char* toString(RenderMode mode);

struct FrameStats {
    // Counts
    uint64_t framesPresented = 0;   // frames actually rendered and presented
    uint64_t framesSkipped = 0;     // attempts that presented nothing (sum below)
    uint64_t skippedNoSurface = 0;
    uint64_t skippedOutOfDate = 0;  // swapchain rebuild pending / zero-size / acquire timeout
    uint64_t skippedSurfaceLost = 0;
    uint64_t skippedDeviceLost = 0;
    uint64_t skippedError = 0;

    // Start-up
    double firstFrameMs = 0.0;      // last attach -> first present (0 = none yet)
    uint32_t warmupFramesLeft = 0;

    // Steady state (warm-up excluded)
    uint64_t steadyFrames = 0;
    double lastCpuMs = 0.0;
    double steadyAvgCpuMs = 0.0;    // exponential moving average
    double steadyMaxCpuMs = 0.0;    // max over the last ~2 s of steady frames

    // Continuous-mode rate (valid only when rateValid)
    bool rateValid = false;
    double fps = 0.0;               // presents per second, continuous only
    double avgIntervalMs = 0.0;     // between consecutive continuous presents
};

class FrameScheduler {
public:
    static constexpr uint32_t kWarmupFrames = 10;
    static constexpr double kMaxIntervalMs = 250.0;

    void setMode(RenderMode mode);
    RenderMode mode() const { return mode_; }

    void requestFrame() { frameRequested_ = true; }
    // True if, given that a surface is available, a frame should be produced.
    bool wantsFrame() const { return mode_ == RenderMode::Continuous || frameRequested_; }

    // A surface was (re)attached: start first-frame timing and warm-up.
    void markAttach(int64_t nowNs);
    // Resize / swapchain rebuild ahead: the next frames are warm-up.
    void markWarmup();

    // Monotonically increasing frame serial (starts at 1).
    uint64_t beginFrame(int64_t nowNs);
    void endFramePresented(int64_t nowNs);
    void endFrameSkipped(FrameStatus reason);

    uint64_t currentSerial() const { return serial_; }
    const FrameStats& stats() const { return stats_; }
    void resetIntervalTracking();

private:
    RenderMode mode_ = RenderMode::OnDemand;
    bool frameRequested_ = true;
    uint64_t serial_ = 0;
    int64_t frameStartNs_ = 0;
    int64_t lastPresentNs_ = 0;
    int64_t attachNs_ = 0;   // 0 = first frame already measured
    FrameStats stats_;
    struct Sample {
        int64_t timeNs;
        double cpuMs;
    };
    std::deque<Sample> steadyWindow_;   // last ~2 s of steady frames
    std::deque<int64_t> rateWindow_;    // continuous presents in the last ~1 s
};

}  // namespace pe::gfx
