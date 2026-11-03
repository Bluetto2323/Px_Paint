#include "brush/BrushEngine.h"

#include <algorithm>

#include "core/Time.h"

namespace pe::brush {

BrushEngine::BrushEngine() {
    queue_.reserve(8192);
    work_.reserve(8192);
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
    e.s = settings.sanitized();
    push(e);
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
        pending_.store(false, std::memory_order_release);
        if (!work_.empty() && (consumedQueuedNs_ == 0 || queuedSinceNs_ < consumedQueuedNs_))
            consumedQueuedNs_ = queuedSinceNs_;
        st_.samplesReceived = received_;
    }
    if (work_.empty()) return false;
    const int64_t t0 = nowNs();
    bool tailDirty = false;
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
                comp_.begin(store, e.s);
                path_.begin(e.p, e.s.spacingPx());
                st_.strokesBegun++;
                st_.samplesProcessed++;
                st_.pathPoints++;
                tailDirty = true;
                break;
            case Ev::Sample:
                st_.samplesProcessed++;
                if (path_.active() && path_.add(e.p)) {
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

BrushStats BrushEngine::stats() const {
    BrushStats s = st_;
    std::lock_guard<std::mutex> lock(inputMutex_);
    s.samplesReceived = received_;
    s.pendingEvents = static_cast<uint32_t>(queue_.size());
    return s;
}

}  // namespace pe::brush
