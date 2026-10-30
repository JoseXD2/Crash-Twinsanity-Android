// VkGl's drawing: a framebuffer's attachments drawn in a rendering instance (dynamic rendering; begun with the first draw or clear
// into them, ended by anything that has to happen outside one: a copy, a read, a barrier, another framebuffer), the pipelines (by
// the program, the vertex layout, the blend and the attachments' formats; the depth and stencil state, the viewport and scissor
// dynamic), the uniforms and the vertexes from the stream buffer, the clears, the uploads, the reads and the blits (a draw of their
// own, the GL blit's sampling). Every image stays in the general layout; a barrier comes before each copy and each rendering
#include "vkglinternal.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace VkGl
{
StreamPiece StreamAllocate(Context& c, VkDeviceSize size, VkDeviceSize alignment);
VkDescriptorSet AllocateSet(Context& c, VkDescriptorSetLayout layout);
void PresentImage(Context& c, Texture& source);

namespace
{
// ------------------------------------------------------------------------------------------------------------- the attachments
struct Attachments
{
    u32 colourCount = 0;
    Texture* colour[4] = {};
    u32 colourLevel[4] = {};
    Texture* depth = nullptr;
    u32 depthLevel = 0;
    u32 width = 0;
    u32 height = 0;
};

bool Gather(Framebuffer* framebuffer, Attachments* out)
{
    *out = Attachments{};
    if (framebuffer == nullptr)
    {
        return false;
    }

    u32 width = ~0u;
    u32 height = ~0u;
    auto take = [&](const Attachment& a, Texture** to, u32* level)
    {
        Texture* texture = FindTexture(a.texture);
        if (texture == nullptr || !EnsureImage(*texture))
        {
            return false;
        }

        *to = texture;
        *level = static_cast<u32>(a.level);
        width = std::min(width, std::max(1u, texture->width >> a.level));
        height = std::min(height, std::max(1u, texture->height >> a.level));
        return true;
    };
    for (u32 i = 0; i < 4; i++)
    {
        const GLenum buffer = framebuffer->drawBuffers[i];
        if (buffer < Gl::COLOR_ATTACHMENT0 || buffer > Gl::COLOR_ATTACHMENT0 + 3)
        {
            continue;
        }

        Texture* texture = nullptr;
        u32 level = 0;
        if (take(framebuffer->colour[buffer - Gl::COLOR_ATTACHMENT0], &texture, &level))
        {
            out->colour[i] = texture;
            out->colourLevel[i] = level;
            out->colourCount = i + 1;
        }
    }

    const Attachment& depth = framebuffer->depth.texture != 0 ? framebuffer->depth : framebuffer->stencil;
    if (depth.texture != 0)
    {
        take(depth, &out->depth, &out->depthLevel);
    }

    if (width == ~0u)
    {
        return false;
    }

    out->width = width;
    out->height = height;
    return true;
}

bool SameAttachments(const Rendering& r, const Attachments& a)
{
    for (u32 i = 0; i < 4; i++)
    {
        VkImage image = i < a.colourCount && a.colour[i] != nullptr ? a.colour[i]->image : VK_NULL_HANDLE;
        if (r.colourImages[i] != image || (image != VK_NULL_HANDLE && r.colourLevels[i] != a.colourLevel[i]))
        {
            return false;
        }
    }

    return r.depthImage == (a.depth != nullptr ? a.depth->image : VK_NULL_HANDLE) && r.width == a.width && r.height == a.height;
}

// A rendering instance into the attachments, its first touch a clear of each when clears are given (else their contents kept)
void BeginRendering(Context& c, const Attachments& a, const VkClearValue* colourClear, const VkClearValue* depthClear,
                    bool clearStencil)
{
    Barrier(c);
    VkRenderingAttachmentInfo colours[4] = {};
    for (u32 i = 0; i < a.colourCount; i++)
    {
        VkRenderingAttachmentInfo& info = colours[i];
        info.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        if (a.colour[i] == nullptr)
        {
            info.imageView = VK_NULL_HANDLE;
            continue;
        }

        info.imageView = ViewOf(*a.colour[i], a.colourLevel[i], 1, VK_IMAGE_ASPECT_COLOR_BIT);
        info.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        info.loadOp = colourClear != nullptr ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
        info.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        if (colourClear != nullptr)
        {
            info.clearValue = *colourClear;
        }
    }

    VkRenderingAttachmentInfo depth = {};
    VkRenderingAttachmentInfo stencil = {};
    const bool hasStencil = a.depth != nullptr && (a.depth->aspects & VK_IMAGE_ASPECT_STENCIL_BIT) != 0;
    if (a.depth != nullptr)
    {
        depth.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        depth.imageView = ViewOf(*a.depth, a.depthLevel, 1, a.depth->aspects);
        depth.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        depth.loadOp = depthClear != nullptr ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
        depth.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        if (depthClear != nullptr)
        {
            depth.clearValue = *depthClear;
        }

        stencil = depth;
        stencil.loadOp = clearStencil ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
        if (clearStencil && depthClear != nullptr)
        {
            stencil.clearValue = *depthClear;
        }
        else if (clearStencil)
        {
            stencil.clearValue.depthStencil = {1.0f, static_cast<u32>(c.clearStencil)};
        }

        if (depthClear == nullptr && clearStencil)
        {
            // (the depth kept, the stencil cleared)
            depth.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        }
    }

    VkRenderingInfo info = {};
    info.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    info.renderArea = {{0, 0}, {a.width, a.height}};
    info.layerCount = 1;
    info.colorAttachmentCount = a.colourCount;
    info.pColorAttachments = colours;
    info.pDepthAttachment = a.depth != nullptr ? &depth : nullptr;
    info.pStencilAttachment = hasStencil ? &stencil : nullptr;
    vkCmdBeginRendering(Commands(c), &info);
    Rendering& r = c.rendering;
    r = Rendering{};
    r.active = true;
    for (u32 i = 0; i < 4; i++)
    {
        r.colourImages[i] = i < a.colourCount && a.colour[i] != nullptr ? a.colour[i]->image : VK_NULL_HANDLE;
        r.colourLevels[i] = i < a.colourCount ? a.colourLevel[i] : 0;
    }

    r.depthImage = a.depth != nullptr ? a.depth->image : VK_NULL_HANDLE;
    r.width = a.width;
    r.height = a.height;
    c.boundPipeline = VK_NULL_HANDLE;
    c.dynamicKnown = false;
}

void EnsureRendering(Context& c, const Attachments& a)
{
    Commands(c);
    if (c.rendering.active && SameAttachments(c.rendering, a))
    {
        return;
    }

    EndRendering(c);
    BeginRendering(c, a, nullptr, nullptr, false);
}

// ------------------------------------------------------------------------------------------------------------------ pipelines
struct PipelineKey
{
    VkPipelineLayout layout;
    VkShaderModule vertex;
    VkShaderModule fragment;
    u32 topologyClass;
    u32 attributeCount;
    u32 locations[16];
    VkFormat formats[16];
    u32 strides[16];
    u32 colourCount;
    VkFormat colourFormats[4];
    VkFormat depthFormat;
    VkFormat stencilFormat;
    u32 blend;
    u32 blendFactors[4];
    u32 blendOps[2];
    u32 writeMask;
    u32 integerColour;
};

struct KeyHash
{
    size_t operator()(const std::string& key) const
    {
        u64 h = 1469598103934665603ull;
        for (char ch : key)
        {
            h = (h ^ static_cast<u8>(ch)) * 1099511628211ull;
        }

        return static_cast<size_t>(h);
    }
};

std::mutex g_PipelinesLock;
std::unordered_map<std::string, VkPipeline, KeyHash> g_Pipelines;

VkBlendFactor Factor(GLenum factor)
{
    switch (factor)
    {
    case Gl::ZERO:
        return VK_BLEND_FACTOR_ZERO;
    case Gl::ONE:
        return VK_BLEND_FACTOR_ONE;
    case Gl::SRC_COLOR:
        return VK_BLEND_FACTOR_SRC_COLOR;
    case Gl::ONE_MINUS_SRC_COLOR:
        return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
    case Gl::SRC_ALPHA:
        return VK_BLEND_FACTOR_SRC_ALPHA;
    case Gl::ONE_MINUS_SRC_ALPHA:
        return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    case Gl::DST_ALPHA:
        return VK_BLEND_FACTOR_DST_ALPHA;
    case Gl::ONE_MINUS_DST_ALPHA:
        return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
    case Gl::DST_COLOR:
        return VK_BLEND_FACTOR_DST_COLOR;
    case Gl::ONE_MINUS_DST_COLOR:
        return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
    case Gl::CONSTANT_COLOR:
        return VK_BLEND_FACTOR_CONSTANT_COLOR;
    case Gl::ONE_MINUS_CONSTANT_COLOR:
        return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
    case Gl::CONSTANT_ALPHA:
        return VK_BLEND_FACTOR_CONSTANT_ALPHA;
    case Gl::ONE_MINUS_CONSTANT_ALPHA:
        return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA;
    case Gl::SRC1_ALPHA:
        return VK_BLEND_FACTOR_SRC1_ALPHA;
    case Gl::ONE_MINUS_SRC1_ALPHA:
        return VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA;
    case Gl::SRC1_COLOR:
        return VK_BLEND_FACTOR_SRC1_COLOR;
    case Gl::ONE_MINUS_SRC1_COLOR:
        return VK_BLEND_FACTOR_ONE_MINUS_SRC1_COLOR;
    default:
        return VK_BLEND_FACTOR_ONE;
    }
}

VkBlendOp Op(GLenum op)
{
    switch (op)
    {
    case Gl::FUNC_SUBTRACT:
        return VK_BLEND_OP_SUBTRACT;
    case Gl::FUNC_REVERSE_SUBTRACT:
        return VK_BLEND_OP_REVERSE_SUBTRACT;
    case Gl::MIN:
        return VK_BLEND_OP_MIN;
    case Gl::MAX:
        return VK_BLEND_OP_MAX;
    default:
        return VK_BLEND_OP_ADD;
    }
}

VkCompareOp Compare(GLenum func)
{
    switch (func)
    {
    case Gl::NEVER:
        return VK_COMPARE_OP_NEVER;
    case Gl::LESS:
        return VK_COMPARE_OP_LESS;
    case Gl::EQUAL:
        return VK_COMPARE_OP_EQUAL;
    case Gl::LEQUAL:
        return VK_COMPARE_OP_LESS_OR_EQUAL;
    case Gl::GREATER:
        return VK_COMPARE_OP_GREATER;
    case Gl::NOTEQUAL:
        return VK_COMPARE_OP_NOT_EQUAL;
    case Gl::GEQUAL:
        return VK_COMPARE_OP_GREATER_OR_EQUAL;
    default:
        return VK_COMPARE_OP_ALWAYS;
    }
}

VkStencilOp StencilOp(GLenum op)
{
    switch (op)
    {
    case Gl::ZERO:
        return VK_STENCIL_OP_ZERO;
    case Gl::REPLACE:
        return VK_STENCIL_OP_REPLACE;
    case Gl::INCR:
        return VK_STENCIL_OP_INCREMENT_AND_CLAMP;
    case Gl::DECR:
        return VK_STENCIL_OP_DECREMENT_AND_CLAMP;
    case Gl::INVERT:
        return VK_STENCIL_OP_INVERT;
    case Gl::INCR_WRAP:
        return VK_STENCIL_OP_INCREMENT_AND_WRAP;
    case Gl::DECR_WRAP:
        return VK_STENCIL_OP_DECREMENT_AND_WRAP;
    default:
        return VK_STENCIL_OP_KEEP;
    }
}

VkPrimitiveTopology Topology(GLenum mode)
{
    switch (mode)
    {
    case Gl::POINTS:
        return VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
    case Gl::LINES:
        return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
    case Gl::LINE_STRIP:
        return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
    case Gl::TRIANGLE_STRIP:
        return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
    case Gl::TRIANGLE_FAN:
        return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN;
    default:
        return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    }
}

u32 TopologyClass(GLenum mode)
{
    return mode == Gl::POINTS ? 0u : (mode == Gl::LINES || mode == Gl::LINE_STRIP) ? 1u : 2u;
}

// A vertex attribute's format; supported for vertex buffers as given (unnormalised bytes as floats are USCALED, which not every GPU
// has: those are made floats as they're copied)
VkFormat AttributeFormat(const VertexAttrib& a, bool* convert)
{
    *convert = false;
    const int n = std::clamp(a.size, 1, 4);
    static const VkFormat floats[4] = {VK_FORMAT_R32_SFLOAT, VK_FORMAT_R32G32_SFLOAT, VK_FORMAT_R32G32B32_SFLOAT,
                                       VK_FORMAT_R32G32B32A32_SFLOAT};
    switch (a.type)
    {
    case Gl::FLOAT:
        return floats[n - 1];
    case Gl::UNSIGNED_INT:
    {
        static const VkFormat u[4] = {VK_FORMAT_R32_UINT, VK_FORMAT_R32G32_UINT, VK_FORMAT_R32G32B32_UINT, VK_FORMAT_R32G32B32A32_UINT};
        static const VkFormat f[4] = {VK_FORMAT_R32_UINT, VK_FORMAT_R32G32_UINT, VK_FORMAT_R32G32B32_UINT, VK_FORMAT_R32G32B32A32_UINT};
        return a.integer ? u[n - 1] : f[n - 1];
    }
    case Gl::INT:
    {
        static const VkFormat s[4] = {VK_FORMAT_R32_SINT, VK_FORMAT_R32G32_SINT, VK_FORMAT_R32G32B32_SINT, VK_FORMAT_R32G32B32A32_SINT};
        return s[n - 1];
    }
    case Gl::UNSIGNED_BYTE:
    {
        static const VkFormat unorm[4] = {VK_FORMAT_R8_UNORM, VK_FORMAT_R8G8_UNORM, VK_FORMAT_R8G8B8_UNORM, VK_FORMAT_R8G8B8A8_UNORM};
        static const VkFormat uint[4] = {VK_FORMAT_R8_UINT, VK_FORMAT_R8G8_UINT, VK_FORMAT_R8G8B8_UINT, VK_FORMAT_R8G8B8A8_UINT};
        static const VkFormat scaled[4] = {VK_FORMAT_R8_USCALED, VK_FORMAT_R8G8_USCALED, VK_FORMAT_R8G8B8_USCALED,
                                           VK_FORMAT_R8G8B8A8_USCALED};
        if (a.integer)
        {
            return uint[n - 1];
        }

        if (a.normalized)
        {
            return unorm[n - 1];
        }

        static const bool scaledVertexes = []
        {
            VkFormatProperties p = {};
            vkGetPhysicalDeviceFormatProperties(GetDevice().physical, VK_FORMAT_R8G8B8A8_USCALED, &p);
            return (p.bufferFeatures & VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT) != 0;
        }();
        if (scaledVertexes)
        {
            return scaled[n - 1];
        }

        *convert = true;
        return floats[n - 1];
    }
    case Gl::UNSIGNED_SHORT:
    {
        static const VkFormat unorm[4] = {VK_FORMAT_R16_UNORM, VK_FORMAT_R16G16_UNORM, VK_FORMAT_R16G16B16_UNORM,
                                          VK_FORMAT_R16G16B16A16_UNORM};
        static const VkFormat uint[4] = {VK_FORMAT_R16_UINT, VK_FORMAT_R16G16_UINT, VK_FORMAT_R16G16B16_UINT, VK_FORMAT_R16G16B16A16_UINT};
        return a.integer || !a.normalized ? uint[n - 1] : unorm[n - 1];
    }
    default:
        return floats[n - 1];
    }
}

u32 AttributeBytes(const VertexAttrib& a)
{
    const u32 n = static_cast<u32>(std::clamp(a.size, 1, 4));
    switch (a.type)
    {
    case Gl::UNSIGNED_BYTE:
    case Gl::BYTE:
        return n;
    case Gl::UNSIGNED_SHORT:
    case Gl::SHORT:
    case Gl::HALF_FLOAT:
        return n * 2;
    default:
        return n * 4;
    }
}

VkPipeline PipelineFor(const PipelineKey& key)
{
    std::string bytes(reinterpret_cast<const char*>(&key), sizeof(key));
    {
        std::lock_guard lock(g_PipelinesLock);
        auto found = g_Pipelines.find(bytes);
        if (found != g_Pipelines.end())
        {
            return found->second;
        }
    }

    Device& d = GetDevice();
    VkPipelineShaderStageCreateInfo stages[2] = {};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = key.vertex;
    stages[0].pName = "main";
    stages[1] = stages[0];
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = key.fragment;
    VkVertexInputBindingDescription bindings[16] = {};
    VkVertexInputAttributeDescription attributes[16] = {};
    for (u32 i = 0; i < key.attributeCount; i++)
    {
        bindings[i] = {i, key.strides[i], VK_VERTEX_INPUT_RATE_VERTEX};
        attributes[i] = {key.locations[i], i, key.formats[i], 0};
    }

    VkPipelineVertexInputStateCreateInfo vertexInput = {};
    vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertexInput.vertexBindingDescriptionCount = key.attributeCount;
    vertexInput.pVertexBindingDescriptions = bindings;
    vertexInput.vertexAttributeDescriptionCount = key.attributeCount;
    vertexInput.pVertexAttributeDescriptions = attributes;
    VkPipelineInputAssemblyStateCreateInfo assembly = {};
    assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    assembly.topology = key.topologyClass == 0   ? VK_PRIMITIVE_TOPOLOGY_POINT_LIST
                        : key.topologyClass == 1 ? VK_PRIMITIVE_TOPOLOGY_LINE_LIST
                                                 : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo viewport = {};
    viewport.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewport.viewportCount = 1;
    viewport.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo raster = {};
    raster.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode = VK_CULL_MODE_NONE;
    raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    raster.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo multisample = {};
    multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo depthStencil = {};
    depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    VkPipelineColorBlendAttachmentState blends[4] = {};
    for (u32 i = 0; i < key.colourCount; i++)
    {
        VkPipelineColorBlendAttachmentState& b = blends[i];
        // (integer attachments don't blend)
        b.blendEnable = key.blend != 0 && (key.integerColour & (1u << i)) == 0 ? VK_TRUE : VK_FALSE;
        b.srcColorBlendFactor = static_cast<VkBlendFactor>(key.blendFactors[0]);
        b.dstColorBlendFactor = static_cast<VkBlendFactor>(key.blendFactors[1]);
        b.srcAlphaBlendFactor = static_cast<VkBlendFactor>(key.blendFactors[2]);
        b.dstAlphaBlendFactor = static_cast<VkBlendFactor>(key.blendFactors[3]);
        b.colorBlendOp = static_cast<VkBlendOp>(key.blendOps[0]);
        b.alphaBlendOp = static_cast<VkBlendOp>(key.blendOps[1]);
        b.colorWriteMask = key.writeMask;
    }

    VkPipelineColorBlendStateCreateInfo blend = {};
    blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    blend.attachmentCount = key.colourCount;
    blend.pAttachments = blends;
    std::vector<VkDynamicState> dynamics = {
        VK_DYNAMIC_STATE_VIEWPORT,           VK_DYNAMIC_STATE_SCISSOR,           VK_DYNAMIC_STATE_BLEND_CONSTANTS,
        VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK, VK_DYNAMIC_STATE_STENCIL_WRITE_MASK, VK_DYNAMIC_STATE_STENCIL_REFERENCE,
        VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE,  VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE, VK_DYNAMIC_STATE_DEPTH_COMPARE_OP,
        VK_DYNAMIC_STATE_STENCIL_TEST_ENABLE, VK_DYNAMIC_STATE_STENCIL_OP,      VK_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY,
        VK_DYNAMIC_STATE_CULL_MODE,          VK_DYNAMIC_STATE_FRONT_FACE,
    };
    VkPipelineDynamicStateCreateInfo dynamic = {};
    dynamic.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamic.dynamicStateCount = static_cast<u32>(dynamics.size());
    dynamic.pDynamicStates = dynamics.data();
    VkPipelineRenderingCreateInfo rendering = {};
    rendering.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    rendering.colorAttachmentCount = key.colourCount;
    rendering.pColorAttachmentFormats = key.colourFormats;
    rendering.depthAttachmentFormat = key.depthFormat;
    rendering.stencilAttachmentFormat = key.stencilFormat;
    VkGraphicsPipelineCreateInfo info = {};
    info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    info.pNext = &rendering;
    info.stageCount = 2;
    info.pStages = stages;
    info.pVertexInputState = &vertexInput;
    info.pInputAssemblyState = &assembly;
    info.pViewportState = &viewport;
    info.pRasterizationState = &raster;
    info.pMultisampleState = &multisample;
    info.pDepthStencilState = &depthStencil;
    info.pColorBlendState = &blend;
    info.pDynamicState = &dynamic;
    info.layout = key.layout;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkResult result = vkCreateGraphicsPipelines(d.device, d.pipelineCache, 1, &info, nullptr, &pipeline);
    if (result != VK_SUCCESS)
    {
        Log("vkCreateGraphicsPipelines: %d", result);
    }

    std::lock_guard lock(g_PipelinesLock);
    g_Pipelines[bytes] = pipeline;
    return pipeline;
}

// A texture to bind where a sampler has none: 1 x 1 of zeroes (a float one, an unsigned integer one)
Texture* Dummy(bool integer)
{
    static Texture floatTexture;
    static Texture integerTexture;
    Texture& t = integer ? integerTexture : floatTexture;
    if (t.image == VK_NULL_HANDLE)
    {
        t.internal = integer ? Gl::R32UI : Gl::RGBA8;
        t.given.assign(1, {1, 1});
        t.minFilter = Gl::NEAREST;
        t.magFilter = Gl::NEAREST;
        EnsureImage(t);
    }

    return &t;
}

bool IsIntegerSamplerType(GLenum type)
{
    // GL's INT_SAMPLER_2D and UNSIGNED_INT_SAMPLER_2D
    return type == 0x8DCA || type == 0x8DD2;
}

// The descriptor set of a program's draw: its uniform block (the program's values now, copied into the stream) and its samplers'
// textures (each unit's, with its sampler object or the texture's own parameters)
bool BindResources(Context& c, Program& program, VkPipelineBindPoint point)
{
    VkDescriptorSet set = AllocateSet(c, program.setLayout);
    if (set == VK_NULL_HANDLE)
    {
        return false;
    }

    Device& d = GetDevice();
    std::vector<VkWriteDescriptorSet> writes;
    std::vector<VkDescriptorImageInfo> images(program.samplerUnits.size() + 1);
    VkDescriptorBufferInfo buffer = {};
    u32 dynamicOffset = 0;
    if (program.hasBlock)
    {
        const VkDeviceSize alignment = std::max<VkDeviceSize>(d.properties.limits.minUniformBufferOffsetAlignment, 16);
        const VkDeviceSize size = std::max<size_t>(program.block.size(), 16);
        StreamPiece piece = StreamAllocate(c, size, alignment);
        if (piece.mapped == nullptr)
        {
            return false;
        }

        std::memcpy(piece.mapped, program.block.data(), program.block.size());
        buffer.buffer = piece.buffer;
        buffer.offset = 0;
        buffer.range = size;
        dynamicOffset = static_cast<u32>(piece.offset);
        VkWriteDescriptorSet write = {};
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = set;
        write.dstBinding = 0;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
        write.pBufferInfo = &buffer;
        writes.push_back(write);
    }

    for (size_t i = 0; i < program.samplerUnits.size(); i++)
    {
        const u32 binding = static_cast<u32>(i + 1);
        bool wantsInteger = false;
        for (const Uniform& u : program.uniforms)
        {
            if (u.sampler && u.binding == binding)
            {
                wantsInteger = IsIntegerSamplerType(u.glType);
            }
        }

        const u32 unit = std::min<u32>(program.samplerUnits[i], 31);
        Texture* texture = FindTexture(c.textures[unit]);
        if (texture == nullptr || !EnsureImage(*texture))
        {
            texture = Dummy(wantsInteger);
        }

        SamplerObject* sampler = FindSampler(c.samplers[unit]);
        const u32 first = static_cast<u32>(std::max(0, texture->baseLevel));
        const u32 last = static_cast<u32>(std::clamp(texture->maxLevel, texture->baseLevel, static_cast<GLint>(texture->levels) - 1));
        const u32 count = last >= first ? last - first + 1 : 1;
        const VkImageAspectFlags aspect = texture->depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
        VkDescriptorImageInfo& image = images[i];
        image.imageView = ViewOf(*texture, first, count, aspect);
        image.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        image.sampler = sampler != nullptr ? SamplerFor(sampler->minFilter, sampler->magFilter, sampler->wrapS, sampler->wrapT,
                                                        sampler->anisotropy, sampler->lodBias, texture->integer, count)
                                           : SamplerFor(texture->minFilter, texture->magFilter, texture->wrapS, texture->wrapT,
                                                        texture->anisotropy, texture->lodBias, texture->integer, count);
        VkWriteDescriptorSet write = {};
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = set;
        write.dstBinding = binding;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        write.pImageInfo = &image;
        writes.push_back(write);
    }

    vkUpdateDescriptorSets(d.device, static_cast<u32>(writes.size()), writes.data(), 0, nullptr);
    vkCmdBindDescriptorSets(Commands(c), point, program.layout, 0, 1, &set, program.hasBlock ? 1 : 0, &dynamicOffset);
    return true;
}

// The dynamic state as the context has it (the viewport, the scissor within the attachments, the depth and stencil tests)
void SetDynamicState(Context& c, const Attachments& a, GLenum mode, bool hasDepth, bool hasStencil)
{
    VkCommandBuffer commands = Commands(c);
    VkViewport viewport = {static_cast<f32>(c.viewport[0]), static_cast<f32>(c.viewport[1]), static_cast<f32>(std::max(1, c.viewport[2])),
                           static_cast<f32>(std::max(1, c.viewport[3])), 0.0f, 1.0f};
    vkCmdSetViewport(commands, 0, 1, &viewport);
    s32 x0 = 0;
    s32 y0 = 0;
    s32 x1 = static_cast<s32>(a.width);
    s32 y1 = static_cast<s32>(a.height);
    if (c.scissorTest)
    {
        x0 = std::max(x0, c.scissor[0]);
        y0 = std::max(y0, c.scissor[1]);
        x1 = std::min(x1, c.scissor[0] + c.scissor[2]);
        y1 = std::min(y1, c.scissor[1] + c.scissor[3]);
    }

    VkRect2D scissor = {{x0, y0}, {static_cast<u32>(std::max(0, x1 - x0)), static_cast<u32>(std::max(0, y1 - y0))}};
    vkCmdSetScissor(commands, 0, 1, &scissor);
    vkCmdSetBlendConstants(commands, c.blendColour);
    vkCmdSetDepthTestEnable(commands, c.depthTest && hasDepth ? VK_TRUE : VK_FALSE);
    vkCmdSetDepthWriteEnable(commands, c.depthTest && hasDepth && c.depthMask ? VK_TRUE : VK_FALSE);
    vkCmdSetDepthCompareOp(commands, Compare(c.depthFunc));
    vkCmdSetStencilTestEnable(commands, c.stencilTest && hasStencil ? VK_TRUE : VK_FALSE);
    vkCmdSetStencilOp(commands, VK_STENCIL_FACE_FRONT_AND_BACK, StencilOp(c.stencilFail), StencilOp(c.stencilPass),
                      StencilOp(c.stencilDepthFail), Compare(c.stencilFunc));
    vkCmdSetStencilCompareMask(commands, VK_STENCIL_FACE_FRONT_AND_BACK, c.stencilValueMask & 0xFF);
    vkCmdSetStencilWriteMask(commands, VK_STENCIL_FACE_FRONT_AND_BACK, c.stencilWriteMask & 0xFF);
    vkCmdSetStencilReference(commands, VK_STENCIL_FACE_FRONT_AND_BACK, static_cast<u32>(c.stencilRef) & 0xFF);
    vkCmdSetPrimitiveTopology(commands, Topology(mode));
    vkCmdSetCullMode(commands, VK_CULL_MODE_NONE);
    vkCmdSetFrontFace(commands, VK_FRONT_FACE_COUNTER_CLOCKWISE);
}

bool ScissorEmpty(const Context& c, const Attachments& a)
{
    if (!c.scissorTest)
    {
        return false;
    }

    const s32 x0 = std::max(0, c.scissor[0]);
    const s32 y0 = std::max(0, c.scissor[1]);
    const s32 x1 = std::min(static_cast<s32>(a.width), c.scissor[0] + c.scissor[2]);
    const s32 y1 = std::min(static_cast<s32>(a.height), c.scissor[1] + c.scissor[3]);
    return x1 <= x0 || y1 <= y0;
}
}

void Draw(Context& c, GLenum mode, GLint first, GLsizei count)
{
    if (count <= 0)
    {
        return;
    }

    Program* program = FindProgram(c.program);
    if (program == nullptr || !program->linked)
    {
        return;
    }

    Attachments a;
    if (!Gather(DrawFramebuffer(c), &a) || ScissorEmpty(c, a))
    {
        return;
    }

    EnsureRendering(c, a);
    VkCommandBuffer commands = Commands(c);
    // The vertexes: each attribute the program reads, its buffer's data copied into the stream (once a buffer), bound by itself
    VertexArray* vao = FindVertexArray(c.vertexArray);
    PipelineKey key = {};
    key.layout = program->layout;
    key.vertex = program->vertex;
    key.fragment = program->fragment;
    key.topologyClass = TopologyClass(mode);
    VkBuffer buffers[16] = {};
    VkDeviceSize offsets[16] = {};
    std::map<GLuint, StreamPiece> copied;
    for (const auto& input : program->inputs)
    {
        if (vao == nullptr || input.first >= 16 || !vao->attribs[input.first].enabled)
        {
            continue;
        }

        const VertexAttrib& attrib = vao->attribs[input.first];
        BufferObject* buffer = FindBuffer(attrib.buffer);
        if (buffer == nullptr || buffer->data.empty())
        {
            continue;
        }

        bool convert = false;
        const VkFormat format = AttributeFormat(attrib, &convert);
        const u32 bytes = AttributeBytes(attrib);
        const u32 stride = attrib.stride != 0 ? static_cast<u32>(attrib.stride) : bytes;
        const u32 i = key.attributeCount++;
        key.locations[i] = input.first;
        key.formats[i] = format;
        if (convert)
        {
            // Unnormalised bytes as floats, a vertex at a time
            const u32 n = static_cast<u32>(std::clamp(attrib.size, 1, 4));
            StreamPiece piece = StreamAllocate(c, static_cast<VkDeviceSize>(first + count) * n * 4, 16);
            auto* to = reinterpret_cast<f32*>(piece.mapped);
            for (GLint v = 0; v < first + count; v++)
            {
                const size_t at = static_cast<size_t>(attrib.offset) + static_cast<size_t>(v) * stride;
                for (u32 k = 0; k < n; k++)
                {
                    to[static_cast<size_t>(v) * n + k] = at + k < buffer->data.size() ? static_cast<f32>(buffer->data[at + k]) : 0.0f;
                }
            }

            key.strides[i] = n * 4;
            buffers[i] = piece.buffer;
            offsets[i] = piece.offset;
            continue;
        }

        auto found = copied.find(attrib.buffer);
        if (found == copied.end())
        {
            StreamPiece piece = StreamAllocate(c, buffer->data.size(), 16);
            if (piece.mapped == nullptr)
            {
                return;
            }

            std::memcpy(piece.mapped, buffer->data.data(), buffer->data.size());
            found = copied.emplace(attrib.buffer, piece).first;
        }

        key.strides[i] = stride;
        buffers[i] = found->second.buffer;
        offsets[i] = found->second.offset + static_cast<VkDeviceSize>(attrib.offset);
    }

    key.colourCount = a.colourCount;
    for (u32 i = 0; i < a.colourCount; i++)
    {
        key.colourFormats[i] = a.colour[i] != nullptr ? a.colour[i]->format : VK_FORMAT_UNDEFINED;
        if (a.colour[i] != nullptr && a.colour[i]->integer)
        {
            key.integerColour |= 1u << i;
        }
    }

    const bool hasDepth = a.depth != nullptr;
    const bool hasStencil = hasDepth && (a.depth->aspects & VK_IMAGE_ASPECT_STENCIL_BIT) != 0;
    key.depthFormat = hasDepth ? a.depth->format : VK_FORMAT_UNDEFINED;
    key.stencilFormat = hasStencil ? a.depth->format : VK_FORMAT_UNDEFINED;
    key.blend = c.blend ? 1 : 0;
    if (c.blend)
    {
        key.blendFactors[0] = Factor(c.blendSourceRgb);
        key.blendFactors[1] = Factor(c.blendDestinationRgb);
        key.blendFactors[2] = Factor(c.blendSourceAlpha);
        key.blendFactors[3] = Factor(c.blendDestinationAlpha);
        key.blendOps[0] = Op(c.blendEquationRgb);
        key.blendOps[1] = Op(c.blendEquationAlpha);
    }

    key.writeMask = (c.colourMask[0] ? VK_COLOR_COMPONENT_R_BIT : 0) | (c.colourMask[1] ? VK_COLOR_COMPONENT_G_BIT : 0) |
                    (c.colourMask[2] ? VK_COLOR_COMPONENT_B_BIT : 0) | (c.colourMask[3] ? VK_COLOR_COMPONENT_A_BIT : 0);
    VkPipeline pipeline = PipelineFor(key);
    if (pipeline == VK_NULL_HANDLE)
    {
        return;
    }

    if (pipeline != c.boundPipeline)
    {
        vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        c.boundPipeline = pipeline;
    }

    SetDynamicState(c, a, mode, hasDepth, hasStencil);
    if (!BindResources(c, *program, VK_PIPELINE_BIND_POINT_GRAPHICS))
    {
        return;
    }

    if (key.attributeCount != 0)
    {
        vkCmdBindVertexBuffers(commands, 0, key.attributeCount, buffers, offsets);
    }

    vkCmdDraw(commands, static_cast<u32>(count), 1, static_cast<u32>(first), 0);
    c.draws++;
    // A long recording handed to the GPU now and then (it starts on it while the rest is recorded)
    if (c.draws >= 4096)
    {
        Submit(c, false);
    }
}

void Clear(Context& c, GLbitfield mask)
{
    Attachments a;
    if (!Gather(DrawFramebuffer(c), &a) || ScissorEmpty(c, a))
    {
        return;
    }

    const bool colour = (mask & Gl::COLOR_BUFFER_BIT) != 0 && a.colourCount != 0;
    const bool depth = (mask & Gl::DEPTH_BUFFER_BIT) != 0 && a.depth != nullptr && c.depthMask;
    const bool stencil = (mask & Gl::STENCIL_BUFFER_BIT) != 0 && a.depth != nullptr && (a.depth->aspects & VK_IMAGE_ASPECT_STENCIL_BIT) != 0 &&
                         (c.stencilWriteMask & 0xFF) != 0;
    if (!colour && !depth && !stencil)
    {
        return;
    }

    if (colour && !(c.colourMask[0] && c.colourMask[1] && c.colourMask[2] && c.colourMask[3]))
    {
        static bool told = false;
        if (!told)
        {
            told = true;
            Log("a clear with a partial colour mask: the whole colour cleared");
        }
    }

    VkClearValue colourValue = {};
    std::memcpy(colourValue.color.float32, c.clearColour, sizeof(c.clearColour));
    for (u32 i = 0; i < a.colourCount; i++)
    {
        if (a.colour[i] != nullptr && a.colour[i]->integer)
        {
            for (int k = 0; k < 4; k++)
            {
                colourValue.color.uint32[k] = static_cast<u32>(c.clearColour[k]);
            }
        }
    }

    VkClearValue depthValue = {};
    depthValue.depthStencil = {c.clearDepth, static_cast<u32>(c.clearStencil) & 0xFF};
    // The whole of every attachment cleared, nothing drawn into them yet: the rendering instance begins with the clear
    const bool whole = !c.scissorTest || (c.scissor[0] <= 0 && c.scissor[1] <= 0 && c.scissor[0] + c.scissor[2] >= static_cast<s32>(a.width) &&
                                          c.scissor[1] + c.scissor[3] >= static_cast<s32>(a.height));
    const bool everyAttachment = (colour || a.colourCount == 0) && (a.depth == nullptr || depth || (!depth && !stencil && false));
    Commands(c);
    if (whole && !(c.rendering.active && SameAttachments(c.rendering, a)) && colour && (a.depth == nullptr) && everyAttachment)
    {
        EndRendering(c);
        BeginRendering(c, a, &colourValue, nullptr, false);
        return;
    }

    EnsureRendering(c, a);
    std::vector<VkClearAttachment> clears;
    if (colour)
    {
        for (u32 i = 0; i < a.colourCount; i++)
        {
            if (a.colour[i] != nullptr)
            {
                clears.push_back({VK_IMAGE_ASPECT_COLOR_BIT, i, colourValue});
            }
        }
    }

    if (depth || stencil)
    {
        VkClearAttachment clear = {};
        clear.aspectMask = (depth ? VK_IMAGE_ASPECT_DEPTH_BIT : 0) | (stencil ? VK_IMAGE_ASPECT_STENCIL_BIT : 0);
        clear.clearValue = depthValue;
        clears.push_back(clear);
    }

    s32 x0 = 0;
    s32 y0 = 0;
    s32 x1 = static_cast<s32>(a.width);
    s32 y1 = static_cast<s32>(a.height);
    if (c.scissorTest)
    {
        x0 = std::max(x0, c.scissor[0]);
        y0 = std::max(y0, c.scissor[1]);
        x1 = std::min(x1, c.scissor[0] + c.scissor[2]);
        y1 = std::min(y1, c.scissor[1] + c.scissor[3]);
    }

    VkClearRect rect = {{{x0, y0}, {static_cast<u32>(x1 - x0), static_cast<u32>(y1 - y0)}}, 0, 1};
    vkCmdClearAttachments(Commands(c), static_cast<u32>(clears.size()), clears.data(), 1, &rect);
}

// ------------------------------------------------------------------------------------------------------------------ uploads
namespace
{
// The bytes of a texel as the client's format and type give it
u32 ClientTexelBytes(GLenum format, GLenum type)
{
    u32 components = 4;
    switch (format)
    {
    case Gl::RED:
    case Gl::RED_INTEGER:
    case Gl::DEPTH_COMPONENT:
    case Gl::STENCIL_INDEX:
        components = 1;
        break;
    case Gl::RG:
    case Gl::RG_INTEGER:
        components = 2;
        break;
    case Gl::RGB:
        components = 3;
        break;
    case Gl::DEPTH_STENCIL:
        return type == Gl::FLOAT_32_UNSIGNED_INT_24_8_REV ? 8 : 4;
    default:
        components = 4;
        break;
    }

    switch (type)
    {
    case Gl::UNSIGNED_BYTE:
    case Gl::BYTE:
        return components;
    case Gl::UNSIGNED_SHORT:
    case Gl::SHORT:
    case Gl::HALF_FLOAT:
        return components * 2;
    default:
        return components * 4;
    }
}
}

void TexSubImage(Context& c, Texture& texture, GLint level, GLint x, GLint y, GLsizei width, GLsizei height, GLenum format, GLenum type,
                 const void* pixels)
{
    if (width <= 0 || height <= 0 || !EnsureImage(texture) || static_cast<u32>(level) >= texture.levels)
    {
        return;
    }

    // The client's rows: from the unpack buffer when one's bound (pixels its offset), else from memory
    const u8* source = static_cast<const u8*>(pixels);
    if (c.unpackBuffer != 0)
    {
        BufferObject* buffer = FindBuffer(c.unpackBuffer);
        if (buffer == nullptr)
        {
            return;
        }

        source = buffer->data.data() + reinterpret_cast<uintptr_t>(pixels);
    }

    if (source == nullptr)
    {
        return;
    }

    const u32 texel = ClientTexelBytes(format, type);
    u32 imageTexel = BytesPerTexel(texture.format);
    VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    if (texture.depth)
    {
        aspect = VK_IMAGE_ASPECT_DEPTH_BIT;
        imageTexel = 4;
    }

    if (texel != imageTexel)
    {
        Log("an upload of %u byte texels into an image of %u byte ones (format 0x%x type 0x%x): left out", texel, imageTexel, format, type);
        return;
    }

    const u32 rowLength = c.unpackRowLength > 0 ? static_cast<u32>(c.unpackRowLength) : static_cast<u32>(width);
    const u32 alignment = static_cast<u32>(std::max(1, c.unpackAlignment));
    const size_t pitch = (static_cast<size_t>(rowLength) * texel + alignment - 1) / alignment * alignment;
    const size_t row = static_cast<size_t>(width) * texel;
    StreamPiece piece = StreamAllocate(c, row * static_cast<size_t>(height), std::max<u32>(texel, 4));
    if (piece.mapped == nullptr)
    {
        return;
    }

    if (pitch == row)
    {
        std::memcpy(piece.mapped, source, row * static_cast<size_t>(height));
    }
    else
    {
        for (GLsizei r = 0; r < height; r++)
        {
            std::memcpy(piece.mapped + static_cast<size_t>(r) * row, source + static_cast<size_t>(r) * pitch, row);
        }
    }

    Barrier(c);
    VkBufferImageCopy copy = {};
    copy.bufferOffset = piece.offset;
    copy.imageSubresource = {aspect, static_cast<u32>(level), 0, 1};
    copy.imageOffset = {x, y, 0};
    copy.imageExtent = {static_cast<u32>(width), static_cast<u32>(height), 1};
    vkCmdCopyBufferToImage(Commands(c), piece.buffer, texture.image, VK_IMAGE_LAYOUT_GENERAL, 1, &copy);
}

void GenerateMipmap(Context& c, Texture& texture)
{
    if (texture.given.empty())
    {
        return;
    }

    // Every level to 1 x 1, each blitted from the one before
    const u32 width = texture.given[0].first;
    const u32 height = texture.given[0].second;
    u32 levels = 1;
    while ((std::max(width, height) >> levels) != 0)
    {
        levels++;
    }

    texture.given.resize(levels);
    for (u32 i = 1; i < levels; i++)
    {
        texture.given[i] = {std::max(1u, width >> i), std::max(1u, height >> i)};
    }

    if (!EnsureImage(texture) || texture.integer || texture.depth)
    {
        return;
    }

    for (u32 i = 1; i < texture.levels; i++)
    {
        Barrier(c);
        VkImageBlit blit = {};
        blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, i - 1, 0, 1};
        blit.srcOffsets[1] = {static_cast<s32>(std::max(1u, width >> (i - 1))), static_cast<s32>(std::max(1u, height >> (i - 1))), 1};
        blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, i, 0, 1};
        blit.dstOffsets[1] = {static_cast<s32>(std::max(1u, width >> i)), static_cast<s32>(std::max(1u, height >> i)), 1};
        vkCmdBlitImage(Commands(c), texture.image, VK_IMAGE_LAYOUT_GENERAL, texture.image, VK_IMAGE_LAYOUT_GENERAL, 1, &blit,
                       VK_FILTER_LINEAR);
    }
}

