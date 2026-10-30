#pragma once

// OpenGL in Vulkan (native/VULKAN.md): the OpenGL the native graphics use (the hardware renderer hw/, the presenter display.cpp, the
// UI's overlay, the launcher), carried out with Vulkan in place of the system's GL (on Android, ANGLE's or the GPU's). Not a
// general OpenGL: the entry points those files load (hw/gl.h's table, display.cpp's, overlayrender.cpp's) with the meanings they use,
// GLSL 330 core compiled to SPIR-V with glslang (relaxed Vulkan rules: the loose uniforms in a block of their own). What it gives over
// a system GL is the control of what a GPU of tiles does between passes, and no driver's checks of every call.
//
// Turned on for a run (NativeGl::Vulkan()): the window is made for Vulkan (SDL_WINDOW_VULKAN), the GL users' entry points come from
// GetProcAddress, their contexts from CreateContext. Every context shares every object (as the users' contexts do); each has its own
// state and its own commands, submitted to the one queue in turn.

#include <SDL3/SDL.h>

namespace VkGl
{
// The device made: on the window (its surface the default framebuffers' swapchain) or with none (offscreen: the harness's). False
// with the reason logged when Vulkan 1.3 with what's needed isn't there
bool Start(SDL_Window* window);
// Whether a device here has what VkGl needs (Vulkan 1.3, dynamic rendering), asked before the window is made for it (an instance
// of its own, gone again); why not to the log
bool Supported();
bool Started();
void Stop();

// A GL entry point by its name ("glDrawArrays"), null when it's not one of these
void* GetProcAddress(const char* name);

// A context: its default framebuffer the window's (presented by SwapWindow), or none (window null: offscreen)
void* CreateContext(SDL_Window* window);
// Current on the calling thread (null: none)
bool MakeCurrent(void* context);
void* CurrentContext();
void DestroyContext(void* context);
// The current context's default framebuffer shown in its window, and the waits for the display (0 none, 1 v-sync)
void SwapWindow(SDL_Window* window);
void SetSwapInterval(int interval);

// The extensions it answers for (GL_... names), as glGetString(GL_EXTENSIONS) lists them
bool HasExtension(const char* name);
}
