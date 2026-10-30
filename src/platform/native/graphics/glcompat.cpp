// OpenGL 3.3 core or OpenGL ES 3.0: see glcompat.h
#include "glcompat.h"

#include "native.h"

#include <filesystem>

#if defined(TWIN_VULKAN)
#include "vk/vkgl.h"
#endif

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string_view>
#include <utility>

#if defined(__ANDROID__)
#include <EGL/egl.h>
#include <android/log.h>
#endif

namespace NativeGl
{
namespace
{
bool g_Vulkan = false;
}

bool Vulkan()
{
    return g_Vulkan;
}

void UseVulkan(bool on)
{
#if defined(TWIN_VULKAN)
    // (asked of the drivers first: an old one, Vulkan 1.1 say, keeps OpenGL with the window never made for Vulkan)
    g_Vulkan = on && VkGl::Supported();
#else
    (void)on;
#endif
}

SDL_WindowFlags WindowFlag()
{
    return g_Vulkan ? SDL_WINDOW_VULKAN : SDL_WINDOW_OPENGL;
}

bool StartVulkan(SDL_Window* window)
{
#if defined(TWIN_VULKAN)
    if (g_Vulkan && !VkGl::Start(window))
    {
        std::fprintf(stderr, "graphics: no Vulkan renderer (the log says why): OpenGL instead\n");
        g_Vulkan = false;
    }

    return g_Vulkan;
#else
    (void)window;
    return false;
#endif
}

#if defined(TWIN_VULKAN)
#define TWIN_VKGL(call, otherwise)                                                                                             \
    if (g_Vulkan)                                                                                                              \
    {                                                                                                                          \
        call;                                                                                                                  \
    }                                                                                                                          \
    else                                                                                                                       \
    {                                                                                                                          \
        otherwise;                                                                                                             \
    }
#else
#define TWIN_VKGL(call, otherwise) otherwise;
#endif

SDL_FunctionPointer GetProcAddress(const char* name)
{
    TWIN_VKGL(return reinterpret_cast<SDL_FunctionPointer>(VkGl::GetProcAddress(name)), return SDL_GL_GetProcAddress(name))
}

SDL_GLContext CreateContext(SDL_Window* window)
{
    TWIN_VKGL(return static_cast<SDL_GLContext>(VkGl::CreateContext(window)), return SDL_GL_CreateContext(window))
}

bool MakeCurrentOn(SDL_Window* window, SDL_GLContext context)
{
    TWIN_VKGL((void)window; return VkGl::MakeCurrent(context), return SDL_GL_MakeCurrent(window, context))
}

SDL_GLContext CurrentContext()
{
    TWIN_VKGL(return static_cast<SDL_GLContext>(VkGl::CurrentContext()), return SDL_GL_GetCurrentContext())
}

void DestroyGlContext(SDL_GLContext context)
{
    TWIN_VKGL(VkGl::DestroyContext(context), SDL_GL_DestroyContext(context))
}

void SwapWindow(SDL_Window* window)
{
#if defined(TWIN_VULKAN)
    // (Vulkan has shown 5 s of pictures: system.cpp's mark of a start in progress gone)
    static u32 shown = 0;
    if (g_Vulkan && ++shown == 300)
    {
        std::error_code ignored;
        std::filesystem::remove(Native::SettingsFolder() + "/vulkan_starting.txt", ignored);
    }
#endif
    TWIN_VKGL(VkGl::SwapWindow(window), SDL_GL_SwapWindow(window))
}

void SetSwapInterval(int interval)
{
    TWIN_VKGL(VkGl::SetSwapInterval(interval), SDL_GL_SetSwapInterval(interval))
}

void SetContextAttributes()
{
    if (Vulkan())
    {
        return;
    }

    if (EsBuild)
    {
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, 0);
    }
    else
    {
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG);
    }
}

namespace
{
constexpr unsigned GL_EXTENSIONS = 0x1F03;
constexpr unsigned GL_NUM_EXTENSIONS = 0x821D;

// (A new string each time, rather than erasing in place: the retail C library's memmove (src/platform/ps2/libc.cpp), which the
// program's std::string uses, copies a forward overlap as the game's memcpy does, and that tears a long erase)
void ReplaceAll(std::string& text, std::string_view from, std::string_view to)
{
    std::string result;
    result.reserve(text.size());
    size_t at = 0;
    for (size_t found = text.find(from); found != std::string::npos; found = text.find(from, at))
    {
        result.append(text, at, found - at);
        result.append(to);
        at = found + from.size();
    }

    result.append(text, at, std::string::npos);
    text = std::move(result);
}
}

