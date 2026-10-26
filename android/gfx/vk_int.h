/* Internals shared by the Vulkan backend's files:
     nvn_vk.c     device, memory pools, submissions and their completion, the command walker
     vk_res.c     textures (images, uploads from guest memory, views), samplers, memory sub-allocation
     vk_shader.c  the shader seed (variants, SPIR-V, reflection), programs, pipelines
     vk_draw.c    draw state, render passes, draws, dispatches, copies, clears, present */
#pragma once
#include "vk_load.h"
#include "vk_fmt.h"
#include "nvn_obj.h"
#include "nvn_mtl.h"
#include "nvn_tex.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

uint64_t host_now_ns(void);
#define LOGV(...) do { fprintf(stderr, "[nvn-vk] " __VA_ARGS__); fputc('\n', stderr); } while (0)
#define DIE(...) do { LOGV(__VA_ARGS__); fflush(stderr); abort(); } while (0)
#define VKCHECK(call) do { VkResult r_ = (call); if (r_) DIE("%s failed: %d", #call, r_); } while (0)
/* a red line the first few times (the Mac's RED_ONCE) */
#define RED(n, ...) do { static int said_; if (said_++ < (n)) LOGV("【红】" __VA_ARGS__); } while (0)

/* Every device-level entry point the backend calls, loaded once (nvn_vk.c). */
#define VK_DEVICE_FUNCS(X) \
    X(vkGetDeviceQueue) X(vkDeviceWaitIdle) X(vkQueueSubmit2) X(vkQueueWaitIdle) \
    X(vkCreateBuffer) X(vkDestroyBuffer) X(vkGetBufferMemoryRequirements) X(vkBindBufferMemory) \
    X(vkCreateImage) X(vkDestroyImage) X(vkGetImageMemoryRequirements) X(vkBindImageMemory) X(vkGetImageSubresourceLayout) \
    X(vkCreateImageView) X(vkDestroyImageView) X(vkCreateBufferView) X(vkDestroyBufferView) \
    X(vkCreateSampler) X(vkDestroySampler) \
    X(vkAllocateMemory) X(vkFreeMemory) X(vkMapMemory) X(vkUnmapMemory) X(vkInvalidateMappedMemoryRanges) \
    X(vkGetMemoryFdKHR) X(vkGetMemoryFdPropertiesKHR) X(vkGetMemoryHostPointerPropertiesEXT) \
    X(vkCreateCommandPool) X(vkDestroyCommandPool) X(vkResetCommandPool) X(vkAllocateCommandBuffers) \
    X(vkBeginCommandBuffer) X(vkEndCommandBuffer) \
    X(vkCreateSemaphore) X(vkDestroySemaphore) X(vkGetSemaphoreFdKHR) X(vkImportSemaphoreFdKHR) X(vkWaitSemaphores) X(vkGetSemaphoreCounterValue) X(vkCreateFence) X(vkWaitForFences) X(vkResetFences) \
    X(vkCreateShaderModule) X(vkDestroyShaderModule) X(vkCreatePipelineCache) X(vkGetPipelineCacheData) \
    X(vkCreateGraphicsPipelines) X(vkCreateComputePipelines) X(vkDestroyPipeline) \
    X(vkCreatePipelineLayout) X(vkDestroyPipelineLayout) X(vkCreateDescriptorSetLayout) X(vkDestroyDescriptorSetLayout) \
    X(vkCreateDescriptorPool) X(vkDestroyDescriptorPool) X(vkResetDescriptorPool) X(vkAllocateDescriptorSets) X(vkUpdateDescriptorSets) \
    X(vkCreateQueryPool) X(vkDestroyQueryPool) X(vkCmdResetQueryPool) X(vkCmdBeginQuery) X(vkCmdEndQuery) X(vkCmdCopyQueryPoolResults) X(vkCmdWriteTimestamp2) X(vkGetQueryPoolResults) \
    X(vkCmdPushDescriptorSetKHR) X(vkCmdPipelineBarrier2) X(vkCmdCopyBuffer) X(vkCmdUpdateBuffer) X(vkCmdFillBuffer) \
    X(vkCmdCopyBufferToImage) X(vkCmdCopyImageToBuffer) X(vkCmdCopyImage) X(vkCmdBlitImage) \
    X(vkCmdClearColorImage) X(vkCmdClearDepthStencilImage) X(vkCmdClearAttachments) \
    X(vkCmdBeginRendering) X(vkCmdEndRendering) X(vkCmdBindPipeline) X(vkCmdBindDescriptorSets) \
    X(vkCmdBindVertexBuffers2) X(vkCmdBindIndexBuffer) X(vkCmdDraw) X(vkCmdDrawIndexed) X(vkCmdDispatch) \
    X(vkCmdSetViewport) X(vkCmdSetScissor) X(vkCmdSetDepthBias) X(vkCmdSetBlendConstants) \
    X(vkCmdSetStencilCompareMask) X(vkCmdSetStencilWriteMask) X(vkCmdSetStencilReference) \
    X(vkCmdSetCullMode) X(vkCmdSetFrontFace) X(vkCmdSetPrimitiveTopology) X(vkCmdSetDepthTestEnable) \
    X(vkCmdSetDepthWriteEnable) X(vkCmdSetDepthCompareOp) X(vkCmdSetStencilTestEnable) X(vkCmdSetStencilOp) \
    X(vkCmdSetDepthBiasEnable) X(vkCmdSetPrimitiveRestartEnable) X(vkCmdSetRasterizerDiscardEnable) X(vkCmdSetLineWidth)
