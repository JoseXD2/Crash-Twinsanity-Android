// VkGl's GL entry points: each GL call the users make, on the current context's state (vkglinternal.h's Context) and the shared
// objects; the draws, clears, uploads, reads and blits handed to vkgldraw.cpp, the programs to vkglprogram.cpp. GetProcAddress gives
// them by name. Errors aren't kept (GetError is always NO_ERROR): the users check what they make by its status, not by GL's errors
#include "vkglinternal.h"
#include "vkgl.h"

#include <algorithm>
#include <cstdio>

namespace VkGl
{
namespace
{
#define VKGL_CONTEXT(...)                                                                                                      \
    Context* c = Current();                                                                                                    \
    if (c == nullptr)                                                                                                          \
    {                                                                                                                          \
        return __VA_ARGS__;                                                                                                    \
    }

// A new name in a table (every kind of object takes from the one counter)
template <typename T> GLuint MakeObject(std::unordered_map<GLuint, std::unique_ptr<T>>& map)
{
    Objects& o = GetObjects();
    std::lock_guard lock(o.lock);
    const GLuint name = o.next++;
    auto made = std::make_unique<T>();
    made->name = name;
    map[name] = std::move(made);
    return name;
}

template <typename T> void MakeObjects(std::unordered_map<GLuint, std::unique_ptr<T>>& map, GLsizei n, GLuint* names)
{
    for (GLsizei i = 0; i < n; i++)
    {
        names[i] = MakeObject(map);
    }
}

template <typename T> std::unique_ptr<T> TakeObject(std::unordered_map<GLuint, std::unique_ptr<T>>& map, GLuint name)
{
    Objects& o = GetObjects();
    std::lock_guard lock(o.lock);
    auto found = map.find(name);
    if (found == map.end())
    {
        return nullptr;
    }

    std::unique_ptr<T> taken = std::move(found->second);
    map.erase(found);
    return taken;
}

Texture* BoundTexture(Context& c)
{
    return FindTexture(c.textures[c.activeUnit]);
}

VertexArray* BoundVertexArray(Context& c)
{
    if (c.vertexArray == 0)
    {
        if (c.defaultVertexArray == 0)
        {
            c.defaultVertexArray = MakeObject(GetObjects().vertexArrays);
        }

        c.vertexArray = c.defaultVertexArray;
    }

    return FindVertexArray(c.vertexArray);
}

GLuint* BufferBinding(Context& c, GLenum target)
{
    switch (target)
    {
    case Gl::ARRAY_BUFFER:
        return &c.arrayBuffer;
    case Gl::PIXEL_UNPACK_BUFFER:
        return &c.unpackBuffer;
    case Gl::PIXEL_PACK_BUFFER:
        return &c.packBuffer;
    default:
        return nullptr;
    }
}

Framebuffer* BoundFramebuffer(Context& c, GLenum target)
{
    const GLuint name = target == Gl::READ_FRAMEBUFFER ? c.readFramebuffer : c.drawFramebuffer;
    return name != 0 ? FindFramebuffer(name) : nullptr;
}

// An unsized internal format as the sized one it means with its type
GLenum SizedFormat(GLint internal, GLenum format, GLenum type)
{
    switch (static_cast<GLenum>(internal))
    {
    case Gl::RGBA:
        return type == Gl::FLOAT ? Gl::RGBA32F : type == Gl::HALF_FLOAT ? Gl::RGBA16F : Gl::RGBA8;
    case Gl::RED:
        return type == Gl::FLOAT ? Gl::R32F : type == Gl::HALF_FLOAT ? Gl::R16F : Gl::R8;
    case Gl::RG:
        return type == Gl::FLOAT ? Gl::RG32F : type == Gl::HALF_FLOAT ? Gl::RG16F : Gl::RG8;
    case Gl::DEPTH_STENCIL:
        return Gl::DEPTH32F_STENCIL8;
    case Gl::DEPTH_COMPONENT:
        return Gl::DEPTH_COMPONENT32F;
    default:
        (void)format;
        return static_cast<GLenum>(internal);
    }
}

std::vector<std::string> Extensions()
{
    std::vector<std::string> list = {"GL_ARB_timer_query", "GL_ARB_sampler_objects", "GL_ARB_explicit_attrib_location"};
    if (Started())
    {
        Device& d = GetDevice();
        if (d.anisotropy)
        {
            list.push_back("GL_EXT_texture_filter_anisotropic");
            list.push_back("GL_ARB_texture_filter_anisotropic");
        }

        if (d.dualSource)
        {
            list.push_back("GL_ARB_blend_func_extended");
        }
    }

    return list;
}

// ------------------------------------------------------------------------------------------------------------------ textures
void GenTextures(GLsizei n, GLuint* names)
{
    MakeObjects(GetObjects().textures, n, names);
}

void DeleteTextures(GLsizei n, const GLuint* names)
{
    Context* c = Current();
    for (GLsizei i = 0; i < n; i++)
    {
        if (names[i] == 0)
        {
            continue;
        }

        if (c != nullptr)
        {
            for (GLuint& bound : c->textures)
            {
                if (bound == names[i])
                {
                    bound = 0;
                }
            }

            // (a rendering instance into it ended before its image goes)
            if (c->rendering.active)
            {
                EndRendering(*c);
            }
        }

        std::unique_ptr<Texture> texture = TakeObject(GetObjects().textures, names[i]);
        if (texture != nullptr)
        {
            ForgetImage(*texture);
        }
    }
}

void BindTexture(GLenum, GLuint name)
{
    VKGL_CONTEXT()
    if (name != 0 && FindTexture(name) == nullptr)
    {
        // (a name GL never gave: made by its binding, as GL's compatibility does)
        Objects& o = GetObjects();
        std::lock_guard lock(o.lock);
        auto made = std::make_unique<Texture>();
        made->name = name;
        o.textures[name] = std::move(made);
        o.next = std::max(o.next, name + 1);
    }

    c->textures[c->activeUnit] = name;
}

void ActiveTexture(GLenum unit)
{
    VKGL_CONTEXT()
    const GLenum index = unit - Gl::TEXTURE0;
    c->activeUnit = index < 32 ? index : 0;
}

void TexImage2D(GLenum, GLint level, GLint internal, GLsizei width, GLsizei height, GLint, GLenum format, GLenum type,
                const void* pixels)
{
    VKGL_CONTEXT()
    Texture* texture = BoundTexture(*c);
    if (texture == nullptr || level < 0 || level > 15 || width <= 0 || height <= 0)
    {
        return;
    }

    const GLenum sized = SizedFormat(internal, format, type);
    const auto size = std::make_pair(static_cast<u32>(width), static_cast<u32>(height));
    if (level == 0 && (texture->given.empty() || texture->given[0] != size || texture->internal != sized))
    {
        // (a new level 0: the other levels GL had are of another texture now)
        texture->given.assign(1, size);
    }

    texture->internal = sized;
    if (texture->given.size() <= static_cast<size_t>(level))
    {
        texture->given.resize(static_cast<size_t>(level) + 1, {0, 0});
    }

    texture->given[static_cast<size_t>(level)] = size;
    if (!EnsureImage(*texture))
    {
        return;
    }

    if (pixels != nullptr || c->unpackBuffer != 0)
    {
        TexSubImage(*c, *texture, level, 0, 0, width, height, format, type, pixels);
    }
}

void TexSubImage2D(GLenum, GLint level, GLint x, GLint y, GLsizei width, GLsizei height, GLenum format, GLenum type, const void* pixels)
{
    VKGL_CONTEXT()
    Texture* texture = BoundTexture(*c);
    if (texture == nullptr || !EnsureImage(*texture))
    {
        return;
    }

    TexSubImage(*c, *texture, level, x, y, width, height, format, type, pixels);
}

void TexParameterValue(GLenum name, GLfloat value)
{
    VKGL_CONTEXT()
    Texture* texture = BoundTexture(*c);
    if (texture == nullptr)
    {
        return;
    }

    const GLenum asEnum = static_cast<GLenum>(value);
    switch (name)
    {
    case Gl::TEXTURE_MIN_FILTER:
        texture->minFilter = asEnum;
        break;
    case Gl::TEXTURE_MAG_FILTER:
        texture->magFilter = asEnum;
        break;
    case Gl::TEXTURE_WRAP_S:
        texture->wrapS = asEnum;
        break;
    case Gl::TEXTURE_WRAP_T:
        texture->wrapT = asEnum;
        break;
    case Gl::TEXTURE_BASE_LEVEL:
        texture->baseLevel = static_cast<GLint>(value);
        break;
    case Gl::TEXTURE_MAX_LEVEL:
        texture->maxLevel = static_cast<GLint>(value);
        break;
    case Gl::TEXTURE_MAX_ANISOTROPY:
        texture->anisotropy = value;
        break;
    case Gl::TEXTURE_LOD_BIAS:
        texture->lodBias = value;
        break;
    default:
        break;
    }
}

void TexParameteri(GLenum, GLenum name, GLint value)
{
    TexParameterValue(name, static_cast<GLfloat>(value));
}

void TexParameterf(GLenum, GLenum name, GLfloat value)
{
    TexParameterValue(name, value);
}

void PixelStorei(GLenum name, GLint value)
{
    VKGL_CONTEXT()
    switch (name)
    {
    case Gl::UNPACK_ALIGNMENT:
        c->unpackAlignment = value;
        break;
    case Gl::UNPACK_ROW_LENGTH:
        c->unpackRowLength = value;
        break;
    case Gl::PACK_ALIGNMENT:
        c->packAlignment = value;
        break;
    case Gl::PACK_ROW_LENGTH:
        c->packRowLength = value;
        break;
    default:
        break;
    }
}

void GenerateMipmapEntry(GLenum)
{
    VKGL_CONTEXT()
    Texture* texture = BoundTexture(*c);
    if (texture != nullptr)
    {
        GenerateMipmap(*c, *texture);
    }
}

// ------------------------------------------------------------------------------------------------------------------ samplers
void GenSamplers(GLsizei n, GLuint* names)
{
    MakeObjects(GetObjects().samplers, n, names);
}

void DeleteSamplers(GLsizei n, const GLuint* names)
{
    Context* c = Current();
    for (GLsizei i = 0; i < n; i++)
    {
        if (c != nullptr)
        {
            for (GLuint& bound : c->samplers)
            {
                if (bound == names[i])
                {
                    bound = 0;
                }
            }
        }

        TakeObject(GetObjects().samplers, names[i]);
    }
}

void BindSampler(GLuint unit, GLuint name)
{
    VKGL_CONTEXT()
    if (unit < 32)
    {
        c->samplers[unit] = name;
    }
}

void SamplerParameterValue(GLuint name, GLenum parameter, GLfloat value)
{
    SamplerObject* sampler = FindSampler(name);
    if (sampler == nullptr)
    {
        return;
    }

    const GLenum asEnum = static_cast<GLenum>(value);
    switch (parameter)
    {
    case Gl::TEXTURE_MIN_FILTER:
        sampler->minFilter = asEnum;
        break;
    case Gl::TEXTURE_MAG_FILTER:
        sampler->magFilter = asEnum;
        break;
    case Gl::TEXTURE_WRAP_S:
        sampler->wrapS = asEnum;
        break;
    case Gl::TEXTURE_WRAP_T:
        sampler->wrapT = asEnum;
        break;
    case Gl::TEXTURE_MAX_ANISOTROPY:
        sampler->anisotropy = value;
        break;
    case Gl::TEXTURE_LOD_BIAS:
        sampler->lodBias = value;
        break;
    default:
        break;
    }
}

void SamplerParameteri(GLuint name, GLenum parameter, GLint value)
{
    SamplerParameterValue(name, parameter, static_cast<GLfloat>(value));
}

void SamplerParameterf(GLuint name, GLenum parameter, GLfloat value)
{
    SamplerParameterValue(name, parameter, value);
}

// ------------------------------------------------------------------------------------------------------------------ framebuffers
void GenFramebuffers(GLsizei n, GLuint* names)
{
    MakeObjects(GetObjects().framebuffers, n, names);
}

void DeleteFramebuffers(GLsizei n, const GLuint* names)
{
    Context* c = Current();
    for (GLsizei i = 0; i < n; i++)
    {
        if (names[i] == 0)
        {
            continue;
        }

        if (c != nullptr)
        {
            if (c->drawFramebuffer == names[i])
            {
                c->drawFramebuffer = 0;
            }

            if (c->readFramebuffer == names[i])
            {
                c->readFramebuffer = 0;
            }
        }

        TakeObject(GetObjects().framebuffers, names[i]);
    }
}

void BindFramebuffer(GLenum target, GLuint name)
{
    VKGL_CONTEXT()
    if (target == Gl::FRAMEBUFFER || target == Gl::DRAW_FRAMEBUFFER)
    {
        c->drawFramebuffer = name;
    }

    if (target == Gl::FRAMEBUFFER || target == Gl::READ_FRAMEBUFFER)
    {
        c->readFramebuffer = name;
    }
}

void FramebufferTexture2D(GLenum target, GLenum attachment, GLenum, GLuint texture, GLint level)
{
    VKGL_CONTEXT()
    Framebuffer* framebuffer = BoundFramebuffer(*c, target);
    if (framebuffer == nullptr)
    {
        return;
    }

    const Attachment a = {texture, level};
    if (attachment >= Gl::COLOR_ATTACHMENT0 && attachment < Gl::COLOR_ATTACHMENT0 + 4)
    {
        framebuffer->colour[attachment - Gl::COLOR_ATTACHMENT0] = a;
    }
    else if (attachment == Gl::DEPTH_ATTACHMENT)
    {
        framebuffer->depth = a;
    }
    else if (attachment == Gl::STENCIL_ATTACHMENT)
    {
        framebuffer->stencil = a;
    }
    else if (attachment == Gl::DEPTH_STENCIL_ATTACHMENT)
    {
        framebuffer->depth = a;
        framebuffer->stencil = a;
    }
}

GLenum CheckFramebufferStatus(GLenum)
{
    return Gl::FRAMEBUFFER_COMPLETE;
}

void DrawBuffers(GLsizei n, const GLenum* buffers)
{
    VKGL_CONTEXT()
    Framebuffer* framebuffer = BoundFramebuffer(*c, Gl::DRAW_FRAMEBUFFER);
    if (framebuffer == nullptr)
    {
        return;
    }

    for (GLsizei i = 0; i < 4; i++)
    {
        framebuffer->drawBuffers[i] = i < n ? buffers[i] : Gl::NONE;
    }
}

void DrawBuffer(GLenum buffer)
{
    DrawBuffers(1, &buffer);
}

void ReadBuffer(GLenum buffer)
{
    VKGL_CONTEXT()
    Framebuffer* framebuffer = BoundFramebuffer(*c, Gl::READ_FRAMEBUFFER);
    if (framebuffer != nullptr)
    {
        framebuffer->readBuffer = buffer;
    }
}

void BlitFramebufferEntry(GLint sx0, GLint sy0, GLint sx1, GLint sy1, GLint dx0, GLint dy0, GLint dx1, GLint dy1, GLbitfield mask,
                          GLenum filter)
{
    VKGL_CONTEXT()
    BlitFramebuffer(*c, sx0, sy0, sx1, sy1, dx0, dy0, dx1, dy1, mask, filter);
}

void ReadPixelsEntry(GLint x, GLint y, GLsizei width, GLsizei height, GLenum format, GLenum type, void* pixels)
{
    VKGL_CONTEXT()
    ReadPixels(*c, x, y, width, height, format, type, pixels);
}

// ------------------------------------------------------------------------------------------------------------------ the state
void Viewport(GLint x, GLint y, GLsizei width, GLsizei height)
{
    VKGL_CONTEXT()
    c->viewport[0] = x;
    c->viewport[1] = y;
    c->viewport[2] = width;
    c->viewport[3] = height;
}

void Scissor(GLint x, GLint y, GLsizei width, GLsizei height)
{
    VKGL_CONTEXT()
    c->scissor[0] = x;
    c->scissor[1] = y;
    c->scissor[2] = width;
    c->scissor[3] = height;
}

void SetCapability(GLenum capability, bool on)
{
    VKGL_CONTEXT()
    switch (capability)
    {
    case Gl::BLEND:
        c->blend = on;
        break;
    case Gl::DEPTH_TEST:
        c->depthTest = on;
        break;
    case Gl::STENCIL_TEST:
        c->stencilTest = on;
        break;
    case Gl::SCISSOR_TEST:
        c->scissorTest = on;
        break;
    case Gl::CULL_FACE:
        c->cullFace = on;
        break;
    default:
        // (PROGRAM_POINT_SIZE: always, the vertex shader's; DITHER, MULTISAMPLE, FRAMEBUFFER_SRGB: none)
        break;
    }
}

void Enable(GLenum capability)
{
    SetCapability(capability, true);
}

void Disable(GLenum capability)
{
    SetCapability(capability, false);
}

void BlendFuncSeparate(GLenum sourceRgb, GLenum destinationRgb, GLenum sourceAlpha, GLenum destinationAlpha)
{
    VKGL_CONTEXT()
    c->blendSourceRgb = sourceRgb;
    c->blendDestinationRgb = destinationRgb;
    c->blendSourceAlpha = sourceAlpha;
    c->blendDestinationAlpha = destinationAlpha;
}

void BlendFunc(GLenum source, GLenum destination)
{
    BlendFuncSeparate(source, destination, source, destination);
}

void BlendEquationSeparate(GLenum rgb, GLenum alpha)
{
    VKGL_CONTEXT()
    c->blendEquationRgb = rgb;
    c->blendEquationAlpha = alpha;
}

void BlendEquation(GLenum mode)
{
    BlendEquationSeparate(mode, mode);
}

void BlendColor(GLfloat r, GLfloat g, GLfloat b, GLfloat a)
{
    VKGL_CONTEXT()
    c->blendColour[0] = r;
    c->blendColour[1] = g;
    c->blendColour[2] = b;
    c->blendColour[3] = a;
}

void ColorMask(GLboolean r, GLboolean g, GLboolean b, GLboolean a)
{
    VKGL_CONTEXT()
    c->colourMask[0] = r != 0;
    c->colourMask[1] = g != 0;
    c->colourMask[2] = b != 0;
    c->colourMask[3] = a != 0;
}

void DepthMask(GLboolean on)
{
    VKGL_CONTEXT()
    c->depthMask = on != 0;
}

void DepthFunc(GLenum func)
{
    VKGL_CONTEXT()
    c->depthFunc = func;
}

void StencilFunc(GLenum func, GLint ref, GLuint mask)
{
    VKGL_CONTEXT()
    c->stencilFunc = func;
    c->stencilRef = ref;
    c->stencilValueMask = mask;
}

void StencilOp(GLenum fail, GLenum depthFail, GLenum pass)
{
    VKGL_CONTEXT()
    c->stencilFail = fail;
    c->stencilDepthFail = depthFail;
    c->stencilPass = pass;
}

void StencilMask(GLuint mask)
{
    VKGL_CONTEXT()
    c->stencilWriteMask = mask;
}

void ClearColor(GLfloat r, GLfloat g, GLfloat b, GLfloat a)
{
    VKGL_CONTEXT()
    c->clearColour[0] = r;
    c->clearColour[1] = g;
    c->clearColour[2] = b;
    c->clearColour[3] = a;
}

void ClearDepthf(GLfloat depth)
{
    VKGL_CONTEXT()
    c->clearDepth = std::clamp(depth, 0.0f, 1.0f);
}

void ClearDepth(double depth)
{
    ClearDepthf(static_cast<GLfloat>(depth));
}

void ClearStencil(GLint value)
{
    VKGL_CONTEXT()
    c->clearStencil = value;
}

void ClearEntry(GLbitfield mask)
{
    VKGL_CONTEXT()
    Clear(*c, mask);
}

void LineWidth(GLfloat width)
{
    VKGL_CONTEXT()
    c->lineWidth = width;
}

void PointSize(GLfloat)
{
}

// ------------------------------------------------------------------------------------------------------------------ programs
GLuint CreateShader(GLenum type)
{
    const GLuint name = MakeObject(GetObjects().shaders);
    FindShader(name)->type = type;
    return name;
}

void ShaderSource(GLuint name, GLsizei count, const GLchar* const* strings, const GLint* lengths)
{
    ShaderObject* shader = FindShader(name);
    if (shader == nullptr)
    {
        return;
    }

    shader->source.clear();
    for (GLsizei i = 0; i < count; i++)
    {
        if (strings[i] == nullptr)
        {
            continue;
        }

        if (lengths != nullptr && lengths[i] >= 0)
        {
            shader->source.append(strings[i], static_cast<size_t>(lengths[i]));
        }
        else
        {
            shader->source.append(strings[i]);
        }
    }
}

void CompileShaderEntry(GLuint name)
{
    ShaderObject* shader = FindShader(name);
    if (shader != nullptr)
    {
        CompileShader(*shader);
    }
}

void GetShaderiv(GLuint name, GLenum parameter, GLint* value)
{
    ShaderObject* shader = FindShader(name);
    if (shader == nullptr)
    {
        *value = 0;
        return;
    }

    switch (parameter)
    {
    case Gl::COMPILE_STATUS:
        *value = shader->compiled ? 1 : 0;
        break;
    case Gl::INFO_LOG_LENGTH:
        *value = shader->log.empty() ? 0 : static_cast<GLint>(shader->log.size() + 1);
        break;
    default:
        *value = 0;
        break;
    }
}

void CopyLog(const std::string& log, GLsizei size, GLsizei* length, GLchar* out)
{
    if (size <= 0 || out == nullptr)
    {
        if (length != nullptr)
        {
            *length = 0;
        }

        return;
    }

    const size_t n = std::min(log.size(), static_cast<size_t>(size - 1));
    std::memcpy(out, log.data(), n);
    out[n] = '\0';
    if (length != nullptr)
    {
        *length = static_cast<GLsizei>(n);
    }
}

void GetShaderInfoLog(GLuint name, GLsizei size, GLsizei* length, GLchar* out)
{
    ShaderObject* shader = FindShader(name);
    CopyLog(shader != nullptr ? shader->log : std::string(), size, length, out);
}

void DeleteShader(GLuint)
{
    // (kept: a program links its shaders by name, and GL lets them be deleted once attached)
}

GLuint CreateProgram()
{
    return MakeObject(GetObjects().programs);
}

void AttachShader(GLuint programName, GLuint shaderName)
{
    Program* program = FindProgram(programName);
    ShaderObject* shader = FindShader(shaderName);
    if (program == nullptr || shader == nullptr)
    {
        return;
    }

    if (shader->type == Gl::VERTEX_SHADER)
    {
        program->vertexShader = shaderName;
    }
    else
    {
        program->fragmentShader = shaderName;
    }
}

void BindAttribLocation(GLuint programName, GLuint index, const GLchar* name)
{
    Program* program = FindProgram(programName);
    if (program != nullptr && name != nullptr)
    {
        program->attributeLocations[name] = index;
    }
}

void BindFragDataLocationIndexed(GLuint, GLuint, GLuint, const GLchar*)
{
    // (the sources' layout qualifiers say where their outputs go)
}

void LinkProgramEntry(GLuint name)
{
    Program* program = FindProgram(name);
    if (program != nullptr)
    {
        LinkProgram(*program);
    }
}

void GetProgramiv(GLuint name, GLenum parameter, GLint* value)
{
    Program* program = FindProgram(name);
    if (program == nullptr)
    {
        *value = 0;
        return;
    }

    switch (parameter)
    {
    case Gl::LINK_STATUS:
        *value = program->linked ? 1 : 0;
        break;
    case Gl::INFO_LOG_LENGTH:
        *value = program->log.empty() ? 0 : static_cast<GLint>(program->log.size() + 1);
        break;
    default:
        *value = 0;
        break;
    }
}

void GetProgramInfoLog(GLuint name, GLsizei size, GLsizei* length, GLchar* out)
{
    Program* program = FindProgram(name);
    CopyLog(program != nullptr ? program->log : std::string(), size, length, out);
}

void UseProgram(GLuint name)
{
    VKGL_CONTEXT()
    c->program = name;
}

void DeleteProgram(GLuint name)
{
    // (its modules and layout stay: the pipelines made with them are cached by them)
    Context* c = Current();
    if (name == 0 || (c != nullptr && c->program == name))
    {
        return;
    }

    TakeObject(GetObjects().programs, name);
}

GLint GetUniformLocation(GLuint name, const GLchar* uniform)
{
    Program* program = FindProgram(name);
    return program != nullptr && uniform != nullptr ? UniformLocation(*program, uniform) : -1;
}

void Uniform1i(GLint location, GLint x)
{
    SetUniform(location, &x, 1, 1, true);
}

void Uniform1iv(GLint location, GLsizei count, const GLint* values)
{
    SetUniform(location, values, 1, count, true);
}

void Uniform1ui(GLint location, GLuint x)
{
    SetUniform(location, &x, 1, 1, true);
}

void Uniform4ui(GLint location, GLuint x, GLuint y, GLuint z, GLuint w)
{
    const GLuint v[4] = {x, y, z, w};
    SetUniform(location, v, 4, 1, true);
}

void Uniform1f(GLint location, GLfloat x)
{
    SetUniform(location, &x, 1, 1, false);
}

void Uniform2f(GLint location, GLfloat x, GLfloat y)
{
    const GLfloat v[2] = {x, y};
    SetUniform(location, v, 2, 1, false);
}

void Uniform3f(GLint location, GLfloat x, GLfloat y, GLfloat z)
{
    const GLfloat v[3] = {x, y, z};
    SetUniform(location, v, 3, 1, false);
}

void Uniform4f(GLint location, GLfloat x, GLfloat y, GLfloat z, GLfloat w)
{
    const GLfloat v[4] = {x, y, z, w};
    SetUniform(location, v, 4, 1, false);
}

void Uniform1fv(GLint location, GLsizei count, const GLfloat* values)
{
    SetUniform(location, values, 1, count, false);
}

void Uniform2fv(GLint location, GLsizei count, const GLfloat* values)
{
    SetUniform(location, values, 2, count, false);
}

void Uniform4fv(GLint location, GLsizei count, const GLfloat* values)
{
    SetUniform(location, values, 4, count, false);
}

// ------------------------------------------------------------------------------------------------------------------ vertexes
void GenVertexArrays(GLsizei n, GLuint* names)
{
    MakeObjects(GetObjects().vertexArrays, n, names);
}

void DeleteVertexArrays(GLsizei n, const GLuint* names)
{
    Context* c = Current();
    for (GLsizei i = 0; i < n; i++)
    {
        if (c != nullptr && c->vertexArray == names[i])
        {
            c->vertexArray = 0;
        }

        TakeObject(GetObjects().vertexArrays, names[i]);
    }
}

void BindVertexArray(GLuint name)
{
    VKGL_CONTEXT()
    c->vertexArray = name;
}

void GenBuffers(GLsizei n, GLuint* names)
{
    MakeObjects(GetObjects().buffers, n, names);
}

void DeleteBuffers(GLsizei n, const GLuint* names)
{
    Context* c = Current();
    for (GLsizei i = 0; i < n; i++)
    {
        if (c != nullptr)
        {
            for (GLuint* bound : {&c->arrayBuffer, &c->unpackBuffer, &c->packBuffer})
            {
                if (*bound == names[i])
                {
                    *bound = 0;
                }
            }
        }

        TakeObject(GetObjects().buffers, names[i]);
    }
}

void BindBuffer(GLenum target, GLuint name)
{
    VKGL_CONTEXT()
    GLuint* binding = BufferBinding(*c, target);
    if (binding != nullptr)
    {
        *binding = name;
    }
}

void BufferData(GLenum target, GLsizeiptr size, const void* data, GLenum)
{
    VKGL_CONTEXT()
    GLuint* binding = BufferBinding(*c, target);
    BufferObject* buffer = binding != nullptr ? FindBuffer(*binding) : nullptr;
    if (buffer == nullptr || size < 0)
    {
        return;
    }

    if (data != nullptr)
    {
        const auto* bytes = static_cast<const u8*>(data);
        buffer->data.assign(bytes, bytes + size);
    }
    else
    {
        buffer->data.assign(static_cast<size_t>(size), 0);
    }
}

void BufferSubData(GLenum target, GLintptr offset, GLsizeiptr size, const void* data)
{
    VKGL_CONTEXT()
    GLuint* binding = BufferBinding(*c, target);
    BufferObject* buffer = binding != nullptr ? FindBuffer(*binding) : nullptr;
    if (buffer == nullptr || data == nullptr || offset < 0 || size < 0 || static_cast<size_t>(offset + size) > buffer->data.size())
    {
        return;
    }

    std::memcpy(buffer->data.data() + offset, data, static_cast<size_t>(size));
}

void SetAttribute(GLuint index, GLint size, GLenum type, bool normalized, bool integer, GLsizei stride, const void* pointer)
{
    VKGL_CONTEXT()
    VertexArray* vao = BoundVertexArray(*c);
    if (vao == nullptr || index >= 16)
    {
        return;
    }

    VertexAttrib& a = vao->attribs[index];
    a.buffer = c->arrayBuffer;
    a.size = size;
    a.type = type;
    a.normalized = normalized;
    a.integer = integer;
    a.stride = stride;
    a.offset = reinterpret_cast<GLintptr>(pointer);
}

void VertexAttribPointer(GLuint index, GLint size, GLenum type, GLboolean normalized, GLsizei stride, const void* pointer)
{
    SetAttribute(index, size, type, normalized != 0, false, stride, pointer);
}

void VertexAttribIPointer(GLuint index, GLint size, GLenum type, GLsizei stride, const void* pointer)
{
    SetAttribute(index, size, type, false, true, stride, pointer);
}

void SetAttributeEnabled(GLuint index, bool on)
{
    VKGL_CONTEXT()
    VertexArray* vao = BoundVertexArray(*c);
    if (vao != nullptr && index < 16)
    {
        vao->attribs[index].enabled = on;
    }
}

void EnableVertexAttribArray(GLuint index)
{
    SetAttributeEnabled(index, true);
}

void DisableVertexAttribArray(GLuint index)
{
    SetAttributeEnabled(index, false);
}

void DrawArrays(GLenum mode, GLint first, GLsizei count)
{
    VKGL_CONTEXT()
    BoundVertexArray(*c);
    Draw(*c, mode, first, count);
}

// ------------------------------------------------------------------------------------------------------------------ syncs
void Flush()
{
    VKGL_CONTEXT()
    Submit(*c, false);
}

void FinishEntry()
{
    VKGL_CONTEXT()
    Submit(*c, true);
}

GLsync FenceSync(GLenum, GLbitfield)
{
    VKGL_CONTEXT(nullptr)
    auto* sync = new Sync();
    const u64 serial = Submit(*c, false);
    sync->serial = serial != 0 ? serial : GetDevice().submitted.load();
    return reinterpret_cast<GLsync>(sync);
}

void WaitSync(GLsync, GLbitfield, GLuint64)
{
    // (the one queue runs the submissions in turn, each begun by a barrier on all before: the fence's work is done before the
    // waiting context's next)
}

GLenum ClientWaitSync(GLsync sync, GLbitfield, GLuint64)
{
    if (sync != nullptr)
    {
        WaitSerial(reinterpret_cast<Sync*>(sync)->serial);
    }

    return 0x911C; // CONDITION_SATISFIED
}

void DeleteSync(GLsync sync)
{
    delete reinterpret_cast<Sync*>(sync);
}

GLenum GetError()
{
    return 0;
}

// ------------------------------------------------------------------------------------------------------------------ queries
void GenQueries(GLsizei n, GLuint* names)
{
    MakeObjects(GetObjects().queries, n, names);
}

void DeleteQueries(GLsizei n, const GLuint* names)
{
    for (GLsizei i = 0; i < n; i++)
    {
        std::unique_ptr<Query> query = TakeObject(GetObjects().queries, names[i]);
        if (query != nullptr && query->ended && !query->resultKnown)
        {
            // (its timestamps' slots given back once they're written)
            Query* kept = query.release();
            AfterSerial(kept->serial, [kept] {
                GLuint64 ignored = 0;
                QueryResult(*kept, &ignored);
                delete kept;
            });
        }
    }
}

void BeginQueryEntry(GLenum, GLuint name)
{
    VKGL_CONTEXT()
    Query* query = FindQuery(name);
    if (query == nullptr)
    {
        return;
    }

    c->activeQuery = name;
    BeginQuery(*c, *query);
}

void EndQueryEntry(GLenum)
{
    VKGL_CONTEXT()
    Query* query = FindQuery(c->activeQuery);
    c->activeQuery = 0;
    if (query != nullptr)
    {
        EndQuery(*c, *query);
    }
}

GLuint64 QueryValue(GLuint name, GLenum parameter)
{
    Query* query = FindQuery(name);
    if (query == nullptr)
    {
        return 0;
    }

    GLuint64 result = 0;
    if (parameter == Gl::QUERY_RESULT_AVAILABLE)
    {
        return QueryResult(*query, &result) ? 1 : 0;
    }

    if (QueryResult(*query, &result))
    {
        return result;
    }

    // (waited for: its submission made, then done)
    Context* c = Current();
    if (c != nullptr && query->serial > GetDevice().submitted.load())
    {
        Submit(*c, false);
    }

    WaitSerial(query->serial);
    if (!QueryResult(*query, &result))
    {
        vkDeviceWaitIdle(GetDevice().device);
        QueryResult(*query, &result);
    }

    return result;
}

void GetQueryObjectuiv(GLuint name, GLenum parameter, GLuint* value)
{
    *value = static_cast<GLuint>(QueryValue(name, parameter));
}

void GetQueryObjectui64v(GLuint name, GLenum parameter, GLuint64* value)
{
    *value = QueryValue(name, parameter);
}

// ------------------------------------------------------------------------------------------------------------------ asking
const unsigned char* GetString(GLenum name)
{
    static std::string vendor = "VkGl";
    static std::string renderer;
    static std::string version = "3.3.0 Core (VkGl)";
    static std::string language = "3.30 (VkGl: glslang to SPIR-V)";
    static std::string extensions;
    switch (name)
    {
    case Gl::VENDOR:
        return reinterpret_cast<const unsigned char*>(vendor.c_str());
    case Gl::RENDERER:
        renderer = std::string("VkGl on ") + (Started() ? GetDevice().properties.deviceName : "no device");
        return reinterpret_cast<const unsigned char*>(renderer.c_str());
    case Gl::VERSION:
        return reinterpret_cast<const unsigned char*>(version.c_str());
    case Gl::SHADING_LANGUAGE_VERSION:
        return reinterpret_cast<const unsigned char*>(language.c_str());
    case Gl::EXTENSIONS:
        extensions.clear();
        for (const std::string& e : Extensions())
        {
            extensions += e + " ";
        }

        return reinterpret_cast<const unsigned char*>(extensions.c_str());
    default:
        return nullptr;
    }
}

const unsigned char* GetStringi(GLenum name, GLuint index)
{
    static std::vector<std::string> list;
    if (name != Gl::EXTENSIONS)
    {
        return nullptr;
    }

    list = Extensions();
    return index < list.size() ? reinterpret_cast<const unsigned char*>(list[index].c_str()) : nullptr;
}

void GetIntegerv(GLenum name, GLint* value)
{
    Context* c = Current();
    const VkPhysicalDeviceLimits* limits = Started() ? &GetDevice().properties.limits : nullptr;
    switch (name)
    {
    case Gl::MAJOR_VERSION:
        *value = 3;
        return;
    case Gl::MINOR_VERSION:
        *value = 3;
        return;
    case Gl::NUM_EXTENSIONS:
        *value = static_cast<GLint>(Extensions().size());
        return;
    case Gl::MAX_TEXTURE_SIZE:
        *value = limits != nullptr ? static_cast<GLint>(limits->maxImageDimension2D) : 4096;
        return;
    case Gl::MAX_VIEWPORT_DIMS:
        value[0] = limits != nullptr ? static_cast<GLint>(limits->maxViewportDimensions[0]) : 4096;
        value[1] = limits != nullptr ? static_cast<GLint>(limits->maxViewportDimensions[1]) : 4096;
        return;
    case Gl::MAX_COLOR_ATTACHMENTS:
    case Gl::MAX_DRAW_BUFFERS:
        *value = 4;
        return;
    case Gl::MAX_DUAL_SOURCE_DRAW_BUFFERS:
        *value = Started() && GetDevice().dualSource ? 1 : 0;
        return;
    case Gl::IMPLEMENTATION_COLOR_READ_FORMAT:
        *value = static_cast<GLint>(Gl::RGBA);
        return;
    case Gl::IMPLEMENTATION_COLOR_READ_TYPE:
        *value = static_cast<GLint>(Gl::UNSIGNED_BYTE);
        return;
    default:
        break;
    }

    if (c == nullptr)
    {
        *value = 0;
        return;
    }

    switch (name)
    {
    case Gl::FRAMEBUFFER_BINDING:
        *value = static_cast<GLint>(c->drawFramebuffer);
        break;
    case Gl::READ_FRAMEBUFFER + 0x2: // READ_FRAMEBUFFER_BINDING (0x8CAA)
        *value = static_cast<GLint>(c->readFramebuffer);
        break;
    case Gl::CURRENT_PROGRAM:
        *value = static_cast<GLint>(c->program);
        break;
    case Gl::VIEWPORT:
        std::copy(c->viewport, c->viewport + 4, value);
        break;
    case Gl::ACTIVE_TEXTURE:
        *value = static_cast<GLint>(Gl::TEXTURE0 + c->activeUnit);
        break;
    case Gl::TEXTURE_BINDING_2D:
        *value = static_cast<GLint>(c->textures[c->activeUnit]);
        break;
    default:
        *value = 0;
        break;
    }
}

void GetFloatv(GLenum name, GLfloat* value)
{
    if (name == Gl::MAX_TEXTURE_MAX_ANISOTROPY)
    {
        *value = Started() && GetDevice().anisotropy ? GetDevice().maxAnisotropy : 1.0f;
        return;
    }

    GLint asInteger[4] = {};
    GetIntegerv(name, asInteger);
    *value = static_cast<GLfloat>(asInteger[0]);
}

// ------------------------------------------------------------------------------------------------------------------ the table
const std::unordered_map<std::string, void*>& Table()
{
#define VKGL_ENTRY(name, function) {name, reinterpret_cast<void*>(&function)}
    static const std::unordered_map<std::string, void*> table = {
        VKGL_ENTRY("glGenTextures", GenTextures),
        VKGL_ENTRY("glDeleteTextures", DeleteTextures),
        VKGL_ENTRY("glBindTexture", BindTexture),
        VKGL_ENTRY("glActiveTexture", ActiveTexture),
        VKGL_ENTRY("glTexImage2D", TexImage2D),
        VKGL_ENTRY("glTexSubImage2D", TexSubImage2D),
        VKGL_ENTRY("glTexParameteri", TexParameteri),
        VKGL_ENTRY("glTexParameterf", TexParameterf),
        VKGL_ENTRY("glPixelStorei", PixelStorei),
        VKGL_ENTRY("glGenerateMipmap", GenerateMipmapEntry),
        VKGL_ENTRY("glGenSamplers", GenSamplers),
        VKGL_ENTRY("glDeleteSamplers", DeleteSamplers),
        VKGL_ENTRY("glBindSampler", BindSampler),
        VKGL_ENTRY("glSamplerParameteri", SamplerParameteri),
        VKGL_ENTRY("glSamplerParameterf", SamplerParameterf),
        VKGL_ENTRY("glGenFramebuffers", GenFramebuffers),
        VKGL_ENTRY("glDeleteFramebuffers", DeleteFramebuffers),
        VKGL_ENTRY("glBindFramebuffer", BindFramebuffer),
        VKGL_ENTRY("glFramebufferTexture2D", FramebufferTexture2D),
        VKGL_ENTRY("glCheckFramebufferStatus", CheckFramebufferStatus),
        VKGL_ENTRY("glDrawBuffer", DrawBuffer),
        VKGL_ENTRY("glDrawBuffers", DrawBuffers),
        VKGL_ENTRY("glReadBuffer", ReadBuffer),
        VKGL_ENTRY("glBlitFramebuffer", BlitFramebufferEntry),
        VKGL_ENTRY("glReadPixels", ReadPixelsEntry),
        VKGL_ENTRY("glViewport", Viewport),
        VKGL_ENTRY("glScissor", Scissor),
        VKGL_ENTRY("glEnable", Enable),
        VKGL_ENTRY("glDisable", Disable),
        VKGL_ENTRY("glBlendFunc", BlendFunc),
        VKGL_ENTRY("glBlendFuncSeparate", BlendFuncSeparate),
        VKGL_ENTRY("glBlendEquation", BlendEquation),
        VKGL_ENTRY("glBlendEquationSeparate", BlendEquationSeparate),
        VKGL_ENTRY("glBlendColor", BlendColor),
        VKGL_ENTRY("glColorMask", ColorMask),
        VKGL_ENTRY("glDepthMask", DepthMask),
        VKGL_ENTRY("glDepthFunc", DepthFunc),
        VKGL_ENTRY("glStencilFunc", StencilFunc),
        VKGL_ENTRY("glStencilOp", StencilOp),
        VKGL_ENTRY("glStencilMask", StencilMask),
        VKGL_ENTRY("glClear", ClearEntry),
        VKGL_ENTRY("glClearColor", ClearColor),
        VKGL_ENTRY("glClearDepth", ClearDepth),
        VKGL_ENTRY("glClearDepthf", ClearDepthf),
        VKGL_ENTRY("glClearStencil", ClearStencil),
        VKGL_ENTRY("glLineWidth", LineWidth),
        VKGL_ENTRY("glPointSize", PointSize),
        VKGL_ENTRY("glCreateShader", CreateShader),
        VKGL_ENTRY("glShaderSource", ShaderSource),
        VKGL_ENTRY("glCompileShader", CompileShaderEntry),
        VKGL_ENTRY("glGetShaderiv", GetShaderiv),
        VKGL_ENTRY("glGetShaderInfoLog", GetShaderInfoLog),
        VKGL_ENTRY("glDeleteShader", DeleteShader),
        VKGL_ENTRY("glCreateProgram", CreateProgram),
        VKGL_ENTRY("glAttachShader", AttachShader),
        VKGL_ENTRY("glBindAttribLocation", BindAttribLocation),
        VKGL_ENTRY("glBindFragDataLocationIndexed", BindFragDataLocationIndexed),
        VKGL_ENTRY("glLinkProgram", LinkProgramEntry),
        VKGL_ENTRY("glGetProgramiv", GetProgramiv),
        VKGL_ENTRY("glGetProgramInfoLog", GetProgramInfoLog),
        VKGL_ENTRY("glUseProgram", UseProgram),
        VKGL_ENTRY("glDeleteProgram", DeleteProgram),
        VKGL_ENTRY("glGetUniformLocation", GetUniformLocation),
        VKGL_ENTRY("glUniform1i", Uniform1i),
        VKGL_ENTRY("glUniform1iv", Uniform1iv),
        VKGL_ENTRY("glUniform1ui", Uniform1ui),
        VKGL_ENTRY("glUniform4ui", Uniform4ui),
        VKGL_ENTRY("glUniform1f", Uniform1f),
        VKGL_ENTRY("glUniform2f", Uniform2f),
        VKGL_ENTRY("glUniform3f", Uniform3f),
        VKGL_ENTRY("glUniform4f", Uniform4f),
        VKGL_ENTRY("glUniform1fv", Uniform1fv),
        VKGL_ENTRY("glUniform2fv", Uniform2fv),
        VKGL_ENTRY("glUniform4fv", Uniform4fv),
        VKGL_ENTRY("glGenVertexArrays", GenVertexArrays),
        VKGL_ENTRY("glDeleteVertexArrays", DeleteVertexArrays),
        VKGL_ENTRY("glBindVertexArray", BindVertexArray),
        VKGL_ENTRY("glGenBuffers", GenBuffers),
        VKGL_ENTRY("glDeleteBuffers", DeleteBuffers),
        VKGL_ENTRY("glBindBuffer", BindBuffer),
        VKGL_ENTRY("glBufferData", BufferData),
        VKGL_ENTRY("glBufferSubData", BufferSubData),
        VKGL_ENTRY("glVertexAttribPointer", VertexAttribPointer),
        VKGL_ENTRY("glVertexAttribIPointer", VertexAttribIPointer),
        VKGL_ENTRY("glEnableVertexAttribArray", EnableVertexAttribArray),
        VKGL_ENTRY("glDisableVertexAttribArray", DisableVertexAttribArray),
        VKGL_ENTRY("glDrawArrays", DrawArrays),
        VKGL_ENTRY("glFlush", Flush),
        VKGL_ENTRY("glFinish", FinishEntry),
        VKGL_ENTRY("glFenceSync", FenceSync),
        VKGL_ENTRY("glWaitSync", WaitSync),
        VKGL_ENTRY("glClientWaitSync", ClientWaitSync),
        VKGL_ENTRY("glDeleteSync", DeleteSync),
        VKGL_ENTRY("glGetError", GetError),
        VKGL_ENTRY("glGenQueries", GenQueries),
        VKGL_ENTRY("glDeleteQueries", DeleteQueries),
        VKGL_ENTRY("glBeginQuery", BeginQueryEntry),
        VKGL_ENTRY("glEndQuery", EndQueryEntry),
        VKGL_ENTRY("glGetQueryObjectuiv", GetQueryObjectuiv),
        VKGL_ENTRY("glGetQueryObjectui64v", GetQueryObjectui64v),
        VKGL_ENTRY("glGetString", GetString),
        VKGL_ENTRY("glGetStringi", GetStringi),
        VKGL_ENTRY("glGetIntegerv", GetIntegerv),
        VKGL_ENTRY("glGetFloatv", GetFloatv),
    };
#undef VKGL_ENTRY
    return table;
}
}

void* GetProcAddress(const char* name)
{
    if (name == nullptr)
    {
        return nullptr;
    }

    const auto& table = Table();
    auto found = table.find(name);
    if (found != table.end())
    {
        return found->second;
    }

    // (the ES extensions' names: the same entry points)
    std::string plain = name;
    for (const char* suffix : {"EXT", "ARB", "OES"})
    {
        const size_t n = std::strlen(suffix);
        if (plain.size() > n && plain.compare(plain.size() - n, n, suffix) == 0)
        {
            found = table.find(plain.substr(0, plain.size() - n));
            if (found != table.end())
            {
                return found->second;
            }
        }
    }

    return nullptr;
}

bool HasExtension(const char* name)
{
    if (name == nullptr)
    {
        return false;
    }

    for (const std::string& e : Extensions())
    {
        if (e == name)
        {
            return true;
        }
    }

    return false;
}
}
