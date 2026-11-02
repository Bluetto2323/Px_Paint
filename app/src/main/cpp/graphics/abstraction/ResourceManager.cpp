#include "graphics/abstraction/ResourceManager.h"

#include <algorithm>

#include "core/Log.h"
#include "core/ShutdownReport.h"

namespace pe::gfx {

namespace {
constexpr const char* kTag = "Resources";
}

const char* toString(ResourceKind kind) {
    switch (kind) {
        case ResourceKind::Buffer: return "Buffer";
        case ResourceKind::Texture: return "Texture";
        case ResourceKind::Shader: return "Shader";
        case ResourceKind::Pipeline: return "Pipeline";
        case ResourceKind::RenderTarget: return "RenderTarget";
        case ResourceKind::Count: break;
    }
    return "?";
}

uint32_t ResourceStats::totalLive() const {
    uint32_t n = 0;
    for (uint32_t c : live) n += c;
    return n;
}

ResourceManager::ResourceManager() : state_(std::make_shared<State>()) {}

ResourceManager::~ResourceManager() { shutdown(); }

void ResourceManager::registerLive(GpuResource* object) {
    std::lock_guard<std::mutex> lock(state_->mutex);
    state_->live.insert(object);
    state_->stats.live[static_cast<size_t>(object->kind())]++;
    state_->stats.liveGpuBytes += object->gpuMemoryBytes();
    state_->stats.totalCreated++;
}

void ResourceManager::retire(const std::shared_ptr<State>& state, GpuResource* object) {
    std::lock_guard<std::mutex> lock(state->mutex);
    if (!state->alive) {
        // Device already gone: running the destructor would call into a
        // destroyed VkDevice / EGLContext. Leak on purpose and make it loud.
        PE_LOGE(kTag, "LEAK: %s '%s' released after device shutdown", toString(object->kind()),
                object->debugName().c_str());
        return;
    }
    state->retired.push_back({std::unique_ptr<GpuResource>(object), state->recordingSerial});
    state->stats.pendingDestruction = static_cast<uint32_t>(state->retired.size());
}

void ResourceManager::setRecordingSerial(uint64_t serial) {
    std::lock_guard<std::mutex> lock(state_->mutex);
    state_->recordingSerial = serial;
}

void ResourceManager::destroyNow(State& state, std::vector<Retired>& batch) {
    // Called with state.mutex held for bookkeeping only; API destruction
    // happens in the object destructors when `batch` is cleared by the caller.
    for (auto& r : batch) {
        GpuResource* p = r.object.get();
        state.live.erase(p);
        auto& count = state.stats.live[static_cast<size_t>(p->kind())];
        if (count > 0) count--;
        state.stats.liveGpuBytes -= std::min(state.stats.liveGpuBytes, p->gpuMemoryBytes());
        state.stats.totalDestroyed++;
    }
}

void ResourceManager::collect(uint64_t completedSerial) {
    std::vector<Retired> ready;
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        auto& retired = state_->retired;
        auto it = std::partition(retired.begin(), retired.end(),
                                 [&](const Retired& r) { return r.serial > completedSerial; });
        ready.assign(std::make_move_iterator(it), std::make_move_iterator(retired.end()));
        retired.erase(it, retired.end());
        destroyNow(*state_, ready);
        state_->stats.pendingDestruction = static_cast<uint32_t>(retired.size());
    }
    ready.clear();  // destructors run outside the lock
}

void ResourceManager::collectAll() {
    std::vector<Retired> ready;
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        ready.swap(state_->retired);
        destroyNow(*state_, ready);
        state_->stats.pendingDestruction = 0;
    }
    ready.clear();
}

ResourceStats ResourceManager::stats() const {
    std::lock_guard<std::mutex> lock(state_->mutex);
    return state_->stats;
}

std::vector<std::string> ResourceManager::liveResourceNames() const {
    std::lock_guard<std::mutex> lock(state_->mutex);
    std::vector<std::string> names;
    names.reserve(state_->live.size());
    for (const GpuResource* r : state_->live) {
        // Retired-but-not-destroyed objects are still in `live`; skip them.
        bool retired = false;
        for (const auto& rr : state_->retired)
            if (rr.object.get() == r) retired = true;
        if (!retired) names.push_back(std::string(toString(r->kind())) + ":" + r->debugName());
    }
    return names;
}

void ResourceManager::shutdown() {
    collectAll();
    std::lock_guard<std::mutex> lock(state_->mutex);
    if (!state_->alive) return;
    state_->alive = false;
    shutdown::recordResources(state_->stats.totalCreated, state_->stats.totalDestroyed, state_->live.size());
    if (!state_->live.empty()) {
        PE_LOGE(kTag, "%zu GPU resource(s) still referenced at device shutdown:", state_->live.size());
        for (const GpuResource* r : state_->live)
            PE_LOGE(kTag, "  leaked %s '%s'", toString(r->kind()), r->debugName().c_str());
    } else {
        PE_LOGI(kTag, "Shutdown clean: created=%llu destroyed=%llu",
                static_cast<unsigned long long>(state_->stats.totalCreated),
                static_cast<unsigned long long>(state_->stats.totalDestroyed));
    }
}

}  // namespace pe::gfx