// ------------------------------------------------------------------------------------------------------------------ reads
namespace
{
f32 HalfToFloat(u16 h)
{
    const u32 sign = (h >> 15) & 1;
    const u32 exponent = (h >> 10) & 31;
    const u32 mantissa = h & 1023;
    f32 value;
    if (exponent == 0)
    {
        value = std::ldexp(static_cast<f32>(mantissa), -24);
    }
    else if (exponent == 31)
    {
        value = mantissa != 0 ? NAN : INFINITY;
    }
    else
    {
        value = std::ldexp(static_cast<f32>(mantissa + 1024), static_cast<int>(exponent) - 25);
    }

    return sign != 0 ? -value : value;
}

ReadBuffer g_Read;
std::mutex g_ReadLock;
}

void ReadPixels(Context& c, GLint x, GLint y, GLsizei width, GLsizei height, GLenum format, GLenum type, void* pixels)
{
    if (width <= 0 || height <= 0 || pixels == nullptr)
    {
        return;
    }

    Framebuffer* framebuffer = ReadFramebuffer(c);
    if (framebuffer == nullptr)
    {
        return;
    }

    const bool depthRead = format == Gl::DEPTH_COMPONENT;
    const Attachment& attachment = depthRead ? (framebuffer->depth.texture != 0 ? framebuffer->depth : framebuffer->stencil)
                                             : framebuffer->colour[framebuffer->readBuffer >= Gl::COLOR_ATTACHMENT0 &&
                                                                           framebuffer->readBuffer <= Gl::COLOR_ATTACHMENT0 + 3
                                                                       ? framebuffer->readBuffer - Gl::COLOR_ATTACHMENT0
                                                                       : 0];
    Texture* texture = FindTexture(attachment.texture);
    if (texture == nullptr || !EnsureImage(*texture))
    {
        return;
    }

    const u32 imageTexel = depthRead ? 4u : BytesPerTexel(texture->format);
    const VkDeviceSize bytes = static_cast<VkDeviceSize>(width) * static_cast<VkDeviceSize>(height) * imageTexel;
    std::lock_guard lock(g_ReadLock);
    if (g_Read.size < bytes)
    {
        DestroyReadBuffer(g_Read);
        if (!MakeReadBuffer(std::max<VkDeviceSize>(bytes, 4ull * 1024 * 1024), &g_Read))
        {
            return;
        }
    }

    Barrier(c);
    VkBufferImageCopy copy = {};
    copy.imageSubresource = {depthRead ? static_cast<VkImageAspectFlags>(VK_IMAGE_ASPECT_DEPTH_BIT) : VK_IMAGE_ASPECT_COLOR_BIT,
                             static_cast<u32>(attachment.level), 0, 1};
    copy.imageOffset = {x, y, 0};
    copy.imageExtent = {static_cast<u32>(width), static_cast<u32>(height), 1};
    vkCmdCopyImageToBuffer(Commands(c), texture->image, VK_IMAGE_LAYOUT_GENERAL, g_Read.buffer, 1, &copy);
    Barrier(c);
    Submit(c, true);
    // Into the client's rows (their length and alignment the pack parameters), converted where the format asked for differs
    const u32 outTexel = ClientTexelBytes(format, type);
    const u32 rowLength = c.packRowLength > 0 ? static_cast<u32>(c.packRowLength) : static_cast<u32>(width);
    const u32 alignment = static_cast<u32>(std::max(1, c.packAlignment));
    const size_t pitch = (static_cast<size_t>(rowLength) * outTexel + alignment - 1) / alignment * alignment;
    const u8* from = g_Read.memory.mapped;
    auto* to = static_cast<u8*>(pixels);
    for (GLsizei r = 0; r < height; r++)
    {
        const u8* in = from + static_cast<size_t>(r) * static_cast<size_t>(width) * imageTexel;
        u8* out = to + static_cast<size_t>(r) * pitch;
        if (outTexel == imageTexel && !(texture->format == VK_FORMAT_R16G16B16A16_SFLOAT && type == Gl::FLOAT))
        {
            std::memcpy(out, in, static_cast<size_t>(width) * imageTexel);
            continue;
        }

        for (GLsizei i = 0; i < width; i++)
        {
            const u8* texelIn = in + static_cast<size_t>(i) * imageTexel;
            u8* texelOut = out + static_cast<size_t>(i) * outTexel;
            if (texture->format == VK_FORMAT_R16G16B16A16_SFLOAT && type == Gl::FLOAT)
            {
                u16 h[4];
                std::memcpy(h, texelIn, 8);
                f32 f[4] = {HalfToFloat(h[0]), HalfToFloat(h[1]), HalfToFloat(h[2]), HalfToFloat(h[3])};
                std::memcpy(texelOut, f, std::min<u32>(outTexel, 16));
            }
            else if (imageTexel == 4 && outTexel == 16)
            {
                // A one channel image read as four (RGBA of R32F or R32UI: the rest 0 and 1)
                u32 value[4] = {0, 0, 0, 0};
                std::memcpy(&value[0], texelIn, 4);
                if (type == Gl::FLOAT)
                {
                    const f32 one = 1.0f;
                    std::memcpy(&value[3], &one, 4);
                }
                else
                {
                    value[3] = 1;
                }

                std::memcpy(texelOut, value, 16);
            }
            else
            {
                std::memset(texelOut, 0, outTexel);
                std::memcpy(texelOut, texelIn, std::min(outTexel, imageTexel));
            }
        }
    }
}

