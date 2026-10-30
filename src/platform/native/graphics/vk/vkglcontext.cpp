// VkGl's contexts: each one's state (vkglinternal.h's Context), its commands (a pool of its own: one thread records them), its stream
// buffer (the vertexes, uniforms and uploads its commands read) and descriptor pools, its submissions; the context current on each
// thread; the default framebuffer of a window's context
#include "vkglinternal.h"
#include "vkgl.h"

#include <algorithm>

namespace VkGl
{
u64 SubmitToQueue(VkCommandBuffer commands, VkSemaphore wait, VkPipelineStageFlags waitStage, VkSemaphore signal);
void PresentImage(Context& c, Texture& source);
void WindowSize(int* width, int* height);

namespace
{
thread_local Context* t_Current = nullptr;

// A context's stream: chunks of host visible coherent memory, the one written and its place; each chunk handed out in a recording is
// that recording's until its submission is done
constexpr VkDeviceSize ChunkSize = 8ull * 1024 * 1024;

struct Chunk
{
    VkBuffer buffer = VK_NULL_HANDLE;
    Allocation memory;
    VkDeviceSize size = 0;
    VkDeviceSize used = 0;
    // The submission that last read it (0: being written)
    u64 serial = 0;
};

struct Stream
{
    std::vector<std::unique_ptr<Chunk>> chunks;
    Chunk* current = nullptr;
    std::vector<Chunk*> inRecording;
};

std::mutex g_StreamsLock;
std::unordered_map<const Context*, Stream> g_Streams;

bool MakeChunk(VkDeviceSize size, Chunk* chunk)
{
    Device& d = GetDevice();
    VkBufferCreateInfo info = {};
    info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    info.size = size;
    info.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                 VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(d.device, &info, nullptr, &chunk->buffer) != VK_SUCCESS)
    {
        return false;
    }

    VkMemoryRequirements needs;
    vkGetBufferMemoryRequirements(d.device, chunk->buffer, &needs);
    if (!Allocate(needs, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &chunk->memory))
    {
        vkDestroyBuffer(d.device, chunk->buffer, nullptr);
        chunk->buffer = VK_NULL_HANDLE;
        return false;
    }

    vkBindBufferMemory(d.device, chunk->buffer, chunk->memory.memory, chunk->memory.offset);
    chunk->size = size;
    return true;
}

// The descriptor pools' sizes: plenty of sets, each a uniform block and a few samplers
VkDescriptorPool MakeDescriptorPool()
{
    VkDescriptorPoolSize sizes[] = {
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 2048},
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 8192},
    };
    VkDescriptorPoolCreateInfo info = {};
    info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    info.maxSets = 2048;
    info.poolSizeCount = 2;
    info.pPoolSizes = sizes;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    vkCreateDescriptorPool(GetDevice().device, &info, nullptr, &pool);
    return pool;
}
}

StreamPiece StreamAllocate(Context& c, VkDeviceSize size, VkDeviceSize alignment)
{
    std::lock_guard lock(g_StreamsLock);
    Stream& s = g_Streams[&c];
    alignment = std::max<VkDeviceSize>(alignment, 16);
    auto fits = [&](Chunk* chunk)
    {
        const VkDeviceSize at = (chunk->used + alignment - 1) / alignment * alignment;
        return at + size <= chunk->size;
    };
    if (s.current == nullptr || !fits(s.current))
    {
        // A chunk no submission still reads (and big enough), else a new one
        const u64 done = CompletedSerial();
        Chunk* found = nullptr;
        for (auto& chunk : s.chunks)
        {
            if (chunk->serial != 0 && chunk->serial <= done && chunk->size >= size &&
                std::find(s.inRecording.begin(), s.inRecording.end(), chunk.get()) == s.inRecording.end())
            {
                found = chunk.get();
                break;
            }
        }

        if (found == nullptr)
        {
            auto chunk = std::make_unique<Chunk>();
            if (!MakeChunk(std::max(ChunkSize, size + alignment), chunk.get()))
            {
                Log("the stream buffer couldn't grow");
                return {};
            }

            found = chunk.get();
            s.chunks.push_back(std::move(chunk));
        }

        found->used = 0;
        found->serial = 0;
        s.current = found;
        s.inRecording.push_back(found);
    }

    Chunk* chunk = s.current;
    const VkDeviceSize at = (chunk->used + alignment - 1) / alignment * alignment;
    chunk->used = at + size;
    StreamPiece piece;
    piece.buffer = chunk->buffer;
    piece.offset = at;
    piece.mapped = chunk->memory.mapped + at;
    return piece;
}

namespace
{
void StreamDone(Context& c, u64 serial)
{
    std::lock_guard lock(g_StreamsLock);
    Stream& s = g_Streams[&c];
    for (Chunk* chunk : s.inRecording)
    {
        chunk->serial = serial;
    }

    s.inRecording.clear();
    s.current = nullptr;
}
}

