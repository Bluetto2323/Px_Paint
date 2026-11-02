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
    char buf[200];
    const auto& f = s.frames;
    o << "State     " << s.state << (s.paused ? " (paused)" : "") << "\n";
    o << "Backend   " << gfx::toString(s.backend);
    if (s.deviceRecoveries) o << "  recoveries:" << s.deviceRecoveries;
    o << "\n";
    o << "GPU       " << s.gpu << "\n";
    o << "API       " << s.api << "\n";
    o << "Driver    " << s.driver << "\n";
    if (s.surface.attached) {
        std::snprintf(buf, sizeof(buf), "Surface   %ux%u %s  img:%u %s  rebuilt:%u (last %.2f ms)\n",
                      s.surface.extent.width, s.surface.extent.height, gfx::toString(s.surface.format),
                      s.surface.imageCount, s.surface.presentMode.c_str(), s.surface.recreateCount,
                      s.surface.lastRebuildMs);
        o << buf;
        o << "Transform " << s.surface.transform << " / display " << s.surface.currentTransform << "\n";
    } else {
        o << "Surface   none\n";
    }
    o << "Mode      render:" << gfx::toString(s.renderMode) << "  diagnostics:" << (s.diagnostics ? "on" : "off")
      << "\n";
    std::snprintf(buf, sizeof(buf), "Startup   backend init %.1f ms, first frame %.1f ms after attach\n",
                  s.backendInitMs, f.firstFrameMs);
    o << buf;
    std::snprintf(buf, sizeof(buf), "Frames    %llu rendered, %llu skipped", static_cast<unsigned long long>(f.framesPresented),
                  static_cast<unsigned long long>(f.framesSkipped));
    o << buf;
    if (f.framesSkipped) {
        std::snprintf(buf, sizeof(buf), " (no-surface %llu, out-of-date %llu, surface-lost %llu, device-lost %llu, error %llu)",
                      static_cast<unsigned long long>(f.skippedNoSurface),
                      static_cast<unsigned long long>(f.skippedOutOfDate),
                      static_cast<unsigned long long>(f.skippedSurfaceLost),
                      static_cast<unsigned long long>(f.skippedDeviceLost),
                      static_cast<unsigned long long>(f.skippedError));
        o << buf;
    }
    o << "\n";
    if (f.steadyFrames) {
        std::snprintf(buf, sizeof(buf), "CPU/frame steady avg %.2f ms, max(2s) %.2f ms, last %.2f ms\n",
                      f.steadyAvgCpuMs, f.steadyMaxCpuMs, f.lastCpuMs);
    } else {
        std::snprintf(buf, sizeof(buf), "CPU/frame warming up (last %.2f ms)\n", f.lastCpuMs);
    }
    o << buf;
    if (s.renderMode == gfx::RenderMode::OnDemand) {
        o << "Rate      n/a: on-demand (idle between requested frames)\n";
    } else if (f.rateValid) {
        std::snprintf(buf, sizeof(buf), "Rate      %.1f fps, %.2f ms between frames\n", f.fps, f.avgIntervalMs);
        o << buf;
    } else {
        o << "Rate      measuring...\n";
    }
    std::snprintf(buf, sizeof(buf), "GPU res.  %u live (%s), %u pending\n", s.resources.totalLive(),
                  mib(s.resources.liveGpuBytes).c_str(), s.resources.pendingDestruction);
    o << buf;
    o << "SelfTest  " << toString(s.selfTest.status);
    if (!s.selfTest.detail.empty()) o << " - " << s.selfTest.detail;
    o << "\n";
    o << "Engine    " << s.errors << " errors, " << s.warnings << " warnings\n";
    o << "Valid.lyr " << (s.validationLayerActive ? "active" : "not installed (optional)");
    if (s.validationLayerActive || s.validationErrors || s.validationWarnings)
        o << ": " << s.validationErrors << " errors, " << s.validationWarnings << " warnings";
    o << "\n";
    if (s.driverDebugMessages) o << "Driver dbg " << s.driverDebugMessages << " messages (informational)\n";
    for (const auto& p : s.recentProblems) o << "  " << p << "\n";
    return o.str();
}

namespace {
std::string jsonEscape(const std::string& in) {
    std::string out;
    out.reserve(in.size() + 8);
    for (char c : in) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char b[8];
                    std::snprintf(b, sizeof(b), "\\u%04x", c);
                    out += b;
                } else {
                    out += c;
                }
        }
    }
    return out;
}
}  // namespace

std::string statusJson(const EngineStatus& s) {
    std::ostringstream o;
    const auto& f = s.frames;
    auto str = [&](const char* k, const std::string& v, bool comma = true) {
        o << '"' << k << "\":\"" << jsonEscape(v) << '"' << (comma ? "," : "");
    };
    auto num = [&](const char* k, double v) { o << '"' << k << "\":" << v << ','; };
    auto boolean = [&](const char* k, bool v) { o << '"' << k << "\":" << (v ? "true" : "false") << ','; };
    o << '{';
    str("state", s.state);
    str("backend", gfx::toString(s.backend));
    str("gpu", s.gpu);
    str("api", s.api);
    str("driver", s.driver);
    boolean("paused", s.paused);
    str("renderMode", gfx::toString(s.renderMode));
    boolean("diagnostics", s.diagnostics);
    boolean("surfaceAttached", s.surface.attached);
    num("surfaceWidth", s.surface.extent.width);
    num("surfaceHeight", s.surface.extent.height);
    num("surfaceRebuilds", s.surface.recreateCount);
    num("lastRebuildMs", s.surface.lastRebuildMs);
    str("transform", s.surface.transform);
    str("currentTransform", s.surface.currentTransform);
    num("backendInitMs", s.backendInitMs);
    num("firstFrameMs", f.firstFrameMs);
    num("framesRendered", static_cast<double>(f.framesPresented));
    num("framesSkipped", static_cast<double>(f.framesSkipped));
    num("skippedOutOfDate", static_cast<double>(f.skippedOutOfDate));
    num("skippedError", static_cast<double>(f.skippedError + f.skippedDeviceLost + f.skippedSurfaceLost));
    num("steadyFrames", static_cast<double>(f.steadyFrames));
    num("steadyAvgCpuMs", f.steadyAvgCpuMs);
    num("steadyMaxCpuMs", f.steadyMaxCpuMs);
    boolean("rateValid", f.rateValid);
    num("fps", f.fps);
    num("liveResources", s.resources.totalLive());
    num("pendingDestruction", s.resources.pendingDestruction);
    num("deviceRecoveries", s.deviceRecoveries);
    str("selfTest", toString(s.selfTest.status));
    num("selfTestRuns", s.selfTest.runs);
    str("selfTestDetail", s.selfTest.detail);
    num("engineErrors", static_cast<double>(s.errors));
    num("engineWarnings", static_cast<double>(s.warnings));
    boolean("validationLayerActive", s.validationLayerActive);
    num("validationErrors", static_cast<double>(s.validationErrors));
    num("validationWarnings", static_cast<double>(s.validationWarnings));
    o << "\"driverDebugMessages\":" << s.driverDebugMessages;
    o << '}';
    return o.str();
}

void logMultiline(const char* tag, const std::string& text) {
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) PE_LOGI(tag, "%s", line.c_str());
}

}  // namespace pe::diagnostics
