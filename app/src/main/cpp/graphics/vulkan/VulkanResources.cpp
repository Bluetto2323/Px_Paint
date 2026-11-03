#include "graphics/vulkan/VulkanResources.h"

#include <cstring>
#include <vector>

#include "core/Log.h"
#include "graphics/abstraction/RenderingContext.h"
#include "graphics/vulkan/VulkanDevice.h"

namespace pe::vk {

namespace {
constexpr const char* kTag = "Vulkan";

VkBufferUsageFlags toVkBufferUsage(uint32_t u) {
    VkBufferUsageFlags f = 0;
    if (u & gfx::BufferUsage::Vertex) f |= VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    if (u & gfx::BufferUsage::Index) f |= VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
    if (u & gfx::BufferUsage::Uniform) f |= VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
    if (u & gfx::BufferUsage::TransferSrc) f |= VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    if (u & gfx::BufferUsage::TransferDst) f |= VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    return f;
}

VkFormat toVkVertexFormat(gfx::VertexFormat f) {
    switch (f) {
        case gfx::VertexFormat::Float2: return VK_FORMAT_R32G32_SFLOAT;
        case gfx::VertexFormat::Float3: return VK_FORMAT_R32G32B32_SFLOAT;
        case gfx::VertexFormat::Float4: return VK_FORMAT_R32G32B32A32_SFLOAT;
        case gfx::VertexFormat::UByte4Norm: return VK_FORMAT_R8G8B8A8_UNORM;
    }
    return VK_FORMAT_UNDEFINED;
}

bool writeMapped(VulkanDevice& device, const Allocation& a, uint64_t offset, const void* data, uint64_t size) {
    std::memcpy(static_cast<uint8_t*>(a.mapped) + offset, data, size);
    if (!a.coherent) {
        VkMappedMemoryRange range{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
        range.memory = a.memory;
        range.offset = 0;
        range.size = VK_WHOLE_SIZE;
        return PE_VK_CHECK(vkFlushMappedMemoryRanges(device.device(), 1, &range));
    }
    return true;
}
}  // namespace

// ------------------------------------------------------------------ Buffer
std::unique_ptr<VulkanBuffer> VulkanBuffer::create(VulkanDevice& device, const gfx::BufferDesc& desc,
                                                   const void* data) {
    if (desc.size == 0) {
        PE_LOGE(kTag, "Buffer '%s': size 0", desc.debugName.c_str());
        return nullptr;
    }
    std::unique_ptr<VulkanBuffer> b(new VulkanBuffer(device, desc));
    VkDevice d = device.device();

    VkBufferCreateInfo ci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    ci.size = desc.size;
    ci.usage = toVkBufferUsage(desc.usage);
    if (desc.memory == gfx::MemoryUsage::GpuOnly && data) ci.usage |= VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (!PE_VK_CHECK(vkCreateBuffer(d, &ci, nullptr, &b->buffer_))) return nullptr;

    VkMemoryRequirements req{};
    vkGetBufferMemoryRequirements(d, b->buffer_, &req);

    bool ok = false;
    bool needsStaging = false;
    switch (desc.memory) {
        case gfx::MemoryUsage::GpuOnly:
            if (data) {
                // Unified memory (Mali, Adreno...): write directly, no copy.
                ok = device.allocate(req, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
                                     VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, b->alloc_);
            }
            if (!ok) {
                ok = device.allocate(req, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0, b->alloc_);
                needsStaging = ok && data;
            }
            break;
        case gfx::MemoryUsage::CpuToGpu:
            ok = device.allocate(req, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
                                 VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, b->alloc_);
            break;
        case gfx::MemoryUsage::GpuToCpu:
            ok = device.allocate(req, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, VK_MEMORY_PROPERTY_HOST_CACHED_BIT,
                                 b->alloc_);
            break;
    }
    if (!ok) {
        PE_LOGE(kTag, "Buffer '%s': no suitable memory type", desc.debugName.c_str());
        return nullptr;
    }
    if (!PE_VK_CHECK(vkBindBufferMemory(d, b->buffer_, b->alloc_.memory, 0))) return nullptr;
    b->setGpuMemoryBytes(b->alloc_.size);

    if (data && !needsStaging) {
        if (!writeMapped(device, b->alloc_, 0, data, desc.size)) return nullptr;
    } else if (data && needsStaging) {
        gfx::BufferDesc sd;
        sd.size = desc.size;
        sd.usage = gfx::BufferUsage::TransferSrc;
        sd.memory = gfx::MemoryUsage::CpuToGpu;
        sd.debugName = desc.debugName + ".staging";
        auto staging = VulkanBuffer::create(device, sd, data);
        if (!staging) return nullptr;
        VkBuffer src = staging->handle();
        VkBuffer dst = b->buffer_;
        const VkDeviceSize size = desc.size;
        if (!device.submitImmediate([&](VkCommandBuffer cmd) {
                VkBufferCopy region{0, 0, size};
                vkCmdCopyBuffer(cmd, src, dst, 1, &region);
            }))
            return nullptr;
    }
    device.setObjectName(VK_OBJECT_TYPE_BUFFER, reinterpret_cast<uint64_t>(b->buffer_), desc.debugName.c_str());
    return b;
}

VulkanBuffer::~VulkanBuffer() {
    if (buffer_) vkDestroyBuffer(device_.device(), buffer_, nullptr);
    device_.free(alloc_);
}

bool VulkanBuffer::update(uint64_t offset, const void* data, uint64_t size) {
    if (!alloc_.mapped || offset + size > desc_.size) return false;
    return writeMapped(device_, alloc_, offset, data, size);
}

// ----------------------------------------------------------------- Texture
std::unique_ptr<VulkanTexture> VulkanTexture::create(VulkanDevice& device, const gfx::TextureDesc& desc) {
    const VkFormat format = toVkFormat(desc.format);
    if (format == VK_FORMAT_UNDEFINED || desc.width == 0 || desc.height == 0) {
        PE_LOGE(kTag, "Texture '%s': invalid format/size", desc.debugName.c_str());
        return nullptr;
    }
    std::unique_ptr<VulkanTexture> t(new VulkanTexture(device, desc));
    VkDevice d = device.device();
    const bool depth = gfx::isDepthFormat(desc.format);

    VkImageUsageFlags usage = 0;
    // Sampled textures are cleared at creation and updated with copies.
    if (desc.usage & gfx::TextureUsage::Sampled) usage |= VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (desc.usage & gfx::TextureUsage::ColorAttachment)
        usage |= depth ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    if (desc.usage & gfx::TextureUsage::TransferSrc) usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    if (desc.usage & gfx::TextureUsage::TransferDst) usage |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (desc.usage & gfx::TextureUsage::Storage) usage |= VK_IMAGE_USAGE_STORAGE_BIT;

    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = format;
    ci.extent = {desc.width, desc.height, 1};
    ci.mipLevels = desc.mipLevels;
    ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = usage;
    ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (!PE_VK_CHECK(vkCreateImage(d, &ci, nullptr, &t->image_))) return nullptr;

    VkMemoryRequirements req{};
    vkGetImageMemoryRequirements(d, t->image_, &req);
    if (!device.allocate(req, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0, t->alloc_)) {
        PE_LOGE(kTag, "Texture '%s': out of device memory", desc.debugName.c_str());
        return nullptr;
    }
    if (!PE_VK_CHECK(vkBindImageMemory(d, t->image_, t->alloc_.memory, 0))) return nullptr;
    t->setGpuMemoryBytes(t->alloc_.size);

    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = t->image_;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = format;
    vi.subresourceRange = {depth ? static_cast<VkImageAspectFlags>(VK_IMAGE_ASPECT_DEPTH_BIT)
                                 : static_cast<VkImageAspectFlags>(VK_IMAGE_ASPECT_COLOR_BIT),
                           0, desc.mipLevels, 0, 1};
    if (!PE_VK_CHECK(vkCreateImageView(d, &vi, nullptr, &t->view_))) return nullptr;
    device.setObjectName(VK_OBJECT_TYPE_IMAGE, reinterpret_cast<uint64_t>(t->image_), desc.debugName.c_str());

    if ((desc.usage & gfx::TextureUsage::Sampled) && !depth) {
        // Defined initial content (transparent) and the layout every later
        // update and draw expects: SHADER_READ_ONLY_OPTIMAL.
        VkImage image = t->image_;
        const uint32_t mips = desc.mipLevels;
        // In a frame this is recorded before the render pass (no GPU wait);
        // a canvas tile created while painting therefore costs no stall.
        const bool cleared = device.recordTransfer([&](VkCommandBuffer cmd) {
            VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
            b.srcAccessMask = 0;
            b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.image = image;
            b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, mips, 0, 1};
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0,
                                 nullptr, 0, nullptr, 1, &b);
            const VkClearColorValue zero{{0.0f, 0.0f, 0.0f, 0.0f}};
            vkCmdClearColorImage(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &zero, 1, &b.subresourceRange);
            b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0,
                                 nullptr, 0, nullptr, 1, &b);
        });
        if (!cleared) return nullptr;
        t->descriptorSet_ = device.allocateTextureSet(t->view_, desc.linearFilter);
        if (!t->descriptorSet_) {
            PE_LOGE(kTag, "Texture '%s': no descriptor set (pool exhausted?)", desc.debugName.c_str());
            return nullptr;
        }
    }
    return t;
}

