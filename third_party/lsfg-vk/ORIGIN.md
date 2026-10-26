# lsfg-vk frame generation core (vendored)

Lossless Scaling frame generation for Vulkan, from lsfg-vk **v1** (MIT, LICENSE.md here), via the GitHub mirror
github.com/LoveDoLove/lsfg-vk at commit 62f814999f754907570c7c55be74493f0ff5e0c7 (2025-10-06). lsfg-vk v2 is
CC BY-NC-ND 4.0 (no derivatives) and is not used.

What is here:
- `framegen/`: the frame generation library, changed in two places (marked TOTK):
  - `src/core/instance.cpp` takes the host's vkGetInstanceProcAddr (the app loads Turnip itself, without the Android
    Vulkan loader) when `lsfg_custom_gipa` is set;
  - semaphores cross as **sync fds** instead of opaque fds (Turnip on kgsl offers only sync fds): `Core::Semaphore`
    imports temporarily, or with fd < 0 makes a semaphore `exportSyncFd` hands out; `presentContext`'s output list is
    an output parameter that receives one sync fd per generated frame after it is submitted (v3.1 and v3.1p).
- `extract/dll.{cpp,hpp}`: the Lossless.dll resource parser (from lsfg-vk's `src/extract/`), unchanged.
- `lsfg_c.{h,cpp}` (TOTK): a C interface, the shader name table from lsfg-vk's `src/extract/extract.cpp`.

Built into `liblsfg.so` by android/Makefile; android/gfx/vk_fg.c loads it with dlopen when TOTK_LSFG is set.
The shaders come at run time from the player's own Lossless.dll (Lossless Scaling, Steam); none are in this repository.
