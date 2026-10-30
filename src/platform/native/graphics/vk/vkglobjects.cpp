// VkGl's objects: the names' tables, a texture's image (its levels as GL was given them), its views, the samplers (by their
// parameters), the timer queries (timestamps)
#include "vkglinternal.h"

#include <algorithm>
#include <cmath>

namespace VkGl
{
namespace
{
Objects g_Objects;

template <typename T> T* Find(std::unordered_map<GLuint, std::unique_ptr<T>>& map, GLuint name)
{
    std::lock_guard lock(g_Objects.lock);
    auto found = map.find(name);
    return found != map.end() ? found->second.get() : nullptr;
}

std::mutex g_SamplersLock;
std::map<std::array<u32, 8>, VkSampler> g_Samplers;

// The timestamps' pool: two a query (its begin and end), slots taken and given back
constexpr u32 QuerySlots = 4096;
VkQueryPool g_QueryPool = VK_NULL_HANDLE;
std::vector<u32> g_FreeSlots;
std::mutex g_QueryLock;
}

Objects& GetObjects()
{
    return g_Objects;
}

Texture* FindTexture(GLuint name)
{
    return name != 0 ? Find(g_Objects.textures, name) : nullptr;
}

SamplerObject* FindSampler(GLuint name)
{
    return name != 0 ? Find(g_Objects.samplers, name) : nullptr;
}

BufferObject* FindBuffer(GLuint name)
{
    return name != 0 ? Find(g_Objects.buffers, name) : nullptr;
}

Framebuffer* FindFramebuffer(GLuint name)
{
    return name != 0 ? Find(g_Objects.framebuffers, name) : nullptr;
}

ShaderObject* FindShader(GLuint name)
{
    return name != 0 ? Find(g_Objects.shaders, name) : nullptr;
}

Program* FindProgram(GLuint name)
{
    return name != 0 ? Find(g_Objects.programs, name) : nullptr;
}

VertexArray* FindVertexArray(GLuint name)
{
    return name != 0 ? Find(g_Objects.vertexArrays, name) : nullptr;
}

Query* FindQuery(GLuint name)
{
    return name != 0 ? Find(g_Objects.queries, name) : nullptr;
}

VkFormat FormatFor(GLenum internal, VkImageAspectFlags* aspects, bool* integer, u32* bytesPerTexel)
{
    VkImageAspectFlags a = VK_IMAGE_ASPECT_COLOR_BIT;
    bool i = false;
    u32 bytes = 4;
    VkFormat format = VK_FORMAT_UNDEFINED;
    switch (internal)
    {
    case Gl::RGBA8:
    case Gl::RGBA:
    case Gl::RGB8:
    case Gl::RGB:
        format = VK_FORMAT_R8G8B8A8_UNORM;
        break;
    case Gl::R8:
    case Gl::RED:
        format = VK_FORMAT_R8_UNORM;
        bytes = 1;
        break;
    case Gl::RG8:
        format = VK_FORMAT_R8G8_UNORM;
        bytes = 2;
        break;
    case Gl::R32UI:
        format = VK_FORMAT_R32_UINT;
        i = true;
        break;
    case Gl::R16UI:
        format = VK_FORMAT_R16_UINT;
        i = true;
        bytes = 2;
        break;
    case Gl::R8UI:
        format = VK_FORMAT_R8_UINT;
        i = true;
        bytes = 1;
        break;
    case Gl::RG32UI:
        format = VK_FORMAT_R32G32_UINT;
        i = true;
        bytes = 8;
        break;
    case Gl::RGBA32UI:
        format = VK_FORMAT_R32G32B32A32_UINT;
        i = true;
        bytes = 16;
        break;
    case Gl::RGBA16UI:
        format = VK_FORMAT_R16G16B16A16_UINT;
        i = true;
        bytes = 8;
        break;
    case Gl::RGBA8UI:
        format = VK_FORMAT_R8G8B8A8_UINT;
        i = true;
        break;
    case Gl::R16F:
        format = VK_FORMAT_R16_SFLOAT;
        bytes = 2;
        break;
    case Gl::R32F:
        format = VK_FORMAT_R32_SFLOAT;
        break;
    case Gl::RG16F:
        format = VK_FORMAT_R16G16_SFLOAT;
        break;
    case Gl::RG32F:
        format = VK_FORMAT_R32G32_SFLOAT;
        bytes = 8;
        break;
    case Gl::RGBA16F:
        format = VK_FORMAT_R16G16B16A16_SFLOAT;
        bytes = 8;
        break;
    case Gl::RGBA32F:
        format = VK_FORMAT_R32G32B32A32_SFLOAT;
        bytes = 16;
        break;
    case Gl::DEPTH32F_STENCIL8:
    case Gl::DEPTH24_STENCIL8:
    case Gl::DEPTH_STENCIL:
        format = VK_FORMAT_D32_SFLOAT_S8_UINT;
        a = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
        bytes = 8;
        break;
    case Gl::DEPTH_COMPONENT32F:
    case Gl::DEPTH_COMPONENT24:
    case Gl::DEPTH_COMPONENT16:
    case Gl::DEPTH_COMPONENT:
        format = VK_FORMAT_D32_SFLOAT;
        a = VK_IMAGE_ASPECT_DEPTH_BIT;
        break;
    default:
        Log("internal format 0x%x isn't known: RGBA8 instead", internal);
        format = VK_FORMAT_R8G8B8A8_UNORM;
        break;
    }

    if (aspects != nullptr)
    {
        *aspects = a;
    }

    if (integer != nullptr)
    {
        *integer = i;
    }

    if (bytesPerTexel != nullptr)
    {
        *bytesPerTexel = bytes;
    }

    return format;
}

u32 BytesPerTexel(VkFormat format)
{
    switch (format)
    {
    case VK_FORMAT_R8_UNORM:
    case VK_FORMAT_R8_UINT:
        return 1;
    case VK_FORMAT_R8G8_UNORM:
    case VK_FORMAT_R16_UINT:
    case VK_FORMAT_R16_SFLOAT:
        return 2;
    case VK_FORMAT_R32G32_UINT:
    case VK_FORMAT_R32G32_SFLOAT:
    case VK_FORMAT_R16G16B16A16_SFLOAT:
    case VK_FORMAT_R16G16B16A16_UINT:
    case VK_FORMAT_D32_SFLOAT_S8_UINT:
        return 8;
    case VK_FORMAT_R32G32B32A32_UINT:
    case VK_FORMAT_R32G32B32A32_SFLOAT:
        return 16;
    default:
        return 4;
    }
}

namespace
{
void DestroyImage(VkImage image, std::map<u64, VkImageView> views, Allocation memory)
{
    Device& d = GetDevice();
    for (auto& view : views)
    {
        vkDestroyImageView(d.device, view.second, nullptr);
    }

    if (image != VK_NULL_HANDLE)
    {
        vkDestroyImage(d.device, image, nullptr);
    }

    Free(memory);
}

// A serial no recording now open can be before: the next submission's (the work after it can't come too soon)
u64 SafeSerial()
{
    return GetDevice().submitted.load() + 64;
}
}

void ForgetImage(Texture& texture)
{
    if (texture.image == VK_NULL_HANDLE)
    {
        return;
    }

    // (destroyed once the work that may read it is done: every open recording submitted, and those submissions finished)
    VkImage image = texture.image;
    auto views = texture.views;
    Allocation memory = texture.memory;
    AfterSerial(SafeSerial(), [image, views, memory] { DestroyImage(image, views, memory); });
    texture.image = VK_NULL_HANDLE;
    texture.views.clear();
    texture.memory = Allocation{};
}

bool EnsureImage(Texture& texture)
{
    if (texture.given.empty() || texture.given[0].first == 0 || texture.given[0].second == 0)
    {
        return false;
    }

    // The levels: the given ones from 0 on, each half the one before (GL's complete mipmaps)
    u32 levels = 1;
    while (levels < texture.given.size() && texture.given[levels].first != 0)
    {
        levels++;
    }

    VkImageAspectFlags aspects = 0;
    bool integer = false;
    VkFormat format = FormatFor(texture.internal, &aspects, &integer, nullptr);
    const u32 width = texture.given[0].first;
    const u32 height = texture.given[0].second;
    if (texture.image != VK_NULL_HANDLE && texture.format == format && texture.width == width && texture.height == height &&
        texture.levels >= levels)
    {
        return true;
    }

    Device& d = GetDevice();
    VkImageCreateInfo info = {};
    info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = format;
    info.extent = {width, height, 1};
    info.mipLevels = levels;
    info.arrayLayers = 1;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    VkFormatProperties properties = {};
    vkGetPhysicalDeviceFormatProperties(d.physical, format, &properties);
    if ((aspects & VK_IMAGE_ASPECT_DEPTH_BIT) != 0)
    {
        info.usage |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    }
    else if ((properties.optimalTilingFeatures & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT) != 0)
    {
        info.usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    }

    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImage image = VK_NULL_HANDLE;
    VkResult result = vkCreateImage(d.device, &info, nullptr, &image);
    if (result != VK_SUCCESS)
    {
        Log("vkCreateImage %ux%u format %d: %d", width, height, format, result);
        return false;
    }

    VkMemoryRequirements needs;
    vkGetImageMemoryRequirements(d.device, image, &needs);
    Allocation memory;
    if (!Allocate(needs, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &memory) && !Allocate(needs, 0, &memory))
    {
        vkDestroyImage(d.device, image, nullptr);
        return false;
    }

    vkBindImageMemory(d.device, image, memory.memory, memory.offset);
    // Into the general layout (every image stays in it), and the old image's levels kept where the size and format are the same
    // (a texture given more levels)
    Context* c = Current();
    const bool keep = texture.image != VK_NULL_HANDLE && texture.format == format && texture.width == width && texture.height == height;
    if (c != nullptr)
    {
        VkCommandBuffer commands = Commands(*c);
        Barrier(*c);
        VkImageMemoryBarrier barrier = {};
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.srcAccessMask = 0;
        barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
        barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = image;
        barrier.subresourceRange = {aspects, 0, levels, 0, 1};
        vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1,
                             &barrier);
        if (keep)
        {
            std::vector<VkImageCopy> copies;
            for (u32 level = 0; level < texture.levels; level++)
            {
                VkImageCopy copy = {};
                copy.srcSubresource = {aspects, level, 0, 1};
                copy.dstSubresource = copy.srcSubresource;
                copy.extent = {std::max(1u, width >> level), std::max(1u, height >> level), 1};
                copies.push_back(copy);
            }

            vkCmdCopyImage(commands, texture.image, VK_IMAGE_LAYOUT_GENERAL, image, VK_IMAGE_LAYOUT_GENERAL,
                           static_cast<u32>(copies.size()), copies.data());
        }

        Barrier(*c);
    }
    else
    {
        Log("an image made with no context current: its layout is set by the next use");
    }