#define VK_DECL(name) extern PFN_##name name;
VK_DEVICE_FUNCS(VK_DECL)
#undef VK_DECL

/* ---------- device (nvn_vk.c) ---------- */
typedef struct {
    VkInstance inst; VkPhysicalDevice pd; VkDevice dev; VkQueue queue; uint32_t qfam;
    VkPhysicalDeviceProperties props; VkPhysicalDeviceMemoryProperties mp;
    int has_custom_border, has_null_descriptor, has_index_u8, has_depth_clip_control, has_eds3, has_divisor, has_bc;
    int has_geometry, has_layer_output, has_logic_op, has_dual_src;
    uint32_t max_push;   /* VK_KHR_push_descriptor's maxPushDescriptors (0: not there) */
    VkSampleCountFlags samples;
    VkPipelineCache pcache;
    PFN_vkGetPhysicalDeviceFormatProperties get_format_props;
    PFN_vkGetPhysicalDeviceFormatProperties2 get_format_props2;
    PFN_vkGetPhysicalDeviceImageFormatProperties get_image_format_props;
} vk_dev;
extern vk_dev V;
void mtl_init(void);
int vk_pick_type(uint32_t bits, VkMemoryPropertyFlags want);

/* Which pool buffer holds [addr, addr + len): 1 with the buffer and offset (nvn_vk.c). */
int vk_resolve(uint64_t addr, uint64_t len, VkBuffer *buf, uint64_t *off);
void vk_ub_touch(uint64_t addr, uint64_t n);   /* a non-uniform GPU access of [addr, +n): logged uniform updates there reach memory first */
void vk_ub_flush(void);
void vk_gpu_write(uint64_t addr, uint64_t n);   /* the GPU writes [addr, +n) in this submission: no uniform snapshot of it (now or while in flight) */
/* TOTK_VK_OFF=<name>,<name>,...: turn individual optimisations off (to find which one misbehaves). Names: rename (uniform
   renaming), ds (descriptor-set reuse), dyn (dynamic-state skipping), block (driver bank / c1 reuse), ts (GPU
   timestamps), span (pool index; off = the old linear scan), all. */
int vk_off(const char *name);
/* One thread records at a time: the replay thread holds this from mtl_sub_begin to mtl_sub_commit, and game threads
   take it for texel reads / writes (nvnTextureReadTexels / WriteTexels run on the game's threads while the replay
   thread may be recording the next submission). Recursive. */
