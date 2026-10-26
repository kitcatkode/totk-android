/* Drawing on Vulkan (vk_int.h): the bound NVN state, render passes as dynamic rendering, graphics pipelines,
   descriptor sets, draws, compute dispatches, texture copies and clears, and present. The Metal counterpart is
   nvn_mtl.m state_op / draw_inner / encode_args / pso_desc / copy_* / clear_tex / dispatch; the rules are the same,
   the many Metal-only optimisations (tile memory, pass merging, extrapolation) are not carried over.

   Conventions shared with the translated shaders (tools/totk_xlate, Metal's): clip depth 0..1 (NEAR_IS_MINUS_W is
   converted in the vertex shader), framebuffer origin at the top left, clip-space y up. Vulkan's clip y points down,
   so the viewport is flipped (negative height), which places every pixel where Metal places it. */
#include "vk_int.h"
#include <sys/resource.h>
#include "nvn_hmap.h"
#include <math.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <sys/stat.h>

#define MAXV(a, b) ((a) > (b) ? (a) : (b))
#define MINV(a, b) ((a) < (b) ? (a) : (b))
vk_state S;
static uint64_t g_stat_draws, g_stat_skips[16], g_stat_passes, g_stat_pipes, g_stat_dispatch;
vk_hitch g_hk;

static void state_reset(void) {
    memset(&S, 0, sizeof S);
    for (int i = 0; i < 8; i++) S.blend[i] = (blend_st){i, NVN_BLEND_FUNC_ONE, NVN_BLEND_FUNC_ZERO, NVN_BLEND_FUNC_ONE, NVN_BLEND_FUNC_ZERO, NVN_BLEND_EQUATION_ADD, NVN_BLEND_EQUATION_ADD};
    S.color = (color_st){0, NVN_LOGIC_OP_COPY, NVN_ALPHA_FUNC_ALWAYS}; S.chmask.mask = 0xffffffffu;
    S.poly = (poly_st){NVN_FACE_NONE, NVN_FRONT_FACE_CCW, NVN_POLYGON_MODE_FILL, 0};
    S.ds = (ds_st){0, 1, NVN_DEPTH_FUNC_LESS, 0, NVN_STENCIL_FUNC_ALWAYS, NVN_STENCIL_FUNC_ALWAYS, 1, 1, 1, 1, 1, 1};
    S.st_vmask[0] = S.st_vmask[1] = S.st_wmask[0] = S.st_wmask[1] = 0xff; S.dr[1] = 1;
    for (int i = 0; i < 6; i++) S.tex_hi[i] = 1;
}
static void state_init(void) { static int done; if (!done) { done = 1; state_reset(); } }

/* ---------- render passes ---------- */
static struct {
    int open;                                   /* vkCmdBeginRendering issued */
    int clear_c[8]; VkClearValue cc[8]; int clear_d, clear_s; float cd; uint32_t cs;
    uint32_t w, h;                              /* extent of the open pass */
    VkFormat cfmt[8]; VkFormat dfmt, sfmt; uint32_t ncolor; int dclass[8];
    vk_tex *ctex[8]; vk_tex *dtex; unsigned c_level[8], c_layer[8]; uint64_t c_serial[8], d_serial; int c_nvnfmt[8];
    unsigned c0_level, c0_layer, d_level, d_layer; int c0_cleared; uint64_t c0_serial; int c0_fmt; uint64_t draws0;   /* colour 0 of the open pass (pass probe) */
    VkSampleCountFlagBits samples;
    int barrier_pending, wrote_storage;          /* an NVN barrier inside the pass not acted on yet; a draw of the pass wrote storage since it opened (barriersmart) */
} R;
static VkPipeline g_bound_pipe; static int g_dyn_dirty = 1;

static int begin_rendering(void);
static void pd_pass(VkCommandBuffer cb);
static void pd_draw_note(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static int pd_split_match(const char *key);
static int pd_active(void);
static uint64_t g_samples_cum;   /* SAMPLES_PASSED, cumulative (see condorder_on) */
/* DEPTH16 depth exactly halfway between two steps (TOTK_VK_D16TIE=1: on; a disproven hypothesis for the black flicker, kept as an option). The game's deferred passes mark each tile's
   material class by drawing a depth of 0.5 into a 640x360 DEPTH16 target, then draw the class's shading with depth test
   EQUAL against it (the shadow / AO mask pass, fragment shader 679c5257f9a55f7f). 0.5 * 65535 = 32767.5 sits exactly on
   a rounding tie: presumably on Tegra a constant-depth triangle interpolates to exactly 0.5 and both rasterizations
   round the same way; on Adreno the interpolated depth may carry an ulp of error whose sign depends on the vertex
   positions (the camera jitter), so the marker and the shading round to different steps on some frames (camera cuts),
   EQUAL rejects every fragment and the lit picture goes black for that frame. Every draw into a DEPTH16 target gets a
   fifth of a depth step of constant bias (and a clear value on a tie the same nudge): far from any tie, and the same
   stored value the game meant everywhere else. */
#define D16_NUDGE_UNITS 0.2f
/* On by default (2026-10-22: the dark-water fix, confirmed on the Thor with fragment-written depth nudged too);
   TOTK_VK_D16TIE=0 or TOTK_VK_OFF=d16tie: off. */
static int d16tie_on(void) { static int on = -1; if (on < 0) { const char *e = getenv("TOTK_VK_D16TIE"); on = !(e && !atoi(e)) && !vk_off("d16tie"); } return on; }
static float d16_clear(float v) { double t = (double)v * 65535.0; return d16tie_on() && fabs(t - floor(t) - 0.5) < 1e-3 ? (float)(v + D16_NUDGE_UNITS / 65535.0) : v; }
/* what the open pass's draws read (pass probe report) and what could not be found: a texture id that resolves to nothing
   is bound as an all-zero dummy, a buffer outside the pools as zeros (both counted, and logged the first times) */
typedef struct { uint64_t img; uint32_t w, h; int fmt; vk_tex *x; } probe_in;
static probe_in g_pin[16]; static int g_npin, g_pdummy, g_pzero; static char g_pkey[24];
static uint64_t g_stat_dummy_tex, g_stat_zero_buf;
typedef struct { VkBuffer b; VkDeviceSize off, n; uint64_t addr; uint8_t stage, reg, storage; } probe_buf;
enum { PROBE_BUFS = 12, PROBE_BUF_BYTES = 256 };
static probe_buf g_pbuf[PROBE_BUFS]; static int g_npbuf; static uint8_t g_pbuf_data[PROBE_BUFS][PROBE_BUF_BYTES];
typedef struct { char vs[17]; int prim; uint32_t count, first, inst; float vp[4], dr[2], bias[3]; int clamp, cull, has_sc; int32_t sc[4]; float v0[8]; float c11_25[4], c4_147[4], c4_7[4]; } probe_draw;
static probe_draw g_pdraw[4]; static int g_npdraw;
typedef struct { int clears, clears_skipped, clears_inpass, setrt, cond, cond_off, disabled_at_start; float cc[4]; } probe_ops;   /* between the previous pass's end and this pass's draws */
static probe_ops g_pops;
static void pass_inputs_reset(void) { g_npin = 0; g_pdummy = g_pzero = 0; g_pkey[0] = 0; g_npbuf = 0; g_npdraw = 0; }
static void pass_buf_note(int stage, int reg, int storage, uint64_t addr, const VkDescriptorBufferInfo *d) {
    if (!R.open || stage > 1) return;
    for (int k = 0; k < g_npbuf; k++) if (g_pbuf[k].stage == stage && g_pbuf[k].reg == reg && g_pbuf[k].b == d->buffer && g_pbuf[k].off == d->offset) return;
    if (g_npbuf >= PROBE_BUFS || !addr) return;
    /* the bytes are read here, on the CPU, from guest memory (as of the submit when uniforms are snapshotted; GPU writes
       earlier in the submission are not seen); inline uniform updates not in memory yet are flagged (storage = 2) */
    uint64_t n = MINV(d->range, (VkDeviceSize)PROBE_BUF_BYTES); VkBuffer pb; uint64_t po, avail;
    if (!vk_resolve_span(addr, &pb, &po, &avail)) return;
    n = MINV(n, avail);
    memcpy(g_pbuf_data[g_npbuf], (const void *)(uintptr_t)addr, (size_t)n);
    g_pbuf[g_npbuf++] = (probe_buf){d->buffer, d->offset, n, addr, (uint8_t)stage, (uint8_t)reg, (uint8_t)(storage | (vk_ub_pending(addr, n) ? 2 : 0))};
}
/* ---------- pass probe (TOTK_VK_PASSPROBE=1, diagnostic) ----------
   After every pass, colour target 0 is shrunk to 8x8 texels (blit, nearest) into a row of an RGBA16F atlas; the rows of
   the last PROBE_FRAMES frames are kept. When the flash detector (black_check) confirms a dark frame, the probes of that
   frame and the good frame after it (the same view, after a camera cut) are compared pass by pass and the passes whose output differs most are logged: the first
   pass that goes dark (or NaN) in the flash frame is where the picture is lost. */
enum { PROBE_FRAMES = 16, PROBE_SLOTS = 256 };
typedef struct { uint64_t serial, draws, img; uint32_t w, h; int fmt; probe_in in[16]; int nin, dummy, zero; char key[24]; probe_buf buf[12]; int nbuf; uint32_t dt, dw, df, st, sf, sref, svm, chmask, blend; probe_draw dr[4]; int ndr, cleared; uint64_t dimg; probe_ops ops; } probe_meta;
/* other writes into textures (depth passes, copies, clears, compute stores), in order: `at` is the number of colour
   passes probed before it, so "written before pass N" can be told */
enum { PROBE_WRITES = 384 };
typedef struct { uint64_t img, src; uint32_t w, h, sw, sh; int fmt, sfmt, at; char kind; uint32_t draws; int dsamp; VkFormat vf; probe_in in[16]; int nin; char key[24]; probe_draw d0; int nd; } probe_write;   /* dsamp: depth samples taken (D) */
typedef struct { _Atomic uint64_t frame; uint64_t fence; int n, nw; probe_meta m[PROBE_SLOTS]; probe_write wr[PROBE_WRITES]; } probe_frame;
static probe_frame g_probe[PROBE_FRAMES];
static VkImage g_probe_img; static vk_alloc g_probe_mem; static VkBuffer g_probe_buf; static VkDeviceMemory g_probe_bmem; static uint16_t *g_probe_ptr; static int g_probe_coherent;
#define PROBE_DEPTH_OFF ((VkDeviceSize)PROBE_FRAMES * PROBE_SLOTS * 64 * 8)   /* depth samples after the colour rows: 64 x 4 bytes per write */
#define PROBE_BUF_OFF (PROBE_DEPTH_OFF + (VkDeviceSize)PROBE_FRAMES * PROBE_WRITES * 64 * 4)   /* then the first 256 bytes of each pass's uniform / storage buffers */
static uint64_t g_probe_frame = 1; static int g_probe_n;
static int probe_on(void) { static int on = -1; if (on < 0) { const char *e = getenv("TOTK_VK_PASSPROBE"); on = e && atoi(e); if (on) LOGV("TOTK_VK_PASSPROBE: every pass's colour target is sampled (8x8) for the flash report"); } return on; }
static int probe_bufs_on(void) { return probe_on(); }   /* CPU-side reads: cheap (a GPU copy per buffer stalled the game for tens of seconds per frame) */
static int probe_init(void) {
    static int state; if (state) return state > 0; state = -1;
    VkImageCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = VK_FORMAT_R16G16B16A16_SFLOAT, .extent = {8, 8 * PROBE_SLOTS, 1}, .mipLevels = 1,
        .arrayLayers = PROBE_FRAMES, .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT};
    if (vkCreateImage(V.dev, &ci, NULL, &g_probe_img)) return 0;
    VkMemoryRequirements req; vkGetImageMemoryRequirements(V.dev, g_probe_img, &req);
    if (!vk_mem_alloc(&req, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &g_probe_mem)) return 0;
    vkBindImageMemory(V.dev, g_probe_img, g_probe_mem.mem, g_probe_mem.off);
    VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = PROBE_BUF_OFF + (VkDeviceSize)PROBE_FRAMES * PROBE_SLOTS * 12 * 256, .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    if (vkCreateBuffer(V.dev, &bci, NULL, &g_probe_buf)) return 0;
    vkGetBufferMemoryRequirements(V.dev, g_probe_buf, &req);
    int t = vk_pick_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (t < 0) t = vk_pick_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
    VkMemoryAllocateInfo mai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size, .memoryTypeIndex = (uint32_t)t};
    if (t < 0 || vkAllocateMemory(V.dev, &mai, NULL, &g_probe_bmem)) return 0;
    vkBindBufferMemory(V.dev, g_probe_buf, g_probe_bmem, 0);
    if (vkMapMemory(V.dev, g_probe_bmem, 0, VK_WHOLE_SIZE, 0, (void **)&g_probe_ptr)) return 0;
    g_probe_coherent = !!(V.mp.memoryTypes[t].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VkImageMemoryBarrier2 ib = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2, .srcStageMask = VK_PIPELINE_STAGE_2_NONE, .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
        .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT, .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_GENERAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = g_probe_img, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, PROBE_FRAMES}};
    VkDependencyInfo di = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &ib};
    vkCmdPipelineBarrier2(vk_cb(), &di);
    state = 1; return 1;
}
static void probe_pass(VkCommandBuffer cb) {
    if (!probe_on() || !R.ctex[0] || g_probe_n >= PROBE_SLOTS || !probe_init()) return;
    vk_tex *x = R.ctex[0];
    if (vk_tex_is3d(x) || !(vk_tex_aspect(x) & VK_IMAGE_ASPECT_COLOR_BIT) || R.samples > 1) return;   /* multisampled images cannot be blitted */
    VkFormat f = vk_tex_format(x); VkFormatProperties fp; V.get_format_props(V.pd, f, &fp);
    if (!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_SRC_BIT)) return;
    { int bw, bh; nvn_fmt_bpb(vk_tex_nvn_format(x), &bw, &bh); if (bw > 1 || bh > 1) return; }
    if ((f >= VK_FORMAT_R8_UINT && f <= VK_FORMAT_R8_SINT) || f == VK_FORMAT_R8G8_UINT || f == VK_FORMAT_R8G8_SINT || f == VK_FORMAT_R8G8B8A8_UINT || f == VK_FORMAT_R8G8B8A8_SINT ||
        f == VK_FORMAT_R16_UINT || f == VK_FORMAT_R16_SINT || f == VK_FORMAT_R16G16_UINT || f == VK_FORMAT_R16G16_SINT || f == VK_FORMAT_R16G16B16A16_UINT || f == VK_FORMAT_R16G16B16A16_SINT ||
        f == VK_FORMAT_R32_UINT || f == VK_FORMAT_R32_SINT || f == VK_FORMAT_R32G32_UINT || f == VK_FORMAT_R32G32_SINT || f == VK_FORMAT_R32G32B32A32_UINT || f == VK_FORMAT_R32G32B32A32_SINT ||
        f == VK_FORMAT_A2B10G10R10_UINT_PACK32) return;   /* integer formats cannot be blitted to float */
    if (R.c0_layer >= vk_tex_layers(x) || R.c0_level >= vk_tex_levels(x)) return;
    int slot = g_probe_n++, fr = (int)(g_probe_frame % PROBE_FRAMES);
    uint32_t w = vk_tex_width(x, R.c0_level), h = vk_tex_height(x, R.c0_level);
    VkImageBlit b = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, R.c0_level, R.c0_layer, 1}, .srcOffsets = {{0, 0, 0}, {(int32_t)w, (int32_t)h, 1}},
        .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, (uint32_t)fr, 1}, .dstOffsets = {{0, slot * 8, 0}, {8, slot * 8 + 8, 1}}};
    vkCmdBlitImage(cb, vk_tex_image(x), VK_IMAGE_LAYOUT_GENERAL, g_probe_img, VK_IMAGE_LAYOUT_GENERAL, 1, &b, VK_FILTER_NEAREST);
    vk_barrier_all(cb);
    probe_meta *pm = &g_probe[fr].m[slot];
    *pm = (probe_meta){.serial = R.c0_serial, .draws = g_stat_draws - R.draws0, .img = (uint64_t)(uintptr_t)vk_tex_image(x), .w = w, .h = h, .fmt = R.c0_fmt, .nin = g_npin, .dummy = g_pdummy, .zero = g_pzero};
    memcpy(pm->in, g_pin, sizeof g_pin); memcpy(pm->key, g_pkey, sizeof g_pkey);
    if (!probe_bufs_on() || g_stat_draws - R.draws0 > 8) g_npbuf = 0;   /* buffers: screen-space passes only */
    pm->nbuf = g_npbuf; memcpy(pm->buf, g_pbuf, sizeof g_pbuf);
    pm->ndr = g_npdraw; memcpy(pm->dr, g_pdraw, sizeof g_pdraw); pm->cleared = R.c0_cleared; pm->ops = g_pops; memset(&g_pops, 0, sizeof g_pops); pm->dimg = R.dtex ? (uint64_t)(uintptr_t)vk_tex_image(R.dtex) : 0;
    pm->dt = S.ds.dt; pm->dw = S.ds.dw; pm->df = S.ds.df; pm->st = S.ds.st; pm->sf = S.ds.sf_f; pm->sref = S.st_ref[0]; pm->svm = S.st_vmask[0]; pm->chmask = S.chmask.mask; pm->blend = S.color.blend_en;   /* as of its last draw */
    for (int k = 0; k < g_npbuf; k++)   /* into the probe buffer's host memory (no GPU work) */
        memcpy((uint8_t *)g_probe_ptr + PROBE_BUF_OFF + (((size_t)fr * PROBE_SLOTS + (size_t)slot) * 12 + (size_t)k) * 256, g_pbuf_data[k], (size_t)g_pbuf[k].n);
}
static void probe_frame_end(uint64_t frame_no) {   /* at present: copy this frame's rows out, start the next frame */
    if (!probe_on() || !g_probe_img) { g_probe_n = 0; return; }
    int fr = (int)(g_probe_frame % PROBE_FRAMES);
    probe_frame *p = &g_probe[fr];
    atomic_store(&p->frame, 0);
    if (g_probe_n) {
        VkCommandBuffer cb = vk_cb();
        VkBufferImageCopy c = {.bufferOffset = (VkDeviceSize)fr * PROBE_SLOTS * 64 * 8, .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, (uint32_t)fr, 1}, .imageExtent = {8, 8 * (uint32_t)g_probe_n, 1}};
        vkCmdCopyImageToBuffer(cb, g_probe_img, VK_IMAGE_LAYOUT_GENERAL, g_probe_buf, 1, &c);
        vk_barrier_all(cb);
    }
    p->n = g_probe_n; p->fence = mtl_last_fence() + 1;
    atomic_store(&p->frame, frame_no);
    g_probe_frame++; g_probe_n = 0;
    probe_frame *q = &g_probe[g_probe_frame % PROBE_FRAMES]; atomic_store(&q->frame, 0); q->nw = 0;
}
static void probe_write_note(char kind, vk_tex *x, vk_tex *src, uint32_t draws) {   /* replay thread */
    if (!probe_on() || !x) return;
    probe_frame *p = &g_probe[g_probe_frame % PROBE_FRAMES];
    if (p->nw >= PROBE_WRITES) return;
    int wi = p->nw++;
    probe_write *w = &p->wr[wi];
    *w = (probe_write){(uint64_t)(uintptr_t)vk_tex_image(x), src ? (uint64_t)(uintptr_t)vk_tex_image(src) : 0, vk_tex_width(x, 0), vk_tex_height(x, 0),
        src ? vk_tex_width(src, 0) : 0, src ? vk_tex_height(src, 0) : 0, vk_tex_nvn_format(x), src ? vk_tex_nvn_format(src) : 0, g_probe_n, kind, draws};
    w->vf = vk_tex_format(x);
    if (kind == 'D') {   /* what the depth pass read, and its depth at 8x8 points */
        w->nin = g_npin; memcpy(w->in, g_pin, sizeof g_pin); memcpy(w->key, g_pkey, sizeof g_pkey); w->nd = g_npdraw; if (g_npdraw) w->d0 = g_pdraw[0];
        if (draws <= 8 && probe_init() && R.samples <= 1 && !vk_tex_is3d(x) && (vk_tex_aspect(x) & VK_IMAGE_ASPECT_DEPTH_BIT) && R.d_layer < vk_tex_layers(x) && R.d_level < vk_tex_levels(x)) {
            uint32_t lw = vk_tex_width(x, R.d_level), lh = vk_tex_height(x, R.d_level);
            VkBufferImageCopy r[64]; VkDeviceSize base = PROBE_DEPTH_OFF + ((VkDeviceSize)(p - g_probe) * PROBE_WRITES + (VkDeviceSize)wi) * 64 * 4;
            for (int k = 0; k < 64; k++) r[k] = (VkBufferImageCopy){.bufferOffset = base + (VkDeviceSize)k * 4, .imageSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, R.d_level, R.d_layer, 1},
                .imageOffset = {(int32_t)(lw * (2 * (k % 8) + 1) / 16), (int32_t)(lh * (2 * (k / 8) + 1) / 16), 0}, .imageExtent = {1, 1, 1}};
            VkCommandBuffer cb = vk_cb();
            vkCmdCopyImageToBuffer(cb, vk_tex_image(x), VK_IMAGE_LAYOUT_GENERAL, g_probe_buf, 64, r);
            vk_barrier_all(cb);
            w->dsamp = 1;
        }
    }
}
static uint32_t g_d16_lo, g_d16_hi;
static int depth_stats(const probe_frame *p, int wi, double *mn, double *mean, double *mx) {
    const probe_write *w = &p->wr[wi]; if (!w->dsamp) return 0;
    const uint8_t *q = (const uint8_t *)g_probe_ptr + PROBE_DEPTH_OFF + ((size_t)(p - g_probe) * PROBE_WRITES + (size_t)wi) * 64 * 4;
    double s = 0; *mn = 1e30; *mx = -1e30;
    for (int k = 0; k < 64; k++) {
        uint32_t u; memcpy(&u, q + k * 4, 4); double v;
        if (w->vf == VK_FORMAT_D16_UNORM || w->vf == VK_FORMAT_D16_UNORM_S8_UINT) { v = (u & 0xffff) / 65535.0; if (k == 0 || (u & 0xffff) < g_d16_lo) g_d16_lo = u & 0xffff; if (k == 0 || (u & 0xffff) > g_d16_hi) g_d16_hi = u & 0xffff; }
        else if (w->vf == VK_FORMAT_D32_SFLOAT || w->vf == VK_FORMAT_D32_SFLOAT_S8_UINT) { float f; memcpy(&f, &u, 4); v = f; }
        else v = (u & 0xffffff) / 16777215.0;
        s += v; if (v < *mn) *mn = v; if (v > *mx) *mx = v;
    }
    *mean = s / 64; return 1;
}
static float half_f(uint16_t h) {
    uint32_t s = (uint32_t)(h >> 15) << 31, e = (h >> 10) & 31, m = h & 1023, u;
    if (!e) { if (!m) u = s; else { e = 113; while (!(m & 1024)) { m <<= 1; e--; } m &= 1023; u = s | e << 23 | m << 13; } }
    else if (e == 31) u = s | 0x7f800000u | m << 13; else u = s | (e + 112) << 23 | m << 13;
    float f; memcpy(&f, &u, 4); return f;
}
static const probe_frame *probe_find(uint64_t frame_no) { for (int i = 0; i < PROBE_FRAMES; i++) if (atomic_load(&g_probe[i].frame) == frame_no) return &g_probe[i]; return NULL; }
static void probe_stats(const probe_frame *p, int slot, double *mean, int *nan) {
    const uint16_t *q = g_probe_ptr + ((size_t)(p - g_probe) * PROBE_SLOTS + (size_t)slot) * 64 * 4;
    double s = 0; int n = 0;
    for (int i = 0; i < 64; i++) { for (int c = 0; c < 3; c++) { float v = half_f(q[i * 4 + c]); if (v != v || v > 65504.f || v < -65504.f) n++; else s += v; } }
    *mean = s / 192; *nan = n;
}
static int probe_report(uint64_t flash, uint64_t before) {   /* presenter thread, after the flash frame's GPU work; returns the passes logged */
    if (!probe_on() || !g_probe_ptr) return -1;
    const probe_frame *a = probe_find(flash), *b = probe_find(before);
    if (!a || !b) { LOGV("pass probe: frames %llu / %llu are no longer kept", (unsigned long long)flash, (unsigned long long)before); return -1; }
    mtl_wait(a->fence, 2000000000ull); mtl_wait(b->fence, 2000000000ull);
    if (!g_probe_coherent) { VkMappedMemoryRange r = {VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, .memory = g_probe_bmem, .size = VK_WHOLE_SIZE}; vkInvalidateMappedMemoryRanges(V.dev, 1, &r); }
    LOGV("pass probe, flash frame %llu (%d passes probed) vs the good frame %llu (%d): passes whose colour output differs (mean RGB, bad = NaN/inf texels of 192)",
         (unsigned long long)flash, a->n, (unsigned long long)before, b->n);
    /* the two frames need not have the same passes (shadow / cube faces take turns): pair them up by size and format
       (longest common subsequence), then compare the pairs */
    static uint16_t L[PROBE_SLOTS + 1][PROBE_SLOTS + 1];
    int na_ = a->n, nb_ = b->n;
    #define PSAME(i, j) (a->m[i].w == b->m[j].w && a->m[i].h == b->m[j].h && a->m[i].fmt == b->m[j].fmt)
    for (int i = 0; i <= na_; i++) L[i][nb_] = 0; for (int j = 0; j <= nb_; j++) L[na_][j] = 0;
    for (int i = na_ - 1; i >= 0; i--) for (int j = nb_ - 1; j >= 0; j--) L[i][j] = PSAME(i, j) ? (uint16_t)(L[i + 1][j + 1] + 1) : MAXV(L[i + 1][j], L[i][j + 1]);
    int shown = 0, paired = 0;
    for (int i = 0, j = 0; i < na_ && j < nb_; ) {
        if (PSAME(i, j)) {
            paired++;
            double ma, mb; int bad_a, bad_b; probe_stats(a, i, &ma, &bad_a); probe_stats(b, j, &mb, &bad_b);
            if ((bad_a != bad_b || fabs(ma - mb) > 0.1 * (fabs(ma) + fabs(mb)) + 0.01) && shown < 60) {
                shown++;
                const probe_meta *x = &a->m[i], *y = &b->m[j];
                LOGV("  pass %3d (good %3d): %ux%u fmt %#x, %llu draws (good %llu): mean %.4f bad %d, good %.4f bad %d",
                     i, j, x->w, x->h, x->fmt, (unsigned long long)x->draws, (unsigned long long)y->draws, ma, bad_a, mb, bad_b);
                for (int f = 0; f < 2; f++) {   /* what the pass read, in the flash frame and in the good one */
                    const probe_frame *pf = f ? b : a; const probe_meta *m = f ? y : x; int at = f ? j : i; const probe_write *dps[4]; int ndp = 0;
                    char line[2400]; size_t o = (size_t)snprintf(line, sizeof line, "      %s: fs %s, %d missing textures, %d buffers outside the pools; reads", f ? "good " : "flash", m->key[0] ? m->key : "?", m->dummy, m->zero);
                    for (int k = 0; k < m->nin && o + 80 < sizeof line; k++) {
                        char fb[160]; int from = -1; for (int q = at - 1; q >= 0; q--) if (pf->m[q].img == m->in[k].img) { from = q; break; }
                        const probe_write *wl = NULL; for (int q = pf->nw - 1; q >= 0; q--) if (pf->wr[q].img == m->in[k].img && pf->wr[q].at <= at && (from < 0 || pf->wr[q].at > from)) { wl = &pf->wr[q]; break; }
                        if (wl) snprintf(fb, sizeof fb, "%s before pass %d%s", wl->kind == 'D' ? "depth pass" : wl->kind == 'C' ? "copy" : wl->kind == 'K' ? "clear" : wl->kind == 'X' ? "image store" : "upload", wl->at,
                                         wl->kind == 'C' ? "" : "");
                        if (wl && wl->kind == 'C') snprintf(fb + strlen(fb), sizeof fb - strlen(fb), " from %ux%u %#x", wl->sw, wl->sh, wl->sfmt);
                        if (wl && wl->kind == 'D') { double dn, dm, dx; snprintf(fb + strlen(fb), sizeof fb - strlen(fb), ", %u draws", wl->draws);
                            if (depth_stats(pf, (int)(wl - pf->wr), &dn, &dm, &dx)) snprintf(fb + strlen(fb), sizeof fb - strlen(fb), ", depth %.4f..%.4f mean %.4f", dn, dx, dm);
                            if (ndp < 4) dps[ndp++] = wl; }
                        else if (from >= 0) snprintf(fb, sizeof fb, "from pass %d", from);
                        o += (size_t)snprintf(line + o, sizeof line - o, " [%ux%u %#x %s]", m->in[k].w, m->in[k].h, m->in[k].fmt, (wl || from >= 0) ? fb : "not written this frame");
                    }
                    if (!m->nin) snprintf(line + o, sizeof line - o, " no textures");
                    LOGV("%s", line);
                    LOGV("        state (last draw): depth test %u func %u write %u, stencil %u func %u ref %#x mask %#x, colour mask %#x, blend %#x",
                         m->dt, m->df, m->dw, m->st, m->sf, m->sref, m->svm, m->chmask, m->blend);
                    { int seen_c = 0, seen_d = 0, wrote_c = -1, wrote_d = -1; const probe_frame *o = f ? a : b;   /* the colour / depth images: also in the other frame? written earlier in this one? */
                      for (int q = 0; q < o->n; q++) { if (o->m[q].img == m->img) seen_c = 1; if (m->dimg && o->m[q].dimg == m->dimg) seen_d = 1; }
                      for (int q = 0; q < at; q++) { if (pf->m[q].img == m->img) wrote_c = q; if (m->dimg && pf->m[q].dimg == m->dimg) wrote_d = q; }
                      LOGV("        since the pass before: %d colour clears issued (%d skipped: render disabled, %d inside a pass), last clear colour %g %g %g %g; %d SetRenderTargets, %d conditional-render ops (%d disabled); render %s at pass start",
                           m->ops.clears, m->ops.clears_skipped, m->ops.clears_inpass, m->ops.cc[0], m->ops.cc[1], m->ops.cc[2], m->ops.cc[3], m->ops.setrt, m->ops.cond, m->ops.cond_off, m->ops.disabled_at_start ? "DISABLED" : "enabled");
                      LOGV("        colour target %#llx: %s, %s the other frame, last drawn earlier this frame at pass %d; depth target %#llx: %s the other frame, used earlier at pass %d",
                           (unsigned long long)m->img, m->cleared ? "cleared at the pass start" : "loaded (not cleared)", seen_c ? "also in" : "NOT in", wrote_c,
                           (unsigned long long)m->dimg, seen_d ? "also in" : "NOT in", wrote_d); }
                    for (int q = 0; q < m->ndr; q++) { const probe_draw *d = &m->dr[q];
                        LOGV("        draw %d: vs %s prim %d count %u first %u inst %u, viewport %g %g %g %g, depth range %g..%g, bias %g/%g on %g, clamp %d, cull %d, scissor %s%d %d %d %d, vertex 0: %g %g %g %g %g %g %g %g",
                             q, d->vs, d->prim, d->count, d->first, d->inst, d->vp[0], d->vp[1], d->vp[2], d->vp[3], d->dr[0], d->dr[1], d->bias[0], d->bias[1], d->bias[2], d->clamp, d->cull,
                             d->has_sc ? "" : "(off) ", d->sc[0], d->sc[1], d->sc[2], d->sc[3], d->v0[0], d->v0[1], d->v0[2], d->v0[3], d->v0[4], d->v0[5], d->v0[6], d->v0[7]);
                        LOGV("          vs slot 8 [25] = %g %g %g %g, slot 1 [147] = %g %g %g %g, slot 1 [7] = %g %g %g %g", d->c11_25[0], d->c11_25[1], d->c11_25[2], d->c11_25[3],
                             d->c4_147[0], d->c4_147[1], d->c4_147[2], d->c4_147[3], d->c4_7[0], d->c4_7[1], d->c4_7[2], d->c4_7[3]); }
                    for (int q = 0; q < pf->nw; q++) if (pf->wr[q].at == at) {   /* depth passes (its own depth attachment too), copies and clears since the colour pass before */
                        const probe_write *w = &pf->wr[q]; double dn, dm, dx; char ds[80] = "";
                        if (w->kind == 'D' && depth_stats(pf, q, &dn, &dm, &dx)) { if (w->vf == VK_FORMAT_D16_UNORM) snprintf(ds, sizeof ds, ", depth %u..%u of 65535", g_d16_lo, g_d16_hi); else snprintf(ds, sizeof ds, ", depth %.4f..%.4f mean %.4f", dn, dx, dm); }
                        if (w->kind == 'D' && w->nd) LOGV("          its first draw: vs %s, slot 8 [25] = %g %g %g %g, slot 1 [147] = %g %g %g %g", w->d0.vs, w->d0.c11_25[0], w->d0.c11_25[1], w->d0.c11_25[2], w->d0.c11_25[3], w->d0.c4_147[0], w->d0.c4_147[1], w->d0.c4_147[2], w->d0.c4_147[3]);
                        LOGV("        just before / with it: %s %ux%u %#x%s%s", w->kind == 'D' ? "depth pass on" : w->kind == 'C' ? "copy into" : w->kind == 'K' ? "clear of" : w->kind == 'X' ? "image store to" : "upload to",
                             w->w, w->h, w->fmt, w->kind == 'D' ? (snprintf(ds + strlen(ds), sizeof ds - strlen(ds), ", %u draws", w->draws), ds) : "", "");
                    }
                    if (f == 1) {   /* the buffers both frames' passes read, side by side, where their bytes differ */
                        for (int k = 0; k < x->nbuf; k++) {
                            const probe_buf *ba = &x->buf[k]; const probe_buf *bb = NULL; int kb = -1;
                            for (int q = 0; q < y->nbuf; q++) if (y->buf[q].stage == ba->stage && y->buf[q].reg == ba->reg) { bb = &y->buf[q]; kb = q; break; }
                            const uint8_t *pa = (const uint8_t *)g_probe_ptr + PROBE_BUF_OFF + (((size_t)(a - g_probe) * PROBE_SLOTS + (size_t)i) * 12 + (size_t)k) * 256;
                            const uint8_t *pb = bb ? (const uint8_t *)g_probe_ptr + PROBE_BUF_OFF + (((size_t)(b - g_probe) * PROBE_SLOTS + (size_t)j) * 12 + (size_t)kb) * 256 : NULL;
                            size_t nn = bb ? MINV(ba->n, bb->n) : 0;
                            if (bb && ba->n == bb->n && !memcmp(pa, pb, nn)) continue;
                            int first = -1; for (size_t q = 0; q < nn; q += 4) if (memcmp(pa + q, pb + q, 4)) { first = (int)q; break; }
                            char l3[900]; size_t o3 = (size_t)snprintf(l3, sizeof l3, "        %s %s %d (%#llx, %llu bytes probed%s) differs%s", ba->stage ? "fs" : "vs", ba->reg >= 16 ? "storage" : "uniform", ba->reg % 16,
                                (unsigned long long)ba->addr, (unsigned long long)ba->n, (ba->storage & 2) || (bb && (bb->storage & 2)) ? ", inline updates pending" : "", bb ? "" : " (not bound in the good frame)");
                            if (first >= 0) {
                                int st = first / 16 * 16; o3 += (size_t)snprintf(l3 + o3, sizeof l3 - o3, " from byte %d; flash/good floats at %d:", first, st);
                                for (int q = st; q < st + 32 && (size_t)q + 4 <= nn && o3 + 40 < sizeof l3; q += 4) { float fa, fb2; memcpy(&fa, pa + q, 4); memcpy(&fb2, pb + q, 4); o3 += (size_t)snprintf(l3 + o3, sizeof l3 - o3, " %g/%g", fa, fb2); }
                            }
                            LOGV("%s", l3);
                        }
                    }
                    for (int d = 0; d < ndp; d++) {   /* the depth passes it read: what they read */
                        const probe_write *w = dps[d]; char l2[600]; size_t o2 = (size_t)snprintf(l2, sizeof l2, "        that %ux%u %#x depth pass: fs %s, reads", w->w, w->h, w->fmt, w->key[0] ? w->key : "(none: depth only)");
                        for (int k = 0; k < w->nin && o2 + 60 < sizeof l2; k++) {
                            int from = -1; for (int q = 0; q < w->at && q < pf->n; q++) if (pf->m[q].img == w->in[k].img) from = q;
                            const probe_write *ww = NULL; for (int q = (int)(w - pf->wr) - 1; q >= 0; q--) if (pf->wr[q].img == w->in[k].img) { ww = &pf->wr[q]; break; }
                            o2 += (size_t)snprintf(l2 + o2, sizeof l2 - o2, " [%ux%u %#x%s%s]", w->in[k].w, w->in[k].h, w->in[k].fmt, ww ? " written by " : "", ww ? (ww->kind == 'D' ? "a depth pass" : ww->kind == 'C' ? "a copy" : ww->kind == 'K' ? "a clear" : ww->kind == 'X' ? "an image store" : "an upload") : (from >= 0 ? " a colour pass" : " not written this frame"));
                            if (ww && ww->kind == 'D') { double dn, dm, dx; if (depth_stats(pf, (int)(ww - pf->wr), &dn, &dm, &dx)) o2 += (size_t)snprintf(l2 + o2, sizeof l2 - o2, " (depth %.4f..%.4f)", dn, dx); }
                        }
                        if (!w->nin) snprintf(l2 + o2, sizeof l2 - o2, " no textures");
                        LOGV("%s", l2);
                    }
                    ndp = 0;
                }
            }
            i++; j++;
        } else if (L[i + 1][j] >= L[i][j + 1]) i++; else j++;
    }
    #undef PSAME
    LOGV("  %d passes paired, %d differ%s", paired, shown, shown >= 60 ? " (first 60 shown)" : "");
    return shown;
}
int vk_probe_report(uint64_t flash, uint64_t before) { return probe_report(flash, before); }   /* tests */
/* depth target sizes per frame: a change is logged with the frame number, to line up with the dark
   flashes (dynamic resolution re-creates the scene targets; their storage then holds last frame's content on hardware) */
