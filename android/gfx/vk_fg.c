/* Frame generation (TOTK, 2026-10-23): lsfg-vk's Lossless Scaling frame generation (third_party/lsfg-vk, v1, MIT) on
   the app's window picture. TOTK_LSFG=2 / 3 / 4: the multiplier (1 or 0: off, the default); the shaders come from the
   player's own Lossless.dll (TOTK_LSFG_DLL, default Lossless.dll in the game folder). TOTK_LSFG_PERF=1: performance
   mode (LSFG 3.1p, lighter); TOTK_LSFG_FLOW=0.25..1: flow scale (lower is cheaper, default 1).

   liblsfg.so runs its own Vulkan device on the same GPU (the same Turnip, by its vkGetInstanceProcAddr) and shares
   images and semaphores through opaque file descriptors, as the lsfg-vk layer does with a game's swapchain:
     each real frame is blitted into input image 0 / 1 in turn, and the submission signals a fresh exported semaphore;
     lsfg_present waits for it, writes the in-between frame(s) into the output images and signals one fresh exported
     semaphore each; the next submission here waits for those and copies the output into a picture to show.
   The presenter (vk_draw.c) shows the generated frame(s) first and the real frame after, evenly spaced. The images
   are RGBA16F as the layer's (it uses 16F unless HDR); any failure turns it off for the run with a red line. */
#include "vk_int.h"
#include "../../third_party/lsfg-vk/lsfg_c.h"
#include <dlfcn.h>
#include <unistd.h>

enum { FG_MAXGEN = 3, FG_RING = 8 };
typedef struct { VkImage img; VkDeviceMemory mem; } fg_img;
static struct {
    int (*init)(PFN_vkGetInstanceProcAddr, uint64_t, const char *, int, float, int, int, char *, size_t);
    int (*create)(int, int, const int *, int, uint32_t, uint32_t, VkFormat, char *, size_t);
    int (*present)(int, int, int *, int, char *, size_t);
    void (*del)(int); void (*fin)(void);
} L;
static void *g_lib; static int g_lsfg_up;   /* liblsfg.so loaded; lsfg_init done (lsfg_finalize owed) */
static int g_state;   /* 0 not tried, 1 running, -1 off for this run */
static int g_ngen, g_ctx = -1; static uint32_t g_w, g_h; static uint64_t g_frame;
static fg_img g_in[2], g_out[FG_MAXGEN];
static VkImage g_pic[FG_MAXGEN]; static vk_alloc g_picmem[FG_MAXGEN];   /* the outputs in the window's format */
static VkSemaphore g_sem[FG_RING][1 + FG_MAXGEN];   /* per frame: the input copy's, then one per generated frame */
static const VkFormat FG_FMT = VK_FORMAT_R16G16B16A16_SFLOAT;

/* The settings, changeable while the game runs (vk_fg_configure: the second screen, runtime/bottom_screen.c; 2026-10-23):
   read from the environment at the first use; a change is applied at the next frame (vk_fg_begin tears frame
   generation down and sets it up again with the new multiplier / mode, and the window asks for the new rate). */
