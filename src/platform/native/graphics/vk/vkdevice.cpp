// Vulkan's loader, instance, device and swapchain (vkdevice.h). The functions are taken from the loader SDL opens
// (SDL_Vulkan_LoadLibrary) through vkGetInstanceProcAddr, so nothing links against a Vulkan library: a phone or PC without one
// says so in the log and the OpenGL path stays.

#include "vkdevice.h"

#include "native.h"

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <vector>

namespace NativeVulkan
{
namespace
{
// The functions used, loaded by name: the global and instance ones through vkGetInstanceProcAddr, the device's through
// vkGetDeviceProcAddr (the direct entry points, past the loader's dispatch)
#define VK_GLOBAL_FUNCTIONS(X)                                                                                                 \
    X(vkCreateInstance)                                                                                                        \
    X(vkEnumerateInstanceExtensionProperties)                                                                                  \
    X(vkEnumerateInstanceVersion)

#define VK_INSTANCE_FUNCTIONS(X)                                                                                               \
    X(vkDestroyInstance)                                                                                                       \
    X(vkEnumeratePhysicalDevices)                                                                                              \
    X(vkGetPhysicalDeviceProperties)                                                                                           \
    X(vkGetPhysicalDeviceFeatures)                                                                                             \
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

#define VK_DEVICE_FUNCTIONS(X)                                                                                                 \
    X(vkDestroyDevice)                                                                                                         \
    X(vkGetDeviceQueue)                                                                                                        \
    X(vkDeviceWaitIdle)                                                                                                        \
    X(vkCreateSwapchainKHR)                                                                                                    \
    X(vkDestroySwapchainKHR)                                                                                                   \
    X(vkGetSwapchainImagesKHR)                                                                                                 \
    X(vkAcquireNextImageKHR)                                                                                                   \
    X(vkQueuePresentKHR)                                                                                                       \
    X(vkCreateCommandPool)                                                                                                     \
    X(vkDestroyCommandPool)                                                                                                    \
    X(vkAllocateCommandBuffers)                                                                                                \
    X(vkResetCommandPool)                                                                                                      \
    X(vkBeginCommandBuffer)                                                                                                    \
    X(vkEndCommandBuffer)                                                                                                      \
    X(vkCmdPipelineBarrier)                                                                                                    \
    X(vkCmdClearColorImage)                                                                                                    \
    X(vkQueueSubmit)                                                                                                           \
    X(vkCreateSemaphore)                                                                                                       \
    X(vkDestroySemaphore)                                                                                                      \
    X(vkCreateFence)                                                                                                           \
    X(vkDestroyFence)                                                                                                          \
    X(vkWaitForFences)                                                                                                         \
    X(vkResetFences)

#define VK_DECLARE(name) PFN_##name name = nullptr;
struct Functions
{
    PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr = nullptr;
    VK_GLOBAL_FUNCTIONS(VK_DECLARE)
    VK_INSTANCE_FUNCTIONS(VK_DECLARE)
    VK_DEVICE_FUNCTIONS(VK_DECLARE)
};
#undef VK_DECLARE

Functions g_Vk;

const char* ResultName(VkResult result)
{
    switch (result)
    {
    case VK_SUCCESS:
        return "VK_SUCCESS";
    case VK_NOT_READY:
        return "VK_NOT_READY";
    case VK_TIMEOUT:
        return "VK_TIMEOUT";
    case VK_SUBOPTIMAL_KHR:
        return "VK_SUBOPTIMAL_KHR";
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
    case VK_ERROR_SURFACE_LOST_KHR:
        return "VK_ERROR_SURFACE_LOST_KHR";
    case VK_ERROR_OUT_OF_DATE_KHR:
        return "VK_ERROR_OUT_OF_DATE_KHR";
    default:
        return "VK_ERROR (other)";
    }
}

std::string Format(const char* format, ...) __attribute__((format(printf, 1, 2)));
std::string Format(const char* format, ...)
{
    char text[512];
    va_list args;
    va_start(args, format);
    std::vsnprintf(text, sizeof(text), format, args);
    va_end(args);
    return text;
}

// A report's line: logged as it's added
void Say(std::string* report, const std::string& line)
{
    Native::Log("vulkan: %s", line.c_str());
    if (report != nullptr)
    {
        *report += line + "\n";
    }
}

// The loader opened and the global functions taken (once; false with the reason when there's no loader)
bool LoadLoader(std::string* error)
{
    static int state = 0;
    static std::string reason;
    if (state == 0)
    {
        state = -1;
        // (the loader is opened by SDL's video subsystem: started here when the probe runs before the window is made; the
        // platform's own start of it later only adds to SDL's count)
        if (!SDL_WasInit(SDL_INIT_VIDEO) && !SDL_InitSubSystem(SDL_INIT_VIDEO))
        {
            reason = Format("SDL's video didn't start: %s", SDL_GetError());
        }
        else if (!SDL_Vulkan_LoadLibrary(nullptr))
        {
            reason = Format("the loader couldn't be opened: %s", SDL_GetError());
        }
        else
        {
            g_Vk.vkGetInstanceProcAddr = reinterpret_cast<PFN_vkGetInstanceProcAddr>(SDL_Vulkan_GetVkGetInstanceProcAddr());
            if (g_Vk.vkGetInstanceProcAddr == nullptr)
            {
                reason = "no vkGetInstanceProcAddr in the loader";
            }
            else
            {
#define VK_LOAD_GLOBAL(name) g_Vk.name = reinterpret_cast<PFN_##name>(g_Vk.vkGetInstanceProcAddr(nullptr, #name));
                VK_GLOBAL_FUNCTIONS(VK_LOAD_GLOBAL)
#undef VK_LOAD_GLOBAL
                if (g_Vk.vkCreateInstance == nullptr || g_Vk.vkEnumerateInstanceExtensionProperties == nullptr)
                {
                    reason = "the loader has no vkCreateInstance";
                }
                else
                {
                    state = 1;
                }
            }
        }
    }

    if (state != 1 && error != nullptr)
    {
        *error = reason;
    }

    return state == 1;
}

struct Instance
{
    VkInstance instance = VK_NULL_HANDLE;
    u32 apiVersion = VK_API_VERSION_1_0;
    std::vector<std::string> extensions;
};

// An instance with the surface extensions the window needs (SDL's list), at the newest API the loader has up to 1.3
bool MakeInstance(Instance* made, bool forSurface, std::string* error)
{
    if (!LoadLoader(error))
    {
        return false;
    }

    u32 loaderVersion = VK_API_VERSION_1_0;
    if (g_Vk.vkEnumerateInstanceVersion != nullptr)
    {
        g_Vk.vkEnumerateInstanceVersion(&loaderVersion);
    }

    made->apiVersion = std::min<u32>(loaderVersion, VK_API_VERSION_1_3);
    std::vector<const char*> wanted;
    if (forSurface)
    {
        Uint32 count = 0;
        const char* const* names = SDL_Vulkan_GetInstanceExtensions(&count);
        if (names == nullptr)
        {
            *error = Format("SDL has no surface extensions here: %s", SDL_GetError());
            return false;
        }

        wanted.assign(names, names + count);
    }

    // The instance's extensions there are, for the report
    u32 count = 0;
    g_Vk.vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr);
    std::vector<VkExtensionProperties> available(count);
    g_Vk.vkEnumerateInstanceExtensionProperties(nullptr, &count, available.data());
    for (const VkExtensionProperties& extension : available)
    {
        made->extensions.push_back(extension.extensionName);
    }