static uint32_t g_fr[16], g_prev[16]; static int g_fr_n, g_prev_n; static uint64_t g_stat_rescg;
static int u32cmp(const void *a, const void *b) { uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b; return x < y ? -1 : x > y; }
static void sizes_str(const uint32_t *v, int n, char *out, size_t cap) {
    size_t o = 0; out[0] = 0;
    for (int i = 0; i < n && o + 16 < cap; i++) o += (size_t)snprintf(out + o, cap - o, "%s%ux%u", i ? " " : "", v[i] >> 16, v[i] & 0xffff);
}
/* NVN barriers between draws on the same render targets (TOTK_VK_OFF=passbarrier: dropped, as before). The game orders
   fragment work and invalidates texture caches between draws that feed each other without changing targets: the
   material shaders store per-tile flags that the lighting quads read (nvn_mtl.m msl_stores_through). Inside one Vulkan
   pass draws have no such ordering (turnip in sysmem mode runs them overlapped), so the barrier ends the pass; the next
   draw reopens it with load. TOTK_VK_SERIAL=1 (diagnostic): end the pass after every draw. */
static uint64_t g_stat_inpass_barriers;
static int pass_barrier_on(void) { static int on = -1; if (on < 0) on = !vk_off("passbarrier"); return on; }
/* Only barriers that can matter to the open pass end it (TOTK_VK_OFF=barriersmart: every one, as before). A barrier
   waits for the next draw: that draw ends the pass first if it samples one of the pass's own attachments, or if a draw of
   the pass wrote storage (buffers or images) since it opened - the material shaders' per-tile flags. Otherwise the
   barrier is dropped: draws that only add to the same targets need no order the pass does not give them. Any end of the
   pass meanwhile (dispatch, copy, split) does the barrier anyway (vk_end_rendering's full barrier). 2026-10-26: ~86 of
   ~136 passes a frame were barrier splits; without any, the GPU did 20.3 -> 19.7 ms a frame. */
static uint64_t g_stat_barriers_dropped;
static int barriersmart_on(void) { static int on = -1; if (on < 0) on = !vk_off("barriersmart"); return on; }
static int serial_draws(void) { static int on = -1; if (on < 0) { const char *e = getenv("TOTK_VK_SERIAL"); on = e && atoi(e); if (on) LOGV("TOTK_VK_SERIAL: every draw is its own pass"); } return on; }
void vk_end_rendering(void) {
    if (!R.open && S.has_rt) {   /* clears still waiting for a draw become a pass of their own (load op CLEAR) */
        int pend = R.clear_d || R.clear_s; for (int i = 0; i < 8; i++) pend |= R.clear_c[i];
        if (pend) begin_rendering();
    }
    if (!R.open) return;
    VkCommandBuffer cb = vk_cb();
    vkCmdEndRendering(cb);
    vk_barrier_all(cb);
    vk_sub_time();
    R.open = 0; g_bound_pipe = VK_NULL_HANDLE; R.barrier_pending = 0; R.wrote_storage = 0;
    if (R.dtex) probe_write_note('D', R.dtex, NULL, (uint32_t)(g_stat_draws - R.draws0));
    probe_pass(cb);
    pd_pass(cb);
}
static int rt_same(const p_set_rt *a, const p_set_rt *b) {
    if (a->n != b->n || a->depth != b->depth || (a->depth && a->depth->serial != b->depth->serial)) return 0;
    if (a->has_dview != b->has_dview || (a->has_dview && memcmp(&a->dview, &b->dview, sizeof a->dview))) return 0;
    for (int i = 0; i < a->n && i < 8; i++) {
        if (!a->color[i] != !b->color[i] || (a->color[i] && a->color[i]->serial != b->color[i]->serial)) return 0;
        if (a->has_cview[i] != b->has_cview[i] || (a->has_cview[i] && memcmp(&a->cview[i], &b->cview[i], sizeof a->cview[i]))) return 0;
    }
    return 1;
}
static unsigned view_level(const view_t *v, int has) { return has && v->set_levels ? v->base_level : 0; }
/* Open the pass for the current targets with the pending clears as load operations. 0 when nothing can be attached. */
static int begin_rendering(void) {
    if (R.open) return 1;
    VkRenderingAttachmentInfo ca[8], da = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO}, sa = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    uint32_t w = 16384, h = 16384, nc = 0; int have = 0;
    memset(R.cfmt, 0, sizeof R.cfmt); R.dfmt = R.sfmt = VK_FORMAT_UNDEFINED; memset(R.ctex, 0, sizeof R.ctex); R.dtex = NULL; R.samples = VK_SAMPLE_COUNT_1_BIT;
    if (S.has_rt) for (int i = 0; i < S.rt.n && i < 8; i++) {
        ca[i] = (VkRenderingAttachmentInfo){VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
            .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD, .storeOp = VK_ATTACHMENT_STORE_OP_STORE};
        nc = (uint32_t)i + 1;
        const tex_t *t = S.rt.color[i]; if (!t) continue;
        vk_tex *x = vk_tex_get(t); if (!x) continue;
        VkImageView v = vk_tex_view(x, t, S.rt.has_cview[i] ? &S.rt.cview[i] : NULL, 1); if (!v) continue;
        unsigned l = view_level(&S.rt.cview[i], S.rt.has_cview[i]);
        int f = S.rt.has_cview[i] && S.rt.cview[i].set_format ? S.rt.cview[i].format : t->d.format;
        uint32_t tw = vk_tex_width(x, l), th = vk_tex_height(x, l);
        { int bw, bh, vbw, vbh; nvn_fmt_bpb(t->d.format, &bw, &bh); nvn_fmt_bpb(f, &vbw, &vbh);   /* a one-texel-per-block view of a BC texture is sized in blocks */
          if (bw > vbw && vbw == 1) tw = (tw + bw - 1) / bw; if (bh > vbh && vbh == 1) th = (th + bh - 1) / bh; }
        w = MINV(w, tw); h = MINV(h, th);
        VkFormat vf; vk_fmt(f, &vf, NULL);
        R.c_level[i] = l; R.c_layer[i] = S.rt.has_cview[i] && S.rt.cview[i].set_layers ? S.rt.cview[i].min_layer : 0; R.c_serial[i] = t->serial; R.c_nvnfmt[i] = f;
        if (i == 0) { R.c0_level = l; R.c0_layer = S.rt.has_cview[0] && S.rt.cview[0].set_layers ? S.rt.cview[0].min_layer : 0; R.c0_serial = t->serial; R.c0_fmt = f; }
        ca[i].imageView = v; R.cfmt[i] = vf; R.ctex[i] = x; R.dclass[i] = vk_fmt_class(f); vk_tex_mark_gpu_written(x); have = 1;
        if (t->d.samples > 1) R.samples = (VkSampleCountFlagBits)t->d.samples;
        if (i == 0) R.c0_cleared = R.clear_c[0] ? 1 : 0;
        if (R.clear_c[i]) { ca[i].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; ca[i].clearValue = R.cc[i]; R.clear_c[i] = 0; }
    }
    if (S.has_rt && S.rt.depth) {
        const tex_t *t = S.rt.depth; vk_tex *x = vk_tex_get(t);
        VkImageView v = x ? vk_tex_view(x, t, S.rt.has_dview ? &S.rt.dview : NULL, 1) : VK_NULL_HANDLE;
        if (v) {
            unsigned l = view_level(&S.rt.dview, S.rt.has_dview);
            R.d_level = l; R.d_layer = S.rt.has_dview && S.rt.dview.set_layers ? S.rt.dview.min_layer : 0;
            w = MINV(w, vk_tex_width(x, l)); h = MINV(h, vk_tex_height(x, l));
            { uint32_t key = vk_tex_width(x, l) << 16 | vk_tex_height(x, l); int k = 0;   /* the frame's depth target sizes */
              while (k < g_fr_n && g_fr[k] != key) k++;
              if (k == g_fr_n && g_fr_n < 16) g_fr[g_fr_n++] = key; }
            VkImageAspectFlags as = vk_tex_aspect(x); R.dtex = x; R.d_serial = t->serial; vk_tex_mark_gpu_written(x); have = 1;
            if (as & VK_IMAGE_ASPECT_DEPTH_BIT) { da.imageView = v; da.imageLayout = VK_IMAGE_LAYOUT_GENERAL; da.loadOp = R.clear_d ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD; da.storeOp = VK_ATTACHMENT_STORE_OP_STORE; R.dfmt = vk_tex_format(x); da.clearValue.depthStencil.depth = R.dfmt == VK_FORMAT_D16_UNORM ? d16_clear(R.cd) : R.cd; }
            if (as & VK_IMAGE_ASPECT_STENCIL_BIT) { sa.imageView = v; sa.imageLayout = VK_IMAGE_LAYOUT_GENERAL; sa.loadOp = R.clear_s ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD; sa.storeOp = VK_ATTACHMENT_STORE_OP_STORE; sa.clearValue.depthStencil.stencil = R.cs; R.sfmt = vk_tex_format(x); }
            if (t->d.samples > 1) R.samples = (VkSampleCountFlagBits)t->d.samples;
            R.clear_d = R.clear_s = 0;
        }
    }
    if (!have) {   /* no target: a pass of rasterisation only, sized by viewport 0 (or the scissor) */
        double x1 = S.has_vp ? ceil(fmax(S.vp[0][0], S.vp[0][0] + S.vp[0][2])) : 0, y1 = S.has_vp ? ceil(fmax(S.vp[0][1], S.vp[0][1] + S.vp[0][3])) : 0;
        if ((x1 < 1 || y1 < 1) && S.has_sc) { x1 = (double)S.sc.x + S.sc.w; y1 = (double)S.sc.y + S.sc.h; }
        w = x1 < 1 ? 1 : x1 > 16384 ? 16384 : (uint32_t)x1; h = y1 < 1 ? 1 : y1 > 16384 ? 16384 : (uint32_t)y1;
    }
    R.ncolor = nc; R.w = w; R.h = h;
    VkCommandBuffer cb = vk_cb();
    vk_barrier_all(cb);
    VkRenderingInfo ri = {VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = {{0, 0}, {w, h}}, .layerCount = 1, .colorAttachmentCount = nc, .pColorAttachments = ca,
        .pDepthAttachment = da.imageView ? &da : NULL, .pStencilAttachment = sa.imageView ? &sa : NULL};
    vkCmdBeginRendering(cb, &ri);
    R.open = 1; g_bound_pipe = VK_NULL_HANDLE; g_dyn_dirty = 1; g_stat_passes++; R.draws0 = g_stat_draws; pass_inputs_reset(); g_pops.disabled_at_start = S.render_disabled;
    return 1;
}
/* Submission split (TOTK, 2026-10-23). TOTK submits a whole frame at once, so the GPU used to start only after the
   replay of the entire frame (~23 ms recording + ~20 ms GPU, back to back: 46 ms frames in the heavy areas). Before
   opening a new pass, if this submission has been recording for TOTK_VK_SPLIT_MS (default 2; 0 = off), what is
   recorded so far is submitted, so the GPU runs while the rest of the frame records. Only between passes: no pass is
   cut. Default 4 -> 2 ms (2026-10-23, after the fence fix made a submit cheap): Lookout Landing 29.2 -> 30.8 fps,
   the game's GPU waits 5.4 -> 4.3 ms a frame; 1 ms gave nothing more. */
static uint64_t g_stat_splits, g_stat_pass_splits;
static int g_split_ms = -1, g_split_every;   /* TOTK_VK_SPLIT_MS=-1 (diagnostic, the tests): before every pass */
static uint64_t g_split_serial, g_split_t0;
static int split_due(void) {   /* this submission has recorded for TOTK_VK_SPLIT_MS */
    if (g_split_ms < 0) { const char *e = getenv("TOTK_VK_SPLIT_MS"); g_split_ms = e ? atoi(e) : 2; if (g_split_ms < 0) { g_split_every = 1; g_split_ms = 1; } LOGV("submission split %s (TOTK_VK_SPLIT_MS, 0 = off)", g_split_every ? "before every pass" : g_split_ms ? "every few ms of recording" : "off"); }
    if (!g_split_ms) return 0;
    if (g_split_every) return 1;
    uint64_t sv = vk_submit_serial(), now = host_now_ns();
    if (sv != g_split_serial) { g_split_serial = sv; g_split_t0 = now; return 0; }
    return now - g_split_t0 >= (uint64_t)g_split_ms * 1000000ull;
}
static void split_maybe(void) {
    if (!split_due()) return;
    vk_submit_split(); g_stat_splits++;
    g_split_serial = vk_submit_serial(); g_split_t0 = host_now_ns();
}
/* Splits inside a pass (TOTK_VK_SPLIT_PASS=<draws>, default 64; 0 = only between passes, as before). The G-buffer is
   one pass of ~2,200 draws that takes about as long to record (4.7 ms) as to draw (5.2 ms), and the GPU could start it
   only once all of it was recorded: the GPU sat idle ~4.4 ms a frame there (2026-10-25 timeline, 6-10 ms into the
   replay). A pass that has drawn this many draws and whose submission is due is ended (store), submitted, and opened
   again with load, the way NVN barriers already cut ~105 passes a frame at no measurable cost (sysmem rendering). */
static int pass_split_due(void) {
    static int min_draws = -1;
    if (min_draws < 0) { const char *e = getenv("TOTK_VK_SPLIT_PASS"); min_draws = e ? atoi(e) : 64; if (min_draws < 0) min_draws = 0; if (min_draws) LOGV("submission split inside passes after %d draws (TOTK_VK_SPLIT_PASS, 0 = off)", min_draws); else LOGV("submission split inside passes off (TOTK_VK_SPLIT_PASS=0)"); }
    return min_draws && !g_split_every && g_stat_draws - R.draws0 >= (uint64_t)min_draws && split_due();
}
static int scissor_full(void) {
    if (!S.has_sc) return 1;
    uint32_t w = 0, h = 0; const tex_t *t = S.has_rt && S.rt.n && S.rt.color[0] ? S.rt.color[0] : S.has_rt ? S.rt.depth : NULL;
    if (t) { w = t->d.width; h = t->d.height; }
    return S.sc.x <= 0 && S.sc.y <= 0 && (uint32_t)(S.sc.x + S.sc.w) >= w && (uint32_t)(S.sc.y + S.sc.h) >= h;
}
static VkRect2D clear_rect(void) {
    int32_t x0 = 0, y0 = 0, x1 = (int32_t)R.w, y1 = (int32_t)R.h;
    if (S.has_sc) { x0 = MAXV(x0, S.sc.x); y0 = MAXV(y0, S.sc.y); x1 = MINV(x1, S.sc.x + S.sc.w); y1 = MINV(y1, S.sc.y + S.sc.h); }
    if (x1 < x0) x1 = x0; if (y1 < y0) y1 = y0;
    return (VkRect2D){{x0, y0}, {(uint32_t)(x1 - x0), (uint32_t)(y1 - y0)}};
}
static VkClearValue color_value(int fmt_class, const float rgba[4]) {
    VkClearValue v;
    if (fmt_class == 1) for (int i = 0; i < 4; i++) v.color.uint32[i] = (uint32_t)rgba[i];
    else if (fmt_class == 2) for (int i = 0; i < 4; i++) v.color.int32[i] = (int32_t)rgba[i];
    else memcpy(v.color.float32, rgba, 16);
    return v;
}
static void clear_color(const p_clear_color *c) {
    if (S.render_disabled) { g_pops.clears_skipped++; return; }
    g_pops.clears++; memcpy(g_pops.cc, c->rgba, 16);
    if (c->index < 0 || c->index >= S.rt.n || c->index >= 8 || !S.rt.color[c->index]) { RED(4, "colour clear of target %d, which is not set", c->index); return; }
    if (!(c->mask & 0xf)) return;
    int f = S.rt.has_cview[c->index] && S.rt.cview[c->index].set_format ? S.rt.cview[c->index].format : S.rt.color[c->index]->d.format;
    VkClearValue v = color_value(vk_fmt_class(f), c->rgba);
    if (!R.open && (c->mask & 0xf) == 0xf && scissor_full()) { R.clear_c[c->index] = 1; R.cc[c->index] = v; return; }
    if ((c->mask & 0xf) != 0xf) RED(4, "colour clear with channel mask %#x: cleared whole (masked clears not done yet)", c->mask);
    g_pops.clears_inpass++;
    begin_rendering();
    VkClearAttachment ca = {VK_IMAGE_ASPECT_COLOR_BIT, (uint32_t)c->index, v};
    VkClearRect cr = {clear_rect(), 0, 1};
    if (cr.rect.extent.width && cr.rect.extent.height) vkCmdClearAttachments(vk_cb(), 1, &ca, 1, &cr);
}
static void clear_ds(const p_clear_ds *c) {
    if (S.render_disabled || !S.has_rt || !S.rt.depth) return;
    int sm = c->stencil_mask & 255; if (!c->depth_mask && !sm) return;
    int fmt = S.rt.depth->d.format;
    VkImageAspectFlags as = (c->depth_mask && vk_fmt_has_depth(fmt) ? VK_IMAGE_ASPECT_DEPTH_BIT : 0) | (sm && vk_fmt_has_stencil(fmt) ? VK_IMAGE_ASPECT_STENCIL_BIT : 0);
    if (!as) return;
    if (!R.open && (!sm || sm == 255) && scissor_full()) {
        if (as & VK_IMAGE_ASPECT_DEPTH_BIT) { R.clear_d = 1; R.cd = c->depth; }
        if (as & VK_IMAGE_ASPECT_STENCIL_BIT) { R.clear_s = 1; R.cs = (uint32_t)c->stencil; }
        return;
    }
    if (sm && sm != 255) RED(4, "stencil clear with mask %#x: cleared whole", sm);
    begin_rendering();
    VkClearAttachment ca = {as, 0, {.depthStencil = {c->depth, (uint32_t)c->stencil}}};
    VkClearRect cr = {clear_rect(), 0, 1};
    if (cr.rect.extent.width && cr.rect.extent.height) vkCmdClearAttachments(vk_cb(), 1, &ca, 1, &cr);
}

/* ---------- graphics pipelines ---------- */
typedef struct {
    void *vs, *fs;
    VkFormat cfmt[8], dfmt, sfmt; uint32_t ncolor, samples;
    blend_st blend[8]; uint32_t blend_en, chmask; uint8_t a2c, poly_mode, topo_class, depth_clamp, logic_op, pad[3];
    uint8_t dclass[8];      /* each target's format class (0 float): integer targets cannot blend */
    struct { uint32_t fmt, offset, stream; } attr[16];
    struct { uint32_t instanced, divisor; } stream[16];
} pipe_key;
static nvn_hmap g_pipes = {.klen = sizeof(pipe_key)};
static int topo_class(int prim) { return prim == 0 ? 0 : prim == 1 || prim == 2 || prim == 3 ? 1 : 2; }
static VkPrimitiveTopology topology(int prim) {
    switch (prim) {
        case 0: return VK_PRIMITIVE_TOPOLOGY_POINT_LIST; case 1: return VK_PRIMITIVE_TOPOLOGY_LINE_LIST; case 3: return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
        case 5: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP; case 6: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN;
    }
    return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;   /* triangles, quads (drawn as indexed triangles) */
}
static void pipe_key_fill(pipe_key *k, vk_shader *vs, vk_shader *fs, int prim) {
    memset(k, 0, sizeof *k);
    k->vs = vs; k->fs = fs;
    memcpy(k->cfmt, R.cfmt, sizeof k->cfmt); for (int i = 0; i < 8; i++) k->dclass[i] = (uint8_t)(i < (int)R.ncolor && R.cfmt[i] ? R.dclass[i] : 0); k->dfmt = R.dfmt; k->sfmt = R.sfmt; k->ncolor = R.ncolor; k->samples = R.samples;
    k->blend_en = S.color.blend_en; k->chmask = S.chmask.mask; k->a2c = S.ms.a2c; k->poly_mode = (uint8_t)S.poly.mode; k->topo_class = (uint8_t)topo_class(prim);
    k->depth_clamp = (uint8_t)S.depth_clamp; k->logic_op = (uint8_t)S.color.logic_op;
    for (uint32_t i = 0; i < k->ncolor && i < 8; i++) if (k->blend_en >> i & 1) k->blend[i] = S.blend[i];
    for (int a = 0; a < 16; a++) if (vs->vin_mask >> a & 1) {
        if (a < S.nattr && S.attr[a].fmt) {
            k->attr[a].fmt = S.attr[a].fmt; k->attr[a].offset = S.attr[a].offset; k->attr[a].stream = S.attr[a].stream;
            int st = S.attr[a].stream; if (st < 16) { k->stream[st].divisor = S.stream[st].divisor; k->stream[st].instanced = S.stream[st].divisor != 0 && S.stream[st].stride != 0; }
        }
    }
}
#define ZERO_BINDING 16   /* a zero vertex buffer for attributes the shader reads but the game left unset */
/* Builds the pipeline itself; callers only look it up. V.pcache is externally synchronised, so every creation takes
   g_pipecache_mu (compute pipelines in vk_shader.c take it too). */
