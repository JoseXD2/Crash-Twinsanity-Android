// The automatic settings (nativeui.h's AutoConfigure): the device's GPU (by its name: the Vulkan probe's, the SoC's on Android
// when there's no Vulkan), its memory and its fastest core put it in a tier, and the tier gives the render scale, the renderer and
// the costly filters. Measured on a Poco X7 Pro (Mali-G720 MC7, the high tier): 3x holds 60 in most scenes with Vulkan; on an
// Exynos 1330 (Mali-G68 MP2, the low tier) 2x didn't, so 1x. The player's own choices afterwards stay theirs: it runs once by
// itself (a phone's first start) and again only from the Display page's button
#include "ui/nativeui.h"

#include "native.h"

#if defined(TWIN_VULKAN)
#include "graphics/vk/vkdevice.h"
#endif

#include <SDL3/SDL.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

#if defined(__ANDROID__)
#include <sys/system_properties.h>
#endif

namespace NativeUi
{
namespace
{
enum Tier : s32
{
    TierLow,
    TierMid,
    TierHigh,
};

const char* TierName(s32 tier)
{
    return tier == TierHigh ? "high" : tier == TierMid ? "mid" : "low";
}

std::string Lower(std::string text)
{
    for (char& c : text)
    {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }

    return text;
}

// The number after a prefix ("mali-g720 mc7" after "mali-g": 720), -1 when there's none
s32 NumberAfter(const std::string& text, const char* prefix)
{
    size_t at = text.find(prefix);
    if (at == std::string::npos)
    {
        return -1;
    }

    at += std::char_traits<char>::length(prefix);
    while (at < text.size() && text[at] == ' ')
    {
        at++;
    }

    if (at >= text.size() || !std::isdigit(static_cast<unsigned char>(text[at])))
    {
        return -1;
    }

    return std::atoi(text.c_str() + at);
}

// The GPU's name: the Vulkan device's ("Mali-G720 MC7"), else empty
std::string GpuName(bool* vulkan)
{
    *vulkan = false;
#if defined(TWIN_VULKAN)
    std::string report;
    if (NativeVulkan::Probe(&report))
    {
        *vulkan = true;
        size_t at = report.find("device: ");
        if (at != std::string::npos)
        {
            at += 8;
            size_t end = report.find(" (vendor", at);
            return report.substr(at, end == std::string::npos ? std::string::npos : end - at);
        }
    }
#endif
    return "";
}

std::string SocName()
{
#if defined(__ANDROID__)
    char value[PROP_VALUE_MAX] = {};
    for (const char* property : {"ro.soc.model", "ro.hardware.chipname", "ro.board.platform"})
    {
        if (__system_property_get(property, value) > 0)
        {
            return value;
        }
    }
#endif
    return "";
}

// The fastest core's top clock in MHz (Linux's cpufreq), 0 when it can't be read
s32 FastestCoreMhz()
{
    s32 best = 0;
    for (s32 cpu = 0; cpu < 16; cpu++)
    {
        std::ifstream file("/sys/devices/system/cpu/cpu" + std::to_string(cpu) + "/cpufreq/cpuinfo_max_freq");
        long khz = 0;
        if (file >> khz)
        {
            best = std::max(best, static_cast<s32>(khz / 1000));
        }
    }

    return best;
}

// The GPU's tier by its name; how sure (false: an unknown GPU, the tier a guess)
s32 GpuTier(const std::string& gpu, bool* known)
{
    *known = true;
    s32 mali = NumberAfter(gpu, "mali-g");
    if (gpu.find("immortalis") != std::string::npos)
    {
        return TierHigh;
    }

    if (mali >= 0)
    {
        // (the cores: "mc7"; a 700 with few is a mid-range part)
        s32 cores = NumberAfter(gpu, " mc");
        cores = cores < 0 ? NumberAfter(gpu, " mp") : cores;
        if (mali >= 700)
        {
            return cores > 0 && cores < 5 ? TierMid : TierHigh;
        }

        if (mali >= 600 || mali == 76 || mali == 77 || mali == 78)
        {
            return TierMid;
        }

        // G57, G68, G52, G51, G31...
        return TierLow;
    }

    s32 adreno = NumberAfter(gpu, "adreno (tm) ");
    adreno = adreno < 0 ? NumberAfter(gpu, "adreno ") : adreno;
    if (adreno >= 0)
    {
        return adreno >= 730 ? TierHigh : adreno >= 640 ? TierMid : TierLow;
    }

    if (gpu.find("xclipse") != std::string::npos)
    {
        return TierMid;
    }

    if (gpu.find("powervr") != std::string::npos)
    {
        return TierLow;
    }

    *known = false;
    return TierMid;
}

// Whether VkGl is the better renderer there: Mali (ANGLE's GL on Vulkan, or Mali's own GL, both slower: native/VULKAN.md) and
// Samsung's Xclipse; Adreno's own GL driver is good, and an unknown GPU keeps the system's GL
bool VulkanBetter(const std::string& gpu)
{
    return gpu.find("mali") != std::string::npos || gpu.find("immortalis") != std::string::npos ||
           gpu.find("xclipse") != std::string::npos;
}
}

void AutoConfigure(bool live)
{
    bool vulkan = false;
    const std::string gpuName = GpuName(&vulkan);
    const std::string gpu = Lower(gpuName);
    const std::string soc = SocName();
    const s32 memory = SDL_GetSystemRAM();
    const s32 cores = SDL_GetNumLogicalCPUCores();
    const s32 mhz = FastestCoreMhz();
    bool known = false;
    s32 tier = GpuTier(gpu, &known);
    std::string why = known ? "the GPU" : "an unknown GPU";
    // Little memory or slow cores: a tier down (the game's three threads want three fast cores)
    if (memory > 0 && memory < 4000)
    {
        tier = std::max<s32>(TierLow, tier - 1);
        why += ", under 4 GB of memory";
    }

    if ((mhz > 0 && mhz < 2000) || (cores > 0 && cores < 6))
    {
        tier = std::max<s32>(TierLow, tier - 1);
        why += ", slow or few cores";
    }

    const s32 scale = tier == TierHigh ? 3 : tier == TierMid ? 2 : 1;
    const bool useVulkan = vulkan && VulkanBetter(gpu);
    Native::Log("auto settings: %s (SoC %s, %d MB, %d cores, fastest %d MHz): the %s tier (%s): %dx, %s",
                gpuName.empty() ? "no GPU name" : gpuName.c_str(), soc.empty() ? "unknown" : soc.c_str(), memory, cores, mhz,
                TierName(tier), why.c_str(), scale, useVulkan ? "Vulkan" : "OpenGL");

    // The options: the renderer, 60 Hz, the filters that cost a pass (FXAA, the CRT's) off below the high tier
    auto set = [live](Option option, s32 value) { live ? SetOption(option, value) : PresetOption(option, value); };
    set(OptionRenderer, useVulkan ? 1 : 0);
    set(OptionRefresh60, 1);
    set(OptionTextureUpscale, 0);
    if (tier != TierHigh)
    {
        set(OptionAntiAliasing, 0);
        set(OptionCrtFilter, 0);
    }

    Resolution resolution = live ? CurrentResolution() : DefaultResolution();
    resolution.scale = std::clamp(scale, 1, ScaleChoicesHere);
    if (live)
    {
        SetResolution(resolution);
    }
    else
    {
        PresetResolution(resolution);
    }
}

void AutoConfigureFirstStart()
{
    // ($TWIN_AUTOCONFIG=1: as a first start anywhere, to test it; 0: never)
    const char* forced = std::getenv("TWIN_AUTOCONFIG");
    if (forced != nullptr && forced[0] == '0')
    {
        return;
    }

    const bool force = forced != nullptr && forced[0] == '1';
    std::error_code error;
    const bool resolutionSaved = std::filesystem::exists(Native::SettingsFolder() + "/resolution.txt", error);
    if (!force && (!Native::IsMobile() || OptionsSaved() || resolutionSaved))
    {
        return;
    }

    AutoConfigure(false);
}
}