static _Atomic int g_req_mult = -1, g_req_perf, g_req_hud = 1, g_req_flow = 100;   /* flow: percent */ static _Atomic unsigned g_req_gen; static unsigned g_applied_gen;
static void cfg_init(void) {
    if (atomic_load(&g_req_mult) >= 0) return;
    const char *e = getenv("TOTK_LSFG"), *pe = getenv("TOTK_LSFG_PERF"), *he = getenv("TOTK_LSFG_HUD"), *fe = getenv("TOTK_LSFG_FLOW");
    float f = fe ? (float)atof(fe) : 1.0f; if (!(f >= 0.25f && f <= 1.0f)) f = 1.0f; atomic_store(&g_req_flow, (int)(f * 100 + 0.5f));
    int m = e ? atoi(e) : 0; if (m < 2) m = 1; if (m > 1 + FG_MAXGEN) m = 1 + FG_MAXGEN;
    atomic_store(&g_req_perf, pe && atoi(pe) > 0); atomic_store(&g_req_hud, !(he && !strcmp(he, "0"))); atomic_store(&g_req_mult, m);
}
int vk_fg_multiplier(void) { cfg_init(); return g_state < 0 ? 1 : atomic_load(&g_req_mult); }
int vk_fg_hud(void) { cfg_init(); return atomic_load(&g_req_hud); }   /* the HUD kept out of it (vk_draw.c hud_on) */
void vk_fg_configure(int mult, int perf, int hud, int flow_pct) {
    cfg_init();
    if (mult < 2) mult = 1; if (mult > 1 + FG_MAXGEN) mult = 1 + FG_MAXGEN;
    if (flow_pct < 25 || flow_pct > 100) flow_pct = 100;
    atomic_store(&g_req_mult, mult); atomic_store(&g_req_perf, perf != 0); atomic_store(&g_req_hud, hud != 0); atomic_store(&g_req_flow, flow_pct);
    atomic_fetch_add(&g_req_gen, 1);
    fprintf(stderr, "[nvn-vk] frame generation: x%d, %s, flow scale %.2f, HUD %s (from the second screen; applied at the next frame)\n", mult, perf ? "performance" : "quality", flow_pct / 100.0, hud ? "kept out" : "mixed in");
}
static void fg_off(const char *what, const char *why) { LOGV("【红】frame generation off: %s%s%s", what, why && *why ? ": " : "", why ? why : ""); g_state = -1; }

static int fg_image(fg_img *x, VkImageUsageFlags usage, int *fd) {   /* lsfg-vk's src/mini/image.cpp, in GENERAL layout */
    VkExternalMemoryImageCreateInfo ext = {VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO, .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT};
    VkImageCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, &ext, .imageType = VK_IMAGE_TYPE_2D, .format = FG_FMT, .extent = {g_w, g_h, 1},
        .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT, .usage = usage, .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    if (vkCreateImage(V.dev, &ci, NULL, &x->img)) return 0;
    VkMemoryRequirements req; vkGetImageMemoryRequirements(V.dev, x->img, &req);
    int t = -1; for (uint32_t i = 0; i < V.mp.memoryTypeCount && t < 0; i++) if ((req.memoryTypeBits >> i & 1) && (V.mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) t = (int)i;
    VkMemoryDedicatedAllocateInfo ded = {VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO, .image = x->img};
    VkExportMemoryAllocateInfo exp = {VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO, &ded, .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT};
    VkMemoryAllocateInfo mai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &exp, .allocationSize = req.size, .memoryTypeIndex = (uint32_t)t};
    if (t < 0 || vkAllocateMemory(V.dev, &mai, NULL, &x->mem)) return 0;
    if (vkBindImageMemory(V.dev, x->img, x->mem, 0)) return 0;
    VkMemoryGetFdInfoKHR gi = {VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR, .memory = x->mem, .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT};
    return !vkGetMemoryFdKHR(V.dev, &gi, fd) && *fd >= 0;
}
/* Semaphores cross to lsfg-vk as sync fds (Turnip on kgsl has no opaque-fd semaphores; the vendored core is patched
   the same way): the input's is exported after the submission that signals it, the outputs' come back from
   lsfg_present and are imported temporarily. */