namespace
{
void ReportLine(const std::string& line)
{
#if defined(__ANDROID__)
    __android_log_write(ANDROID_LOG_ERROR, "twinsanity", line.c_str());
#else
    std::fprintf(stderr, "%s\n", line.c_str());
#endif
}
}

void ReportShaderFailure(const char* what, const char* log, const std::string& source)
{
    ReportLine(std::string("graphics: ") + what + " didn't compile: " + (log != nullptr ? log : ""));
    int number = 1;
    size_t at = 0;
    while (at < source.size())
    {
        size_t end = source.find('\n', at);
        end = end == std::string::npos ? source.size() : end;
        ReportLine(std::to_string(number++) + ": " + source.substr(at, end - at));
        at = end + 1;
    }
}

bool HasExtension(const char* name)
{
#if defined(TWIN_VULKAN)
    if (g_Vulkan)
    {
        return VkGl::HasExtension(name);
    }
#endif
    auto getStringi = reinterpret_cast<const unsigned char* (*)(unsigned, unsigned)>(SDL_GL_GetProcAddress("glGetStringi"));
    auto getIntegerv = reinterpret_cast<void (*)(unsigned, int*)>(SDL_GL_GetProcAddress("glGetIntegerv"));
    if (getStringi != nullptr && getIntegerv != nullptr)
    {
        int count = 0;
        getIntegerv(GL_NUM_EXTENSIONS, &count);
        for (int i = 0; i < count; i++)
        {
            const unsigned char* extension = getStringi(GL_EXTENSIONS, static_cast<unsigned>(i));
            if (extension != nullptr && std::strcmp(reinterpret_cast<const char*>(extension), name) == 0)
            {
                return true;
            }
        }

        return false;
    }

    auto getString = reinterpret_cast<const unsigned char* (*)(unsigned)>(SDL_GL_GetProcAddress("glGetString"));
    if (getString == nullptr)
    {
        return false;
    }

    const unsigned char* all = getString(GL_EXTENSIONS);
    if (all == nullptr)
    {
        return false;
    }

    std::string_view list(reinterpret_cast<const char*>(all));
    size_t length = std::strlen(name);
    for (size_t at = list.find(name); at != std::string_view::npos; at = list.find(name, at + 1))
    {
        bool startsWord = at == 0 || list[at - 1] == ' ';
        bool endsWord = at + length == list.size() || list[at + length] == ' ';
        if (startsWord && endsWord)
        {
            return true;
        }
    }

    return false;
}

bool DualSourceBlend()
{
#if defined(TWIN_VULKAN)
    if (g_Vulkan)
    {
        return VkGl::HasExtension("GL_ARB_blend_func_extended");
    }
#endif
    if (!IsEs())
    {
        return true;
    }

    // Asked once (the renderer's start, a context current): the GPU's extensions are the same for every context
    static const bool answer = []
    {
        // ($TWIN_GLES_NO_DUAL=1: as a GPU without it, to test the shader blend's path)
        const char* forced = std::getenv("TWIN_GLES_NO_DUAL");
        return (forced == nullptr || forced[0] == '0') && HasExtension("GL_EXT_blend_func_extended");
    }();
    return answer;
}

bool ShaderBlendByFetch()
{
    // Asked once, a context current
    static const bool answer = []
    {
        if (!FramebufferFetch())
        {
            return false;
        }

        if (!DualSourceBlend())
        {
            return true;
        }

        // ($TWIN_HW_FETCH=1 or 0: as asked; fetch_off.txt in the settings folder: off, the copies as before)
        if (const char* forced = std::getenv("TWIN_HW_FETCH"))
        {
            return forced[0] == '1';
        }

        std::error_code missing;
        if (std::filesystem::exists(Native::SettingsFolder() + "/fetch_off.txt", missing))
        {
            return false;
        }

        auto getString = reinterpret_cast<const unsigned char* (*)(unsigned)>(GetProcAddress("glGetString"));
        const unsigned char* renderer = getString != nullptr ? getString(0x1F01) : nullptr;
        return renderer != nullptr && std::strstr(reinterpret_cast<const char*>(renderer), "Adreno") != nullptr;
    }();
    return answer;
}

bool FramebufferFetch()
{
    if (!IsEs())
    {
        return false;
    }

    static const bool answer = []
    {
        const char* forced = std::getenv("TWIN_GLES_NO_FETCH");
        return (forced == nullptr || forced[0] == '0') && HasExtension("GL_EXT_shader_framebuffer_fetch");
    }();
    return answer;
}

