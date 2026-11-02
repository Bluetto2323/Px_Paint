#pragma once

#include <memory>

#include "graphics/abstraction/BufferResource.h"
#include "graphics/abstraction/PipelineResource.h"
#include "graphics/abstraction/RenderTarget.h"
#include "graphics/abstraction/ShaderResource.h"
#include "graphics/abstraction/TextureResource.h"
#include "graphics/opengles/GLESCommon.h"

namespace pe::gles {

struct GLFormat {
    GLenum internalFormat = 0;
    GLenum format = 0;
    GLenum type = 0;
};
GLFormat toGLFormat(gfx::TextureFormat format);

// Binds `buffer` as GL_ARRAY_BUFFER and points every attribute of `layout` at
// it (into the currently bound VAO).
void applyVertexLayout(const gfx::VertexLayout& layout, GLuint buffer, uint64_t offset);

class GLESBuffer final : public gfx::BufferResource {
public:
    static std::unique_ptr<GLESBuffer> create(const gfx::BufferDesc& desc, const void* data);
    ~GLESBuffer() override;
    bool update(uint64_t offset, const void* data, uint64_t size) override;
    GLuint handle() const { return buffer_; }

private:
    explicit GLESBuffer(const gfx::BufferDesc& desc) : BufferResource(desc) {}
    GLuint buffer_ = 0;
};

class GLESTexture final : public gfx::TextureResource {
public:
    static std::unique_ptr<GLESTexture> create(const gfx::TextureDesc& desc);
    ~GLESTexture() override;
    GLuint handle() const { return texture_; }

private:
    explicit GLESTexture(const gfx::TextureDesc& desc) : TextureResource(desc) {}
    GLuint texture_ = 0;
};

class GLESShader final : public gfx::ShaderResource {
public:
    static std::unique_ptr<GLESShader> create(const gfx::ShaderDesc& desc);
    ~GLESShader() override;
    GLuint handle() const { return shader_; }

private:
    GLESShader(gfx::ShaderStage stage, std::string name) : ShaderResource(stage, std::move(name)) {}
    GLuint shader_ = 0;
};

class GLESPipeline final : public gfx::PipelineResource {
public:
    static std::unique_ptr<GLESPipeline> create(const gfx::PipelineDesc& desc);
    ~GLESPipeline() override;

    GLuint program() const { return program_; }
    GLuint vertexArray() const { return vao_; }
    GLenum topology() const { return topology_; }
    gfx::BlendMode blend() const { return blend_; }
    const gfx::VertexLayout& layout() const { return layout_; }
    GLint constantsLocation() const { return constantsLocation_; }

private:
    GLESPipeline(gfx::TextureFormat fmt, std::string name, uint32_t constantsSize)
        : PipelineResource(fmt, std::move(name), constantsSize) {}
    GLuint program_ = 0;
    GLuint vao_ = 0;
    GLenum topology_ = GL_TRIANGLES;
    gfx::BlendMode blend_ = gfx::BlendMode::Opaque;
    gfx::VertexLayout layout_;
    GLint constantsLocation_ = -1;  // uniform vec4 uPush[N]
};

// The default framebuffer of the EGL window surface.
class GLESBackbuffer final : public gfx::RenderTarget {
public:
    gfx::Extent2D extent() const override { return extent_; }
    gfx::TextureFormat colorFormat() const override { return gfx::TextureFormat::RGBA8_UNORM; }
    bool isBackbuffer() const override { return true; }
    void setExtent(gfx::Extent2D e) { extent_ = e; }
    GLuint framebuffer() const { return 0; }

private:
    gfx::Extent2D extent_;
};

}  // namespace pe::gles