VulkanTexture::~VulkanTexture() {
    device_.freeTextureSet(descriptorSet_);
    VkDevice d = device_.device();
    if (view_) vkDestroyImageView(d, view_, nullptr);
    if (image_) vkDestroyImage(d, image_, nullptr);
    device_.free(alloc_);
}

// ------------------------------------------------------------------ Shader
std::unique_ptr<VulkanShader> VulkanShader::create(VulkanDevice& device, const gfx::ShaderDesc& desc) {
    if (!desc.spirv || desc.spirvSizeBytes == 0 || desc.spirvSizeBytes % 4 != 0) {
        PE_LOGE(kTag, "Shader '%s': no valid SPIR-V provided", desc.debugName.c_str());
        return nullptr;
    }
    std::unique_ptr<VulkanShader> s(new VulkanShader(device, desc.stage, desc.debugName));
    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = desc.spirvSizeBytes;
    ci.pCode = desc.spirv;
    if (!PE_VK_CHECK(vkCreateShaderModule(device.device(), &ci, nullptr, &s->module_))) return nullptr;
    return s;
}

VulkanShader::~VulkanShader() {
    if (module_) vkDestroyShaderModule(device_.device(), module_, nullptr);
}

// ---------------------------------------------------------------- Pipeline
std::unique_ptr<VulkanPipeline> VulkanPipeline::create(VulkanDevice& device, const gfx::PipelineDesc& desc) {
    auto* vs = static_cast<VulkanShader*>(desc.vertexShader.get());
    auto* fs = static_cast<VulkanShader*>(desc.fragmentShader.get());
    const VkFormat colorFormat = toVkFormat(desc.colorFormat);
    if (!vs || !fs || colorFormat == VK_FORMAT_UNDEFINED) {
        PE_LOGE(kTag, "Pipeline '%s': missing shader or color format", desc.debugName.c_str());
        return nullptr;
    }
    if (desc.constantsSize % 16 != 0 || desc.constantsSize > gfx::RenderingContext::kMaxConstantsSize) {
        PE_LOGE(kTag, "Pipeline '%s': constantsSize %u must be a multiple of 16 and <= %u", desc.debugName.c_str(),
                desc.constantsSize, gfx::RenderingContext::kMaxConstantsSize);
        return nullptr;
    }
    std::unique_ptr<VulkanPipeline> p(new VulkanPipeline(device, desc.colorFormat, desc.debugName, desc.constantsSize));
    VkDevice d = device.device();

    // 128 bytes of push constants are guaranteed by the spec (maxPushConstantsSize >= 128).
    VkPushConstantRange range{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, desc.constantsSize};
    VkDescriptorSetLayout setLayout = device.textureSetLayout();
    VkPipelineLayoutCreateInfo li{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    if (desc.constantsSize > 0) {
        li.pushConstantRangeCount = 1;
        li.pPushConstantRanges = &range;
    }
    if (desc.sampledTexture) {
        li.setLayoutCount = 1;
        li.pSetLayouts = &setLayout;
    }
    if (!PE_VK_CHECK(vkCreatePipelineLayout(d, &li, nullptr, &p->layout_))) return nullptr;

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vs->module();
    stages[0].pName = "main";
    stages[1] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fs->module();
    stages[1].pName = "main";

    VkVertexInputBindingDescription binding{0, desc.vertexLayout.stride, VK_VERTEX_INPUT_RATE_VERTEX};
    std::vector<VkVertexInputAttributeDescription> attrs;
    for (const auto& a : desc.vertexLayout.attributes)
        attrs.push_back({a.location, 0, toVkVertexFormat(a.format), a.offset});
    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    if (desc.vertexLayout.stride > 0) {
        vi.vertexBindingDescriptionCount = 1;
        vi.pVertexBindingDescriptions = &binding;
        vi.vertexAttributeDescriptionCount = static_cast<uint32_t>(attrs.size());
        vi.pVertexAttributeDescriptions = attrs.data();
    }

    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    switch (desc.topology) {
        case gfx::PrimitiveTopology::TriangleList: ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST; break;
        case gfx::PrimitiveTopology::TriangleStrip: ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP; break;
        case gfx::PrimitiveTopology::LineList: ia.topology = VK_PRIMITIVE_TOPOLOGY_LINE_LIST; break;
    }

    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = 1;
    vp.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineColorBlendAttachmentState blend{};
    blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT |
                           VK_COLOR_COMPONENT_A_BIT;
    if (desc.blend == gfx::BlendMode::PremultipliedAlpha) {
        blend.blendEnable = VK_TRUE;
        blend.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
        blend.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        blend.colorBlendOp = VK_BLEND_OP_ADD;
        blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        blend.alphaBlendOp = VK_BLEND_OP_ADD;
    }
    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = 1;
    cb.pAttachments = &blend;

    const VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    ds.dynamicStateCount = 2;
    ds.pDynamicStates = dyn;

    VkGraphicsPipelineCreateInfo ci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    ci.stageCount = 2;
    ci.pStages = stages;
    ci.pVertexInputState = &vi;
    ci.pInputAssemblyState = &ia;
    ci.pViewportState = &vp;
    ci.pRasterizationState = &rs;
    ci.pMultisampleState = &ms;
    ci.pColorBlendState = &cb;
    ci.pDynamicState = &ds;
    ci.layout = p->layout_;
    ci.renderPass = device.renderPass(colorFormat, gfx::LoadOp::Clear, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
    ci.subpass = 0;
    if (!ci.renderPass) return nullptr;
    if (!PE_VK_CHECK(vkCreateGraphicsPipelines(d, device.pipelineCache(), 1, &ci, nullptr, &p->pipeline_)))
        return nullptr;
    device.setObjectName(VK_OBJECT_TYPE_PIPELINE, reinterpret_cast<uint64_t>(p->pipeline_), desc.debugName.c_str());
    return p;
}

VulkanPipeline::~VulkanPipeline() {
    VkDevice d = device_.device();
    if (pipeline_) vkDestroyPipeline(d, pipeline_, nullptr);
    if (layout_) vkDestroyPipelineLayout(d, layout_, nullptr);
}

}  // namespace pe::vk
