#include "gl.h"
#include "../glcompat.h"

#include <SDL3/SDL.h>

#include <cstdio>
#include <cstdlib>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace Hw
{
namespace
{
Gl g_Gl;
bool g_Loaded = false;
bool g_Anisotropy = true;

// The hardware renderer asks for its passes' uniforms by name every time it uses them (thousands a frame), and the driver looks
// each name up again: the answers kept, a thread's (each has its own context) by program and name. The renderer's programs live
// as long as it does, so an answer never goes stale
GLint (*g_GetUniformLocation)(GLuint, const GLchar*) = nullptr;

struct NameHash
{
    using is_transparent = void;
    size_t operator()(std::string_view name) const
    {
        return std::hash<std::string_view>()(name);
    }
};

GLint CachedUniformLocation(GLuint program, const GLchar* name)
{
    thread_local std::unordered_map<GLuint, std::unordered_map<std::string, GLint, NameHash, std::equal_to<>>> cache;
    auto& names = cache[program];
    auto found = names.find(std::string_view(name));
    if (found != names.end())
    {
        return found->second;
    }

    GLint location = g_GetUniformLocation(program, name);
    names.emplace(name, location);
    return location;
}

template <typename T>
bool Load(T& function, const char* name)
{
    function = reinterpret_cast<T>(NativeGl::GetProcAddress(name));
    if (function == nullptr)
    {
        std::fprintf(stderr, "graphics: OpenGL has no %s\n", name);
        return false;
    }

    return true;
}
}

bool LoadGl()
{
    if (g_Loaded)
    {
        return true;
    }

    bool ok = true;
    ok &= Load(g_Gl.GenTextures, "glGenTextures");
    ok &= Load(g_Gl.DeleteTextures, "glDeleteTextures");
    ok &= Load(g_Gl.BindTexture, "glBindTexture");
    ok &= Load(g_Gl.TexImage2D, "glTexImage2D");
    ok &= Load(g_Gl.TexSubImage2D, "glTexSubImage2D");
    ok &= Load(g_Gl.TexParameteri, "glTexParameteri");
    ok &= Load(g_Gl.ActiveTexture, "glActiveTexture");
    ok &= Load(g_Gl.PixelStorei, "glPixelStorei");
    ok &= Load(g_Gl.GenFramebuffers, "glGenFramebuffers");
    ok &= Load(g_Gl.DeleteFramebuffers, "glDeleteFramebuffers");
    ok &= Load(g_Gl.BindFramebuffer, "glBindFramebuffer");
    ok &= Load(g_Gl.FramebufferTexture2D, "glFramebufferTexture2D");
    ok &= Load(g_Gl.CheckFramebufferStatus, "glCheckFramebufferStatus");
    ok &= Load(g_Gl.BlitFramebuffer, "glBlitFramebuffer");
    ok &= Load(g_Gl.ReadPixels, "glReadPixels");
    ok &= Load(g_Gl.ReadBuffer, "glReadBuffer");
    if (NativeGl::IsEs())
    {
        g_Gl.DrawBuffer = nullptr;
        g_Gl.BindFragDataLocationIndexed = nullptr;
        g_Gl.PointSize = nullptr;
    }
    else
    {
        ok &= Load(g_Gl.DrawBuffer, "glDrawBuffer");
        ok &= Load(g_Gl.BindFragDataLocationIndexed, "glBindFragDataLocationIndexed");
        ok &= Load(g_Gl.PointSize, "glPointSize");
    }
    ok &= Load(g_Gl.DrawBuffers, "glDrawBuffers");
    ok &= Load(g_Gl.GetIntegerv, "glGetIntegerv");
    ok &= Load(g_Gl.GenSamplers, "glGenSamplers");
    ok &= Load(g_Gl.BindSampler, "glBindSampler");
    ok &= Load(g_Gl.SamplerParameteri, "glSamplerParameteri");
    ok &= Load(g_Gl.SamplerParameterf, "glSamplerParameterf");
    ok &= Load(g_Gl.Viewport, "glViewport");
    ok &= Load(g_Gl.Scissor, "glScissor");
    ok &= Load(g_Gl.Enable, "glEnable");
    ok &= Load(g_Gl.Disable, "glDisable");
    ok &= Load(g_Gl.BlendFuncSeparate, "glBlendFuncSeparate");
    ok &= Load(g_Gl.BlendEquationSeparate, "glBlendEquationSeparate");
    ok &= Load(g_Gl.BlendColor, "glBlendColor");
    ok &= Load(g_Gl.ColorMask, "glColorMask");
    ok &= Load(g_Gl.DepthMask, "glDepthMask");
    ok &= Load(g_Gl.DepthFunc, "glDepthFunc");
    ok &= Load(g_Gl.Clear, "glClear");
    ok &= Load(g_Gl.ClearColor, "glClearColor");
    ok &= Load(g_Gl.CreateShader, "glCreateShader");
    ok &= Load(g_Gl.ShaderSource, "glShaderSource");
    ok &= Load(g_Gl.CompileShader, "glCompileShader");
    ok &= Load(g_Gl.GetShaderiv, "glGetShaderiv");
    ok &= Load(g_Gl.GetShaderInfoLog, "glGetShaderInfoLog");
    ok &= Load(g_Gl.DeleteShader, "glDeleteShader");
    ok &= Load(g_Gl.CreateProgram, "glCreateProgram");
    ok &= Load(g_Gl.AttachShader, "glAttachShader");
    ok &= Load(g_Gl.BindAttribLocation, "glBindAttribLocation");
    ok &= Load(g_Gl.LinkProgram, "glLinkProgram");
    ok &= Load(g_Gl.GetProgramiv, "glGetProgramiv");
    ok &= Load(g_Gl.GetProgramInfoLog, "glGetProgramInfoLog");
    ok &= Load(g_Gl.UseProgram, "glUseProgram");
    ok &= Load(g_Gl.GetUniformLocation, "glGetUniformLocation");
    ok &= Load(g_Gl.Uniform1i, "glUniform1i");
    ok &= Load(g_Gl.Uniform1f, "glUniform1f");
    ok &= Load(g_Gl.Uniform2f, "glUniform2f");
    ok &= Load(g_Gl.Uniform3f, "glUniform3f");
    ok &= Load(g_Gl.Uniform4f, "glUniform4f");
    ok &= Load(g_Gl.Uniform1iv, "glUniform1iv");
    ok &= Load(g_Gl.GenVertexArrays, "glGenVertexArrays");
    ok &= Load(g_Gl.BindVertexArray, "glBindVertexArray");
    ok &= Load(g_Gl.GenBuffers, "glGenBuffers");
    ok &= Load(g_Gl.BindBuffer, "glBindBuffer");
    ok &= Load(g_Gl.BufferData, "glBufferData");
    ok &= Load(g_Gl.VertexAttribPointer, "glVertexAttribPointer");
    ok &= Load(g_Gl.VertexAttribIPointer, "glVertexAttribIPointer");
    ok &= Load(g_Gl.EnableVertexAttribArray, "glEnableVertexAttribArray");
    ok &= Load(g_Gl.DrawArrays, "glDrawArrays");
    ok &= Load(g_Gl.Finish, "glFinish");
    ok &= Load(g_Gl.GetError, "glGetError");
    ok &= Load(g_Gl.LineWidth, "glLineWidth");
    ok &= Load(g_Gl.Uniform4ui, "glUniform4ui");
    ok &= Load(g_Gl.Uniform1ui, "glUniform1ui");
    ok &= Load(g_Gl.StencilFunc, "glStencilFunc");
    ok &= Load(g_Gl.TexParameterf, "glTexParameterf");
    ok &= Load(g_Gl.GenerateMipmap, "glGenerateMipmap");
    ok &= Load(g_Gl.StencilOp, "glStencilOp");
    ok &= Load(g_Gl.StencilMask, "glStencilMask");
    ok &= Load(g_Gl.ClearStencil, "glClearStencil");
    ok &= Load(g_Gl.Flush, "glFlush");
    ok &= Load(g_Gl.FenceSync, "glFenceSync");
    ok &= Load(g_Gl.WaitSync, "glWaitSync");
    ok &= Load(g_Gl.DeleteSync, "glDeleteSync");
    // (optional: the vertex ring's mapping, the renderer's name)
    g_Gl.MapBufferRange = reinterpret_cast<void* (*)(GLenum, std::ptrdiff_t, GLsizeiptr, GLbitfield)>(NativeGl::GetProcAddress("glMapBufferRange"));
    g_Gl.BufferSubData = reinterpret_cast<void (*)(GLenum, std::ptrdiff_t, GLsizeiptr, const void*)>(NativeGl::GetProcAddress("glBufferSubData"));
    g_Gl.UnmapBuffer = reinterpret_cast<GLboolean (*)(GLenum)>(NativeGl::GetProcAddress("glUnmapBuffer"));
    g_Gl.TexImage3D = reinterpret_cast<void (*)(GLenum, GLint, GLint, GLsizei, GLsizei, GLsizei, GLint, GLenum, GLenum, const void*)>(
        NativeGl::GetProcAddress("glTexImage3D"));
    g_Gl.FramebufferTextureLayer =
        reinterpret_cast<void (*)(GLenum, GLenum, GLuint, GLint, GLint)>(NativeGl::GetProcAddress("glFramebufferTextureLayer"));
    g_Gl.GetString = reinterpret_cast<const unsigned char* (*)(GLenum)>(NativeGl::GetProcAddress("glGetString"));
    // (optional: the timer queries, the EXT's entry points on ES)
    g_Gl.GenQueries = nullptr;
    if (!NativeGl::IsEs() || NativeGl::HasExtension("GL_EXT_disjoint_timer_query"))
    {
        const char* suffix = NativeGl::IsEs() ? "EXT" : "";
        g_Gl.GenQueries = reinterpret_cast<void (*)(GLsizei, GLuint*)>(NativeGl::GetProcAddress((std::string("glGenQueries") + suffix).c_str()));
        g_Gl.BeginQuery = reinterpret_cast<void (*)(GLenum, GLuint)>(NativeGl::GetProcAddress((std::string("glBeginQuery") + suffix).c_str()));
        g_Gl.EndQuery = reinterpret_cast<void (*)(GLenum)>(NativeGl::GetProcAddress((std::string("glEndQuery") + suffix).c_str()));
        g_Gl.GetQueryObjectuiv = reinterpret_cast<void (*)(GLuint, GLenum, GLuint*)>(NativeGl::GetProcAddress((std::string("glGetQueryObjectuiv") + suffix).c_str()));
        g_Gl.GetQueryObjectui64v = reinterpret_cast<void (*)(GLuint, GLenum, GLuint64*)>(NativeGl::GetProcAddress((std::string("glGetQueryObjectui64v") + suffix).c_str()));
        if (g_Gl.BeginQuery == nullptr || g_Gl.EndQuery == nullptr || g_Gl.GetQueryObjectuiv == nullptr || g_Gl.GetQueryObjectui64v == nullptr)
        {
            g_Gl.GenQueries = nullptr;
        }
    }

    const char* caching = std::getenv("TWIN_HW_UNIFORM_CACHE");
    if (ok && (caching == nullptr || caching[0] != '0'))
    {
        g_GetUniformLocation = g_Gl.GetUniformLocation;
        g_Gl.GetUniformLocation = CachedUniformLocation;
    }

    g_Loaded = ok;
    if (ok)
    {
        g_Anisotropy = !NativeGl::IsEs() || NativeGl::HasExtension("GL_EXT_texture_filter_anisotropic");
    }

    return ok;
}

void DrawNone(Gl& gl)
{
    if (gl.DrawBuffer != nullptr)
    {
        gl.DrawBuffer(GL_NONE);
    }
    else
    {
        const GLenum none = GL_NONE;
        gl.DrawBuffers(1, &none);
    }
}

bool IsEs()
{
    return NativeGl::IsEs();
}

bool HasAnisotropy()
{
    return g_Anisotropy;
}

Gl& GetGl()
{
    return g_Gl;
}
}
