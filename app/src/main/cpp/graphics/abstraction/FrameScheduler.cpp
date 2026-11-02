#include "graphics/abstraction/FrameScheduler.h"

#include <algorithm>

#include "core/Time.h"

namespace pe::gfx {

const char* toString(RenderMode mode) {
    return mode == RenderMode::Continuous ? "continuous" : "on-demand";
}

uint64_t FrameScheduler::beginFrame(int64_t now) {
    frameStartNs_ = now;
    return ++serial_;
}

void FrameScheduler::endFramePresented(int64_t now) {
    frameRequested_ = false;
    const double cpuMs = nsToMs(now - frameStartNs_);
    stats_.framesPresented++;
    stats_.lastCpuMs = cpuMs;
    stats_.avgCpuMs = stats_.framesPresented == 1 ? cpuMs : stats_.avgCpuMs * 0.95 + cpuMs * 0.05;

    if (lastPresentNs_ != 0) {
        const double interval = nsToMs(now - lastPresentNs_);
        stats_.lastIntervalMs = interval;
        stats_.avgIntervalMs = stats_.avgIntervalMs == 0.0 ? interval : stats_.avgIntervalMs * 0.95 + interval * 0.05;
    }
    lastPresentNs_ = now;

    window_.push_back({now, cpuMs});
    constexpr int64_t kWindowNs = 2'000'000'000;
    while (!window_.empty() && now - window_.front().timeNs > kWindowNs) window_.pop_front();
    double maxMs = 0.0;
    size_t lastSecond = 0;
    for (const auto& s : window_) {
        maxMs = std::max(maxMs, s.cpuMs);
        if (now - s.timeNs <= 1'000'000'000) lastSecond++;
    }
    stats_.maxCpuMsWindow = maxMs;
    stats_.fps = static_cast<double>(lastSecond);
}

void FrameScheduler::endFrameSkipped() { stats_.framesSkipped++; }

}  // namespace pe::gfx
