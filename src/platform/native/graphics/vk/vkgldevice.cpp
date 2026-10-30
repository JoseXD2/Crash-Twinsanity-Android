// VkGl's device: the loader (SDL's), the instance and the device (Vulkan 1.3: dynamic rendering, the dynamic depth and stencil
// state), the memory (blocks sub-allocated), the submissions (one queue, numbered, each with its fence), the work put off until a
// submission is done, and the swapchain of the window
#include "vkglinternal.h"

#include "native.h"

#include <SDL3/SDL_vulkan.h>

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <deque>
#include <list>

namespace VkGl
{
#define VKGL_DEFINE(name) PFN_##name name = nullptr;
VKGL_GLOBAL(VKGL_DEFINE)
VKGL_INSTANCE(VKGL_DEFINE)
VKGL_DEVICE(VKGL_DEFINE)
#undef VKGL_DEFINE

void Log(const char* format, ...)
{
    char text[1024];
    va_list arguments;
    va_start(arguments, format);
    std::vsnprintf(text, sizeof(text), format, arguments);
    va_end(arguments);
    Native::Log("vulkan: %s", text);
}

namespace
{
Device g_Device;
bool g_Started = false;
PFN_vkGetInstanceProcAddr g_GetInstanceProcAddr = nullptr;

const char* ResultName(VkResult result)
{
    switch (result)
    {
    case VK_SUCCESS:
        return "VK_SUCCESS";
    case VK_ERROR_OUT_OF_HOST_MEMORY:
        return "VK_ERROR_OUT_OF_HOST_MEMORY";
    case VK_ERROR_OUT_OF_DEVICE_MEMORY:
        return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
    case VK_ERROR_INITIALIZATION_FAILED:
        return "VK_ERROR_INITIALIZATION_FAILED";
    case VK_ERROR_DEVICE_LOST:
        return "VK_ERROR_DEVICE_LOST";
    case VK_ERROR_EXTENSION_NOT_PRESENT:
        return "VK_ERROR_EXTENSION_NOT_PRESENT";
    case VK_ERROR_FEATURE_NOT_PRESENT:
        return "VK_ERROR_FEATURE_NOT_PRESENT";
    case VK_ERROR_INCOMPATIBLE_DRIVER:
        return "VK_ERROR_INCOMPATIBLE_DRIVER";
    case VK_ERROR_OUT_OF_DATE_KHR:
        return "VK_ERROR_OUT_OF_DATE_KHR";
    case VK_SUBOPTIMAL_KHR:
        return "VK_SUBOPTIMAL_KHR";
    case VK_ERROR_SURFACE_LOST_KHR:
        return "VK_ERROR_SURFACE_LOST_KHR";
    default:
        return "VK_ERROR (other)";
    }
}

// ------------------------------------------------------------------------------------------------------------------ the memory
// A block of one memory type, handed out first fit; its free ranges by offset (merged with their neighbours as they come back)
constexpr VkDeviceSize BlockSize = 64ull * 1024 * 1024;

struct Block
{
    VkDeviceMemory memory = VK_NULL_HANDLE;
    u32 type = 0;
    VkDeviceSize size = 0;
    u8* mapped = nullptr;
    std::map<VkDeviceSize, VkDeviceSize> free;
};

std::mutex g_MemoryLock;
std::list<Block> g_Blocks;

bool MemoryType(u32 bits, VkMemoryPropertyFlags wanted, u32* type)
{
    const VkPhysicalDeviceMemoryProperties& m = g_Device.memory;
    for (u32 i = 0; i < m.memoryTypeCount; i++)
    {
        if ((bits & (1u << i)) != 0 && (m.memoryTypes[i].propertyFlags & wanted) == wanted)
        {
            *type = i;
            return true;
        }
    }

    return false;
}

bool TakeFrom(Block& block, VkDeviceSize size, VkDeviceSize alignment, Allocation* out)
{
    for (auto it = block.free.begin(); it != block.free.end(); ++it)
    {
        const VkDeviceSize start = (it->first + alignment - 1) / alignment * alignment;
        const VkDeviceSize end = it->first + it->second;
        if (start + size > end)
        {
            continue;
        }

        const VkDeviceSize rangeStart = it->first;
        block.free.erase(it);
        if (start > rangeStart)
        {
            block.free[rangeStart] = start - rangeStart;
        }

        if (start + size < end)
        {
            block.free[start + size] = end - (start + size);
        }

        out->memory = block.memory;
        out->offset = start;
        out->size = size;
        out->block = &block;
        out->mapped = block.mapped != nullptr ? block.mapped + start : nullptr;
        return true;
    }

    return false;
}

// ------------------------------------------------------------------------------------------------------------------ submissions
struct Pending
{
    u64 serial;
    VkFence fence;
};

std::mutex g_PendingLock;
std::deque<Pending> g_Pending;
std::vector<VkFence> g_SpareFences;
std::multimap<u64, std::function<void()>> g_After;

// ------------------------------------------------------------------------------------------------------------------ the window
struct Swapchain
{
    SDL_Window* window = nullptr;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkExtent2D extent = {};
    std::vector<VkImage> images;
    // A semaphore pair a frame in flight (acquired, drawn), by turns
    std::vector<VkSemaphore> acquired;
    std::vector<VkSemaphore> drawn;
    u32 turn = 0;
    int interval = 1;
    bool stale = true;
};

Swapchain g_Swapchain;
}

Device& GetDevice()
{
    return g_Device;
}

bool Started()
{
    return g_Started;
}

bool Allocate(const VkMemoryRequirements& needs, VkMemoryPropertyFlags wanted, Allocation* out)
{
    u32 type = 0;
    if (!MemoryType(needs.memoryTypeBits, wanted, &type))
    {
        // Host visible memory that isn't coherent too: coherent was asked for the stream (writes seen without a flush)
        Log("no memory type for 0x%x (types 0x%x)", wanted, needs.memoryTypeBits);
        return false;
    }

    std::lock_guard lock(g_MemoryLock);
    const VkDeviceSize alignment = std::max<VkDeviceSize>(needs.alignment, 256);
    if (needs.size <= BlockSize / 2)
    {
        for (Block& block : g_Blocks)
        {
            if (block.type == type && TakeFrom(block, needs.size, alignment, out))
            {
                return true;
            }
        }
    }

    // A new block (or a dedicated one for something big)
    Block block;
    block.type = type;
    block.size = std::max(BlockSize, needs.size);
    VkMemoryAllocateInfo info = {};
    info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    info.allocationSize = block.size;
    info.memoryTypeIndex = type;
    VkResult result = vkAllocateMemory(g_Device.device, &info, nullptr, &block.memory);
    if (result != VK_SUCCESS)
    {
        Log("vkAllocateMemory(%llu bytes): %s", static_cast<unsigned long long>(block.size), ResultName(result));
        return false;
    }

    if ((g_Device.memory.memoryTypes[type].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0)
    {
        void* mapped = nullptr;
        vkMapMemory(g_Device.device, block.memory, 0, VK_WHOLE_SIZE, 0, &mapped);
        block.mapped = static_cast<u8*>(mapped);
    }

    block.free[0] = block.size;
    g_Blocks.push_back(std::move(block));
    return TakeFrom(g_Blocks.back(), needs.size, alignment, out);
}

void Free(Allocation& allocation)
{
    if (allocation.memory == VK_NULL_HANDLE || allocation.block == nullptr)
    {
        allocation = Allocation{};
        return;
    }

    std::lock_guard lock(g_MemoryLock);
    Block& block = *static_cast<Block*>(allocation.block);
    VkDeviceSize start = allocation.offset;
    VkDeviceSize size = allocation.size;
    // Merged with the free ranges either side
    auto next = block.free.lower_bound(start);
    if (next != block.free.end() && next->first == start + size)
    {
        size += next->second;
        next = block.free.erase(next);
    }

    if (next != block.free.begin())
    {
        auto previous = std::prev(next);
        if (previous->first + previous->second == start)
        {
            start = previous->first;
            size += previous->second;
            block.free.erase(previous);
        }
    }

    block.free[start] = size;
    allocation = Allocation{};
}

u64 CompletedSerial()
{
    std::lock_guard lock(g_PendingLock);
    while (!g_Pending.empty() && vkGetFenceStatus(g_Device.device, g_Pending.front().fence) == VK_SUCCESS)
    {
        g_Device.completed = g_Pending.front().serial;
        vkResetFences(g_Device.device, 1, &g_Pending.front().fence);
        g_SpareFences.push_back(g_Pending.front().fence);
        g_Pending.pop_front();
    }

    if (g_Pending.empty())
    {
        g_Device.completed = g_Device.submitted.load();
    }

    return g_Device.completed;
}

void WaitSerial(u64 serial)
{
    for (;;)
    {
        VkFence fence = VK_NULL_HANDLE;
        {
            std::lock_guard lock(g_PendingLock);
            if (g_Device.completed >= serial)
            {
                return;
            }

            for (const Pending& p : g_Pending)
            {
                if (p.serial >= serial)
                {
                    fence = p.fence;
                    break;
                }
            }
        }

        if (fence == VK_NULL_HANDLE)
        {
            CompletedSerial();
            return;
        }

        vkWaitForFences(g_Device.device, 1, &fence, VK_TRUE, ~0ull);
        CompletedSerial();
    }
}

void AfterSerial(u64 serial, std::function<void()> work)
{
    std::lock_guard lock(g_PendingLock);
    g_After.emplace(serial, std::move(work));
}

void RunFinishedWork()
{
    const u64 done = CompletedSerial();
    std::vector<std::function<void()>> ready;
    {
        std::lock_guard lock(g_PendingLock);
        auto end = g_After.upper_bound(done);
        for (auto it = g_After.begin(); it != end; ++it)
        {
            ready.push_back(std::move(it->second));
        }

        g_After.erase(g_After.begin(), end);
    }

    for (auto& work : ready)
    {
        work();
    }
}

// A submission's fence: a spare one, or a new one
VkFence TakeFence()
{
    std::lock_guard lock(g_PendingLock);
    if (!g_SpareFences.empty())
    {
        VkFence fence = g_SpareFences.back();
        g_SpareFences.pop_back();
        return fence;
    }

    VkFenceCreateInfo info = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, nullptr, 0};
    VkFence fence = VK_NULL_HANDLE;
    vkCreateFence(g_Device.device, &info, nullptr, &fence);
    return fence;
}

// A queue submission of command buffers (waiting and signalling semaphores when given): its serial
u64 SubmitToQueue(VkCommandBuffer commands, VkSemaphore wait, VkPipelineStageFlags waitStage, VkSemaphore signal)
{
    VkFence fence = TakeFence();
    VkSubmitInfo submit = {};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = commands != VK_NULL_HANDLE ? 1 : 0;
    submit.pCommandBuffers = &commands;
    if (wait != VK_NULL_HANDLE)
    {
        submit.waitSemaphoreCount = 1;
        submit.pWaitSemaphores = &wait;
        submit.pWaitDstStageMask = &waitStage;
    }

    if (signal != VK_NULL_HANDLE)
    {
        submit.signalSemaphoreCount = 1;
        submit.pSignalSemaphores = &signal;
    }

    std::lock_guard queue(g_Device.queueLock);
    VkResult result = vkQueueSubmit(g_Device.queue, 1, &submit, fence);
    if (result != VK_SUCCESS)
    {
        Log("vkQueueSubmit: %s", ResultName(result));
    }

    const u64 serial = ++g_Device.submitted;
    std::lock_guard lock(g_PendingLock);
    g_Pending.push_back({serial, fence});
    return serial;
}

bool MakeReadBuffer(VkDeviceSize size, ReadBuffer* out)
{
    VkBufferCreateInfo info = {};
    info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    info.size = size;
    info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(g_Device.device, &info, nullptr, &out->buffer) != VK_SUCCESS)
    {
        return false;
    }