Context* Current()
{
    return t_Current;
}

VkCommandBuffer Commands(Context& c)
{
    if (c.commands != VK_NULL_HANDLE)
    {
        return c.commands;
    }

    Device& d = GetDevice();
    if (c.pool == VK_NULL_HANDLE)
    {
        VkCommandPoolCreateInfo info = {};
        info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        info.queueFamilyIndex = d.queueFamily;
        vkCreateCommandPool(d.device, &info, nullptr, &c.pool);
    }

    // A command buffer whose submission is done, else a new one
    const u64 done = CompletedSerial();
    for (size_t i = 0; i < c.spareCommands.size(); i++)
    {
        if (c.spareCommands[i].first <= done)
        {
            c.commands = c.spareCommands[i].second;
            c.spareCommands.erase(c.spareCommands.begin() + static_cast<std::ptrdiff_t>(i));
            break;
        }
    }

    if (c.commands == VK_NULL_HANDLE)
    {
        VkCommandBufferAllocateInfo info = {};
        info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        info.commandPool = c.pool;
        info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        info.commandBufferCount = 1;
        vkAllocateCommandBuffers(d.device, &info, &c.commands);
    }

    VkCommandBufferBeginInfo begin = {};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(c.commands, &begin);
    c.rendering = Rendering{};
    c.boundPipeline = VK_NULL_HANDLE;
    c.dynamicKnown = false;
    c.draws = 0;
    // What every submission before wrote, seen by this one (the other contexts' too: GL's fences ordered by the one queue)
    VkMemoryBarrier barrier = {};
    barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    barrier.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    vkCmdPipelineBarrier(c.commands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &barrier, 0, nullptr,
                         0, nullptr);
    return c.commands;
}

VkDescriptorSet AllocateSet(Context& c, VkDescriptorSetLayout layout)
{
    Device& d = GetDevice();
    for (int attempt = 0; attempt < 2; attempt++)
    {
        if (c.descriptorPool == VK_NULL_HANDLE)
        {
            // A pool whose submission is done (reset), else a new one
            const u64 done = CompletedSerial();
            for (size_t i = 0; i < c.descriptorPools.size(); i++)
            {
                if (c.descriptorPools[i].first <= done)
                {
                    c.descriptorPool = c.descriptorPools[i].second;
                    c.descriptorPools.erase(c.descriptorPools.begin() + static_cast<std::ptrdiff_t>(i));
                    vkResetDescriptorPool(d.device, c.descriptorPool, 0);
                    break;
                }
            }

            if (c.descriptorPool == VK_NULL_HANDLE)
            {
                c.descriptorPool = MakeDescriptorPool();
            }
        }

        VkDescriptorSetAllocateInfo info = {};
        info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        info.descriptorPool = c.descriptorPool;
        info.descriptorSetCount = 1;
        info.pSetLayouts = &layout;
        VkDescriptorSet set = VK_NULL_HANDLE;
        if (vkAllocateDescriptorSets(d.device, &info, &set) == VK_SUCCESS)
        {
            return set;
        }

        // Full: retired with the recording (it's reset once the submission is done)
        c.descriptorPools.emplace_back(~0ull, c.descriptorPool);
        c.descriptorPool = VK_NULL_HANDLE;
    }

    Log("no descriptor set");
    return VK_NULL_HANDLE;
}

void EndRendering(Context& c)
{
    if (c.rendering.active)
    {
        if (GetDevice().renderPasses)
        {
            vkCmdEndRenderPass(c.commands);
        }
        else
        {
            vkCmdEndRendering(c.commands);
        }

        c.rendering.active = false;
    }
}

void Barrier(Context& c)
{
    VkCommandBuffer commands = Commands(c);
    EndRendering(c);
    VkMemoryBarrier barrier = {};
    barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    barrier.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &barrier, 0, nullptr, 0,
                         nullptr);
}

namespace
{
// The recording ended and submitted (waiting and signalling semaphores when given): its serial, its pieces and pools kept until then
u64 Finish(Context& c, VkSemaphore wait, VkPipelineStageFlags waitStage, VkSemaphore signal)
{
    EndRendering(c);
    vkEndCommandBuffer(c.commands);
    const u64 serial = SubmitToQueue(c.commands, wait, waitStage, signal);
    c.spareCommands.emplace_back(serial, c.commands);
    c.commands = VK_NULL_HANDLE;
    StreamDone(c, serial);
    for (auto& pool : c.descriptorPools)
    {
        if (pool.first == ~0ull)
        {
            pool.first = serial;
        }
    }

    // The current pool is this recording's too: retired with it (the next recording takes one that's done)
    if (c.descriptorPool != VK_NULL_HANDLE)
    {
        c.descriptorPools.emplace_back(serial, c.descriptorPool);
        c.descriptorPool = VK_NULL_HANDLE;
    }

    RunFinishedWork();
    return serial;
}

u64 g_LastSerialOf = 0;
}

