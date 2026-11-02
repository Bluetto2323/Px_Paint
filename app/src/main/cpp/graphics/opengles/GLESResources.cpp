#include "graphics/opengles/GLESResources.h"

#include <string>
#include <vector>

#include "core/Log.h"
#include "graphics/abstraction/RenderingContext.h"

namespace pe::gles {

namespace {
constexpr const char* kTag = "GLES";

GLenum bufferTarget(uint32_t usage) {
    if (usage & gfx::BufferUsage::Index) return GL_ELEMENT_ARRAY_BUFFER;
    if (usage & gfx::BufferUsage::Uniform) return GL_UNIFORM_BUFFER;
    return GL_ARRAY_BUFFER;
}

GLenum bufferHint(gfx::MemoryUsage m) {
    switch (m) {
        case gfx::MemoryUsage::GpuOnly: return GL_STATIC_DRAW;
        case gfx::MemoryUsage::CpuToGpu: return GL_DYNAMIC_DRAW;
        case gfx::MemoryUsage::GpuToCpu: return GL_STREAM_READ;
    }
    return GL_STATIC_DRAW;
}

std::string shaderLog(GLuint shader) {
    GLint len = 0;
    glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &len);
    std::string log(static_cast<size_t>(len > 1 ? len : 1), '\0');
    if (len > 1) glGetShaderInfoLog(shader, len, nullptr, log.data());
    return log;
}

std::string programLog(GLuint program) {
    GLint len = 0;
    glGetProgramiv(program, GL_INFO_LOG_LENGTH, &len);
    std::string log(static_cast<size_t>(len > 1 ? len : 1), '\0');
    if (len > 1) glGetProgramInfoLog(program, len, nullptr, log.data());
    return log;
}

void attribFormat(gfx::VertexFormat f, GLint& components, GLenum& type, GLboolean& normalized) {
    switch (f) {
        case gfx::VertexFormat::Float2: components = 2; type = GL_FLOAT; normalized = GL_FALSE; return;
        case gfx::VertexFormat::Float3: components = 3; type = GL_FLOAT; normalized = GL_FALSE; return;
        case gfx::VertexFormat::Float4: components = 4; type = GL_FLOAT; normalized = GL_FALSE; return;
        case gfx::VertexFormat::UByte4Norm: components = 4; type = GL_UNSIGNED_BYTE; normalized = GL_TRUE; return;
    }
}
}  // namespace

GLFormat toGLFormat(gfx::TextureFormat format) {
    using F = gfx::TextureFormat;
    switch (format) {
        case F::RGBA8_UNORM: return {GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE};
        case F::RGBA8_SRGB: return {GL_SRGB8_ALPHA8, GL_RGBA, GL_UNSIGNED_BYTE};
        case F::R8_UNORM: return {GL_R8, GL_RED, GL_UNSIGNED_BYTE};
        case F::RG8_UNORM: return {GL_RG8, GL_RG, GL_UNSIGNED_BYTE};
        case F::RGBA16_FLOAT: return {GL_RGBA16F, GL_RGBA, GL_HALF_FLOAT};
        case F::R16_FLOAT: return {GL_R16F, GL_RED, GL_HALF_FLOAT};
        case F::R32_FLOAT: return {GL_R32F, GL_RED, GL_FLOAT};
        case F::RGB10A2_UNORM: return {GL_RGB10_A2, GL_RGBA, GL_UNSIGNED_INT_2_10_10_10_REV};
        case F::D24_UNORM_S8_UINT: return {GL_DEPTH24_STENCIL8, GL_DEPTH_STENCIL, GL_UNSIGNED_INT_24_8};
        case F::D32_FLOAT: return {GL_DEPTH_COMPONENT32F, GL_DEPTH_COMPONENT, GL_FLOAT};
        case F::BGRA8_UNORM: return {GL_BGRA8_EXT, GL_BGRA_EXT, GL_UNSIGNED_BYTE};
        default: return {};
    }
}

// ---------------------------------------------------------------- Buffer
std::unique_ptr<GLESBuffer> GLESBuffer::create(const gfx::BufferDesc& desc, const void* data) {
    if (desc.size == 0) {
        PE_LOGE(kTag, "Buffer '%s': size 0", desc.debugName.c_str());
        return nullptr;
    }
    std::unique_ptr<GLESBuffer> b(new GLESBuffer(desc));
    glGenBuffers(1, &b->buffer_);
    const GLenum target = bufferTarget(desc.usage);
    glBindBuffer(target, b->buffer_);
    glBufferData(target, static_cast<GLsizeiptr>(desc.size), data, bufferHint(desc.memory));
    glBindBuffer(target, 0);
    if (drainGLErrors("glBufferData") != 0) return nullptr;
    b->setGpuMemoryBytes(desc.size);
    return b;
}