static VkPipeline pipe_build(const pipe_key *k, vk_shader *vs, vk_shader *fs, VkPipelineLayout layout) {
    VkPipeline p;
    VkPipelineShaderStageCreateInfo st[2] = {
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vs->mod, .pName = "main"},
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = fs ? fs->mod : VK_NULL_HANDLE, .pName = "main"}};
    /* depth the fragment shader writes gets the same DEPTH16 nudge as rasterized depth (see d16tie_on) */
    float d16spec = D16_NUDGE_UNITS / 65535.0f; VkSpecializationMapEntry sme = {D16_SPEC_ID, 0, 4}; VkSpecializationInfo spi = {1, &sme, 4, &d16spec};
    if (fs && fs->depth_spec && k->dfmt == VK_FORMAT_D16_UNORM && d16tie_on()) st[1].pSpecializationInfo = &spi;
    VkVertexInputBindingDescription bd[17]; VkVertexInputAttributeDescription ad[32]; VkVertexInputBindingDivisorDescriptionEXT dd[16];
    uint32_t nb = 0, na = 0, nd = 0, used_streams = 0; int zero_used = 0;
    for (int a = 0; a < 32; a++) if (vs->vin_mask >> a & 1) {
        VkFormat vf = a < 16 && k->attr[a].fmt ? vk_vertex_fmt((int)k->attr[a].fmt) : VK_FORMAT_UNDEFINED;
        if (a < 16 && k->attr[a].fmt && !vf) RED(8, "vertex attribute format %#x has no Vulkan format", k->attr[a].fmt);
        if (!vf) {
            int cls = vs->vin_class[a];
            ad[na++] = (VkVertexInputAttributeDescription){(uint32_t)a, ZERO_BINDING, cls == 1 ? VK_FORMAT_R32G32B32A32_UINT : cls == 2 ? VK_FORMAT_R32G32B32A32_SINT : VK_FORMAT_R32G32B32A32_SFLOAT, 0};
            zero_used = 1; continue;
        }
        uint32_t s = k->attr[a].stream & 15;
        ad[na++] = (VkVertexInputAttributeDescription){(uint32_t)a, s, vf, k->attr[a].offset};
        used_streams |= 1u << s;
    }
    for (uint32_t s = 0; s < 16; s++) if (used_streams >> s & 1) {
        bd[nb++] = (VkVertexInputBindingDescription){s, 0, k->stream[s].instanced ? VK_VERTEX_INPUT_RATE_INSTANCE : VK_VERTEX_INPUT_RATE_VERTEX};
        if (k->stream[s].instanced && k->stream[s].divisor > 1) {
            if (V.has_divisor) dd[nd++] = (VkVertexInputBindingDivisorDescriptionEXT){s, k->stream[s].divisor};
            else RED(4, "instance divisor %u needs VK_EXT_vertex_attribute_divisor", k->stream[s].divisor);
        }
    }
    if (zero_used) bd[nb++] = (VkVertexInputBindingDescription){ZERO_BINDING, 0, VK_VERTEX_INPUT_RATE_VERTEX};
    VkPipelineVertexInputDivisorStateCreateInfoEXT dsi = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_DIVISOR_STATE_CREATE_INFO_EXT, .vertexBindingDivisorCount = nd, .pVertexBindingDivisors = dd};
    VkPipelineVertexInputStateCreateInfo vi = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO, nd ? &dsi : NULL, 0, nb, bd, na, ad};
    VkPipelineInputAssemblyStateCreateInfo ia = {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = k->topo_class == 0 ? VK_PRIMITIVE_TOPOLOGY_POINT_LIST : k->topo_class == 1 ? VK_PRIMITIVE_TOPOLOGY_LINE_LIST : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    VkPipelineViewportStateCreateInfo vp = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1, .scissorCount = 1};
    VkPipelineRasterizationStateCreateInfo rs = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .depthClampEnable = k->depth_clamp && V.props.limits.maxViewports, .polygonMode = k->poly_mode == NVN_POLYGON_MODE_LINE ? VK_POLYGON_MODE_LINE : k->poly_mode == NVN_POLYGON_MODE_POINT ? VK_POLYGON_MODE_POINT : VK_POLYGON_MODE_FILL, .lineWidth = 1};
    VkPipelineMultisampleStateCreateInfo ms = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, .rasterizationSamples = k->samples ? (VkSampleCountFlagBits)k->samples : VK_SAMPLE_COUNT_1_BIT, .alphaToCoverageEnable = k->a2c & 1};
    VkPipelineDepthStencilStateCreateInfo ds = {VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    VkPipelineColorBlendAttachmentState cba[8];
    for (uint32_t i = 0; i < k->ncolor && i < 8; i++) {
        uint32_t m = k->chmask >> (4 * i) & 0xf;
        cba[i] = (VkPipelineColorBlendAttachmentState){.colorWriteMask = (m & 1 ? VK_COLOR_COMPONENT_R_BIT : 0) | (m & 2 ? VK_COLOR_COMPONENT_G_BIT : 0) | (m & 4 ? VK_COLOR_COMPONENT_B_BIT : 0) | (m & 8 ? VK_COLOR_COMPONENT_A_BIT : 0)};
        if (k->blend_en >> i & 1 && k->cfmt[i] && k->dclass[i] == 0) {   /* from the key: this may run on a pipeline worker, where R is another pass's */
            cba[i].blendEnable = VK_TRUE;
            cba[i].srcColorBlendFactor = vk_blend_factor(k->blend[i].src_rgb); cba[i].dstColorBlendFactor = vk_blend_factor(k->blend[i].dst_rgb); cba[i].colorBlendOp = vk_blend_op(k->blend[i].eq_rgb);
            cba[i].srcAlphaBlendFactor = vk_blend_factor(k->blend[i].src_a); cba[i].dstAlphaBlendFactor = vk_blend_factor(k->blend[i].dst_a); cba[i].alphaBlendOp = vk_blend_op(k->blend[i].eq_a);
        }
        if (!k->cfmt[i]) cba[i].colorWriteMask = 0;
    }
    VkPipelineColorBlendStateCreateInfo cb = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, .attachmentCount = k->ncolor, .pAttachments = cba};
    if (k->logic_op != NVN_LOGIC_OP_COPY && k->logic_op && V.has_logic_op) { cb.logicOpEnable = VK_TRUE; cb.logicOp = (VkLogicOp)(k->logic_op - 1 > 15 ? 3 : k->logic_op - 1); }
    static const VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_DEPTH_BIAS, VK_DYNAMIC_STATE_BLEND_CONSTANTS,
        VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK, VK_DYNAMIC_STATE_STENCIL_WRITE_MASK, VK_DYNAMIC_STATE_STENCIL_REFERENCE, VK_DYNAMIC_STATE_CULL_MODE, VK_DYNAMIC_STATE_FRONT_FACE,
        VK_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY, VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE, VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE, VK_DYNAMIC_STATE_DEPTH_COMPARE_OP,
        VK_DYNAMIC_STATE_STENCIL_TEST_ENABLE, VK_DYNAMIC_STATE_STENCIL_OP, VK_DYNAMIC_STATE_DEPTH_BIAS_ENABLE, VK_DYNAMIC_STATE_PRIMITIVE_RESTART_ENABLE,
        VK_DYNAMIC_STATE_VERTEX_INPUT_BINDING_STRIDE, VK_DYNAMIC_STATE_LINE_WIDTH};
    VkPipelineDynamicStateCreateInfo dy = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, .dynamicStateCount = sizeof dyn / sizeof *dyn, .pDynamicStates = dyn};
    VkPipelineRenderingCreateInfo pr = {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO, .colorAttachmentCount = k->ncolor, .pColorAttachmentFormats = k->cfmt,
        .depthAttachmentFormat = k->dfmt, .stencilAttachmentFormat = k->sfmt};
    VkGraphicsPipelineCreateInfo ci = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, &pr, .stageCount = fs ? 2 : 1, .pStages = st, .pVertexInputState = &vi,
        .pInputAssemblyState = &ia, .pViewportState = &vp, .pRasterizationState = &rs, .pMultisampleState = &ms, .pDepthStencilState = &ds, .pColorBlendState = &cb,
        .pDynamicState = &dy, .layout = layout};
    uint64_t t0 = host_now_ns();
    pthread_mutex_lock(&g_pipecache_mu);
    VkResult r = vkCreateGraphicsPipelines(V.dev, V.pcache, 1, &ci, NULL, &p);
    pthread_mutex_unlock(&g_pipecache_mu);
    if (r) { RED(16, "graphics pipeline (vs %.20s fs %.20s): %d", vs->key, fs ? fs->key : "-", r); p = VK_NULL_HANDLE; HK_ADD(pipes_fail, 1); }
    g_stat_pipes++;
    { uint64_t ns_ = host_now_ns() - t0; HK_ADD(pipes, 1); HK_ADD(pipe_ns, ns_); }
    uint64_t ms_ = (host_now_ns() - t0) / 1000000;
    if (ms_ > 50 && g_stat_pipes < 2000) { static int n; if (n++ < 10) LOGV("pipeline %llu took %llu ms (vs %.16s fs %.16s)", (unsigned long long)g_stat_pipes, (unsigned long long)ms_, vs->key, fs ? fs->key : "-"); }
    return p;
}

/* ---------- pipelines built on a worker (TOTK, 2026-10-22) ----------
   vkCreateGraphicsPipelines on Adreno takes tens to over a thousand ms and the recording thread sat in it: the hitch
   profiler showed hitches of exactly that shape (frame 2720: 47 pipelines, 1356 ms). Now a miss copies the key to a
   ring, the worker builds and caches the pipeline, and the draw is skipped for that submission (the "pipeline" skip
   counter, one frame of missing geometry per material). TOTK_VK_OFF=pipeasync turns it off (built synchronously as
   before). */
typedef struct { pipe_key k; vk_shader *vs, *fs; VkPipelineLayout layout; } pipe_job;
#define PJOBS 256
static pthread_mutex_t g_pipe_mu; static pthread_cond_t g_pj_cv;
pthread_mutex_t g_pipecache_mu = PTHREAD_MUTEX_INITIALIZER;   /* defined here, vk_shader.c takes it for compute pipelines */
static pipe_job g_pjobs[PJOBS]; static unsigned g_pj_head, g_pj_tail;
/* Submissions a key's draws were skipped waiting for its build. Bounded (TOTK, 2026-10-22): past PIPE_WAIT_MAX the
   next miss builds synchronously - one hitch instead of geometry that never appears (the loading-screen map went
   black while the worker chewed through an area's pipeline batch). */
static nvn_hmap g_pipe_waits = {.klen = sizeof(pipe_key)};
/* TOTK (2026-10-22, later): 90 skipped submissions (~3 s) left terrain / shadow-caster draws out long enough to see:
   lit tiles lost their sun term (the black squares) and shadow maps lost casters. Two submissions now, then a
   synchronous build; the pipeline cache on disk (nvn_vk.c) makes those builds cheap after the first time.
   TOTK_PIPE_WAIT_MAX=<n> to tune. */
static uint64_t pipe_wait_max(void) { static long v = -1; if (v < 0) { const char *e = getenv("TOTK_PIPE_WAIT_MAX"); v = e && *e ? atol(e) : 2; if (v < 0) v = 0; } return (uint64_t)v; }
#define PIPE_WAIT_MAX pipe_wait_max()
static void pipe_mu_init(void) {
    static int once; if (!once) { once = 1; pthread_mutex_init(&g_pipe_mu, NULL); pthread_cond_init(&g_pj_cv, NULL); }
}
static void *pipe_worker(void *_) {
    (void)_;
    setpriority(PRIO_PROCESS, 0, 10);   /* the driver's compile burns CPU for seconds: the game's threads go first */
    for (;;) {
        pipe_mu_init();
        pthread_mutex_lock(&g_pipe_mu);
        while (g_pj_head == g_pj_tail) pthread_cond_wait(&g_pj_cv, &g_pipe_mu);
        pipe_job j = g_pjobs[g_pj_head]; g_pj_head = (g_pj_head + 1) % PJOBS;
        pthread_mutex_unlock(&g_pipe_mu);
        VkPipeline p = pipe_build(&j.k, j.vs, j.fs, j.layout);
        pthread_mutex_lock(&g_pipe_mu);
        nvn_hm_put(&g_pipes, &j.k, p ? (void *)p : (void *)(uintptr_t)1);   /* 1: failed, do not retry every draw */
        pthread_mutex_unlock(&g_pipe_mu);
    }
    return NULL;
}
static VkPipeline pipe_get_slow(const pipe_key *k, vk_shader *vs, vk_shader *fs, VkPipelineLayout layout) {
    pipe_mu_init();
    static int async = -1;
    /* Off by default (TOTK, 2026-10-22 night): a skipped draw is not just one frame of missing geometry. The lighting
       works from per-tile flags the previous frame's draws leave, so tiles whose draws were skipped dropped out and
       stayed out; standing still, the picture degraded over time, and the loading-screen map stayed black. Synchronous
       building fixed both on the Thor and hitched no worse (the pipeline cache on disk). TOTK_PIPE_ASYNC=1: the worker. */
    if (async < 0) { const char *e = getenv("TOTK_PIPE_ASYNC"); async = e && atoi(e) > 0 && !vk_off("pipeasync");
        LOGV("pipelines: %s (TOTK_PIPE_ASYNC=1 for the worker)", async ? "built on a worker thread, draws skipped meanwhile" : "built synchronously on the recording thread"); }
    pthread_mutex_lock(&g_pipe_mu);
    VkPipeline p = (VkPipeline)nvn_hm_get(&g_pipes, k);
    if (p) nvn_hm_del(&g_pipe_waits, k);
    if (!p && async) {
        /* the waits map holds the submission of the key's first miss (+1): skipped for at most PIPE_WAIT_MAX submissions */
        uint64_t sub = vk_submit_serial(), first = (uint64_t)(uintptr_t)nvn_hm_get(&g_pipe_waits, k);
        if (!first) first = sub + 1;
        uint64_t waited = sub + 1 - first;
        if (waited >= PIPE_WAIT_MAX) {   /* geometry must appear: one synchronous build (a hitch, not a hole) */
            nvn_hm_del(&g_pipe_waits, k);
            RED(4, "pipeline still unbuilt after %llu submissions: building synchronously", (unsigned long long)waited);
            pthread_mutex_unlock(&g_pipe_mu);
            HK_ADD(pipes_sync, 1);
            p = pipe_build(k, vs, fs, layout);
            pthread_mutex_lock(&g_pipe_mu);
            nvn_hm_put(&g_pipes, k, p ? (void *)p : (void *)(uintptr_t)1);
            pthread_mutex_unlock(&g_pipe_mu);
            return p;
        }
        nvn_hm_put(&g_pipe_waits, k, (void *)(uintptr_t)first);
        int queued = 0;
        for (unsigned i = g_pj_head; i != g_pj_tail; i = (i + 1) % PJOBS) if (!memcmp(&g_pjobs[i].k, k, sizeof *k)) { queued = 1; break; }
        if (queued || (g_pj_tail + 1) % PJOBS != g_pj_head) {   /* duplicates are skipped too; a full ring falls through to sync */
            if (!queued) {
                pipe_job *j = &g_pjobs[g_pj_tail]; g_pj_tail = (g_pj_tail + 1) % PJOBS;
                j->k = *k; j->vs = vs; j->fs = fs; j->layout = layout;
                /* two builders by default (TOTK_PIPE_WORKERS): one builder at ~10-40 ms a pipeline loses to the stream */
                static int started; if (!started) {
                    started = 1; int n = 2; { const char *e = getenv("TOTK_PIPE_WORKERS"); if (e && *e) { n = atoi(e); if (n < 1) n = 1; if (n > 4) n = 4; } }
                    for (int w = 0; w < n; w++) { pthread_t th; if (!pthread_create(&th, NULL, pipe_worker, NULL)) pthread_detach(th); }
                }
                pthread_cond_signal(&g_pj_cv);
            }
            pthread_mutex_unlock(&g_pipe_mu);
            return NULL;   /* the draw is skipped this submission; the worker fills the cache for the next */
        }
        RED(8, "pipeline job ring full (%d): the worker is behind, building synchronously", PJOBS);
    }
    pthread_mutex_unlock(&g_pipe_mu);
    if (p) return p;
    HK_ADD(pipes_sync, 1);
    p = pipe_build(k, vs, fs, layout);
    pthread_mutex_lock(&g_pipe_mu);
    nvn_hm_put(&g_pipes, k, p ? (void *)p : (void *)(uintptr_t)1);
    pthread_mutex_unlock(&g_pipe_mu);
    return p;
}

static uint64_t fnv(const void *p, size_t n, uint64_t h);
/* Per-thread front cache of built pipelines (TOTK, 2026-10-23: the map's mutex and two hash operations on every draw;
   built pipelines are never removed). TOTK_VK_OFF=pfront: off. */
static VkPipeline pipe_get(const pipe_key *k, vk_shader *vs, vk_shader *fs, VkPipelineLayout layout) {
    enum { PF = 1024 };
    typedef struct { pipe_key k; VkPipeline p; } pf_ent;
    static __thread pf_ent *pf;
    static int off = -1; if (off < 0) off = vk_off("pfront");
    if (off) return pipe_get_slow(k, vs, fs, layout);
    if (!pf && !(pf = calloc(PF, sizeof *pf))) return pipe_get_slow(k, vs, fs, layout);
    pf_ent *e = &pf[fnv(k, sizeof *k, 0x84222325cbf29ce4ull) & (PF - 1)];
    if (e->p && !memcmp(&e->k, k, sizeof *k)) return e->p;
    VkPipeline p = pipe_get_slow(k, vs, fs, layout);
    if (p && p != (VkPipeline)(uintptr_t)1) { e->k = *k; e->p = p; }
    return p;
}

/* ---------- descriptor sets ---------- */
static void driver_bank_fill(uint8_t *d, int bank, int stage, const uint32_t grid[3]) {   /* nvn_mtl.m driver_bank_fill */
    memset(d, 0, 0xA00);
    if (bank == 0) {
        int base = stage == 0 ? 0x110 : stage == 5 ? 0x310 : stage == 1 ? 0x510 : -1;
        if (base >= 0) for (int r = 0; r < 16; r++) { uint64_t a = S.ssbo[stage][r].addr; uint32_t n = (uint32_t)S.ssbo[stage][r].size; memcpy(d + base + 16 * r, &a, 8); memcpy(d + base + 16 * r + 8, &n, 4); }
        if (stage == 5) {
            if (grid) memcpy(d + 0x200, grid, 12);
            for (int r = 0; r < 16; r++) { uint64_t a = S.ubo[5][r].addr; uint32_t n = (uint32_t)S.ubo[5][r].size; memcpy(d + 0x210 + 16 * r, &a, 8); memcpy(d + 0x210 + 16 * r + 8, &n, 4); }
        } else if (stage == 0) { const float ps_max = 2047.9375f; memcpy(d + 0x610, &ps_max, sizeof ps_max); }
    } else {
        for (int r = 0; r < 128; r++) { uint32_t th = (uint32_t)S.tex[stage][r], sh_ = (uint32_t)S.samp[stage][r]; memcpy(d + 0x168 + 8 * r, &th, 4); memcpy(d + 0x568 + 8 * r, &sh_, 4); }
        for (int r = 0; r < 32; r++) { uint32_t th = (uint32_t)S.tex[stage][r]; memcpy(d + 0x20 + 8 * r, &th, 4); }
    }
}
static void support_fill(uint8_t *d, uint32_t w, uint32_t h) {   /* Ryujinx SupportBuffer: render scale 1, no BGRA swap */
    memset(d, 0, 1392);
    float vi[4] = {2.0f / (w ? w : 1), 2.0f / (h ? h : 1), 1, 1}, vs[4] = {(float)w, (float)h, 1, 1};
    memcpy(d + 144, vi, 16); memcpy(d + 160, vs, 16);
    for (int i = 0; i < 73; i++) { float one = 1; memcpy(d + 192 + 16 * i, &one, 4); }
}
typedef struct { VkDescriptorBufferInfo b; VkDescriptorImageInfo i; VkBufferView t; } dinfo;
static int ubo_desc(uint64_t addr, uint64_t size, VkDescriptorBufferInfo *out, int storage, int rw) {
    VkBuffer b; uint64_t off, avail;
    if (!addr || !vk_resolve_span(addr, &b, &off, &avail)) return 0;
    if (!storage) { uint64_t n = MINV(size ? size : 0x10000, MINV(avail, V.props.limits.maxUniformBufferRange)); if (vk_ub_rename(addr, n, out)) return 1; }
    else {   /* a storage binding sees the logged updates in memory; draw_touch wrote them before the pass opened */
        uint64_t n = MINV(size ? size : avail, avail);
        if (rw) vk_gpu_write(addr, n);   /* the shader may write it: no uniform snapshot of it from here on */
        if (!R.open) vk_ub_touch(addr, n);
        else if (vk_ub_pending(addr, n)) RED(4, "storage buffer %#llx overlaps a uniform update logged inside the pass: it reads the bytes from before", (unsigned long long)addr);
    }
    VkDeviceSize align = storage ? V.props.limits.minStorageBufferOffsetAlignment : V.props.limits.minUniformBufferOffsetAlignment;
    uint64_t lim = storage ? V.props.limits.maxStorageBufferRange : V.props.limits.maxUniformBufferRange;
    if (storage) {   /* TOTK_VK_SSBO_RANGE=<bytes> (experiment): bind storage buffers past the advertised limit */
        static uint64_t over = UINT64_MAX; if (over == UINT64_MAX) { const char *e = getenv("TOTK_VK_SSBO_RANGE"); over = e ? strtoull(e, NULL, 0) : 0; if (over) LOGV("TOTK_VK_SSBO_RANGE: storage buffers bound up to %llu bytes", (unsigned long long)over); }
        if (over) lim = over;
    }
    if (storage && (size ? size : avail) > lim) RED(8, "storage buffer at %#llx: %llu bytes available, the device binds at most %llu (maxStorageBufferRange): reads past that are undefined",
                                                   (unsigned long long)addr, (unsigned long long)(size ? size : avail), (unsigned long long)lim);
    if (!size || size > lim) size = lim;
    if (off % align) {   /* not aligned for the device: a copy in this submission's memory (uniforms only change at submission boundaries) */
        uint64_t n = MINV(size, avail);
        VkBuffer tb; VkDeviceSize to; uint8_t *p = vk_transient(n, align, &tb, &to);
        memcpy(p, (const void *)(uintptr_t)addr, n);
        if (storage) RED(4, "storage buffer at %#llx is not %llu-aligned: bound as a copy (GPU writes to it are lost)", (unsigned long long)addr, (unsigned long long)align);
        *out = (VkDescriptorBufferInfo){tb, to, n}; return 1;
    }
    *out = (VkDescriptorBufferInfo){b, off, MINV(size, avail)};   /* clamped to the pool's buffer */
    return 1;
}
static VkBuffer g_zero_ubo; static VkDeviceSize g_zero_ubo_off;
static void zero_buffer(VkDescriptorBufferInfo *out, size_t size) {
    VkBuffer b; VkDeviceSize o; void *p = vk_transient(size, V.props.limits.minUniformBufferOffsetAlignment, &b, &o); memset(p, 0, size);
    *out = (VkDescriptorBufferInfo){b, o, size};
}
/* Small uniform blocks the backend fills per draw (driver banks, support buffer): a block with the same bytes as the
   last one of its kind in this submission reuses that copy, so the descriptor sets of consecutive draws can match. */
static void block_desc(int key, const void *data, size_t n, VkDescriptorBufferInfo *out) {
    static struct { uint64_t sub; size_t n; uint8_t *bytes; VkDescriptorBufferInfo d; } last[32];
    uint64_t sub = vk_submit_serial();
    static int off = -1; if (off < 0) off = vk_off("block");
    if (!off && key >= 0 && key < 32 && last[key].sub == sub && last[key].n == n && !memcmp(last[key].bytes, data, n)) { *out = last[key].d; return; }
    VkBuffer tb; VkDeviceSize to; uint8_t *p = vk_transient(n, V.props.limits.minUniformBufferOffsetAlignment, &tb, &to); memcpy(p, data, n);
    *out = (VkDescriptorBufferInfo){tb, to, n};
    if (key >= 0 && key < 32) { last[key].bytes = realloc(last[key].bytes, n); memcpy(last[key].bytes, data, n); last[key].n = n; last[key].sub = sub; last[key].d = *out; }
}
/* Descriptor sets already written in this submission, by their contents: a draw that binds the same resources as an
   earlier one reuses its set instead of allocating and writing a new one. */
enum { DS_CACHE = 2048 };
static struct { uint64_t sub, hash; VkDescriptorSetLayout dsl; size_t n, cap; dinfo *info; VkDescriptorSet set; } g_dsc[DS_CACHE];
static struct { uint64_t sub; VkPipelineLayout layout; VkDescriptorSet set; } g_bound_set[2][6];   /* [graphics, compute][set index] */
static uint64_t g_stat_sets_new, g_stat_sets_reused;
/* the descriptor-set cache's hash (TOTK, 2026-10-23: was FNV-1a byte by byte over ~1 KB per stage per draw, most of
   bind_stage's own time): eight bytes a step; the cache compares the contents anyway */
static uint64_t fnv(const void *p, size_t n, uint64_t h) {
    const uint8_t *b = p; size_t i = 0;
    for (; i + 8 <= n; i += 8) { uint64_t w; memcpy(&w, b + i, 8); h = (h ^ w) * 0x9E3779B97F4A7C15ull; h ^= h >> 29; }
    for (; i < n; i++) { h ^= b[i]; h *= 0x100000001b3ull; }
    return h;
}
/* Fill and bind one stage's descriptor set; stage: NVN stage. Returns 0 when a binding cannot be satisfied. */
/* Depth-compare samplers (TOTK on Turnip, 2026-10-23: the grey lighting squares; Eden PR 4324 fixed squares on the
   Thor with the second rule). Maxwell applies a sampler's compare only in TEX.DC; Vulkan applies it (undefined) to
   every sample through it. 1 = bind the sampler without compare:
   - the shader samples this binding plainly (its SPIR-V image is not Depth = 1);
   - the view's format has no depth-compare support (Eden: drop_depth_comparison).
   TOTK_VK_OFF=cmpfix keeps the compare everywhere (the old behaviour). */
static uint64_t g_stat_cmp_plain, g_stat_cmp_fmt, g_stat_cmp_sep;
static int cmp_fix(const vk_bind *b, const vk_shader *sh, int tid) {
    static int off = -1; if (off < 0) off = vk_off("cmpfix");
    int f = vk_treg_fmt(tid); VkFormat vf = VK_FORMAT_UNDEFINED; if (f > 0) vk_fmt(f, &vf, NULL);
    int why = !b->shadow ? 1 : (vf != VK_FORMAT_UNDEFINED && !vk_fmt_cmp_ok(vf)) ? 2 : 0;
    if (!why) return 0;
    uint64_t n = why == 1 ? ++g_stat_cmp_plain : ++g_stat_cmp_fmt;
    if (n <= 6) LOGV("compare sampler on %s (shader %.16s binding %u, nvn format %#x -> vk %d)%s", why == 1 ? "a plainly sampled texture" : "a format without depth compare",
                     sh->key, b->binding, f, (int)vf, off ? " [kept: TOTK_VK_OFF=cmpfix]" : ": compare dropped");
    return !off;
}

