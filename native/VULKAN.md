# Vulkan for the native graphics (a branch: `vulkan`)

Why: on phones the frame's limits are the render thread's CPU and the GPU's passes, not the pixels. Measured on a Poco X7 Pro
(Mali-G720, ANGLE on Vulkan) at 3x in the heaviest scenes (3.2-3.4 M primitives a second): the render thread at 18 ms a frame,
of which `HwGs::Flush` is ~4.7 ms (the submission of ~160 batches and ~74 passes, ~3 ms of it inside ANGLE and the driver);
the GPU at 12-16 ms, of which ~4 ms are the `target to memory` and `texture decode` passes (fixed in cost, whatever the
resolution) and the rest the draws, paying a tile load and store at every one of the 74 passes. Mali's own OpenGL ES driver was
tried before ANGLE and was worse. The GL path has been squeezed where it could (the GIF's packets decoded on the VU stage,
`WriteImage`'s whole pages, cached uniforms, the fast draw path): what is left is the structure of the GL path on a tiled GPU.

What Vulkan gives, in order of what it is worth here:

1. No translation: the ~3 ms a frame ANGLE spends turning GL calls into Vulkan ones go. The GL path's per-batch work
   (`Flush`: program, uniforms, textures, blend state) becomes a pipeline handle, a descriptor set and a few dynamic states.
2. Passes under our control. A frame's "CPU writes to memory → memory to target → target copy → draw" cycles (six a frame
   into the 16 bit buffers at 0x2800 and 0x3000) and the "target to memory → decode → draw" ones (four a frame from 0x2c00)
   become subpasses of one render pass each, and the tile traffic of 74 passes becomes that of a dozen.
3. The shader blend without a copy: a frame of 16 bits, or a blend GL can't make exactly, reads its own pixel as an input
   attachment. With `VK_ARM_rasterization_order_attachment_access` (every Mali from the G77) or `VK_EXT_...`, the read is
   ordered and exact, which is what `GL_EXT_shader_framebuffer_fetch` promised and ANGLE didn't deliver (drew wrong, and slower:
   hwgs.cpp's note at its copies).
4. Pipelines made ahead: the 32 ms hitches ANGLE gives when a new shader variant appears mid-game (its pipeline compiled then)
   become a warm-up at the start, from a cache written to the settings folder.
5. `VK_EXT_load_store_op_none` / `DONT_CARE` for what isn't read back: the depth of a pass whose depth is never read again isn't
   stored.

## The path taken first: VkGl (OpenGL in Vulkan)

Rather than a second GS backend written beside `HwGs`, the renderer's own GL is carried out with Vulkan: `graphics/vk/vkgl*.cpp`
(`vkgl.h` says what it is). The GL users (the hardware renderer, the presenter, the UI's overlay, the launcher) load their entry
points from `VkGl::GetProcAddress` and make their contexts with `VkGl::CreateContext` when the run uses it (`glcompat.h`'s
`NativeGl::Vulkan()`); nothing else of theirs changes, so `HwGs` stays one renderer, checked once. What it takes away is ANGLE
(point 1 below: its translation and checks), and it is where the pass control (points 2, 3, 5) goes next, since it sees every
framebuffer change and every copy.

- The parts: `vkgldevice.cpp` (the instance, device, memory blocks, submissions by serial with fences, the swapchain: the default
  framebuffer is an image of our own blitted upside down into it), `vkglcontext.cpp` (a context: its commands, its stream buffer
  of vertexes and uniforms, its descriptor pools), `vkglobjects.cpp` (textures as images, all in the GENERAL layout; samplers;
  timestamp queries), `vkglprogram.cpp` (GLSL 330 core made 450 and compiled by glslang with the relaxed Vulkan rules: the loose
  uniforms in one block, read back by reflection), `vkgldraw.cpp` (dynamic rendering, pipelines by state with the depth, stencil,
  viewport and scissor dynamic, clears, uploads, reads, blits as draws), `vkglentry.cpp` (the GL entry points).
- Chosen by the Display page's "renderer" (options.txt's `renderer=1`) or `$TWIN_RENDERER=vulkan`, at the start: the window is
  made with `SDL_WINDOW_VULKAN`; when Vulkan doesn't start the window is made again for OpenGL (the log says which it is).
- Checked: `vktest` (a GL program run on Mesa's GL and on VkGl over lavapipe: textures, blending, depth, stencil, dual-source, an
  integer target read back, a flipped linear blit, a sub-image upload with a row length, mipmaps by LOD, the default
  framebuffer) is pixel-identical, with the validation layer silent; the harness's replays run through it
  (`TWIN_RENDERER=vulkan graphics-harness OUT --replay FILE --hardware 1`).
- Built with glslang 15.1.0 (static, no optimizer, no HLSL): `build_android.py` downloads and builds it into
  `build/android/deps`; on Linux, glslang-dev.

## The stages (the native backend, if VkGl isn't enough)

Each stage is checked against the software GS with the harness's replays (`--replay FILE --hardware 1`, native/GRAPHICS.md's
"The hardware renderer"): the picture out of the Vulkan path compared pixel by pixel as the GL path's is. The GL path stays
until the Vulkan one is its equal everywhere; `TWIN_GS=vulkan` (an option later) picks it.