void vk_rec_lock(void);
uint64_t vk_submit_split(void);   /* a split of a long replay (vk_draw.c split_maybe); deferred with TOTK_VK_SPLIT_DEFER=1 */
uint64_t vk_submit_now(void);   /* submit what is recorded without waiting; its fence (the last one if nothing was recorded) */
void vk_sub_signal(VkSemaphore s);   /* a binary semaphore the next submission signals / waits for (frame generation, vk_fg.c) */
void vk_sub_wait(VkSemaphore s);
PFN_vkGetInstanceProcAddr vk_gipa(void);   /* the driver's entry point (vk_fg.c hands it to lsfg-vk) */
void vk_rec_unlock(void);
void vk_ub_peek(uint64_t addr, uint64_t n, void *dst);   /* the bytes a uniform binding there shows the GPU now (diagnostics) */
int vk_ub_pending(uint64_t addr, uint64_t n);   /* does [addr, +n) overlap logged updates not yet in memory */
void vk_perf_take(double *replay_ms, double *wait_ms, double *gpu_ms, uint64_t *subs);   /* totals since the last call */                         /* all logged uniform updates reach memory (outside passes) */
int vk_ub_rename(uint64_t addr, uint64_t size, VkDescriptorBufferInfo *out);   /* a private copy of a uniform range with logged updates */
int vk_resolve_span(uint64_t addr, VkBuffer *buf, uint64_t *off, uint64_t *avail);   /* the buffer holding addr and the bytes it holds from there */

/* ---------- submissions (nvn_vk.c) ----------
   A submission records into two command buffers of its ring slot: "pre" (texture uploads, image creation barriers:
   things that must happen before the game's commands, as the Metal backend's upload batch) and "main" (the game's
   commands). Both go to the queue together, pre first. Per-slot transient memory (staging bytes, small uniform data,
   descriptor sets) is recycled when the slot's work has completed. */
VkCommandBuffer vk_cb(void);       /* the main command buffer being recorded (begins one if needed) */
VkCommandBuffer vk_pre_cb(void);   /* the pre command buffer of the same submission */
void vk_cpu_step(void);            /* the GPU finishes everything recorded so far (then the CPU may act in order) */
void vk_gpu_idle(void);            /* everything handed to the queue is done */
/* transient host-visible memory of the current submission (staging, driver banks, support buffer): pointer + buffer/offset */
void *vk_transient(size_t size, size_t align, VkBuffer *buf, VkDeviceSize *off);
/* a descriptor set for the current submission */
VkDescriptorSet vk_alloc_set(VkDescriptorSetLayout layout);
/* a full memory barrier between whatever came before and after, in the main command buffer (outside rendering) */
void vk_barrier_all(VkCommandBuffer cb);
uint64_t vk_submit_serial(void);   /* counts submissions: "once per submission" checks */
uint64_t vk_replay_serial(void);   /* counts replays (game submissions) and CPU steps: "memory changed?" checks */
void vk_flush_wait(void);          /* the backend's own work (present, CPU reads): submit what is recorded and wait */
void vk_note_draw(void);           /* statistics */
/* What a submission holds, logged if the GPU hangs on it (nvn_vk.c notes_log): its passes ('p', the target size and
   the first draw's shaders), dispatches ('d', the grid) and copies ('c'). Keys must outlive the submission. */
typedef struct { char kind; const char *k0, *k1; uint32_t a, b, c, draws; } vk_sub_note_t;
void vk_sub_note(char kind, const char *k0, const char *k1, uint32_t a, uint32_t b, uint32_t c);
void vk_sub_time(void);   /* TOTK_VK_PASSTIME: a GPU timestamp here, booked to the newest note (after a pass's or dispatch's barrier) */

/* ---------- memory (vk_res.c): device memory for images, sub-allocated from large blocks ---------- */
typedef struct { VkDeviceMemory mem; VkDeviceSize off, size; int block; } vk_alloc;
int vk_mem_alloc(const VkMemoryRequirements *req, VkMemoryPropertyFlags want, vk_alloc *out);
void vk_mem_free(vk_alloc *a);

/* ---------- textures (vk_res.c) ---------- */
typedef struct vk_tex vk_tex;
/* the Vulkan texture of an NVN texture object (created / uploaded / re-uploaded as needed), NULL if it cannot be made */
vk_tex *vk_tex_get(const tex_t *t);
/* an image view for sampling / attaching: view may be NULL (whole texture); for an attachment, one level and layer
   (layer_count layers for a layered attachment) */