    ForgetImage(texture);
    texture.image = image;
    texture.memory = memory;
    texture.format = format;
    texture.width = width;
    texture.height = height;
    texture.levels = levels;
    texture.aspects = aspects;
    texture.integer = integer;
    texture.depth = (aspects & VK_IMAGE_ASPECT_DEPTH_BIT) != 0;
    texture.version++;
    return true;
}

VkImageView ViewOf(Texture& texture, u32 firstLevel, u32 levelCount, VkImageAspectFlags aspect)
{
    if (texture.image == VK_NULL_HANDLE)
    {
        return VK_NULL_HANDLE;
    }

    firstLevel = std::min(firstLevel, texture.levels - 1);
    levelCount = std::max(1u, std::min(levelCount, texture.levels - firstLevel));
    const u64 key = static_cast<u64>(firstLevel) | static_cast<u64>(levelCount) << 8 | static_cast<u64>(aspect) << 16;
    auto found = texture.views.find(key);
    if (found != texture.views.end())
    {
        return found->second;
    }

    VkImageViewCreateInfo info = {};
    info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    info.image = texture.image;
    info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    info.format = texture.format;
    info.components = {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
                       VK_COMPONENT_SWIZZLE_IDENTITY};
    info.subresourceRange = {aspect, firstLevel, levelCount, 0, 1};
    VkImageView view = VK_NULL_HANDLE;
    if (vkCreateImageView(GetDevice().device, &info, nullptr, &view) != VK_SUCCESS)
    {
        Log("vkCreateImageView failed (format %d, aspect 0x%x)", texture.format, aspect);
        return VK_NULL_HANDLE;
    }

    texture.views[key] = view;
    return view;
}