std::string Shader(const char* source)
{
    return Shader(std::string(source));
}

std::string Shader(const std::string& given)
{
    // (a debugging aid: every shader's source into $TWIN_DUMP_SHADERS, a file a source)
    if (const char* folder = std::getenv("TWIN_DUMP_SHADERS"))
    {
        static int number = 0;
        char path[512];
        std::snprintf(path, sizeof(path), "%s/shader%03d.glsl", folder, number++);
        if (FILE* file = std::fopen(path, "wb"))
        {
            std::fwrite(given.data(), 1, given.size(), file);
            std::fclose(file);
        }
    }

    if (!IsEs())
    {
        return given;
    }

    std::string text = given;
    bool dual = text.find("index = 1") != std::string::npos;
    std::string head = "#version 300 es\n";
    if (dual)
    {
        // (a shader blend that reads the framebuffer: its outputs as the fetch wants them, the dual-source blend there or not)
        const bool fetching = text.find("#define FBFETCH 1") != std::string::npos;
        if (DualSourceBlend() && !fetching)
        {
            head += "#extension GL_EXT_blend_func_extended : require\n";
        }
        else
        {
            // No dual-source blend: the factor output is a plain variable nothing reads (the renderer blends in the shader,
            // reading the destination from the framebuffer itself where the GPU lets a shader, else from a copy)
            ReplaceAll(text, "layout(location = 0, index = 1) out vec4 outFactor;", "vec4 outFactor;");
            if (text.find("#define FBFETCH 1") != std::string::npos)
            {
                head += "#extension GL_EXT_shader_framebuffer_fetch : require\n";
                ReplaceAll(text, "layout(location = 0, index = 0) out vec4 outColour;", "layout(location = 0) inout highp vec4 outColour;");
            }
            else
            {
                ReplaceAll(text, "layout(location = 0, index = 0) out vec4 outColour;", "layout(location = 0) out vec4 outColour;");
            }
        }
    }

    head += "precision highp float;\nprecision highp int;\nprecision highp sampler2D;\nprecision highp usampler2D;\n"
            "precision highp isampler2D;\n";
    size_t version = text.find("#version 330 core");
    if (version != std::string::npos)
    {
        text = text.substr(0, version) + head + text.substr(version + std::strlen("#version 330 core"));
    }
    else
    {
        text = head + text;
    }

    // Every vertex has w = 1: perspective-correct interpolation is linear in the window, as noperspective's
    ReplaceAll(text, "noperspective ", "");
    return text;
}

bool CreateOffscreenContext(SDL_Window* mainWindow, SDL_GLContext shareWith, Context* out)
{
    *out = Context{};
#if defined(TWIN_VULKAN)
    // VkGl's: a context with no window (every context shares every object), left not current
    if (g_Vulkan)
    {
        out->context = static_cast<SDL_GLContext>(VkGl::CreateContext(nullptr));
        return out->context != nullptr;
    }
#endif
    SetContextAttributes();
    if (shareWith != nullptr && mainWindow != nullptr)
    {
        SDL_GL_MakeCurrent(mainWindow, shareWith);
        SDL_GL_SetAttribute(SDL_GL_SHARE_WITH_CURRENT_CONTEXT, 1);
    }

#if defined(__ANDROID__)
    // One window only: the context is made on it, and made current without a surface (or with a pbuffer of our own)
    if (mainWindow == nullptr)
    {
        SDL_GL_SetAttribute(SDL_GL_SHARE_WITH_CURRENT_CONTEXT, 0);
        std::fprintf(stderr, "graphics: no window for an offscreen context\n");
        return false;
    }

    out->window = nullptr;
    out->context = SDL_GL_CreateContext(mainWindow);
    SDL_GL_SetAttribute(SDL_GL_SHARE_WITH_CURRENT_CONTEXT, 0);
    if (out->context == nullptr)
    {
        std::fprintf(stderr, "graphics: no offscreen context: %s\n", SDL_GetError());
        return false;
    }

    // SDL's MakeCurrent with no window releases the context (Android_GLES_MakeCurrent), so EGL itself makes it current: with
    // no surface when EGL allows it, else on a 1x1 pbuffer of the window's configuration
    {
        EGLDisplay display = eglGetCurrentDisplay();
        if (display == EGL_NO_DISPLAY)
        {
            display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
        }

        EGLContext context = static_cast<EGLContext>(out->context);
        out->egl = true;
        out->eglDisplay = display;
        out->eglSurface = nullptr;
        if (eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, context) == EGL_TRUE)
        {
            std::fprintf(stderr, "graphics: the offscreen context is current with no surface\n");
        }
        else
        {
            EGLint configId = 0;
            eglQueryContext(display, context, EGL_CONFIG_ID, &configId);
            EGLint attributes[] = {EGL_CONFIG_ID, configId, EGL_NONE};
            EGLConfig config = nullptr;
            EGLint count = 0;
            eglChooseConfig(display, attributes, &config, 1, &count);
            EGLint pbufferAttributes[] = {EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE};
            EGLSurface surface = count > 0 ? eglCreatePbufferSurface(display, config, pbufferAttributes) : EGL_NO_SURFACE;
            if (surface == EGL_NO_SURFACE || eglMakeCurrent(display, surface, surface, context) != EGL_TRUE)
            {
                std::fprintf(stderr, "graphics: the offscreen context can't be made current (no surfaceless context, no pbuffer: EGL error %#x)\n",
                             eglGetError());
                SDL_GL_DestroyContext(out->context);
                out->context = nullptr;
                return false;
            }

            out->eglSurface = surface;
            std::fprintf(stderr, "graphics: the offscreen context on a pbuffer\n");
        }

        // Let go here: the render thread takes it
        eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    }

    // The window's context current again, as before
    if (shareWith != nullptr)
    {
        SDL_GL_MakeCurrent(mainWindow, shareWith);
    }

    return true;
