#include "core/ShutdownReport.h"

#include <mutex>

namespace pe::shutdown {
namespace {
std::mutex gMutex;
Report gPending;  // filled while a device is being destroyed
Report gLast;
uint32_t gCount = 0;
}  // namespace

void recordResources(uint64_t created, uint64_t destroyed, uint64_t leaked) {
    std::lock_guard<std::mutex> lock(gMutex);
    gPending.resourcesCreated = created;
    gPending.resourcesDestroyed = destroyed;
    gPending.resourcesLeaked = leaked;
}

void recordMemoryLeaks(uint64_t leakedAllocations) {
    std::lock_guard<std::mutex> lock(gMutex);
    gPending.memoryAllocationsLeaked = leakedAllocations;
}

void recordDeviceDestroyed(const char* backend) {
    std::lock_guard<std::mutex> lock(gMutex);
    gPending.valid = true;
    gPending.backend = backend ? backend : "?";
    gPending.devicesShutDown = ++gCount;
    gLast = gPending;
    gPending = Report{};
}

Report last() {
    std::lock_guard<std::mutex> lock(gMutex);
    return gLast;
}

std::string describe(const Report& r) {
    if (!r.valid) return "no device shut down yet";
    return r.backend + ": " + (r.clean() ? "clean" : "LEAKS") + " (resources created " +
           std::to_string(r.resourcesCreated) + ", destroyed " + std::to_string(r.resourcesDestroyed) +
           ", leaked " + std::to_string(r.resourcesLeaked) + ", leaked memory allocations " +
           std::to_string(r.memoryAllocationsLeaked) + "; device #" + std::to_string(r.devicesShutDown) + ")";
}

}  // namespace pe::shutdown