    VkMemoryRequirements needs;
    vkGetBufferMemoryRequirements(g_Device.device, out->buffer, &needs);
    // Cached memory for the CPU's reads where there is some (coherent or not: a read after the fence sees it on the devices here)
    if (!Allocate(needs, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                  &out->memory) &&
        !Allocate(needs, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &out->memory))
    {
        vkDestroyBuffer(g_Device.device, out->buffer, nullptr);
        out->buffer = VK_NULL_HANDLE;
        return false;
    }

    vkBindBufferMemory(g_Device.device, out->buffer, out->memory.memory, out->memory.offset);
    out->size = size;
    return true;
}

void DestroyReadBuffer(ReadBuffer& buffer)
{
    if (buffer.buffer != VK_NULL_HANDLE)
    {
        vkDestroyBuffer(g_Device.device, buffer.buffer, nullptr);
    }

    Free(buffer.memory);
    buffer = ReadBuffer{};
}

// ------------------------------------------------------------------------------------------------------------------ the device
namespace
{
bool LoadLoader()
{
    if (g_GetInstanceProcAddr != nullptr)
    {
        return true;
    }

    if (!SDL_WasInit(SDL_INIT_VIDEO) && !SDL_InitSubSystem(SDL_INIT_VIDEO))
    {
        Log("SDL's video didn't start: %s", SDL_GetError());
        return false;
    }

    if (!SDL_Vulkan_LoadLibrary(nullptr))
    {
        Log("the loader couldn't be opened: %s", SDL_GetError());
        return false;
    }

    g_GetInstanceProcAddr = reinterpret_cast<PFN_vkGetInstanceProcAddr>(SDL_Vulkan_GetVkGetInstanceProcAddr());
    if (g_GetInstanceProcAddr == nullptr)
    {
        Log("no vkGetInstanceProcAddr");
        return false;
    }

#define VKGL_LOAD_GLOBAL(name) name = reinterpret_cast<PFN_##name>(g_GetInstanceProcAddr(nullptr, #name));
    VKGL_GLOBAL(VKGL_LOAD_GLOBAL)
#undef VKGL_LOAD_GLOBAL
    return vkCreateInstance != nullptr;
}

bool HasDeviceExtension(VkPhysicalDevice physical, const char* name)
{
    u32 count = 0;
    vkEnumerateDeviceExtensionProperties(physical, nullptr, &count, nullptr);
    std::vector<VkExtensionProperties> list(count);
    vkEnumerateDeviceExtensionProperties(physical, nullptr, &count, list.data());
    for (const VkExtensionProperties& e : list)
    {
        if (std::strcmp(e.extensionName, name) == 0)
        {
            return true;
        }
    }

    return false;
}

bool MakeSwapchain(Swapchain& s)
{
    Device& d = g_Device;
    VkSurfaceCapabilitiesKHR capabilities = {};
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(d.physical, s.surface, &capabilities);
    u32 formatCount = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(d.physical, s.surface, &formatCount, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(formatCount);
    vkGetPhysicalDeviceSurfaceFormatsKHR(d.physical, s.surface, &formatCount, formats.data());
    if (formats.empty())
    {
        Log("the surface has no formats");
        return false;
    }

    VkSurfaceFormatKHR chosen = formats[0];
    for (const VkSurfaceFormatKHR& f : formats)
    {
        if (f.format == VK_FORMAT_R8G8B8A8_UNORM || f.format == VK_FORMAT_B8G8R8A8_UNORM)
        {
            chosen = f;
            break;
        }
    }

    u32 modeCount = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(d.physical, s.surface, &modeCount, nullptr);
    std::vector<VkPresentModeKHR> modes(modeCount);
    vkGetPhysicalDeviceSurfacePresentModesKHR(d.physical, s.surface, &modeCount, modes.data());
    VkPresentModeKHR mode = VK_PRESENT_MODE_FIFO_KHR;
    if (s.interval == 0)
    {
        for (VkPresentModeKHR wanted : {VK_PRESENT_MODE_MAILBOX_KHR, VK_PRESENT_MODE_IMMEDIATE_KHR})
        {
            if (std::find(modes.begin(), modes.end(), wanted) != modes.end())
            {
                mode = wanted;
                break;
            }
        }
    }

    VkExtent2D extent = capabilities.currentExtent;
    if (extent.width == 0xFFFFFFFF)
    {
        int w = 0;
        int h = 0;
        SDL_GetWindowSizeInPixels(s.window, &w, &h);
        extent = {static_cast<u32>(std::max(1, w)), static_cast<u32>(std::max(1, h))};
    }

    if (extent.width == 0 || extent.height == 0)
    {
        return false;
    }

    u32 imageCount = std::max<u32>(capabilities.minImageCount, 3);
    if (capabilities.maxImageCount != 0)
    {
        imageCount = std::min(imageCount, capabilities.maxImageCount);
    }

    VkSwapchainCreateInfoKHR info = {};
    info.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    info.surface = s.surface;
    info.minImageCount = imageCount;
    info.imageFormat = chosen.format;
    info.imageColorSpace = chosen.colorSpace;
    info.imageExtent = extent;
    info.imageArrayLayers = 1;
    info.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.preTransform = (capabilities.supportedTransforms & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR) != 0
                            ? VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR
                            : capabilities.currentTransform;
    info.compositeAlpha = (capabilities.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR) != 0
                              ? VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR
                              : VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;
    info.presentMode = mode;
    info.clipped = VK_TRUE;
    info.oldSwapchain = s.swapchain;
    VkSwapchainKHR made = VK_NULL_HANDLE;
    VkResult result = vkCreateSwapchainKHR(d.device, &info, nullptr, &made);
    if (s.swapchain != VK_NULL_HANDLE)
    {
        vkDeviceWaitIdle(d.device);
        vkDestroySwapchainKHR(d.device, s.swapchain, nullptr);
        s.swapchain = VK_NULL_HANDLE;
    }

    if (result != VK_SUCCESS)
    {
        Log("vkCreateSwapchainKHR: %s", ResultName(result));
        return false;
    }

    s.swapchain = made;
    s.format = chosen.format;
    s.extent = extent;
    u32 count = 0;
    vkGetSwapchainImagesKHR(d.device, s.swapchain, &count, nullptr);
    s.images.resize(count);
    vkGetSwapchainImagesKHR(d.device, s.swapchain, &count, s.images.data());
    VkSemaphoreCreateInfo semaphoreInfo = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, nullptr, 0};
    while (s.acquired.size() < count + 1)
    {
        VkSemaphore a = VK_NULL_HANDLE;
        VkSemaphore b = VK_NULL_HANDLE;
        vkCreateSemaphore(d.device, &semaphoreInfo, nullptr, &a);
        vkCreateSemaphore(d.device, &semaphoreInfo, nullptr, &b);
        s.acquired.push_back(a);
        s.drawn.push_back(b);
    }

    s.stale = false;
    Log("swapchain %ux%u, %u images, %s", extent.width, extent.height, count,
        mode == VK_PRESENT_MODE_FIFO_KHR ? "v-sync" : mode == VK_PRESENT_MODE_MAILBOX_KHR ? "mailbox" : "immediate");
    return true;
}
}

