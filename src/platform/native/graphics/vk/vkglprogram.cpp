// VkGl's programs: the users' GLSL 330 core (the hardware renderer's, the presenter's, the overlay's) made Vulkan's GLSL 450 and
// compiled to SPIR-V by glslang with its relaxed Vulkan rules (the loose uniforms gathered in one block, set 0 binding 0). The text
// changed first: the version; the samplers numbered (bindings 1 on, in the order they're declared, the same name the same binding in
// both stages); the vertex inputs given their locations (BindAttribLocation's, else the next free); GL's depth range made Vulkan's
// after the vertex shader's main. The uniforms' places are glslang's reflection's (std140: an array's elements 16 bytes apart)
#include "vkglinternal.h"

#include <glslang/Public/ResourceLimits.h>
#include <glslang/Public/ShaderLang.h>
#include <glslang/SPIRV/GlslangToSpv.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <set>

namespace VkGl
{
namespace
{
std::once_flag g_GlslangStarted;
std::mutex g_CompileLock;

void StartGlslang()
{
    std::call_once(g_GlslangStarted, [] { glslang::InitializeProcess(); });
}

bool IsWordChar(char c)
{
    return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_';
}

// The declarations' lines that start with a keyword (after a layout(...) when there's one): "uniform sampler2D tex;" gives the type
// and the name
struct Declaration
{
    size_t start;
    size_t end;
    std::string qualifier;
    std::string type;
    std::string name;
    bool hasLayout;
};

std::vector<Declaration> Declarations(const std::string& source, const char* keyword)
{
    std::vector<Declaration> found;
    size_t lineStart = 0;
    while (lineStart < source.size())
    {
        size_t lineEnd = source.find('\n', lineStart);
        if (lineEnd == std::string::npos)
        {
            lineEnd = source.size();
        }

        std::string line = source.substr(lineStart, lineEnd - lineStart);
        size_t at = line.find_first_not_of(" \t");
        bool hasLayout = false;
        if (at != std::string::npos && line.compare(at, 6, "layout") == 0)
        {
            size_t close = line.find(')', at);
            if (close != std::string::npos)
            {
                hasLayout = true;
                at = line.find_first_not_of(" \t", close + 1);
            }
        }

        // An interpolation qualifier before in/out ("noperspective out vec4 vColour;")
        for (const char* interpolation : {"noperspective ", "flat ", "smooth ", "centroid "})
        {
            if (at != std::string::npos && line.compare(at, std::strlen(interpolation), interpolation) == 0)
            {
                at = line.find_first_not_of(" \t", at + std::strlen(interpolation));
            }
        }

        const size_t keywordLength = std::strlen(keyword);
        if (at != std::string::npos && line.compare(at, keywordLength, keyword) == 0 && at + keywordLength < line.size() &&
            line[at + keywordLength] == ' ')
        {
            size_t typeStart = line.find_first_not_of(" \t", at + keywordLength);
            // (a precision qualifier: skipped)
            for (const char* precision : {"highp ", "mediump ", "lowp "})
            {
                if (typeStart != std::string::npos && line.compare(typeStart, std::strlen(precision), precision) == 0)
                {
                    typeStart = line.find_first_not_of(" \t", typeStart + std::strlen(precision));
                }
            }

            size_t typeEnd = typeStart;
            while (typeEnd < line.size() && IsWordChar(line[typeEnd]))
            {
                typeEnd++;
            }

            size_t nameStart = line.find_first_not_of(" \t", typeEnd);
            size_t nameEnd = nameStart;
            while (nameEnd < line.size() && IsWordChar(line[nameEnd]))
            {
                nameEnd++;
            }

            if (typeStart != std::string::npos && nameStart != std::string::npos && nameEnd > nameStart)
            {
                found.push_back({lineStart + (hasLayout ? line.find("layout") : at), lineStart + lineEnd - lineStart, keyword,
                                 line.substr(typeStart, typeEnd - typeStart), line.substr(nameStart, nameEnd - nameStart), hasLayout});
            }
        }

        lineStart = lineEnd + 1;
    }

    return found;
}

bool IsSamplerType(const std::string& type)
{
    return type.find("sampler") != std::string::npos;
}

// A shader's text made for glslang's Vulkan: the version, the samplers' bindings (from the program's table: a name's binding once
// given), the vertex inputs' locations (given ones kept, BindAttribLocation's, else the next free), the depth range
std::string Translate(const std::string& given, bool vertex, std::map<std::string, u32>& samplerBindings,
                      const std::map<std::string, GLuint>& attributes, std::map<u32, bool>* inputs)
{
    std::string source = given;
    size_t at = source.find("#version");
    if (at != std::string::npos)
    {
        size_t end = source.find('\n', at);
        source.replace(at, end - at, "#version 450");
    }
    else
    {
        source = "#version 450\n" + source;
    }

    // The samplers, in declaration order (from the end, so the places found stay right)
    std::vector<Declaration> uniforms = Declarations(source, "uniform");
    for (const Declaration& d : uniforms)
    {
        if (IsSamplerType(d.type) && samplerBindings.find(d.name) == samplerBindings.end())
        {
            const u32 binding = 1 + static_cast<u32>(samplerBindings.size());
            samplerBindings[d.name] = binding;
        }
    }

    for (auto it = uniforms.rbegin(); it != uniforms.rend(); ++it)
    {
        if (IsSamplerType(it->type) && !it->hasLayout)
        {
            char layout[48];
            std::snprintf(layout, sizeof(layout), "layout(set = 0, binding = %u) ", samplerBindings[it->name]);
            source.insert(it->start, layout);
        }
    }

    if (vertex)
    {
        std::vector<Declaration> ins = Declarations(source, "in");
        std::set<u32> taken;
        for (const Declaration& d : ins)
        {
            if (d.hasLayout)
            {
                const size_t location = source.find("location", d.start);
                if (location != std::string::npos && location < d.end)
                {
                    const size_t equals = source.find('=', location);
                    const u32 value = static_cast<u32>(std::atoi(source.c_str() + equals + 1));
                    taken.insert(value);
                    if (inputs != nullptr)
                    {
                        (*inputs)[value] = d.type.find("uint") == 0 || d.type.find("int") == 0 || d.type.find("ivec") == 0 ||
                                           d.type.find("uvec") == 0;
                    }
                }
            }
        }

        for (const auto& bound : attributes)
        {
            taken.insert(bound.second);
        }

        for (auto it = ins.rbegin(); it != ins.rend(); ++it)
        {
            if (it->hasLayout)
            {
                continue;
            }

            u32 location = 0;
            auto bound = attributes.find(it->name);
            if (bound != attributes.end())
            {
                location = bound->second;
            }
            else
            {
                while (taken.count(location) != 0)
                {
                    location++;
                }

                taken.insert(location);
            }

            if (inputs != nullptr)
            {
                (*inputs)[location] = it->type.find("uint") == 0 || it->type.find("int") == 0 || it->type.find("ivec") == 0 ||
                                      it->type.find("uvec") == 0;
            }

            char layout[32];
            std::snprintf(layout, sizeof(layout), "layout(location = %u) ", location);
            source.insert(it->start, layout);
        }

        // GL's clip space depth (-1 to 1) made Vulkan's (0 to 1) after the shader's own main
        size_t main = source.find("void main()");
        if (main != std::string::npos)
        {
            source.replace(main, 11, "void twin_main()");
            source += "\nvoid main()\n{\n    twin_main();\n    gl_Position.z = (gl_Position.z + gl_Position.w) * 0.5;\n}\n";
        }
    }

    return source;
}

bool CompileStages(const std::string& vertexSource, const std::string& fragmentSource, std::vector<u32>* vertexSpirv,
                   std::vector<u32>* fragmentSpirv, std::string* log, Program* reflect)
{
    StartGlslang();
    std::lock_guard lock(g_CompileLock);
    glslang::TShader vertex(EShLangVertex);
    glslang::TShader fragment(EShLangFragment);
    glslang::TShader* shaders[2] = {&vertex, &fragment};
    const char* sources[2] = {vertexSource.c_str(), fragmentSource.c_str()};
    for (int i = 0; i < 2; i++)
    {
        glslang::TShader& s = *shaders[i];
        s.setStrings(&sources[i], 1);
        s.setEnvInput(glslang::EShSourceGlsl, i == 0 ? EShLangVertex : EShLangFragment, glslang::EShClientVulkan, 100);
        s.setEnvClient(glslang::EShClientVulkan, glslang::EShTargetVulkan_1_1);
        s.setEnvTarget(glslang::EShTargetSpv, glslang::EShTargetSpv_1_3);
        s.setEnvInputVulkanRulesRelaxed();
        s.setGlobalUniformBlockName("TwinUniforms");
        s.setGlobalUniformSet(0);
        s.setGlobalUniformBinding(0);
        s.setAutoMapLocations(true);
        if (!s.parse(GetDefaultResources(), 450, false, EShMsgDefault))
        {
            *log = std::string(i == 0 ? "vertex shader: " : "fragment shader: ") + s.getInfoLog();
            return false;
        }
    }

    glslang::TProgram program;
    program.addShader(&vertex);
    program.addShader(&fragment);
    if (!program.link(EShMsgDefault) || !program.mapIO())
    {
        *log = std::string("link: ") + program.getInfoLog();
        return false;
    }

    if (reflect != nullptr)
    {
        program.buildReflection(EShReflectionDefault | EShReflectionSeparateBuffers | EShReflectionAllBlockVariables);
        reflect->uniforms.clear();
        reflect->hasBlock = false;
        size_t blockSize = 0;
        for (int i = 0; i < program.getNumUniformBlocks(); i++)
        {
            const glslang::TObjectReflection& b = program.getUniformBlock(i);
            if (b.name == "TwinUniforms")
            {
                reflect->hasBlock = true;
                blockSize = static_cast<size_t>(b.size);
            }
        }

        reflect->block.assign(blockSize, 0);
        for (int i = 0; i < program.getNumUniformVariables(); i++)
        {
            const glslang::TObjectReflection& u = program.getUniform(i);
            Uniform made;
            made.name = u.name;
            // (an array's name ends in "[0]" in some reflections: the name without it)
            if (made.name.size() > 3 && made.name.compare(made.name.size() - 3, 3, "[0]") == 0)
            {
                made.name.resize(made.name.size() - 3);
            }

            made.glType = static_cast<GLenum>(u.glDefineType);
            made.arraySize = std::max(1, u.size);
            made.arrayStride = 16;
            if (u.getBinding() >= 0 && u.offset < 0)
            {
                made.sampler = true;
                made.binding = static_cast<u32>(u.getBinding());
            }
            else
            {
                made.offset = u.offset;
            }

            reflect->uniforms.push_back(made);
        }
    }

    glslang::SpvOptions options;
    options.generateDebugInfo = false;
    options.disableOptimizer = true;
    glslang::GlslangToSpv(*program.getIntermediate(EShLangVertex), *vertexSpirv, &options);
    glslang::GlslangToSpv(*program.getIntermediate(EShLangFragment), *fragmentSpirv, &options);
    return true;
}

VkShaderModule Module(const std::vector<u32>& spirv)
{
    VkShaderModuleCreateInfo info = {};
    info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    info.codeSize = spirv.size() * 4;
    info.pCode = spirv.data();
    VkShaderModule module = VK_NULL_HANDLE;
    vkCreateShaderModule(GetDevice().device, &info, nullptr, &module);
    return module;
}

bool MakeLayout(Program& program, const std::map<std::string, u32>& samplerBindings)
{
    Device& d = GetDevice();
    std::vector<VkDescriptorSetLayoutBinding> bindings;
    if (program.hasBlock)
    {
        VkDescriptorSetLayoutBinding block = {};
        block.binding = 0;
        block.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
        block.descriptorCount = 1;
        block.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
        bindings.push_back(block);
    }

    // Every sampler declared (used or not: its binding is in the SPIR-V either way)
    program.samplerUnits.assign(samplerBindings.size(), 0);
    program.samplerKinds.assign(samplerBindings.size(), VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
    for (const auto& sampler : samplerBindings)
    {
        VkDescriptorSetLayoutBinding binding = {};
        binding.binding = sampler.second;
        binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        binding.descriptorCount = 1;
        binding.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
        bindings.push_back(binding);
    }

    VkDescriptorSetLayoutCreateInfo info = {};
    info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    info.bindingCount = static_cast<u32>(bindings.size());
    info.pBindings = bindings.data();
    if (vkCreateDescriptorSetLayout(d.device, &info, nullptr, &program.setLayout) != VK_SUCCESS)
    {
        return false;
    }

    VkPipelineLayoutCreateInfo layoutInfo = {};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.setLayoutCount = 1;
    layoutInfo.pSetLayouts = &program.setLayout;
    return vkCreatePipelineLayout(d.device, &layoutInfo, nullptr, &program.layout) == VK_SUCCESS;
}
}

bool CompileShader(ShaderObject& shader)
{
    // Compiled with its program (glslang links the stages together): GL's compile only checks there's a source
    shader.compiled = !shader.source.empty();
    shader.log = shader.compiled ? "" : "no source";
    return shader.compiled;
}

bool LinkProgram(Program& program)
{
    ShaderObject* vs = FindShader(program.vertexShader);
    ShaderObject* fs = FindShader(program.fragmentShader);
    program.linked = false;
    if (vs == nullptr || fs == nullptr)
    {
        program.log = "a stage is missing";
        return false;
    }

    std::map<std::string, u32> samplerBindings;
    program.inputs.clear();
    std::string vertexSource = Translate(vs->source, true, samplerBindings, program.attributeLocations, &program.inputs);
    std::string fragmentSource = Translate(fs->source, false, samplerBindings, {}, nullptr);
    program.dualSource = fragmentSource.find("index = 1") != std::string::npos || fragmentSource.find("index=1") != std::string::npos;
    std::vector<u32> vertexSpirv;
    std::vector<u32> fragmentSpirv;
    if (!CompileStages(vertexSource, fragmentSource, &vertexSpirv, &fragmentSpirv, &program.log, &program))
    {
        Log("a program didn't compile: %s", program.log.c_str());
        // The sources numbered, for the log
        for (const std::string* source : {&vertexSource, &fragmentSource})
        {
            int number = 1;
            size_t at = 0;
            while (at < source->size())
            {
                size_t end = source->find('\n', at);
                if (end == std::string::npos)
                {
                    end = source->size();
                }

                Log("%4d  %s", number++, source->substr(at, end - at).c_str());
                at = end + 1;
            }
        }

        return false;
    }

    // The samplers the reflection didn't list (unused): theirs from the table, so their units can still be set
    for (const auto& sampler : samplerBindings)
    {
        bool listed = false;
        for (Uniform& u : program.uniforms)
        {
            if (u.sampler && u.name == sampler.first)
            {
                listed = true;
                u.binding = sampler.second;
            }
        }

        if (!listed)
        {
            Uniform u;
            u.name = sampler.first;
            u.sampler = true;
            u.binding = sampler.second;
            program.uniforms.push_back(u);
        }
    }

    program.locations.clear();
    if (!MakeLayout(program, samplerBindings))
    {
        program.log = "no pipeline layout";
        return false;
    }

    program.vertex = Module(vertexSpirv);
    program.fragment = Module(fragmentSpirv);
    program.linked = program.vertex != VK_NULL_HANDLE && program.fragment != VK_NULL_HANDLE;
    return program.linked;
}

GLint UniformLocation(Program& program, const char* name)
{
    auto known = program.locations.find(name);
    if (known != program.locations.end())
    {
        return known->second;
    }

    // "name" or "name[index]": the uniform's number and the element (a location is the number times 256 and the element)
    std::string base = name;
    u32 element = 0;
    size_t bracket = base.find('[');
    if (bracket != std::string::npos)
    {
        element = static_cast<u32>(std::atoi(base.c_str() + bracket + 1));
        base.resize(bracket);
    }

    GLint location = -1;
    for (size_t i = 0; i < program.uniforms.size(); i++)
    {
        const Uniform& u = program.uniforms[i];
        if (u.name == base && element < static_cast<u32>(u.arraySize))
        {
            location = static_cast<GLint>((i << 8) | element);
            break;
        }
    }

    program.locations[name] = location;
    return location;
}

void SetUniform(GLint location, const void* values, u32 components, GLsizei count, bool integer)
{
    Context* c = Current();
    if (c == nullptr || location < 0)
    {
        return;
    }

    Program* program = FindProgram(c->program);
    if (program == nullptr)
    {
        return;
    }

    const size_t index = static_cast<size_t>(location) >> 8;
    const u32 element = static_cast<u32>(location) & 0xFF;
    if (index >= program->uniforms.size())
    {
        return;
    }

    const Uniform& u = program->uniforms[index];
    if (u.sampler)
    {
        if (integer && u.binding >= 1 && u.binding - 1 < program->samplerUnits.size())
        {
            program->samplerUnits[u.binding - 1] = static_cast<u32>(*static_cast<const GLint*>(values));
        }

        return;
    }

    const u8* from = static_cast<const u8*>(values);
    for (GLsizei i = 0; i < count && element + static_cast<u32>(i) < static_cast<u32>(u.arraySize); i++)
    {
        const size_t at = static_cast<size_t>(u.offset) + static_cast<size_t>(element + static_cast<u32>(i)) *
                                                              (u.arraySize > 1 ? static_cast<size_t>(u.arrayStride) : 0);
        if (at + components * 4 <= program->block.size())
        {
            std::memcpy(program->block.data() + at, from + static_cast<size_t>(i) * components * 4, components * 4);
        }
    }
}

bool InternalProgram(const char* vertex, const char* fragment, Program* out)
{
    std::map<std::string, u32> samplerBindings;
    std::string vertexSource = Translate(vertex, true, samplerBindings, {}, &out->inputs);
    std::string fragmentSource = Translate(fragment, false, samplerBindings, {}, nullptr);
    std::vector<u32> vertexSpirv;
    std::vector<u32> fragmentSpirv;
    if (!CompileStages(vertexSource, fragmentSource, &vertexSpirv, &fragmentSpirv, &out->log, out))
    {
        Log("an internal program didn't compile: %s", out->log.c_str());
        return false;
    }

    for (const auto& sampler : samplerBindings)
    {
        bool listed = false;
        for (Uniform& u : out->uniforms)
        {
            listed = listed || (u.sampler && u.name == sampler.first);
        }

        if (!listed)
        {
            Uniform u;
            u.name = sampler.first;
            u.sampler = true;
            u.binding = sampler.second;
            out->uniforms.push_back(u);
        }
    }

    if (!MakeLayout(*out, samplerBindings))
    {
        return false;
    }

    out->vertex = Module(vertexSpirv);
    out->fragment = Module(fragmentSpirv);
    out->linked = true;
    return true;
}
}
