// Paint Engine - process-wide record of the last graphics-device shutdown.
//
// The device that was destroyed (e.g. when switching backend) no longer
// exists when someone wants to know whether it shut down cleanly, so its
// final bookkeeping is stored here and survives it.
#pragma once

#include <cstdint>
#include <string>

namespace pe::shutdown {

struct Report {
    bool valid = false;           // at least one device has shut down
    std::string backend;          // "Vulkan" / "OpenGL ES"
    uint64_t resourcesCreated = 0;
    uint64_t resourcesDestroyed = 0;
    uint64_t resourcesLeaked = 0; // still referenced at device shutdown
    uint64_t memoryAllocationsLeaked = 0;  // Vulkan VkDeviceMemory
    uint32_t devicesShutDown = 0; // total in this process
    bool clean() const { return resourcesLeaked == 0 && memoryAllocationsLeaked == 0; }
};

void recordResources(uint64_t created, uint64_t destroyed, uint64_t leaked);
void recordMemoryLeaks(uint64_t leakedAllocations);
// Called last by the device destructor: finalises the record.
void recordDeviceDestroyed(const char* backend);

Report last();
std::string describe(const Report& r);

}  // namespace pe::shutdown
