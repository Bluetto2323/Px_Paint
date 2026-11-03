#include "brush/BrushEngine.h"

#include <algorithm>
#include <cstring>

#include "core/Time.h"

namespace pe::brush {

BrushEngine::BrushEngine() {
    queue_.reserve(8192);
    work_.reserve(8192);
    beginSettings_.reserve(16);
    workSettings_.reserve(16);
    dabs_.reserve(16384);
}

void BrushEngine::push(const Event& e) {
    std::lock_guard<std::mutex> lock(inputMutex_);
    if (queue_.empty()) queuedSinceNs_ = nowNs();
    queue_.push_back(e);
    if (e.type == Ev::Begin || e.type == Ev::Sample) received_++;
    pending_.store(true, std::memory_order_release);
}

void BrushEngine::begin(const BrushSettings& settings, const StrokePoint& first) {
    Event e;
    e.type = Ev::Begin;
    e.p = first;
    std::lock_guard<std::mutex> lock(inputMutex_);
    if (queue_.empty()) queuedSinceNs_ = nowNs();
    beginSettings_.push_back(settings.sanitized());
    queue_.push_back(e);
    received_++;
    pending_.store(true, std::memory_order_release);
}

void BrushEngine::addSamples(const StrokePoint* points, size_t count) {
    if (count == 0) return;
    std::lock_guard<std::mutex> lock(inputMutex_);
    if (queue_.empty()) queuedSinceNs_ = nowNs();
    for (size_t i = 0; i < count; ++i) {
        Event e;
        e.type = Ev::Sample;
        e.p = points[i];
        queue_.push_back(e);
    }
    received_ += count;
    pending_.store(true, std::memory_order_release);
}

void BrushEngine::end() {
    Event e;
    e.type = Ev::End;
    push(e);
}

void BrushEngine::cancel() {
    Event e;
    e.type = Ev::Cancel;
    push(e);
}

void BrushEngine::discard() {
    {
        std::lock_guard<std::mutex> lock(inputMutex_);
        queue_.clear();
        beginSettings_.clear();
        pending_.store(false, std::memory_order_release);
    }
    path_.reset();
    comp_.abandon();
    dabs_.clear();
}

bool BrushEngine::process(canvas::RasterStore& store) {
    {
        std::lock_guard<std::mutex> lock(inputMutex_);
        work_.clear();
        work_.swap(queue_);  // both keep their capacity: no allocation once warmed up
        workSettings_.clear();
        workSettings_.swap(beginSettings_);
        pending_.store(false, std::memory_order_release);
        if (!work_.empty() && (consumedQueuedNs_ == 0 || queuedSinceNs_ < consumedQueuedNs_))
            consumedQueuedNs_ = queuedSinceNs_;
        st_.samplesReceived = received_;
    }
    if (work_.empty()) return false;
    const int64_t t0 = nowNs();
    bool tailDirty = false;
    size_t settingsIndex = 0;
    for (const Event& e : work_) {
        switch (e.type) {
            case Ev::Begin:
                if (comp_.active()) {  // missing end(): keep what was drawn
                    path_.finish();
                    dabs_.clear();
                    path_.takeCommitted(dabs_);
                    comp_.addDabs(store, dabs_.data(), dabs_.size());
                    comp_.commit(store);
                    st_.strokesCommitted++;
                }
                {
                    BrushSettings bs = workSettings_[settingsIndex++];
                    bs.seed = strokeSeed(e.p);
                    strokeHasPressure_ = std::isfinite(e.p.axes.pressure);
                    strokeHasTilt_ = std::isfinite(e.p.axes.tilt) && std::isfinite(e.p.axes.orientation);
                    pressure_.reset(strokeHasPressure_ ? bs.pressureSmoothing : 0.0f);
                    comp_.begin(store, bs, strokeHasPressure_, strokeHasTilt_);
                    path_.begin(prepare(e.p), bs.spacingPx(), &comp_.dynamics());
                }
                st_.strokesBegun++;
                st_.samplesProcessed++;
                st_.pathPoints++;
                tailDirty = true;
                break;
            case Ev::Sample:
                st_.samplesProcessed++;
                if (path_.active() && path_.add(prepare(e.p))) {
                    st_.pathPoints++;
                    tailDirty = true;
                }
                break;
            case Ev::End:
                if (comp_.active()) {
                    path_.finish();
                    dabs_.clear();
                    path_.takeCommitted(dabs_);
                    comp_.addDabs(store, dabs_.data(), dabs_.size());
                    st_.dabs += dabs_.size();
                    comp_.commit(store);
                    st_.strokesCommitted++;
                    tailDirty = false;
                }
                break;
            case Ev::Cancel:
                if (comp_.active()) {
                    path_.reset();
                    comp_.cancel(store);
                    st_.strokesCancelled++;
                    tailDirty = false;
                }
                break;
        }
        // Committed dabs are rasterised as they appear (keeps dabs_ small).
        if (comp_.active()) {
            dabs_.clear();
            path_.takeCommitted(dabs_);
            if (!dabs_.empty()) {
                comp_.addDabs(store, dabs_.data(), dabs_.size());
                st_.dabs += dabs_.size();
            }
        }
    }
    if (comp_.active()) {
        if (tailDirty) {
            dabs_.clear();
            path_.provisional(dabs_);
            comp_.setTail(store, dabs_.data(), dabs_.size());
        }
        comp_.flush(store);
    }
    const auto& cs = comp_.stats();
    st_.compositedPixels = cs.compositedPixels;
    st_.strokeTiles = cs.strokeTiles;
    st_.pooledTiles = cs.pooledTiles;
    st_.strokeActive = comp_.active();
    st_.lastProcessMs = static_cast<double>(nowNs() - t0) / 1e6;
    st_.maxProcessMs = std::max(st_.maxProcessMs, st_.lastProcessMs);
    return true;
}

void BrushEngine::finishActive(canvas::RasterStore& store) {
    if (!comp_.active()) return;
    path_.finish();
    dabs_.clear();
    path_.takeCommitted(dabs_);
    comp_.addDabs(store, dabs_.data(), dabs_.size());
    st_.dabs += dabs_.size();
    comp_.commit(store);
    path_.reset();
    st_.strokesCommitted++;
    st_.strokeActive = false;
}

uint32_t BrushEngine::strokeSeed(const StrokePoint& p) {
    // From the first sample: replaying the same input gives the same jitter.
    auto bits = [](double v) {
        const float f = static_cast<float>(v);
        uint32_t u;
        std::memcpy(&u, &f, sizeof u);
        return u;
    };
    uint32_t h = 0x811C9DC5u;
    for (uint32_t v : {bits(p.x), bits(p.y), bits(p.t)}) {
        h ^= v;
        h *= 0x01000193u;
        h ^= h >> 15;
    }
    return h;
}

StrokePoint BrushEngine::prepare(const StrokePoint& in) {
    StrokePoint p = in;
    if (strokeHasPressure_) {
        p.axes.pressure = pressure_.filter(std::isfinite(p.axes.pressure) ? std::clamp(p.axes.pressure, 0.0f, 1.0f)
                                                                         : p.axes.pressure,
                                           p.t);
    } else {
        p.axes.pressure = kNoValue;
    }
    if (!strokeHasTilt_) {
        p.axes.tilt = kNoValue;
        p.axes.orientation = kNoValue;
    }
    return p;
}

BrushStats BrushEngine::stats() const {
    BrushStats s = st_;
    std::lock_guard<std::mutex> lock(inputMutex_);
    s.samplesReceived = received_;
    s.pendingEvents = static_cast<uint32_t>(queue_.size());
    return s;
}

}  // namespace pe::brush
