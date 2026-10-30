#pragma once

// VkGl's insides (vkgl.h): the Vulkan functions, the GL objects as Vulkan has them, the contexts, the device. The parts:
// vkgldevice.cpp (the device, memory, submissions, the stream buffer, the swapchain), vkglobjects.cpp (textures, buffers,
// framebuffers, samplers, queries, syncs), vkglprogram.cpp (shaders: GLSL to SPIR-V, the uniforms), vkgldraw.cpp (the state, the
// pipelines, rendering, draws, clears, blits, reads) and vkglentry.cpp (the GL entry points)

#include "common.h"

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <SDL3/SDL.h>

#include <array>
#include <atomic>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace VkGl
{
// ------------------------------------------------------------------------------------------------------------------ Vulkan's
#define VKGL_GLOBAL(X)                                                                                                         \
    X(vkCreateInstance)                                                                                                        \
    X(vkEnumerateInstanceExtensionProperties)                                                                                  \
    X(vkEnumerateInstanceVersion)

#define VKGL_INSTANCE(X)                                                                                                       \
    X(vkDestroyInstance)                                                                                                       \
    X(vkEnumeratePhysicalDevices)                                                                                              \
    X(vkGetPhysicalDeviceProperties)                                                                                           \
    X(vkGetPhysicalDeviceFeatures)                                                                                             \
    X(vkGetPhysicalDeviceFeatures2)                                                                                            \
    X(vkGetPhysicalDeviceMemoryProperties)                                                                                     \
    X(vkGetPhysicalDeviceFormatProperties)                                                                                     \
    X(vkGetPhysicalDeviceQueueFamilyProperties)                                                                                \
    X(vkEnumerateDeviceExtensionProperties)                                                                                    \
    X(vkCreateDevice)                                                                                                          \
    X(vkGetDeviceProcAddr)                                                                                                     \
    X(vkDestroySurfaceKHR)                                                                                                     \
    X(vkGetPhysicalDeviceSurfaceSupportKHR)                                                                                    \
    X(vkGetPhysicalDeviceSurfaceCapabilitiesKHR)                                                                               \
    X(vkGetPhysicalDeviceSurfaceFormatsKHR)                                                                                    \
    X(vkGetPhysicalDeviceSurfacePresentModesKHR)

#define VKGL_DEVICE(X)                                                                                                         \
    X(vkDestroyDevice)                                                                                                         \
    X(vkGetDeviceQueue)                                                                                                        \
    X(vkDeviceWaitIdle)                                                                                                        \
    X(vkQueueSubmit)                                                                                                           \
    X(vkQueueWaitIdle)                                                                                                         \
    X(vkAllocateMemory)                                                                                                        \
    X(vkFreeMemory)                                                                                                            \
    X(vkMapMemory)                                                                                                             \
    X(vkUnmapMemory)                                                                                                           \
    X(vkCreateBuffer)                                                                                                          \
    X(vkDestroyBuffer)                                                                                                         \
    X(vkGetBufferMemoryRequirements)                                                                                           \
    X(vkBindBufferMemory)                                                                                                      \
    X(vkCreateImage)                                                                                                           \
    X(vkDestroyImage)                                                                                                          \
    X(vkGetImageMemoryRequirements)                                                                                            \
    X(vkBindImageMemory)                                                                                                       \
    X(vkCreateImageView)                                                                                                       \
    X(vkDestroyImageView)                                                                                                      \
    X(vkCreateSampler)                                                                                                         \
    X(vkDestroySampler)                                                                                                        \
    X(vkCreateShaderModule)                                                                                                    \
    X(vkDestroyShaderModule)                                                                                                   \
    X(vkCreateDescriptorSetLayout)                                                                                             \
    X(vkDestroyDescriptorSetLayout)                                                                                            \
    X(vkCreatePipelineLayout)                                                                                                  \
    X(vkDestroyPipelineLayout)                                                                                                 \
    X(vkCreatePipelineCache)                                                                                                   \
    X(vkDestroyPipelineCache)                                                                                                  \
    X(vkCreateGraphicsPipelines)                                                                                               \
    X(vkDestroyPipeline)                                                                                                       \
    X(vkCreateDescriptorPool)                                                                                                  \
    X(vkDestroyDescriptorPool)                                                                                                 \
    X(vkResetDescriptorPool)                                                                                                   \
    X(vkAllocateDescriptorSets)                                                                                                \
    X(vkUpdateDescriptorSets)                                                                                                  \
    X(vkCreateCommandPool)                                                                                                     \
    X(vkDestroyCommandPool)                                                                                                    \
    X(vkResetCommandPool)                                                                                                      \
    X(vkAllocateCommandBuffers)                                                                                                \
    X(vkFreeCommandBuffers)                                                                                                    \
    X(vkBeginCommandBuffer)                                                                                                    \
    X(vkEndCommandBuffer)                                                                                                      \
    X(vkCreateFence)                                                                                                           \
    X(vkDestroyFence)                                                                                                          \
    X(vkWaitForFences)                                                                                                         \
    X(vkResetFences)                                                                                                           \
    X(vkGetFenceStatus)                                                                                                        \
    X(vkCreateSemaphore)                                                                                                       \
    X(vkDestroySemaphore)                                                                                                      \
    X(vkCreateQueryPool)                                                                                                       \
    X(vkDestroyQueryPool)                                                                                                      \
    X(vkGetQueryPoolResults)                                                                                                   \
    X(vkCmdResetQueryPool)                                                                                                     \
    X(vkResetQueryPool)                                                                                                        \
    X(vkCmdWriteTimestamp)                                                                                                     \
    X(vkCmdPipelineBarrier)                                                                                                    \
    X(vkCmdBeginRendering)                                                                                                     \
    X(vkCmdEndRendering)                                                                                                       \
    X(vkCmdBindPipeline)                                                                                                       \
    X(vkCmdBindDescriptorSets)                                                                                                 \
    X(vkCmdBindVertexBuffers)                                                                                                  \
    X(vkCmdDraw)                                                                                                               \
    X(vkCmdSetViewport)                                                                                                        \
    X(vkCmdSetScissor)                                                                                                         \
    X(vkCmdSetBlendConstants)                                                                                                  \
    X(vkCmdSetStencilCompareMask)                                                                                              \
    X(vkCmdSetStencilWriteMask)                                                                                                \
    X(vkCmdSetStencilReference)                                                                                                \
    X(vkCmdSetDepthTestEnable)                                                                                                 \
    X(vkCmdSetDepthWriteEnable)                                                                                                \
    X(vkCmdSetDepthCompareOp)                                                                                                  \
    X(vkCmdSetStencilTestEnable)                                                                                               \
    X(vkCmdSetStencilOp)                                                                                                       \
    X(vkCmdSetPrimitiveTopology)                                                                                               \
    X(vkCmdSetCullMode)                                                                                                        \
    X(vkCmdSetFrontFace)                                                                                                       \
    X(vkCmdClearAttachments)                                                                                                   \
    X(vkCmdClearColorImage)                                                                                                    \
    X(vkCmdClearDepthStencilImage)                                                                                             \
    X(vkCmdCopyBufferToImage)                                                                                                  \
    X(vkCmdCopyImageToBuffer)                                                                                                  \
    X(vkCmdCopyImage)                                                                                                          \
    X(vkCmdBlitImage)                                                                                                          \
    X(vkCreateSwapchainKHR)                                                                                                    \
    X(vkDestroySwapchainKHR)                                                                                                   \
    X(vkGetSwapchainImagesKHR)                                                                                                 \
    X(vkAcquireNextImageKHR)                                                                                                   \
    X(vkQueuePresentKHR)

#define VKGL_DECLARE(name) extern PFN_##name name;
VKGL_GLOBAL(VKGL_DECLARE)
VKGL_INSTANCE(VKGL_DECLARE)
VKGL_DEVICE(VKGL_DECLARE)
#undef VKGL_DECLARE

// ------------------------------------------------------------------------------------------------------------------ GL's types
using GLenum = unsigned int;
using GLuint = unsigned int;
using GLint = int;
using GLsizei = int;
using GLboolean = unsigned char;
using GLchar = char;
using GLbitfield = unsigned int;
using GLfloat = float;
using GLsizeiptr = std::ptrdiff_t;
using GLintptr = std::ptrdiff_t;
using GLuint64 = unsigned long long;
using GLsync = struct __GLsync*;

// The GL constants the users pass (their values as GL's headers have them)
namespace Gl
{
constexpr GLenum NONE = 0;
constexpr GLenum POINTS = 0x0000;
constexpr GLenum LINES = 0x0001;
constexpr GLenum LINE_STRIP = 0x0003;
constexpr GLenum TRIANGLES = 0x0004;
constexpr GLenum TRIANGLE_STRIP = 0x0005;
constexpr GLenum TRIANGLE_FAN = 0x0006;
constexpr GLenum NEVER = 0x0200;
constexpr GLenum LESS = 0x0201;
constexpr GLenum EQUAL = 0x0202;
constexpr GLenum LEQUAL = 0x0203;
constexpr GLenum GREATER = 0x0204;
constexpr GLenum NOTEQUAL = 0x0205;
constexpr GLenum GEQUAL = 0x0206;
constexpr GLenum ALWAYS = 0x0207;
constexpr GLenum ZERO = 0;
constexpr GLenum ONE = 1;
constexpr GLenum SRC_COLOR = 0x0300;
constexpr GLenum ONE_MINUS_SRC_COLOR = 0x0301;
constexpr GLenum SRC_ALPHA = 0x0302;
constexpr GLenum ONE_MINUS_SRC_ALPHA = 0x0303;
constexpr GLenum DST_ALPHA = 0x0304;
constexpr GLenum ONE_MINUS_DST_ALPHA = 0x0305;
constexpr GLenum DST_COLOR = 0x0306;
constexpr GLenum ONE_MINUS_DST_COLOR = 0x0307;
constexpr GLenum CONSTANT_COLOR = 0x8001;
constexpr GLenum ONE_MINUS_CONSTANT_COLOR = 0x8002;
constexpr GLenum CONSTANT_ALPHA = 0x8003;
constexpr GLenum ONE_MINUS_CONSTANT_ALPHA = 0x8004;
constexpr GLenum SRC1_ALPHA = 0x8589;
constexpr GLenum SRC1_COLOR = 0x88F9;
constexpr GLenum ONE_MINUS_SRC1_COLOR = 0x88FA;
constexpr GLenum ONE_MINUS_SRC1_ALPHA = 0x88FB;
constexpr GLenum FUNC_ADD = 0x8006;
constexpr GLenum MIN = 0x8007;
constexpr GLenum MAX = 0x8008;
constexpr GLenum FUNC_SUBTRACT = 0x800A;
constexpr GLenum FUNC_REVERSE_SUBTRACT = 0x800B;
constexpr GLenum KEEP = 0x1E00;
constexpr GLenum REPLACE = 0x1E01;
constexpr GLenum INCR = 0x1E02;
constexpr GLenum DECR = 0x1E03;
constexpr GLenum INVERT = 0x150A;
constexpr GLenum INCR_WRAP = 0x8507;
constexpr GLenum DECR_WRAP = 0x8508;
constexpr GLenum CULL_FACE = 0x0B44;
constexpr GLenum DEPTH_TEST = 0x0B71;
constexpr GLenum STENCIL_TEST = 0x0B90;
constexpr GLenum DITHER = 0x0BD0;
constexpr GLenum BLEND = 0x0BE2;
constexpr GLenum SCISSOR_TEST = 0x0C11;
constexpr GLenum PROGRAM_POINT_SIZE = 0x8642;
constexpr GLenum FRAMEBUFFER_SRGB = 0x8DB9;
constexpr GLenum MULTISAMPLE = 0x809D;
constexpr GLenum TEXTURE_2D = 0x0DE1;
constexpr GLenum TEXTURE0 = 0x84C0;
constexpr GLenum TEXTURE_MAG_FILTER = 0x2800;
constexpr GLenum TEXTURE_MIN_FILTER = 0x2801;
constexpr GLenum TEXTURE_WRAP_S = 0x2802;
constexpr GLenum TEXTURE_WRAP_T = 0x2803;
constexpr GLenum TEXTURE_BASE_LEVEL = 0x813C;
constexpr GLenum TEXTURE_MAX_LEVEL = 0x813D;
constexpr GLenum TEXTURE_MIN_LOD = 0x813A;
constexpr GLenum TEXTURE_MAX_LOD = 0x813B;
constexpr GLenum TEXTURE_LOD_BIAS = 0x8501;
constexpr GLenum TEXTURE_MAX_ANISOTROPY = 0x84FE;
constexpr GLenum TEXTURE_COMPARE_MODE = 0x884C;
constexpr GLenum NEAREST = 0x2600;
constexpr GLenum LINEAR = 0x2601;
constexpr GLenum NEAREST_MIPMAP_NEAREST = 0x2700;
constexpr GLenum LINEAR_MIPMAP_NEAREST = 0x2701;
constexpr GLenum NEAREST_MIPMAP_LINEAR = 0x2702;
constexpr GLenum LINEAR_MIPMAP_LINEAR = 0x2703;
constexpr GLenum REPEAT = 0x2901;
constexpr GLenum CLAMP_TO_EDGE = 0x812F;
constexpr GLenum MIRRORED_REPEAT = 0x8370;
constexpr GLenum CLAMP_TO_BORDER = 0x812D;
constexpr GLenum UNPACK_ROW_LENGTH = 0x0CF2;
constexpr GLenum UNPACK_SKIP_ROWS = 0x0CF3;
constexpr GLenum UNPACK_SKIP_PIXELS = 0x0CF4;
constexpr GLenum UNPACK_ALIGNMENT = 0x0CF5;
constexpr GLenum PACK_ROW_LENGTH = 0x0D02;
constexpr GLenum PACK_SKIP_ROWS = 0x0D03;
constexpr GLenum PACK_SKIP_PIXELS = 0x0D04;
constexpr GLenum PACK_ALIGNMENT = 0x0D05;
constexpr GLenum BYTE = 0x1400;
constexpr GLenum UNSIGNED_BYTE = 0x1401;
constexpr GLenum SHORT = 0x1402;
constexpr GLenum UNSIGNED_SHORT = 0x1403;
constexpr GLenum INT = 0x1404;
constexpr GLenum UNSIGNED_INT = 0x1405;
constexpr GLenum FLOAT = 0x1406;
constexpr GLenum HALF_FLOAT = 0x140B;
constexpr GLenum UNSIGNED_INT_24_8 = 0x84FA;
constexpr GLenum FLOAT_32_UNSIGNED_INT_24_8_REV = 0x8DAD;
constexpr GLenum RED = 0x1903;
constexpr GLenum RG = 0x8227;
constexpr GLenum RGB = 0x1907;
constexpr GLenum RGBA = 0x1908;
constexpr GLenum BGRA = 0x80E1;
constexpr GLenum RED_INTEGER = 0x8D94;
constexpr GLenum RG_INTEGER = 0x8228;
constexpr GLenum RGBA_INTEGER = 0x8D99;
constexpr GLenum DEPTH_COMPONENT = 0x1902;
constexpr GLenum STENCIL_INDEX = 0x1901;
constexpr GLenum DEPTH_STENCIL = 0x84F9;
constexpr GLenum R8 = 0x8229;
constexpr GLenum RG8 = 0x822B;
constexpr GLenum R16F = 0x822D;
constexpr GLenum R32F = 0x822E;
constexpr GLenum RG16F = 0x822F;
constexpr GLenum RG32F = 0x8230;
constexpr GLenum R8UI = 0x8232;
constexpr GLenum R16UI = 0x8234;
constexpr GLenum R32UI = 0x8236;
constexpr GLenum RG32UI = 0x823C;
constexpr GLenum RGBA8 = 0x8058;
constexpr GLenum RGB8 = 0x8051;
constexpr GLenum RGBA16F = 0x881A;
constexpr GLenum RGBA32F = 0x8814;
constexpr GLenum RGBA32UI = 0x8D70;
constexpr GLenum RGBA16UI = 0x8D76;
constexpr GLenum RGBA8UI = 0x8D7C;
constexpr GLenum DEPTH_COMPONENT16 = 0x81A5;
constexpr GLenum DEPTH_COMPONENT24 = 0x81A6;
constexpr GLenum DEPTH_COMPONENT32F = 0x8CAC;
constexpr GLenum DEPTH24_STENCIL8 = 0x88F0;
constexpr GLenum DEPTH32F_STENCIL8 = 0x8CAD;
constexpr GLenum COLOR_BUFFER_BIT = 0x00004000;
constexpr GLenum DEPTH_BUFFER_BIT = 0x00000100;
constexpr GLenum STENCIL_BUFFER_BIT = 0x00000400;
constexpr GLenum FRAMEBUFFER = 0x8D40;
constexpr GLenum READ_FRAMEBUFFER = 0x8CA8;
constexpr GLenum DRAW_FRAMEBUFFER = 0x8CA9;
constexpr GLenum COLOR_ATTACHMENT0 = 0x8CE0;
constexpr GLenum DEPTH_ATTACHMENT = 0x8D00;
constexpr GLenum STENCIL_ATTACHMENT = 0x8D20;
constexpr GLenum DEPTH_STENCIL_ATTACHMENT = 0x821A;
constexpr GLenum FRAMEBUFFER_COMPLETE = 0x8CD5;
constexpr GLenum BACK = 0x0405;
constexpr GLenum FRONT = 0x0404;
constexpr GLenum ARRAY_BUFFER = 0x8892;
constexpr GLenum ELEMENT_ARRAY_BUFFER = 0x8893;
constexpr GLenum PIXEL_PACK_BUFFER = 0x88EB;
constexpr GLenum PIXEL_UNPACK_BUFFER = 0x88EC;
constexpr GLenum UNIFORM_BUFFER = 0x8A11;
constexpr GLenum VERTEX_SHADER = 0x8B31;
constexpr GLenum FRAGMENT_SHADER = 0x8B30;
constexpr GLenum COMPILE_STATUS = 0x8B81;
constexpr GLenum LINK_STATUS = 0x8B82;
constexpr GLenum INFO_LOG_LENGTH = 0x8B84;
constexpr GLenum TIME_ELAPSED = 0x88BF;
constexpr GLenum TIMESTAMP = 0x8E28;
constexpr GLenum QUERY_RESULT = 0x8866;
constexpr GLenum QUERY_RESULT_AVAILABLE = 0x8867;
constexpr GLenum SYNC_GPU_COMMANDS_COMPLETE = 0x9117;
constexpr GLenum EXTENSIONS = 0x1F03;
constexpr GLenum VENDOR = 0x1F00;
constexpr GLenum RENDERER = 0x1F01;
constexpr GLenum VERSION = 0x1F02;
constexpr GLenum SHADING_LANGUAGE_VERSION = 0x8B8C;
constexpr GLenum NUM_EXTENSIONS = 0x821D;
constexpr GLenum MAX_TEXTURE_SIZE = 0x0D33;
constexpr GLenum MAX_TEXTURE_MAX_ANISOTROPY = 0x84FF;
constexpr GLenum IMPLEMENTATION_COLOR_READ_TYPE = 0x8B9A;
constexpr GLenum IMPLEMENTATION_COLOR_READ_FORMAT = 0x8B9B;
constexpr GLenum FRAMEBUFFER_BINDING = 0x8CA6;
constexpr GLenum CURRENT_PROGRAM = 0x8B8D;
constexpr GLenum VIEWPORT = 0x0BA2;
constexpr GLenum ACTIVE_TEXTURE = 0x84E0;
constexpr GLenum TEXTURE_BINDING_2D = 0x8069;
constexpr GLenum MAX_COLOR_ATTACHMENTS = 0x8CDF;
constexpr GLenum MAX_DRAW_BUFFERS = 0x8824;
constexpr GLenum MAX_DUAL_SOURCE_DRAW_BUFFERS = 0x88FC;
constexpr GLenum MAX_VIEWPORT_DIMS = 0x0D3A;
constexpr GLenum MAJOR_VERSION = 0x821B;
constexpr GLenum MINOR_VERSION = 0x821C;
}

// ------------------------------------------------------------------------------------------------------------------ the device
struct Allocation
{
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkDeviceSize offset = 0;
    VkDeviceSize size = 0;
    // (the block it's in, for its return; null: a dedicated allocation)
    void* block = nullptr;
    u8* mapped = nullptr;
};

struct Device
{
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    u32 queueFamily = 0;
    VkPhysicalDeviceProperties properties = {};
    VkPhysicalDeviceMemoryProperties memory = {};
    bool dualSource = false;
    bool anisotropy = false;
    f32 maxAnisotropy = 1.0f;
    bool wideLines = false;
    bool timestamps = false;
    f32 timestampPeriod = 1.0f;
    VkPipelineCache pipelineCache = VK_NULL_HANDLE;
    // The queue's submissions by number: the last submitted, and the last known done
    std::atomic<u64> submitted{0};
    std::atomic<u64> completed{0};
    std::mutex queueLock;
};

Device& GetDevice();

// Memory: device local for images and the GPU's buffers, host visible (and coherent) for what the CPU writes or reads
bool Allocate(const VkMemoryRequirements& needs, VkMemoryPropertyFlags wanted, Allocation* out);
void Free(Allocation& allocation);

// The queue's submissions done so far (fences polled), and a wait for one
u64 CompletedSerial();
void WaitSerial(u64 serial);
// Work to do once a submission is done (a deletion: the GPU may still read the object)
void AfterSerial(u64 serial, std::function<void()> work);
void RunFinishedWork();

// The stream buffer: host visible memory written by the CPU (vertexes, uniforms, uploads), handed out in pieces, each given back once
// the submission that read it is done
struct StreamPiece
{
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceSize offset = 0;
    u8* mapped = nullptr;
};
// (each context has its own, handed back when the submission that read them is done)
u64 SubmitToQueue(VkCommandBuffer commands, VkSemaphore wait, VkPipelineStageFlags waitStage, VkSemaphore signal);

// A buffer of its own (the CPU's reads of the GPU's pixels)
struct ReadBuffer
{
    VkBuffer buffer = VK_NULL_HANDLE;
    Allocation memory;
    VkDeviceSize size = 0;
};
bool MakeReadBuffer(VkDeviceSize size, ReadBuffer* out);
void DestroyReadBuffer(ReadBuffer& buffer);

// ------------------------------------------------------------------------------------------------------------------ the objects
struct Texture
{
    GLuint name = 0;
    VkImage image = VK_NULL_HANDLE;
    Allocation memory;
    VkFormat format = VK_FORMAT_UNDEFINED;
    GLenum internal = 0;
    u32 width = 0;
    u32 height = 0;
    u32 levels = 0;
    VkImageAspectFlags aspects = 0;
    bool integer = false;
    bool depth = false;
    // The levels GL was given (TexImage2D's sizes: an image is made for all of them once level 0 is there)
    std::vector<std::pair<u32, u32>> given;
    // Its sampling parameters (as a texture's own, when no sampler object is bound to its unit)
    GLenum minFilter = Gl::NEAREST_MIPMAP_LINEAR;
    GLenum magFilter = Gl::LINEAR;
    GLenum wrapS = Gl::REPEAT;
    GLenum wrapT = Gl::REPEAT;
    GLint baseLevel = 0;
    GLint maxLevel = 1000;
    GLfloat anisotropy = 1.0f;
    GLfloat lodBias = 0.0f;
    // Views by (first level, levels, aspect), made as they're asked for
    std::map<u64, VkImageView> views;
    // Changed when the image is made again (the descriptors' caches)
    u32 version = 0;
};

struct SamplerObject
{
    GLuint name = 0;
    GLenum minFilter = Gl::NEAREST_MIPMAP_LINEAR;
    GLenum magFilter = Gl::LINEAR;
    GLenum wrapS = Gl::REPEAT;
    GLenum wrapT = Gl::REPEAT;
    GLfloat anisotropy = 1.0f;
    GLfloat lodBias = 0.0f;
};

struct BufferObject
{
    GLuint name = 0;
    // Its data as last given (BufferData): kept by the CPU, copied into the stream by each draw that reads it
    std::vector<u8> data;
};

struct Attachment
{
    GLuint texture = 0;
    GLint level = 0;
};

struct Framebuffer
{
    GLuint name = 0;
    Attachment colour[4];
    Attachment depth;
    Attachment stencil;
    GLenum drawBuffers[4] = {Gl::COLOR_ATTACHMENT0, Gl::NONE, Gl::NONE, Gl::NONE};
    GLenum readBuffer = Gl::COLOR_ATTACHMENT0;
};

struct ShaderObject
{
    GLuint name = 0;
    GLenum type = 0;
    std::string source;
    bool compiled = false;
    std::string log;
};

// A uniform of the program's block (or a sampler: its binding), by the location GetUniformLocation gives
struct Uniform
{
    std::string name;
    GLint offset = -1;
    GLint arraySize = 1;
    GLint arrayStride = 16;
    GLenum glType = 0;
    bool sampler = false;
    u32 binding = 0;
};

struct Program
{
    GLuint name = 0;
    GLuint vertexShader = 0;
    GLuint fragmentShader = 0;
    std::map<std::string, GLuint> attributeLocations;
    bool linked = false;
    std::string log;
    VkShaderModule vertex = VK_NULL_HANDLE;
    VkShaderModule fragment = VK_NULL_HANDLE;
    std::vector<Uniform> uniforms;
    std::unordered_map<std::string, GLint> locations;
    // The block's bytes as the program's uniforms are now, and whether the program has a block at all
    std::vector<u8> block;
    bool hasBlock = false;
    // Each sampler's texture unit (its binding: 1 + its index)
    std::vector<u32> samplerUnits;
    std::vector<VkDescriptorType> samplerKinds;
    // Its vertex inputs (location: whether it's an integer one) and whether it writes a second blend source (location 0 index 1)
    std::map<u32, bool> inputs;
    bool dualSource = false;
    VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
};

struct VertexAttrib
{
    bool enabled = false;
    GLuint buffer = 0;
    GLint size = 4;
    GLenum type = Gl::FLOAT;
    bool normalized = false;
    bool integer = false;
    GLsizei stride = 0;
    GLintptr offset = 0;
};

struct VertexArray
{
    GLuint name = 0;
    VertexAttrib attribs[16];
};

struct Query
{
    GLuint name = 0;
    // The pool's two timestamps (begin and end), the submission they're in
    u32 slot = 0;
    u64 serial = 0;
    bool ended = false;
    GLuint64 result = 0;
    bool resultKnown = false;
};

struct Sync
{
    u64 serial = 0;
};

// The objects by name (every context shares them); names from 1
struct Objects
{
    std::mutex lock;
    GLuint next = 1;
    std::unordered_map<GLuint, std::unique_ptr<Texture>> textures;
    std::unordered_map<GLuint, std::unique_ptr<SamplerObject>> samplers;
    std::unordered_map<GLuint, std::unique_ptr<BufferObject>> buffers;
    std::unordered_map<GLuint, std::unique_ptr<Framebuffer>> framebuffers;
    std::unordered_map<GLuint, std::unique_ptr<ShaderObject>> shaders;
    std::unordered_map<GLuint, std::unique_ptr<Program>> programs;
    std::unordered_map<GLuint, std::unique_ptr<VertexArray>> vertexArrays;
    std::unordered_map<GLuint, std::unique_ptr<Query>> queries;
};
Objects& GetObjects();

Texture* FindTexture(GLuint name);
SamplerObject* FindSampler(GLuint name);
BufferObject* FindBuffer(GLuint name);
Framebuffer* FindFramebuffer(GLuint name);
ShaderObject* FindShader(GLuint name);
Program* FindProgram(GLuint name);
VertexArray* FindVertexArray(GLuint name);
Query* FindQuery(GLuint name);

// A texture's image made for the levels it was given (again when the size or format changed), and a view of it
bool EnsureImage(Texture& texture);
// Its image destroyed once the GPU can no longer be reading it
void ForgetImage(Texture& texture);
VkImageView ViewOf(Texture& texture, u32 firstLevel, u32 levelCount, VkImageAspectFlags aspect);
VkFormat FormatFor(GLenum internal, VkImageAspectFlags* aspects, bool* integer, u32* bytesPerTexel);
u32 BytesPerTexel(VkFormat format);
VkSampler SamplerFor(GLenum minFilter, GLenum magFilter, GLenum wrapS, GLenum wrapT, f32 anisotropy, f32 lodBias, bool integer,
                     u32 levels);

// ------------------------------------------------------------------------------------------------------------------ a context
struct Rendering
{
    bool active = false;
    // The attachments it's for (images and their levels), the framebuffer's size
    VkImage colourImages[4] = {};
    u32 colourLevels[4] = {};
    VkImage depthImage = VK_NULL_HANDLE;
    u32 width = 0;
    u32 height = 0;
};

struct Context
{
    SDL_Window* window = nullptr;
    // The default framebuffer (a window context's): an image of the window's size, shown by SwapWindow upside down (GL's rows from
    // the bottom, Vulkan's from the top: the same numbers everywhere else)
    GLuint windowColour = 0;
    GLuint windowDepth = 0;
    GLuint windowFramebuffer = 0;
    // The commands being recorded (begun on the first that needs them), and their pool
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer commands = VK_NULL_HANDLE;
    std::vector<std::pair<u64, VkCommandBuffer>> spareCommands;
    std::vector<std::pair<u64, VkDescriptorPool>> descriptorPools;
    VkDescriptorPool descriptorPool = VK_NULL_HANDLE;
    u32 draws = 0;
    Rendering rendering;
    // The state
    GLenum activeUnit = 0;
    GLuint textures[32] = {};
    GLuint samplers[32] = {};
    GLuint program = 0;
    GLuint vertexArray = 0;
    // (the vertex array bound as 0: one of the context's own)
    GLuint defaultVertexArray = 0;
    GLuint arrayBuffer = 0;
    GLuint unpackBuffer = 0;
    GLuint packBuffer = 0;
    GLuint drawFramebuffer = 0;
    GLuint readFramebuffer = 0;
    bool blend = false;
    bool depthTest = false;
    bool stencilTest = false;
    bool scissorTest = false;
    bool cullFace = false;
    GLenum blendSourceRgb = Gl::ONE;
    GLenum blendDestinationRgb = Gl::ZERO;
    GLenum blendSourceAlpha = Gl::ONE;
    GLenum blendDestinationAlpha = Gl::ZERO;
    GLenum blendEquationRgb = Gl::FUNC_ADD;
    GLenum blendEquationAlpha = Gl::FUNC_ADD;
    f32 blendColour[4] = {};
    bool colourMask[4] = {true, true, true, true};
    bool depthMask = true;
    GLenum depthFunc = Gl::LESS;
    GLenum stencilFunc = Gl::ALWAYS;
    GLint stencilRef = 0;
    GLuint stencilValueMask = 0xFFFFFFFF;
    GLuint stencilWriteMask = 0xFFFFFFFF;
    GLenum stencilFail = Gl::KEEP;
    GLenum stencilDepthFail = Gl::KEEP;
    GLenum stencilPass = Gl::KEEP;
    GLint viewport[4] = {};
    GLint scissor[4] = {};
    f32 clearColour[4] = {};
    f32 clearDepth = 1.0f;
    GLint clearStencil = 0;
    GLint unpackAlignment = 4;
    GLint unpackRowLength = 0;
    GLint packAlignment = 4;
    GLint packRowLength = 0;
    f32 lineWidth = 1.0f;
    // The query being timed (TIME_ELAPSED), and the pool the queries' timestamps are written in
    GLuint activeQuery = 0;
    // The pipeline bound, and the state set dynamically, as last recorded (none: set again)
    VkPipeline boundPipeline = VK_NULL_HANDLE;
    bool dynamicKnown = false;
};

Context* Current();
// The commands begun (outside or in a rendering instance as they were)
VkCommandBuffer Commands(Context& c);
// The rendering instance ended (copies, barriers and reads happen outside one)
void EndRendering(Context& c);
// Every write before made visible to every read and write after (the copies', the renderings'): outside a rendering instance
void Barrier(Context& c);
// The commands submitted (their serial; 0 when there were none); waited for when asked
u64 Submit(Context& c, bool wait);
// A piece of the context's stream buffer, and a descriptor set of its pools
StreamPiece StreamAllocate(Context& c, VkDeviceSize size, VkDeviceSize alignment);
VkDescriptorSet AllocateSet(Context& c, VkDescriptorSetLayout layout);
// Submitted waiting for and signalling semaphores (the swapchain's)
u64 SubmitWith(Context& c, VkSemaphore wait, VkPipelineStageFlags waitStage, VkSemaphore signal);
// The swapchain's (vkgldevice.cpp): an image shown, the window's size in pixels
void PresentImage(Context& c, Texture& source);
void WindowSize(int* width, int* height);
// The framebuffer GL means by a binding (0: the context's default one)
Framebuffer* DrawFramebuffer(Context& c);
Framebuffer* ReadFramebuffer(Context& c);

// The draws (vkgldraw.cpp)
void Draw(Context& c, GLenum mode, GLint first, GLsizei count);
void Clear(Context& c, GLbitfield mask);
void BlitFramebuffer(Context& c, GLint sx0, GLint sy0, GLint sx1, GLint sy1, GLint dx0, GLint dy0, GLint dx1, GLint dy1, GLbitfield mask,
                     GLenum filter);
void ReadPixels(Context& c, GLint x, GLint y, GLsizei width, GLsizei height, GLenum format, GLenum type, void* pixels);
void TexSubImage(Context& c, Texture& texture, GLint level, GLint x, GLint y, GLsizei width, GLsizei height, GLenum format,
                 GLenum type, const void* pixels);
void GenerateMipmap(Context& c, Texture& texture);
void PresentWindow(Context& c);

// The programs (vkglprogram.cpp)
bool CompileShader(ShaderObject& shader);
bool LinkProgram(Program& program);
GLint UniformLocation(Program& program, const char* name);
void SetUniform(GLint location, const void* values, u32 components, GLsizei count, bool integer);
// An internal program from GLSL 450 sources (the blits'): its modules and layout, as a program's
bool InternalProgram(const char* vertex, const char* fragment, Program* out);

// The queries (vkglobjects.cpp)
void BeginQuery(Context& c, Query& query);
void EndQuery(Context& c, Query& query);
bool QueryResult(Query& query, GLuint64* result);

void Log(const char* format, ...) __attribute__((format(printf, 1, 2)));
}