    VkApplicationInfo application = {};
    application.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    application.pApplicationName = "Crash Twinsanity";
    application.applicationVersion = 1;
    application.pEngineName = "twinsanity-native";
    application.engineVersion = 1;
    application.apiVersion = made->apiVersion;
    VkInstanceCreateInfo info = {};
    info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    info.pApplicationInfo = &application;
    info.enabledExtensionCount = static_cast<u32>(wanted.size());
    info.ppEnabledExtensionNames = wanted.data();
    VkResult result = g_Vk.vkCreateInstance(&info, nullptr, &made->instance);
    if (result != VK_SUCCESS)
    {
        *error = Format("vkCreateInstance: %s", ResultName(result));
        return false;
    }

#define VK_LOAD_INSTANCE(name) g_Vk.name = reinterpret_cast<PFN_##name>(g_Vk.vkGetInstanceProcAddr(made->instance, #name));
    VK_INSTANCE_FUNCTIONS(VK_LOAD_INSTANCE)
#undef VK_LOAD_INSTANCE
    if (g_Vk.vkEnumeratePhysicalDevices == nullptr || g_Vk.vkCreateDevice == nullptr || g_Vk.vkGetDeviceProcAddr == nullptr)
    {
        *error = "the instance has no vkEnumeratePhysicalDevices or vkCreateDevice";
        g_Vk.vkDestroyInstance(made->instance, nullptr);
        made->instance = VK_NULL_HANDLE;
        return false;
    }

