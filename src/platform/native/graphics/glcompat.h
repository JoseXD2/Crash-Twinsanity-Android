#pragma once

// OpenGL 3.3 core (macOS, Linux, Windows) or OpenGL ES 3.0 (Android, and Linux with TWIN_GLES for testing): what the three GL
// users (the hardware renderer hw/, the presenter display.cpp, the UI's overlayrender.cpp) do differently on each.
//
// - The context's attributes (SetContextAttributes).
// - The shaders: written for GLSL 330 core, translated for GLSL ES 3.00 (Shader): the version, precision statements, no
//   noperspective (every vertex has w = 1, so the perspective-correct interpolation is linear in the window: the same), and
//   the dual-source blend's outputs by GL_EXT_blend_func_extended when the GPU has it, else plain variables (the renderer
//   then blends in the shader, as it does where GL's blend can't be exact).
// - The offscreen contexts (the render thread's, the hardware renderer's when there's no window): a hidden window's on the
//   desktop; Android has one window only, so a context sharing the window's, made current with no surface (SDL supports it
//   when the driver has GL_OES_surfaceless_context, else an EGL pbuffer of our own).
#include <SDL3/SDL.h>

#include <string>

namespace NativeGl
{
// Whether this run's GL is VkGl (vk/vkgl.h: OpenGL 3.3 core's meanings carried out with Vulkan): chosen before the window is made
// (UseVulkan), kept when VkGl starts on the window (StartVulkan), else the system's GL. Only with TWIN_VULKAN builds
bool Vulkan();
void UseVulkan(bool on);
// The window made for it (SDL_WINDOW_VULKAN, else SDL_WINDOW_OPENGL) and the device made on it; false: the system's GL from then
// (the window made again with SDL_WINDOW_OPENGL by the caller)
SDL_WindowFlags WindowFlag();
bool StartVulkan(SDL_Window* window);

// Whether the GL is OpenGL ES 3.0 (the build's, TWIN_GLES), not with VkGl (its GLSL is 330 core's)
#if defined(TWIN_GLES)
inline bool EsBuild = true;
#else
inline bool EsBuild = false;
#endif
inline bool IsEs()
{
    return EsBuild && !Vulkan();
}

// The SDL_GL_ calls the GL users make, VkGl's with it
SDL_FunctionPointer GetProcAddress(const char* name);
SDL_GLContext CreateContext(SDL_Window* window);
bool MakeCurrentOn(SDL_Window* window, SDL_GLContext context);
SDL_GLContext CurrentContext();
void DestroyGlContext(SDL_GLContext context);
void SwapWindow(SDL_Window* window);
void SetSwapInterval(int interval);

// The GL attributes for the next context (SDL_GL_SetAttribute): 3.3 core, or ES 3.0
void SetContextAttributes();

// A shader's source as the context's GLSL wants it (the desktop's: as given)
std::string Shader(const char* source);
std::string Shader(const std::string& source);

// A shader that didn't compile: its log and its source with the lines numbered, written where the platform's log goes
// (Android: logcat, synchronously, since the program aborts after)
void ReportShaderFailure(const char* what, const char* log, const std::string& source);

// Whether the current context has an extension (GL_... name), and the dual-source blend (GL 3.3: always; ES: the extension)
bool HasExtension(const char* name);
bool DualSourceBlend();
// ES: whether the fragment shader can read the framebuffer's pixel (GL_EXT_shader_framebuffer_fetch): the renderer's shader
// blend then reads the destination there, with no copy of the target before each batch (GL 3.3: never)
bool FramebufferFetch();
// Whether the hardware renderer's shader blend (and its destination alpha test) reads the framebuffer rather than a copy of the
// target: with the fetch and no dual-source blend (Mali's own driver), and on Adreno's own driver with both (its fetch reads the
// tile in place, where each copy of a target costs the tile's store and load: the Poco F5's gems); not on ANGLE (drew wrong)
bool ShaderBlendByFetch();

// An offscreen context: a hidden window's, or on Android the window's with no surface
struct Context
{
    SDL_Window* window = nullptr;
    SDL_GLContext context = nullptr;
    // Android: made current through EGL itself (SDL's MakeCurrent with no window releases the context instead), on no
    // surface (EGL_KHR_surfaceless_context) or a 1x1 pbuffer of our own
    bool egl = false;
    void* eglDisplay = nullptr;
    void* eglSurface = nullptr;
};

// Made sharing shareWith's objects when it isn't null (shareWith must be current on mainWindow then); on Android mainWindow
// is the app's window (there's no other). The new context is left not current
bool CreateOffscreenContext(SDL_Window* mainWindow, SDL_GLContext shareWith, Context* out);
bool MakeCurrent(const Context& context);
void ReleaseCurrent(const Context& context);
bool IsCurrent(const Context& context);
void DestroyContext(Context* context);
}