GLESBuffer::~GLESBuffer() {
    if (buffer_) glDeleteBuffers(1, &buffer_);
}

bool GLESBuffer::update(uint64_t offset, const void* data, uint64_t size) {
    if (desc_.memory == gfx::MemoryUsage::GpuOnly || offset + size > desc_.size) return false;
    const GLenum target = bufferTarget(desc_.usage);
    glBindBuffer(target, buffer_);
    glBufferSubData(target, static_cast<GLintptr>(offset), static_cast<GLsizeiptr>(size), data);
    glBindBuffer(target, 0);
    return true;
}

// ---------------------------------------------------------------- Texture
std::unique_ptr<GLESTexture> GLESTexture::create(const gfx::TextureDesc& desc) {
    const GLFormat f = toGLFormat(desc.format);
    if (f.internalFormat == 0 || desc.width == 0 || desc.height == 0) {
        PE_LOGE(kTag, "Texture '%s': invalid format/size", desc.debugName.c_str());
        return nullptr;
    }
    std::unique_ptr<GLESTexture> t(new GLESTexture(desc));
    glGenTextures(1, &t->texture_);
    glBindTexture(GL_TEXTURE_2D, t->texture_);
    glTexStorage2D(GL_TEXTURE_2D, static_cast<GLsizei>(desc.mipLevels), f.internalFormat,
                   static_cast<GLsizei>(desc.width), static_cast<GLsizei>(desc.height));
    if (desc.usage & gfx::TextureUsage::Sampled) {
        const GLint filter = desc.linearFilter ? GL_LINEAR : GL_NEAREST;
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }
    glBindTexture(GL_TEXTURE_2D, 0);
    if (drainGLErrors("glTexStorage2D") != 0) return nullptr;
    if ((desc.usage & gfx::TextureUsage::Sampled) && !gfx::isDepthFormat(desc.format)) {
        // Defined initial content (transparent) on every backend: clear via a
        // temporary framebuffer instead of uploading a zero buffer.
        GLint previousFbo = 0;
        glGetIntegerv(GL_FRAMEBUFFER_BINDING, &previousFbo);
        GLuint fbo = 0;
        glGenFramebuffers(1, &fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t->texture_, 0);
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE) {
            GLboolean mask[4];
            glGetBooleanv(GL_COLOR_WRITEMASK, mask);
            const GLboolean scissor = glIsEnabled(GL_SCISSOR_TEST);
            glDisable(GL_SCISSOR_TEST);
            glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
            glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
            glClear(GL_COLOR_BUFFER_BIT);
            glColorMask(mask[0], mask[1], mask[2], mask[3]);
            if (scissor) glEnable(GL_SCISSOR_TEST);
        } else {
            PE_LOGW(kTag, "Texture '%s': not clearable through an FBO, content undefined", desc.debugName.c_str());
        }
        glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(previousFbo));
        glDeleteFramebuffers(1, &fbo);
        if (drainGLErrors("texture initial clear") != 0) return nullptr;
    }
    uint64_t bytes = static_cast<uint64_t>(desc.width) * desc.height * gfx::bytesPerPixel(desc.format);
    if (desc.mipLevels > 1) bytes = bytes * 4 / 3;
    t->setGpuMemoryBytes(bytes);
    return t;
}

GLESTexture::~GLESTexture() {
    if (texture_) glDeleteTextures(1, &texture_);
}

// ---------------------------------------------------------------- Shader
std::unique_ptr<GLESShader> GLESShader::create(const gfx::ShaderDesc& desc) {
    if (!desc.glslEs) {
        PE_LOGE(kTag, "Shader '%s': no GLSL ES source provided", desc.debugName.c_str());
        return nullptr;
    }
    std::unique_ptr<GLESShader> s(new GLESShader(desc.stage, desc.debugName));
    s->shader_ = glCreateShader(desc.stage == gfx::ShaderStage::Vertex ? GL_VERTEX_SHADER : GL_FRAGMENT_SHADER);
    glShaderSource(s->shader_, 1, &desc.glslEs, nullptr);
    glCompileShader(s->shader_);
    GLint ok = GL_FALSE;
    glGetShaderiv(s->shader_, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        PE_LOGE(kTag, "Shader '%s' (%s) compile failed:\n%s", desc.debugName.c_str(), gfx::toString(desc.stage),
                shaderLog(s->shader_).c_str());
        return nullptr;
    }
    return s;
}

