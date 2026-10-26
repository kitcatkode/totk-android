# NVN on Vulkan: the plan

`android/gfx/nvn_vk.c` implements the replay interface `third_party/d2r_gfx/gfx/nvn/nvn_mtl.h`. The NVN layer
(`hle_nvn.c`) records the game's command buffers and hands them over. On the Mac, `nvn_mtl.m` (14k lines) plays them
on Metal. This file is its Vulkan counterpart, built in stages. Each stage runs the real game before the next one starts.

## What the AYN Thor's drivers offer (android/vkprobe)

| | Qualcomm (stock, Dec 2023) | Turnip (Mesa 26.3, Balemuni build) |
| --- | --- | --- |
| Vulkan | 1.3.128 | 1.4.359 |
| Host-pointer import (`VK_EXT_external_memory_host`) | no | no |
| dma-buf export, mapped at a fixed address, GPU round trip | (rerun the probe) | **works** |
| dma-buf heap import | (rerun the probe) | **works** |
| BC textures | yes | yes |
| shaderInt64 / shaderFloat64 | no / no | yes / no |
| extended dynamic state 3, conditional rendering | no | yes |
| layer / viewport index output from the vertex stage | no | yes |

Turnip is the main target. The stock driver should work too, at some cost in pipeline variants, if its dma-buf path
passes. `TOTK_VK_DRIVER` (or `driver.so` next to `run.sh`) picks the driver file. `android/gfx/vk_load.c` loads ICDs,
plain libraries and Android HAL modules (the format the emulator driver packs ship in).

## Memory: zero-copy pools without host-pointer import

The game reads and writes NVN memory pools through its own pointers, so the GPU has to use the same pages. Metal and
`VK_EXT_external_memory_host` wrap the existing pages. The Adreno drivers can do neither, so the backend works the
other way round. When a pool is registered, the backend allocates device memory exported as a dma-buf, copies the
pool's bytes into it, and `mmap`s the dma-buf with `MAP_FIXED` over the pool's own address range. The game keeps its
pointers, and the GPU and CPU now share those pages. When the pool is finalized, the bytes are moved back into
ordinary shared memory with one `mremap`.

Virtual pools (`totk_vpool.c`) alias physical pool pages. For a dma-buf pool that alias is a second `mmap` of the same
dma-buf at an offset (`mtl_pool_alias`), because `mremap` cannot duplicate a dma-buf mapping. A GPU address inside a
virtual pool resolves through the alias table to the physical pool's buffer.

`android/gfx/test_vk.c` checks all of this without the game: contents kept both ways, GPU copies and uniform updates
in order, CPU steps, events and counters after completion, many submissions in flight, aliases.

## Stages

1. **Done: the CPU-visible half on the GPU.** Pools as above. Buffer copies and uniform updates are recorded into
   Vulkan command buffers in command order. Event values, counter reports and fences are applied by a completion
   thread only after the timeline semaphore reaches the submission, as on Metal. Work on addresses outside every pool
   becomes a "CPU step": the GPU finishes what came before, then the CPU does it. Draws are counted and skipped.
2. **Written, checked on lavapipe: shaders, textures, draws.** `vk_shader.c`, `vk_res.c` and `vk_draw.c` implement
   stages 2-4 below in a first, plain form (no queries yet, coarse barriers, every image in GENERAL layout).
   `test_vkdraw.c` draws through the whole backend (seed lookup by NVN key, block-linear upload, sampler, uniforms,
   vertex buffer, clear, Y orientation, re-upload after a guest write) and is clean under the Khronos validation
   layer. Missing variants are requested in `shader-requests/` and filled from the PC with `android/fill_shaders.py`.
   Frames can be saved as PNG with `TOTK_VK_SHOT=<every n-th frame>` (into `shots/`).

   The plan the code follows:

   **Shaders.** `tools/totk_xlate` already produces Vulkan SPIR-V (Ryujinx `TargetApi.Vulkan`); the Mac then
   converts it to MSL. Android stops at the SPIR-V and keeps the translator's reflection JSON for resource layouts. The
   translator is a .NET NativeAOT program. During development it runs on the PC and the device reaches it through
   `adb reverse`. For players, either publish it for `linux-bionic-arm64` or ship a pre-translated cache for the game's
   known shader list.
   **Textures and render targets.** `tex_t` → `VkImage` plus views. Block-linear deswizzle on upload (`gfx/tex`, then
   a compute version). BC is native on both drivers. Samplers come from the NVN sampler pool, with custom border
   colours. Texture copies (B2T, T2B, T2T), clears, and render targets as dynamic rendering. Textures in pools need the
   same "pool contents changed → re-upload" tracking the Metal backend has (`nvn_dirty.c`). Write-protect in large
   runs, because Linux has a per-process mapping limit (`vm.max_map_count`).
   **Pipelines and draws.** Hash the NVN state into `VkPipeline`s, with dynamic state where the driver has it.
   Descriptor sets follow the Ryujinx layout, through push descriptors. Uniform snapshots at submit
   (`mtl_sub_capture_ubos`), because the game rewrites uniforms right after submitting. Vertex streams, index buffers,
   indirect draws, compute.
3. **Occlusion queries and conditional rendering** (next). Real sample counts; the game culls light sources by them.
4. **Picture: done (first form).** PNG frame dumps from `adb shell` (`TOTK_VK_SHOT`), and in the app ([APP.md](APP.md)) each
   presented frame is read back and written into the `ANativeWindow`. A zero-copy path (AHardwareBuffer + SurfaceControl) is later work.
5. **The APK's driver picker:** import driver zips like the emulators do (adrenotools-style namespace loading).

`nvn_mtl.m` stays the reference for NVN semantics. Most of its length is Metal-specific: tile memory, argument
buffers, MetalFX. Its comments record the game behaviours each piece exists for.
