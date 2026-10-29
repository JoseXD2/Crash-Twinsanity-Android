#pragma once

// Vulkan for the native graphics (native/VULKAN.md): the loader reached through SDL, an instance, the device and its queue,
// and a swapchain on the platform's window. This is the foundation the Vulkan backend of the GS (vkgs, to come) and the
// Vulkan presenter are built on; nothing here draws the game yet.
//
// Stage 1 (this file): a probe that reports what the phone's GPU offers for the renderer's design (the blend and attachment
// features, the extensions that let a fragment shader read its own pixel, the formats the targets need), and a swapchain test
// that clears and presents a few frames. Built with TWIN_VULKAN (native/CMakeLists.txt; Android's default); the functions are
// loaded at run time, so the program links no Vulkan library and runs where there is none.

#include "common.h"

#include <string>

struct SDL_Window;

namespace NativeVulkan
{
// Whether the loader could be opened and an instance made: the device's name and what it offers, written to the log as
// "vulkan: ..." lines and returned (several lines). False with the reason when Vulkan isn't usable here
bool Probe(std::string* report);

// The swapchain test: a surface on the window (made with SDL_WINDOW_VULKAN), a swapchain, and frames cleared to a colour that
// changes, presented. False with the reason on failure; the window is left as it was
bool ClearTest(SDL_Window* window, u32 frames, std::string* error);
}