VkSampler SamplerFor(GLenum minFilter, GLenum magFilter, GLenum wrapS, GLenum wrapT, f32 anisotropy, f32 lodBias, bool integer,
                     u32 levels)
{
    if (integer)
    {
        minFilter = Gl::NEAREST;
        magFilter = Gl::NEAREST;
        anisotropy = 1.0f;
    }

    const bool mipmapped = minFilter == Gl::NEAREST_MIPMAP_NEAREST || minFilter == Gl::LINEAR_MIPMAP_NEAREST ||
                           minFilter == Gl::NEAREST_MIPMAP_LINEAR || minFilter == Gl::LINEAR_MIPMAP_LINEAR;
    u32 anisotropyBits = 0;
    std::memcpy(&anisotropyBits, &anisotropy, 4);
    u32 biasBits = 0;
    std::memcpy(&biasBits, &lodBias, 4);
    const std::array<u32, 8> key = {minFilter, magFilter, wrapS, wrapT, anisotropyBits, biasBits, mipmapped ? levels : 1u, 0};
    std::lock_guard lock(g_SamplersLock);
    auto found = g_Samplers.find(key);
    if (found != g_Samplers.end())
    {
        return found->second;
    }

    auto wrap = [](GLenum mode)
    {
        switch (mode)
        {
        case Gl::CLAMP_TO_EDGE:
            return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        case Gl::MIRRORED_REPEAT:
            return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
        case Gl::CLAMP_TO_BORDER:
            return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
        default:
            return VK_SAMPLER_ADDRESS_MODE_REPEAT;
        }
    };
    Device& d = GetDevice();
    VkSamplerCreateInfo info = {};
    info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    info.magFilter = magFilter == Gl::LINEAR ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    info.minFilter = (minFilter == Gl::LINEAR || minFilter == Gl::LINEAR_MIPMAP_NEAREST || minFilter == Gl::LINEAR_MIPMAP_LINEAR)
                         ? VK_FILTER_LINEAR
                         : VK_FILTER_NEAREST;
    info.mipmapMode = (minFilter == Gl::NEAREST_MIPMAP_LINEAR || minFilter == Gl::LINEAR_MIPMAP_LINEAR) ? VK_SAMPLER_MIPMAP_MODE_LINEAR
                                                                                                       : VK_SAMPLER_MIPMAP_MODE_NEAREST;
    info.addressModeU = wrap(wrapS);
    info.addressModeV = wrap(wrapT);
    info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    info.mipLodBias = lodBias;
    info.anisotropyEnable = d.anisotropy && anisotropy > 1.0f ? VK_TRUE : VK_FALSE;
    info.maxAnisotropy = std::min(anisotropy, d.maxAnisotropy);
    info.minLod = 0.0f;
    // A filter without mipmaps samples the first level only (GL's), else every level the view has
    info.maxLod = mipmapped ? static_cast<f32>(levels) : 0.25f;
    info.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    VkSampler sampler = VK_NULL_HANDLE;
    vkCreateSampler(d.device, &info, nullptr, &sampler);
    g_Samplers[key] = sampler;
    return sampler;
}

