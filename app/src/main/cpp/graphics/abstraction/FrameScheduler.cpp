#include "graphics/abstraction/FrameScheduler.h"

#include <algorithm>

#include "core/Time.h"

namespace pe::gfx {

namespace {
constexpr int64_t kSteadyWindowNs = 2'000'000'000;
constexpr int64_t kRateWindowNs = 1'000'000'000;
}  // namespace

const char* toString(RenderMode mode) {
    return mode == RenderMode::Continuous ? "continuous" : "on-demand";
}

void FrameScheduler::setMode(RenderMode mode) {
    mode_ = mode;
    resetIntervalTracking();
}

void FrameScheduler::resetIntervalTracking() {
    lastPresentNs_ = 0;
    rateWindow_.clear();
    stats_.rateValid = false;
    stats_.fps = 0.0;
    stats_.avgIntervalMs = 0.0;
}

void FrameScheduler::markAttach(int64_t now) {
    attachNs_ = now;
    stats_.firstFrameMs = 0.0;
    markWarmup();
    resetIntervalTracking();
}

void FrameScheduler::markWarmup() { stats_.warmupFramesLeft = kWarmupFrames; }

uint64_t FrameScheduler::beginFrame(int64_t now) {
    frameStartNs_ = now;
    return ++serial_;
}

void FrameScheduler::endFramePresented(int64_t now, double waitMs, double recordMs, double submitMs) {
    frameRequested_ = false;
    const double cpuMs = recordMs + submitMs;
    stats_.framesPresented++;
    stats_.lastCpuMs = cpuMs;

    if (attachNs_ != 0) {
        stats_.firstFrameMs = nsToMs(now - attachNs_);
        attachNs_ = 0;
    }

    if (stats_.warmupFramesLeft > 0) {
        stats_.warmupFramesLeft--;
    } else {
        stats_.steadyFrames++;
        auto ema = [&](double& avg, double v) { avg = stats_.steadyFrames == 1 ? v : avg * 0.95 + v * 0.05; };
        ema(stats_.steadyAvgCpuMs, cpuMs);
        ema(stats_.steadyAvgRecordMs, recordMs);
        ema(stats_.steadyAvgSubmitMs, submitMs);
        ema(stats_.steadyAvgWaitMs, waitMs);
        steadyWindow_.push_back({now, cpuMs});
        while (!steadyWindow_.empty() && now - steadyWindow_.front().timeNs > kSteadyWindowNs)
            steadyWindow_.pop_front();
        double maxMs = 0.0;
        for (const auto& s : steadyWindow_) maxMs = std::max(maxMs, s.cpuMs);
        stats_.steadyMaxCpuMs = maxMs;
    }

    // Rates: continuous rendering only, never across idle gaps.
    if (mode_ != RenderMode::Continuous) {
        resetIntervalTracking();
        return;
    }
    if (lastPresentNs_ != 0) {
        const double interval = nsToMs(now - lastPresentNs_);
        if (interval <= kMaxIntervalMs) {
            stats_.avgIntervalMs =
                stats_.avgIntervalMs == 0.0 ? interval : stats_.avgIntervalMs * 0.95 + interval * 0.05;
        } else {
            rateWindow_.clear();  // a stall: restart the rate window
        }
    }
    lastPresentNs_ = now;
    rateWindow_.push_back(now);
    while (!rateWindow_.empty() && now - rateWindow_.front() > kRateWindowNs) rateWindow_.pop_front();
    // Valid once we have about half a second of uninterrupted frames.
    const double spanMs = rateWindow_.size() > 1 ? nsToMs(rateWindow_.back() - rateWindow_.front()) : 0.0;
    stats_.rateValid = spanMs >= 500.0;
    stats_.fps = stats_.rateValid ? static_cast<double>(rateWindow_.size() - 1) * 1000.0 / spanMs : 0.0;
}

void FrameScheduler::endFrameSkipped(FrameStatus reason) {
    stats_.framesSkipped++;
    switch (reason) {
        case FrameStatus::NoSurface: stats_.skippedNoSurface++; break;
        case FrameStatus::SurfaceOutOfDate: stats_.skippedOutOfDate++; break;
        case FrameStatus::SurfaceLost: stats_.skippedSurfaceLost++; break;
        case FrameStatus::DeviceLost: stats_.skippedDeviceLost++; break;
        case FrameStatus::Error:
        case FrameStatus::Ok: stats_.skippedError++; break;
    }
}

}  // namespace pe::gfx