/* a uniform binding's size: only the bytes the shader can read (vk_shader.c spirv_ubo_extent), within the game's */
static uint64_t ub_size(const vk_bind *b, uint64_t size, int storage) {
    if (storage || !b->used) return size;
    return size && size < b->used ? size : b->used;
}
static int bind_stage(VkCommandBuffer cb, VkPipelineBindPoint bp, VkPipelineLayout layout, uint32_t set_index, vk_shader *sh, int stage, const uint32_t grid[3]) {
    if (!sh->nb) return 1;
    size_t total = 0; for (int i = 0; i < sh->nb; i++) total += sh->b[i].count;
    static VkWriteDescriptorSet *w; static dinfo *info; static size_t capw, capi;
    if ((size_t)sh->nb > capw) { capw = (size_t)sh->nb + 16; w = realloc(w, capw * sizeof *w); }
    if (total > capi) { capi = total + 64; info = realloc(info, capi * sizeof *info); }
    memset(info, 0, total * sizeof *info);
    int k = 0;
    for (int i = 0; i < sh->nb; i++) {
        const vk_bind *b = &sh->b[i];
        dinfo *in = &info[k]; k += b->count;
        w[i] = (VkWriteDescriptorSet){VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = b->binding, .descriptorCount = b->count, .descriptorType = (VkDescriptorType)b->dtype,
            .pBufferInfo = &in->b, .pImageInfo = &in->i, .pTexelBufferView = &in->t};
        for (int e = 0; e < b->count; e++) {
            dinfo *d = &in[e];
            /* buffers */
            if (b->dtype == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER || b->dtype == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER) {
                int storage = b->dtype == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, ok = 0;
                if (b->kind == RB_SUPPORT) {   /* depends on the render size only: built once per size and submission (TOTK, 2026-10-25: the fill and block_desc's 1.4 KB compare were ~1.5 % of the replay) */
                    static uint64_t ssub; static uint32_t sw, shh; static VkDescriptorBufferInfo sd; static int soff = -1; uint64_t sub = vk_submit_serial();
                    if (soff < 0) soff = vk_off("block");
                    if (ssub != sub || sw != R.w || shh != R.h || soff) { uint8_t blk[1392]; support_fill(blk, R.w, R.h); block_desc(0, blk, sizeof blk, &sd); ssub = sub; sw = R.w; shh = R.h; }
                    d->b = sd; ok = 1;
                }
                else if (b->kind == RB_CBUF && b->reg >= 0 && b->reg < 16) ok = ubo_desc(S.ubo[stage][b->reg].addr, ub_size(b, S.ubo[stage][b->reg].size, storage), &d->b, storage, storage && b->rw);
                else if (b->kind == RB_C1) {
                    uint64_t sub = vk_submit_serial();
                    static int c1off = -1; if (c1off < 0) c1off = vk_off("block");
                    if (c1off || sh->c1_sub != sub) { size_t n = (sh->c1_len + 15) & ~(size_t)15; if (!n) n = 16; VkBuffer tb; VkDeviceSize to; uint8_t *p = vk_transient(n, V.props.limits.minUniformBufferOffsetAlignment, &tb, &to); memset(p, 0, n); if (sh->c1_len) memcpy(p, sh->c1, sh->c1_len); sh->c1_d = (VkDescriptorBufferInfo){tb, to, n}; sh->c1_sub = sub; }
                    d->b = sh->c1_d; ok = 1;
                }
                else if (b->kind == RB_DRV0 || b->kind == RB_DRV2) { uint8_t blk[0xA00]; driver_bank_fill(blk, b->kind == RB_DRV2 ? 2 : 0, stage, grid); block_desc(1 + (b->kind == RB_DRV2) * 6 + stage, blk, sizeof blk, &d->b); ok = 1; }
                else if (b->kind == RB_SBUF && b->reg >= 0 && b->reg < 16) ok = ubo_desc(S.ssbo[stage][b->reg].addr, S.ssbo[stage][b->reg].size, &d->b, storage, storage && b->rw);
                else if (b->kind == RB_UBO_PTR && b->reg >= 0 && b->reg < 16) ok = ubo_desc(S.ubo[stage][b->reg].addr, ub_size(b, S.ubo[stage][b->reg].size, storage), &d->b, storage, storage && b->rw);
                else if (b->kind == RB_SBUF_UBO && b->reg >= 0 && b->reg < 16) {
                    const p_bind_buf *u = &S.ubo[stage][b->reg]; uint64_t va = 0;
                    /* the pointer as this draw's uniforms hold it, not memory: inline uniform updates are logged and laid over
                       memory per draw, and memory already has the last one (the Depths terrain changes it between its draws) */
                    if (u->addr && u->size >= (uint64_t)b->off + 8) vk_ub_peek(u->addr + b->off, 8, &va);
                    ok = va && ubo_desc(va, 0, &d->b, storage, storage && b->rw);
                    if (!ok) RED(8, "storage buffer pointer in uniform slot %d+%u (%#llx) is outside the pools", b->reg, b->off, (unsigned long long)va);
                }
                if (storage && b->rw && R.open) R.wrote_storage = 1;   /* barriersmart: a later barrier in this pass splits it */
                if (!ok) { zero_buffer(&d->b, storage ? 256 : 256); if (b->kind != RB_SBUF_UBO) { g_pzero++; g_stat_zero_buf++; } }
                else if (probe_on() && (b->kind == RB_CBUF || b->kind == RB_SBUF || b->kind == RB_UBO_PTR || b->kind == RB_SBUF_UBO) && b->reg >= 0 && b->reg < 16)
                    pass_buf_note(stage, b->reg + (b->kind == RB_SBUF ? 16 : 0), storage, b->kind == RB_SBUF ? S.ssbo[stage][b->reg].addr : S.ubo[stage][b->reg].addr, &d->b);
                continue;
            }
            /* texel buffers */
            if (b->dtype == VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER || b->dtype == VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER) {
                int slot = b->kind == RB_IMG ? b->reg : b->reg;
                uint64_t h = slot >= 0 ? (b->kind == RB_IMG ? (slot < 32 ? S.img[stage][slot] : 0) : (slot < 128 ? S.tex[stage][slot] : 0)) : 0;
                d->t = h ? vk_tbuf_by_id((int)(h & 0xfffff)) : VK_NULL_HANDLE;
                if (!d->t) d->t = vk_dummy_tbuf();
                continue;
            }
            /* images and samplers */
            int tslot = b->reg, target = b->target;
            if (b->dtype == VK_DESCRIPTOR_TYPE_SAMPLER) {
                int sid = 0;
                if (b->kind == RB_SAMP && tslot >= 0 && tslot < 128) sid = (int)((b->sreg == -2 ? S.tex[stage][tslot] : S.samp[stage][tslot]) >> 20);
                d->i.sampler = sid ? vk_samp_by_id(sid) : VK_NULL_HANDLE;
                if (!d->i.sampler) d->i.sampler = vk_dummy_sampler();
                if (sid && vk_samp_cmp(sid)) g_stat_cmp_sep++;
                continue;
            }
            VkImageView view = VK_NULL_HANDLE; vk_tex *x = NULL;
            if (b->dtype == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE) {
                if (tslot >= 0 && tslot < 32 && S.img[stage][tslot]) { int want = -2; view = vk_tex_by_id((int)(S.img[stage][tslot] & 0xfffff), &want, &x); }
                if (x) { vk_tex_mark_gpu_written(x); probe_write_note('X', x, NULL, 0); }
                if (R.open) R.wrote_storage = 1;   /* barriersmart */
                d->i.imageView = view ? view : vk_dummy_view(target, b->is_int); d->i.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
                continue;
            }
            if (tslot >= 0 && tslot < 128 && S.tex[stage][tslot]) {
                int want = target; view = vk_tex_by_id((int)(S.tex[stage][tslot] & 0xfffff), &want, &x);
                if (!view) { g_pdummy++; uint64_t n = ++g_stat_dummy_tex;
                    if (n <= 16) LOGV("texture id %d (stage %d slot %d, shader %.16s) resolves to nothing: an all-zero dummy is bound (%llu so far)", (int)(S.tex[stage][tslot] & 0xfffff), stage, tslot, sh->key, (unsigned long long)n); }
                else if (x && R.open) {
                    uint64_t im = (uint64_t)(uintptr_t)vk_tex_image(x); int k = 0; while (k < g_npin && g_pin[k].img != im) k++;
                    if (k == g_npin && g_npin < 16) g_pin[g_npin++] = (probe_in){im, vk_tex_width(x, 0), vk_tex_height(x, 0), vk_tex_nvn_format(x), x};
                }
            }
            if (stage == 1 && R.open && !g_pkey[0]) snprintf(g_pkey, sizeof g_pkey, "%.16s", sh->key);
            d->i.imageView = view ? view : vk_dummy_view(target, b->is_int); d->i.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
            if (b->dtype == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER) {
                int sid = 0;
                if (tslot >= 0 && tslot < 128) sid = (int)((b->sreg >= 0 && b->sreg < 128 ? S.samp[stage][b->sreg] : S.tex[stage][tslot]) >> 20);
                int nocmp = 0;
                if (sid && view && vk_samp_cmp(sid)) nocmp = cmp_fix(b, sh, (int)(S.tex[stage][tslot] & 0xfffff));
                d->i.sampler = sid ? (nocmp ? vk_samp_by_id_nocmp(sid) : vk_samp_by_id(sid)) : VK_NULL_HANDLE;
                if (!d->i.sampler) d->i.sampler = vk_dummy_sampler();
            }
            if (b->count > 1 && e == 0) RED(4, "texture array binding (%u elements, slot %d): every element shows the first texture", b->count, tslot);
        }
        (void)g_zero_ubo; (void)g_zero_ubo_off;
    }
    if (sh->push) {   /* TOTK_VK_PUSH (vk_shader.c push_layout): straight into the command buffer, unless the same as the last push */
        int pi = bp == VK_PIPELINE_BIND_POINT_COMPUTE; uint64_t psub = vk_submit_serial();
        static struct { uint64_t sub; VkPipelineLayout layout; const vk_shader *sh; size_t n, cap; dinfo *info; } last[2];
        if (last[pi].sub == psub && last[pi].layout == layout && last[pi].sh == sh && last[pi].n == total && !memcmp(last[pi].info, info, total * sizeof *info)) { g_stat_sets_reused++; return 1; }
        vkCmdPushDescriptorSetKHR(cb, bp, layout, set_index, (uint32_t)sh->nb, w);
        last[pi].sub = psub; last[pi].layout = layout; last[pi].sh = sh; last[pi].n = total;
        if (last[pi].cap < total) { last[pi].cap = total + 8; last[pi].info = realloc(last[pi].info, last[pi].cap * sizeof *info); }
        memcpy(last[pi].info, info, total * sizeof *info);
        g_stat_sets_new++;
        return 1;
    }
    uint64_t sub = vk_submit_serial(), h = fnv(info, total * sizeof *info, fnv(&sh->dsl, sizeof sh->dsl, 0xcbf29ce484222325ull));
    unsigned slot = (unsigned)(h % DS_CACHE);
    VkDescriptorSet set;
    static int dsoff = -1; if (dsoff < 0) dsoff = vk_off("ds");
    if (!dsoff && g_dsc[slot].sub == sub && g_dsc[slot].hash == h && g_dsc[slot].dsl == sh->dsl && g_dsc[slot].n == total && !memcmp(g_dsc[slot].info, info, total * sizeof *info)) {
        set = g_dsc[slot].set; g_stat_sets_reused++;
    } else {
        set = vk_alloc_set(sh->dsl);
        for (int i = 0; i < sh->nb; i++) w[i].dstSet = set;
        vkUpdateDescriptorSets(V.dev, (uint32_t)sh->nb, w, 0, NULL);
        g_dsc[slot].sub = sub; g_dsc[slot].hash = h; g_dsc[slot].dsl = sh->dsl; g_dsc[slot].n = total; g_dsc[slot].set = set;
        if (g_dsc[slot].cap < total) { g_dsc[slot].cap = total + 8; g_dsc[slot].info = realloc(g_dsc[slot].info, g_dsc[slot].cap * sizeof *info); }   /* grows only: was a realloc per new set */
        memcpy(g_dsc[slot].info, info, total * sizeof *info);
        g_stat_sets_new++;
    }
    int bi = bp == VK_PIPELINE_BIND_POINT_COMPUTE;
    if (!dsoff && set_index < 6 && g_bound_set[bi][set_index].sub == sub && g_bound_set[bi][set_index].set == set && g_bound_set[bi][set_index].layout == layout) return 1;
    vkCmdBindDescriptorSets(cb, bp, layout, set_index, 1, &set, 0, NULL);
    if (set_index < 6) { g_bound_set[bi][set_index].sub = sub; g_bound_set[bi][set_index].set = set; g_bound_set[bi][set_index].layout = layout; }
    for (uint32_t j = set_index + 1; j < 6; j++) g_bound_set[bi][j].sub = 0;   /* a later set may have been disturbed */
    return 1;
}

/* ---------- draws ---------- */
static VkBuffer g_quad_ib; static uint32_t g_quad_max;
static void quad_indices(uint32_t quads) {
    if (g_quad_ib && quads <= g_quad_max) return;
    uint32_t n = quads < 65536 ? 65536 : quads;
    VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = (VkDeviceSize)n * 24, .usage = VK_BUFFER_USAGE_INDEX_BUFFER_BIT};
    VkBuffer b; VKCHECK(vkCreateBuffer(V.dev, &bci, NULL, &b));
    VkMemoryRequirements req; vkGetBufferMemoryRequirements(V.dev, b, &req);
    vk_alloc a; if (!vk_mem_alloc(&req, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &a)) DIE("quad index buffer");
    VKCHECK(vkBindBufferMemory(V.dev, b, a.mem, a.off));
    uint32_t *ix; VKCHECK(vkMapMemory(V.dev, a.mem, a.off, (VkDeviceSize)n * 24, 0, (void **)&ix));
    for (uint32_t q = 0; q < n; q++) { uint32_t v = q * 4, t[6] = {v, v + 1, v + 2, v, v + 2, v + 3}; memcpy(ix + q * 6, t, 24); }
    vkUnmapMemory(V.dev, a.mem);
    g_quad_ib = b; g_quad_max = n;   /* the old buffer (if any) is left alive: it may be in flight */
}
static void dynamic_state(VkCommandBuffer cb, int prim) {
    /* everything the draw sets dynamically, compared with what this command buffer last set (unchanged: nothing issued) */
    struct {
        VkViewport vp; VkRect2D sc; uint32_t bias_en; float bias[3], blend[4];
        uint32_t cmask[2], wmask[2], ref[2], cull, front, topo, dt, dw, dc, st, sop[2][4], restart;
    } D;
    memset(&D, 0, sizeof D);
    float vx = S.has_vp ? S.vp[0][0] : 0, vy = S.has_vp ? S.vp[0][1] : 0, vw = S.has_vp ? S.vp[0][2] : (float)R.w, vh = S.has_vp ? S.vp[0][3] : (float)R.h;
    D.vp = (VkViewport){vx, vy + vh, vw, -vh, S.dr[0], S.dr[1]};
    if (D.vp.minDepth < 0) D.vp.minDepth = 0; if (D.vp.maxDepth > 1) D.vp.maxDepth = 1;
    int32_t x0 = 0, y0 = 0, x1 = (int32_t)R.w, y1 = (int32_t)R.h;
    if (S.has_sc) { x0 = MAXV(0, S.sc.x); y0 = MAXV(0, S.sc.y); x1 = MINV(x1, S.sc.x + S.sc.w); y1 = MINV(y1, S.sc.y + S.sc.h); }
    D.sc = (VkRect2D){{x0, y0}, {(uint32_t)MAXV(0, x1 - x0), (uint32_t)MAXV(0, y1 - y0)}};
    D.bias_en = (S.poly.offset_en & 4) != 0; D.bias[0] = S.po.units; D.bias[1] = S.po.clamp; D.bias[2] = S.po.factor;
    if (R.dfmt == VK_FORMAT_D16_UNORM && d16tie_on()) {   /* see d16tie_on */
        if (!D.bias_en) { D.bias[0] = D.bias[1] = D.bias[2] = 0; D.bias_en = 1; }
        D.bias[0] += D16_NUDGE_UNITS;
    }
    memcpy(D.blend, S.blend_color, sizeof D.blend);
    for (int f = 0; f < 2; f++) { D.cmask[f] = S.st_vmask[f]; D.wmask[f] = S.st_wmask[f]; D.ref[f] = S.st_ref[f]; }
    static int flip = -1; if (flip < 0) { const char *e = getenv("TOTK_VK_WINDING_FLIP"); flip = e && atoi(e); }
    D.cull = S.poly.cull == NVN_FACE_FRONT ? VK_CULL_MODE_FRONT_BIT : S.poly.cull == NVN_FACE_BACK ? VK_CULL_MODE_BACK_BIT : VK_CULL_MODE_NONE;
    D.front = ((S.poly.front == NVN_FRONT_FACE_CCW) != (flip != 0)) ? VK_FRONT_FACE_COUNTER_CLOCKWISE : VK_FRONT_FACE_CLOCKWISE;
    D.topo = topology(prim);
    int has_d = R.dfmt != VK_FORMAT_UNDEFINED, has_s = R.sfmt != VK_FORMAT_UNDEFINED;
    D.dt = has_d && S.ds.dt; D.dw = has_d && S.ds.dw; D.dc = vk_compare(S.ds.df); D.st = has_s && S.ds.st;
    D.sop[0][0] = vk_stencil_op(S.ds.fail_f); D.sop[0][1] = vk_stencil_op(S.ds.zpass_f); D.sop[0][2] = vk_stencil_op(S.ds.zfail_f); D.sop[0][3] = vk_compare(S.ds.sf_f);
    D.sop[1][0] = vk_stencil_op(S.ds.fail_b); D.sop[1][1] = vk_stencil_op(S.ds.zpass_b); D.sop[1][2] = vk_stencil_op(S.ds.zfail_b); D.sop[1][3] = vk_compare(S.ds.sf_b);
    D.restart = prim == 3 || prim == 5;
    static __typeof__(D) last;
    static int dynoff = -1; if (dynoff < 0) dynoff = vk_off("dyn");
    if (!dynoff && !g_dyn_dirty && !memcmp(&D, &last, sizeof D)) return;
    int all = g_dyn_dirty || dynoff; g_dyn_dirty = 0;
#define CH(f) (all || memcmp(&D.f, &last.f, sizeof D.f))
    if (CH(vp)) vkCmdSetViewport(cb, 0, 1, &D.vp);
    if (CH(sc)) vkCmdSetScissor(cb, 0, 1, &D.sc);
    if (all) vkCmdSetLineWidth(cb, 1.0f);
    if (CH(bias_en)) vkCmdSetDepthBiasEnable(cb, D.bias_en);
    if (CH(bias)) vkCmdSetDepthBias(cb, D.bias[0], D.bias[1], D.bias[2]);
    if (CH(blend)) vkCmdSetBlendConstants(cb, D.blend);
    if (CH(cmask)) { vkCmdSetStencilCompareMask(cb, VK_STENCIL_FACE_FRONT_BIT, D.cmask[0]); vkCmdSetStencilCompareMask(cb, VK_STENCIL_FACE_BACK_BIT, D.cmask[1]); }
    if (CH(wmask)) { vkCmdSetStencilWriteMask(cb, VK_STENCIL_FACE_FRONT_BIT, D.wmask[0]); vkCmdSetStencilWriteMask(cb, VK_STENCIL_FACE_BACK_BIT, D.wmask[1]); }
    if (CH(ref)) { vkCmdSetStencilReference(cb, VK_STENCIL_FACE_FRONT_BIT, D.ref[0]); vkCmdSetStencilReference(cb, VK_STENCIL_FACE_BACK_BIT, D.ref[1]); }
    if (CH(cull)) vkCmdSetCullMode(cb, D.cull);
    if (CH(front)) vkCmdSetFrontFace(cb, (VkFrontFace)D.front);
    if (CH(topo)) vkCmdSetPrimitiveTopology(cb, (VkPrimitiveTopology)D.topo);
    if (CH(dt)) vkCmdSetDepthTestEnable(cb, D.dt);
    if (CH(dw)) vkCmdSetDepthWriteEnable(cb, D.dw);
    if (CH(dc)) vkCmdSetDepthCompareOp(cb, (VkCompareOp)D.dc);
    if (CH(st)) vkCmdSetStencilTestEnable(cb, D.st);
    if (CH(sop)) {
        vkCmdSetStencilOp(cb, VK_STENCIL_FACE_FRONT_BIT, D.sop[0][0], D.sop[0][1], D.sop[0][2], D.sop[0][3]);
        vkCmdSetStencilOp(cb, VK_STENCIL_FACE_BACK_BIT, D.sop[1][0], D.sop[1][1], D.sop[1][2], D.sop[1][3]);
    }
    if (CH(restart)) vkCmdSetPrimitiveRestartEnable(cb, D.restart);
#undef CH
    last = D;
}
/* Vertex, index and storage buffers of a draw that overlap logged uniform updates: those reach memory before the pass
   opens (nvn_vk.c, renaming). Storage buffers are touched again when bound; by then nothing is left to write. */
static void draw_touch(vk_shader *const *sh, const p_draw *dw, const p_draw_idx *di) {
    for (int a = 0; a < 16; a++) if ((sh[0]->vin_mask >> a & 1) && a < S.nattr && S.attr[a].fmt) { const NVNbufferRange *v = &S.vbuf[S.attr[a].stream & 15]; if (v->address) vk_ub_touch(v->address, v->size ? v->size : 16); }
    if (di) vk_ub_touch(di->ibuf, (uint64_t)di->count * (di->itype == 0 ? 1 : di->itype == 1 ? 2 : 4));
    for (int st = 0; st < 2; st++) if (sh[st]) for (int i = 0; i < sh[st]->nb; i++) {   /* every storage binding, as bind_stage resolves it */
        const vk_bind *b = &sh[st]->b[i];
        if (b->dtype != VK_DESCRIPTOR_TYPE_STORAGE_BUFFER || b->reg < 0 || b->reg >= 16) continue;
        uint64_t addr = 0, size = 0;
        if (b->kind == RB_SBUF) { addr = S.ssbo[st][b->reg].addr; size = S.ssbo[st][b->reg].size; }
        else if (b->kind == RB_CBUF || b->kind == RB_UBO_PTR) { addr = S.ubo[st][b->reg].addr; size = S.ubo[st][b->reg].size; }
        else if (b->kind == RB_SBUF_UBO) { const p_bind_buf *u = &S.ubo[st][b->reg]; if (u->addr && u->size >= (uint64_t)b->off + 8) vk_ub_peek(u->addr + b->off, 8, &addr); }
        VkBuffer vb; uint64_t vo, avail;
        if (addr && vk_resolve_span(addr, &vb, &vo, &avail)) vk_ub_touch(addr, MINV(size ? size : avail, avail));
    }
    (void)dw;
}
#ifdef TOTK_ANDROID_APP
static void hud_mark(vk_shader **sh);   /* the app's window section below: the scene before the HUD, for frame generation */
#else
static void hud_mark(vk_shader **sh) { (void)sh; }
#endif
/* TOTK_VK_DRAWCAP=<vertex shader key prefix>[:<frames to skip>] (diagnostic, 2026-10-25, the Depths terrain): the inputs
   of the first 4 indexed draws of that vertex shader after the skipped frames, into drawcap/ in the game folder, to run
   the shader on the PC: dN.txt (draw parameters, uniform slots, storage pointers), dN_idx.bin, dN_u<reg>.bin (vertex
   stage uniforms as the GPU sees them), sb_<address>.bin (each storage buffer once, up to 256 MiB). */
