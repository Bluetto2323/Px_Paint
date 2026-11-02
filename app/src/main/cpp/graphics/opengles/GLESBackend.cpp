#include "graphics/opengles/GLESBackend.h"

#include <sstream>

#include "core/Log.h"
#include "graphics/opengles/GLESDevice.h"

namespace pe::gles {

namespace {
constexpr const char* kTag = "GLES";

std::vector<std::string> splitExtensions(const char* s) {
    std::vector<std::string> out;
    if (!s) return out;
    std::istringstream in(s);
    std::string ext;
    while (in >> ext) out.push_back(ext);
    return out;
}
}  // namespace

std::unique_ptr<gfx::GraphicsBackend> createGLESBackend() { return std::make_unique<GLESBackend>(); }

GLESBackend::~GLESBackend() {
#if !defined(__ANDROID__)
    // On Android the EGLDisplay is shared with the UI toolkit (HWUI) in the
    // same process; it is intentionally never terminated by the engine.
    if (initialized_) eglTerminate(display_);
#endif
}

bool GLESBackend::hasEglExtension(const char* name) const {
    for (const auto& e : eglExtensions_)
        if (e == name) return true;
    return false;
}

std::string GLESBackend::eglVersion() const {
    return std::to_string(eglMajor_) + "." + std::to_string(eglMinor_);
}

bool GLESBackend::initialize(const gfx::BackendInitOptions& options, std::string& error) {
    options_ = options;
    display_ = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (display_ == EGL_NO_DISPLAY) {
        error = "eglGetDisplay returned EGL_NO_DISPLAY";
        return false;
    }
    if (!eglInitialize(display_, &eglMajor_, &eglMinor_)) {
        error = std::string("eglInitialize failed: ") + eglErrorString(eglGetError());
        display_ = EGL_NO_DISPLAY;
        return false;
    }
    initialized_ = true;
    const char* vendor = eglQueryString(display_, EGL_VENDOR);
    eglVendor_ = vendor ? vendor : "?";
    eglExtensions_ = splitExtensions(eglQueryString(display_, EGL_EXTENSIONS));
    PE_LOGI(kTag, "EGL %d.%d vendor='%s' extensions=%zu", eglMajor_, eglMinor_, eglVendor_.c_str(),
            eglExtensions_.size());

    if (!eglBindAPI(EGL_OPENGL_ES_API)) {
        error = std::string("eglBindAPI(GLES) failed: ") + eglErrorString(eglGetError());
        return false;
    }
    return chooseConfig(error);
}

bool GLESBackend::chooseConfig(std::string& error) {
    const bool es3Bit = (eglMajor_ > 1 || (eglMajor_ == 1 && eglMinor_ >= 5)) ||
                        hasEglExtension("EGL_KHR_create_context");
    if (!es3Bit) PE_LOGW(kTag, "No EGL_KHR_create_context / EGL 1.5: selecting config by ES2 bit");
    const EGLint renderable = es3Bit ? EGL_OPENGL_ES3_BIT_KHR : EGL_OPENGL_ES2_BIT;
    const EGLint requiredSurface =
        options_.windowKind == WindowKind::Headless ? EGL_PBUFFER_BIT : EGL_WINDOW_BIT;

    const EGLint attribs[] = {EGL_RENDERABLE_TYPE, renderable, EGL_SURFACE_TYPE, requiredSurface,
                              EGL_RED_SIZE,        8,          EGL_GREEN_SIZE,   8,
                              EGL_BLUE_SIZE,       8,          EGL_ALPHA_SIZE,   8,
                              EGL_NONE};
    EGLint count = 0;
    if (!eglChooseConfig(display_, attribs, nullptr, 0, &count) || count <= 0) {
        error = "no EGL config with RGBA8888 + ES3";
        return false;
    }
    std::vector<EGLConfig> configs(static_cast<size_t>(count));
    eglChooseConfig(display_, attribs, configs.data(), count, &count);

    // eglChooseConfig sorts deeper formats first; pick exactly RGBA8888 with
    // the smallest depth/stencil, preferring configs that also allow pbuffers.
    int bestScore = -1;
    for (EGLConfig c : configs) {
        EGLint r, g, b, a, d, s, surf;
        eglGetConfigAttrib(display_, c, EGL_RED_SIZE, &r);
        eglGetConfigAttrib(display_, c, EGL_GREEN_SIZE, &g);
        eglGetConfigAttrib(display_, c, EGL_BLUE_SIZE, &b);
        eglGetConfigAttrib(display_, c, EGL_ALPHA_SIZE, &a);
        eglGetConfigAttrib(display_, c, EGL_DEPTH_SIZE, &d);
        eglGetConfigAttrib(display_, c, EGL_STENCIL_SIZE, &s);
        eglGetConfigAttrib(display_, c, EGL_SURFACE_TYPE, &surf);
        if (r != 8 || g != 8 || b != 8 || a != 8) continue;
        int score = 1000 - d - s;
        if (surf & EGL_PBUFFER_BIT) score += 1;
        if (score > bestScore) {
            bestScore = score;
            config_ = c;
            configHasPbuffer_ = (surf & EGL_PBUFFER_BIT) != 0;
        }
    }
    if (!config_) {
        error = "no exact RGBA8888 EGL config";
        return false;
    }
    return true;
}

std::unique_ptr<gfx::GraphicsDevice> GLESBackend::createDevice(PlatformWindow* window, std::string& error) {
    (void)window;  // EGL contexts are not tied to a window; attachSurface() verifies it.
    auto device = std::make_unique<GLESDevice>(*this);
    if (!device->initialize(error)) return nullptr;
    return device;
}

std::string GLESBackend::describe() const {
    return "EGL " + eglVersion() + " (" + eglVendor_ + ")";
}

}  // namespace pe::gles
