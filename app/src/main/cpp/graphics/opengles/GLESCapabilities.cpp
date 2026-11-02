// Capability discovery for the OpenGL ES backend. Every value comes from a
// glGet* / eglQueryString call on the live context; format support follows
// the ES 3.0/3.1/3.2 specification tables plus the extensions that extend them.
#include <algorithm>

#include "core/Log.h"
#include "graphics/opengles/GLESBackend.h"
#include "graphics/opengles/GLESDevice.h"

namespace pe::gles {

namespace {
std::string glString(GLenum name) {
    const auto* s = reinterpret_cast<const char*>(glGetString(name));
    return s ? s : "";
}

uint32_t getU32(GLenum name) {
    GLint v = 0;
    glGetIntegerv(name, &v);
    return v > 0 ? static_cast<uint32_t>(v) : 0u;
}
}  // namespace

void GLESDevice::queryCapabilities() {
    using F = gfx::TextureFormat;
    auto& c = caps_;
    c = {};
    c.backend = gfx::BackendType::OpenGLES;
    c.vendorName = glString(GL_VENDOR);
    c.deviceName = glString(GL_RENDERER);
    c.driverInfo = glString(GL_VERSION);
    c.shadingLanguage = glString(GL_SHADING_LANGUAGE_VERSION);
    c.deviceType = "unknown (not exposed by GLES)";
    c.apiMajor = getU32(GL_MAJOR_VERSION);
    c.apiMinor = getU32(GL_MINOR_VERSION);
    c.apiVersionString = "OpenGL ES " + std::to_string(c.apiMajor) + "." + std::to_string(c.apiMinor);
    c.instanceVersionString = "EGL " + backend_.eglVersion();
    c.driverVersion = c.driverInfo;
    c.instanceExtensions = backend_.eglExtensions();

    const uint32_t extCount = getU32(GL_NUM_EXTENSIONS);
    for (uint32_t i = 0; i < extCount; ++i) {
        const auto* e = reinterpret_cast<const char*>(glGetStringi(GL_EXTENSIONS, i));
        if (e) c.deviceExtensions.emplace_back(e);
    }
    const bool es31 = c.apiMajor > 3 || (c.apiMajor == 3 && c.apiMinor >= 1);
    const bool es32 = c.apiMajor > 3 || (c.apiMajor == 3 && c.apiMinor >= 2);
    auto has = [&](const char* e) { return c.hasDeviceExtension(e); };

    // Limits
    auto& l = c.limits;
    l.maxTextureSize2D = getU32(GL_MAX_TEXTURE_SIZE);
    GLint viewport[2] = {0, 0};
    glGetIntegerv(GL_MAX_VIEWPORT_DIMS, viewport);
    const uint32_t rb = getU32(GL_MAX_RENDERBUFFER_SIZE);
    l.maxFramebufferWidth = std::min<uint32_t>(static_cast<uint32_t>(viewport[0]), rb);
    l.maxFramebufferHeight = std::min<uint32_t>(static_cast<uint32_t>(viewport[1]), rb);
    l.maxColorAttachments = getU32(GL_MAX_COLOR_ATTACHMENTS);
    l.maxVertexAttributes = getU32(GL_MAX_VERTEX_ATTRIBS);
    l.maxUniformBufferRange = getU32(GL_MAX_UNIFORM_BLOCK_SIZE);
    l.maxArrayTextureLayers = getU32(GL_MAX_ARRAY_TEXTURE_LAYERS);
    l.maxSamples = getU32(GL_MAX_SAMPLES);
    if (has("GL_EXT_texture_filter_anisotropic")) {
        GLfloat a = 0.0f;
        glGetFloatv(GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT, &a);
        l.maxSamplerAnisotropy = a;
    }
    if (es31) l.maxComputeWorkGroupInvocations = getU32(GL_MAX_COMPUTE_WORK_GROUP_INVOCATIONS);
    drainGLErrors("capability limits");  // tolerate drivers rejecting a query

    // Memory: GLES has no portable query.
    c.memory.known = false;

    // Formats (ES 3.0 table 3.13 + extensions)
    const bool halfRender = es32 || has("GL_EXT_color_buffer_half_float") || has("GL_EXT_color_buffer_float");
    const bool floatRender = es32 || has("GL_EXT_color_buffer_float");
    const bool floatLinear = has("GL_OES_texture_float_linear");
    const bool floatBlend = has("GL_EXT_float_blend");
    const bool bgra = has("GL_EXT_texture_format_BGRA8888");

    c.format(F::RGBA8_UNORM) = {true, true, true, true, es31, false};
    c.format(F::RGBA8_SRGB) = {true, true, true, true, false, false};
    c.format(F::BGRA8_UNORM) = {bgra, bgra, false, false, false, false};
    c.format(F::R8_UNORM) = {true, true, true, true, false, false};
    c.format(F::RG8_UNORM) = {true, true, true, true, false, false};
    c.format(F::RGBA16_FLOAT) = {true, true, halfRender, halfRender, es31, false};
    c.format(F::R16_FLOAT) = {true, true, halfRender, halfRender, false, false};
    c.format(F::R32_FLOAT) = {true, floatLinear, floatRender, floatRender && floatBlend, es31, false};
    c.format(F::RGB10A2_UNORM) = {true, true, true, true, false, false};
    c.format(F::D24_UNORM_S8_UINT) = {true, false, false, false, false, true};
    c.format(F::D32_FLOAT) = {true, false, false, false, false, true};

    // Usable optional features (verified, never assumed)
    c.addFeature("OpenGL ES 3.1 (compute, SSBO, image load/store)", es31);
    c.addFeature("OpenGL ES 3.2", es32);
    c.addFeature("KHR_debug", has("GL_KHR_debug"), debugContext_ ? "debug context" : "");
    c.addFeature("Half-float render targets", halfRender);
    c.addFeature("Float32 render targets", floatRender);
    c.addFeature("Float32 linear filtering", floatLinear);
    c.addFeature("Anisotropic filtering", l.maxSamplerAnisotropy > 0.0f,
                 l.maxSamplerAnisotropy > 0.0f ? "max " + std::to_string(l.maxSamplerAnisotropy) : "");
    c.addFeature("Framebuffer fetch",
                 has("GL_EXT_shader_framebuffer_fetch") || has("GL_ARM_shader_framebuffer_fetch"),
                 has("GL_EXT_shader_framebuffer_fetch") ? "EXT" : (has("GL_ARM_shader_framebuffer_fetch") ? "ARM" : ""));
    c.addFeature("Pixel local storage", has("GL_EXT_shader_pixel_local_storage"));
    c.addFeature("GPU timer queries", has("GL_EXT_disjoint_timer_query"));
    c.addFeature("ASTC LDR textures", has("GL_KHR_texture_compression_astc_ldr"));
    c.addFeature("BGRA8 textures", bgra);
    c.addFeature("EGL surfaceless context", backend_.hasEglExtension("EGL_KHR_surfaceless_context"));
    c.addFeature("EGL partial update / buffer age",
                 backend_.hasEglExtension("EGL_KHR_partial_update") || backend_.hasEglExtension("EGL_EXT_buffer_age"));
    c.addFeature("EGL presentation time", backend_.hasEglExtension("EGL_ANDROID_presentation_time"));

    if (!es31) c.warnings.push_back("OpenGL ES 3.0 only: no compute shaders on this backend");
    PE_LOGI("GLES", "Capabilities: %s, %zu GL extensions, max texture %u", c.apiVersionString.c_str(),
            c.deviceExtensions.size(), l.maxTextureSize2D);
}

}  // namespace pe::gles