    return true;
}

void DestroyInstance(Instance* instance)
{
    if (instance->instance != VK_NULL_HANDLE && g_Vk.vkDestroyInstance != nullptr)
    {
        g_Vk.vkDestroyInstance(instance->instance, nullptr);
    }

    instance->instance = VK_NULL_HANDLE;
}

// The first physical device (a phone has one; a PC's first is the loader's preferred), its queue family that draws and
// presents, and what it offers
struct Gpu
{
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkPhysicalDeviceProperties properties = {};
    VkPhysicalDeviceFeatures features = {};
    VkPhysicalDeviceMemoryProperties memory = {};
    std::vector<std::string> extensions;
    u32 queueFamily = ~0u;
};

bool HasExtension(const std::vector<std::string>& extensions, const char* name)
{
    return std::find(extensions.begin(), extensions.end(), name) != extensions.end();
}

bool PickGpu(const Instance& instance, VkSurfaceKHR surface, Gpu* gpu, std::string* error)
{
    u32 count = 0;
    g_Vk.vkEnumeratePhysicalDevices(instance.instance, &count, nullptr);
    if (count == 0)
    {
        *error = "no Vulkan device";
        return false;
    }

    std::vector<VkPhysicalDevice> devices(count);
    g_Vk.vkEnumeratePhysicalDevices(instance.instance, &count, devices.data());
    for (VkPhysicalDevice physical : devices)
    {
        Gpu candidate;
        candidate.physical = physical;
        g_Vk.vkGetPhysicalDeviceProperties(physical, &candidate.properties);
        g_Vk.vkGetPhysicalDeviceFeatures(physical, &candidate.features);
        g_Vk.vkGetPhysicalDeviceMemoryProperties(physical, &candidate.memory);
        u32 extensionCount = 0;
        g_Vk.vkEnumerateDeviceExtensionProperties(physical, nullptr, &extensionCount, nullptr);
        std::vector<VkExtensionProperties> extensions(extensionCount);
        g_Vk.vkEnumerateDeviceExtensionProperties(physical, nullptr, &extensionCount, extensions.data());
        for (const VkExtensionProperties& extension : extensions)
        {
            candidate.extensions.push_back(extension.extensionName);
        }

        u32 familyCount = 0;
        g_Vk.vkGetPhysicalDeviceQueueFamilyProperties(physical, &familyCount, nullptr);
        std::vector<VkQueueFamilyProperties> families(familyCount);
        g_Vk.vkGetPhysicalDeviceQueueFamilyProperties(physical, &familyCount, families.data());
        for (u32 i = 0; i < familyCount; i++)
        {
            if ((families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) == 0)
            {
                continue;
            }

            if (surface != VK_NULL_HANDLE)
            {
                VkBool32 presents = VK_FALSE;
                g_Vk.vkGetPhysicalDeviceSurfaceSupportKHR(physical, i, surface, &presents);
                if (!presents)
                {
                    continue;
                }
            }

            candidate.queueFamily = i;
            break;
        }

        if (candidate.queueFamily == ~0u)
        {
            continue;
        }

        // A discrete GPU over an integrated one on a PC; a phone has the one
        if (gpu->physical == VK_NULL_HANDLE || (candidate.properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU &&
                                                gpu->properties.deviceType != VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU))
        {
            *gpu = candidate;
        }
    }

    if (gpu->physical == VK_NULL_HANDLE)
    {
        *error = "no device with a queue that draws (and presents to the window)";
        return false;
    }

    return true;
}

struct Device
{
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    u32 queueFamily = ~0u;
};

// The logical device with the features the renderer's design counts on where they exist (dual-source blending, independent
// blending, stores from fragment shaders), and the swapchain extension when there's a window
bool MakeDevice(const Gpu& gpu, bool forSurface, Device* made, std::string* error)
{
    float priority = 1.0f;
    VkDeviceQueueCreateInfo queue = {};
    queue.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queue.queueFamilyIndex = gpu.queueFamily;
    queue.queueCount = 1;
    queue.pQueuePriorities = &priority;
    VkPhysicalDeviceFeatures features = {};
    features.dualSrcBlend = gpu.features.dualSrcBlend;
    features.independentBlend = gpu.features.independentBlend;
    features.fragmentStoresAndAtomics = gpu.features.fragmentStoresAndAtomics;
    features.shaderStorageImageExtendedFormats = gpu.features.shaderStorageImageExtendedFormats;
    features.largePoints = gpu.features.largePoints;
    std::vector<const char*> extensions;
    if (forSurface)
    {
        if (!HasExtension(gpu.extensions, VK_KHR_SWAPCHAIN_EXTENSION_NAME))
        {
            *error = "the device has no VK_KHR_swapchain";
            return false;
        }

        extensions.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
    }

    VkDeviceCreateInfo info = {};
    info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    info.queueCreateInfoCount = 1;
    info.pQueueCreateInfos = &queue;
    info.pEnabledFeatures = &features;
    info.enabledExtensionCount = static_cast<u32>(extensions.size());
    info.ppEnabledExtensionNames = extensions.data();
    VkResult result = g_Vk.vkCreateDevice(gpu.physical, &info, nullptr, &made->device);
    if (result != VK_SUCCESS)
    {
        *error = Format("vkCreateDevice: %s", ResultName(result));
        return false;
    }

#define VK_LOAD_DEVICE(name) g_Vk.name = reinterpret_cast<PFN_##name>(g_Vk.vkGetDeviceProcAddr(made->device, #name));
    VK_DEVICE_FUNCTIONS(VK_LOAD_DEVICE)
#undef VK_LOAD_DEVICE
    made->queueFamily = gpu.queueFamily;
    g_Vk.vkGetDeviceQueue(made->device, gpu.queueFamily, 0, &made->queue);
    return true;
}

void DestroyDevice(Device* device)
{
    if (device->device != VK_NULL_HANDLE)
    {
        g_Vk.vkDeviceWaitIdle(device->device);
        g_Vk.vkDestroyDevice(device->device, nullptr);
    }

    device->device = VK_NULL_HANDLE;
}

// Whether a format can be what the renderer uses it for (its optimal tiling features)
std::string FormatReport(const Gpu& gpu, VkFormat format, const char* name, VkFormatFeatureFlags wanted)
{
    VkFormatProperties properties = {};
    g_Vk.vkGetPhysicalDeviceFormatProperties(gpu.physical, format, &properties);
    const VkFormatFeatureFlags has = properties.optimalTilingFeatures;
    std::string missing;
    auto check = [&](VkFormatFeatureFlags bit, const char* what)
    {
        if ((wanted & bit) != 0 && (has & bit) == 0)
        {
            missing += std::string(missing.empty() ? "" : ", ") + what;
        }
    };
    check(VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT, "sampled");
    check(VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT, "storage");
    check(VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT, "colour attachment");
    check(VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT, "blend");
    check(VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT, "depth/stencil attachment");
    check(VK_FORMAT_FEATURE_BLIT_SRC_BIT, "blit source");
    check(VK_FORMAT_FEATURE_BLIT_DST_BIT, "blit destination");
    return Format("  %s: %s", name, missing.empty() ? "all wanted" : ("missing " + missing).c_str());
}

struct Swapchain
{
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkExtent2D extent = {};
    std::vector<VkImage> images;
};

bool MakeSwapchain(const Instance& instance, const Gpu& gpu, const Device& device, SDL_Window* window, Swapchain* made,
                   std::string* error)
{
    if (!SDL_Vulkan_CreateSurface(window, instance.instance, nullptr, &made->surface))
    {
        *error = Format("SDL_Vulkan_CreateSurface: %s", SDL_GetError());
        return false;
    }

    VkSurfaceCapabilitiesKHR capabilities = {};
    g_Vk.vkGetPhysicalDeviceSurfaceCapabilitiesKHR(gpu.physical, made->surface, &capabilities);
    u32 formatCount = 0;
    g_Vk.vkGetPhysicalDeviceSurfaceFormatsKHR(gpu.physical, made->surface, &formatCount, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(formatCount);
    g_Vk.vkGetPhysicalDeviceSurfaceFormatsKHR(gpu.physical, made->surface, &formatCount, formats.data());
    if (formats.empty())
    {
        *error = "the surface has no formats";
        return false;
    }

    // 8 bit RGBA or BGRA, sRGB-less (the picture is the PS2's bytes)
    VkSurfaceFormatKHR chosen = formats[0];
    for (const VkSurfaceFormatKHR& format : formats)
    {
        if (format.format == VK_FORMAT_R8G8B8A8_UNORM || format.format == VK_FORMAT_B8G8R8A8_UNORM)
        {
            chosen = format;
            break;
        }
    }

    u32 presentCount = 0;
    g_Vk.vkGetPhysicalDeviceSurfacePresentModesKHR(gpu.physical, made->surface, &presentCount, nullptr);
    std::vector<VkPresentModeKHR> presentModes(presentCount);
    g_Vk.vkGetPhysicalDeviceSurfacePresentModesKHR(gpu.physical, made->surface, &presentCount, presentModes.data());
    // FIFO is always there (vsync); the game paces its own frames, so FIFO it is for the test
    const VkPresentModeKHR presentMode = VK_PRESENT_MODE_FIFO_KHR;
    made->extent = capabilities.currentExtent;
    if (made->extent.width == 0xFFFFFFFF)
    {
        int w = 0;
        int h = 0;
        SDL_GetWindowSizeInPixels(window, &w, &h);
        made->extent = {static_cast<u32>(w), static_cast<u32>(h)};
    }

    u32 imageCount = std::max<u32>(capabilities.minImageCount, 3);
    if (capabilities.maxImageCount != 0)
    {
        imageCount = std::min(imageCount, capabilities.maxImageCount);
    }

    VkSwapchainCreateInfoKHR info = {};
    info.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    info.surface = made->surface;
    info.minImageCount = imageCount;
    info.imageFormat = chosen.format;
    info.imageColorSpace = chosen.colorSpace;
    info.imageExtent = made->extent;
    info.imageArrayLayers = 1;
    info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.preTransform = (capabilities.supportedTransforms & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR) != 0
                            ? VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR
                            : capabilities.currentTransform;
    info.compositeAlpha = (capabilities.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR) != 0
                              ? VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR
                              : VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;
    info.presentMode = presentMode;
    info.clipped = VK_TRUE;
    VkResult result = g_Vk.vkCreateSwapchainKHR(device.device, &info, nullptr, &made->swapchain);
    if (result != VK_SUCCESS)
    {
        *error = Format("vkCreateSwapchainKHR: %s", ResultName(result));
        return false;
    }

    made->format = chosen.format;
    u32 count = 0;
    g_Vk.vkGetSwapchainImagesKHR(device.device, made->swapchain, &count, nullptr);
    made->images.resize(count);
    g_Vk.vkGetSwapchainImagesKHR(device.device, made->swapchain, &count, made->images.data());
    return true;
}

void DestroySwapchain(const Instance& instance, const Device& device, Swapchain* swapchain)
{
    if (swapchain->swapchain != VK_NULL_HANDLE)
    {
        g_Vk.vkDestroySwapchainKHR(device.device, swapchain->swapchain, nullptr);
    }

    if (swapchain->surface != VK_NULL_HANDLE)
    {
        g_Vk.vkDestroySurfaceKHR(instance.instance, swapchain->surface, nullptr);
    }

    swapchain->swapchain = VK_NULL_HANDLE;
    swapchain->surface = VK_NULL_HANDLE;
}

// An image's layout changed, with every stage waited for (the test's frames are few)
void Transition(VkCommandBuffer commands, VkImage image, VkImageLayout from, VkImageLayout to, VkAccessFlags fromAccess,
                VkAccessFlags toAccess)
{
    VkImageMemoryBarrier barrier = {};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.srcAccessMask = fromAccess;
    barrier.dstAccessMask = toAccess;
    barrier.oldLayout = from;
    barrier.newLayout = to;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    g_Vk.vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0,
                              nullptr, 1, &barrier);
}
}