u64 Submit(Context& c, bool wait)
{
    u64 serial = 0;
    if (c.commands != VK_NULL_HANDLE)
    {
        serial = Finish(c, VK_NULL_HANDLE, 0, VK_NULL_HANDLE);
    }
    else
    {
        serial = GetDevice().submitted.load();
    }

    if (wait && serial != 0)
    {
        WaitSerial(serial);
        RunFinishedWork();
    }

    (void)g_LastSerialOf;
    return serial;
}

u64 SubmitWith(Context& c, VkSemaphore wait, VkPipelineStageFlags waitStage, VkSemaphore signal)
{
    Commands(c);
    return Finish(c, wait, waitStage, signal);
}

Framebuffer* DrawFramebuffer(Context& c)
{
    if (c.drawFramebuffer != 0)
    {
        return FindFramebuffer(c.drawFramebuffer);
    }

    if (c.window == nullptr)
    {
        return nullptr;
    }

    // The window's: its image as big as the window (made again when it isn't)
    int width = 0;
    int height = 0;
    SDL_GetWindowSizeInPixels(c.window, &width, &height);
    width = std::max(1, width);
    height = std::max(1, height);
    Objects& o = GetObjects();
    Texture* colour = c.windowColour != 0 ? FindTexture(c.windowColour) : nullptr;
    if (colour == nullptr || colour->width != static_cast<u32>(width) || colour->height != static_cast<u32>(height))
    {
        if (colour == nullptr)
        {
            std::lock_guard lock(o.lock);
            c.windowColour = o.next++;
            auto made = std::make_unique<Texture>();
            made->name = c.windowColour;
            colour = made.get();
            o.textures[c.windowColour] = std::move(made);
        }

        colour->internal = Gl::RGBA8;
        colour->given.assign(1, {static_cast<u32>(width), static_cast<u32>(height)});
        EnsureImage(*colour);
    }

    Framebuffer* framebuffer = c.windowFramebuffer != 0 ? FindFramebuffer(c.windowFramebuffer) : nullptr;
    if (framebuffer == nullptr)
    {
        std::lock_guard lock(o.lock);
        c.windowFramebuffer = o.next++;
        auto made = std::make_unique<Framebuffer>();
        made->name = c.windowFramebuffer;
        made->colour[0].texture = c.windowColour;
        framebuffer = made.get();
        o.framebuffers[c.windowFramebuffer] = std::move(made);
    }

    return framebuffer;
}

Framebuffer* ReadFramebuffer(Context& c)
{
    if (c.readFramebuffer != 0)
    {
        return FindFramebuffer(c.readFramebuffer);
    }

    GLuint saved = c.drawFramebuffer;
    c.drawFramebuffer = 0;
    Framebuffer* framebuffer = DrawFramebuffer(c);
    c.drawFramebuffer = saved;
    return framebuffer;
}

void* CreateContext(SDL_Window* window)
{
    if (!Started())
    {
        return nullptr;
    }

    auto* c = new Context();
    c->window = window;
    return c;
}

bool MakeCurrent(void* context)
{
    t_Current = static_cast<Context*>(context);
    return true;
}

void* CurrentContext()
{
    return t_Current;
}

void DestroyContext(void* context)
{
    auto* c = static_cast<Context*>(context);
    if (c == nullptr)
    {
        return;
    }

    if (c->commands != VK_NULL_HANDLE)
    {
        Submit(*c, true);
    }

    Device& d = GetDevice();
    vkDeviceWaitIdle(d.device);
    for (auto& pool : c->descriptorPools)
    {
        vkDestroyDescriptorPool(d.device, pool.second, nullptr);
    }

    if (c->descriptorPool != VK_NULL_HANDLE)
    {
        vkDestroyDescriptorPool(d.device, c->descriptorPool, nullptr);
    }

    if (c->pool != VK_NULL_HANDLE)
    {
        vkDestroyCommandPool(d.device, c->pool, nullptr);
    }

    {
        std::lock_guard lock(g_StreamsLock);
        auto found = g_Streams.find(c);
        if (found != g_Streams.end())
        {
            for (auto& chunk : found->second.chunks)
            {
                vkDestroyBuffer(d.device, chunk->buffer, nullptr);
                Free(chunk->memory);
            }

            g_Streams.erase(found);
        }
    }

    if (t_Current == c)
    {
        t_Current = nullptr;
    }

    delete c;
}

void SwapWindow(SDL_Window* window)
{
    Context* c = t_Current;
    if (c == nullptr || c->window == nullptr || window == nullptr)
    {
        return;
    }

    Framebuffer* framebuffer = DrawFramebuffer(*c);
    Texture* colour = framebuffer != nullptr ? FindTexture(framebuffer->colour[0].texture) : nullptr;
    if (colour == nullptr || colour->image == VK_NULL_HANDLE)
    {
        Submit(*c, false);
        return;
    }

    PresentImage(*c, *colour);
}
}
