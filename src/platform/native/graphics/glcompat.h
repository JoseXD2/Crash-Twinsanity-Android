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
#if defined(TWIN_GLES)
constexpr bool Es = true;
#else
constexpr bool Es = false;
#endif

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
