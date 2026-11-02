// Paint Engine - frame scheduling and timing.
//
// Decides *whether* a frame should be produced (continuous vs on-demand) and
// measures how long frames take. Presentation pacing itself is done by the
// backend (FIFO present / eglSwapInterval(1)), which blocks on vsync.
#pragma once

#include <cstdint>
#include <deque>

namespace pe::gfx {

enum class RenderMode : uint8_t {
    Continuous,  // render every vsync (diagnostics / measurements)
    OnDemand,    // render only when requested (default for a paint app)
};
const char* toString(RenderMode mode);

struct FrameStats {
    uint64_t framesPresented = 0;
    uint64_t framesSkipped = 0;     // NoSurface / OutOfDate / errors
    double lastCpuMs = 0.0;         // record + submit + present call
    double avgCpuMs = 0.0;          // exponential moving average
    double maxCpuMsWindow = 0.0;    // max over the last ~2 seconds
    double lastIntervalMs = 0.0;    // time between consecutive presents
    double avgIntervalMs = 0.0;
    double fps = 0.0;               // presents during the last second
};

class FrameScheduler {
public:
    void setMode(RenderMode mode) { mode_ = mode; }
    RenderMode mode() const { return mode_; }

    void requestFrame() { frameRequested_ = true; }
    // True if, given that a surface is available, a frame should be produced.
    bool wantsFrame() const { return mode_ == RenderMode::Continuous || frameRequested_; }

    // Monotonically increasing frame serial (starts at 1).
    uint64_t beginFrame(int64_t nowNs);
    void endFramePresented(int64_t nowNs);
    void endFrameSkipped();

    uint64_t currentSerial() const { return serial_; }
    const FrameStats& stats() const { return stats_; }
    void resetIntervalTracking() { lastPresentNs_ = 0; }

private:
    RenderMode mode_ = RenderMode::OnDemand;
    bool frameRequested_ = true;
    uint64_t serial_ = 0;
    int64_t frameStartNs_ = 0;
    int64_t lastPresentNs_ = 0;
    FrameStats stats_;
    struct Sample {
        int64_t timeNs;
        double cpuMs;
    };
    std::deque<Sample> window_;
};

}  // namespace pe::gfx
