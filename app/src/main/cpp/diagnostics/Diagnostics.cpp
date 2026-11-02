#include "diagnostics/Diagnostics.h"

#include <cstdio>
#include <sstream>

#include "core/Log.h"

namespace pe::diagnostics {

namespace {
std::string mib(uint64_t bytes) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.0f MiB", static_cast<double>(bytes) / (1024.0 * 1024.0));
    return buf;
}

const char* yn(bool b) { return b ? "yes" : "no"; }

void listBlock(std::ostringstream& o, const char* title, const std::vector<std::string>& items) {
    o << title << " (" << items.size() << "):\n";
    for (const auto& i : items) o << "    " << i << "\n";
}
}  // namespace

std::string capabilityReport(const gfx::GraphicsCapabilities& c, const std::string& backendDescription,
                             const std::vector<gfx::BackendAttempt>& attempts) {
    std::ostringstream o;
    o << "================ PAINT ENGINE GPU REPORT ================\n";
    o << "Backend selection:\n";
    for (const auto& a : attempts) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%.1f ms", a.durationMs);
        o << "  - " << gfx::toString(a.type) << ": "
          << (a.success ? "SELECTED" : "rejected at '" + a.failedStage + "': " + a.reason) << " (" << buf << ")\n";
    }
    o << "Backend          : " << gfx::toString(c.backend) << " - " << backendDescription << "\n";
    o << "GPU              : " << c.deviceName << "\n";
    o << "Vendor           : " << c.vendorName;
    if (c.vendorId) {
        char buf[48];
        std::snprintf(buf, sizeof(buf), " (vendor 0x%04X, device 0x%08X)", c.vendorId, c.deviceId);
        o << buf;
    }
    o << "\nDevice type      : " << c.deviceType << "\n";
    o << "API version      : " << c.apiVersion() << "\n";
    o << "Instance/display : " << c.instanceVersionString << "\n";
    o << "Driver version   : " << c.driverVersion << "\n";
    if (!c.driverInfo.empty() && c.driverInfo != c.driverVersion) o << "Driver info      : " << c.driverInfo << "\n";
    o << "Shading language : " << c.shadingLanguage << "\n";

    const auto& l = c.limits;
    o << "Limits:\n";
    o << "    max texture 2D        " << l.maxTextureSize2D << "\n";
    o << "    max framebuffer       " << l.maxFramebufferWidth << "x" << l.maxFramebufferHeight << "\n";
    o << "    max color attachments " << l.maxColorAttachments << "\n";
    o << "    max vertex attributes " << l.maxVertexAttributes << "\n";
    o << "    max uniform range     " << l.maxUniformBufferRange << "\n";
    o << "    max array layers      " << l.maxArrayTextureLayers << "\n";
    o << "    max MSAA samples      " << l.maxSamples << "\n";
    o << "    max anisotropy        " << l.maxSamplerAnisotropy << "\n";
    o << "    compute invocations   " << l.maxComputeWorkGroupInvocations << "\n";
    if (c.backend == gfx::BackendType::Vulkan) {
        o << "    push constants        " << l.maxPushConstantsSize << " B\n";
        o << "    memory allocations    " << l.maxMemoryAllocationCount << "\n";
        o << "    timestamp period      " << l.timestampPeriodNs << " ns\n";
    }
    o << "Memory:\n";
    if (c.memory.known) {
        o << "    device-local heaps    " << mib(c.memory.deviceLocalBytes) << "\n";
        o << "    host-visible heaps    " << mib(c.memory.hostVisibleBytes) << "\n";
        o << "    unified memory        " << yn(c.memory.unifiedMemory) << "\n";
    } else {
        o << "    not queryable on this backend\n";
    }
    if (c.subgroup.queried) {
        o << "Subgroups        : size " << c.subgroup.size << ", stages " << c.subgroup.supportedStages << ", ops "
          << c.subgroup.supportedOperations << "\n";
    }
    o << "Texture formats (sampled/filter/render/blend/storage/depth):\n";
    for (size_t i = 1; i < static_cast<size_t>(gfx::TextureFormat::Count); ++i) {
        const auto f = static_cast<gfx::TextureFormat>(i);
        const auto& s = c.format(f);
        char buf[96];
        std::snprintf(buf, sizeof(buf), "    %-18s %c %c %c %c %c %c\n", gfx::toString(f), s.sampled ? 'S' : '-',
                      s.linearFilter ? 'F' : '-', s.colorAttachment ? 'R' : '-', s.blend ? 'B' : '-',
                      s.storage ? 'W' : '-', s.depthAttachment ? 'D' : '-');
        o << buf;
    }
    o << "Usable features:\n";
    for (const auto& f : c.features) {
        o << "    [" << (f.usable ? "x" : " ") << "] " << f.name;
        if (!f.detail.empty()) o << " (" << f.detail << ")";
        o << "\n";
    }
    listBlock(o, "Enabled extensions", c.enabledExtensions);
    if (!c.layers.empty()) listBlock(o, "Available layers", c.layers);
    listBlock(o, c.backend == gfx::BackendType::Vulkan ? "Instance extensions" : "EGL extensions",
              c.instanceExtensions);
    listBlock(o, c.backend == gfx::BackendType::Vulkan ? "Device extensions" : "GL extensions", c.deviceExtensions);
    if (!c.warnings.empty()) listBlock(o, "Warnings", c.warnings);
    o << "=========================================================";
    return o.str();
}

