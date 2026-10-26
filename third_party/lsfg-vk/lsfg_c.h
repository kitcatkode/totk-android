/* TOTK: a C interface to lsfg-vk's frame generation core (framegen/), built into liblsfg.so and loaded with dlopen by
   android/gfx/vk_fg.c. Every call returns 0 (or a context id >= 0) on success and -1 on failure with the reason in err.
   File descriptors passed in are owned by the library from then on (Vulkan imports take them). */
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <vulkan/vulkan_core.h>
#ifdef __cplusplus
extern "C" {
#endif
#pragma GCC visibility push(default)
/* gipa: the driver's vkGetInstanceProcAddr; device_uuid: vendorID << 32 | deviceID of the GPU to use; dll: path to
   Lossless.dll (its shaders); perf: performance mode (LSFG 3.1p); flow_scale 0.25..1 (lower is cheaper);
   generated: frames generated per real frame (multiplier - 1) */
int lsfg_init(PFN_vkGetInstanceProcAddr gipa, uint64_t device_uuid, const char *dll, int perf, float flow_scale,
              int generated, int no_fp16, char *err, size_t errn);
int lsfg_create(int in0, int in1, const int *out, int nout, uint32_t w, uint32_t h, VkFormat format, char *err, size_t errn);
/* in_sem: a sync fd of the submission that wrote the newest input; out_sems receives one sync fd per generated frame,
   signalled when it is written (Turnip on kgsl shares semaphores only as sync fds) */
int lsfg_present(int ctx, int in_sem, int *out_sems, int nout, char *err, size_t errn);
void lsfg_delete(int ctx);
void lsfg_finalize(void);   /* everything torn down (waits for its GPU work); lsfg_init may follow with other settings */
#pragma GCC visibility pop
#ifdef __cplusplus
}
#endif