GLESShader::~GLESShader() {
    if (shader_) glDeleteShader(shader_);
}

// ---------------------------------------------------------------- Pipeline
std::unique_ptr<GLESPipeline> GLESPipeline::create(const gfx::PipelineDesc& desc) {
    auto* vs = static_cast<GLESShader*>(desc.vertexShader.get());
    auto* fs = static_cast<GLESShader*>(desc.fragmentShader.get());
    if (!vs || !fs) {
        PE_LOGE(kTag, "Pipeline '%s': missing shader", desc.debugName.c_str());
        return nullptr;
    }
    if (desc.constantsSize % 16 != 0 || desc.constantsSize > gfx::RenderingContext::kMaxConstantsSize) {
        PE_LOGE(kTag, "Pipeline '%s': constantsSize %u must be a multiple of 16 and <= %u", desc.debugName.c_str(),
                desc.constantsSize, gfx::RenderingContext::kMaxConstantsSize);
        return nullptr;
    }
    std::unique_ptr<GLESPipeline> p(new GLESPipeline(desc.colorFormat, desc.debugName, desc.constantsSize));
    p->program_ = glCreateProgram();
    glAttachShader(p->program_, vs->handle());
    glAttachShader(p->program_, fs->handle());
    glLinkProgram(p->program_);
    GLint ok = GL_FALSE;
    glGetProgramiv(p->program_, GL_LINK_STATUS, &ok);
    glDetachShader(p->program_, vs->handle());
    glDetachShader(p->program_, fs->handle());
    if (!ok) {
        PE_LOGE(kTag, "Pipeline '%s' link failed:\n%s", desc.debugName.c_str(), programLog(p->program_).c_str());
        return nullptr;
    }

    if (desc.constantsSize > 0) {
        p->constantsLocation_ = glGetUniformLocation(p->program_, "uPush");
        if (p->constantsLocation_ < 0) {
            PE_LOGE(kTag, "Pipeline '%s': shader has no 'uniform vec4 uPush[]'", desc.debugName.c_str());
            return nullptr;
        }
    }
    if (desc.sampledTexture) {
        const GLint texLocation = glGetUniformLocation(p->program_, "uTex");
        if (texLocation < 0) {
            PE_LOGE(kTag, "Pipeline '%s': shader has no 'uniform sampler2D uTex'", desc.debugName.c_str());
            return nullptr;
        }
        GLint previous = 0;
        glGetIntegerv(GL_CURRENT_PROGRAM, &previous);
        glUseProgram(p->program_);
        glUniform1i(texLocation, 0);  // texture unit 0
        glUseProgram(static_cast<GLuint>(previous));
    }

    glGenVertexArrays(1, &p->vao_);
    glBindVertexArray(p->vao_);
    for (const auto& a : desc.vertexLayout.attributes) glEnableVertexAttribArray(a.location);
    glBindVertexArray(0);

    switch (desc.topology) {
        case gfx::PrimitiveTopology::TriangleList: p->topology_ = GL_TRIANGLES; break;
        case gfx::PrimitiveTopology::TriangleStrip: p->topology_ = GL_TRIANGLE_STRIP; break;
        case gfx::PrimitiveTopology::LineList: p->topology_ = GL_LINES; break;
    }
    p->blend_ = desc.blend;
    p->layout_ = desc.vertexLayout;
    if (drainGLErrors("createPipeline") != 0) return nullptr;
    return p;
}

GLESPipeline::~GLESPipeline() {
    if (vao_) glDeleteVertexArrays(1, &vao_);
    if (program_) glDeleteProgram(program_);
}

// Vertex attribute setup is applied lazily by the rendering context because
// GLES 3.0 binds the buffer into the VAO at glVertexAttribPointer time.
void applyVertexLayout(const gfx::VertexLayout& layout, GLuint buffer, uint64_t offset) {
    glBindBuffer(GL_ARRAY_BUFFER, buffer);
    for (const auto& a : layout.attributes) {
        GLint comps = 0;
        GLenum type = GL_FLOAT;
        GLboolean norm = GL_FALSE;
        attribFormat(a.format, comps, type, norm);
        glVertexAttribPointer(a.location, comps, type, norm, static_cast<GLsizei>(layout.stride),
                              reinterpret_cast<const void*>(static_cast<uintptr_t>(offset + a.offset)));
    }
}

}  // namespace pe::gles