#else
    out->window = SDL_CreateWindow("graphics", 64, 64, SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
    if (out->window != nullptr)
    {
        out->context = SDL_GL_CreateContext(out->window);
    }

    SDL_GL_SetAttribute(SDL_GL_SHARE_WITH_CURRENT_CONTEXT, 0);
    if (out->context == nullptr)
    {
        std::fprintf(stderr, "graphics: no offscreen context: %s\n", SDL_GetError());
        if (out->window != nullptr)
        {
            SDL_DestroyWindow(out->window);
            out->window = nullptr;
        }

        return false;
    }

    // Making it made it current here
    if (shareWith != nullptr && mainWindow != nullptr)
    {
        SDL_GL_MakeCurrent(mainWindow, shareWith);
    }
    else
    {
        SDL_GL_MakeCurrent(out->window, nullptr);
    }

    return true;
#endif
}

bool MakeCurrent(const Context& context)
{
#if defined(TWIN_VULKAN)
    if (g_Vulkan)
    {
        return VkGl::MakeCurrent(context.context);
    }
#endif
#if defined(__ANDROID__)
    if (context.egl)
    {
        EGLSurface surface = context.eglSurface != nullptr ? static_cast<EGLSurface>(context.eglSurface) : EGL_NO_SURFACE;
        return eglMakeCurrent(static_cast<EGLDisplay>(context.eglDisplay), surface, surface, static_cast<EGLContext>(context.context)) == EGL_TRUE;
    }
#endif
    return SDL_GL_MakeCurrent(context.window, context.context);
}

void ReleaseCurrent(const Context& context)
{
#if defined(TWIN_VULKAN)
    if (g_Vulkan)
    {
        if (VkGl::CurrentContext() == context.context)
        {
            VkGl::MakeCurrent(nullptr);
        }

        return;
    }
#endif
#if defined(__ANDROID__)
    if (context.egl)
    {
        eglMakeCurrent(static_cast<EGLDisplay>(context.eglDisplay), EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        return;
    }
#endif
    SDL_GL_MakeCurrent(context.window, nullptr);
}

bool IsCurrent(const Context& context)
{
#if defined(TWIN_VULKAN)
    if (g_Vulkan)
    {
        return VkGl::CurrentContext() == context.context;
    }
#endif
#if defined(__ANDROID__)
    if (context.egl)
    {
        return eglGetCurrentContext() == static_cast<EGLContext>(context.context);
    }
#endif
    return SDL_GL_GetCurrentContext() == context.context;
}

void DestroyContext(Context* context)
{
#if defined(__ANDROID__)
    if (context->eglSurface != nullptr)
    {
        eglDestroySurface(static_cast<EGLDisplay>(context->eglDisplay), static_cast<EGLSurface>(context->eglSurface));
        context->eglSurface = nullptr;
    }
#endif
    if (context->context != nullptr)
    {
        DestroyGlContext(context->context);
        context->context = nullptr;
    }

    if (context->window != nullptr)
    {
        SDL_DestroyWindow(context->window);
        context->window = nullptr;
    }
}
}