#include <unistd.h>
static uint64_t g_present_n;   /* defined with the present bookkeeping below */
static void drawcap(vk_shader *vs, const p_draw_idx *di) {
    static const char *want = (const char *)1; static long skip; static int n;
    if (want == (const char *)1) { want = getenv("TOTK_VK_DRAWCAP"); const char *c = want ? strchr(want, ':') : NULL; skip = c ? atol(c + 1) : 3000; }
    static uint64_t sbva[4], sbframe; static int nsb, later;
    if (want && n >= 4 && !later && g_present_n >= sbframe + 3) {   /* the start of each storage buffer again, 3 frames later */
        later = 1;
        for (int i = 0; i < nsb; i++) { char sp[96]; snprintf(sp, sizeof sp, "drawcap/sb_%llx_later.bin", (unsigned long long)sbva[i]);
            FILE *g = fopen(sp, "wb"); if (g) { fwrite((const void *)(uintptr_t)sbva[i], 1, 2u << 20, g); fclose(g); } }
        LOGV("drawcap: %d storage buffer(s) dumped again at frame %llu", nsb, (unsigned long long)g_present_n + 1);
    }
    if (!want || !di || n >= 4 || (long)g_present_n < skip) return;
    size_t wl = strcspn(want, ":"); if (strncmp(vs->key, want, wl)) return;
    mkdir("drawcap", 0755);
    int k = n++; char path[96]; FILE *f;
    snprintf(path, sizeof path, "drawcap/d%d.txt", k);
    if (!(f = fopen(path, "w"))) return;
    uint32_t isz = di->itype == 0 ? 1 : di->itype == 1 ? 2 : 4;
    fprintf(f, "frame %llu\nvs %s\nprim %d\ncount %u\nbase_vertex %d\ninstances %u\nbase_instance %u\nitype %d\nibuf %#llx\n", (unsigned long long)g_present_n + 1, vs->key, di->prim,
            (unsigned)di->count, (int)di->base_vertex, (unsigned)di->instances, (unsigned)di->base_instance, (int)di->itype, (unsigned long long)di->ibuf);
    { char ip[96]; snprintf(ip, sizeof ip, "drawcap/d%d_idx.bin", k); FILE *g = fopen(ip, "wb"); if (g) { fwrite((const void *)(uintptr_t)di->ibuf, isz, di->count, g); fclose(g); } }
    for (int r = 0; r < 16; r++) {
        const p_bind_buf *u = &S.ubo[0][r]; if (!u->addr || !u->size) continue;
        uint64_t sz = MINV(u->size, (uint64_t)0x10000); uint8_t *buf = malloc(sz); if (!buf) continue;
        vk_ub_peek(u->addr, sz, buf);
        fprintf(f, "ubo %d %#llx %llu\n", r, (unsigned long long)u->addr, (unsigned long long)u->size);
        char up[96]; snprintf(up, sizeof up, "drawcap/d%d_u%d.bin", k, r); FILE *g = fopen(up, "wb"); if (g) { fwrite(buf, 1, sz, g); fclose(g); }
        if (memcmp(buf, (const void *)(uintptr_t)u->addr, sz)) {   /* guest memory differs from what the GPU is shown (logged updates) */
            fprintf(f, "ubo %d memory differs from the GPU view\n", r);
            snprintf(up, sizeof up, "drawcap/d%d_m%d.bin", k, r); if ((g = fopen(up, "wb"))) { fwrite((const void *)(uintptr_t)u->addr, 1, sz, g); fclose(g); }
        }
        if (u->size >= 8) {   /* a storage pointer at offset 0 (the terrain's vp_s0 comes from slot 3 + 0) */
            uint64_t va; memcpy(&va, buf, 8); VkBuffer bb; uint64_t oo, av;
            if (va && vk_resolve_span(va, &bb, &oo, &av)) {
                fprintf(f, "ptr %d+0 %#llx avail %llu\n", r, (unsigned long long)va, (unsigned long long)av);
                char sp[96]; snprintf(sp, sizeof sp, "drawcap/sb_%llx.bin", (unsigned long long)va);
                if (access(sp, F_OK) && nsb < 4) { sbva[nsb++] = va; sbframe = g_present_n; }
                if (access(sp, F_OK)) { FILE *g = fopen(sp, "wb"); if (g) { fwrite((const void *)(uintptr_t)va, 1, MINV(av, (uint64_t)256 << 20), g); fclose(g); } }
            }
        }
        free(buf);
    }
    fclose(f);
    LOGV("drawcap: draw %d of vs %.16s (frame %llu) written to drawcap/", k, vs->key, (unsigned long long)g_present_n + 1);
}
/* barriersmart: does this draw read one of the open pass's attachments (sampled, as a storage image or texel view)? */
static int samples_attachment(vk_shader *const *sh) {
    VkImage at[9]; int na = 0;
    for (uint32_t i = 0; i < R.ncolor && i < 8; i++) if (R.ctex[i]) at[na++] = vk_tex_image(R.ctex[i]);
    if (R.dtex) at[na++] = vk_tex_image(R.dtex);
    if (!na) return 0;
    for (int st = 0; st < 2; st++) if (sh[st]) for (int i = 0; i < sh[st]->nb; i++) {
        const vk_bind *b = &sh[st]->b[i]; int slot = b->reg; uint64_t h = 0; int want;
        if (b->dtype == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE) { if (slot >= 0 && slot < 32) h = S.img[st][slot]; want = -2; }
        else if (b->dtype == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER || b->dtype == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE) { if (slot >= 0 && slot < 128) h = S.tex[st][slot]; want = b->target; }
        else if (b->dtype == VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER || b->dtype == VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER) return 1;   /* not resolved to an image here: keep the order */
        else continue;
        if (!h) continue;
        vk_tex *x = NULL; vk_tex_by_id((int)(h & 0xfffff), &want, &x);
        if (!x) continue;
        VkImage im = vk_tex_image(x);
        for (int k = 0; k < na; k++) if (im == at[k]) return 1;
    }
    return 0;
}
static void draw(const p_draw *dw, const p_draw_idx *di) {
    int prim = dw ? dw->prim : di->prim;
    if (!S.prog) { g_stat_skips[0]++; return; }
    if (S.render_disabled || S.raster_discard) { g_stat_skips[1]++; pd_draw_note("      draw skipped: %s", S.render_disabled ? "conditional rendering off" : "rasterizer discard"); return; }
    if (S.poly.cull == NVN_FACE_FRONT_AND_BACK) { g_stat_skips[2]++; pd_draw_note("      draw skipped: cull front and back"); return; }
    if (prim == 2 || prim >= 8) { RED(4, "primitive type %d not drawn", prim); g_stat_skips[3]++; return; }
    if (di && prim == 7) { RED(4, "indexed quads not drawn"); g_stat_skips[3]++; return; }
    uint32_t count = dw ? (uint32_t)dw->count : (uint32_t)di->count; if (!count) return;
    vk_shader *sh[6];
    if (vk_prog_resolve(S.prog, sh) || !sh[0]) { g_stat_skips[4]++; return; }
    if (R.open && R.barrier_pending) {
        R.barrier_pending = 0;
        if (samples_attachment(sh)) { vk_end_rendering(); g_stat_inpass_barriers++; }
        else if (++g_stat_barriers_dropped <= 4 || g_stat_barriers_dropped % 100000 == 0)
            LOGV("%llu barriers inside passes dropped: the next draw neither samples the pass's targets nor follows a storage write (TOTK_VK_OFF=barriersmart: split at each)", (unsigned long long)g_stat_barriers_dropped);
    }
    int was_open = R.open;
    if (was_open && pass_split_due()) {   /* the pass goes on after the split: no new-pass work (HUD mark, pass note) */
        vk_end_rendering(); split_maybe(); g_stat_pass_splits++;
        if (!begin_rendering()) { g_stat_skips[5]++; return; }
    }
    if (!was_open) { split_maybe(); hud_mark(sh); }
    draw_touch(sh, dw, di);
    if (!begin_rendering()) { g_stat_skips[5]++; return; }
    if (!was_open) vk_sub_note('p', sh[0]->key, sh[1] ? sh[1]->key : NULL, R.w, R.h, (uint32_t)R.ncolor);
    if (S.has_sc) { int32_t x1 = MINV((int32_t)R.w, S.sc.x + S.sc.w), y1 = MINV((int32_t)R.h, S.sc.y + S.sc.h); if (x1 <= MAXV(0, S.sc.x) || y1 <= MAXV(0, S.sc.y)) { g_stat_skips[6]++; pd_draw_note("      draw skipped: empty scissor"); return; } }
    VkPipelineLayout layout = vk_layout_of(sh[0], sh[1]);
    pipe_key k; pipe_key_fill(&k, sh[0], sh[1], prim);
    VkPipeline p = pipe_get(&k, sh[0], sh[1], layout);
    if (!p || p == (VkPipeline)(uintptr_t)1) { g_stat_skips[7]++; pd_draw_note("      draw skipped: no pipeline (vs %.16s fs %.16s)", sh[0]->key, sh[1] ? sh[1]->key : "-"); return; }
    VkCommandBuffer cb = vk_cb();
    if (pd_active()) {
        float c25[4] = {0}; const p_bind_buf *u8 = &S.ubo[0][8]; if (u8->addr && u8->size >= 26 * 16) vk_ub_peek(u8->addr + 25 * 16, 16, c25);
        pd_draw_note("      draw: vs %.16s fs %.16s prim %d count %u inst %u first %u%s; depth test %d func %d write %d; stencil %d func %d ref %#x mask %#x; colour mask %#x blend %#x; scissor %s %d,%d %dx%d; vs c11[25] %g %g %g %g",
            sh[0]->key, sh[1] ? sh[1]->key : "-", prim, count, dw ? (uint32_t)dw->instances : (uint32_t)di->instances, dw ? (uint32_t)dw->first : (uint32_t)di->base_vertex, di ? " indexed" : "",
            S.ds.dt, S.ds.df, S.ds.dw, S.ds.st, S.ds.sf_f, S.st_ref[0], S.st_vmask[0], S.chmask.mask, S.color.blend_en, S.has_sc ? "on" : "off", S.sc.x, S.sc.y, S.sc.w, S.sc.h,
            (double)c25[0], (double)c25[1], (double)c25[2], (double)c25[3]);
    }
    if (p != g_bound_pipe) { vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p); g_bound_pipe = p; }
    dynamic_state(cb, prim);
    bind_stage(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, sh[0], 0, NULL);
    if (sh[1]) bind_stage(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 1, sh[1], 1, NULL);
    if (!R.open) { RED(4, "the render pass ended while a draw was being set up: draw skipped"); g_stat_skips[5]++; return; }
    drawcap(sh[0], di);
    if (probe_on() && g_npdraw < 4) {   /* pass probe: what each of the pass's first draws was */
        probe_draw *q = &g_pdraw[g_npdraw++]; memset(q, 0, sizeof *q);
        snprintf(q->vs, sizeof q->vs, "%.16s", sh[0]->key); q->prim = prim; q->count = dw ? (uint32_t)dw->count : (uint32_t)di->count; q->first = dw ? (uint32_t)dw->first : (uint32_t)di->base_vertex;
        q->inst = dw ? (uint32_t)dw->instances : (uint32_t)di->instances;
        if (S.has_vp) memcpy(q->vp, S.vp[0], 16); memcpy(q->dr, S.dr, 8); q->bias[0] = S.po.factor; q->bias[1] = S.po.units; q->bias[2] = (S.poly.offset_en & 4) ? 1.f : 0.f;
        q->clamp = S.depth_clamp; q->cull = (int)S.poly.cull; q->has_sc = S.has_sc; if (S.has_sc) { q->sc[0] = S.sc.x; q->sc[1] = S.sc.y; q->sc[2] = S.sc.w; q->sc[3] = S.sc.h; }
        /* the words the class-shading vertex shaders (345bb4abe6b6bbf4 / 97c2cb79ae8c75a5) turn into depth: vp_c11[25].x
           (the class, uniform slot 8) and vp_c4[147].z (slot 1); depth = k + (1 - k) * nibble_swap(class) / 255 */
        { const p_bind_buf *u8 = &S.ubo[0][8], *u1 = &S.ubo[0][1];
          if (u8->addr && u8->size >= 26 * 16) vk_ub_peek(u8->addr + 25 * 16, 16, q->c11_25);
          if (u1->addr && u1->size >= 148 * 16) vk_ub_peek(u1->addr + 147 * 16, 16, q->c4_147);
          if (u1->addr && u1->size >= 8 * 16) vk_ub_peek(u1->addr + 7 * 16, 16, q->c4_7); }
        int a0 = -1; for (int a = 0; a < 16; a++) if ((sh[0]->vin_mask >> a & 1) && a < S.nattr && S.attr[a].fmt) { a0 = a; break; }
        if (a0 >= 0) { uint32_t st = S.attr[a0].stream & 15; uint64_t va = S.vbuf[st].address; VkBuffer bb; uint64_t oo, av;
            if (va && vk_resolve_span(va, &bb, &oo, &av) && av >= 32) memcpy(q->v0, (const void *)(uintptr_t)(va + (uint64_t)q->first * S.stream[st].stride), 32); }
    }
    /* vertex streams the shader reads */
    VkBuffer vb[17]; VkDeviceSize vo[17], vsz[17], vst[17]; uint32_t used = 0;
    for (int a = 0; a < 16; a++) if ((sh[0]->vin_mask >> a & 1) && a < S.nattr && S.attr[a].fmt) used |= 1u << (S.attr[a].stream & 15);
    for (uint32_t s = 0; s < 16; s++) if (used >> s & 1) {
        VkBuffer b; uint64_t o, avail; uint64_t sz = S.vbuf[s].size ? S.vbuf[s].size : 16;
        if (!S.vbuf[s].address || !vk_resolve_span(S.vbuf[s].address, &b, &o, &avail)) { VkDeviceSize to; void *z = vk_transient(64, 16, &b, &to); memset(z, 0, 64); o = to; sz = 64; RED(4, "vertex stream %u is not in a memory pool", s); }
        else sz = MINV(sz, avail);
        VkDeviceSize stride = S.stream[s].stride;
        vb[0] = b; vo[0] = o; vsz[0] = sz; vst[0] = stride;
        vkCmdBindVertexBuffers2(cb, s, 1, vb, vo, vsz, vst);
    }
    if (sh[0]->vin_mask) {   /* the zero stream (only read where the pipeline points an attribute at it) */
        VkBuffer zb; VkDeviceSize zo; void *z = vk_transient(64, 16, &zb, &zo); memset(z, 0, 64);
        VkDeviceSize zs = 64, zst = 0;
        vkCmdBindVertexBuffers2(cb, ZERO_BINDING, 1, &zb, &zo, &zs, &zst);
    }
    uint32_t inst = dw ? (dw->instances > 1 ? (uint32_t)dw->instances : 1) : (di->instances > 1 ? (uint32_t)di->instances : 1);
    uint32_t base_inst = dw ? (uint32_t)dw->base_instance : (uint32_t)di->base_instance;
    if (dw && prim == 7) {
        if (count >= 4) { quad_indices(count / 4); vkCmdBindIndexBuffer(cb, g_quad_ib, 0, VK_INDEX_TYPE_UINT32); vkCmdDrawIndexed(cb, (count / 4) * 6, inst, 0, dw->first, base_inst); }
    } else if (dw) vkCmdDraw(cb, count, inst, (uint32_t)dw->first, base_inst);
    else {
        VkBuffer ib; uint64_t io;
        uint32_t isz = di->itype == 0 ? 1 : di->itype == 1 ? 2 : 4;
        if (!vk_resolve(di->ibuf, (uint64_t)count * isz, &ib, &io)) { RED(4, "index buffer is not in a memory pool"); g_stat_skips[8]++; return; }
        VkIndexType it = di->itype == 1 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32;
        if (di->itype == 0) {
            if (V.has_index_u8) it = VK_INDEX_TYPE_UINT8_EXT;
            else {   /* widen to 16 bits (restart 0xff -> 0xffff); the bytes are those the game wrote by this submission */
                VkDeviceSize to; uint16_t *w = vk_transient((size_t)count * 2 + 2, 16, &ib, &to); const uint8_t *src = (const uint8_t *)(uintptr_t)di->ibuf;
                for (uint32_t i = 0; i < count; i++) w[i] = src[i] == 0xff ? 0xffff : src[i];
                io = to; it = VK_INDEX_TYPE_UINT16;
            }
        }
        if (io % isz) { RED(4, "index buffer offset %#llx is not aligned", (unsigned long long)io); g_stat_skips[8]++; return; }
        vkCmdBindIndexBuffer(cb, ib, io, it);
        vkCmdDrawIndexed(cb, count, inst, 0, di->base_vertex, base_inst);
    }
    g_stat_draws++; vk_note_draw(); g_samples_cum += count * inst;
    if (sh[1] && pd_split_match(sh[1]->key)) vk_end_rendering();
}
static void dispatch(const p_dispatch *dp) {
    vk_ub_flush();   /* work outside passes sees every logged uniform update */
    if (!S.prog) return;
    vk_shader *sh[6];
    if (vk_prog_resolve(S.prog, sh) || !sh[5]) { g_stat_skips[9]++; return; }
    VkPipelineLayout layout; VkPipeline p = vk_compute_pipeline(sh[5], &layout);
    if (!p) return;
    vk_end_rendering();
    VkCommandBuffer cb = vk_cb();
    vk_barrier_all(cb);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, p);
    uint32_t grid[3] = {(uint32_t)dp->x, (uint32_t)dp->y, (uint32_t)dp->z};
    bind_stage(cb, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, sh[5], 5, grid);
    vkCmdDispatch(cb, grid[0], grid[1], grid[2]); vk_sub_note('d', sh[5]->key, NULL, grid[0], grid[1], grid[2]);
    vk_barrier_all(cb);
    vk_sub_time();
    g_bound_pipe = VK_NULL_HANDLE; g_stat_dispatch++;
}

/* ---------- copies and clears of textures ---------- */
static unsigned vlevel(const view_t *v, int has) { return has && v->set_levels ? v->base_level : 0; }
static unsigned vlayer(const view_t *v, int has) { return has && v->set_layers ? v->min_layer : 0; }
static VkImageAspectFlags copy_aspect(vk_tex *x) { VkImageAspectFlags a = vk_tex_aspect(x); return a == (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT) ? VK_IMAGE_ASPECT_DEPTH_BIT : a; }
static void copy_b2t(const p_copy_b2t *c) {   /* buffer -> texture (row stride from SetCopyRowStride, 0 = packed) */
    vk_ub_flush();   /* work outside passes sees every logged uniform update */
    vk_tex *x = vk_tex_get(c->tex); if (!x) return;
    int bw, bh, bpb = nvn_fmt_bpb(c->tex->d.format, &bw, &bh); if (!bpb) return;
    unsigned level = vlevel(&c->view, c->has_view), layer0 = vlayer(&c->view, c->has_view);
    size_t row = S.copy_stride ? (size_t)S.copy_stride : (size_t)((c->r.width + bw - 1) / bw) * bpb, rows = (c->r.height + bh - 1) / bh, img = row * rows;
    int nz = c->r.depth ? c->r.depth : 1, is3d = vk_tex_is3d(x);
    VkBuffer b; uint64_t o;
    if (!vk_resolve(c->src, img * nz, &b, &o)) { RED(4, "buffer-to-texture copy: the source is not in a memory pool"); return; }
    if (row % bpb || o % 4) { RED(4, "buffer-to-texture copy with stride %zu / offset %#llx not expressible", row, (unsigned long long)o); return; }
    vk_end_rendering();
    VkCommandBuffer cb = vk_cb(); vk_barrier_all(cb);
    VkBufferImageCopy r = {.bufferOffset = o, .bufferRowLength = (uint32_t)(row / bpb * bw), .bufferImageHeight = (uint32_t)(rows * bh),
        .imageSubresource = {copy_aspect(x), level, is3d ? 0 : layer0 + c->r.zoffset, is3d ? 1 : (uint32_t)nz},
        .imageOffset = {c->r.xoffset, c->r.yoffset, is3d ? c->r.zoffset : 0}, .imageExtent = {(uint32_t)c->r.width, (uint32_t)c->r.height, is3d ? (uint32_t)nz : 1}};
    vkCmdCopyBufferToImage(cb, b, vk_tex_image(x), VK_IMAGE_LAYOUT_GENERAL, 1, &r);
    vk_barrier_all(cb);
    vk_tex_mark_gpu_written(x); probe_write_note('B', x, NULL, 0);
}
static void copy_t2b(const p_copy_b2t *c) {   /* texture -> buffer */
    vk_ub_flush();   /* work outside passes sees every logged uniform update */
    vk_tex *x = vk_tex_get(c->tex); if (!x) return;
    int bw, bh, bpb = nvn_fmt_bpb(c->tex->d.format, &bw, &bh); if (!bpb) return;
    unsigned level = vlevel(&c->view, c->has_view), layer0 = vlayer(&c->view, c->has_view);
    size_t row = S.copy_stride ? (size_t)S.copy_stride : (size_t)((c->r.width + bw - 1) / bw) * bpb, rows = (c->r.height + bh - 1) / bh, img = row * rows;
    int nz = c->r.depth ? c->r.depth : 1, is3d = vk_tex_is3d(x);
    VkBuffer b; uint64_t o;
    if (!vk_resolve(c->src, img * nz, &b, &o)) { RED(4, "texture-to-buffer copy: the destination is not in a memory pool"); return; }
    vk_gpu_write(c->src, img * nz);
    if (row % bpb || o % 4) { RED(4, "texture-to-buffer copy with stride %zu / offset %#llx not expressible", row, (unsigned long long)o); return; }
    vk_end_rendering();
    VkCommandBuffer cb = vk_cb(); vk_barrier_all(cb);
    VkBufferImageCopy r = {.bufferOffset = o, .bufferRowLength = (uint32_t)(row / bpb * bw), .bufferImageHeight = (uint32_t)(rows * bh),
        .imageSubresource = {copy_aspect(x), level, is3d ? 0 : layer0 + c->r.zoffset, is3d ? 1 : (uint32_t)nz},
        .imageOffset = {c->r.xoffset, c->r.yoffset, is3d ? c->r.zoffset : 0}, .imageExtent = {(uint32_t)c->r.width, (uint32_t)c->r.height, is3d ? (uint32_t)nz : 1}};
    vkCmdCopyImageToBuffer(cb, vk_tex_image(x), VK_IMAGE_LAYOUT_GENERAL, b, 1, &r);
    vk_barrier_all(cb);
}
static void copy_t2t(const p_copy_t2t *c) {   /* texture -> texture: same size as a copy, scaled as a blit */
    vk_ub_flush();   /* work outside passes sees every logged uniform update */
    vk_tex *s = vk_tex_get(c->src), *d = vk_tex_get(c->dst); if (!s || !d) return;
    unsigned sl = vlevel(&c->sv, c->has_sv), dl = vlevel(&c->dv, c->has_dv), sz0 = vlayer(&c->sv, c->has_sv), dz0 = vlayer(&c->dv, c->has_dv);
    int s3 = vk_tex_is3d(s), d3 = vk_tex_is3d(d), nz = c->sr.depth ? c->sr.depth : 1;
    vk_end_rendering();
    VkCommandBuffer cb = vk_cb(); vk_barrier_all(cb);
    int same = c->sr.width == c->dr.width && c->sr.height == c->dr.height && (c->sr.depth ? c->sr.depth : 1) == (c->dr.depth ? c->dr.depth : 1) && s3 == d3;
    int bwa, bha, bwb, bhb, bpa = nvn_fmt_bpb(c->src->d.format, &bwa, &bha), bpbb = nvn_fmt_bpb(c->dst->d.format, &bwb, &bhb);
    if (same && bpa == bpbb && bwa == bwb && bha == bhb) {
        VkImageCopy r = {.srcSubresource = {copy_aspect(s), sl, s3 ? 0 : sz0 + c->sr.zoffset, s3 ? 1 : (uint32_t)nz}, .srcOffset = {c->sr.xoffset, c->sr.yoffset, s3 ? c->sr.zoffset : 0},
            .dstSubresource = {copy_aspect(d), dl, d3 ? 0 : dz0 + c->dr.zoffset, d3 ? 1 : (uint32_t)nz}, .dstOffset = {c->dr.xoffset, c->dr.yoffset, d3 ? c->dr.zoffset : 0},
            .extent = {(uint32_t)c->sr.width, (uint32_t)c->sr.height, s3 ? (uint32_t)nz : 1}};
        vkCmdCopyImage(cb, vk_tex_image(s), VK_IMAGE_LAYOUT_GENERAL, vk_tex_image(d), VK_IMAGE_LAYOUT_GENERAL, 1, &r);
    } else if (!s3 && !d3 && copy_aspect(s) == copy_aspect(d)) {
        VkImageBlit r = {.srcSubresource = {copy_aspect(s), sl, sz0 + c->sr.zoffset, 1}, .srcOffsets = {{c->sr.xoffset, c->sr.yoffset, 0}, {c->sr.xoffset + c->sr.width, c->sr.yoffset + c->sr.height, 1}},
            .dstSubresource = {copy_aspect(d), dl, dz0 + c->dr.zoffset, 1}, .dstOffsets = {{c->dr.xoffset, c->dr.yoffset, 0}, {c->dr.xoffset + c->dr.width, c->dr.yoffset + c->dr.height, 1}}};
        int linear = (c->flags & NVN_COPY_FLAGS_LINEAR_FILTER_BIT) && copy_aspect(s) == VK_IMAGE_ASPECT_COLOR_BIT;
        vkCmdBlitImage(cb, vk_tex_image(s), VK_IMAGE_LAYOUT_GENERAL, vk_tex_image(d), VK_IMAGE_LAYOUT_GENERAL, 1, &r, linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST);
    } else RED(4, "texture copy %dx%dx%d -> %dx%dx%d (3D %d -> %d) not done", c->sr.width, c->sr.height, c->sr.depth, c->dr.width, c->dr.height, c->dr.depth, s3, d3);
    vk_barrier_all(cb);
    vk_tex_mark_gpu_written(d); probe_write_note('C', d, s, 0);
}
static void clear_tex(const p_clear_tex *c) {   /* clear a texture region with a value, outside any pass */
    vk_ub_flush();   /* work outside passes sees every logged uniform update */
    vk_tex *x = vk_tex_get(c->tex); if (!x) return;
    unsigned lv = vlevel(&c->view, c->has_view), l0 = vlayer(&c->view, c->has_view);
    int nz = c->r.depth ? c->r.depth : 1;
    if ((c->mask & 0xf) != 0xf) RED(4, "texture clear with channel mask %#x: cleared whole", c->mask);
    if (c->r.xoffset || c->r.yoffset || (uint32_t)c->r.width != vk_tex_width(x, lv) || (uint32_t)c->r.height != vk_tex_height(x, lv)) RED(4, "partial texture clear: cleared whole level");
    vk_end_rendering();
    VkCommandBuffer cb = vk_cb(); vk_barrier_all(cb);
    VkImageAspectFlags as = vk_tex_aspect(x);
    VkImageSubresourceRange rr = {as, lv, 1, vk_tex_is3d(x) ? 0 : l0 + c->r.zoffset, vk_tex_is3d(x) ? 1 : (uint32_t)nz};
    if (as & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)) {
        VkClearDepthStencilValue v = {vk_tex_format(x) == VK_FORMAT_D16_UNORM ? d16_clear(c->c.f[0]) : c->c.f[0], (as & VK_IMAGE_ASPECT_DEPTH_BIT) ? c->c.u[1] : c->c.u[0]};
        vkCmdClearDepthStencilImage(cb, vk_tex_image(x), VK_IMAGE_LAYOUT_GENERAL, &v, 1, &rr);
    } else {
        VkClearColorValue v; memcpy(&v, c->c.u, 16);
        if (!c->is_uint) { int f = c->has_view && c->view.set_format ? c->view.format : c->tex->d.format; int cls = vk_fmt_class(f); if (cls) { for (int i = 0; i < 4; i++) v.uint32[i] = (uint32_t)c->c.f[i]; } }
        vkCmdClearColorImage(cb, vk_tex_image(x), VK_IMAGE_LAYOUT_GENERAL, &v, 1, &rr);
    }
    vk_barrier_all(cb);
    vk_tex_mark_gpu_written(x); probe_write_note('K', x, NULL, 0);
}

/* ---------- the walker's other ops ---------- */
/* ---------- counters and conditional rendering in GPU order (TOTK_VK_OFF=condorder: the old record-time memory read) ----------
   A counter report's memory is written when its submission completes, but a conditional (SetRenderEnableConditional)
   later in the same or the next submissions was evaluated at record time from memory - that is, from whatever an older,
   already completed report left there. Which one that was depended on timing, so the game's per-frame conditional
   clears of the shadow / AO mask (the camera-cut black frames, DEBUGGING.md) ran on some frames and not on others.
   Now every report's value is fixed when it is recorded and kept here (the last one per address); a conditional reads
   these first, memory only for addresses no report has written. Occlusion is still "everything visible", but as NVN
   does it: SAMPLES_PASSED is a cumulative counter - ResetCounter zeroes it, every recorded draw adds to it, and a report
   holds the value at that point (two reports with no draw between them are equal, as with real queries). */
enum { CNT_N = 4096 };
static struct { uint64_t addr, val; } g_cnt[CNT_N];
static int condorder_on(void) { static int on = -1; if (on < 0) on = !vk_off("condorder"); return on; }
static int cnt_find(uint64_t addr, uint64_t **val) {
    unsigned h = (unsigned)((addr >> 4) * 2654435761u) & (CNT_N - 1);
    for (unsigned i = 0; i < CNT_N; i++, h = (h + 1) & (CNT_N - 1)) {
        if (g_cnt[h].addr == addr) { *val = &g_cnt[h].val; return 1; }
        if (!g_cnt[h].addr) { *val = &g_cnt[h].val; g_cnt[h].addr = addr; g_cnt[h].val = 0; return 0; }
    }
    static uint64_t spill; *val = &spill; return 0;   /* full: never expected (counter addresses are few) */
}
static void counter_report(uint64_t addr, uint32_t type) { uint64_t *v; cnt_find(addr, &v); *v = type == NVN_COUNTER_TYPE_SAMPLES_PASSED ? g_samples_cum : 0; }
static int counter_known(uint64_t addr, uint64_t *out) {
    unsigned h = (unsigned)((addr >> 4) * 2654435761u) & (CNT_N - 1);
    for (unsigned i = 0; i < CNT_N; i++, h = (h + 1) & (CNT_N - 1)) { if (g_cnt[h].addr == addr) { *out = g_cnt[h].val; return 1; } if (!g_cnt[h].addr) return 0; }
    return 0;
}
uint64_t vk_counter_samples(uint64_t addr) {   /* nvn_vk.c, when the report is recorded: the value it will write */
    uint64_t v; if (condorder_on() && counter_known(addr, &v)) return v;
    return 1u << 20;   /* old behaviour: everything visible */
}
void vk_draw_exec(uint32_t op, const void *a) {
    state_init();
    switch (op) {
        case OP_SET_RT: {
            const p_set_rt *r = a; g_pops.setrt++;
            int pend = R.clear_d || R.clear_s; for (int i = 0; i < 8; i++) pend |= R.clear_c[i];
            if (R.open && !pend && S.has_rt && rt_same(&S.rt, r)) { S.rt = *r; break; }   /* the same targets again: keep the pass */
            vk_end_rendering();
            memset(R.clear_c, 0, sizeof R.clear_c); R.clear_d = R.clear_s = 0;
            S.rt = *r; S.has_rt = 1;
            break;
        }
        case OP_CLEAR_COLOR: clear_color(a); break;
        case OP_CLEAR_DS: clear_ds(a); break;
        case OP_DRAW: draw(a, NULL); if (serial_draws()) vk_end_rendering(); break;
        case OP_DRAW_IDX: draw(NULL, a); if (serial_draws()) vk_end_rendering(); break;
        case OP_DISPATCH: dispatch(a); break;
        case OP_COPY_B2T: copy_b2t(a); break;
        case OP_COPY_T2B: copy_t2b(a); break;
        case OP_COPY_T2T: copy_t2t(a); break;
        case OP_CLEAR_TEX: clear_tex(a); break;
        case OP_BARRIER: {
            int bits; memcpy(&bits, a, 4);
            if (!R.open) vk_barrier_all(vk_cb());
            else if ((bits & ~NVN_BARRIER_ORDER_PRIMITIVES_BIT) && pass_barrier_on()) {
                if (barriersmart_on() && !R.wrote_storage) R.barrier_pending = 1;   /* the next draw decides */
                else { vk_end_rendering(); g_stat_inpass_barriers++; }   /* the next draw reopens the pass (load) */
            }
            break; }
        case OP_SCISSOR: S.sc = *(const p_scissor *)a; S.has_sc = 1; break;
        case OP_BIND_PROGRAM: S.prog = ((const p_bind_program *)a)->prog; break;
        case OP_BIND_STATE: {
            const p_bind_state *b = a; blend_st bs;
            switch (b->kind) {
                case 0: memcpy(&bs, b->bytes, sizeof bs); S.blend[bs.target & 7] = bs; break;
                case 1: memcpy(&S.color, b->bytes, sizeof S.color); break;
                case 2: memcpy(&S.chmask, b->bytes, sizeof S.chmask); break;
                case 3: memcpy(&S.ms, b->bytes, sizeof S.ms); break;
                case 4: memcpy(&S.poly, b->bytes, sizeof S.poly); break;
                case 5: memcpy(&S.ds, b->bytes, sizeof S.ds); break;
            }
            break;
        }
        case OP_BIND_VATTR: { const p_bind_vattr *b = a; if (b->n >= 0 && b->n <= 16) { memcpy(S.attr, b->st, 4 * (size_t)b->n); S.nattr = b->n; } break; }
        case OP_BIND_VSTREAM: { const p_bind_vstream *b = a; if (b->n >= 0 && b->n <= 16) { memcpy(S.stream, b->st, 8 * (size_t)b->n); S.nstream = b->n; } break; }
        case OP_BIND_VBUF: { const p_bind_vbuf *b = a; if (b->first >= 0 && b->first + b->n <= 16) memcpy(&S.vbuf[b->first], b->r, sizeof *b->r * (size_t)b->n); break; }
        case OP_BIND_UBO: { const p_bind_buf *b = a; if (b->stage >= 0 && b->stage < 6 && b->index >= 0 && b->index < 16) S.ubo[b->stage][b->index] = *b; break; }
        case OP_BIND_SSBO: { const p_bind_buf *b = a; if (b->stage >= 0 && b->stage < 6 && b->index >= 0 && b->index < 16) S.ssbo[b->stage][b->index] = *b; break; }
        case OP_BIND_TEX: { const p_bind_handles *b = a; if (b->stage >= 0 && b->stage < 6 && b->first >= 0 && b->first + b->n <= 128) { memcpy(&S.tex[b->stage][b->first], b->h, 8 * (size_t)b->n); if (b->first + b->n > S.tex_hi[b->stage]) S.tex_hi[b->stage] = b->first + b->n; } break; }
        case OP_BIND_SAMP: { const p_bind_handles *b = a; if (b->stage >= 0 && b->stage < 6 && b->first >= 0 && b->first + b->n <= 128) memcpy(&S.samp[b->stage][b->first], b->h, 8 * (size_t)b->n); break; }
        case OP_BIND_IMG: { const p_bind_handles *b = a; if (b->stage >= 0 && b->stage < 6 && b->first >= 0 && b->first + b->n <= 32) memcpy(&S.img[b->stage][b->first], b->h, 8 * (size_t)b->n); break; }
        case OP_STENCIL: { const p_stencil *b = a; for (int f = 0; f < 2; f++) if (b->face & (1 << f)) { if (b->which == 0) S.st_vmask[f] = (uint32_t)b->value_mask; else if (b->which == 1) S.st_wmask[f] = (uint32_t)b->mask; else S.st_ref[f] = (uint32_t)b->ref; } break; }
        case OP_POLY_OFFSET: S.po = *(const p_poly_offset *)a; break;
        case OP_VIEWPORTS: { const p_viewports *b = a; for (int i = 0; i < b->n && b->first + i < 16; i++) memcpy(S.vp[b->first + i], b->v[i], 16); S.has_vp = 1; break; }
        case OP_DEPTH_RANGE: { const p_depth_range *b = a; S.dr[0] = b->n; S.dr[1] = b->f; break; }
        case OP_BLEND_COLOR: memcpy(S.blend_color, ((const p_blend_color *)a)->rgba, 16); break;
        case OP_COPY_STRIDE: S.copy_stride = ((const p_copy_stride *)a)->stride; break;
        case OP_DEPTH_CLAMP: S.depth_clamp = ((const p_flag *)a)->value; break;
        case OP_RENDER_ENABLE: S.render_disabled = !((const p_flag *)a)->value; break;
        case OP_RENDER_COND: {   /* render if the two counters at addr / addr + 16 are equal (mode 0) or not (1), as reported (all visible for now) */
            const p_render_cond *c = a; uint64_t x = 0, y = 0;
            if (c->addr) {
                if (!condorder_on() || !counter_known(c->addr, &x)) memcpy(&x, (const void *)(uintptr_t)c->addr, 8);
                if (!condorder_on() || !counter_known(c->addr + 16, &y)) memcpy(&y, (const void *)(uintptr_t)(c->addr + 16), 8);
            }
            S.render_disabled = c->mode == 0 ? x != y : x == y;
            g_pops.cond++; if (S.render_disabled) g_pops.cond_off++;
            break;
        }
        case OP_RASTER_DISCARD: S.raster_discard = ((const p_flag *)a)->value; break;
        case OP_RESET_COUNTER: { uint64_t t; memcpy(&t, a, 8); if (t == NVN_COUNTER_TYPE_SAMPLES_PASSED) g_samples_cum = 0; break; }
        case OP_REPORT_COUNTER: { uint64_t t, ad; memcpy(&t, a, 8); memcpy(&ad, a + 8, 8); if (condorder_on()) counter_report(ad, (uint32_t)t); break; }
        case OP_SET_TEX_POOL: case OP_SET_SAMP_POOL: break;
        default: break;
    }
}