// ------------------------------------------------------------------------------------------------------------------ blits
namespace
{
constexpr const char* BlitVertex = R"(#version 450
void main()
{
    vec2 corner = vec2(gl_VertexIndex & 1, gl_VertexIndex >> 1);
    gl_Position = vec4(corner * 2.0 - 1.0, 0.0, 1.0);
}
)";

// GL's blit: each destination pixel's centre mapped into the source rectangle, the texel there (nearest) or the filtered colour
constexpr const char* BlitColourFragment = R"(#version 450
uniform sampler2D source;
uniform vec4 sourceRect;
uniform vec4 destinationRect;
uniform vec2 sourceSize;
uniform int linear;
out vec4 colour;
void main()
{
    vec2 t = (gl_FragCoord.xy - destinationRect.xy) / (destinationRect.zw - destinationRect.xy);
    vec2 s = sourceRect.xy + t * (sourceRect.zw - sourceRect.xy);
    if (linear != 0)
    {
        colour = textureLod(source, s / sourceSize, 0.0);
    }
    else
    {
        colour = texelFetch(source, clamp(ivec2(floor(s)), ivec2(0), ivec2(sourceSize) - 1), 0);
    }
}
)";

constexpr const char* BlitIntegerFragment = R"(#version 450
uniform usampler2D source;
uniform vec4 sourceRect;
uniform vec4 destinationRect;
uniform vec2 sourceSize;
uniform int linear;
out uvec4 colour;
void main()
{
    vec2 t = (gl_FragCoord.xy - destinationRect.xy) / (destinationRect.zw - destinationRect.xy);
    vec2 s = sourceRect.xy + t * (sourceRect.zw - sourceRect.xy);
    colour = texelFetch(source, clamp(ivec2(floor(s)), ivec2(0), ivec2(sourceSize) - 1), 0);
}
)";