// ------------------------------------------------------------------------------------------------------------------ the queries
namespace
{
bool QueryPool()
{
    Device& d = GetDevice();
    if (g_QueryPool != VK_NULL_HANDLE)
    {
        return true;
    }

    if (!d.timestamps || vkResetQueryPool == nullptr)
    {
        return false;
    }

    VkQueryPoolCreateInfo info = {};
    info.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
    info.queryType = VK_QUERY_TYPE_TIMESTAMP;
    info.queryCount = QuerySlots;
    if (vkCreateQueryPool(d.device, &info, nullptr, &g_QueryPool) != VK_SUCCESS)
    {
        return false;
    }

    vkResetQueryPool(d.device, g_QueryPool, 0, QuerySlots);
    for (u32 slot = 0; slot < QuerySlots; slot += 2)
    {
        g_FreeSlots.push_back(slot);
    }

    return true;
}
}

void BeginQuery(Context& c, Query& query)
{
    std::lock_guard lock(g_QueryLock);
    query.ended = false;
    query.resultKnown = false;
    query.result = 0;
    if (!QueryPool() || g_FreeSlots.empty())
    {
        query.slot = ~0u;
        return;
    }

    query.slot = g_FreeSlots.back();
    g_FreeSlots.pop_back();
    vkCmdWriteTimestamp(Commands(c), VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, g_QueryPool, query.slot);
}

void EndQuery(Context& c, Query& query)
{
    std::lock_guard lock(g_QueryLock);
    query.ended = true;
    query.serial = GetDevice().submitted.load() + 1;
    if (query.slot == ~0u)
    {
        query.resultKnown = true;
        return;
    }

    vkCmdWriteTimestamp(Commands(c), VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, g_QueryPool, query.slot + 1);
}

bool QueryResult(Query& query, GLuint64* result)
{
    std::lock_guard lock(g_QueryLock);
    if (query.resultKnown)
    {
        *result = query.result;
        return true;
    }

    if (!query.ended || query.slot == ~0u)
    {
        *result = 0;
        return query.slot == ~0u;
    }

    u64 stamps[2] = {};
    Device& d = GetDevice();
    VkResult got = vkGetQueryPoolResults(d.device, g_QueryPool, query.slot, 2, sizeof(stamps), stamps, sizeof(u64), VK_QUERY_RESULT_64_BIT);
    if (got != VK_SUCCESS)
    {
        return false;
    }

    query.result = stamps[1] > stamps[0] ? static_cast<GLuint64>(static_cast<double>(stamps[1] - stamps[0]) * d.timestampPeriod) : 0;
    query.resultKnown = true;
    vkResetQueryPool(d.device, g_QueryPool, query.slot, 2);
    g_FreeSlots.push_back(query.slot);
    query.slot = ~0u;
    *result = query.result;
    return true;
}
}
