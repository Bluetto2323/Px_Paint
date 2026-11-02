#pragma once

#include <memory>
#include <string>
#include <vector>

#include "graphics/abstraction/GraphicsBackend.h"
#include "graphics/opengles/GLESCommon.h"

namespace pe::gles {

// EGL display + config. Creates one GLESDevice (EGL context).
class GLESBackend final : public gfx::GraphicsBackend {
public:
    GLESBackend() = default;
    ~GLESBackend() override;

    gfx::BackendType type() const override { return gfx::BackendType::OpenGLES; }
    bool initialize(const gfx::BackendInitOptions& options, std::string& error) override;
    std::unique_ptr<gfx::GraphicsDevice> createDevice(PlatformWindow* window, std::string& error) override;
    std::string describe() const override;

    EGLDisplay display() const { return display_; }
    EGLConfig config() const { return config_; }
    bool configHasPbuffer() const { return configHasPbuffer_; }
    bool hasEglExtension(const char* name) const;
    const std::vector<std::string>& eglExtensions() const { return eglExtensions_; }
    std::string eglVersion() const;
    WindowKind windowKind() const { return options_.windowKind; }
    bool validation() const { return options_.enableValidation; }

private:
    bool chooseConfig(std::string& error);

    gfx::BackendInitOptions options_;
    EGLDisplay display_ = EGL_NO_DISPLAY;
    bool initialized_ = false;
    EGLConfig config_ = nullptr;
    bool configHasPbuffer_ = false;
    EGLint eglMajor_ = 0;
    EGLint eglMinor_ = 0;
    std::string eglVendor_;
    std::vector<std::string> eglExtensions_;
};

std::unique_ptr<gfx::GraphicsBackend> createGLESBackend();

}  // namespace pe::gles