namespace
{
bool StartOn(SDL_Window* window);

// What a Start that failed half way made, undone (the surface above all: on Android it holds the window's surface, and GL can't
// make one of its own on it while it does)
void Unmake()
{
    Device& d = g_Device;
    Swapchain& s = g_Swapchain;
    if (d.device != VK_NULL_HANDLE)
    {
        vkDeviceWaitIdle(d.device);
        if (s.swapchain != VK_NULL_HANDLE && vkDestroySwapchainKHR != nullptr)
        {
            vkDestroySwapchainKHR(d.device, s.swapchain, nullptr);
            s.swapchain = VK_NULL_HANDLE;
        }

        if (d.pipelineCache != VK_NULL_HANDLE)
        {
            vkDestroyPipelineCache(d.device, d.pipelineCache, nullptr);
            d.pipelineCache = VK_NULL_HANDLE;
        }

        vkDestroyDevice(d.device, nullptr);
        d.device = VK_NULL_HANDLE;
    }

    if (s.surface != VK_NULL_HANDLE && d.instance != VK_NULL_HANDLE)
    {
        vkDestroySurfaceKHR(d.instance, s.surface, nullptr);
        s.surface = VK_NULL_HANDLE;
    }

    if (d.instance != VK_NULL_HANDLE)
    {
        vkDestroyInstance(d.instance, nullptr);
        d.instance = VK_NULL_HANDLE;
    }

    d.physical = VK_NULL_HANDLE;
    s.window = nullptr;
}
}