/* ---------- present: count frames; TOTK_VK_SHOT=<n> saves every n-th presented image as PNG (headless check) ---------- */
static uint64_t g_present_n, g_present_t0, g_last_ticket; static double g_pres_t[64];
static uint32_t crc_table[256];
static uint32_t crc32_(const uint8_t *p, size_t n, uint32_t c) {
    if (!crc_table[1]) for (uint32_t i = 0; i < 256; i++) { uint32_t v = i; for (int k = 0; k < 8; k++) v = v & 1 ? 0xedb88320u ^ (v >> 1) : v >> 1; crc_table[i] = v; }
    c = ~c; for (size_t i = 0; i < n; i++) c = crc_table[(c ^ p[i]) & 0xff] ^ (c >> 8); return ~c;
}

/* ---------- pass dump (TOTK_INPUT_FILE "passdump <n>", n frames from the one after next) ----------
   Every pass's attachments (each colour target and the depth) as PNG files in passdump/f<frame>/, plus index.txt (pass
   number, targets with serial / size / NVN format, draws, first fragment shader). For finding the pass where a picture
   problem first appears. Rows are sampled on the GPU (216 rows per attachment, full width) into one host buffer and
   converted on the CPU at the frame's present; float formats are scaled from their own min..max per channel (so linear
   depth or HDR colour is visible), depth likewise. Slow; a diagnostic only. */
#define PD_ROWS 216
#define PD_COLS 384
#define PD_BUF (1024ull << 20)
typedef struct { int pass, att; VkFormat f; int nvnfmt; uint32_t w, h, rows; VkDeviceSize off, rowbytes; uint64_t serial; unsigned level, layer; uint32_t tb; } pd_img;
static long g_pd_first, g_pd_last;   /* frames (present numbers) to dump */
static char g_pdd[32][240]; static int g_npdd;   /* the open pass's draws (index.txt lists them for passes of up to 32) */
static int pd_active(void);
static void pd_draw_note(const char *fmt, ...) {
    if (!pd_active() || g_npdd >= 32) { if (pd_active()) g_npdd++; return; }
    va_list ap; va_start(ap, fmt); vsnprintf(g_pdd[g_npdd], sizeof g_pdd[0], fmt, ap); va_end(ap); g_npdd++;
}
/* TOTK_VK_PD_SPLIT=<fs prefix>[,...] or d16 (diagnostic, with passdump): during a dumped frame, the pass ends after every draw
   whose fragment shader key starts with one of these (d16: every draw in a pass with colour targets and a DEPTH16 depth),
   so each draw's result is dumped on its own. */
static int pd_split_match(const char *key) {
    static const char *e; static int init; if (!init) { init = 1; e = getenv("TOTK_VK_PD_SPLIT"); }
    if (!e || !*e || !key || !pd_active()) return 0;
    if (!strcmp(e, "d16")) return R.dfmt == VK_FORMAT_D16_UNORM && R.ncolor > 0;   /* every pass with colour targets and a DEPTH16 depth (the deferred lighting) */
    for (const char *p = e; *p; ) { const char *q = strchr(p, ','); size_t l = q ? (size_t)(q - p) : strlen(p); if (l && !strncmp(key, p, l)) return 1; p += l; if (*p == ',') p++; }
    return 0;
}
static VkBuffer g_pd_buf; static VkDeviceMemory g_pd_mem; static uint8_t *g_pd_ptr; static VkDeviceSize g_pd_used;
static pd_img *g_pd; static int g_npd, g_cappd, g_pd_pass; static char *g_pd_idx; static size_t g_pd_idxn, g_pd_idxcap; static int g_pd_full;
void dm_gfx_passdump_next(long n) {
    g_pd_first = (long)g_present_n + 2; g_pd_last = g_pd_first + n - 1;
    LOGV("passdump: frames %ld to %ld into passdump/", g_pd_first, g_pd_last);
}
static int pd_active(void) {
    long f = (long)g_present_n + 1; return g_pd_first && f >= g_pd_first && f <= g_pd_last; }
static void pd_idx(const char *fmt, ...) {
    char line[600]; va_list ap; va_start(ap, fmt); int n = vsnprintf(line, sizeof line, fmt, ap); va_end(ap); if (n < 0) return; if (n >= (int)sizeof line) n = sizeof line - 1;
    if (g_pd_idxn + (size_t)n + 1 > g_pd_idxcap) { g_pd_idxcap = (g_pd_idxcap + (size_t)n + 1) * 2; g_pd_idx = realloc(g_pd_idx, g_pd_idxcap); }
    memcpy(g_pd_idx + g_pd_idxn, line, (size_t)n); g_pd_idxn += (size_t)n; g_pd_idx[g_pd_idxn] = 0;
}
static uint32_t pd_texel_bytes(VkFormat f) {
    switch (f) {
        case VK_FORMAT_R8_UNORM: case VK_FORMAT_R8_SRGB: case VK_FORMAT_R8_UINT: case VK_FORMAT_R8_SINT: case VK_FORMAT_R8_SNORM: case VK_FORMAT_S8_UINT: return 1;
        case VK_FORMAT_R8G8_UNORM: case VK_FORMAT_R8G8_UINT: case VK_FORMAT_R8G8_SNORM: case VK_FORMAT_R16_SFLOAT: case VK_FORMAT_R16_UNORM: case VK_FORMAT_R16_UINT: case VK_FORMAT_D16_UNORM: case VK_FORMAT_R5G6B5_UNORM_PACK16:
        case VK_FORMAT_A1R5G5B5_UNORM_PACK16: case VK_FORMAT_R5G5B5A1_UNORM_PACK16: case VK_FORMAT_R4G4B4A4_UNORM_PACK16: return 2;
        case VK_FORMAT_R16G16B16A16_SFLOAT: case VK_FORMAT_R16G16B16A16_UNORM: case VK_FORMAT_R16G16B16A16_UINT: case VK_FORMAT_R32G32_SFLOAT: case VK_FORMAT_R32G32_UINT: return 8;
        case VK_FORMAT_R32G32B32A32_SFLOAT: case VK_FORMAT_R32G32B32A32_UINT: return 16;
        default: return 4;
    }
}
static void pd_copy(VkCommandBuffer cb, vk_tex *x, VkImageAspectFlags aspect, VkFormat f, int att, uint64_t serial, int nvnfmt, unsigned level, unsigned layer) {
    if (!x || vk_tex_samples(x) > 1) return;
    { int bw, bh; nvn_fmt_bpb(vk_tex_nvn_format(x), &bw, &bh); if (bw > 1 || bh > 1) { pd_idx("    att %d: block-compressed target, not dumped\n", att); return; } }
    if (level >= vk_tex_levels(x)) return;
    uint32_t w = vk_tex_width(x, level), h = vk_tex_height(x, level), rows = MINV(h, (uint32_t)PD_ROWS);
    uint32_t tb = aspect == VK_IMAGE_ASPECT_DEPTH_BIT ? (f == VK_FORMAT_D16_UNORM ? 2 : 4) : aspect == VK_IMAGE_ASPECT_STENCIL_BIT ? 1 : pd_texel_bytes(f);
    VkDeviceSize rb = ((VkDeviceSize)w * tb + 3) & ~(VkDeviceSize)3, need = rb * rows;
    if (!g_pd_buf) {
        VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = PD_BUF, .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT};
        if (vkCreateBuffer(V.dev, &bci, NULL, &g_pd_buf)) { g_pd_full = 1; return; }
        VkMemoryRequirements req; vkGetBufferMemoryRequirements(V.dev, g_pd_buf, &req);
        int t = vk_pick_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
        if (t < 0) t = vk_pick_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
        VkMemoryAllocateInfo mai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size, .memoryTypeIndex = (uint32_t)t};
        if (t < 0 || vkAllocateMemory(V.dev, &mai, NULL, &g_pd_mem)) { vkDestroyBuffer(V.dev, g_pd_buf, NULL); g_pd_buf = VK_NULL_HANDLE; g_pd_full = 1; return; }
        vkBindBufferMemory(V.dev, g_pd_buf, g_pd_mem, 0); vkMapMemory(V.dev, g_pd_mem, 0, VK_WHOLE_SIZE, 0, (void **)&g_pd_ptr); g_pd_used = 0;
    }
    if (g_pd_used + need > PD_BUF) { if (!g_pd_full) LOGV("passdump: buffer full at pass %d", g_pd_pass); g_pd_full = 1; return; }
    int is3d = vk_tex_is3d(x);
    VkBufferImageCopy *rg = calloc(rows, sizeof *rg);
    for (uint32_t r = 0; r < rows; r++) {
        uint32_t y = rows == h ? r : (uint32_t)(((uint64_t)r * h + h / 2) / rows); if (y >= h) y = h - 1;
        rg[r] = (VkBufferImageCopy){.bufferOffset = g_pd_used + r * rb, .bufferRowLength = 0, .imageSubresource = {aspect, level, is3d ? 0 : layer, 1},
            .imageOffset = {0, (int32_t)y, is3d ? (int32_t)layer : 0}, .imageExtent = {w, 1, 1}};
    }
    vkCmdCopyImageToBuffer(cb, vk_tex_image(x), VK_IMAGE_LAYOUT_GENERAL, g_pd_buf, rows, rg);
    free(rg);
    if (g_npd == g_cappd) { g_cappd = g_cappd ? 2 * g_cappd : 256; g_pd = realloc(g_pd, (size_t)g_cappd * sizeof *g_pd); }
    g_pd[g_npd++] = (pd_img){g_pd_pass, att, aspect == VK_IMAGE_ASPECT_STENCIL_BIT ? VK_FORMAT_S8_UINT : f, nvnfmt, w, h, rows, g_pd_used, rb, serial, level, layer, tb};
    g_pd_used += (need + 255) & ~(VkDeviceSize)255;
}
static void pd_pass(VkCommandBuffer cb) {
    if (!pd_active()) return;
    int k = g_pd_pass;
    pd_idx("f%llu pass %d: %llu draws, fs %s%s\n", (unsigned long long)(g_present_n + 1), k, (unsigned long long)(g_stat_draws - R.draws0), g_pkey[0] ? g_pkey : "-", R.samples > 1 ? ", multisampled (not dumped)" : "");
    for (uint32_t i = 0; i < R.ncolor && i < 8; i++) if (R.ctex[i]) {
        pd_idx("    c%u: ser %llu %ux%u nvnfmt %#x vk %d level %u layer %u\n", i, (unsigned long long)R.c_serial[i], vk_tex_width(R.ctex[i], R.c_level[i]), vk_tex_height(R.ctex[i], R.c_level[i]), R.c_nvnfmt[i], R.cfmt[i], R.c_level[i], R.c_layer[i]);
        pd_copy(cb, R.ctex[i], VK_IMAGE_ASPECT_COLOR_BIT, R.cfmt[i], (int)i, R.c_serial[i], R.c_nvnfmt[i], R.c_level[i], R.c_layer[i]);
    }
    if (R.dtex) {
        VkImageAspectFlags as = vk_tex_aspect(R.dtex);
        pd_idx("    depth: ser %llu %ux%u vk %d level %u layer %u\n", (unsigned long long)R.d_serial, vk_tex_width(R.dtex, R.d_level), vk_tex_height(R.dtex, R.d_level), vk_tex_format(R.dtex), R.d_level, R.d_layer);
        if (as & VK_IMAGE_ASPECT_DEPTH_BIT) pd_copy(cb, R.dtex, VK_IMAGE_ASPECT_DEPTH_BIT, vk_tex_format(R.dtex), 8, R.d_serial, 0, R.d_level, R.d_layer);
        if (as & VK_IMAGE_ASPECT_STENCIL_BIT) pd_copy(cb, R.dtex, VK_IMAGE_ASPECT_STENCIL_BIT, vk_tex_format(R.dtex), 9, R.d_serial, 0, R.d_level, R.d_layer);
    }
    /* what the pass's draws sampled; screen-space passes (few draws) also get their inputs dumped (level 0, layer 0) */
    uint64_t nd = g_stat_draws - R.draws0;
    if (g_npdd <= 32) for (int i = 0; i < g_npdd; i++) pd_idx("%s\n", g_pdd[i]);
    g_npdd = 0;
    for (int i = 0; i < g_npin; i++) {
        vk_tex *x = g_pin[i].x; if (!x) continue;
        pd_idx("    in%d: ser %llu %ux%u nvnfmt %#x vk %d%s\n", i, (unsigned long long)vk_tex_serial(x), g_pin[i].w, g_pin[i].h, g_pin[i].fmt, vk_tex_format(x), vk_tex_is3d(x) ? " 3D" : "");
        if (nd > 16 || vk_tex_is3d(x)) continue;
        VkImageAspectFlags as = vk_tex_aspect(x);
        pd_copy(cb, x, as & VK_IMAGE_ASPECT_COLOR_BIT ? VK_IMAGE_ASPECT_COLOR_BIT : VK_IMAGE_ASPECT_DEPTH_BIT, vk_tex_format(x), 10 + i, vk_tex_serial(x), g_pin[i].fmt, 0, 0);
    }
    vk_barrier_all(cb);
    g_pd_pass++;
}
static float pd_h2f(uint16_t h) {
    uint32_t s = (uint32_t)(h >> 15) << 31, e = (h >> 10) & 31, m = h & 1023, v;
    if (!e) { if (!m) v = s; else { e = 113; while (!(m & 1024)) { m <<= 1; e--; } m &= 1023; v = s | e << 23 | m << 13; } }
    else if (e == 31) v = s | 0xff << 23 | m << 13; else v = s | (e + 112) << 23 | m << 13;
    float f; memcpy(&f, &v, 4); return f;
}
static float pd_uf(uint32_t bits, int mb) {   /* unsigned 11/10-bit float, 5-bit exponent */
    uint32_t e = bits >> mb, m = bits & ((1u << mb) - 1);
    if (!e) return ldexpf((float)m, -14 - mb); if (e == 31) return m ? NAN : INFINITY;
    return ldexpf(1.0f + (float)m / (float)(1u << mb), (int)e - 15);
}
/* texel -> up to 4 channels (float); returns the channel count */
static int pd_texel(VkFormat f, const uint8_t *p, float c[4]) {
    uint32_t u; memcpy(&u, p, 4);
    switch (f) {
        case VK_FORMAT_R8G8B8A8_UNORM: case VK_FORMAT_R8G8B8A8_SRGB: case VK_FORMAT_A8B8G8R8_UNORM_PACK32: case VK_FORMAT_A8B8G8R8_SRGB_PACK32:
        case VK_FORMAT_R8G8B8A8_UINT: for (int i = 0; i < 4; i++) c[i] = p[i] / 255.0f; return 4;
        case VK_FORMAT_R8G8B8A8_SNORM: for (int i = 0; i < 4; i++) c[i] = (int8_t)p[i] / 127.0f; return 4;
        case VK_FORMAT_B8G8R8A8_UNORM: case VK_FORMAT_B8G8R8A8_SRGB: c[0] = p[2] / 255.0f; c[1] = p[1] / 255.0f; c[2] = p[0] / 255.0f; c[3] = p[3] / 255.0f; return 4;
        case VK_FORMAT_A2B10G10R10_UNORM_PACK32: case VK_FORMAT_A2B10G10R10_UINT_PACK32: c[0] = (u & 1023) / 1023.0f; c[1] = (u >> 10 & 1023) / 1023.0f; c[2] = (u >> 20 & 1023) / 1023.0f; c[3] = (u >> 30) / 3.0f; return 4;
        case VK_FORMAT_B10G11R11_UFLOAT_PACK32: c[0] = pd_uf(u & 2047, 6); c[1] = pd_uf(u >> 11 & 2047, 6); c[2] = pd_uf(u >> 22, 5); return 3;
        case VK_FORMAT_R16G16B16A16_SFLOAT: for (int i = 0; i < 4; i++) { uint16_t h; memcpy(&h, p + 2 * i, 2); c[i] = pd_h2f(h); } return 4;
        case VK_FORMAT_R16G16_SFLOAT: for (int i = 0; i < 2; i++) { uint16_t h; memcpy(&h, p + 2 * i, 2); c[i] = pd_h2f(h); } return 2;
        case VK_FORMAT_R16_SFLOAT: { uint16_t h; memcpy(&h, p, 2); c[0] = pd_h2f(h); return 1; }
        case VK_FORMAT_R16G16B16A16_UNORM: case VK_FORMAT_R16G16B16A16_UINT: for (int i = 0; i < 4; i++) { uint16_t h; memcpy(&h, p + 2 * i, 2); c[i] = h; } return 4;
        case VK_FORMAT_R16G16_UNORM: case VK_FORMAT_R16G16_UINT: case VK_FORMAT_R16G16_SINT: case VK_FORMAT_R16G16_SNORM: for (int i = 0; i < 2; i++) { uint16_t h; memcpy(&h, p + 2 * i, 2); c[i] = h; } return 2;
        case VK_FORMAT_R16_UNORM: case VK_FORMAT_R16_UINT: case VK_FORMAT_D16_UNORM: { uint16_t h; memcpy(&h, p, 2); c[0] = h; return 1; }
        case VK_FORMAT_R32_SFLOAT: case VK_FORMAT_D32_SFLOAT: case VK_FORMAT_D32_SFLOAT_S8_UINT: memcpy(c, p, 4); return 1;
        case VK_FORMAT_X8_D24_UNORM_PACK32: case VK_FORMAT_D24_UNORM_S8_UINT: c[0] = (float)(u & 0xffffff); return 1;
        case VK_FORMAT_R32_UINT: case VK_FORMAT_R32_SINT: c[0] = (float)u; return 1;
        case VK_FORMAT_R32G32_SFLOAT: memcpy(c, p, 8); return 2;
        case VK_FORMAT_R32G32_UINT: { uint32_t v[2]; memcpy(v, p, 8); c[0] = (float)v[0]; c[1] = (float)v[1]; return 2; }
        case VK_FORMAT_R32G32B32A32_SFLOAT: memcpy(c, p, 16); return 4;
        case VK_FORMAT_R32G32B32A32_UINT: { uint32_t v[4]; memcpy(v, p, 16); for (int i = 0; i < 4; i++) c[i] = (float)v[i]; return 4; }
        case VK_FORMAT_R8_UNORM: case VK_FORMAT_R8_SRGB: case VK_FORMAT_R8_UINT: case VK_FORMAT_S8_UINT: c[0] = p[0] / 255.0f; return 1;
        case VK_FORMAT_R8G8_UNORM: case VK_FORMAT_R8G8_UINT: c[0] = p[0] / 255.0f; c[1] = p[1] / 255.0f; return 2;
        default: c[0] = (u & 255) / 255.0f; c[1] = (u >> 8 & 255) / 255.0f; c[2] = (u >> 16 & 255) / 255.0f; c[3] = (u >> 24) / 255.0f; return 4;
    }
}
static int pd_unorm8(VkFormat f) {
    return f == VK_FORMAT_R8G8B8A8_UNORM || f == VK_FORMAT_R8G8B8A8_SRGB || f == VK_FORMAT_A8B8G8R8_UNORM_PACK32 || f == VK_FORMAT_A8B8G8R8_SRGB_PACK32 ||
           f == VK_FORMAT_B8G8R8A8_UNORM || f == VK_FORMAT_B8G8R8A8_SRGB || f == VK_FORMAT_A2B10G10R10_UNORM_PACK32 || f == VK_FORMAT_R8_UNORM || f == VK_FORMAT_R8G8_UNORM;
}
static void write_png(const char *path, const uint8_t *rgba, uint32_t w, uint32_t h, size_t pitch);
static void pd_frame_end(uint64_t n) {
    if (!g_pd_first || (long)n < g_pd_first || (long)n > g_pd_last) return;
    vk_rec_lock(); vk_flush_wait();
    char dir[64]; mkdir("passdump", 0755); snprintf(dir, sizeof dir, "passdump/f%llu", (unsigned long long)n); mkdir(dir, 0755);
    for (int i = 0; i < g_npd; i++) {
        pd_img *d = &g_pd[i];
        uint32_t cols = MINV(d->w, (uint32_t)PD_COLS), tb = d->tb;
        float *v = malloc((size_t)cols * d->rows * 4 * sizeof *v); int nch = 1;
        float lo[4] = {INFINITY, INFINITY, INFINITY, INFINITY}, hi[4] = {-INFINITY, -INFINITY, -INFINITY, -INFINITY};
        VkFormat cf = d->att == 9 ? VK_FORMAT_S8_UINT : d->f;
        for (uint32_t r = 0; r < d->rows; r++) for (uint32_t x = 0; x < cols; x++) {
            uint32_t sx = cols == d->w ? x : (uint32_t)(((uint64_t)x * d->w + d->w / 2) / cols); if (sx >= d->w) sx = d->w - 1;
            float c[4] = {0, 0, 0, 1}; nch = pd_texel(cf, g_pd_ptr + d->off + r * d->rowbytes + (size_t)sx * tb, c);
            for (int k = 0; k < 4; k++) { v[((size_t)r * cols + x) * 4 + k] = c[k]; if (k < nch && isfinite(c[k])) { if (c[k] < lo[k]) lo[k] = c[k]; if (c[k] > hi[k]) hi[k] = c[k]; } }
        }
        int fixed = pd_unorm8(cf);
        uint8_t *px = malloc((size_t)cols * d->rows * 4);
        for (size_t t = 0; t < (size_t)cols * d->rows; t++) {
            for (int k = 0; k < 4; k++) {
                float c = v[t * 4 + k]; uint8_t o;
                if (k >= nch) o = k == 3 ? 255 : (nch == 1 ? px[t * 4] : 0);
                else if (!isfinite(c)) o = k == 0 ? 255 : k == 1 ? 0 : 255;   /* NaN / inf: magenta */
                else if (fixed) o = (uint8_t)(c < 0 ? 0 : c > 1 ? 255 : c * 255.0f + 0.5f);
                else { float a = lo[k], b = hi[k]; o = b > a ? (uint8_t)((c - a) / (b - a) * 255.0f + 0.5f) : 128; }
                px[t * 4 + k] = o;
            }
            if (nch < 4) px[t * 4 + 3] = 255;
            if (nch == 1) px[t * 4 + 1] = px[t * 4 + 2] = px[t * 4];   /* grey */
            if (nch >= 4 && !fixed) px[t * 4 + 3] = 255;
            if (fixed) px[t * 4 + 3] = 255;   /* alpha shown opaque; its range is in index.txt */
        }
        char path[160]; const char *kind = d->att == 8 ? "depth" : d->att == 9 ? "stencil" : "c";
        if (d->att >= 10) snprintf(path, sizeof path, "%s/p%03d_in%d_ser%llu_%ux%u.png", dir, d->pass, d->att - 10, (unsigned long long)d->serial, d->w, d->h);
        else if (d->att < 8) snprintf(path, sizeof path, "%s/p%03d_c%d_%ux%u.png", dir, d->pass, d->att, d->w, d->h);
        else snprintf(path, sizeof path, "%s/p%03d_%s_%ux%u.png", dir, d->pass, kind, d->w, d->h);
        write_png(path, px, cols, d->rows, (size_t)cols * 4);
        if (d->f == VK_FORMAT_D16_UNORM) {   /* DEPTH16: the raw steps too (class-depth EQUAL questions need exact values): cols x rows uint16, row-major */
            char rp[180]; snprintf(rp, sizeof rp, "%.*s.d16", (int)(strlen(path) - 4), path);
            FILE *rf = fopen(rp, "wb");
            if (rf) { for (uint32_t r = 0; r < d->rows; r++) for (uint32_t x = 0; x < cols; x++) {
                    uint32_t sx = cols == d->w ? x : (uint32_t)(((uint64_t)x * d->w + d->w / 2) / cols); if (sx >= d->w) sx = d->w - 1;
                    fwrite(g_pd_ptr + d->off + r * d->rowbytes + (size_t)sx * 2, 2, 1, rf); }
                fclose(rf); }
        }
        pd_idx("p%03d %s%d: ranges", d->pass, d->att >= 10 ? "in" : d->att < 8 ? "c" : kind, d->att >= 10 ? d->att - 10 : d->att < 8 ? d->att : 0);
        for (int k = 0; k < nch; k++) pd_idx(" [%g, %g]", (double)lo[k], (double)hi[k]);
        pd_idx("%s\n", fixed ? " (unorm: shown as is)" : " (scaled to its range; magenta = NaN / inf)");
        free(v); free(px);
    }
    pd_idx("skips at frame end (cumulative): no program %llu, disabled %llu, culled %llu, primitive %llu, shader missing %llu, scissor %llu, pipeline %llu\n",
         (unsigned long long)g_stat_skips[0], (unsigned long long)g_stat_skips[1], (unsigned long long)g_stat_skips[2],
         (unsigned long long)g_stat_skips[3], (unsigned long long)g_stat_skips[4], (unsigned long long)g_stat_skips[6], (unsigned long long)g_stat_skips[7]);
    char ip[96]; snprintf(ip, sizeof ip, "%s/index.txt", dir);
    FILE *f = fopen(ip, "w"); if (f) { if (g_pd_idx) fwrite(g_pd_idx, 1, g_pd_idxn, f); fclose(f); }
    LOGV("passdump: frame %llu, %d passes, %d images in %s%s", (unsigned long long)n, g_pd_pass, g_npd, dir, g_pd_full ? " (buffer full: later passes missing)" : "");
    g_npd = 0; g_pd_pass = 0; g_pd_idxn = 0; if (g_pd_idx) g_pd_idx[0] = 0; g_pd_used = 0; g_pd_full = 0;
    if ((long)n >= g_pd_last) {
        g_pd_first = g_pd_last = 0;
        if (g_pd_buf) { vkUnmapMemory(V.dev, g_pd_mem); vkDestroyBuffer(V.dev, g_pd_buf, NULL); vkFreeMemory(V.dev, g_pd_mem, NULL); g_pd_buf = VK_NULL_HANDLE; g_pd_mem = VK_NULL_HANDLE; g_pd_ptr = NULL; }
    }
    vk_rec_unlock();
}
static void png_chunk(FILE *f, const char *type, const uint8_t *data, uint32_t n) {
    uint8_t len[4] = {(uint8_t)(n >> 24), (uint8_t)(n >> 16), (uint8_t)(n >> 8), (uint8_t)n}; fwrite(len, 1, 4, f); fwrite(type, 1, 4, f); if (n) fwrite(data, 1, n, f);
    uint32_t c = crc32_((const uint8_t *)type, 4, 0); c = crc32_(data, n, c);
    uint8_t cb[4] = {(uint8_t)(c >> 24), (uint8_t)(c >> 16), (uint8_t)(c >> 8), (uint8_t)c}; fwrite(cb, 1, 4, f);
}
static void write_png(const char *path, const uint8_t *rgba, uint32_t w, uint32_t h, size_t pitch) {   /* uncompressed (stored) deflate: no zlib needed */
    FILE *f = fopen(path, "wb"); if (!f) { RED(4, "cannot write %s", path); return; }
    static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', 13, 10, 26, 10}; fwrite(sig, 1, 8, f);
    uint8_t ihdr[13] = {(uint8_t)(w >> 24), (uint8_t)(w >> 16), (uint8_t)(w >> 8), (uint8_t)w, (uint8_t)(h >> 24), (uint8_t)(h >> 16), (uint8_t)(h >> 8), (uint8_t)h, 8, 6, 0, 0, 0};
    png_chunk(f, "IHDR", ihdr, 13);
    size_t raw_n = (size_t)h * (1 + (size_t)w * 4), blocks = (raw_n + 65534) / 65535;
    uint8_t *z = malloc(2 + raw_n + blocks * 5 + 4), *raw = malloc(raw_n);
    for (uint32_t y = 0; y < h; y++) { raw[y * (1 + (size_t)w * 4)] = 0; memcpy(raw + y * (1 + (size_t)w * 4) + 1, rgba + y * pitch, (size_t)w * 4); }
    size_t o = 0; z[o++] = 0x78; z[o++] = 0x01;
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < raw_n; i++) { a = (a + raw[i]) % 65521; b = (b + a) % 65521; }
    for (size_t at = 0; at < raw_n; at += 65535) {
        size_t n = raw_n - at < 65535 ? raw_n - at : 65535;
        z[o++] = at + n >= raw_n; z[o++] = (uint8_t)n; z[o++] = (uint8_t)(n >> 8); z[o++] = (uint8_t)~n; z[o++] = (uint8_t)(~n >> 8);
        memcpy(z + o, raw + at, n); o += n;
    }
    uint32_t ad = b << 16 | a; z[o++] = (uint8_t)(ad >> 24); z[o++] = (uint8_t)(ad >> 16); z[o++] = (uint8_t)(ad >> 8); z[o++] = (uint8_t)ad;
    png_chunk(f, "IDAT", z, (uint32_t)o); png_chunk(f, "IEND", NULL, 0);
    fclose(f); free(z); free(raw);
}
static void shot(const tex_t *t, uint64_t n, const int crop[4]) {
    vk_tex *x = vk_tex_get(t); if (!x) return;
    uint32_t w = vk_tex_width(x, 0), h = vk_tex_height(x, 0); int32_t cx = 0, cy = 0;
    /* the window crop (top-left origin, nvn_mtl.m present_encode): the game renders below 1080p into a 1080p texture */
    if (crop && crop[2] > 0 && crop[3] > 0 && crop[0] >= 0 && crop[1] >= 0 && (uint32_t)(crop[0] + crop[2]) <= w && (uint32_t)(crop[1] + crop[3]) <= h) {
        cx = crop[0]; cy = crop[1]; w = (uint32_t)crop[2]; h = (uint32_t)crop[3];
    }
    vk_end_rendering();
    VkCommandBuffer cb = vk_cb(); vk_barrier_all(cb);
    VkBuffer b; VkDeviceSize o; uint8_t *p = vk_transient((size_t)w * h * 4, 16, &b, &o);
    VkBufferImageCopy r = {.bufferOffset = o, .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageOffset = {cx, cy, 0}, .imageExtent = {w, h, 1}};
    int bpb = nvn_fmt_bpb(t->d.format, NULL, NULL);
    if (bpb != 4) { RED(2, "screenshot of a format %#x image (not 4 bytes per pixel) skipped", t->d.format); return; }
    vkCmdCopyImageToBuffer(cb, vk_tex_image(x), VK_IMAGE_LAYOUT_GENERAL, b, 1, &r);
    vk_flush_wait();
    VkFormat f = vk_tex_format(x);
    if (f == VK_FORMAT_B8G8R8A8_UNORM || f == VK_FORMAT_B8G8R8A8_SRGB) for (size_t i = 0; i < (size_t)w * h; i++) { uint8_t c = p[4 * i]; p[4 * i] = p[4 * i + 2]; p[4 * i + 2] = c; }
    for (size_t i = 0; i < (size_t)w * h; i++) p[4 * i + 3] = 255;
    const char *dir = getenv("TOTK_VK_SHOT_DIR"); if (!dir || !*dir) dir = "shots";
    mkdir(dir, 0755);
    char path[1024]; snprintf(path, sizeof path, "%s/frame_%06llu.png", dir, (unsigned long long)n);
    write_png(path, p, w, h, (size_t)w * 4);
    LOGV("saved %s (%ux%u)", path, w, h);
}
#ifdef TOTK_ANDROID_APP
/* ---------- the app's window (frontend_android.c) ----------
   Each presented frame (its window crop) is copied by the GPU into one of three host-visible buffers, submitted without
   waiting, and a presenter thread waits for that submission, then writes the pixels into the window
   (ANativeWindow_lock / unlockAndPost; the compositor scales them to the screen). It works with any driver, including a
   Turnip loaded directly, which has no swapchain of its own (the Android loader provides those). When all three
   buffers are still on their way to the screen, the frame is not shown (the game is not held up). */