bool Probe(std::string* report)
{
    std::string error;
    Instance instance;
    if (!MakeInstance(&instance, false, &error))
    {
        Say(report, "not usable: " + error);
        return false;
    }

    Say(report, Format("loader API %u.%u, instance at %u.%u, %zu instance extensions", VK_API_VERSION_MAJOR(instance.apiVersion),
                       VK_API_VERSION_MINOR(instance.apiVersion), VK_API_VERSION_MAJOR(instance.apiVersion),
                       VK_API_VERSION_MINOR(instance.apiVersion), instance.extensions.size()));
    Gpu gpu;
    if (!PickGpu(instance, VK_NULL_HANDLE, &gpu, &error))
    {
        Say(report, "not usable: " + error);
        DestroyInstance(&instance);
        return false;
    }

    const VkPhysicalDeviceProperties& p = gpu.properties;
    Say(report, Format("device: %s (vendor %04x, device %04x, driver %u.%u.%u), API %u.%u.%u, %s", p.deviceName, p.vendorID, p.deviceID,
                       VK_API_VERSION_MAJOR(p.driverVersion), VK_API_VERSION_MINOR(p.driverVersion), VK_API_VERSION_PATCH(p.driverVersion),
                       VK_API_VERSION_MAJOR(p.apiVersion), VK_API_VERSION_MINOR(p.apiVersion), VK_API_VERSION_PATCH(p.apiVersion),
                       p.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? "integrated" : p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? "discrete" : "other"));
    const VkPhysicalDeviceFeatures& f = gpu.features;
    Say(report, Format("features: dual-source blend %s, independent blend %s, fragment stores and atomics %s, storage image extended formats %s, "
                       "depth clamp %s, large points %s, wide lines %s, shader int16 %s, shader int64 %s, timestamps %s (%.1f ns a tick)",
                       f.dualSrcBlend ? "yes" : "NO", f.independentBlend ? "yes" : "NO", f.fragmentStoresAndAtomics ? "yes" : "NO",
                       f.shaderStorageImageExtendedFormats ? "yes" : "NO", f.depthClamp ? "yes" : "no", f.largePoints ? "yes" : "no",
                       f.wideLines ? "yes" : "no", f.shaderInt16 ? "yes" : "no", f.shaderInt64 ? "yes" : "no",
                       p.limits.timestampComputeAndGraphics ? "yes" : "no", static_cast<double>(p.limits.timestampPeriod)));
    Say(report, Format("limits: image 2D %u, colour attachments %u, dual-source attachments %u, input attachments %u, framebuffer %ux%u, "
                       "push constants %u bytes, uniform buffer %u bytes, sampled images a stage %u, point size %.0f-%.0f",
                       p.limits.maxImageDimension2D, p.limits.maxColorAttachments, p.limits.maxFragmentDualSrcAttachments,
                       p.limits.maxPerStageDescriptorInputAttachments, p.limits.maxFramebufferWidth, p.limits.maxFramebufferHeight,
                       p.limits.maxPushConstantsSize, p.limits.maxUniformBufferRange, p.limits.maxPerStageDescriptorSampledImages,
                       static_cast<double>(p.limits.pointSizeRange[0]), static_cast<double>(p.limits.pointSizeRange[1])));
    // The extensions the renderer's design would use: reading the framebuffer's own pixel in order (the shader blend without
    // copies), dynamic rendering (no render pass objects), stencil export, load/store op none (no tile traffic for what isn't
    // read), unrestricted depth, provoking vertex, memory budget
    const char* interesting[] = {
        "VK_ARM_rasterization_order_attachment_access", "VK_EXT_rasterization_order_attachment_access", "VK_KHR_dynamic_rendering",
        "VK_EXT_shader_stencil_export", "VK_EXT_load_store_op_none", "VK_EXT_depth_range_unrestricted", "VK_EXT_provoking_vertex",
        "VK_EXT_fragment_shader_interlock", "VK_KHR_fragment_shader_barycentric", "VK_EXT_memory_budget", "VK_KHR_shader_float16_int8",
        "VK_EXT_shader_demote_to_helper_invocation", "VK_KHR_format_feature_flags2", "VK_EXT_custom_border_color", "VK_KHR_swapchain",
        "VK_EXT_image_view_min_lod", "VK_EXT_host_query_reset", "VK_KHR_timeline_semaphore", "VK_EXT_descriptor_indexing",
        "VK_EXT_shader_image_atomic_int64", "VK_KHR_shader_subgroup_extended_types",
    };
    std::string has;
    std::string lacks;
    for (const char* name : interesting)
    {
        std::string& to = HasExtension(gpu.extensions, name) ? has : lacks;
        to += std::string(to.empty() ? "" : " ") + (name + 3);
    }

    Say(report, Format("extensions (%zu): %s", gpu.extensions.size(), has.c_str()));
    Say(report, "lacking: " + (lacks.empty() ? std::string("none of the ones looked for") : lacks));
    // The formats the targets, the memory texture and the shadow sums would take
    Say(report, "formats:");
    Say(report, FormatReport(gpu, VK_FORMAT_R8G8B8A8_UNORM, "R8G8B8A8_UNORM (colour targets)",
                             VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT |
                                 VK_FORMAT_FEATURE_BLIT_DST_BIT));
    Say(report, FormatReport(gpu, VK_FORMAT_D32_SFLOAT_S8_UINT, "D32_SFLOAT_S8_UINT (depth targets)",
                             VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT));
    Say(report, FormatReport(gpu, VK_FORMAT_D32_SFLOAT, "D32_SFLOAT (depth copies)",
                             VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT));
    Say(report, FormatReport(gpu, VK_FORMAT_R32_UINT, "R32_UINT (local memory, the layout tables)",
                             VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT));
    Say(report, FormatReport(gpu, VK_FORMAT_R32_SFLOAT, "R32_SFLOAT (depth read back as colour)",
                             VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT));
    Say(report, FormatReport(gpu, VK_FORMAT_R16G16B16A16_SFLOAT, "R16G16B16A16_SFLOAT (the wrapping additive sums)",
                             VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT));
    Say(report, FormatReport(gpu, VK_FORMAT_R32G32B32A32_UINT, "R32G32B32A32_UINT (integer buffers read back)",
                             VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT));
    u64 deviceLocal = 0;
    for (u32 i = 0; i < gpu.memory.memoryHeapCount; i++)
    {
        if ((gpu.memory.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) != 0)
        {
            deviceLocal += gpu.memory.memoryHeaps[i].size;
        }
    }

    Say(report, Format("memory: %u heaps, %.0f MB device local; queue family %u", gpu.memory.memoryHeapCount,
                       static_cast<double>(deviceLocal) / (1024.0 * 1024.0), gpu.queueFamily));
    // The device made and destroyed: the driver takes the features asked for
    Device device;
    if (!MakeDevice(gpu, false, &device, &error))
    {
        Say(report, "device: " + error);
        DestroyInstance(&instance);
        return false;
    }

    Say(report, "device made with the features wanted");
    DestroyDevice(&device);
    DestroyInstance(&instance);
    return true;
}