std::string statusSummary(const EngineStatus& s) {
    std::ostringstream o;
    char buf[160];
    o << "State     " << s.state << (s.paused ? " (paused)" : "") << "\n";
    o << "Backend   " << gfx::toString(s.backend);
    if (s.deviceRecoveries) o << "  recoveries:" << s.deviceRecoveries;
    o << "\n";
    o << "GPU       " << s.gpu << "\n";
    o << "API       " << s.api << "\n";
    o << "Driver    " << s.driver << "\n";
    if (s.surface.attached) {
        std::snprintf(buf, sizeof(buf), "Surface   %ux%u %s  img:%u %s  rebuilt:%u\n", s.surface.extent.width,
                      s.surface.extent.height, gfx::toString(s.surface.format), s.surface.imageCount,
                      s.surface.presentMode.c_str(), s.surface.recreateCount);
        o << buf;
        o << "Transform " << s.surface.transform << "  window:" << s.windowKind << "\n";
    } else {
        o << "Surface   none\n";
    }
    std::snprintf(buf, sizeof(buf), "Frames    %llu shown, %llu skipped\n",
                  static_cast<unsigned long long>(s.frames.framesPresented),
                  static_cast<unsigned long long>(s.frames.framesSkipped));
    o << buf;
    std::snprintf(buf, sizeof(buf), "Mode      render:%s  diagnostics:%s\n", gfx::toString(s.renderMode),
                  s.diagnostics ? "on" : "off");
    o << buf;
    std::snprintf(buf, sizeof(buf), "CPU frame %.2f ms (avg %.2f, max2s %.2f)\n", s.frames.lastCpuMs, s.frames.avgCpuMs,
                  s.frames.maxCpuMsWindow);
    o << buf;
    std::snprintf(buf, sizeof(buf), "Interval  %.2f ms avg  %.0f fps\n", s.frames.avgIntervalMs, s.frames.fps);
    o << buf;
    std::snprintf(buf, sizeof(buf), "GPU res.  %u live (%s), %u pending\n", s.resources.totalLive(),
                  mib(s.resources.liveGpuBytes).c_str(), s.resources.pendingDestruction);
    o << buf;
    o << "SelfTest  " << toString(s.selfTest.status);
    if (!s.selfTest.detail.empty()) o << " - " << s.selfTest.detail;
    o << "\n";
    o << "Log       " << s.warnings << " warnings, " << s.errors << " errors\n";
    for (const auto& p : s.recentProblems) o << "  " << p << "\n";
    return o.str();
}

void logMultiline(const char* tag, const std::string& text) {
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) PE_LOGI(tag, "%s", line.c_str());
}

}  // namespace pe::diagnostics