#include <android/native_window.h>
#include <dlfcn.h>
#include <stdatomic.h>
void front_frame_presented(void);
static void pctl(uint64_t *v, int n, double out[4]);   /* perf summary, below */
enum { NPRES = 8 };   /* 3 were enough before frame generation; a real frame and up to 3 generated ones each take one */
typedef struct { VkBuffer buf; VkDeviceMemory mem; uint8_t *ptr; VkDeviceSize size; int coherent; _Atomic int busy; uint64_t fence; uint32_t w, h; int swap; int gen; uint64_t frame, ctr[10], t_call;   /* t_call: when the game called nvnQueuePresentTexture */
    VkBuffer xbuf; VkDeviceMemory xmem; uint8_t *xptr; VkDeviceSize xsize; int xcoherent, masked, real; } pres_buf;   /* real: its frame's own picture */   /* x: the scene and the final picture of a generated frame (HUD apart) */
int vk_fg_multiplier(void); int vk_fg_begin(VkCommandBuffer cb, VkImage src, uint32_t w, uint32_t h, VkFormat src_fmt); int vk_fg_dispatch(void); VkImage vk_fg_output(VkCommandBuffer cb, int i);   /* vk_fg.c */
static uint64_t g_next_call;   /* the game's present call for the next mtl_present (hle_nvn.c present_exec, same thread) */
void mtl_present_called_at(uint64_t t) { g_next_call = t; }
static _Atomic uint64_t g_fg_spacing;   /* frame generation: the time between two pictures shown (the real frame interval / multiplier) */
static pres_buf g_pres[NPRES];
static pthread_mutex_t g_win_mu = PTHREAD_MUTEX_INITIALIZER; static pthread_cond_t g_pres_cv = PTHREAD_COND_INITIALIZER;
static ANativeWindow *g_win; static uint32_t g_win_w, g_win_h, g_win_pw, g_win_ph;   /* buffer size set; the window's own size */
static int g_pres_q[NPRES * 2], g_pres_qh, g_pres_qt;
static uint64_t g_pres_shown, g_pres_dropped;
uint64_t vk_window_posts(void) { return __atomic_load_n(&g_pres_shown, __ATOMIC_RELAXED); }   /* pictures shown, generated ones included (the second screen's readout) */
/* The frame cap as the window's frame rate (2026-10-23): Android then runs the display at a rate it divides (40 fps ->
   120 Hz, every frame 3 refreshes), so a cap other than 30 / 60 still paces evenly; with frame generation, the pictures
   shown (cap x multiplier). Again when frame generation's settings change (vk_fg.c). */
static void window_rate_locked(void) {
    if (!g_win) return;
    const char *hz = getenv("TOTK_UNCAPPED_HZ"); float r = hz ? (float)atof(hz) * vk_fg_multiplier() : 0;
    if (r < 20 || r > 240) return;
    typedef int32_t (*set_rate_fn)(ANativeWindow *, float, int8_t);   /* libnativewindow (API 30), looked up at run time: no link dependency */
    static set_rate_fn set_rate; static int looked;
    if (!looked) { looked = 1; void *lib = dlopen("libnativewindow.so", RTLD_NOW); set_rate = (set_rate_fn)(lib ? dlsym(lib, "ANativeWindow_setFrameRate") : dlsym(RTLD_DEFAULT, "ANativeWindow_setFrameRate")); }
    int32_t e = set_rate ? set_rate(g_win, r, 1) : -1;   /* 1: ANATIVEWINDOW_FRAME_RATE_COMPATIBILITY_FIXED_SOURCE */
    fprintf(stderr, "[nvn-vk] window frame rate %.0f fps%s\n", r, !set_rate ? " (not available)" : e ? " (refused)" : "");
}
void vk_window_rate(void) { pthread_mutex_lock(&g_win_mu); window_rate_locked(); pthread_mutex_unlock(&g_win_mu); atomic_store(&g_fg_spacing, 0); }
void vk_window_set(ANativeWindow *w) {
    pthread_mutex_lock(&g_win_mu);
    if (g_win) ANativeWindow_release(g_win);
    g_win = w; g_win_w = g_win_h = 0;
    if (w) { ANativeWindow_acquire(w); g_win_pw = (uint32_t)ANativeWindow_getWidth(w); g_win_ph = (uint32_t)ANativeWindow_getHeight(w);
        window_rate_locked(); }
    pthread_mutex_unlock(&g_win_mu);
    fprintf(stderr, "[nvn-vk] window %s\n", w ? "attached" : "detached");
}
/* Diagnostics: a frame whose picture is almost all black right after a normal one (the brief black flashes) is logged
   with what happened since the last frame shown, and saved as shots/black_<frame>.png. TOTK_VK_BLACK=0: off. */
static void black_check(const pres_buf *b) {
    /* A flash: a frame far darker than the last few, with the brightness back within five frames (a fade to black stays
       dark and is not one). The dark frame is kept until that is known; up to 40 are logged and 5 saved. */
    static int on = -1; if (on < 0) { const char *e = getenv("TOTK_VK_BLACK"); on = !(e && !strcmp(e, "0")); }
    static double avg = -1, dark_m, dark_avg; static uint64_t pctr[10], dctr[10], pframe, dframe, dprev; static int saved, logged, pending;
    static uint8_t *keep; static uint32_t kw, kh;
    if (!on) return;
    double sum = 0;   /* brightness at 49 points across the picture */
    for (uint32_t gy = 1; gy < 8; gy++) for (uint32_t gx = 1; gx < 8; gx++) { const uint8_t *p = b->ptr + ((size_t)(b->h * gy / 8) * b->w + b->w * gx / 8) * 4; sum += (p[0] + p[1] + p[2]) / 3.0; }
    double m = sum / 49;
    if (pending) {
        if (m >= dark_avg * 0.75) {   /* back: it was a flash */
            if (logged++ < 40) {
                uint64_t d[10]; for (int i = 0; i < 10; i++) d[i] = dctr[i] - pctr[i];
                LOGV("【红】dark flash at frame %llu (after frame %llu; brightness %.1f, around it %.1f; back at frame %llu): before it %llu draws, %llu passes, %llu skipped, %llu images made, %llu shared reuses, %llu uploads, %llu re-uploads, %llu swept, %llu CPU-pool images shared, %llu serials switched",
                     (unsigned long long)dframe, (unsigned long long)dprev, dark_m, dark_avg, (unsigned long long)b->frame, (unsigned long long)d[0], (unsigned long long)d[1], (unsigned long long)d[2],
                     (unsigned long long)d[3], (unsigned long long)d[4], (unsigned long long)d[5], (unsigned long long)d[6], (unsigned long long)d[7], (unsigned long long)d[8], (unsigned long long)d[9]);
            }
            if (logged <= 12) probe_report(dframe, b->frame);   /* against the frame after it: the same view after a camera cut */
            if (saved < 5 && keep) { saved++; mkdir("shots", 0755); char path[256]; snprintf(path, sizeof path, "shots/flash_%06llu.png", (unsigned long long)dframe); write_png(path, keep, kw, kh, (size_t)kw * 4); }
            pending = 0;
        } else if (b->frame - dframe > 5) { pending = 0; avg = m; }   /* stayed dark: a fade (the new brightness is the normal one now) */
    } else if (avg > 6 && m < avg * 0.45) {
        pending = 1; dark_m = m; dark_avg = avg; dframe = b->frame; dprev = pframe; memcpy(dctr, b->ctr, sizeof dctr);
        if (saved < 5) { size_t n = (size_t)b->w * b->h * 4; if (!keep || kw * kh * 4 < n) keep = realloc(keep, n); memcpy(keep, b->ptr, n); kw = b->w; kh = b->h; }
        return;   /* pctr stays the frame before it */
    }
    if (!pending && m >= avg * 0.45) avg = avg < 0 ? m : avg * 0.75 + m * 0.25;
    if (!pending) { memcpy(pctr, b->ctr, sizeof pctr); pframe = b->frame; }
}
static void *presenter(void *arg) {
    (void)arg;
    for (;;) {
        pthread_mutex_lock(&g_win_mu);
        while (g_pres_qh == g_pres_qt) pthread_cond_wait(&g_pres_cv, &g_win_mu);
        int i = g_pres_q[g_pres_qh++ % (NPRES * 2)];
        pthread_mutex_unlock(&g_win_mu);
        pres_buf *b = &g_pres[i];
        mtl_wait(b->fence, 2000000000ull);
        if (!b->coherent) { VkMappedMemoryRange r = {VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, .memory = b->mem, .size = VK_WHOLE_SIZE}; vkInvalidateMappedMemoryRanges(V.dev, 1, &r); }
        if (b->masked) {   /* the HUD apart: only when the final image is what the frame showed (the title draws effects onto the output after it) */
            if (!b->xcoherent) { VkMappedMemoryRange r = {VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, .memory = b->xmem, .size = VK_WHOLE_SIZE}; vkInvalidateMappedMemoryRanges(V.dev, 1, &r); }
            /* 49 pixels of the final picture (xptr) against the same pixels of the frame shown: a bias (a fade, an effect drawn
               onto the output after the HUD image) means the generated scene lacks it. Not per-pixel: the game scales to the
               output and back, which blurs edges either way (2026-10-23: a per-pixel test rejected every in-game frame). */
            const pres_buf *rb = &g_pres[b->real]; int k = 0; double bias = 0;
            if (!rb->coherent) { VkMappedMemoryRange r = {VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, .memory = rb->mem, .size = VK_WHOLE_SIZE}; vkInvalidateMappedMemoryRanges(V.dev, 1, &r); }   /* its own turn comes after this */
            for (uint32_t gy = 1; gy < 8; gy++) for (uint32_t gx = 1; gx < 8; gx++, k++) { size_t o = ((size_t)(b->h * gy / 8) * b->w + b->w * gx / 8) * 4; for (int c = 0; c < 3; c++) bias += (int)b->xptr[k * 4 + c] - (int)rb->ptr[o + c]; }
            bias /= 49 * 3;
            if (bias > 6 || bias < -6) {   /* not shown: the frame's own picture follows */
                static uint64_t n; if (n++ % 300 == 0) LOGV("frame generation: a generated frame not shown, the frame drew more than the HUD image holds (bias %.1f; %llu so far)", bias, (unsigned long long)n);
                atomic_store(&b->busy, 0); continue;
            }
        }
        pthread_mutex_lock(&g_win_mu);
        ANativeWindow *w = g_win; if (w) ANativeWindow_acquire(w);
        int resize = w && (g_win_w != b->w || g_win_h != b->h);
        if (resize) { g_win_w = b->w; g_win_h = b->h; }
        pthread_mutex_unlock(&g_win_mu);
        {   /* frame generation: a picture after a generated one waits until its turn, so all are shown evenly spaced */
            static uint64_t last_post; static int prev_gen;
            uint64_t sp = atomic_load(&g_fg_spacing), now = host_now_ns();
            if (prev_gen && sp && sp < 50000000ull && now < last_post + sp) { uint64_t d = last_post + sp - now; struct timespec ts = {(time_t)(d / 1000000000ull), (long)(d % 1000000000ull)}; nanosleep(&ts, NULL); }
            last_post = host_now_ns(); prev_gen = b->gen != 0;
        }
        if (w) {
            if (resize) ANativeWindow_setBuffersGeometry(w, (int32_t)b->w, (int32_t)b->h, WINDOW_FORMAT_RGBX_8888);
            ANativeWindow_Buffer out; uint64_t t_copy = host_now_ns();
            if (!ANativeWindow_lock(w, &out, NULL)) {
                uint32_t rw = MINV(b->w, (uint32_t)out.width), rh = MINV(b->h, (uint32_t)out.height);
                for (uint32_t y = 0; y < rh; y++) {
                    const uint8_t *src = b->ptr + (size_t)y * b->w * 4; uint8_t *dst = (uint8_t *)out.bits + (size_t)y * (size_t)out.stride * 4;
                    if (!b->swap) memcpy(dst, src, (size_t)rw * 4);
                    else for (uint32_t x = 0; x < rw; x++) { dst[4 * x] = src[4 * x + 2]; dst[4 * x + 1] = src[4 * x + 1]; dst[4 * x + 2] = src[4 * x]; dst[4 * x + 3] = 255; }
                }
                ANativeWindow_unlockAndPost(w);
                g_pres_shown++; if (!b->gen) front_frame_presented();
                if (!b->gen && b->t_call) {   /* latency: the game's present call to its real picture posted (input is read ~1 frame before the call; the compositor adds a constant) */
                    static uint64_t lat[300], rv[300], last_real; static int nl;
                    uint64_t now = host_now_ns(); lat[nl] = now - b->t_call; rv[nl] = last_real ? now - last_real : 0; last_real = now;
                    if (++nl == 300) { double q[4], r[4]; pctl(lat, nl, q); pctl(rv, nl, r);
                        LOGV("latency, last 300 real frames: present call to window post p50 %.1f p90 %.1f p99 %.1f ms; real frame interval p50 %.1f; input to post ~%.1f ms (interval + p50; frame generation x%d)",
                             q[0], q[1], q[2], r[0], q[0] + r[0], vk_fg_multiplier());
                        nl = 0; }
                }
                {   /* window posts: the interval between them is what the screen shows (pacing); and the copy's cost */
                    static uint64_t last, iv[300], cp_sum; static int ni;
                    uint64_t now = host_now_ns();
                    if (last && ni < 300) iv[ni++] = now - last;
                    last = now; cp_sum += now - t_copy;
                    if (ni == 300) { double q[4]; pctl(iv, ni, q); int j34 = 0; for (int k = 0; k < ni; k++) if (iv[k] > 34000000ull) j34++;
                        LOGV("window posts, last 300: interval p50 %.1f p90 %.1f p99 %.1f max %.1f ms, %d over 34 ms; lock + copy + post %.2f ms each", q[0], q[1], q[2], q[3], j34, cp_sum / 1e6 / ni);
                        ni = 0; cp_sum = 0; }
                }
            }
            ANativeWindow_release(w);
        }
        black_check(b);
        atomic_store(&b->busy, 0);
    }
    return NULL;
}
static int host_buf(VkBuffer *buf, VkDeviceMemory *mem, uint8_t **ptr, VkDeviceSize *have, int *coherent, VkDeviceSize size) {   /* a mapped readback buffer */
    if (*have >= size) return 1;
    if (*buf) { vkDestroyBuffer(V.dev, *buf, NULL); vkFreeMemory(V.dev, *mem, NULL); *buf = VK_NULL_HANDLE; *have = 0; }
    VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size, .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    if (vkCreateBuffer(V.dev, &bci, NULL, buf)) return 0;
    VkMemoryRequirements req; vkGetBufferMemoryRequirements(V.dev, *buf, &req);
    /* the CPU reads every byte: cached memory first */
    int t = vk_pick_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (t < 0) t = vk_pick_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
    if (t < 0) t = vk_pick_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VkMemoryAllocateInfo mai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size, .memoryTypeIndex = (uint32_t)t};
    if (t < 0 || vkAllocateMemory(V.dev, &mai, NULL, mem)) { vkDestroyBuffer(V.dev, *buf, NULL); *buf = VK_NULL_HANDLE; return 0; }
    vkBindBufferMemory(V.dev, *buf, *mem, 0);
    if (vkMapMemory(V.dev, *mem, 0, VK_WHOLE_SIZE, 0, (void **)ptr)) return 0;
    *coherent = !!(V.mp.memoryTypes[t].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    *have = size;
    return 1;
}
static int pres_alloc(pres_buf *b, VkDeviceSize size) { return host_buf(&b->buf, &b->mem, &b->ptr, &b->size, &b->coherent, size); }
typedef struct { VkImage img; vk_alloc mem; uint32_t w, h; VkFormat f; } win_img;
static int win_image_u(win_img *x, uint32_t w, uint32_t h, VkFormat f, VkImageUsageFlags extra, const char *what) {   /* a window-sized image (GENERAL layout) */
    if (x->img && x->w == w && x->h == h && x->f == f) return 1;
    if (x->img) { vk_flush_wait(); vkDestroyImage(V.dev, x->img, NULL); vk_mem_free(&x->mem); x->img = VK_NULL_HANDLE; }
    VkImageCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = f, .extent = {w, h, 1}, .mipLevels = 1, .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | extra};
    if (vkCreateImage(V.dev, &ci, NULL, &x->img)) { RED(2, "%s image %ux%u could not be made", what, w, h); x->img = VK_NULL_HANDLE; return 0; }
    VkMemoryRequirements req; vkGetImageMemoryRequirements(V.dev, x->img, &req);
    if (!vk_mem_alloc(&req, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &x->mem)) { vkDestroyImage(V.dev, x->img, NULL); x->img = VK_NULL_HANDLE; return 0; }
    vkBindImageMemory(V.dev, x->img, x->mem.mem, x->mem.off);
    VkImageMemoryBarrier2 ib = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2, .srcStageMask = VK_PIPELINE_STAGE_2_NONE, .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
        .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT, .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_GENERAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = x->img, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    VkDependencyInfo di = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &ib};
    vkCmdPipelineBarrier2(vk_cb(), &di);
    VkClearColorValue black = {{0, 0, 0, 1}}; VkImageSubresourceRange rr = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};   /* black bars where the frame does not fill it */
    vkCmdClearColorImage(vk_cb(), x->img, VK_IMAGE_LAYOUT_GENERAL, &black, 1, &rr); vk_barrier_all(vk_cb());
    x->w = w; x->h = h; x->f = f;
    fprintf(stderr, "[nvn-vk] %s image %ux%u\n", what, w, h);
    return 1;
}
static int win_image(win_img *x, uint32_t w, uint32_t h, VkFormat f, const char *what) { return win_image_u(x, w, h, f, 0, what); }
static win_img g_pi;   /* the window-sized image the frame is scaled into */
#define g_pimg g_pi.img
static int pres_image(uint32_t w, uint32_t h, VkFormat f) { return win_image(&g_pi, w, h, f, "window picture"); }

/* ---------- the HUD apart from frame generation (TOTK, 2026-10-23) ----------
   TOTK draws its HUD (fs 309199f103aa91e3: icons, hearts, minimap, text) onto the finished 1280x720 scene image, which
   a pass after it scales into the output (pass dumps at Lookout Landing and the field: composite fs 0ac9a549..., the
   HUD passes, the scale fs dd3d1e0ebf732428, then the present). Just before the first pass (of any shader) drawn onto the image the
   composite finished, the image is blitted into the window-sized "scene" picture; when the scale pass starts reading
   it, into the "final" picture, the same way (an sRGB staging image encodes like the game's output view), so the two
   are bit-identical wherever no HUD was drawn. Frame generation then interpolates scenes only; a compute pass
   (shaders/hud_mask.comp) gives a generated picture the final pixel wherever final != scene: the HUD stays exact.
   The presenter checks 49 pixels of the final picture against the frame shown and drops the generated picture on a
   bias (an effect drawn onto the output after the HUD image, e.g. the title). A frame without that sequence (menus,
   cutscenes) feeds the window picture as before. TOTK_LSFG_HUD=0: off; TOTK_LSFG_SCENE_FS / _OUT_FS:
   the shader key prefixes. */
static win_img g_hs, g_hf;   /* scene, final */
/* this frame's HUD target, from its first HUD pass to the present: the backend's record of it, resolved then (the game's
   texture object may describe something else by the present: per-frame render-target objects); its image is asked for at
   each blit (the backend may switch a texture to another image while the frame renders it) */
static vk_tex *g_hud_x;
static int32_t g_hud_rect[4];   /* the window picture's frame rectangle (x, y, w, h), from the last present */
int vk_fg_hud(void);   /* vk_fg.c: changeable while the game runs */
static int hud_on(void) {
    int on = vk_fg_multiplier() > 1 && vk_fg_hud();
    static int said = -1; if (on != said) { said = on; if (on) LOGV("frame generation: the HUD is kept out of it (TOTK_LSFG_HUD=0 to mix it in)"); }
    return on;
}
/* The HUD target holds linear colour; the game's last pass writes it through an sRGB view of the output (a fit of the
   final copy against the shown frame was exactly the sRGB curve, 2026-10-23), so the blit goes through an sRGB image
   (encoding like that write) and its bytes are copied raw into the scene / final picture: display-encoded bytes like
   the window picture's. */