bool Supported()
{
    if (!LoadLoader())
    {
        return false;
    }

    u32 loaderVersion = VK_API_VERSION_1_0;
    if (vkEnumerateInstanceVersion != nullptr)
    {
        vkEnumerateInstanceVersion(&loaderVersion);
    }

    if (loaderVersion < VK_API_VERSION_1_3)
    {
        Log("not used: the loader has Vulkan %u.%u, 1.3 is needed", VK_API_VERSION_MAJOR(loaderVersion), VK_API_VERSION_MINOR(loaderVersion));
        return false;
    }

    // An instance of its own, the devices asked for Vulkan 1.3 and dynamic rendering, gone again
    VkApplicationInfo application = {};
    application.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    application.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo info = {};
    info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    info.pApplicationInfo = &application;
    VkInstance instance = VK_NULL_HANDLE;
    if (vkCreateInstance(&info, nullptr, &instance) != VK_SUCCESS)
    {
        Log("not used: no instance");
        return false;
    }

    auto enumerate = reinterpret_cast<PFN_vkEnumeratePhysicalDevices>(g_GetInstanceProcAddr(instance, "vkEnumeratePhysicalDevices"));
    auto properties = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(g_GetInstanceProcAddr(instance, "vkGetPhysicalDeviceProperties"));
    auto features = reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures2>(g_GetInstanceProcAddr(instance, "vkGetPhysicalDeviceFeatures2"));
    auto destroy = reinterpret_cast<PFN_vkDestroyInstance>(g_GetInstanceProcAddr(instance, "vkDestroyInstance"));
    bool found = false;
    if (enumerate != nullptr && properties != nullptr && features != nullptr)
    {
        u32 count = 0;
        enumerate(instance, &count, nullptr);
        std::vector<VkPhysicalDevice> devices(count);
        enumerate(instance, &count, devices.data());
        for (VkPhysicalDevice physical : devices)
        {
            VkPhysicalDeviceProperties p;
            properties(physical, &p);
            VkPhysicalDeviceVulkan13Features has13 = {};
            has13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
            VkPhysicalDeviceFeatures2 has = {};
            has.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
            if (p.apiVersion >= VK_API_VERSION_1_3)
            {
                has.pNext = &has13;
                features(physical, &has);
            }

            if (p.apiVersion >= VK_API_VERSION_1_3 && has13.dynamicRendering)
            {
                found = true;
            }
            else
            {
                Log("not used on %s: its driver has Vulkan %u.%u%s (1.3 with dynamic rendering is needed)", p.deviceName,
                    VK_API_VERSION_MAJOR(p.apiVersion), VK_API_VERSION_MINOR(p.apiVersion),
                    p.apiVersion >= VK_API_VERSION_1_3 ? " without dynamic rendering" : "");
            }
        }
    }

    if (destroy != nullptr)
    {
        destroy(instance, nullptr);
    }

    return found;
}