| Stage | What | Checked by |
|---|---|---|
| 1 (here) | `graphics/vk/vkdevice.cpp`: the loader through SDL (no library linked), the instance, the device and queue, the probe of what the GPU offers, a swapchain cleared and presented | `--selftest-vulkan` on a PC (the probe's report in the log, 120 frames blue to orange); `vulkan_probe.txt` in the settings folder on Android (the report in the log before the game starts as usual) |
| 2 | The GS's memory on the GPU: the 1024x1024 R32_UINT memory image, the layouts' tables (`hwgs.cpp`'s `fwdTables_` / `invTables_`), the CPU's page uploads (a staging buffer a frame, `vkCmdCopyBufferToImage` by runs of pages), the targets as images with their framebuffers; `MemFromTarget` / `TargetFromMem` as the same shaders (GLSL compiled to SPIR-V at build time by glslangValidator, embedded; `UnitAddress` / `UnitPixel` unchanged) | A replay's memory read back after each conversion against the GL path's |
| 3 | The draws: `ProgramFor(bits)` becomes a pipeline per key (specialisation constants for what the `#define`s choose where the shader's declarations don't change, separate SPIR-V where they do: the dual-source output, the input attachment), the vertex buffer streamed (`HwVertex` as it is), depth/stencil (D32_SFLOAT_S8_UINT; the stencil DATE pass), dual-source blending (`dualSrcBlend` is there on Mali), the alpha test's AFAIL second pass, the wrapping additive sums into R16G16B16A16_SFLOAT | The replays' pictures against the software GS: the beach and the flyover first, as the GL path was |
| 4 | Texture decodes (`DecodePasses`: the memory image and the CLUT image through the same shaders), the replacement textures, the content cache, the display read-out and the presenter (the picture to the swapchain, the UI's overlay drawn on it: `ui/overlayrender.cpp`'s quads as one pipeline) | The game from the title screen on; the level sweep's snapshots |
| 5 | The gains: passes merged (subpasses, input attachments in place of `CopyTarget`), `rasterization_order_attachment_access` for the shader blend, pipeline cache on disk, `LOAD_OP_DONT_CARE` / `STORE_OP_NONE` where a pass's attachment isn't read | The frame statistics (`passes:` and `frame:` lines): the render thread and the GPU below 12 ms in the heaviest scenes |

## Where things are

- `src/platform/native/graphics/vk/`: everything Vulkan. `vkdevice.{h,cpp}` (stage 1). The backend of the GS (`vkgs`) and the
  presenter come as the stages do; `Gs::Backend` (`gs/backend.h`) is the interface they implement, as `HwGs` does.
- `native/CMakeLists.txt`: `TWIN_VULKAN` (Android's default; needs `<vulkan/vulkan.h>`: the NDK's sysroot, Linux's
  libvulkan-dev, and for Windows' LLVM-MinGW a copy of the Vulkan-Headers would go in `native/third_party`). `VK_NO_PROTOTYPES`:
  every function is loaded through `vkGetInstanceProcAddr` / `vkGetDeviceProcAddr` (`vkdevice.cpp`'s X-macro lists).
- The shaders: `hwgs.cpp`'s GLSL sources, with `#version 450` in place of `330 core` and the bindings laid out (`layout(set, binding)`),
  compiled by `glslangValidator -V` in the build (stage 2 adds the CMake rule and `native/tools/embed_spirv.py`).

## What the probe found (Poco X7 Pro, Mali-G720 MC7, driver 49.1.0, Vulkan 1.3.278)

Everything the design counts on is there: dual-source and independent blending, fragment stores and atomics, int64,
`VK_ARM_rasterization_order_attachment_access` (and the EXT), `VK_KHR_dynamic_rendering`, `VK_EXT_load_store_op_none`,
`VK_EXT_provoking_vertex`, timeline semaphores, host query reset, timestamps (76.9 ns a tick), and all seven formats the
renderer uses (R8G8B8A8_UNORM, D32_SFLOAT_S8_UINT, D32_SFLOAT, R32_UINT, R32_SFLOAT, R16G16B16A16_SFLOAT, R32G32B32A32_UINT)
with every feature wanted of them. Lacking: `VK_EXT_shader_stencil_export` (not needed: the stencil DATE is a stencil pass, as in
GL) and `VK_EXT_depth_range_unrestricted` (not needed: the depth is in [0, 1] by construction). Limits: 8 colour attachments,
8 dual-source attachments, 9 input attachments, 256 bytes of push constants. So the stages below assume dynamic rendering,
rasterization-order attachment reads for the shader blend, and load/store op none; a GPU without them (an older Mali, an
Adreno) gets the render-pass-object path and the copies, as GL has them, when the time comes.

## Decisions taken

- One queue (graphics + present): the Mali has one family; the transfers go in the same command buffers, ordered by barriers.
- Memory: a simple allocator of our own by heap (a few large `VkDeviceMemory` blocks sub-allocated: the targets and the memory
  image are few and long-lived; the staging buffers ring). No VMA: one more dependency isn't worth it for this set.
- Synchronisation between the render thread and the GPU: one fence a frame, three frames in flight; the readbacks the CPU side
  needs (`SyncCpu`: the display read, the software fallbacks) wait on the frame's fence as the GL path waits on `glReadPixels`.
- Timer queries: `VK_QUERY_TYPE_TIMESTAMP` per pass, read a frame later, for the `passes:` statistics as the GL path has them.