static win_img g_hstage;
static void hud_blit(VkCommandBuffer cb, vk_tex *x, VkImage dst) {   /* the whole target into the window picture's frame rectangle */
    if (!x || !g_hstage.img) return;
    VkImage src = vk_tex_image(x);
    VkImageBlit bl = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .srcOffsets = {{0, 0, 0}, {(int32_t)vk_tex_width(x, 0), (int32_t)vk_tex_height(x, 0), 1}},
                      .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .dstOffsets = {{g_hud_rect[0], g_hud_rect[1], 0}, {g_hud_rect[0] + g_hud_rect[2], g_hud_rect[1] + g_hud_rect[3], 1}}};
    vkCmdBlitImage(cb, src, VK_IMAGE_LAYOUT_GENERAL, g_hstage.img, VK_IMAGE_LAYOUT_GENERAL, 1, &bl, VK_FILTER_LINEAR);
    vk_barrier_all(cb);
    VkImageCopy c = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .extent = {g_hstage.w, g_hstage.h, 1}};
    vkCmdCopyImage(cb, g_hstage.img, VK_IMAGE_LAYOUT_GENERAL, dst, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
}
/* The mask on the GPU (shaders/hud_mask.comp): the generated picture takes the final pixel wherever final != scene */
#include "shaders/hud_mask.spv.h"
static VkDescriptorSetLayout g_hm_dsl; static VkPipelineLayout g_hm_layout; static VkPipeline g_hm_pipe; static int g_hm_state;
static int hud_pipeline(void) {
    if (g_hm_state) return g_hm_state > 0;
    g_hm_state = -1;
    VkDescriptorSetLayoutBinding b[3];
    for (int i = 0; i < 3; i++) b[i] = (VkDescriptorSetLayoutBinding){(uint32_t)i, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL};
    VkDescriptorSetLayoutCreateInfo dci = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 3, .pBindings = b};
    if (vkCreateDescriptorSetLayout(V.dev, &dci, NULL, &g_hm_dsl)) return 0;
    VkPipelineLayoutCreateInfo lci = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &g_hm_dsl};
    if (vkCreatePipelineLayout(V.dev, &lci, NULL, &g_hm_layout)) return 0;
    VkShaderModuleCreateInfo mci = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = sizeof hud_mask_spv, .pCode = hud_mask_spv};
    VkShaderModule m; if (vkCreateShaderModule(V.dev, &mci, NULL, &m)) return 0;
    VkComputePipelineCreateInfo pci = {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, .stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = m, .pName = "main"}, .layout = g_hm_layout};
    VkResult r = vkCreateComputePipelines(V.dev, VK_NULL_HANDLE, 1, &pci, NULL, &g_hm_pipe);
    vkDestroyShaderModule(V.dev, m, NULL);
    if (r) { RED(2, "HUD mask pipeline: %d", r); return 0; }
    g_hm_state = 1; return 1;
}
static struct { VkImage img; VkImageView v; } g_hv[8]; static int g_nhv;
void vk_hud_forget(VkImage img) {   /* vk_fg.c destroys a generated picture: its view first (a new image may get the same handle) */
    for (int i = 0; i < g_nhv; i++) if (g_hv[i].img == img) { vkDestroyImageView(V.dev, g_hv[i].v, NULL); g_hv[i] = g_hv[--g_nhv]; return; }
}
static VkImageView hud_view(VkImage img) {   /* a storage view per image (a handful: scene, final, the generated pictures) */
#define c g_hv
#define n g_nhv
    for (int i = 0; i < n; i++) if (c[i].img == img) return c[i].v;
    VkImageViewCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = img, .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM,
                                .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    VkImageView v; if (vkCreateImageView(V.dev, &ci, NULL, &v)) return VK_NULL_HANDLE;
    if (n == 8) { n = 0; }   /* images are made once; a size change makes new ones (the old views leak, a few KB) */
    c[n].img = img; c[n].v = v; n++;
    return v;
#undef c
#undef n
}
static void hud_mask_run(VkCommandBuffer cb, VkImage gen, uint32_t w, uint32_t h) {
    VkDescriptorSet set = vk_alloc_set(g_hm_dsl);
    VkDescriptorImageInfo ii[3] = {{VK_NULL_HANDLE, hud_view(g_hs.img), VK_IMAGE_LAYOUT_GENERAL}, {VK_NULL_HANDLE, hud_view(g_hf.img), VK_IMAGE_LAYOUT_GENERAL}, {VK_NULL_HANDLE, hud_view(gen), VK_IMAGE_LAYOUT_GENERAL}};
    VkWriteDescriptorSet wr = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = 0, .descriptorCount = 3, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .pImageInfo = ii};
    vkUpdateDescriptorSets(V.dev, 1, &wr, 0, NULL);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, g_hm_pipe);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, g_hm_layout, 0, 1, &set, 0, NULL);
    vkCmdDispatch(cb, (w + 7) / 8, (h + 7) / 8, 1);
    vk_barrier_all(cb);
    g_bound_pipe = VK_NULL_HANDLE; g_bound_set[1][0].sub = 0;   /* the backend's own compute state was replaced */
}
static const void *g_scene_mem;   /* this frame's scene composite target (its storage), from the composite pass to the present */
static int g_hud_final;   /* the final picture of this frame was taken (at the scale pass) */
static void hud_mark(vk_shader **sh) {
    static const char *skey, *okey; static size_t skl, okl;
    if (!skey) { skey = getenv("TOTK_LSFG_SCENE_FS"); if (!skey || !*skey) skey = "0ac9a54938974396"; skl = strlen(skey);
                okey = getenv("TOTK_LSFG_OUT_FS"); if (!okey || !*okey) okey = "dd3d1e0ebf732428"; okl = strlen(okey); }
    /* the final picture: when the game's scale pass starts reading the finished HUD image into the output (a submission
       can hold the start of the next frame too, so by the present the HUD image may already hold some of it; 2026-10-23).
       The same shader blurs the minimap into small targets: only an output at least as large as the HUD image counts. */
    if (g_hud_x && !g_hud_final && sh[1] && !strncmp(sh[1]->key, okey, okl) && S.has_rt && S.rt.n >= 1 && S.rt.color[0] && S.rt.color[0]->d.width >= vk_tex_width(g_hud_x, 0)) {
        VkCommandBuffer cb = vk_cb();
        vk_barrier_all(cb); hud_blit(cb, g_hud_x, g_hf.img); vk_barrier_all(cb);
        g_hud_final = 1; return;
    }
    if (g_hud_x || !g_hs.img || !g_hud_rect[2] || !sh[1] || !S.has_rt || S.rt.n < 1 || !S.rt.color[0] || !hud_on()) return;
    /* only what is drawn onto the image the scene composite (TOTK_LSFG_SCENE_FS) finished this frame: menus and the title
       draw the HUD shader onto other targets (2026-10-23: the scene copy was empty there and generated frames black).
       The scene is copied at the first pass of any shader on that image after the composite, not only the HUD shader's
       (user, 2026-10-23: the stamina wheel was still interpolated: it comes before the HUD shader's first pass) */
    if (!strncmp(sh[1]->key, skey, skl)) { g_scene_mem = S.rt.color[0]->mem; return; }
    if (!g_scene_mem || S.rt.color[0]->mem != g_scene_mem) return;
    g_hud_x = vk_tex_get(S.rt.color[0]); if (!g_hud_x) return;
    VkCommandBuffer cb = vk_cb();
    vk_barrier_all(cb); hud_blit(cb, g_hud_x, g_hs.img); vk_barrier_all(cb);
}
static void present_window(const tex_t *t, const int crop[4]) {
    static int started; if (!started) { started = 1; pthread_t th; if (!pthread_create(&th, NULL, presenter, NULL)) pthread_detach(th); }
    pthread_mutex_lock(&g_win_mu); int have = g_win != NULL; pthread_mutex_unlock(&g_win_mu);
    if (!have || !t) return;
    int i = -1; for (int k = 0; k < NPRES; k++) if (!atomic_load(&g_pres[k].busy)) { i = k; break; }
    if (i < 0) { g_pres_dropped++; return; }
    vk_tex *x = vk_tex_get(t); if (!x) return;
    VkFormat f = vk_tex_format(x);
    int srgb = f == VK_FORMAT_R8G8B8A8_SRGB || f == VK_FORMAT_B8G8R8A8_SRGB || f == VK_FORMAT_A8B8G8R8_SRGB_PACK32;
    uint32_t w = vk_tex_width(x, 0), h = vk_tex_height(x, 0); int32_t cx = 0, cy = 0;
    if (crop && crop[2] > 0 && crop[3] > 0 && crop[0] >= 0 && crop[1] >= 0 && (uint32_t)(crop[0] + crop[2]) <= w && (uint32_t)(crop[1] + crop[3]) <= h) { cx = crop[0]; cy = crop[1]; w = (uint32_t)crop[2]; h = (uint32_t)crop[3]; }
    /* The window buffers keep one size, the window's: the game's crop changes with its dynamic resolution, and resizing the
       window's buffers each time showed black frames. The GPU scales the crop into a window-sized image (aspect kept,
       black bars where it differs) in the window's byte order, which is then read back. */
    /* 1280 wide (the game's handheld resolution; TOTK_APP_WIDTH changes it), as tall as the window's shape makes it; the
       compositor scales that to the screen for free, and the CPU copies less than at the screen's own size */
    static uint32_t want_w; if (!want_w) { const char *e = getenv("TOTK_APP_WIDTH"); want_w = e && atoi(e) >= 16 ? (uint32_t)atoi(e) : 1280; }
    uint32_t ow = want_w, oh = g_win_pw && g_win_ph ? (uint32_t)((uint64_t)want_w * g_win_ph / g_win_pw) & ~1u : want_w * 9 / 16;
    VkFormat of = srgb ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;
    if (!pres_image(ow, oh, of)) return;
    pres_buf *b = &g_pres[i];
    if (!pres_alloc(b, (VkDeviceSize)ow * oh * 4)) { RED(2, "presentation buffer (%ux%u) could not be made", ow, oh); return; }
    double sc = fmin((double)ow / w, (double)oh / h); int32_t dw = (int32_t)(w * sc + 0.5), dh = (int32_t)(h * sc + 0.5), dx = ((int32_t)ow - dw) / 2, dy = ((int32_t)oh - dh) / 2;
    vk_ub_flush(); vk_end_rendering();
    VkCommandBuffer cb = vk_cb(); vk_barrier_all(cb);
    if (dw < (int32_t)ow || dh < (int32_t)oh) {
        VkClearColorValue black = {{0, 0, 0, 1}}; VkImageSubresourceRange rr = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdClearColorImage(cb, g_pimg, VK_IMAGE_LAYOUT_GENERAL, &black, 1, &rr); vk_barrier_all(cb);
    }
    VkImageBlit bl = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .srcOffsets = {{cx, cy, 0}, {cx + (int32_t)w, cy + (int32_t)h, 1}},
                      .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .dstOffsets = {{dx, dy, 0}, {dx + dw, dy + dh, 1}}};
    vkCmdBlitImage(cb, vk_tex_image(x), VK_IMAGE_LAYOUT_GENERAL, g_pimg, VK_IMAGE_LAYOUT_GENERAL, 1, &bl, VK_FILTER_LINEAR);
    vk_barrier_all(cb);
    VkBufferImageCopy r = {.bufferOffset = 0, .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {ow, oh, 1}};
    vkCmdCopyImageToBuffer(cb, g_pimg, VK_IMAGE_LAYOUT_GENERAL, b->buf, 1, &r);
    vk_barrier_all(cb);
    g_hud_rect[0] = dx; g_hud_rect[1] = dy; g_hud_rect[2] = dw; g_hud_rect[3] = dh;
    VkImage fg_src = g_pimg; int masked = 0;   /* the HUD apart (hud_mark): lsfg-vk gets the scene; the final picture is kept for the mask */
    if (hud_on() && of == VK_FORMAT_R8G8B8A8_UNORM && hud_pipeline() && win_image(&g_hstage, ow, oh, VK_FORMAT_R8G8B8A8_SRGB, "HUD staging")
        && win_image_u(&g_hs, ow, oh, of, VK_IMAGE_USAGE_STORAGE_BIT, "HUD scene") && win_image_u(&g_hf, ow, oh, of, VK_IMAGE_USAGE_STORAGE_BIT, "HUD final")
        && g_hud_x && g_hud_final) { fg_src = g_hs.img; masked = 1; }   /* scene and final both taken this frame (hud_mark) */
    g_hud_x = NULL; g_scene_mem = NULL; g_hud_final = 0;
    int ng = vk_fg_begin(cb, fg_src, ow, oh, of);   /* frame generation (vk_fg.c): this frame becomes lsfg-vk's newest input */
    b->w = ow; b->h = oh; b->swap = 0; b->gen = 0; b->masked = 0; atomic_store(&b->busy, 1);   /* masked: a reused buffer may have held a generated picture */
    { uint64_t skipped = 0; for (int k = 0; k < 16; k++) skipped += g_stat_skips[k];
      b->ctr[0] = g_stat_draws; b->ctr[1] = g_stat_passes; b->ctr[2] = skipped; vk_res_counters(b->ctr + 3); b->frame = g_present_n; b->t_call = g_next_call; }
    b->fence = vk_submit_now();
    int order[4], no = 0;
    if (ng && vk_fg_dispatch()) {   /* the generated frame(s) between the last real one and this: shown first */
        static uint64_t last_real; uint64_t now = host_now_ns();
        if (last_real && now - last_real < 200000000ull) { uint64_t iv = (now - last_real) / (uint64_t)(ng + 1), sp = atomic_load(&g_fg_spacing); atomic_store(&g_fg_spacing, sp ? (sp * 7 + iv) / 8 : iv); }
        last_real = now;
        for (int g = 0; g < ng && g < 3; g++) {
            int j = -1; for (int k = 0; k < NPRES; k++) if (!atomic_load(&g_pres[k].busy)) { j = k; break; }
            if (j < 0 || !pres_alloc(&g_pres[j], (VkDeviceSize)ow * oh * 4)) { g_pres_dropped++; break; }
            pres_buf *gb = &g_pres[j];
            VkCommandBuffer c2 = vk_cb(); VkImage gi = vk_fg_output(c2, g);
            gb->masked = masked && host_buf(&gb->xbuf, &gb->xmem, &gb->xptr, &gb->xsize, &gb->xcoherent, 49 * 4);
            if (gb->masked) {   /* the HUD from the final picture (on the GPU), and 49 of its pixels for the presenter's check */
                hud_mask_run(c2, gi, ow, oh);
                VkBufferImageCopy pts[49]; int k = 0;
                for (uint32_t gy = 1; gy < 8; gy++) for (uint32_t gx = 1; gx < 8; gx++, k++)
                    pts[k] = (VkBufferImageCopy){.bufferOffset = (VkDeviceSize)k * 4, .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                                                 .imageOffset = {(int32_t)(ow * gx / 8), (int32_t)(oh * gy / 8), 0}, .imageExtent = {1, 1, 1}};
                vkCmdCopyImageToBuffer(c2, g_hf.img, VK_IMAGE_LAYOUT_GENERAL, gb->xbuf, 49, pts);
            }
            vkCmdCopyImageToBuffer(c2, gi, VK_IMAGE_LAYOUT_GENERAL, gb->buf, 1, &r);
            vk_barrier_all(c2);
            gb->real = i;
            gb->w = ow; gb->h = oh; gb->swap = 0; gb->gen = 1 + g; gb->frame = b->frame; gb->t_call = b->t_call; memcpy(gb->ctr, b->ctr, sizeof gb->ctr); atomic_store(&gb->busy, 1);
            gb->fence = vk_submit_now();
            order[no++] = j;
        }
    }
    order[no++] = i;
    pthread_mutex_lock(&g_win_mu); for (int k = 0; k < no; k++) g_pres_q[g_pres_qt++ % (NPRES * 2)] = order[k]; pthread_cond_signal(&g_pres_cv); pthread_mutex_unlock(&g_win_mu);
}
#endif
/* ---------- perf summary (TOTK, 2026-10-23: frame pacing work) ----------
   Every 300 frames: frame-time percentiles (the game's present to present), CPU per thread (the top ones, from
   /proc/self/task: which thread sets the frame), the time guest threads spent waiting for the GPU (and how much of it
   while holding one of the 3 virtual cores), and the time they spent queued for a core. */
#include <dirent.h>
#include <unistd.h>
static int u64cmp(const void *a, const void *b) { uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b; return x < y ? -1 : x > y; }
static void pctl(uint64_t *v, int n, double out[4]) {   /* p50 p90 p99 max, ms */
    if (n <= 0) { out[0] = out[1] = out[2] = out[3] = 0; return; }
    qsort(v, (size_t)n, sizeof *v, u64cmp);
    out[0] = v[n / 2] / 1e6; out[1] = v[(n * 9) / 10] / 1e6; out[2] = v[(n * 99) / 100 < n ? (n * 99) / 100 : n - 1] / 1e6; out[3] = v[n - 1] / 1e6;
}
static void thread_cpu_report(uint64_t frame, double frames) {
    enum { TMAX = 768 };
    typedef struct { int tid; uint64_t t, d; char name[16]; } trow;
    static trow prev[TMAX]; static int nprev;
    static trow cur[TMAX]; int nc = 0;
    DIR *dir = opendir("/proc/self/task"); if (!dir) return;
    struct dirent *e;
    while ((e = readdir(dir)) && nc < TMAX) {
        int tid = atoi(e->d_name); if (tid <= 0) continue;
        char path[64], buf[600]; snprintf(path, sizeof path, "/proc/self/task/%d/stat", tid);
        FILE *f = fopen(path, "r"); if (!f) continue;
        size_t k = fread(buf, 1, sizeof buf - 1, f); fclose(f); buf[k] = 0;
        char *l = strchr(buf, '('), *r = strrchr(buf, ')'); if (!l || !r || r < l) continue;
        char *q = r + 2; int field = 3; while (*q && field < 14) { if (*q == ' ') field++; q++; }   /* utime is field 14, stime 15 */
        unsigned long long ut = 0, st = 0; if (sscanf(q, "%llu %llu", &ut, &st) != 2) continue;
        trow *c = &cur[nc++]; c->tid = tid; c->t = ut + st; c->d = 0;
        size_t nl = (size_t)(r - l - 1); if (nl > 15) nl = 15; memcpy(c->name, l + 1, nl); c->name[nl] = 0;
        for (int i = 0; i < nprev; i++) if (prev[i].tid == tid) { c->d = c->t >= prev[i].t ? c->t - prev[i].t : 0; break; }
    }
    closedir(dir);
    int had = nprev; memcpy(prev, cur, (size_t)nc * sizeof *cur); nprev = nc;
    if (!had || frames <= 0) return;
    /* top 12 by CPU this window */
    trow top[12]; int nt = 0; uint64_t total = 0;
    for (int i = 0; i < nc; i++) {
        total += cur[i].d;
        int at = nt; while (at > 0 && top[at - 1].d < cur[i].d) at--;
        if (at < 12) { int last = nt < 12 ? nt : 11; for (int j = last; j > at; j--) top[j] = top[j - 1]; top[at] = cur[i]; if (nt < 12) nt++; }
    }
    double tick_ms = 1000.0 / (double)sysconf(_SC_CLK_TCK);
    char line[1400]; int o = snprintf(line, sizeof line, "frame %llu: CPU per frame %.1f ms over %d threads; top:", (unsigned long long)frame, total * tick_ms / frames, nc);
    for (int i = 0; i < nt && o < (int)sizeof line - 40; i++) o += snprintf(line + o, sizeof line - (size_t)o, " %s/%d %.1f", top[i].name, top[i].tid, top[i].d * tick_ms / frames);
    LOGV("%s", line);
}
static uint64_t g_ft[300]; static int g_ftn;   /* this window's frame times */
static void perf_summary(uint64_t n) {
    double fp[4]; pctl(g_ft, g_ftn, fp);
    int over34 = 0, over50 = 0; for (int i = 0; i < g_ftn; i++) { if (g_ft[i] > 34000000ull) over34++; if (g_ft[i] > 50000000ull) over50++; }
    uint64_t vk_guest_wait_take(uint64_t *n, uint64_t *core_ns); extern uint64_t runtime_core_wait_take(uint64_t *count) __attribute__((weak));
    uint64_t gwn, gwc, gw = vk_guest_wait_take(&gwn, &gwc), cwn = 0, cw = runtime_core_wait_take ? runtime_core_wait_take(&cwn) : 0;
    extern uint64_t runtime_core_grant_take(uint64_t *held) __attribute__((weak)); uint64_t held = 0, cg = runtime_core_grant_take ? runtime_core_grant_take(&held) : 0;
    double fr = g_ftn > 0 ? g_ftn : 1;
    LOGV("frame %llu: frame time p50 %.1f p90 %.1f p99 %.1f max %.1f ms, %d of %d over 34 ms, %d over 50; per frame: guest GPU waits %.1f ms (%.1f waits, %.1f ms of it holding a core), guest core queueing %.1f ms (%.1f waits, %.2f ms of it a granted core idle until its thread ran), virtual cores held %.1f ms of 3 x %.1f",
         (unsigned long long)n, fp[0], fp[1], fp[2], fp[3], over34, g_ftn, over50, gw / 1e6 / fr, gwn / fr, gwc / 1e6 / fr, cw / 1e6 / fr, cwn / fr, cg / 1e6 / fr, held / 1e6 / fr, fp[0]);
    thread_cpu_report(n, fr);
    { extern void runtime_core_wait_report(double frames) __attribute__((weak)); if (runtime_core_wait_report) runtime_core_wait_report(fr); }
    g_ftn = 0;
}
uint64_t mtl_present(const tex_t *t, double period, const int crop[4]) {
    (void)period;
    uint64_t now = host_now_ns(), n = ++g_present_n;
    /* ---------- hitch report (TOTK, 2026-10-22) ----------
       The time between two presents is the game's frame. Above 40 ms (and 2.5x the recent median) a line logs what
       the backend did in that window: anything at all points at the backend, all zeros at the game's own code
       (streaming, allocation). Every 300 frames a histogram line summarises them. */
    static uint64_t last_present;
    uint64_t dt_hk = last_present ? now - last_present : 0;
    last_present = now;
    static uint64_t ht[5], ht_max, ht_n; static double ht_sum; static int ht_logged;
    if (dt_hk) {
        static const uint32_t ht_ms[5] = {40, 60, 100, 200, 400};
        if (dt_hk >= ht_ms[0] * 1000000ull) {
            for (int i = 4; i >= 0; i--) if (dt_hk >= ht_ms[i] * 1000000ull) { ht[i]++; break; }
            if (dt_hk > ht_max) ht_max = dt_hk;
        }
        ht_n++; ht_sum += dt_hk / 1e6;
        if (g_ftn < 300) g_ft[g_ftn++] = dt_hk;
    }
    static double med;
    {   /* median of the last 64 frame times (including this one) */
        static uint64_t w[64]; static int wn, wi;
        if (wn < 64) w[wn++] = dt_hk; else { w[wi] = dt_hk; wi = (wi + 1) & 63; }
        uint64_t s[64]; memcpy(s, w, (size_t)wn * sizeof *s);
        for (int i = 1; i < wn; i++) for (int j = i; j > 0 && s[j - 1] > s[j]; j--) { uint64_t t = s[j]; s[j] = s[j - 1]; s[j - 1] = t; }
        med = (wn & 1 ? s[wn / 2] : (s[wn / 2 - 1] + s[wn / 2]) / 2) / 1e6;
    }
    if (dt_hk >= 40000000ull && dt_hk / 1e6 >= 2.5 * med && ht_logged < 24) {   /* at most 24 detail lines a window */
        ht_logged++;
            uint64_t replay = atomic_exchange_explicit(&g_hk.replay_ns, 0, memory_order_relaxed) / 1000,
                     wait = atomic_exchange_explicit(&g_hk.wait_ns, 0, memory_order_relaxed) / 1000,
                     subs = atomic_exchange_explicit(&g_hk.subs, 0, memory_order_relaxed),
                     pipes = atomic_exchange_explicit(&g_hk.pipes, 0, memory_order_relaxed),
                     pns = atomic_exchange_explicit(&g_hk.pipe_ns, 0, memory_order_relaxed),
                     sh = atomic_exchange_explicit(&g_hk.shloads, 0, memory_order_relaxed),
                     shb = atomic_exchange_explicit(&g_hk.shbytes, 0, memory_order_relaxed),
                     vl = atomic_exchange_explicit(&g_hk.vlists, 0, memory_order_relaxed),
                     rq = atomic_exchange_explicit(&g_hk.sreq, 0, memory_order_relaxed),
                     dp = atomic_exchange_explicit(&g_hk.dpools, 0, memory_order_relaxed),
                     ch = atomic_exchange_explicit(&g_hk.chunks, 0, memory_order_relaxed),
                     cs = atomic_exchange_explicit(&g_hk.cpu_steps, 0, memory_order_relaxed),
                     id = atomic_exchange_explicit(&g_hk.idles, 0, memory_order_relaxed),
                     ins = atomic_exchange_explicit(&g_hk.idle_ns, 0, memory_order_relaxed),
                     iv = atomic_exchange_explicit(&g_hk.inval, 0, memory_order_relaxed),
                     ivns = atomic_exchange_explicit(&g_hk.inval_ns, 0, memory_order_relaxed),
                     wb = atomic_exchange_explicit(&g_hk.wb, 0, memory_order_relaxed),
                     wbb = atomic_exchange_explicit(&g_hk.wb_bytes, 0, memory_order_relaxed),
                     ps = atomic_exchange_explicit(&g_hk.pipes_sync, 0, memory_order_relaxed),
                     pf = atomic_exchange_explicit(&g_hk.pipes_fail, 0, memory_order_relaxed),
                     fin = atomic_exchange_explicit(&g_hk.idle_fin, 0, memory_order_relaxed),
                     fl = atomic_exchange_explicit(&g_hk.idle_flush, 0, memory_order_relaxed),
                     pl = atomic_exchange_explicit(&g_hk.idle_pool, 0, memory_order_relaxed),
                     sb = atomic_exchange_explicit(&g_hk.shbias, 0, memory_order_relaxed),
                     gw = atomic_exchange_explicit(&g_hk.gwaits, 0, memory_order_relaxed),
                     gwn = atomic_exchange_explicit(&g_hk.gwait_ns, 0, memory_order_relaxed),
                     cn = atomic_exchange_explicit(&g_hk.comp_ns, 0, memory_order_relaxed),
                     wk = atomic_exchange_explicit(&g_hk.wake_ns, 0, memory_order_relaxed);
            LOGV("hitch at frame %llu: +%.1f ms (median %.1f): replay %.1f ms in %llu submits (GPU wait %.1f ms); pipelines %llu (%.0f ms, %llu sync-built, %llu failed), shader loads %llu (%llu gather-biased, %.0f KiB), variant lists %llu, requests written %llu, descriptor pools %llu, chunks %llu, cpu steps %llu, gpu idles %llu (%.0f ms, %llu queue finishes, %llu flush waits, %llu pool drops), invalidates %llu (%.0f ms, %llu write-backs, %.1f MiB), game waits %llu (%.0f ms, of it completion %.1f ms + wake %.1f ms)",
                 (unsigned long long)n, dt_hk / 1e6, med, replay / 1e6, (unsigned long long)subs, wait / 1e6,
                 (unsigned long long)pipes, pns / 1e6, (unsigned long long)ps, (unsigned long long)pf, (unsigned long long)sh, (unsigned long long)sb, shb / 1024.0, (unsigned long long)vl, (unsigned long long)rq,
                 (unsigned long long)dp, (unsigned long long)ch, (unsigned long long)cs, (unsigned long long)id, ins / 1e6,
                 (unsigned long long)fin, (unsigned long long)fl, (unsigned long long)pl,
                 (unsigned long long)iv, ivns / 1e6, (unsigned long long)wb, wbb / 1048576.0, (unsigned long long)gw, gwn / 1e6, cn / 1e6, wk / 1e6);
    }
    if (n % 300 == 0 && ht_n > 1) {
        LOGV("hitches, last 300 frames: 40-60 ms %llu, 60-100 %llu, 100-200 %llu, 200-400 %llu, >400 %llu; worst %.1f ms, median %.1f ms, mean %.1f ms%s",
             (unsigned long long)ht[0], (unsigned long long)ht[1], (unsigned long long)ht[2], (unsigned long long)ht[3], (unsigned long long)ht[4],
             ht_max / 1e6, med, ht_sum / (double)ht_n, ht[0] + ht[1] + ht[2] + ht[3] + ht[4] > (uint64_t)ht_logged ? " (detail lines capped)" : "");
        LOGV("shader seed: %llu gather-biased shaders loaded so far (0 = the seed predates the bias)",
             (unsigned long long)vk_shbias_total());
        memset(ht, 0, sizeof ht); ht_max = 0; ht_n = 0; ht_sum = 0; ht_logged = 0;
    }
    if (n % 300 == 0) vk_pcache_save_async();
    probe_frame_end(n);
    pd_frame_end(n);
    if (g_fr_n) {
        qsort(g_fr, (size_t)g_fr_n, sizeof *g_fr, u32cmp);
        if (g_fr_n != g_prev_n || memcmp(g_fr, g_prev, (size_t)g_fr_n * sizeof *g_fr)) {
            uint64_t c = ++g_stat_rescg;
            if (g_prev_n && (c <= 64 || c % 100 == 0)) { char a[300], b[300]; sizes_str(g_prev, g_prev_n, a, sizeof a); sizes_str(g_fr, g_fr_n, b, sizeof b);
                LOGV("depth target sizes at frame %llu: %s (frame before: %s; %llu changes so far)", (unsigned long long)n, b, a, (unsigned long long)c); }
            memcpy(g_prev, g_fr, sizeof g_fr); g_prev_n = g_fr_n;
        }
    }
    g_fr_n = 0;
    g_pres_t[n % 64] = now / 1e9;
    if (!g_present_t0) g_present_t0 = now;
    static long every = -1; if (every < 0) { const char *e = getenv("TOTK_VK_SHOT"); every = e ? atol(e) : 0; if (every) LOGV("saving every %ld-th frame as PNG (TOTK_VK_SHOT)", every); }
    if (every > 0 && t && n % (uint64_t)every == 0) { vk_rec_lock(); shot(t, n, crop); vk_rec_unlock(); }
#ifdef TOTK_ANDROID_APP
    vk_rec_lock(); present_window(t, crop); vk_rec_unlock();
#endif
    if (n % 300 == 0) {
        LOGV("frame %llu: %.1f fps over the last 300, %llu draws, %llu passes (%llu split by barriers), %llu pipelines, %llu dispatches; skipped: no program %llu, disabled %llu, culled %llu, primitive %llu, shader missing %llu, scissor %llu, pipeline %llu; missing textures %llu, buffers outside the pools %llu",
             (unsigned long long)n, 300e9 / (double)(now - g_present_t0), (unsigned long long)g_stat_draws, (unsigned long long)g_stat_passes, (unsigned long long)g_stat_inpass_barriers, (unsigned long long)g_stat_pipes,
             (unsigned long long)g_stat_dispatch, (unsigned long long)g_stat_skips[0], (unsigned long long)g_stat_skips[1], (unsigned long long)g_stat_skips[2],
             (unsigned long long)g_stat_skips[3], (unsigned long long)g_stat_skips[4], (unsigned long long)g_stat_skips[6], (unsigned long long)g_stat_skips[7],
             (unsigned long long)g_stat_dummy_tex, (unsigned long long)g_stat_zero_buf);
        double rp, wt, gp; uint64_t ns; vk_perf_take(&rp, &wt, &gp, &ns);
        LOGV("frame %llu, per frame: recording %.1f ms, waiting for the GPU %.1f ms, GPU %.1f ms, %.1f submissions (%.1f splits, %.1f inside passes); descriptor sets %llu written, %llu reused",
             (unsigned long long)n, rp / 300, wt / 300, gp / 300, ns / 300.0, g_stat_splits / 300.0, g_stat_pass_splits / 300.0, (unsigned long long)g_stat_sets_new, (unsigned long long)g_stat_sets_reused);
        g_stat_splits = 0; g_stat_pass_splits = 0;
        g_stat_sets_new = g_stat_sets_reused = 0;
        perf_summary(n);
        if (g_stat_cmp_plain || g_stat_cmp_fmt || g_stat_cmp_sep)
            LOGV("frame %llu: compare samplers on plainly sampled textures %llu, on formats without depth compare %llu (lifetime binds), separate compare samplers %llu",
                 (unsigned long long)n, (unsigned long long)g_stat_cmp_plain, (unsigned long long)g_stat_cmp_fmt, (unsigned long long)g_stat_cmp_sep);
        { void vk_mem_report(void); vk_mem_report(); }
#ifdef TOTK_ANDROID_APP
        LOGV("window: %llu frames shown, %llu not shown (all buffers busy)", (unsigned long long)g_pres_shown, (unsigned long long)g_pres_dropped);
#endif
        g_present_t0 = now;
    }
    /* The ticket is the fence of the work this present follows (the frame's submissions and the window copy):
       acquiring this swapchain texture again waits for it (mtl_present_wait), as on the Mac. Without that the game ran
       several frames ahead of the GPU and rewrote per-frame data (lights, constants) the GPU had not read yet: the lit
       scene went dark for a frame now and then. */
    g_last_ticket = mtl_last_fence() ? mtl_last_fence() : 1;
    return g_last_ticket;
}
/* TOTK_VK_FRAMES_AHEAD=1: acquiring any swapchain texture waits for the latest present (the game at most one frame
   ahead of the GPU, as on the Switch where the GPU is rarely behind); default 2 (the texture's own last present). */
void mtl_present_wait(uint64_t tk) {
    static int off = -1, ahead = 2; if (off < 0) { off = vk_off("present_wait"); const char *e = getenv("TOTK_VK_FRAMES_AHEAD"); if (e && atoi(e) == 1) ahead = 1; }
    if (off) return;
    if (ahead == 1 && g_last_ticket > tk) tk = g_last_ticket;
    if (tk) mtl_wait(tk, 1000000000ull);
}
double mtl_presented_time(uint64_t n) { return n && n + 64 > g_present_n ? g_pres_t[n % 64] : 0; }

uint64_t vk_present_count(void) { return g_present_n; }   /* tests */