static int fg_sem(VkSemaphore *s, int exportable) {   /* a fresh binary semaphore */
    if (*s) { vkDestroySemaphore(V.dev, *s, NULL); *s = VK_NULL_HANDLE; }
    VkExportSemaphoreCreateInfo ex = {VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO, .handleTypes = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT};
    VkSemaphoreCreateInfo ci = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, exportable ? &ex : NULL};
    VkResult r = vkCreateSemaphore(V.dev, &ci, NULL, s);
    if (r) { LOGV("frame generation: vkCreateSemaphore: %d", r); return 0; }
    return 1;
}
static void sem_caps(void) {   /* what the driver can export and import (logged once) */
    PFN_vkGetPhysicalDeviceExternalSemaphoreProperties f = (PFN_vkGetPhysicalDeviceExternalSemaphoreProperties)vk_gipa()(V.inst, "vkGetPhysicalDeviceExternalSemaphoreProperties");
    if (!f) return;
    for (int t = 0; t < 2; t++) {
        VkExternalSemaphoreHandleTypeFlagBits ht = t ? VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT : VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT;
        VkPhysicalDeviceExternalSemaphoreInfo in = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_SEMAPHORE_INFO, .handleType = ht};
        VkExternalSemaphoreProperties out = {VK_STRUCTURE_TYPE_EXTERNAL_SEMAPHORE_PROPERTIES};
        f(V.pd, &in, &out);
        LOGV("frame generation: %s semaphores: export from %#x, compatible %#x, features %#x", t ? "sync-fd" : "opaque-fd", out.exportFromImportedHandleTypes, out.compatibleHandleTypes, out.externalSemaphoreFeatures);
    }
}
static void general(VkCommandBuffer cb, VkImage img) {   /* UNDEFINED -> GENERAL once, the backend's layout for everything */
    VkImageMemoryBarrier2 ib = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2, .srcStageMask = VK_PIPELINE_STAGE_2_NONE, .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
        .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT, .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_GENERAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = img, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    VkDependencyInfo di = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &ib};
    vkCmdPipelineBarrier2(cb, &di);
}
static int fg_setup(uint32_t w, uint32_t h, VkFormat pic_fmt) {
    int m = vk_fg_multiplier(); if (m < 2) return 0;
    if (!vkGetSemaphoreFdKHR || !vkImportSemaphoreFdKHR || !vkDestroySemaphore) { fg_off("the driver lacks VK_KHR_external_semaphore_fd", NULL); return 0; }
    sem_caps();
    if (!g_lib) {
        g_lib = dlopen("liblsfg.so", RTLD_NOW | RTLD_LOCAL);
        if (!g_lib) { fg_off("liblsfg.so", dlerror()); return 0; }
        *(void **)&L.init = dlsym(g_lib, "lsfg_init"); *(void **)&L.create = dlsym(g_lib, "lsfg_create"); *(void **)&L.present = dlsym(g_lib, "lsfg_present");
        *(void **)&L.del = dlsym(g_lib, "lsfg_delete"); *(void **)&L.fin = dlsym(g_lib, "lsfg_finalize");
    }
    if (!L.init || !L.create || !L.present || !L.del || !L.fin) { fg_off("liblsfg.so lacks its entry points", NULL); return 0; }
    const char *dll = getenv("TOTK_LSFG_DLL"); if (!dll || !*dll) dll = "Lossless.dll";
    int perf = atomic_load(&g_req_perf); float flow = atomic_load(&g_req_flow) / 100.0f;
    g_ngen = m - 1; g_w = w; g_h = h;
    char err[512] = "";
    uint64_t uuid = (uint64_t)V.props.vendorID << 32 | V.props.deviceID;
    if (L.init(vk_gipa(), uuid, dll, perf, flow, g_ngen, 0, err, sizeof err)) { fg_off(dll, err); return 0; }
    g_lsfg_up = 1;
    int in_fd[2], out_fd[FG_MAXGEN];
    for (int i = 0; i < 2; i++) if (!fg_image(&g_in[i], VK_IMAGE_USAGE_TRANSFER_DST_BIT, &in_fd[i])) { fg_off("input image", NULL); return 0; }
    for (int i = 0; i < g_ngen; i++) if (!fg_image(&g_out[i], VK_IMAGE_USAGE_TRANSFER_SRC_BIT, &out_fd[i])) { fg_off("output image", NULL); return 0; }
    g_ctx = L.create(in_fd[0], in_fd[1], out_fd, g_ngen, w, h, FG_FMT, err, sizeof err);
    if (g_ctx < 0) { fg_off("context", err); return 0; }
    VkCommandBuffer cb = vk_cb();
    for (int i = 0; i < 2; i++) general(cb, g_in[i].img);
    for (int i = 0; i < g_ngen; i++) {
        general(cb, g_out[i].img);
        VkImageCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = pic_fmt, .extent = {w, h, 1}, .mipLevels = 1, .arrayLayers = 1,
            .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | (pic_fmt == VK_FORMAT_R8G8B8A8_UNORM ? VK_IMAGE_USAGE_STORAGE_BIT : 0)};   /* storage: the HUD mask (vk_draw.c) */
        VkMemoryRequirements req;
        if (vkCreateImage(V.dev, &ci, NULL, &g_pic[i])) { fg_off("picture image", NULL); return 0; }
        vkGetImageMemoryRequirements(V.dev, g_pic[i], &req);
        if (!vk_mem_alloc(&req, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &g_picmem[i])) { fg_off("picture memory", NULL); return 0; }
        vkBindImageMemory(V.dev, g_pic[i], g_picmem[i].mem, g_picmem[i].off);
        general(cb, g_pic[i]);
    }
    LOGV("frame generation: lsfg-vk %s, x%d (%d generated a frame), flow scale %.2f, %ux%u, shaders from %s", perf ? "3.1p (performance)" : "3.1", m, g_ngen, flow, w, h, dll);
    g_state = 1;
    return 1;
}