VkImageView vk_tex_view(vk_tex *x, const tex_t *t, const view_t *v, int attach);
VkImage vk_tex_image(vk_tex *x);
uint64_t vk_tex_serial(vk_tex *x);
uint32_t vk_tex_samples(vk_tex *x);
VkFormat vk_tex_format(vk_tex *x);
int vk_tex_nvn_format(vk_tex *x);
uint32_t vk_tex_width(vk_tex *x, unsigned level);
uint32_t vk_tex_height(vk_tex *x, unsigned level);
uint32_t vk_tex_layers(vk_tex *x);
uint32_t vk_tex_levels(vk_tex *x);
int vk_tex_is3d(vk_tex *x);
VkImageAspectFlags vk_tex_aspect(vk_tex *x);
void vk_tex_mark_gpu_written(vk_tex *x);
void vk_res_sweep(void);
void vk_res_counters(uint64_t v[7]);   /* images made, storage-shared reuses, uploads, re-uploads, shared images swept, CPU-pool images shared, serials switched */                    /* after each commit: destroy what the GPU is done with */
void vk_tex_pool_gone(uintptr_t lo, uintptr_t hi);
void vk_wb_gpu_wrote(uint64_t a, uint64_t n);      /* GPU copies newer than noted renderings there: not written back */
void vk_wb_range_gone(uintptr_t lo, uintptr_t hi);  /* a pool's memory goes */
void vk_res_stats(void);   /* rendered / copied / cleared by the GPU: never re-uploaded from guest memory */
/* the texture registered at a texture-pool id (vk_reg), resolved to its Vulkan view; NULL for an empty slot */
VkImageView vk_tex_by_id(int tid, int *target_out, vk_tex **x_out);
VkBufferView vk_tbuf_by_id(int tid);
uint64_t vk_treg_epoch(void);   /* bumped by every texture registration (vk_shader.c stage_sig memo) */
int vk_treg_fmt(int tid);   /* the format a texture id samples as, -1 for an empty id (cached per registration) */
VkSampler vk_samp_by_id(int sid);
VkSampler vk_samp_by_id_nocmp(int sid);   /* the same sampler without depth compare */
int vk_samp_cmp(int sid);                 /* the sampler compares depth */
int vk_fmt_cmp_ok(VkFormat f);            /* the format supports depth-compare sampling */
VkImageView vk_dummy_view(int target, int is_int);   /* zero texture of a target (unbound slots) */
VkSampler vk_dummy_sampler(void);
VkBufferView vk_dummy_tbuf(void);

/* ---------- shaders and programs (vk_shader.c) ---------- */
enum { RB_SUPPORT = 1, RB_CBUF, RB_C1, RB_DRV0, RB_DRV2, RB_SBUF, RB_SBUF_UBO, RB_UBO_PTR, RB_TEX, RB_TBUF, RB_IMG, RB_SAMP, RB_TEXSAMP };
typedef struct {
    uint16_t binding, count;
    uint8_t kind;           /* RB_* */
    uint8_t dtype;          /* VkDescriptorType (fits: 0..10) */
    uint8_t target;         /* NVN texture target the shader declares (for dummies), image dim */
    uint8_t rw, is_int;
    int16_t reg, sreg;      /* NVN slot: uniform / storage / texture slot; sampler slot (RB_TEXSAMP: -1 = combined handle) */
    uint16_t off;           /* RB_SBUF_UBO: byte offset of the pointer in the uniform block */
    uint8_t shadow;         /* sampled with depth compare (SPIR-V image Depth = 1; vk_shader.c spirv_mark_shadow) */
    uint32_t used;          /* uniform buffer: bytes the shader can read from its start (constant indices only); 0 = all */
} vk_bind;
typedef struct vk_shader {
    char key[80];           /* variant name */
    int stage;              /* NVN stage 0 vertex, 1 fragment, 5 compute */
    VkShaderModule mod;
    VkDescriptorSetLayout dsl;
    int nb; vk_bind *b;
    uint8_t *c1; uint32_t c1_len;   /* the shader's own constant bank c1 (from its package) */
    uint32_t vin_mask;      /* vertex inputs read (location = NVN attribute) */
    uint8_t vin_class[32];  /* 0 float 1 uint 2 sint */
    uint32_t fout_mask;     /* fragment colour outputs written */
    int writes_depth;
    int depth_spec;         /* fragment depth exports patched: + specialization constant D16_SPEC_ID (the DEPTH16 nudge, vk_draw.c) */
    uint32_t local[3];      /* compute thread group size */
    int push;               /* its set layout is a push-descriptor layout (vk_shader.c push_layout; vk_draw.c bind_stage pushes) */
    uint64_t c1_sub; VkDescriptorBufferInfo c1_d;   /* vk_draw.c: this submission's copy of c1 */
} vk_shader;
#define D16_SPEC_ID 9001
typedef struct vk_prog vk_prog;
/* The program's shaders for the current bind state (variant per stage): 0 ok, -1 a variant is missing (draw skipped) */
int vk_prog_resolve(const prog_t *p, vk_shader *out[6]);
VkPipelineLayout vk_layout_of(vk_shader *vs, vk_shader *fs);
VkPipeline vk_compute_pipeline(vk_shader *cs, VkPipelineLayout *layout);
int vk_shaders_ready(void);
uint64_t vk_shbias_total(void);   /* shaders loaded so far whose SPIR-V carries the gather bias (the black-squares fix) */

