#include "graphics/opengles/GLESDevice.h"

#include <cstring>

#include "core/Log.h"
#include "core/ShutdownReport.h"
#include "core/TestHooks.h"
#include "core/Time.h"
#include "graphics/opengles/GLESBackend.h"
#include "platform/PlatformWindow.h"

namespace pe::gles {

namespace {
constexpr const char* kTag = "GLES";

void GL_APIENTRY debugCallback(GLenum source, GLenum type, GLuint id, GLenum severity, GLsizei, const GLchar* message,
                               const void*) {
    (void)source;
    if (severity == GL_DEBUG_SEVERITY_NOTIFICATION_KHR) return;
    const bool error = type == GL_DEBUG_TYPE_ERROR_KHR || severity == GL_DEBUG_SEVERITY_HIGH_KHR;
    // Driver debug output is counted separately from engine errors.
    log::writeFrom(LogSource::DriverDebug, error ? LogLevel::Error : LogLevel::Warn, "GLES.debug", "[id %u] %s", id,
                   message);
}
}  // namespace

GLESDevice::GLESDevice(GLESBackend& backend) : backend_(backend), display_(backend.display()) {}

// Destruction order: window EGLSurface (not current) -> GL objects (context
// current on the idle surface) -> unbind -> idle pbuffer -> context ->
// per-thread EGL state. The EGLDisplay belongs to the backend.
GLESDevice::~GLESDevice() {
    if (context_ != EGL_NO_CONTEXT) {
        destroyWindowSurface();
        if (!makeIdleCurrent())
            PE_LOGW(kTag, "eglMakeCurrent(idle) failed at shutdown: %s", eglErrorString(eglGetError()));
        // Deferred GL deletions need the context current. On a lost context
        // the deletes are no-ops, which is harmless.
        resources_.shutdown();
        eglMakeCurrent(display_, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (idleSurface_ != EGL_NO_SURFACE) eglDestroySurface(display_, idleSurface_);
        eglDestroyContext(display_, context_);
        PE_LOGI(kTag, "EGL context destroyed%s", contextLost_ ? " (was lost)" : "");
        shutdown::recordDeviceDestroyed("OpenGL ES");
    }
    // Frees the render thread's EGL bookkeeping; the context is no longer
    // current, so this does not affect any other thread.
    eglReleaseThread();
}

bool GLESDevice::initialize(std::string& error) {
    const EGLConfig config = backend_.config();
    const bool createContextKHR = backend_.hasEglExtension("EGL_KHR_create_context");

    // Try the richest context first, then drop optional attributes one by
    // one: debug (validation builds) and reset notification (robustness).
    const bool wantDebug = backend_.validation() && createContextKHR;
    const bool wantRobust = backend_.hasEglExtension("EGL_EXT_create_context_robustness");
    const bool combos[4][2] = {{wantDebug, wantRobust}, {false, wantRobust}, {wantDebug, false}, {false, false}};
    for (int i = 0; i < 4 && context_ == EGL_NO_CONTEXT; ++i) {
        const bool debug = combos[i][0], robust = combos[i][1];
        bool duplicate = false;
        for (int j = 0; j < i; ++j) duplicate |= combos[j][0] == debug && combos[j][1] == robust;
        if (duplicate) continue;
        EGLint attribs[9];
        int n = 0;
        attribs[n++] = EGL_CONTEXT_CLIENT_VERSION;  // == EGL_CONTEXT_MAJOR_VERSION_KHR
        attribs[n++] = 3;
        if (debug) {
            attribs[n++] = EGL_CONTEXT_FLAGS_KHR;
            attribs[n++] = EGL_CONTEXT_OPENGL_DEBUG_BIT_KHR;
        }
        if (robust) {
            attribs[n++] = EGL_CONTEXT_OPENGL_RESET_NOTIFICATION_STRATEGY_EXT;
            attribs[n++] = EGL_LOSE_CONTEXT_ON_RESET_EXT;
        }
        attribs[n++] = EGL_NONE;
        context_ = eglCreateContext(display_, config, EGL_NO_CONTEXT, attribs);
        if (context_ != EGL_NO_CONTEXT) {
            debugContext_ = debug;
            robustContext_ = robust;
        } else if (debug || robust) {
            PE_LOGW(kTag, "eglCreateContext(debug=%d robust=%d) refused: %s; retrying with fewer attributes", debug,
                    robust, eglErrorString(eglGetError()));
        }
    }
    if (context_ == EGL_NO_CONTEXT) {
        error = std::string("eglCreateContext(ES 3) failed: ") + eglErrorString(eglGetError());
        return false;
    }

    surfaceless_ = backend_.hasEglExtension("EGL_KHR_surfaceless_context");
    if (!surfaceless_) {
        if (!backend_.configHasPbuffer()) {
            error = "neither EGL_KHR_surfaceless_context nor pbuffer-capable config";
            return false;
        }
        const EGLint pb[] = {EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE};
        idleSurface_ = eglCreatePbufferSurface(display_, config, pb);
        if (idleSurface_ == EGL_NO_SURFACE) {
            error = std::string("idle pbuffer creation failed: ") + eglErrorString(eglGetError());
            return false;
        }
    }
    if (!makeIdleCurrent()) {
        error = std::string("eglMakeCurrent failed: ") + eglErrorString(eglGetError());
        return false;
    }

    GLint major = 0, minor = 0;
    glGetIntegerv(GL_MAJOR_VERSION, &major);
    glGetIntegerv(GL_MINOR_VERSION, &minor);
    if (major < 3) {
        error = "context is OpenGL ES " + std::to_string(major) + "." + std::to_string(minor) + " (< 3.0)";
        return false;
    }

    queryCapabilities();
    installDebugCallback();
    resolveRobustness();
    drainGLErrors("GLESDevice::initialize");
    PE_LOGI(kTag, "Device ready: %s / %s, ES %d.%d%s%s", caps_.vendorName.c_str(), caps_.deviceName.c_str(), major,
            minor, debugContext_ ? " (debug)" : "", robustContext_ ? " (reset notification)" : "");
    return true;
}

void GLESDevice::resolveRobustness() {
    // Only trust the entry point when the extension is advertised.
    if (caps_.hasDeviceExtension("GL_KHR_robustness"))
        ext_.getGraphicsResetStatus =
            reinterpret_cast<PFNGLGETGRAPHICSRESETSTATUSKHRPROC>(eglGetProcAddress("glGetGraphicsResetStatusKHR"));
    else if (caps_.hasDeviceExtension("GL_EXT_robustness"))
        ext_.getGraphicsResetStatus =
            reinterpret_cast<PFNGLGETGRAPHICSRESETSTATUSKHRPROC>(eglGetProcAddress("glGetGraphicsResetStatusEXT"));
    caps_.addFeature("Context reset detection",
                     robustContext_ && ext_.getGraphicsResetStatus != nullptr,
                     robustContext_ ? "LOSE_CONTEXT_ON_RESET" : "EGL_CONTEXT_LOST only");
}

bool GLESDevice::checkContextLost(const char* where) {
    if (contextLost_) return true;
    if (robustContext_ && ext_.getGraphicsResetStatus) {
        const GLenum status = ext_.getGraphicsResetStatus();
        if (status != GL_NO_ERROR) {
            PE_LOGE(kTag, "GPU reset detected at %s (status 0x%04x): context lost", where, status);
            contextLost_ = true;
        }
    }
    return contextLost_;
}

bool GLESDevice::makeIdleCurrent() {
    return eglMakeCurrent(display_, idleSurface_, idleSurface_, context_) == EGL_TRUE;
}

void GLESDevice::installDebugCallback() {
    ext_ = GLExtFunctions{};
    ext_.pushDebugGroup = reinterpret_cast<PFNGLPUSHDEBUGGROUPKHRPROC>(eglGetProcAddress("glPushDebugGroupKHR"));
    ext_.popDebugGroup = reinterpret_cast<PFNGLPOPDEBUGGROUPKHRPROC>(eglGetProcAddress("glPopDebugGroupKHR"));
    ext_.objectLabel = reinterpret_cast<PFNGLOBJECTLABELKHRPROC>(eglGetProcAddress("glObjectLabelKHR"));
    ext_.debugMessageCallback =
        reinterpret_cast<PFNGLDEBUGMESSAGECALLBACKKHRPROC>(eglGetProcAddress("glDebugMessageCallbackKHR"));
    if (!caps_.hasDeviceExtension("GL_KHR_debug")) {
        // eglGetProcAddress may return non-null stubs for unsupported names.
        ext_ = GLExtFunctions{};
        return;
    }
    if (backend_.validation() && ext_.debugMessageCallback) {
        glEnable(GL_DEBUG_OUTPUT_KHR);
        glEnable(GL_DEBUG_OUTPUT_SYNCHRONOUS_KHR);
        ext_.debugMessageCallback(&debugCallback, nullptr);
        PE_LOGI(kTag, "KHR_debug message callback installed");
    }
}

// ------------------------------------------------------------------ surface
bool GLESDevice::attachSurface(PlatformWindow& window) {
    if (windowSurface_ != EGL_NO_SURFACE) detachSurface();
    window_ = &window;
    requestedExtent_ = {window.width(), window.height()};
    if (!createWindowSurface()) {
        window_ = nullptr;
        return false;
    }
    PE_LOGI(kTag, "Surface attached (%s) %ux%u", toString(window.kind()), backbuffer_.extent().width,
            backbuffer_.extent().height);
    return true;
}

bool GLESDevice::createWindowSurface() {
    const int64_t t0 = nowNs();
    const EGLConfig config = backend_.config();
    if (window_->kind() == WindowKind::Headless) {
        const EGLint pb[] = {EGL_WIDTH, static_cast<EGLint>(requestedExtent_.width), EGL_HEIGHT,
                             static_cast<EGLint>(requestedExtent_.height), EGL_NONE};
        windowSurface_ = eglCreatePbufferSurface(display_, config, pb);
    } else {
        windowSurface_ = eglCreateWindowSurface(
            display_, config, reinterpret_cast<EGLNativeWindowType>(window_->nativeHandle()), nullptr);
    }
    if (windowSurface_ == EGL_NO_SURFACE) {
        PE_LOGE(kTag, "EGL surface creation failed: %s", eglErrorString(eglGetError()));
        return false;
    }
    if (!eglMakeCurrent(display_, windowSurface_, windowSurface_, context_)) {
        PE_LOGE(kTag, "eglMakeCurrent(window) failed: %s", eglErrorString(eglGetError()));
        eglDestroySurface(display_, windowSurface_);
        windowSurface_ = EGL_NO_SURFACE;
        makeIdleCurrent();
        return false;
    }
    if (!eglSwapInterval(display_, swapInterval_))
        PE_LOGW(kTag, "eglSwapInterval(%d) failed: %s", swapInterval_, eglErrorString(eglGetError()));
    backbuffer_.setExtent(querySurfaceExtent());
    lastRebuildMs_ = nsToMs(nowNs() - t0);
    return true;
}

void GLESDevice::destroyWindowSurface() {
    if (windowSurface_ == EGL_NO_SURFACE) return;
    // All GL work targeting the window must be finished before the EGLSurface
    // (and then the ANativeWindow) goes away. A lost context executes nothing.
    if (!contextLost_) glFinish();
    // The surface must not be current when destroyed (EGL would only defer
    // the destruction until it is released).
    if (!makeIdleCurrent()) PE_LOGW(kTag, "eglMakeCurrent(idle) failed: %s", eglErrorString(eglGetError()));
    if (!eglDestroySurface(display_, windowSurface_))
        PE_LOGW(kTag, "eglDestroySurface failed: %s", eglErrorString(eglGetError()));
    windowSurface_ = EGL_NO_SURFACE;
}

void GLESDevice::detachSurface() {
    if (windowSurface_ == EGL_NO_SURFACE && !window_) return;
    inFrame_ = false;  // a frame abandoned mid-way (exception) is simply dropped
    captureRequested_ = false;
    destroyWindowSurface();
    window_ = nullptr;
    backbuffer_.setExtent({});
    PE_LOGI(kTag, "Surface detached");
}

void GLESDevice::notifySurfaceResized(gfx::Extent2D extent) {
    requestedExtent_ = extent;
    // Android window surfaces follow the ANativeWindow size automatically;
    // pbuffers have a fixed size and must be rebuilt.
    if (window_ && window_->kind() == WindowKind::Headless) recreateSurface_ = true;
}

gfx::Extent2D GLESDevice::querySurfaceExtent() const {
    EGLint w = 0, h = 0;
    eglQuerySurface(display_, windowSurface_, EGL_WIDTH, &w);
    eglQuerySurface(display_, windowSurface_, EGL_HEIGHT, &h);
    return {static_cast<uint32_t>(w > 0 ? w : 0), static_cast<uint32_t>(h > 0 ? h : 0)};
}

gfx::SurfaceState GLESDevice::surfaceState() const {
    gfx::SurfaceState s;
    s.attached = windowSurface_ != EGL_NO_SURFACE;
    s.extent = backbuffer_.extent();
    s.format = gfx::TextureFormat::RGBA8_UNORM;
    s.imageCount = 0;  // not exposed by EGL
    s.presentMode = "swapInterval=" + std::to_string(swapInterval_);
    s.transform = "n/a (EGL)";
    s.currentTransform = "handled by compositor";
    s.recreateCount = recreateCount_;
    s.lastRebuildMs = lastRebuildMs_;
    s.captureSupported = s.attached;
    return s;
}

// ---------------------------------------------------------------- resources
gfx::Ref<gfx::BufferResource> GLESDevice::createBuffer(const gfx::BufferDesc& desc, const void* data) {
    return resources_.track(GLESBuffer::create(desc, data));
}

gfx::Ref<gfx::TextureResource> GLESDevice::createTexture(const gfx::TextureDesc& desc) {
    const auto& fs = caps_.format(desc.format);
    if (!fs.sampled && !fs.colorAttachment && !fs.depthAttachment) {
        PE_LOGE(kTag, "Texture '%s': format %s not supported on this device", desc.debugName.c_str(),
                gfx::toString(desc.format));
        return nullptr;
    }
    return resources_.track(GLESTexture::create(desc));
}

gfx::Ref<gfx::ShaderResource> GLESDevice::createShader(const gfx::ShaderDesc& desc) {
    return resources_.track(GLESShader::create(desc));
}

gfx::Ref<gfx::PipelineResource> GLESDevice::createPipeline(const gfx::PipelineDesc& desc) {
    return resources_.track(GLESPipeline::create(desc));
}

// ------------------------------------------------------------------- frames
gfx::FrameStatus GLESDevice::beginFrame(uint64_t serial, gfx::FrameContext& out) {
    if (windowSurface_ == EGL_NO_SURFACE) return gfx::FrameStatus::NoSurface;
    if (contextLost_) return gfx::FrameStatus::DeviceLost;  // never touch a lost context
    inFrame_ = false;  // previous frame abandoned without endFrame(): nothing to undo in GL

    if (recreateSurface_) {
        recreateSurface_ = false;
        (void)PE_TEST_HOOK("surface.rebuild");  // timing hook only
        destroyWindowSurface();
        if (!createWindowSurface()) return gfx::FrameStatus::SurfaceLost;
        recreateCount_++;
    }
    const gfx::Extent2D extent = querySurfaceExtent();
    if (extent != backbuffer_.extent()) {
        backbuffer_.setExtent(extent);
        recreateCount_++;
    }
    if (extent.empty()) return gfx::FrameStatus::SurfaceOutOfDate;

    frameSerial_ = serial;
    resources_.setRecordingSerial(serial);
    context2d_.beginFrame();
    inFrame_ = true;
    out.context = &context2d_;
    out.backbuffer = &backbuffer_;
    out.serial = serial;
    return gfx::FrameStatus::Ok;
}

gfx::FrameStatus GLESDevice::endFrame() {
    if (!inFrame_) return gfx::FrameStatus::Error;
    inFrame_ = false;

    if (captureRequested_) {
        captureRequested_ = false;
        const auto e = backbuffer_.extent();
        std::vector<uint8_t> raw(static_cast<size_t>(e.width) * e.height * 4);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glReadPixels(0, 0, static_cast<GLsizei>(e.width), static_cast<GLsizei>(e.height), GL_RGBA,
                     GL_UNSIGNED_BYTE, raw.data());
        if (drainGLErrors("capture glReadPixels") == 0) {
            // GL origin is bottom-left: flip to top-left.
            capture_.width = e.width;
            capture_.height = e.height;
            capture_.rgba.resize(raw.size());
            const size_t row = static_cast<size_t>(e.width) * 4;
            for (uint32_t y = 0; y < e.height; ++y)
                std::memcpy(&capture_.rgba[y * row], &raw[(e.height - 1 - y) * row], row);
            captureReady_ = true;
        }
    }

    // glGetError can force a pipeline sync on some drivers: check every frame
    // only with validation on, otherwise periodically.
    if (backend_.validation() || ++framesSinceErrorCheck_ >= 120) {
        framesSinceErrorCheck_ = 0;
        drainGLErrors("frame");
    }

    gfx::FrameStatus status = gfx::FrameStatus::Ok;
    if (!eglSwapBuffers(display_, windowSurface_)) {
        const EGLint err = eglGetError();
        PE_LOGE(kTag, "eglSwapBuffers failed: %s", eglErrorString(err));
        if (err == EGL_CONTEXT_LOST) {
            // Every GL object is gone: the Engine recreates the device and the
            // scene recreates its resources lazily on the next frame.
            contextLost_ = true;
            status = gfx::FrameStatus::DeviceLost;
        } else if (err == EGL_BAD_SURFACE || err == EGL_BAD_NATIVE_WINDOW) {
            status = gfx::FrameStatus::SurfaceLost;
        } else {
            status = gfx::FrameStatus::Error;
        }
    }
    if (status == gfx::FrameStatus::Ok && checkContextLost("endFrame")) status = gfx::FrameStatus::DeviceLost;
    // GL keeps objects alive while queued commands reference them, so retired
    // resources can be released as soon as the frame was submitted.
    resources_.collect(frameSerial_);
    return status;
}

void GLESDevice::waitIdle() { glFinish(); }

bool GLESDevice::requestCapture() {
    if (windowSurface_ == EGL_NO_SURFACE) return false;
    captureRequested_ = true;
    captureReady_ = false;
    return true;
}

bool GLESDevice::takeCapture(gfx::CaptureResult& out) {
    if (!captureReady_) return false;
    out = std::move(capture_);
    capture_ = {};
    captureReady_ = false;
    return true;
}

}  // namespace pe::gles