/* The real frame in src (w x h, the window picture): copied into this frame's input and the submission it goes with
   signals the input's semaphore. Returns the number of frames to generate (0: none). Call vk_fg_dispatch after that
   submission. */
void vk_hud_forget(VkImage img) __attribute__((weak));   /* vk_draw.c (the app's window): a cached view of an image about to go */
void vk_window_rate(void) __attribute__((weak));         /* vk_draw.c (the app's window): the window asks for cap x multiplier */
static void fg_teardown(void) {   /* everything frame generation made, so it can start again with other settings */
    if (g_state == 0 && !g_lsfg_up) return;
    vk_gpu_idle();   /* our submissions that read or wait for its images and semaphores are done */
    if (g_ctx >= 0) L.del(g_ctx);
    if (g_lsfg_up) L.fin();   /* waits for its own device */
    g_ctx = -1; g_lsfg_up = 0;
    for (int i = 0; i < 2; i++) if (g_in[i].img) { vkDestroyImage(V.dev, g_in[i].img, NULL); vkFreeMemory(V.dev, g_in[i].mem, NULL); g_in[i] = (fg_img){0}; }
    for (int i = 0; i < FG_MAXGEN; i++) {
        if (g_out[i].img) { vkDestroyImage(V.dev, g_out[i].img, NULL); vkFreeMemory(V.dev, g_out[i].mem, NULL); g_out[i] = (fg_img){0}; }
        if (g_pic[i]) { if (vk_hud_forget) vk_hud_forget(g_pic[i]); vkDestroyImage(V.dev, g_pic[i], NULL); vk_mem_free(&g_picmem[i]); g_pic[i] = VK_NULL_HANDLE; }
    }
    for (int f = 0; f < FG_RING; f++) for (int i = 0; i < 1 + FG_MAXGEN; i++) if (g_sem[f][i]) { vkDestroySemaphore(V.dev, g_sem[f][i], NULL); g_sem[f][i] = VK_NULL_HANDLE; }
    g_state = 0; g_frame = 0;
    LOGV("frame generation: stopped (to start again with the new settings)");
}
int vk_fg_begin(VkCommandBuffer cb, VkImage src, uint32_t w, uint32_t h, VkFormat src_fmt) {
    cfg_init();
    unsigned gen = atomic_load(&g_req_gen);
    if (gen != g_applied_gen) { g_applied_gen = gen; fg_teardown(); g_state = 0; if (vk_window_rate) vk_window_rate(); }   /* new settings: start over (a failed start is tried again too) */
    if (vk_fg_multiplier() < 2) return 0;
    if (!g_state && !fg_setup(w, h, src_fmt)) return 0;
    if (g_state < 0) return 0;
    if (w != g_w || h != g_h) { RED(2, "frame generation: the window picture changed size (%ux%u -> %ux%u); not generating", g_w, g_h, w, h); return 0; }
    VkSemaphore *ps = g_sem[g_frame % FG_RING];
    if (!fg_sem(&ps[0], 1)) { fg_off("input semaphore", NULL); return 0; }
    for (int i = 0; i < g_ngen; i++) if (!fg_sem(&ps[1 + i], 0)) { fg_off("output semaphore", NULL); return 0; }
    VkImageBlit bl = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .srcOffsets = {{0, 0, 0}, {(int32_t)w, (int32_t)h, 1}},
                      .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .dstOffsets = {{0, 0, 0}, {(int32_t)w, (int32_t)h, 1}}};
    vkCmdBlitImage(cb, src, VK_IMAGE_LAYOUT_GENERAL, g_in[g_frame & 1].img, VK_IMAGE_LAYOUT_GENERAL, 1, &bl, VK_FILTER_NEAREST);
    vk_barrier_all(cb);
    vk_sub_signal(ps[0]);
    return g_ngen;
}
/* After the submission vk_fg_begin went with: lsfg-vk generates the in-between frame(s). 0 on failure (then off). */
int vk_fg_dispatch(void) {
    char err[512] = "";
    VkSemaphore *ps = g_sem[g_frame++ % FG_RING];
    int in_fd = -1, out_fd[FG_MAXGEN];
    VkSemaphoreGetFdInfoKHR gi = {VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR, .semaphore = ps[0], .handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT};
    VkResult r = vkGetSemaphoreFdKHR(V.dev, &gi, &in_fd);
    if (r || in_fd < 0) { snprintf(err, sizeof err, "vkGetSemaphoreFdKHR %d", r); fg_off("input sync fd", err); return 0; }
    if (L.present(g_ctx, in_fd, out_fd, g_ngen, err, sizeof err)) { fg_off("present", err); return 0; }
    for (int i = 0; i < g_ngen; i++) {
        VkImportSemaphoreFdInfoKHR ii = {VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_FD_INFO_KHR, .semaphore = ps[1 + i], .flags = VK_SEMAPHORE_IMPORT_TEMPORARY_BIT,
                                         .handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT, .fd = out_fd[i]};
        r = vkImportSemaphoreFdKHR(V.dev, &ii);
        if (r) { snprintf(err, sizeof err, "vkImportSemaphoreFdKHR %d (fd %d)", r, out_fd[i]); fg_off("output sync fd", err); return 0; }
    }
    return 1;
}
/* Generated frame i of the frame just dispatched, in the window's format: the submission this is recorded into waits
   for lsfg-vk to finish it. */
VkImage vk_fg_output(VkCommandBuffer cb, int i) {
    vk_sub_wait(g_sem[(g_frame - 1) % FG_RING][1 + i]);
    VkImageBlit bl = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .srcOffsets = {{0, 0, 0}, {(int32_t)g_w, (int32_t)g_h, 1}},
                      .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .dstOffsets = {{0, 0, 0}, {(int32_t)g_w, (int32_t)g_h, 1}}};
    vkCmdBlitImage(cb, g_out[i].img, VK_IMAGE_LAYOUT_GENERAL, g_pic[i], VK_IMAGE_LAYOUT_GENERAL, 1, &bl, VK_FILTER_NEAREST);
    vk_barrier_all(cb);
    return g_pic[i];
}