/* ---------- hitch counters (TOTK, 2026-10-22) ----------
   Everything that can stall the game between two presents, accumulated between them and reset by mtl_present
   (vk_draw.c), which logs a breakdown on every slow frame and a histogram every 300 frames: shader and pipeline
   creation (first use mid-play), descriptor pools and transient chunks made mid-play, CPU steps and full GPU idles
   (the game's QueueFinish), pool-invalidate write-backs (a queue drain + GPU flush + CPU swizzle on the game's
   thread), and the replay / GPU-wait time of the submissions between two presents. A slow frame with all-zero
   counters is a hitch in the game's own code (streaming, allocation), not in the backend. */
typedef struct {
    _Atomic uint64_t pipes, pipes_sync, pipe_ns, pipes_fail, shloads, shbias, shbytes, vlists, sreq, dpools, chunks, cpu_steps,
                     idles, idle_ns, idle_fin, idle_flush, idle_pool, inval, inval_ns, wb, wb_bytes, subs, replay_ns, wait_ns,
                     gwaits, gwait_ns,   /* threads blocked in mtl_wait (fences, present waits): game-side stalls with async replay */
                     comp_ns, wake_ns;   /* completion thread: driver signal -> g_done (comp), completion -> waiter running (wake) */
} vk_hitch;
extern vk_hitch g_hk;
#define HK_ADD(field, n) atomic_fetch_add_explicit(&g_hk.field, (n), memory_order_relaxed)

extern pthread_mutex_t g_pipecache_mu;   /* V.pcache is externally synchronised: every pipeline creation takes this */
void vk_pcache_save_async(void);   /* the pipeline cache to disk when pipelines were added (nvn_vk.c) */

/* ---------- draw side (vk_draw.c) ---------- */
void vk_draw_exec(uint32_t op, const void *a);   /* every op the walker does not handle itself */
void vk_end_rendering(void);                     /* end the render pass if one is open (transfers, submission end) */
uint64_t vk_counter_samples(uint64_t addr);      /* samples passed for a counter report at addr (completion thread) */
/* the bound state the shader variants are chosen by (vk_shader.c reads it) */
typedef struct {
    const prog_t *prog;
    blend_st blend[8]; color_st color; chmask_st chmask; ms_st ms; poly_st poly; ds_st ds;
    va_st attr[16]; int nattr; vs_st stream[16]; int nstream;
    NVNbufferRange vbuf[16];
    p_bind_buf ubo[6][16], ssbo[6][16];
    uint64_t tex[6][128], samp[6][128], img[6][32];
    uint32_t st_vmask[2], st_wmask[2], st_ref[2];
    p_poly_offset po; float vp[16][6]; float dr[2]; int has_vp; int depth_clamp, render_disabled, raster_discard;
    float blend_color[4]; int64_t copy_stride;
    p_scissor sc; int has_sc;
    p_set_rt rt; int has_rt;
    int tex_hi[6];
} vk_state;
extern vk_state S;

/* texture-pool registry (vk_res.c) */
typedef struct { tex_t t; int ok; view_t v; int has_v; } vk_treg;
int vk_treg_get(int tid, vk_treg *out);      /* a copy, taken under the registry lock (the game registers from its own threads) */