constexpr const char* BlitDepthFragment = R"(#version 450
uniform sampler2D source;
uniform vec4 sourceRect;
uniform vec4 destinationRect;
uniform vec2 sourceSize;
uniform int linear;
void main()
{
    vec2 t = (gl_FragCoord.xy - destinationRect.xy) / (destinationRect.zw - destinationRect.xy);
    vec2 s = sourceRect.xy + t * (sourceRect.zw - sourceRect.xy);
    gl_FragDepth = texelFetch(source, clamp(ivec2(floor(s)), ivec2(0), ivec2(sourceSize) - 1), 0).r;
}
)";

Program g_BlitColour;
Program g_BlitInteger;
Program g_BlitDepth;
std::mutex g_BlitLock;

void Set(Program& p, const char* name, const void* values, u32 components)
{
    for (const Uniform& u : p.uniforms)
    {
        if (!u.sampler && u.name == name && u.offset >= 0 && static_cast<size_t>(u.offset) + components * 4 <= p.block.size())
        {
            std::memcpy(p.block.data() + u.offset, values, components * 4);
        }
    }
}

void BlitOne(Context& c, Texture& source, u32 sourceLevel, const Attachments& destination, bool depth, GLint sx0, GLint sy0, GLint sx1,
             GLint sy1, GLint dx0, GLint dy0, GLint dx1, GLint dy1, bool linear)
{
    std::lock_guard lock(g_BlitLock);
    Program& p = depth ? g_BlitDepth : source.integer ? g_BlitInteger : g_BlitColour;
    if (!p.linked && !InternalProgram(BlitVertex, depth ? BlitDepthFragment : source.integer ? BlitIntegerFragment : BlitColourFragment, &p))
    {
        return;
    }

    // Just the attachment the blit writes
    Attachments a = destination;
    if (depth)
    {
        a.colourCount = 0;
    }
    else
    {
        a.depth = nullptr;
    }

    EnsureRendering(c, a);
    const f32 sourceRect[4] = {static_cast<f32>(sx0), static_cast<f32>(sy0), static_cast<f32>(sx1), static_cast<f32>(sy1)};
    const f32 destinationRect[4] = {static_cast<f32>(dx0), static_cast<f32>(dy0), static_cast<f32>(dx1), static_cast<f32>(dy1)};
    const f32 sourceSize[2] = {static_cast<f32>(std::max(1u, source.width >> sourceLevel)), static_cast<f32>(std::max(1u, source.height >> sourceLevel))};
    const GLint linearValue = linear ? 1 : 0;
    Set(p, "sourceRect", sourceRect, 4);
    Set(p, "destinationRect", destinationRect, 4);
    Set(p, "sourceSize", sourceSize, 2);
    Set(p, "linear", &linearValue, 1);
    PipelineKey key = {};
    key.layout = p.layout;
    key.vertex = p.vertex;
    key.fragment = p.fragment;
    key.topologyClass = 2;
    key.colourCount = a.colourCount;
    for (u32 i = 0; i < a.colourCount; i++)
    {
        key.colourFormats[i] = a.colour[i] != nullptr ? a.colour[i]->format : VK_FORMAT_UNDEFINED;
        key.integerColour |= a.colour[i] != nullptr && a.colour[i]->integer ? (1u << i) : 0u;
    }

    key.depthFormat = a.depth != nullptr ? a.depth->format : VK_FORMAT_UNDEFINED;
    key.stencilFormat = a.depth != nullptr && (a.depth->aspects & VK_IMAGE_ASPECT_STENCIL_BIT) != 0 ? a.depth->format : VK_FORMAT_UNDEFINED;
    key.writeMask = 0xF;
    VkPipeline pipeline = PipelineFor(key);
    VkCommandBuffer commands = Commands(c);
    vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    c.boundPipeline = pipeline;
    const s32 x0 = std::max(0, std::min(dx0, dx1));
    const s32 y0 = std::max(0, std::min(dy0, dy1));
    const s32 x1 = std::min(static_cast<s32>(a.width), std::max(dx0, dx1));
    const s32 y1 = std::min(static_cast<s32>(a.height), std::max(dy0, dy1));
    if (x1 <= x0 || y1 <= y0)
    {
        return;
    }

    VkViewport viewport = {static_cast<f32>(x0), static_cast<f32>(y0), static_cast<f32>(x1 - x0), static_cast<f32>(y1 - y0), 0.0f, 1.0f};
    vkCmdSetViewport(commands, 0, 1, &viewport);
    VkRect2D scissor = {{x0, y0}, {static_cast<u32>(x1 - x0), static_cast<u32>(y1 - y0)}};
    vkCmdSetScissor(commands, 0, 1, &scissor);
    vkCmdSetBlendConstants(commands, c.blendColour);
    vkCmdSetDepthTestEnable(commands, depth ? VK_TRUE : VK_FALSE);
    vkCmdSetDepthWriteEnable(commands, depth ? VK_TRUE : VK_FALSE);
    vkCmdSetDepthCompareOp(commands, VK_COMPARE_OP_ALWAYS);
    vkCmdSetStencilTestEnable(commands, VK_FALSE);
    vkCmdSetStencilOp(commands, VK_STENCIL_FACE_FRONT_AND_BACK, VK_STENCIL_OP_KEEP, VK_STENCIL_OP_KEEP, VK_STENCIL_OP_KEEP, VK_COMPARE_OP_ALWAYS);
    vkCmdSetStencilCompareMask(commands, VK_STENCIL_FACE_FRONT_AND_BACK, 0xFF);
    vkCmdSetStencilWriteMask(commands, VK_STENCIL_FACE_FRONT_AND_BACK, 0);
    vkCmdSetStencilReference(commands, VK_STENCIL_FACE_FRONT_AND_BACK, 0);
    vkCmdSetPrimitiveTopology(commands, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP);
    vkCmdSetCullMode(commands, VK_CULL_MODE_NONE);
    vkCmdSetFrontFace(commands, VK_FRONT_FACE_COUNTER_CLOCKWISE);
    // The source bound as the program's one sampler (its first unit's texture, for this draw only)
    const GLuint savedTexture = c.textures[0];
    const GLuint savedSampler = c.samplers[0];
    c.textures[0] = source.name;
    c.samplers[0] = 0;
    const GLenum savedMin = source.minFilter;
    const GLenum savedMag = source.magFilter;
    const GLint savedBase = source.baseLevel;
    const GLint savedMax = source.maxLevel;
    source.minFilter = linear ? Gl::LINEAR : Gl::NEAREST;
    source.magFilter = linear ? Gl::LINEAR : Gl::NEAREST;
    source.baseLevel = static_cast<GLint>(sourceLevel);
    source.maxLevel = static_cast<GLint>(sourceLevel);
    std::fill(p.samplerUnits.begin(), p.samplerUnits.end(), 0u);
    BindResources(c, p, VK_PIPELINE_BIND_POINT_GRAPHICS);
    source.minFilter = savedMin;
    source.magFilter = savedMag;
    source.baseLevel = savedBase;
    source.maxLevel = savedMax;
    c.textures[0] = savedTexture;
    c.samplers[0] = savedSampler;
    vkCmdDraw(commands, 4, 1, 0, 0);
    c.boundPipeline = VK_NULL_HANDLE;
    c.dynamicKnown = false;
}
}