bool Start(SDL_Window* window)
{
    if (StartOn(window))
    {
        return true;
    }

    Unmake();
    return false;
}

namespace
{
bool StartOn(SDL_Window* window)
{
    if (g_Started)
    {
        return true;
    }

    if (!LoadLoader())
    {
        return false;
    }

    Device& d = g_Device;
    u32 loaderVersion = VK_API_VERSION_1_0;
    if (vkEnumerateInstanceVersion != nullptr)
    {
        vkEnumerateInstanceVersion(&loaderVersion);
    }

    if (loaderVersion < VK_API_VERSION_1_3)
    {
        Log("the loader has Vulkan %u.%u: 1.3 is needed", VK_API_VERSION_MAJOR(loaderVersion), VK_API_VERSION_MINOR(loaderVersion));
        return false;
    }

    std::vector<const char*> instanceExtensions;
    if (window != nullptr)
    {
        Uint32 count = 0;
        const char* const* names = SDL_Vulkan_GetInstanceExtensions(&count);
        if (names == nullptr)
        {
            Log("SDL has no surface extensions: %s", SDL_GetError());
            return false;
        }

        instanceExtensions.assign(names, names + count);
    }

    VkApplicationInfo application = {};
    application.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    application.pApplicationName = "Crash Twinsanity";
    application.pEngineName = "twinsanity-vkgl";
    application.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo instanceInfo = {};
    instanceInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    instanceInfo.pApplicationInfo = &application;
    instanceInfo.enabledExtensionCount = static_cast<u32>(instanceExtensions.size());
    instanceInfo.ppEnabledExtensionNames = instanceExtensions.data();
    // $TWIN_VULKAN_VALIDATION=1: the Khronos validation layer (where it's installed)
    const char* validation = "VK_LAYER_KHRONOS_validation";
    const char* wantValidation = std::getenv("TWIN_VULKAN_VALIDATION");
    if (wantValidation != nullptr && wantValidation[0] == '1')
    {
        instanceInfo.enabledLayerCount = 1;
        instanceInfo.ppEnabledLayerNames = &validation;
    }

    VkResult result = vkCreateInstance(&instanceInfo, nullptr, &d.instance);
    if (result != VK_SUCCESS && instanceInfo.enabledLayerCount != 0)
    {
        instanceInfo.enabledLayerCount = 0;
        result = vkCreateInstance(&instanceInfo, nullptr, &d.instance);
    }

    if (result != VK_SUCCESS)
    {
        Log("vkCreateInstance: %s", ResultName(result));
        return false;
    }

#define VKGL_LOAD_INSTANCE(name) name = reinterpret_cast<PFN_##name>(g_GetInstanceProcAddr(d.instance, #name));
    VKGL_INSTANCE(VKGL_LOAD_INSTANCE)
#undef VKGL_LOAD_INSTANCE
    Swapchain& s = g_Swapchain;
    if (window != nullptr && !SDL_Vulkan_CreateSurface(window, d.instance, nullptr, &s.surface))
    {
        Log("SDL_Vulkan_CreateSurface: %s", SDL_GetError());
        return false;
    }

    u32 count = 0;
    vkEnumeratePhysicalDevices(d.instance, &count, nullptr);
    std::vector<VkPhysicalDevice> devices(count);
    vkEnumeratePhysicalDevices(d.instance, &count, devices.data());
    // $TWIN_VULKAN_DEVICE: a device's number when there are several (a PC's)
    const char* wantedDevice = std::getenv("TWIN_VULKAN_DEVICE");
    for (u32 i = 0; i < count; i++)
    {
        if (wantedDevice != nullptr && static_cast<u32>(std::atoi(wantedDevice)) != i)
        {
            continue;
        }

        VkPhysicalDeviceProperties properties;
        vkGetPhysicalDeviceProperties(devices[i], &properties);
        if (properties.apiVersion < VK_API_VERSION_1_3)
        {
            continue;
        }

        u32 families = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(devices[i], &families, nullptr);
        std::vector<VkQueueFamilyProperties> list(families);
        vkGetPhysicalDeviceQueueFamilyProperties(devices[i], &families, list.data());
        for (u32 f = 0; f < families; f++)
        {
            VkBool32 presents = VK_TRUE;
            if (s.surface != VK_NULL_HANDLE)
            {
                vkGetPhysicalDeviceSurfaceSupportKHR(devices[i], f, s.surface, &presents);
            }

            if ((list[f].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0 && presents)
            {
                // A discrete GPU over an integrated one over the others
                const bool better = d.physical == VK_NULL_HANDLE ||
                                    (properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU &&
                                     d.properties.deviceType != VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU);
                if (better)
                {
                    d.physical = devices[i];
                    d.properties = properties;
                    d.queueFamily = f;
                }

                break;
            }
        }
    }

    if (d.physical == VK_NULL_HANDLE)
    {
        Log("no Vulkan 1.3 device with a queue that draws%s", s.surface != VK_NULL_HANDLE ? " and presents" : "");
        return false;
    }

    vkGetPhysicalDeviceMemoryProperties(d.physical, &d.memory);
    VkPhysicalDeviceFeatures has = {};
    vkGetPhysicalDeviceFeatures(d.physical, &has);
    VkPhysicalDeviceVulkan13Features has13 = {};
    has13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
    VkPhysicalDeviceFeatures2 has2 = {};
    has2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    VkPhysicalDeviceVulkan12Features has12 = {};
    has12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    has13.pNext = &has12;
    has2.pNext = &has13;
    vkGetPhysicalDeviceFeatures2(d.physical, &has2);
    if (!has13.dynamicRendering)
    {
        Log("the device has no dynamic rendering");
        return false;
    }

    float priority = 1.0f;
    VkDeviceQueueCreateInfo queue = {};
    queue.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queue.queueFamilyIndex = d.queueFamily;
    queue.queueCount = 1;
    queue.pQueuePriorities = &priority;
    VkPhysicalDeviceVulkan13Features want13 = {};
    want13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
    want13.dynamicRendering = VK_TRUE;
    VkPhysicalDeviceVulkan12Features want12 = {};
    want12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    want12.hostQueryReset = has12.hostQueryReset;
    want13.pNext = &want12;
    VkPhysicalDeviceFeatures2 want = {};
    want.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    want.pNext = &want13;
    want.features.dualSrcBlend = has.dualSrcBlend;
    want.features.samplerAnisotropy = has.samplerAnisotropy;
    want.features.wideLines = has.wideLines;
    want.features.largePoints = has.largePoints;
    want.features.independentBlend = has.independentBlend;
    std::vector<const char*> extensions;
    if (s.surface != VK_NULL_HANDLE)
    {
        if (!HasDeviceExtension(d.physical, VK_KHR_SWAPCHAIN_EXTENSION_NAME))
        {
            Log("the device has no swapchain");
            return false;
        }

        extensions.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
    }

    VkDeviceCreateInfo deviceInfo = {};
    deviceInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    deviceInfo.pNext = &want;
    deviceInfo.queueCreateInfoCount = 1;
    deviceInfo.pQueueCreateInfos = &queue;
    deviceInfo.enabledExtensionCount = static_cast<u32>(extensions.size());
    deviceInfo.ppEnabledExtensionNames = extensions.data();
    result = vkCreateDevice(d.physical, &deviceInfo, nullptr, &d.device);
    if (result != VK_SUCCESS)
    {
        Log("vkCreateDevice: %s", ResultName(result));
        return false;
    }

#define VKGL_LOAD_DEVICE(name)                                                                                                 \
    name = reinterpret_cast<PFN_##name>(vkGetDeviceProcAddr(d.device, #name));                                                 \
    if (name == nullptr)                                                                                                       \
    {                                                                                                                          \
        Log("the device has no %s", #name);                                                                                    \
    }
    VKGL_DEVICE(VKGL_LOAD_DEVICE)
#undef VKGL_LOAD_DEVICE
    if (!has12.hostQueryReset)
    {
        vkResetQueryPool = nullptr;
    }

    vkGetDeviceQueue(d.device, d.queueFamily, 0, &d.queue);
    d.dualSource = has.dualSrcBlend != 0;
    d.anisotropy = has.samplerAnisotropy != 0;
    d.maxAnisotropy = d.properties.limits.maxSamplerAnisotropy;
    d.wideLines = has.wideLines != 0;
    d.timestamps = d.properties.limits.timestampComputeAndGraphics != 0;
    d.timestampPeriod = d.properties.limits.timestampPeriod;
    VkPipelineCacheCreateInfo cacheInfo = {VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO, nullptr, 0, 0, nullptr};
    vkCreatePipelineCache(d.device, &cacheInfo, nullptr, &d.pipelineCache);
    if (window != nullptr)
    {
        s.window = window;
        if (!MakeSwapchain(s))
        {
            return false;
        }
    }

    g_Started = true;
    Log("OpenGL in Vulkan on %s (Vulkan %u.%u.%u, driver 0x%x): dual-source blend %s, anisotropy %s", d.properties.deviceName,
        VK_API_VERSION_MAJOR(d.properties.apiVersion), VK_API_VERSION_MINOR(d.properties.apiVersion),
        VK_API_VERSION_PATCH(d.properties.apiVersion), d.properties.driverVersion, d.dualSource ? "yes" : "no",
        d.anisotropy ? "yes" : "no");
    return true;
}
}

void Stop()
{
    if (!g_Started)
    {
        return;
    }

    Device& d = g_Device;
    vkDeviceWaitIdle(d.device);
    RunFinishedWork();
    g_Started = false;
}

void SetSwapInterval(int interval)
{
    if (g_Swapchain.interval != interval)
    {
        g_Swapchain.interval = interval;
        g_Swapchain.stale = true;
    }
}

// The window's image (its colour texture) copied upside down into the swapchain's next image and presented; the swapchain made again
// when the window's size changed
void PresentImage(Context& c, Texture& source)
{
    Device& d = g_Device;
    Swapchain& s = g_Swapchain;
    if (s.swapchain == VK_NULL_HANDLE || s.stale)
    {
        Submit(c, false);
        if (!MakeSwapchain(s))
        {
            return;
        }
    }

    EndRendering(c);
    const u32 turn = s.turn++ % static_cast<u32>(s.acquired.size());
    VkSemaphore acquired = s.acquired[turn];
    VkSemaphore drawn = s.drawn[turn];
    u32 index = 0;
    VkResult result = vkAcquireNextImageKHR(d.device, s.swapchain, ~0ull, acquired, VK_NULL_HANDLE, &index);
    if (result == VK_ERROR_OUT_OF_DATE_KHR)
    {
        s.stale = true;
        return;
    }

    if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR)
    {
        Log("vkAcquireNextImageKHR: %s", ResultName(result));
        return;
    }

    VkCommandBuffer commands = Commands(c);
    Barrier(c);
    VkImage target = s.images[index];
    VkImageMemoryBarrier toCopy = {};
    toCopy.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    toCopy.srcAccessMask = 0;
    toCopy.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    toCopy.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    toCopy.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    toCopy.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toCopy.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toCopy.image = target;
    toCopy.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                         &toCopy);
    // GL's bottom row the screen's: the rows reversed
    VkImageBlit blit = {};
    blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    blit.srcOffsets[0] = {0, static_cast<s32>(source.height), 0};
    blit.srcOffsets[1] = {static_cast<s32>(source.width), 0, 1};
    blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    blit.dstOffsets[0] = {0, 0, 0};
    blit.dstOffsets[1] = {static_cast<s32>(s.extent.width), static_cast<s32>(s.extent.height), 1};
    vkCmdBlitImage(commands, source.image, VK_IMAGE_LAYOUT_GENERAL, target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit,
                   source.width == s.extent.width && source.height == s.extent.height ? VK_FILTER_NEAREST : VK_FILTER_LINEAR);
    VkImageMemoryBarrier toPresent = toCopy;
    toPresent.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    toPresent.dstAccessMask = 0;
    toPresent.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    toPresent.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, 1,
                         &toPresent);
    SubmitWith(c, acquired, VK_PIPELINE_STAGE_TRANSFER_BIT, drawn);
    VkPresentInfoKHR present = {};
    present.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    present.waitSemaphoreCount = 1;
    present.pWaitSemaphores = &drawn;
    present.swapchainCount = 1;
    present.pSwapchains = &s.swapchain;
    present.pImageIndices = &index;
    {
        std::lock_guard queue(d.queueLock);
        result = vkQueuePresentKHR(d.queue, &present);
    }

    // (VK_SUBOPTIMAL_KHR kept: Android says it whenever the screen is turned from the panel's own orientation and the swapchain
    // isn't pre-rotated; the compositor turns it. Made again for it, the swapchain was suboptimal again at once: a new one, with
    // its buffers allocated, every frame)
    if (result == VK_ERROR_OUT_OF_DATE_KHR)
    {
        s.stale = true;
    }
    else if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR)
    {
        Log("vkQueuePresentKHR: %s", ResultName(result));
    }

    // The window resized: the swapchain made again before the next frame
    int w = 0;
    int h = 0;
    SDL_GetWindowSizeInPixels(s.window, &w, &h);
    if (static_cast<u32>(w) != s.extent.width || static_cast<u32>(h) != s.extent.height)
    {
        s.stale = true;
    }
}

void WindowSize(int* width, int* height)
{
    if (g_Swapchain.window != nullptr)
    {
        SDL_GetWindowSizeInPixels(g_Swapchain.window, width, height);
    }
}
}