bool ClearTest(SDL_Window* window, u32 frames, std::string* error)
{
    Instance instance;
    if (!MakeInstance(&instance, true, error))
    {
        return false;
    }

    // (the surface before the GPU's pick: the queue family has to present to it)
    Swapchain swapchain;
    if (!SDL_Vulkan_CreateSurface(window, instance.instance, nullptr, &swapchain.surface))
    {
        *error = Format("SDL_Vulkan_CreateSurface: %s", SDL_GetError());
        DestroyInstance(&instance);
        return false;
    }

    Gpu gpu;
    Device device;
    bool ok = PickGpu(instance, swapchain.surface, &gpu, error) && MakeDevice(gpu, true, &device, error);
    if (ok)
    {
        g_Vk.vkDestroySurfaceKHR(instance.instance, swapchain.surface, nullptr);
        swapchain.surface = VK_NULL_HANDLE;
        ok = MakeSwapchain(instance, gpu, device, window, &swapchain, error);
    }

    if (ok)
    {
        Native::Log("vulkan: swapchain %ux%u, %zu images, format %d", swapchain.extent.width, swapchain.extent.height,
                    swapchain.images.size(), static_cast<int>(swapchain.format));
        VkCommandPoolCreateInfo poolInfo = {};
        poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        poolInfo.queueFamilyIndex = device.queueFamily;
        VkCommandPool pool = VK_NULL_HANDLE;
        VkCommandBuffer commands = VK_NULL_HANDLE;
        VkSemaphore acquired = VK_NULL_HANDLE;
        VkSemaphore drawn = VK_NULL_HANDLE;
        VkFence done = VK_NULL_HANDLE;
        VkSemaphoreCreateInfo semaphoreInfo = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, nullptr, 0};
        VkFenceCreateInfo fenceInfo = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, nullptr, VK_FENCE_CREATE_SIGNALED_BIT};
        ok = g_Vk.vkCreateCommandPool(device.device, &poolInfo, nullptr, &pool) == VK_SUCCESS &&
             g_Vk.vkCreateSemaphore(device.device, &semaphoreInfo, nullptr, &acquired) == VK_SUCCESS &&
             g_Vk.vkCreateSemaphore(device.device, &semaphoreInfo, nullptr, &drawn) == VK_SUCCESS &&
             g_Vk.vkCreateFence(device.device, &fenceInfo, nullptr, &done) == VK_SUCCESS;
        if (ok)
        {
            VkCommandBufferAllocateInfo allocate = {};
            allocate.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
            allocate.commandPool = pool;
            allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            allocate.commandBufferCount = 1;
            ok = g_Vk.vkAllocateCommandBuffers(device.device, &allocate, &commands) == VK_SUCCESS;
        }

        // Each image's layout, undefined until its first frame
        std::vector<bool> presentedBefore(swapchain.images.size(), false);
        for (u32 frame = 0; ok && frame < frames; frame++)
        {
            g_Vk.vkWaitForFences(device.device, 1, &done, VK_TRUE, ~0ull);
            g_Vk.vkResetFences(device.device, 1, &done);
            u32 index = 0;
            VkResult result = g_Vk.vkAcquireNextImageKHR(device.device, swapchain.swapchain, ~0ull, acquired, VK_NULL_HANDLE, &index);
            if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR)
            {
                *error = Format("vkAcquireNextImageKHR: %s", ResultName(result));
                ok = false;
                break;
            }

            VkCommandBufferBeginInfo begin = {};
            begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
            begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            g_Vk.vkBeginCommandBuffer(commands, &begin);
            VkImage image = swapchain.images[index];
            Transition(commands, image, presentedBefore[index] ? VK_IMAGE_LAYOUT_PRESENT_SRC_KHR : VK_IMAGE_LAYOUT_UNDEFINED,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT);
            // Blue to orange over the frames
            const float t = frames > 1 ? static_cast<float>(frame) / static_cast<float>(frames - 1) : 1.0f;
            VkClearColorValue colour = {{t, 0.4f * t, 1.0f - t, 1.0f}};
            VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            g_Vk.vkCmdClearColorImage(commands, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &colour, 1, &range);
            Transition(commands, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_ACCESS_TRANSFER_WRITE_BIT, 0);
            g_Vk.vkEndCommandBuffer(commands);
            presentedBefore[index] = true;
            VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
            VkSubmitInfo submit = {};
            submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
            submit.waitSemaphoreCount = 1;
            submit.pWaitSemaphores = &acquired;
            submit.pWaitDstStageMask = &waitStage;
            submit.commandBufferCount = 1;
            submit.pCommandBuffers = &commands;
            submit.signalSemaphoreCount = 1;
            submit.pSignalSemaphores = &drawn;
            result = g_Vk.vkQueueSubmit(device.queue, 1, &submit, done);
            if (result != VK_SUCCESS)
            {
                *error = Format("vkQueueSubmit: %s", ResultName(result));
                ok = false;
                break;
            }

            VkPresentInfoKHR present = {};
            present.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
            present.waitSemaphoreCount = 1;
            present.pWaitSemaphores = &drawn;
            present.swapchainCount = 1;
            present.pSwapchains = &swapchain.swapchain;
            present.pImageIndices = &index;
            result = g_Vk.vkQueuePresentKHR(device.queue, &present);
            if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR)
            {
                *error = Format("vkQueuePresentKHR: %s", ResultName(result));
                ok = false;
                break;
            }
        }

        if (device.device != VK_NULL_HANDLE)
        {
            g_Vk.vkDeviceWaitIdle(device.device);
            if (done != VK_NULL_HANDLE)
            {
                g_Vk.vkDestroyFence(device.device, done, nullptr);
            }

            if (drawn != VK_NULL_HANDLE)
            {
                g_Vk.vkDestroySemaphore(device.device, drawn, nullptr);
            }

            if (acquired != VK_NULL_HANDLE)
            {
                g_Vk.vkDestroySemaphore(device.device, acquired, nullptr);
            }

            if (pool != VK_NULL_HANDLE)
            {
                g_Vk.vkDestroyCommandPool(device.device, pool, nullptr);
            }
        }

        if (ok)
        {
            Native::Log("vulkan: %u frames cleared and presented", frames);
        }
    }

    if (device.device != VK_NULL_HANDLE)
    {
        DestroySwapchain(instance, device, &swapchain);
        DestroyDevice(&device);
    }
    else if (swapchain.surface != VK_NULL_HANDLE)
    {
        g_Vk.vkDestroySurfaceKHR(instance.instance, swapchain.surface, nullptr);
    }

    DestroyInstance(&instance);
    return ok;
}
}