void BlitFramebuffer(Context& c, GLint sx0, GLint sy0, GLint sx1, GLint sy1, GLint dx0, GLint dy0, GLint dx1, GLint dy1, GLbitfield mask,
                     GLenum filter)
{
    Framebuffer* read = ReadFramebuffer(c);
    Attachments destination;
    if (read == nullptr || !Gather(DrawFramebuffer(c), &destination))
    {
        return;
    }

    if ((mask & Gl::COLOR_BUFFER_BIT) != 0 && destination.colourCount != 0)
    {
        const u32 index = read->readBuffer >= Gl::COLOR_ATTACHMENT0 && read->readBuffer <= Gl::COLOR_ATTACHMENT0 + 3
                              ? read->readBuffer - Gl::COLOR_ATTACHMENT0
                              : 0;
        Texture* source = FindTexture(read->colour[index].texture);
        if (source != nullptr && EnsureImage(*source))
        {
            // (the source's rendering ended first: it's read as a texture)
            EndRendering(c);
            BlitOne(c, *source, static_cast<u32>(read->colour[index].level), destination, false, sx0, sy0, sx1, sy1, dx0, dy0, dx1, dy1,
                    filter == Gl::LINEAR);
        }
    }

    if ((mask & Gl::DEPTH_BUFFER_BIT) != 0 && destination.depth != nullptr)
    {
        const Attachment& depth = read->depth.texture != 0 ? read->depth : read->stencil;
        Texture* source = FindTexture(depth.texture);
        if (source != nullptr && EnsureImage(*source))
        {
            EndRendering(c);
            BlitOne(c, *source, static_cast<u32>(depth.level), destination, true, sx0, sy0, sx1, sy1, dx0, dy0, dx1, dy1, false);
        }
    }

    // (the destination's rendering ended: what follows reads it as GL's blit had finished)
    EndRendering(c);
}

void PresentWindow(Context& c)
{
    Framebuffer* framebuffer = c.window != nullptr ? DrawFramebuffer(c) : nullptr;
    Texture* colour = framebuffer != nullptr ? FindTexture(framebuffer->colour[0].texture) : nullptr;
    if (colour != nullptr)
    {
        PresentImage(c, *colour);
    }
}
}
