// Paint Engine - GPU resource lifetime manager.
//
// Responsibilities:
//  * hand out Ref<T> whose deleter retires the object instead of deleting it;
//  * delete retired objects only once the GPU completed the frame (serial)
//    during which they were released -> no resource destroyed while in use;
//  * keep live counts / memory totals for diagnostics and leak detection.
//
// Thread model: create/retire/collect are expected on the render thread, but
// the internal state is mutex-protected so a stray release from another
// thread is still safe (it is only deferred).
#pragma once

#include <array>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_set>
#include <vector>

#include "graphics/abstraction/GpuResource.h"
#include "graphics/abstraction/GraphicsTypes.h"

namespace pe::gfx {

struct ResourceStats {
    std::array<uint32_t, static_cast<size_t>(ResourceKind::Count)> live{};
    uint32_t pendingDestruction = 0;
    uint64_t liveGpuBytes = 0;
    uint64_t totalCreated = 0;
    uint64_t totalDestroyed = 0;
    uint32_t totalLive() const;
};

class ResourceManager {
public:
    ResourceManager();
    ~ResourceManager();  // destroys anything still retired; reports leaks

    ResourceManager(const ResourceManager&) = delete;
    ResourceManager& operator=(const ResourceManager&) = delete;

    // Takes ownership of a freshly created backend object.
    template <class T>
    Ref<T> track(std::unique_ptr<T> object) {
        if (!object) return nullptr;
        T* raw = object.release();
        registerLive(raw);
        auto state = state_;
        return Ref<T>(raw, [state](T* p) { ResourceManager::retire(state, p); });
    }

    // Serial of the frame currently being recorded. Objects released now are
    // destroyed once completedSerial >= this value.
    void setRecordingSerial(uint64_t serial);
    // Destroys every retired object whose serial <= completedSerial.
    void collect(uint64_t completedSerial);
    // Destroys every retired object. Caller must guarantee the GPU is idle.
    void collectAll();

    ResourceStats stats() const;
    std::vector<std::string> liveResourceNames() const;

    // Called right before the owning device is destroyed. Any Ref still alive
    // after this point is leaked deliberately (its API objects would reference
    // a destroyed device) and reported as an error.
    void shutdown();

private:
    struct Retired {
        std::unique_ptr<GpuResource> object;
        uint64_t serial;
    };
    struct State {
        mutable std::mutex mutex;
        bool alive = true;
        uint64_t recordingSerial = 0;
        std::vector<Retired> retired;
        std::unordered_set<const GpuResource*> live;
        ResourceStats stats;
    };

    void registerLive(GpuResource* object);
    static void retire(const std::shared_ptr<State>& state, GpuResource* object);
    static void destroyNow(State& state, std::vector<Retired>& batch);

    std::shared_ptr<State> state_;
};

}  // namespace pe::gfx
