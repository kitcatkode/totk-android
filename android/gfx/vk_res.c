/* Textures and samplers of the Vulkan backend (vk_int.h); the Metal counterpart is the texture half of nvn_mtl.m
   (tex_mtl, tex_by_id, samp_by_id, pkg_upload, stor_*), whose rules this follows:

   - A texture in a CPU-visible pool ("mirrored": tex_mirrored) is uploaded from the guest's bytes at its first use and
     uploaded again when the game wrote any of its pages since (nvn_dirty.c page protection), checked once per
     submission -- unless the GPU has written it (render target, image store, copy, clear): from then on the image is
     the content, as on Metal.
   - Every other texture is GPU-only. Texture objects that describe the same storage with the same layout share one
     image (storage-shared textures: the game samples through one serial what it renders through another, re-created
     every frame); a different format of the same texel size is a view of the shared image.
   - Images stay in VK_IMAGE_LAYOUT_GENERAL; ordering between passes, copies and dispatches is by full memory barriers
     (vk_barrier_all). Uploads are recorded into the submission's "pre" command buffer, ahead of the game's commands.
   - Unbound or empty texture slots read zero (null descriptors where the driver has them, else 1x1 zero images). */
#include "vk_int.h"
#include "nvn_dirty.h"
#include "nvn_hmap.h"
#include "../../third_party/d2r_gfx/gfx/tex/nvn_deswizzle.h"
#include <math.h>

#define MAXV(a, b) ((a) > (b) ? (a) : (b))
#define MINV(a, b) ((a) < (b) ? (a) : (b))

/* ---------- device memory: 64 MiB blocks per memory type, first-fit free lists; big images get their own ---------- */
#define BLOCK_SIZE (64ull << 20)
typedef struct { VkDeviceSize off, size; } span_t;
typedef struct { VkDeviceMemory mem; uint32_t type; span_t *free; int nfree, capfree; VkDeviceSize used; } block_t;
static block_t *g_blocks; static int g_nblocks;
static pthread_mutex_t g_mem_mu = PTHREAD_MUTEX_INITIALIZER;
static uint64_t g_mem_dedicated_bytes, g_mem_block_bytes;

static void block_free_insert(block_t *b, VkDeviceSize off, VkDeviceSize size) {
    int i = 0; while (i < b->nfree && b->free[i].off < off) i++;
    if (b->nfree == b->capfree) { b->capfree = b->capfree ? 2 * b->capfree : 16; b->free = realloc(b->free, (size_t)b->capfree * sizeof *b->free); }
    memmove(b->free + i + 1, b->free + i, (size_t)(b->nfree - i) * sizeof *b->free); b->free[i] = (span_t){off, size}; b->nfree++;
    if (i + 1 < b->nfree && b->free[i].off + b->free[i].size == b->free[i + 1].off) { b->free[i].size += b->free[i + 1].size; memmove(b->free + i + 1, b->free + i + 2, (size_t)(b->nfree - i - 2) * sizeof *b->free); b->nfree--; }
    if (i > 0 && b->free[i - 1].off + b->free[i - 1].size == b->free[i].off) { b->free[i - 1].size += b->free[i].size; memmove(b->free + i, b->free + i + 1, (size_t)(b->nfree - i - 1) * sizeof *b->free); b->nfree--; }
}
int vk_mem_alloc(const VkMemoryRequirements *req, VkMemoryPropertyFlags want, vk_alloc *out) {
    int type = vk_pick_type(req->memoryTypeBits, want);
    if (type < 0) type = vk_pick_type(req->memoryTypeBits, 0);
    if (type < 0) return 0;
    memset(out, 0, sizeof *out);
    VkDeviceSize align = req->alignment ? req->alignment : 1;
    if (align < V.props.limits.bufferImageGranularity) align = V.props.limits.bufferImageGranularity;
    if (req->size > BLOCK_SIZE / 4) {
        VkMemoryAllocateInfo mai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req->size, .memoryTypeIndex = (uint32_t)type};
        if (vkAllocateMemory(V.dev, &mai, NULL, &out->mem)) return 0;
        out->size = req->size; out->block = -1;
        pthread_mutex_lock(&g_mem_mu); g_mem_dedicated_bytes += req->size; pthread_mutex_unlock(&g_mem_mu);
        return 1;
    }
    pthread_mutex_lock(&g_mem_mu);
    for (int pass = 0; pass < 2; pass++) {
        for (int bi = 0; bi < g_nblocks; bi++) {
            block_t *b = &g_blocks[bi];
            if (b->type != (uint32_t)type || !b->mem) continue;
            for (int i = 0; i < b->nfree; i++) {
                VkDeviceSize at = (b->free[i].off + align - 1) / align * align, end = b->free[i].off + b->free[i].size;
                if (at + req->size > end) continue;
                span_t f = b->free[i];
                memmove(b->free + i, b->free + i + 1, (size_t)(b->nfree - i - 1) * sizeof *b->free); b->nfree--;
                if (at > f.off) block_free_insert(b, f.off, at - f.off);
                if (at + req->size < end) block_free_insert(b, at + req->size, end - at - req->size);
                b->used += req->size;
                out->mem = b->mem; out->off = at; out->size = req->size; out->block = bi;
                pthread_mutex_unlock(&g_mem_mu);
                return 1;
            }
        }
        if (pass) break;
        VkMemoryAllocateInfo mai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = BLOCK_SIZE, .memoryTypeIndex = (uint32_t)type};
        VkDeviceMemory m;
        if (vkAllocateMemory(V.dev, &mai, NULL, &m)) break;
        g_blocks = realloc(g_blocks, (size_t)(g_nblocks + 1) * sizeof *g_blocks);
        block_t *b = &g_blocks[g_nblocks++]; memset(b, 0, sizeof *b); b->mem = m; b->type = (uint32_t)type;
        block_free_insert(b, 0, BLOCK_SIZE);
        g_mem_block_bytes += BLOCK_SIZE;
    }
    pthread_mutex_unlock(&g_mem_mu);
    return 0;
}
void vk_mem_free(vk_alloc *a) {
    if (!a->mem) return;
    if (a->block < 0) { vkFreeMemory(V.dev, a->mem, NULL); pthread_mutex_lock(&g_mem_mu); g_mem_dedicated_bytes -= a->size; pthread_mutex_unlock(&g_mem_mu); }
    else { pthread_mutex_lock(&g_mem_mu); block_t *b = &g_blocks[a->block]; block_free_insert(b, a->off, a->size); b->used -= a->size; pthread_mutex_unlock(&g_mem_mu); }
    memset(a, 0, sizeof *a);
}

/* ---------- format support ---------- */
static VkFormatFeatureFlags fmt_features(VkFormat f) {
    static VkFormatFeatureFlags cache[256]; static uint8_t known[256];
    unsigned i = f < 185 ? (unsigned)f : f >= 1000340000 && f <= 1000340001 ? 185 + (unsigned)(f - 1000340000) : 255;
    if (i < 255 && known[i]) return cache[i];
    VkFormatProperties fp; V.get_format_props(V.pd, f, &fp);
    if (i < 255) { cache[i] = fp.optimalTilingFeatures; known[i] = 1; }
    return fp.optimalTilingFeatures;
}

/* ---------- images: shared by storage for GPU-only textures, one per serial for mirrored ones ---------- */
typedef struct { uint64_t mem, size; uint32_t w, h, d, levels, target, samples, bpb, depth, fcls; } stor_key;   /* fcls: compressible storage only */
typedef struct { uint64_t k0, k1; VkImageView v; } view_ent;
typedef struct vk_img {
    VkImage img; vk_alloc mem; VkFormat fmt; int nvn_fmt; VkImageUsageFlags usage; VkImageType type;
    uint32_t w, h, d, levels, layers, samples; VkImageAspectFlags aspect;
    int shared, refs; stor_key k; uint64_t idle_sub, born;   /* born: creation order (the sweep's "made later over the same memory") */
    int wbrefs, orphan;   /* unshared images: write-back records holding it; its texture object is gone (retired at the last record) */
    view_ent *views; int nviews, capviews;
    int gpu_written;
    int arr3d;      /* 3D, created 2D_ARRAY_COMPATIBLE: its slices can be render targets */
    int blk_rt;     /* block-compressed, created BLOCK_TEXEL_VIEW_COMPATIBLE | EXTENDED_USAGE with attachment usage */
    int listed; VkFormat twin;   /* created with a format list (fmtlist_on): views only in fmt or twin (0: none) */
} vk_img;
struct vk_tex {
    uint64_t serial; tex_t t; vk_img *im;
    int mirrored; uint64_t stamp, csub;   /* mirrored: dirty-page stamp of the uploaded content, last submission checked */
    int cpu_pool;                         /* tex_mirrored at creation: GPU writes to it are written back on invalidate (wb_note) */
    VkBufferView tbv;                     /* TARGET_BUFFER textures */
};
static nvn_hmap g_by_serial = {.klen = 8};          /* serial -> vk_tex */
static nvn_hmap g_by_storage = {.klen = sizeof(stor_key)};   /* stor_key -> vk_img (shared) */
static pthread_mutex_t g_tex_mu = PTHREAD_MUTEX_INITIALIZER;
static uint64_t g_stat_uploads, g_stat_reuploads, g_stat_upload_bytes, g_stat_images, g_stat_shared_hits, g_stat_swept, g_stat_published, g_stat_switched;
/* GPU-written CPU-pool textures shared by storage (TOTK_VK_OFF=mirshare: off). A texture in a CPU-visible pool mirrors the
   pool's bytes into its own image (one per serial). When the GPU renders into one, the pool bytes are stale; another
   serial at the same storage with the same layout (the game re-initialises render-target objects at the storage that
   a persistent sampling object describes, as nvn_mtl.m's storage sharing notes) would read its own stale upload: the
   deferred lighting then read empty G-buffer textures and drew lit geometry black. So a GPU-written mirrored image is
   published under its storage key, and every serial at that storage reads it from then on. */
static int mirshare_on(void) { static int on = -1; if (on < 0) on = !vk_off("mirshare"); return on; }

static void wb_age_locked(void);   /* GPU write-back records (below) */
/* retired objects: destroyed once the GPU is past the submission that last could use them */
typedef struct { VkImage img; vk_alloc mem; view_ent *views; int nviews; VkBufferView bv; void *host; uint64_t after; } retired_t;   /* host: a vk_img / vk_tex the replay thread may still hold */
static retired_t *g_retired; static int g_nretired, g_capretired;
static void retire(retired_t r) {
    if (g_nretired == g_capretired) { g_capretired = g_capretired ? 2 * g_capretired : 64; g_retired = realloc(g_retired, (size_t)g_capretired * sizeof *g_retired); }
    r.after = mtl_last_fence() + 1;   /* the submission being recorded now */
    g_retired[g_nretired++] = r;
}
/* The game finalises textures on its own threads while the replay thread may be recording with them, so the host
   objects go only when the GPU is past the submission being recorded (the replay thread drops them at each commit). */
static void retire_img(vk_img *im) { retire((retired_t){.img = im->img, .mem = im->mem, .views = im->views, .nviews = im->nviews, .host = im}); }
static void retire_bview(VkBufferView bv) { if (bv) retire((retired_t){.bv = bv}); }
static void retire_host(void *p) { if (p) retire((retired_t){.host = p}); }
void vk_res_sweep(void) {   /* nvn_vk.c, after each commit */
    pthread_mutex_lock(&g_tex_mu);
    for (int i = 0; i < g_nretired; ) {
        retired_t *r = &g_retired[i];
        if (!mtl_wait(r->after, 0)) { i++; continue; }
        for (int v = 0; v < r->nviews; v++) vkDestroyImageView(V.dev, r->views[v].v, NULL);
        free(r->views);
        if (r->img) vkDestroyImage(V.dev, r->img, NULL);
        vk_mem_free(&r->mem);
        if (r->bv) vkDestroyBufferView(V.dev, r->bv, NULL);
        free(r->host);
        g_retired[i] = g_retired[--g_nretired];
    }
    wb_age_locked();
    /* Shared images nobody holds. TOTK (2026-10-22): they were dropped after 600 idle submissions (~20 s at 30 fps), but
       such an image is the only copy of what the GPU rendered into that memory (on hardware the memory keeps it): the
       game re-creates texture objects over long-lived storage (baked terrain / LOD / impostor textures) and samples them
       again, and after ~20-30 s of standing still things popped out of view. Now an idle image goes only when another
       image has since been made over overlapping memory (its content is superseded there), or after a long time as a
       memory backstop (TOTK_VK_SWEEP_IDLE=<submissions>, default 36000 ~ 20 min; 0 = never). Checked every 300
       submissions. */
    uint64_t sub = vk_submit_serial();
    static uint64_t last_sweep; static long idle_max = -1;
    if (idle_max < 0) { const char *e = getenv("TOTK_VK_SWEEP_IDLE"); idle_max = e && *e ? atol(e) : 36000; }
    /* Off by default (TOTK, 2026-10-22 night): even the superseded rule lost content the game still samples - the
       terrain's moss and grass detail degraded patch by patch over minutes while standing still, and stopped with
       TOTK_VK_OFF=sweep. Shared images now go only with their pool (vk_tex_pool_gone); TOTK_VK_SWEEP=1 turns the
       idle sweep back on. */
    static int sweep_on = -1; if (sweep_on < 0) { const char *e = getenv("TOTK_VK_SWEEP"); sweep_on = e && atoi(e) > 0 && !vk_off("sweep"); }
    if (sub - last_sweep >= 300 && sweep_on) {
        last_sweep = sub;
        for (uint32_t i = 0; i < g_by_storage.cap; i++) {
            vk_img *im = g_by_storage.vals ? g_by_storage.vals[i] : NULL;
            if (!im || im->refs || sub - im->idle_sub < 600) continue;
            int superseded = 0;
            uint64_t lo = im->k.mem, hi = im->k.mem + im->k.size;
            for (uint32_t j = 0; j < g_by_storage.cap && !superseded; j++) {
                vk_img *o = g_by_storage.vals ? g_by_storage.vals[j] : NULL;
                if (o && o != im && o->born > im->born && o->k.mem < hi && lo < o->k.mem + o->k.size) superseded = 1;
            }
            if (!superseded && !(idle_max > 0 && sub - im->idle_sub >= (uint64_t)idle_max)) continue;
            stor_key k = im->k; nvn_hm_del(&g_by_storage, &k); retire_img(im); i--; g_stat_swept++;   /* deletion shifts entries back */
        }
    }
    pthread_mutex_unlock(&g_tex_mu);
}

/* NVN compression (nvn_mtl.m stor_compressed, the 2026-10-15 frame-wide flashes): a compressible texture in a compressible
   pool keeps its contents compressed, and a clear only sets the compression state, so on hardware a descriptor reading
   the same storage in another format does not see such writes. When the game moves the linear depth to new memory, the
   deferred lighting keeps reading the old depth descriptor for a frame or two while the old memory already belongs to an
   RGBA8 target cleared to white: with one shared image every depth texel reads 0xFFFFFFFF = NaN and the lit geometry goes
   black for that frame. So compressible textures share an image only with the same format or its sRGB / linear twin;
   each format class keeps its own image (and content) at that storage. TOTK_VK_OFF=compalias shares across formats. */
static int compalias_on(void) { static int on = -1; if (on < 0) on = !vk_off("compalias"); return on; }
/* TOTK_VK_FMT_SPLIT=1 (diagnostic): no storage sharing across format classes at all, compressible or not. On hardware
   plain memory aliases across formats, so this can break intended reinterpretation; it tells whether any cross-format
   sharing is behind a picture problem. */
static int fmtsplit_on(void) { static int on = -1; if (on < 0) { const char *e = getenv("TOTK_VK_FMT_SPLIT"); on = e && atoi(e); if (on) LOGV("TOTK_VK_FMT_SPLIT: textures share storage only within a format class"); } return on; }
static int stor_compressed(const tex_t *t) { return (t->d.flags & NVN_TEXTURE_FLAGS_COMPRESSIBLE) && t->pool && (t->pool->flags & NVN_MEMORY_POOL_FLAGS_COMPRESSIBLE); }
static uint32_t fmt_class(int nvn_fmt) {
    VkFormat f; VkComponentMapping sw;
    if (!vk_fmt(nvn_fmt, &f, &sw) || !f) return 0x80000000u | (uint32_t)nvn_fmt;
    switch (f) {
        case VK_FORMAT_R8G8B8A8_SRGB: f = VK_FORMAT_R8G8B8A8_UNORM; break;
        case VK_FORMAT_B8G8R8A8_SRGB: f = VK_FORMAT_B8G8R8A8_UNORM; break;
        case VK_FORMAT_A8B8G8R8_SRGB_PACK32: f = VK_FORMAT_A8B8G8R8_UNORM_PACK32; break;
        case VK_FORMAT_R8_SRGB: f = VK_FORMAT_R8_UNORM; break;
        case VK_FORMAT_R8G8_SRGB: f = VK_FORMAT_R8G8_UNORM; break;
        default: break;
    }
    return (uint32_t)f + 1;
}
static uint64_t g_stat_compsplit, g_stat_xfmt;   /* compressible storage taken by another format class */
static nvn_hmap g_comp_owner = {.klen = sizeof(stor_key)};   /* key without fcls -> the last format class (as a pointer) */
static int stor_key_of(const tex_t *t, stor_key *k) {
    if (!t->mem || !t->L.size) return 0;
    int bw = 1, bh = 1, bpb = nvn_fmt_bpb(t->d.format, &bw, &bh);
    if (!bpb || bw != 1 || bh != 1) return 0;   /* block-compressed formats keep one image per serial, as on Metal */
    memset(k, 0, sizeof *k);
    k->mem = (uint64_t)(uintptr_t)t->mem; k->size = t->L.size; k->w = t->d.width ? t->d.width : 1; k->h = t->d.height ? t->d.height : 1;
    k->d = t->d.depth ? t->d.depth : 1; k->levels = t->d.levels ? t->d.levels : 1; k->target = (uint32_t)t->d.target;
    k->samples = t->d.samples > 1 ? (uint32_t)t->d.samples : 1; k->bpb = (uint32_t)bpb; k->depth = (uint32_t)vk_fmt_is_depth(t->d.format) | (uint32_t)vk_fmt_has_stencil(t->d.format) << 1;
    if ((compalias_on() && stor_compressed(t)) || fmtsplit_on()) k->fcls = fmt_class(t->d.format);
    return 1;
}
static int tex_mirrored(const tex_t *t) {   /* nvn_mtl.m tex_mirrored */
    if (!t || !t->mem) return 0;
    if (t->has_layout) return 1;
    const pool_t *p = t->pool;
    if (!p || p->host || !(p->flags & (NVN_MEMORY_POOL_FLAGS_CPU_UNCACHED | NVN_MEMORY_POOL_FLAGS_CPU_CACHED))) return 0;
    if (t->d.flags & (NVN_TEXTURE_FLAGS_DISPLAY | NVN_TEXTURE_FLAGS_COMPRESSIBLE | NVN_TEXTURE_FLAGS_ADAPTIVE_ZCULL |
                      NVN_TEXTURE_FLAGS_DEPTH16_PREFER_FAST_CLEAR | NVN_TEXTURE_FLAGS_ZCULL_SUPPORT_STENCIL)) return 0;
    if (t->d.samples > 1 || t->d.target == 10) return 0;
    if (t->d.format >= 0x31 && t->d.format <= 0x36) return 0;
    return 1;
}
static void image_layers(const tex_t *t, VkImageType *type, uint32_t *w, uint32_t *h, uint32_t *d, uint32_t *layers, uint32_t *samples) {
    *w = t->d.width ? t->d.width : 1; *h = t->d.height ? t->d.height : 1; *d = 1; *layers = 1; *samples = 1; *type = VK_IMAGE_TYPE_2D;
    switch (t->d.target) {
        case 0: *type = VK_IMAGE_TYPE_1D; *h = 1; break;
        case 2: *type = VK_IMAGE_TYPE_3D; *d = t->d.depth ? t->d.depth : 1; break;
        case 3: *type = VK_IMAGE_TYPE_1D; *layers = *h; *h = 1; break;
        case 4: *layers = t->d.depth ? t->d.depth : 1; break;
        case 5: *samples = t->d.samples > 1 ? (uint32_t)t->d.samples : 1; break;
        case 6: *samples = t->d.samples > 1 ? (uint32_t)t->d.samples : 1; *layers = t->d.depth ? t->d.depth : 1; break;
        case 8: *layers = 6; break;
        case 9: *layers = 6 * (t->d.depth ? t->d.depth : 1); break;
    }
    if (*w > 16384) *w = 16384; if (*h > 16384) *h = 16384; if (*d > 2048) *d = 2048; if (*layers > 2048) *layers = 2048;
}
static void image_barrier_init(VkCommandBuffer cb, VkImage img, VkImageAspectFlags aspect) {
    VkImageMemoryBarrier2 b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2, .srcStageMask = VK_PIPELINE_STAGE_2_NONE, .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
        .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT, .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_GENERAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = img,
        .subresourceRange = {aspect, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS}};
    VkDependencyInfo di = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &b};
    vkCmdPipelineBarrier2(cb, &di);
}
/* UBWC (2026-10-25): Turnip turns Adreno framebuffer compression off for a MUTABLE_FORMAT image without a format list
   (TU_DEBUG=perf: "Disabling UBWC ... mutable formats (fmt list missing)"), which was every colour image. TOTK views
   uncompressed textures in another format only as their sRGB / UNORM twin (a 3-minute survey at Lookout Landing), so
   such images list their format and its twin, or are not MUTABLE when there is none. A view in any other format
   marks that NVN format: its later images are created fully MUTABLE again (the view itself is refused once).
   TOTK_VK_OFF=fmtlist: every uncompressed colour image MUTABLE without a list (the old rule). */
static int fmtlist_on(void) { static int on = -1; if (on < 0) on = !vk_off("fmtlist"); return on; }
static VkFormat srgb_twin(VkFormat f) {
    switch (f) {
        case VK_FORMAT_R8G8B8A8_UNORM: return VK_FORMAT_R8G8B8A8_SRGB;  case VK_FORMAT_R8G8B8A8_SRGB: return VK_FORMAT_R8G8B8A8_UNORM;
        case VK_FORMAT_B8G8R8A8_UNORM: return VK_FORMAT_B8G8R8A8_SRGB;  case VK_FORMAT_B8G8R8A8_SRGB: return VK_FORMAT_B8G8R8A8_UNORM;
        case VK_FORMAT_A8B8G8R8_UNORM_PACK32: return VK_FORMAT_A8B8G8R8_SRGB_PACK32;  case VK_FORMAT_A8B8G8R8_SRGB_PACK32: return VK_FORMAT_A8B8G8R8_UNORM_PACK32;
        case VK_FORMAT_R8_UNORM: return VK_FORMAT_R8_SRGB;  case VK_FORMAT_R8_SRGB: return VK_FORMAT_R8_UNORM;
        case VK_FORMAT_R8G8_UNORM: return VK_FORMAT_R8G8_SRGB;  case VK_FORMAT_R8G8_SRGB: return VK_FORMAT_R8G8_UNORM;
        default: return VK_FORMAT_UNDEFINED;
    }
}
static uint8_t g_fmt_mutable[256];   /* NVN formats seen viewed outside their format list: created MUTABLE without one */
static vk_img *img_create(const tex_t *t) {
    VkFormat f; VkComponentMapping fs;
    if (!vk_fmt(t->d.format, &f, &fs) || !f) { RED(8, "texture format %#x has no Vulkan format (ser %llu %ux%u)", t->d.format, (unsigned long long)t->serial, t->d.width, t->d.height); return NULL; }
    VkFormatFeatureFlags ff = fmt_features(f);
    if (!(ff & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) && !(ff & (VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT))) {
        RED(8, "texture format %#x (Vulkan %d) is not supported by this driver", t->d.format, f); return NULL;
    }
    vk_img *im = calloc(1, sizeof *im);
    image_layers(t, &im->type, &im->w, &im->h, &im->d, &im->layers, &im->samples);
    uint32_t biggest = MAXV(im->w, im->h); if (im->type == VK_IMAGE_TYPE_3D) biggest = MAXV(biggest, im->d);
    uint32_t allowed = 1; for (uint32_t v = biggest; v > 1; v >>= 1) allowed++;
    im->levels = t->d.levels ? t->d.levels : 1; if (im->levels > allowed) im->levels = allowed; if (im->samples > 1) im->levels = 1;
    im->fmt = f; im->nvn_fmt = t->d.format; im->aspect = vk_fmt_aspect(t->d.format);
    im->usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (ff & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) im->usage |= VK_IMAGE_USAGE_SAMPLED_BIT;
    if (ff & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT) im->usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    if (ff & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) im->usage |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    if ((ff & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) && (t->d.flags & NVN_TEXTURE_FLAGS_IMAGE)) im->usage |= VK_IMAGE_USAGE_STORAGE_BIT;
    VkImageCreateFlags flags = 0;
    int compressed; { int bw, bh; nvn_fmt_bpb(t->d.format, &bw, &bh); compressed = bw > 1 || bh > 1; }
    VkFormat list[2] = {f, srgb_twin(f)}; int listed = 0;
    if (!compressed && !vk_fmt_is_depth(t->d.format) && t->d.format != 0x31) {   /* views of another format, same texel size */
        if (fmtlist_on() && !g_fmt_mutable[t->d.format & 255]) { listed = 1; if (list[1]) flags |= VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT; }
        else flags |= VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
    }
    if (compressed) flags |= VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;   /* sRGB / UNORM views of the same blocks */
    /* TOTK renders into BC4 textures through an RGBA16UI view whose texel is one block (nvn_mtl.m blk_aux). Vulkan
       has that view natively; the image must allow attachment usage for the view's format. */
    int blk_rt = 0;
    if (compressed && im->samples == 1 && (im->type == VK_IMAGE_TYPE_2D || im->type == VK_IMAGE_TYPE_3D) && V.get_image_format_props) {
        VkImageFormatProperties ip;
        VkImageCreateFlags bf = flags | VK_IMAGE_CREATE_BLOCK_TEXEL_VIEW_COMPATIBLE_BIT | VK_IMAGE_CREATE_EXTENDED_USAGE_BIT;
        if (!V.get_image_format_props(V.pd, f, im->type, VK_IMAGE_TILING_OPTIMAL, im->usage | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, bf, &ip)) { flags = bf; blk_rt = 1; }
    }
    if (im->type == VK_IMAGE_TYPE_2D && im->w == im->h && im->layers >= 6 && im->samples == 1) flags |= VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
    if (im->type == VK_IMAGE_TYPE_3D) flags |= VK_IMAGE_CREATE_2D_ARRAY_COMPATIBLE_BIT;
    VkImageFormatListCreateInfo fl = {VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO, .viewFormatCount = 2, .pViewFormats = list};
    VkImageCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, listed && list[1] ? &fl : NULL, .flags = flags, .imageType = im->type, .format = f, .extent = {im->w, im->h, im->d},
        .mipLevels = im->levels, .arrayLayers = im->layers, .samples = (VkSampleCountFlagBits)im->samples, .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = im->usage, .sharingMode = VK_SHARING_MODE_EXCLUSIVE, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    if (blk_rt) ci.usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    VkResult r = vkCreateImage(V.dev, &ci, NULL, &im->img);
    if (r && blk_rt) { blk_rt = 0; ci.usage = im->usage; ci.flags &= ~(VK_IMAGE_CREATE_BLOCK_TEXEL_VIEW_COMPATIBLE_BIT | VK_IMAGE_CREATE_EXTENDED_USAGE_BIT); r = vkCreateImage(V.dev, &ci, NULL, &im->img); }
    if (r && (ci.flags & VK_IMAGE_CREATE_2D_ARRAY_COMPATIBLE_BIT)) { ci.flags &= ~VK_IMAGE_CREATE_2D_ARRAY_COMPATIBLE_BIT; r = vkCreateImage(V.dev, &ci, NULL, &im->img); }
    im->usage = ci.usage; im->blk_rt = blk_rt; im->listed = listed; im->twin = list[1]; im->arr3d = !!(ci.flags & VK_IMAGE_CREATE_2D_ARRAY_COMPATIBLE_BIT);
    if (r) { RED(8, "vkCreateImage %ux%ux%u fmt %d levels %u layers %u: %d", im->w, im->h, im->d, f, im->levels, im->layers, r); free(im); return NULL; }
    VkMemoryRequirements req; vkGetImageMemoryRequirements(V.dev, im->img, &req);
    if (!vk_mem_alloc(&req, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &im->mem)) { RED(8, "out of GPU memory for a %ux%u texture (%llu MiB)", im->w, im->h, (unsigned long long)(req.size >> 20)); vkDestroyImage(V.dev, im->img, NULL); free(im); return NULL; }
    VKCHECK(vkBindImageMemory(V.dev, im->img, im->mem.mem, im->mem.off));
    image_barrier_init(vk_pre_cb(), im->img, im->aspect);
    g_stat_images++; im->born = g_stat_images;
    return im;
}

/* Upload a mirrored texture from the guest's bytes (pre command buffer). */
static void upload(vk_tex *x) {
    const tex_t *t = &x->t; vk_img *im = x->im;
    int bw, bh, bpb = nvn_fmt_bpb(t->d.format, &bw, &bh);
    if (!bpb) return;
    x->stamp = nvn_dirty_now();   /* before the copy: a write during it is newer than this content */
    nvn_dirty_protect(t->mem, t->L.size);
    int is3d = im->type == VK_IMAGE_TYPE_3D, linear = nvn_tex_is_linear(&t->d);
    VkCommandBuffer cb = vk_pre_cb();
    vk_barrier_all(cb);
    if (linear && !is3d && im->layers == 1) {   /* pitch textures: the pool bytes are already rows */
        size_t stride = nvn_tex_linear_stride(&t->d); VkBuffer pb; uint64_t po;
        uint32_t w = im->w, h = im->h; size_t rows = (h + bh - 1) / bh;
        if (stride % bpb == 0 && vk_resolve((uint64_t)(uintptr_t)t->mem, stride * rows, &pb, &po) && po % 16 == 0) {
            VkBufferImageCopy c = {.bufferOffset = po, .bufferRowLength = (uint32_t)(stride / bpb * bw), .imageSubresource = {im->aspect, 0, 0, 1}, .imageExtent = {w, h, 1}};
            vkCmdCopyBufferToImage(cb, pb, im->img, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
            vk_barrier_all(cb); g_stat_upload_bytes += stride * rows;
            return;
        }
    }
    size_t total = 0;
    for (unsigned l = 0; l < im->levels; l++) {
        uint32_t w = MAXV(im->w >> l, 1u), h = MAXV(im->h >> l, 1u), z = is3d ? MAXV(im->d >> l, 1u) : 1;
        total += (((size_t)((w + bw - 1) / bw) * bpb * ((h + bh - 1) / bh) * z + 15) & ~(size_t)15) * (is3d ? 1 : im->layers);
    }
    VkBuffer sb; VkDeviceSize so; uint8_t *stg = vk_transient(total ? total : 16, 16, &sb, &so);
    VkBufferImageCopy *regions = calloc((size_t)im->levels * im->layers, sizeof *regions); int nr = 0;
    size_t off = 0;
    for (unsigned l = 0; l < im->levels; l++) {
        uint32_t w = MAXV(im->w >> l, 1u), h = MAXV(im->h >> l, 1u), z = is3d ? MAXV(im->d >> l, 1u) : 1;
        size_t pitch = (size_t)((w + bw - 1) / bw) * bpb, rows = (h + bh - 1) / bh;
        for (unsigned y = 0; y < (is3d ? 1u : im->layers); y++) {
            int bad;
            if (linear) { size_t stride = nvn_tex_linear_stride(&t->d); bad = 0; for (size_t r = 0; r < rows * z; r++) memcpy(stg + off + r * pitch, t->mem + r * stride, pitch); }
            else bad = nvn_deswizzle(&t->d, l, y, t->mem, stg + off, pitch);
            if (bad) { RED(8, "deswizzle failed: format %#x target %d", t->d.format, t->d.target); free(regions); return; }
            regions[nr++] = (VkBufferImageCopy){.bufferOffset = so + off, .imageSubresource = {im->aspect, l, y, 1}, .imageExtent = {w, h, z}};
            off += (pitch * rows * z + 15) & ~(size_t)15;
        }
    }
    if (nr) vkCmdCopyBufferToImage(cb, sb, im->img, VK_IMAGE_LAYOUT_GENERAL, (uint32_t)nr, regions);
    free(regions);
    vk_barrier_all(cb);
    g_stat_upload_bytes += total;
}

static void tex_drop_locked(vk_tex *x) {
    if (!x) return;
    if (x->tbv) retire_bview(x->tbv);
    if (x->im) {
        if (x->im->shared) { if (--x->im->refs == 0) x->im->idle_sub = vk_submit_serial(); }
        else if (x->im->wbrefs) x->im->orphan = 1;   /* a rendering waiting for write-back keeps it */
        else retire_img(x->im);
    }
    retire_host(x);
}
/* TOTK_VK_TEXLOG=<nvn format hex>[,...] (diagnostic): how textures of these formats get their image and content */
static int texlog_on(int f) {
    static const char *e = (const char *)1; if (e == (const char *)1) e = getenv("TOTK_VK_TEXLOG");
    if (e) for (const char *p = e; *p; ) { char *z; long k = strtol(p, &z, 16); if (z == p) break; if (k == f) return 1; p = *z ? z + 1 : z; }
    return 0;
}
static void texlog(const tex_t *t, const char *what, const vk_img *im) {
    static _Atomic int n; if (!texlog_on(t->d.format) || atomic_fetch_add(&n, 1) >= 400) return;
    LOGV("[texlog] ser %llu fmt %#x %ux%u lv %u tgt %d mem %p size %llu flags %#x pool %#x%s layout %d linear %d: %s (image fmt %#x vk %d gpu_written %d shared %d)",
         (unsigned long long)t->serial, t->d.format, t->d.width, t->d.height, t->d.levels, t->d.target, (void *)t->mem, (unsigned long long)t->L.size, t->d.flags,
         t->pool ? t->pool->flags : 0, t->pool && t->pool->host ? " host" : "", t->has_layout, nvn_tex_is_linear(&t->d), what,
         im ? im->nvn_fmt : -1, im ? (int)im->fmt : -1, im ? im->gpu_written : -1, im ? im->shared : -1);
}
vk_tex *vk_tex_get(const tex_t *t) {
    if (!t || !t->serial) return NULL;
    pthread_mutex_lock(&g_tex_mu);
    vk_tex *x = nvn_hm_get(&g_by_serial, &t->serial);
    if (x && x->im && x->mirrored && !x->im->gpu_written && x->csub != vk_replay_serial() && texlog_on(t->d.format)
        && (!nvn_dirty_tracked(t->mem, t->L.size) || nvn_dirty_since(t->mem, t->L.size, x->stamp))) texlog(t, "re-upload (CPU wrote it)", x->im);
    uint64_t sub = vk_replay_serial();
    if (x && x->im && x->mirrored && !x->im->gpu_written && x->csub != sub) {   /* once per replay: changed since the upload? */
        x->csub = sub;
        stor_key k; vk_img *g = NULL;
        if (mirshare_on() && stor_key_of(t, &k) && (g = nvn_hm_get(&g_by_storage, &k)) && g != x->im && g->gpu_written) {
            retire_img(x->im); x->im = g; g->refs++; x->mirrored = 0; g_stat_switched++;   /* the GPU renders this storage now */
        } else if (!nvn_dirty_tracked(t->mem, t->L.size) || nvn_dirty_since(t->mem, t->L.size, x->stamp)) { upload(x); g_stat_reuploads++; }
    }
    if (x) { pthread_mutex_unlock(&g_tex_mu); return x->im || x->tbv ? x : NULL; }
    x = calloc(1, sizeof *x); x->serial = t->serial; x->t = *t; x->csub = sub;
    nvn_hm_put(&g_by_serial, &t->serial, x);
    if (t->d.target == 10) { pthread_mutex_unlock(&g_tex_mu); return x; }   /* buffer textures: vk_tbuf_by_id */
    x->mirrored = x->cpu_pool = tex_mirrored(t);
    stor_key k;
    if (x->mirrored && mirshare_on() && stor_key_of(t, &k)) {   /* a GPU-written image already holds this storage */
        vk_img *g = nvn_hm_get(&g_by_storage, &k);
        if (g && g->gpu_written) { x->mirrored = 0; g->refs++; x->im = g; g_stat_shared_hits++; texlog(t, "new, CPU pool, takes a GPU-written image", g); pthread_mutex_unlock(&g_tex_mu); return x; }
    }
    if (!x->mirrored && stor_key_of(t, &k)) {
        vk_img *im = nvn_hm_get(&g_by_storage, &k);
        if (im) { g_stat_shared_hits++;
            if (fmt_class(im->nvn_fmt) != fmt_class(t->d.format)) { uint64_t n = ++g_stat_xfmt;
                if (n <= 8) LOGV("storage %p (%ux%u) shared across formats: image NVN format %#x, texture %#x%s (%llu so far)", (void *)t->mem, t->d.width, t->d.height, im->nvn_fmt, t->d.format, stor_compressed(t) ? ", compressible" : "", (unsigned long long)n); } }
        else if ((im = img_create(t))) { im->shared = 1; im->k = k; nvn_hm_put(&g_by_storage, &k, im); if (k.fcls) {
            stor_key k0 = k; k0.fcls = 0; uintptr_t prev = (uintptr_t)nvn_hm_get(&g_comp_owner, &k0);
            if (prev && prev != k.fcls) { uint64_t n = ++g_stat_compsplit;
                if (n <= 8) LOGV("compressible storage %p (%ux%u) now NVN format %#x: it gets its own image, the other format keeps its content (%llu so far)", (void *)t->mem, t->d.width, t->d.height, t->d.format, (unsigned long long)n); }
            nvn_hm_put(&g_comp_owner, &k0, (void *)(uintptr_t)k.fcls);
        } }
        if (im) { im->refs++; x->im = im; }
        texlog(t, "new, shared image by storage", im);
    } else {
        x->im = img_create(t);
        if (x->im && x->mirrored) { upload(x); g_stat_uploads++; }
        texlog(t, x->mirrored ? "new, CPU pool: uploaded" : "new, own image (no storage key)", x->im);
    }
    pthread_mutex_unlock(&g_tex_mu);
    return x->im ? x : NULL;
}
void mtl_tex_release(tex_t *t) {
    if (!t->serial) return;
    pthread_mutex_lock(&g_tex_mu);
    vk_tex *x = nvn_hm_del(&g_by_serial, &t->serial);
    tex_drop_locked(x);
    pthread_mutex_unlock(&g_tex_mu);
}
void vk_tex_pool_gone(uintptr_t lo, uintptr_t hi) {   /* nvn_vk.c: a pool was finalised: shared images over it are dropped */
    pthread_mutex_lock(&g_tex_mu);
    for (uint32_t i = 0; i < g_by_storage.cap; i++) {
        vk_img *im = g_by_storage.vals ? g_by_storage.vals[i] : NULL;
        if (!im || im->refs || im->k.mem < lo || im->k.mem >= hi) continue;
        stor_key k = im->k; nvn_hm_del(&g_by_storage, &k); retire_img(im); i--;
    }
    pthread_mutex_unlock(&g_tex_mu);
}

VkImage vk_tex_image(vk_tex *x) { return x->im->img; }
uint64_t vk_tex_serial(vk_tex *x) { return x->serial; }
uint32_t vk_tex_samples(vk_tex *x) { return x->im->samples; }
VkFormat vk_tex_format(vk_tex *x) { return x->im->fmt; }
int vk_tex_nvn_format(vk_tex *x) { return x->im->nvn_fmt; }
uint32_t vk_tex_width(vk_tex *x, unsigned l) { return MAXV(x->im->w >> l, 1u); }
uint32_t vk_tex_height(vk_tex *x, unsigned l) { return MAXV(x->im->h >> l, 1u); }
uint32_t vk_tex_layers(vk_tex *x) { return x->im->layers; }
uint32_t vk_tex_levels(vk_tex *x) { return x->im->levels; }
int vk_tex_is3d(vk_tex *x) { return x->im->type == VK_IMAGE_TYPE_3D; }
VkImageAspectFlags vk_tex_aspect(vk_tex *x) { return x->im->aspect; }
void vk_end_rendering(void);
/* ---------- GPU write-back into CPU-visible pools (nvn_mtl.m wb_note / mtl_pool_invalidate, TOTK_VK_OFF=writeback) ----------
   On hardware the GPU writes a texture's memory, and the game reads it back with the CPU after
   nvnMemoryPoolInvalidateMappedRange. Here a texture in a CPU-visible pool keeps what the GPU rendered only in its
   VkImage (block-linear tiling is not the Vulkan image's), so the CPU read the bytes uploaded before. The game bakes
   terrain tile normals on the GPU into such a pool and copies them with the CPU into new tiles: with stale normals the
   ground went black in patches (the Mac's 2026-10-15 bug, seen on the Thor too). Each GPU write to such a texture is
   noted (with a reference on its image, so the result outlives a temporary texture object); an invalidate over its
   memory waits for the GPU and writes every level back in the NVN layout, unless the CPU has written that memory since
   (its bytes are newer then). One record per memory address: the newest rendering there is the one written back. */
typedef struct { tex_t t; vk_img *im; uint64_t stamp, sub; } wb_ent;
static wb_ent *g_wb; static int g_nwb, g_capwb;   /* g_tex_mu */
static uint64_t g_wb_lo = UINT64_MAX, g_wb_hi;   /* covers every record (only grows until the records are swept): unlocked quick reject */
static uint64_t g_wb_done, g_wb_skip_cpu, g_wb_skip_fmt, g_wb_superseded, g_wb_bytes, g_wb_aged, g_wb_age_kept;
static int wb_on(void) { static int on = -1; if (on < 0) on = !vk_off("writeback"); return on; }
static int invalskip_on(void) { static int on = -1; if (on < 0) on = !vk_off("invalskip"); return on; }
static _Atomic uint64_t g_wb_inval_skipped;   /* invalidates with no record in range, returned before the drain */
static void wb_ref(vk_img *im) { if (im->shared) im->refs++; else im->wbrefs++; }
static void wb_unref(vk_img *im) {
    if (im->shared) { if (--im->refs == 0) im->idle_sub = vk_submit_serial(); }
    else if (--im->wbrefs == 0 && im->orphan) retire_img(im);
}
static void wb_ent_drop_locked(int i) {   /* the entry's image reference goes; the entry is removed */
    wb_unref(g_wb[i].im);
    g_wb[i] = g_wb[--g_nwb];
}
static void wb_note_locked(vk_tex *x) {
    const tex_t *t = &x->t;
    if (!wb_on() || !x->cpu_pool || !x->im || !t->mem || !t->L.size) return;
    /* block-compressed ones too: TOTK renders terrain into BC textures through a one-texel-per-block view (blk_rt) */
    int bw, bh; if (!nvn_fmt_bpb(t->d.format, &bw, &bh) || t->d.target == 2 || t->d.samples > 1 || (x->im->aspect & ~VK_IMAGE_ASPECT_COLOR_BIT)) return;
    /* the stamp comes first: a write that faults while the pages are protected counts as after it (nvn_mtl.m #90) */
    uint64_t stamp = nvn_dirty_now();
    nvn_dirty_protect_exact(t->mem, t->L.size);
    for (int i = 0; i < g_nwb; i++) if (g_wb[i].t.mem == t->mem) {
        wb_ent *e = &g_wb[i];
        if (e->im != x->im) { wb_ref(x->im); wb_unref(e->im); e->im = x->im; }
        e->t = *t; e->stamp = stamp; e->sub = vk_submit_serial(); return;
    }
    if (g_nwb == g_capwb) { g_capwb = g_capwb ? 2 * g_capwb : 64; g_wb = realloc(g_wb, (size_t)g_capwb * sizeof *g_wb); }
    wb_ref(x->im);
    { uint64_t m = (uint64_t)(uintptr_t)t->mem; if (m < g_wb_lo) g_wb_lo = m; if (m + t->L.size > g_wb_hi) g_wb_hi = m + t->L.size; }
    g_wb[g_nwb++] = (wb_ent){*t, x->im, stamp, vk_submit_serial()};
}
/* nvn_vk.c vk_gpu_write: a GPU copy into memory that a noted rendering covers is newer than it (nvn_mtl.m
   botw_gpu_copy_supersedes: an invalidate wrote an older rendering back over copied bytes). Recorded in GPU order. */
void vk_wb_gpu_wrote(uint64_t a, uint64_t n) {
    if (!n || !g_nwb || a >= g_wb_hi || a + n <= g_wb_lo) return;   /* unlocked look: only the recording thread adds records */
    pthread_mutex_lock(&g_tex_mu);
    for (int i = 0; i < g_nwb; ) {
        uint64_t m = (uint64_t)(uintptr_t)g_wb[i].t.mem;
        if (m < a + n && a < m + g_wb[i].t.L.size) { wb_ent_drop_locked(i); g_wb_superseded++; } else i++;
    }
    pthread_mutex_unlock(&g_tex_mu);
}
void vk_wb_range_gone(uintptr_t lo, uintptr_t hi) {   /* a pool's memory goes: nothing is written back into it */
    pthread_mutex_lock(&g_tex_mu);
    for (int i = 0; i < g_nwb; ) { uintptr_t m = (uintptr_t)g_wb[i].t.mem; if (m < hi && lo < m + g_wb[i].t.L.size) wb_ent_drop_locked(i); else i++; }
    pthread_mutex_unlock(&g_tex_mu);
}
/* Renderings nobody invalidated for a long time keep no orphaned image alive (memory). Only orphans age (TOTK,
   2026-10-23): a record whose image a texture object still holds, or a shared image (kept until its pool goes), costs
   nothing, and dropping it lost renderings the game reads back much later - after ~3000 submissions standing still the
   ground lost its grass and leaf litter and stayed that way (the invalidate then found no record and the CPU read the
   old bytes). The age counted Vulkan submissions, so the submission split (~3.5 a frame) brought it ~3x sooner: the
   "terrain tiles only with the split" bug. TOTK_VK_WB_AGE=<submissions> (default 3000; 0 = never). */
static void wb_age_locked(void) {
    uint64_t sub = vk_submit_serial();
    static long age = -1; if (age < 0) { const char *e = getenv("TOTK_VK_WB_AGE"); age = e && *e ? atol(e) : 3000; }
    if (age) for (int i = 0; i < g_nwb; ) {
        if (sub - g_wb[i].sub <= (uint64_t)age) { i++; continue; }
        const vk_img *im = g_wb[i].im;
        if (im->shared || !im->orphan) { g_wb[i].sub = sub; g_wb_age_kept++; i++; continue; }   /* checked again after another age */
        wb_ent_drop_locked(i); g_wb_aged++;
    }
    uint64_t lo = UINT64_MAX, hi = 0;   /* the quick-reject range shrinks back to the records left */
    for (int i = 0; i < g_nwb; i++) { uint64_t m = (uint64_t)(uintptr_t)g_wb[i].t.mem; if (m < lo) lo = m; if (m + g_wb[i].t.L.size > hi) hi = m + g_wb[i].t.L.size; }
    g_wb_lo = lo; g_wb_hi = hi;
}
void mtl_pool_invalidate(const pool_t *p, uint64_t off, uint64_t n) {
    if (!p || !p->mem || !n || !wb_on()) return;
    uint64_t t0 = host_now_ns();
    const uint8_t *a = p->mem + off, *z = a + n;
    /* No rendering to write back in the range: done, without waiting for the translation queue or the recording lock
       (TOTK_VK_OFF=invalskip: drain first as before). The game can only expect renderings of submissions whose fence it
       waited for, and a fence wait returns only once its submission is translated, so their records are noted already;
       renderings still queued are of submissions nobody waited for. 2026-10-25: ~93 % of invalidates found no record,
       but each one waited ~10 ms for the queue on a recording thread, ~6 ms before the frame's submit. */
    if (invalskip_on()) {
        int hit = 0;
        pthread_mutex_lock(&g_tex_mu);
        if (g_nwb && (uint64_t)(uintptr_t)a < g_wb_hi && (uint64_t)(uintptr_t)z > g_wb_lo)
            for (int i = 0; i < g_nwb && !hit; i++) hit = g_wb[i].t.mem < z && a < g_wb[i].t.mem + g_wb[i].t.L.size;
        pthread_mutex_unlock(&g_tex_mu);
        if (!hit) { uint64_t k = ++g_wb_inval_skipped; if (k <= 4 || k % 5000 == 0) LOGV("[writeback] %llu invalidates with no rendering in range: returned without draining the replay queue (TOTK_VK_OFF=invalskip: drain)", (unsigned long long)k); return; }
    }
    /* with the translation thread (TOTK_ASYNC_REPLAY=1) the renderings this invalidate is about may still be queued:
       they run (and note themselves) first (nvn_mtl.m wb_write_list does the same) */
    { extern void nvn_x_drain(void) __attribute__((weak)); if (nvn_x_drain) nvn_x_drain(); }
    vk_rec_lock();
    pthread_mutex_lock(&g_tex_mu);
    int nl = 0, cap = 0; wb_ent *list = NULL;
    for (int i = 0; i < g_nwb; ) {
        wb_ent *e = &g_wb[i];
        if (e->t.mem >= z || e->t.mem + e->t.L.size <= a) { i++; continue; }
        if (nl == cap) { cap = cap ? 2 * cap : 16; list = realloc(list, (size_t)cap * sizeof *list); }
        list[nl++] = *e; g_wb[i] = g_wb[--g_nwb];   /* the reference moves to the list */
    }
    pthread_mutex_unlock(&g_tex_mu);
    if (!nl) { vk_rec_unlock(); return; }
    /* Freshness is decided before anything is written back (a write-back marks no pages written, but keep the order). */
    size_t total = 0; int any = 0;
    int *fresh = calloc((size_t)nl, sizeof *fresh);
    for (int i = 0; i < nl; i++) {
        wb_ent *e = &list[i];
        fresh[i] = !nvn_dirty_tracked(e->t.mem, e->t.L.size) || !nvn_dirty_since(e->t.mem, e->t.L.size, e->stamp);
        if (!fresh[i]) { if (++g_wb_skip_cpu <= 16) LOGV("[writeback] skipped (the CPU wrote it since the rendering) ser=%llu %ux%u nvnfmt=%#x", (unsigned long long)e->t.serial, e->t.d.width, e->t.d.height, e->t.d.format); continue; }
        int bw, bh, bpb = nvn_fmt_bpb(e->t.d.format, &bw, &bh);
        unsigned levels = MINV(e->t.d.levels ? e->t.d.levels : 1u, e->im->levels), layers = MINV(e->t.L.layers ? e->t.L.layers : 1u, e->im->layers);
        if (nvn_tex_is_linear(&e->t.d)) levels = layers = 1;
        for (unsigned l = 0; l < levels; l++) total += (((size_t)((MAXV(e->im->w >> l, 1u) + bw - 1) / bw) * bpb * ((MAXV(e->im->h >> l, 1u) + bh - 1) / bh) + 15) & ~(size_t)15) * layers;
        any = 1;
    }
    if (any) {
        vk_end_rendering();
        VkCommandBuffer cb = vk_cb();
        VkBuffer sb; VkDeviceSize so; uint8_t *stg = vk_transient(total ? total : 16, 16, &sb, &so);
        vk_barrier_all(cb);
        size_t o = 0;
        for (int i = 0; i < nl; i++) {
            if (!fresh[i]) continue;
            wb_ent *e = &list[i]; int bw, bh, bpb = nvn_fmt_bpb(e->t.d.format, &bw, &bh);
            unsigned levels = MINV(e->t.d.levels ? e->t.d.levels : 1u, e->im->levels), layers = MINV(e->t.L.layers ? e->t.L.layers : 1u, e->im->layers);
            if (nvn_tex_is_linear(&e->t.d)) levels = layers = 1;
            for (unsigned l = 0; l < levels; l++) for (unsigned y = 0; y < layers; y++) {
                uint32_t w = MAXV(e->im->w >> l, 1u), h = MAXV(e->im->h >> l, 1u);
                VkBufferImageCopy c = {.bufferOffset = so + o, .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, l, y, 1}, .imageExtent = {w, h, 1}};   /* the whole level: no block alignment needed */
                vkCmdCopyImageToBuffer(cb, e->im->img, VK_IMAGE_LAYOUT_GENERAL, sb, 1, &c);
                o += ((size_t)((w + bw - 1) / bw) * bpb * ((h + bh - 1) / bh) + 15) & ~(size_t)15;
            }
        }
        vk_flush_wait();
        o = 0;
        for (int i = 0; i < nl; i++) {
            if (!fresh[i]) continue;
            wb_ent *e = &list[i]; const tex_t *t = &e->t; int bw, bh, bpb = nvn_fmt_bpb(t->d.format, &bw, &bh);
            unsigned levels = MINV(t->d.levels ? t->d.levels : 1u, e->im->levels), layers = MINV(t->L.layers ? t->L.layers : 1u, e->im->layers);
            if (nvn_tex_is_linear(&t->d)) levels = layers = 1;
            /* the GPU's bytes landing in memory: not a CPU write */
            void *tok = nvn_dirty_tracked(t->mem, t->L.size) ? nvn_dirty_gpu_write_begin(t->mem, t->L.size) : NULL;
            int ok = 1;
            for (unsigned l = 0; l < levels; l++) for (unsigned y = 0; y < layers; y++) {
                uint32_t w = MAXV(e->im->w >> l, 1u), h = MAXV(e->im->h >> l, 1u);
                size_t pitch = (size_t)((w + bw - 1) / bw) * bpb;
                if (nvn_swizzle(&t->d, l, y, stg + o, pitch, (void *)t->mem)) ok = 0;
                o += (pitch * ((h + bh - 1) / bh) + 15) & ~(size_t)15;
            }
            nvn_dirty_gpu_write_end(tok);
            if (!ok) { if (++g_wb_skip_fmt <= 8) RED(4, "GPU write-back: layout not supported ser=%llu %ux%u format %#x target %d", (unsigned long long)t->serial, t->d.width, t->d.height, t->d.format, t->d.target); continue; }
            g_wb_bytes += t->L.size;
            if (bw > 1) { static int nbc; if (++nbc <= 8) LOGV("[writeback] block-compressed ser=%llu %ux%u nvnfmt=%#x lv%u written back (%d so far)", (unsigned long long)t->serial, t->d.width, t->d.height, t->d.format, t->d.levels, nbc); }
            uint64_t d = ++g_wb_done;
            if (d <= 8 || d % 500 == 0) LOGV("[writeback] ser=%llu %ux%u nvnfmt=%#x lv%u: GPU rendering written back to memory (%llu so far, %llu MiB; %llu skipped as written by the CPU since, %llu superseded by GPU copies)",
                (unsigned long long)t->serial, t->d.width, t->d.height, t->d.format, t->d.levels, (unsigned long long)d, (unsigned long long)(g_wb_bytes >> 20), (unsigned long long)g_wb_skip_cpu, (unsigned long long)g_wb_superseded);
        }
    }
    pthread_mutex_lock(&g_tex_mu);
    for (int i = 0; i < nl; i++) wb_unref(list[i].im);
    pthread_mutex_unlock(&g_tex_mu);
    free(list); free(fresh);
    vk_rec_unlock();
    HK_ADD(inval, 1); HK_ADD(inval_ns, host_now_ns() - t0);
}
void vk_tex_mark_gpu_written(vk_tex *x) {
    if (!x || !x->im) return;
    if (x->im->gpu_written && !x->mirrored && !x->cpu_pool) return;
    pthread_mutex_lock(&g_tex_mu);
    if (!x->im->gpu_written) texlog(&x->t, "the GPU writes it (first time)", x->im);
    x->im->gpu_written = 1;
    stor_key k;
    if (x->mirrored && !x->im->shared && mirshare_on() && stor_key_of(&x->t, &k) && !nvn_hm_get(&g_by_storage, &k)) {
        x->im->shared = 1; x->im->k = k; x->im->refs = 1; nvn_hm_put(&g_by_storage, &k, x->im); x->mirrored = 0; g_stat_published++;
    }
    if (x->cpu_pool) wb_note_locked(x);
    pthread_mutex_unlock(&g_tex_mu);
}

/* ---------- views ---------- */
static VkImageViewType view_type_of(int target, const vk_img *im, uint32_t nlayers) {
    switch (target) {
        case 0: return VK_IMAGE_VIEW_TYPE_1D; case 2: return VK_IMAGE_VIEW_TYPE_3D; case 3: return VK_IMAGE_VIEW_TYPE_1D_ARRAY;
        case 4: case 6: return VK_IMAGE_VIEW_TYPE_2D_ARRAY;
        case 8: return nlayers >= 6 && (im->w == im->h) ? VK_IMAGE_VIEW_TYPE_CUBE : VK_IMAGE_VIEW_TYPE_2D;
        case 9: return nlayers >= 6 && nlayers % 6 == 0 ? VK_IMAGE_VIEW_TYPE_CUBE_ARRAY : VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    }
    return VK_IMAGE_VIEW_TYPE_2D;
}
/* attach: 0 sampling (want_target: the shader's declared target, -1 = the texture's own), 1 attachment, 2 storage */
static VkImageView view_get(vk_tex *x, const view_t *v, int attach, int want_target) {
    vk_img *im = x->im; const tex_t *t = &x->t;
    int fmt = v && v->set_format ? v->format : t->d.format;
    VkFormat vf = im->fmt; VkComponentMapping fs = {VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_G, VK_COMPONENT_SWIZZLE_B, VK_COMPONENT_SWIZZLE_A};
    if (v && v->set_format) {
        int bw, bh, vbw, vbh, bpb = nvn_fmt_bpb(t->d.format, &bw, &bh), vbpb = nvn_fmt_bpb(v->format, &vbw, &vbh);
        VkFormat f2;
        int blk = im->blk_rt && attach == 1 && vbpb == bpb && vbw == 1 && vbh == 1;   /* one texel per block */
        if (!vk_fmt(v->format, &f2, &fs) || !f2 || vbpb != bpb || (!blk && (vbw != bw || vbh != bh)) || vk_fmt_aspect(v->format) != vk_fmt_aspect(t->d.format)) { RED(16, "texture view format %#x refused on a %#x texture (ser %llu)", v->format, t->d.format, (unsigned long long)t->serial); return VK_NULL_HANDLE; }
        vf = f2;
        if (im->listed && vf != im->fmt && vf != im->twin) {
            g_fmt_mutable[t->d.format & 255] = 1;
            RED(16, "texture %#x (ser %llu) viewed as %#x, outside its format list: view refused, its later images are MUTABLE", t->d.format, (unsigned long long)t->serial, v->format);
            return VK_NULL_HANDLE;
        }
        if (vf != im->fmt) {   /* (diagnostic, 2026-10-25, UBWC) each pair of image / view format once: what a format list must hold */
            static uint64_t seen[256]; static int nseen; uint64_t key = (uint64_t)t->d.format << 32 | v->format; int i;
            for (i = 0; i < nseen && seen[i] != key; i++) {}
            if (i == nseen && nseen < 256) { seen[nseen++] = key; LOGV("view format: texture %#x (vk %d) %ux%u viewed as %#x (vk %d) as %s", t->d.format, im->fmt, im->w, im->h, v->format, vf,
                                                                       attach == 1 ? "render target" : attach == 2 ? "storage image" : "sampled"); }
        }
    } else vk_fmt(fmt, NULL, &fs);
    unsigned l0 = v && v->set_levels ? v->base_level : 0, nl = v && v->set_levels && v->nlevels ? v->nlevels : im->levels - MINV(l0, im->levels);
    unsigned s0 = v && v->set_layers ? v->min_layer : 0;
    /* layers of a 3D texture are its depth slices at the level: render targets of one slice (2D_ARRAY_COMPATIBLE);
       sampling always takes the whole volume */
    unsigned nslices = im->type == VK_IMAGE_TYPE_3D ? (attach == 1 && l0 < im->levels ? MAXV(im->d >> l0, 1u) : 1) : im->layers;
    if (im->type == VK_IMAGE_TYPE_3D && attach != 1) s0 = 0;
    if (im->type == VK_IMAGE_TYPE_3D && attach == 1 && !im->arr3d) { RED(8, "render target in a 3D texture slice: this driver has no 2D views of 3D images (ser %llu)", (unsigned long long)t->serial); return VK_NULL_HANDLE; }
    unsigned ns = v && v->set_layers && v->nlayers ? v->nlayers : nslices - MINV(s0, nslices);
    if (im->type == VK_IMAGE_TYPE_3D && attach != 1) ns = 1;
    if (l0 >= im->levels || !nl || l0 + nl > im->levels || s0 + ns > nslices) { RED(16, "invalid texture view (levels %u+%u of %u, layers %u+%u of %u, ser %llu)", l0, nl, im->levels, s0, ns, im->layers, (unsigned long long)t->serial); return VK_NULL_HANDLE; }
    int target = v && v->set_target ? v->target : t->d.target;
    VkImageViewType vt;
    VkComponentMapping sw = {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY};
    VkImageAspectFlags aspect = im->aspect;
    if (attach == 1) {   /* one level; the layers of the view (a layered pass) or one */
        nl = 1; vt = ns > 1 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
        if (im->type == VK_IMAGE_TYPE_1D) vt = ns > 1 ? VK_IMAGE_VIEW_TYPE_1D_ARRAY : VK_IMAGE_VIEW_TYPE_1D;
    } else {
        if (want_target >= 0) {   /* the shader's declaration wins where the layout allows it (the Mac's tex_type_view) */
            if (want_target == 1 && (target == 4 || target == 8 || target == 9)) { target = 1; ns = 1; }
            else if (want_target == 4 && (target == 1 || target == 7 || target == 8 || target == 9)) target = 4;
            else if (want_target == 8 && target == 9) { target = 8; ns = 6; }
            else if (want_target == 9 && target == 8) target = 9;
            else if (want_target == 2 && target != 2) { RED(8, "a 3D texture slot holds a %d texture", target); return VK_NULL_HANDLE; }
        }
        vt = view_type_of(target, im, ns);
        if (vt == VK_IMAGE_VIEW_TYPE_2D || vt == VK_IMAGE_VIEW_TYPE_1D) ns = 1;
        if (vt == VK_IMAGE_VIEW_TYPE_CUBE) ns = 6;
        if (vt == VK_IMAGE_VIEW_TYPE_CUBE_ARRAY) ns -= ns % 6;
        if (s0 + ns > im->layers && im->type != VK_IMAGE_TYPE_3D) { RED(16, "texture view of %u layers from %u of %u (ser %llu)", ns, s0, im->layers, (unsigned long long)t->serial); return VK_NULL_HANDLE; }
        if (aspect == (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)) aspect = VK_IMAGE_ASPECT_DEPTH_BIT;   /* sampling a depth-stencil texture reads depth */
        if (attach == 0) {   /* NVN swizzle selectors over the format's own remap (nvn_mtl.m tex_swizzle_sel) */
            unsigned sel[4] = {t->d.swizzle_r, t->d.swizzle_g, t->d.swizzle_b, t->d.swizzle_a};
            if (v && v->set_swizzle) for (unsigned i = 0; i < 4; i++) sel[i] = v->swz[i];
            const VkComponentSwizzle fc[4] = {fs.r, fs.g, fs.b, fs.a};
            VkComponentSwizzle c[4];
            for (unsigned i = 0; i < 4; i++) c[i] = sel[i] == 0 ? VK_COMPONENT_SWIZZLE_ZERO : sel[i] == 1 ? VK_COMPONENT_SWIZZLE_ONE : sel[i] <= 5 ? fc[sel[i] - 2] : VK_COMPONENT_SWIZZLE_IDENTITY;
            if (!t->d.swizzle_r && !t->d.swizzle_g && !t->d.swizzle_b && !t->d.swizzle_a && !(v && v->set_swizzle)) { c[0] = fs.r; c[1] = fs.g; c[2] = fs.b; c[3] = fs.a; }   /* no selectors: the format's remap */
            sw = (VkComponentMapping){c[0], c[1], c[2], c[3]};
        }
    }
    if (vt == VK_IMAGE_VIEW_TYPE_3D && im->type != VK_IMAGE_TYPE_3D) vt = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    if (im->type == VK_IMAGE_TYPE_3D && attach != 1) { vt = VK_IMAGE_VIEW_TYPE_3D; s0 = 0; ns = 1; }
    uint64_t k0 = (uint64_t)vf | (uint64_t)vt << 32 | (uint64_t)aspect << 40 | (uint64_t)(attach & 3) << 48, k1 = (uint64_t)l0 | (uint64_t)nl << 8 | (uint64_t)s0 << 16 | (uint64_t)ns << 28
        | (uint64_t)(sw.r & 7) << 40 | (uint64_t)(sw.g & 7) << 43 | (uint64_t)(sw.b & 7) << 46 | (uint64_t)(sw.a & 7) << 49;
    for (int i = 0; i < im->nviews; i++) if (im->views[i].k0 == k0 && im->views[i].k1 == k1) return im->views[i].v;
    VkImageViewUsageCreateInfo uci = {VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO,
        .usage = attach == 1 ? im->usage & (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT)
               : attach == 2 ? VK_IMAGE_USAGE_STORAGE_BIT : VK_IMAGE_USAGE_SAMPLED_BIT};
    if (!(im->usage & uci.usage)) { RED(16, "texture ser %llu cannot be %s (format %#x)", (unsigned long long)t->serial, attach == 1 ? "a render target" : attach == 2 ? "a storage image" : "sampled", t->d.format); return VK_NULL_HANDLE; }
    VkImageViewCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, &uci, .image = im->img, .viewType = vt, .format = vf, .components = sw,
        .subresourceRange = {aspect, l0, nl, s0, ns}};
    VkImageView view;
    if (vkCreateImageView(V.dev, &ci, NULL, &view)) { RED(16, "vkCreateImageView failed (ser %llu fmt %d type %d)", (unsigned long long)t->serial, vf, vt); return VK_NULL_HANDLE; }
    if (im->nviews == im->capviews) { im->capviews = im->capviews ? 2 * im->capviews : 4; im->views = realloc(im->views, (size_t)im->capviews * sizeof *im->views); }
    im->views[im->nviews++] = (view_ent){k0, k1, view};
    return view;
}
VkImageView vk_tex_view(vk_tex *x, const tex_t *t, const view_t *v, int attach) {
    (void)t;
    pthread_mutex_lock(&g_tex_mu); VkImageView r = x && x->im ? view_get(x, v, attach, -1) : VK_NULL_HANDLE; pthread_mutex_unlock(&g_tex_mu);
    return r;
}

/* ---------- texture / sampler pools: id -> registered object ---------- */
static vk_treg *g_treg; static int g_ntreg, g_tpool_n;
static samp_par *g_sreg; static uint8_t *g_sreg_ok; static VkSampler *g_sobj; static int g_nsreg;
static pthread_mutex_t g_reg_mu = PTHREAD_MUTEX_INITIALIZER;
static _Atomic uint64_t g_treg_epoch = 1;   /* every texture registration or pool size change (vk_treg_epoch) */
void mtl_set_tex_pool_n(int n) { g_tpool_n = n; atomic_fetch_add(&g_treg_epoch, 1); }
/* Per-replay lookup cache (TOTK, 2026-10-22: the world binds ~30k texture ids a frame; each took the registry, texture
   and view locks and copied the registry entry, and those locks were ~15% of the CPU in a profile). An id's resolved view
   is kept for the rest of the submission while its registry entry is unchanged (g_tgen: bumped by every registration of
   that id, never reallocated). vk_tex_get's once-per-submission work (re-upload checks, storage switches) still runs at
   the first use in each submission. TOTK_VK_OFF=texcache: off. */
#define TGEN_CAP (1u << 20)
static _Atomic uint32_t *g_tgen;
static _Atomic uint32_t g_samp_epoch = 1;
static int texcache_on(void) { static int on = -1; if (on < 0) on = !vk_off("texcache"); return on; }
void mtl_reg_tex(int tid, const tex_t *t, const view_t *v) {
    if (tid < 0) return;
    pthread_mutex_lock(&g_reg_mu);
    if (tid >= g_ntreg) { int n = tid + 1024; g_treg = realloc(g_treg, (size_t)n * sizeof *g_treg); memset(g_treg + g_ntreg, 0, (size_t)(n - g_ntreg) * sizeof *g_treg); g_ntreg = n; }
    vk_treg *r = &g_treg[tid];
    r->ok = t != NULL; if (t) r->t = *t; r->has_v = v != NULL;
    if (v) r->v = *v; else memset(&r->v, 0, sizeof r->v);
    if (!g_tgen) g_tgen = calloc(TGEN_CAP, sizeof *g_tgen);
    if ((unsigned)tid < TGEN_CAP) atomic_fetch_add(&g_tgen[tid], 1);
    atomic_fetch_add(&g_treg_epoch, 1);
    pthread_mutex_unlock(&g_reg_mu);
}
int vk_treg_get(int tid, vk_treg *out) {   /* g_treg grows by realloc on the game's threads: never hand out a pointer into it */
    pthread_mutex_lock(&g_reg_mu);
    int ok = tid > 0 && tid < g_ntreg && !(g_tpool_n && tid >= g_tpool_n) && g_treg[tid].ok;
    if (ok) *out = g_treg[tid];
    pthread_mutex_unlock(&g_reg_mu);
    return ok;
}
/* The format a texture id is sampled as (view format or the texture's), -1 for an empty id; cached per id until its next
   registration (vk_shader.c stage_sig asks for every bound id on every draw: ~7% of the CPU through the registry lock). */
uint64_t vk_treg_epoch(void) { return atomic_load(&g_treg_epoch); }
int vk_treg_fmt(int tid) {
    static int32_t *fmt; static uint32_t *fgen;
    if (texcache_on() && g_tgen && tid > 0 && (unsigned)tid < TGEN_CAP) {
        if (!fmt) { fmt = calloc(TGEN_CAP, sizeof *fmt); fgen = calloc(TGEN_CAP, sizeof *fgen); }
        uint32_t gen = atomic_load(&g_tgen[tid]) + 1;   /* + 1: 0 marks "never looked up" */
        if (fgen[tid] == gen) return fmt[tid];
        vk_treg rc; int f = vk_treg_get(tid, &rc) ? (rc.has_v && rc.v.set_format ? rc.v.format : rc.t.d.format) : -1;
        fmt[tid] = f; fgen[tid] = gen; return f;
    }
    vk_treg rc; return vk_treg_get(tid, &rc) ? (rc.has_v && rc.v.set_format ? rc.v.format : rc.t.d.format) : -1;
}
VkImageView vk_tex_by_id(int tid, int *target_out, vk_tex **x_out) {
    vk_treg rc, *r = &rc;
    if (x_out) *x_out = NULL;
    enum { TC = 1 << 14 };
    static struct { int tid, want; uint32_t gen; uint64_t sub; VkImageView v; vk_tex *x; } tc[TC];
    int want0 = target_out ? *target_out : -1;
    uint32_t gen = 0; uint64_t sub = 0; unsigned h = 0; int cacheable = texcache_on() && g_tgen && tid > 0 && (unsigned)tid < TGEN_CAP;
    if (cacheable) {
        /* per replay, not per Vulkan submission (2026-10-23): vk_tex_get's checks are once per replay (vk_replay_serial,
           which CPU steps bump too), and with the split every 2 ms a frame has ~6 submissions: the cache started over 6
           times a frame */
        gen = atomic_load(&g_tgen[tid]); sub = vk_replay_serial(); h = ((unsigned)tid * 2654435761u ^ (unsigned)want0 * 40503u) & (TC - 1);
        if (tc[h].tid == tid && tc[h].want == want0 && tc[h].gen == gen && tc[h].sub == sub) { if (x_out) *x_out = tc[h].x; return tc[h].v; }
    }
    if (!vk_treg_get(tid, r)) return VK_NULL_HANDLE;
    vk_tex *x = vk_tex_get(&r->t);
    if (!x || !x->im) return VK_NULL_HANDLE;
    int want = target_out ? *target_out : -1;
    pthread_mutex_lock(&g_tex_mu);
    VkImageView v = view_get(x, r->has_v ? &r->v : NULL, want == -2 ? 2 : 0, want == -2 ? -1 : want);
    pthread_mutex_unlock(&g_tex_mu);
    if (x_out) *x_out = x;
    if (cacheable && v) { tc[h].tid = tid; tc[h].want = want0; tc[h].gen = gen; tc[h].sub = sub; tc[h].v = v; tc[h].x = x; }
    return v;
}
VkBufferView vk_tbuf_by_id(int tid) {
    vk_treg rc, *r = &rc;
    if (!vk_treg_get(tid, r) || r->t.d.target != 10) return VK_NULL_HANDLE;
    vk_tex *x = vk_tex_get(&r->t);
    pthread_mutex_lock(&g_tex_mu);
    if (!x) { x = nvn_hm_get(&g_by_serial, &r->t.serial); }
    VkBufferView bv = x ? x->tbv : VK_NULL_HANDLE;
    if (x && !bv) {
        int bw, bh, bpb = nvn_fmt_bpb(r->t.d.format, &bw, &bh); VkFormat f; VkBuffer b; uint64_t off;
        if (!vk_fmt(r->t.d.format, &f, NULL) || !f || !bpb) RED(4, "buffer texture format %#x has no Vulkan format", r->t.d.format);
        else if (!vk_resolve((uint64_t)(uintptr_t)r->t.mem, (uint64_t)r->t.d.width * bpb, &b, &off)) RED(4, "buffer texture storage is outside the memory pools");
        else if (off % V.props.limits.minTexelBufferOffsetAlignment) RED(4, "buffer texture offset %#llx is not %llu-aligned", (unsigned long long)off, (unsigned long long)V.props.limits.minTexelBufferOffsetAlignment);
        else {
            VkBufferViewCreateInfo ci = {VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO, .buffer = b, .format = f, .offset = off, .range = (VkDeviceSize)r->t.d.width * bpb};
            if (ci.range > (VkDeviceSize)V.props.limits.maxTexelBufferElements * bpb) ci.range = (VkDeviceSize)V.props.limits.maxTexelBufferElements * bpb;
            if (!vkCreateBufferView(V.dev, &ci, NULL, &x->tbv)) bv = x->tbv;
        }
    }
    pthread_mutex_unlock(&g_tex_mu);
    return bv;
}
void mtl_reg_samp(int sid, const samp_par *sp) {
    if (sid < 0) return;
    pthread_mutex_lock(&g_reg_mu);
    if (sid >= g_nsreg) {
        int n = sid + 256; g_sreg = realloc(g_sreg, (size_t)n * sizeof *g_sreg); g_sreg_ok = realloc(g_sreg_ok, (size_t)n); g_sobj = realloc(g_sobj, (size_t)n * sizeof *g_sobj);
        memset(g_sreg_ok + g_nsreg, 0, (size_t)(n - g_nsreg)); memset(g_sobj + g_nsreg, 0, (size_t)(n - g_nsreg) * sizeof *g_sobj); g_nsreg = n;
    }
    if (g_sobj[sid] && !memcmp(&g_sreg[sid], sp, sizeof *sp)) { pthread_mutex_unlock(&g_reg_mu); return; }
    if (g_sobj[sid]) { /* the old sampler may still be in recorded descriptor sets: keep it (samplers are few) */ g_sobj[sid] = VK_NULL_HANDLE; }
    g_sreg[sid] = *sp; g_sreg_ok[sid] = 1;
    atomic_fetch_add(&g_samp_epoch, 1);
    pthread_mutex_unlock(&g_reg_mu);
}
static VkSampler make_sampler(const samp_par *p) {
    VkSamplerCreateInfo ci = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    ci.magFilter = p->magf ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    ci.minFilter = (p->minf == 1 || p->minf == 3 || p->minf == 5) ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    ci.mipmapMode = (p->minf == 4 || p->minf == 5) ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
    ci.addressModeU = vk_wrap(p->ws); ci.addressModeV = vk_wrap(p->wt); ci.addressModeW = vk_wrap(p->wr);
    ci.mipLodBias = p->lod_bias;
    ci.minLod = p->minf <= 1 ? 0 : (p->lod_min < 0 ? 0 : p->lod_min);
    ci.maxLod = p->minf <= 1 ? 0.25f : p->lod_max;   /* not mipmapped: level 0 only (the Mac's MipFilterNotMipmapped) */
    if (p->aniso > 1 && V.props.limits.maxSamplerAnisotropy > 1) { ci.anisotropyEnable = VK_TRUE; ci.maxAnisotropy = MINV(p->aniso, V.props.limits.maxSamplerAnisotropy); }
    if (p->cmp_mode) { ci.compareEnable = VK_TRUE; ci.compareOp = vk_compare(p->cmp_func); }
    VkSamplerCustomBorderColorCreateInfoEXT cb = {VK_STRUCTURE_TYPE_SAMPLER_CUSTOM_BORDER_COLOR_CREATE_INFO_EXT};
    VkSamplerReductionModeCreateInfo rm = {VK_STRUCTURE_TYPE_SAMPLER_REDUCTION_MODE_CREATE_INFO, .reductionMode = (VkSamplerReductionMode)(p->reduction <= 2 ? p->reduction : 0)};
    const void *chain = NULL;
    if (p->reduction) { rm.pNext = chain; chain = &rm; }
    const float *b = p->border; int grey = b[0] == b[1] && b[1] == b[2];
    if (grey && b[0] == 0 && b[3] == 0) ci.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    else if (grey && b[0] == 0 && b[3] == 1) ci.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
    else if (grey && b[0] == 1 && b[3] == 1) ci.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
    else if (V.has_custom_border) { ci.borderColor = VK_BORDER_COLOR_FLOAT_CUSTOM_EXT; memcpy(cb.customBorderColor.float32, b, 16); cb.format = VK_FORMAT_UNDEFINED; cb.pNext = chain; chain = &cb; }
    else ci.borderColor = b[3] > 0.5f ? (b[0] > 0.5f ? VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE : VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK) : VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    ci.pNext = chain;
    VkSampler s;
    if (vkCreateSampler(V.dev, &ci, NULL, &s)) { RED(4, "vkCreateSampler failed"); return VK_NULL_HANDLE; }
    return s;
}
/* nocmp: the sampler with its depth compare turned off (vk_draw.c: a compare sampler on a texture the shader reads
   plainly, or on a view whose format has no depth-compare support; Eden PR 4324 drops it there too) */
static VkSampler samp_get(int sid, int nocmp) {
    enum { SC = 1 << 12 };   /* sampler ids change rarely: cached until any sampler registration changes one */
    static struct { int sid; uint32_t ep; VkSampler s; } sc[2][SC];
    uint32_t ep = atomic_load(&g_samp_epoch); unsigned h = (unsigned)sid & (SC - 1);
    if (texcache_on() && sid > 0 && sc[nocmp][h].sid == sid && sc[nocmp][h].ep == ep) return sc[nocmp][h].s;
    pthread_mutex_lock(&g_reg_mu);
    VkSampler s = VK_NULL_HANDLE;
    if (sid > 0 && sid < g_nsreg && g_sreg_ok[sid]) {
        /* identical parameters share a sampler: drivers limit how many exist (maxSamplerAllocationCount) */
        static nvn_hmap by_par = {.klen = sizeof(samp_par)};
        if (!nocmp || !g_sreg[sid].cmp_mode) {
            if (!g_sobj[sid]) {
                VkSampler have = nvn_hm_get(&by_par, &g_sreg[sid]);
                if (!have && (have = make_sampler(&g_sreg[sid]))) nvn_hm_put(&by_par, &g_sreg[sid], (void *)have);
                g_sobj[sid] = have;
            }
            s = g_sobj[sid];
        } else {
            samp_par p = g_sreg[sid]; p.cmp_mode = 0; p.cmp_func = 0;
            s = nvn_hm_get(&by_par, &p);
            if (!s && (s = make_sampler(&p))) nvn_hm_put(&by_par, &p, (void *)s);
        }
    }
    pthread_mutex_unlock(&g_reg_mu);
    if (s) { sc[nocmp][h].sid = sid; sc[nocmp][h].ep = ep; sc[nocmp][h].s = s; }
    return s;
}
VkSampler vk_samp_by_id(int sid) { return samp_get(sid, 0); }
VkSampler vk_samp_by_id_nocmp(int sid) { return samp_get(sid, 1); }
int vk_samp_cmp(int sid) {   /* the sampler compares depth (cached like the samplers) */
    enum { SC = 1 << 12 };
    static struct { int sid; uint32_t ep; int8_t c; } sc[SC];
    uint32_t ep = atomic_load(&g_samp_epoch); unsigned h = (unsigned)sid & (SC - 1);
    if (sid <= 0) return 0;
    if (texcache_on() && sc[h].sid == sid && sc[h].ep == ep) return sc[h].c;
    pthread_mutex_lock(&g_reg_mu);
    int c = sid < g_nsreg && g_sreg_ok[sid] && g_sreg[sid].cmp_mode;
    pthread_mutex_unlock(&g_reg_mu);
    sc[h].sid = sid; sc[h].ep = ep; sc[h].c = (int8_t)c;
    return c;
}
/* the format can be sampled with depth compare (VK_FORMAT_FEATURE_2_SAMPLED_IMAGE_DEPTH_COMPARISON_BIT, optimal tiling) */
int vk_fmt_cmp_ok(VkFormat f) {
    static int8_t memo[512];   /* core formats; 0 unknown, 1 no, 2 yes */
    if ((unsigned)f < 512 && memo[f]) return memo[f] == 2;
    VkFormatProperties3 p3 = {VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_3};
    VkFormatProperties2 p2 = {VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2, &p3};
    if (!V.get_format_props2) return 1;
    V.get_format_props2(V.pd, f, &p2);
    int ok = (p3.optimalTilingFeatures & VK_FORMAT_FEATURE_2_SAMPLED_IMAGE_DEPTH_COMPARISON_BIT) != 0;
    if ((unsigned)f < 512) memo[f] = (int8_t)(ok ? 2 : 1);
    return ok;
}
VkSampler vk_dummy_sampler(void) {
    static VkSampler s;
    if (!s) { VkSamplerCreateInfo ci = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .maxLod = 0.25f}; VKCHECK(vkCreateSampler(V.dev, &ci, NULL, &s)); }
    return s;
}
/* Empty slots: null descriptors when the driver has them, else a 1x1 zero image of the declared type. */
VkImageView vk_dummy_view(int target, int is_int) {
    if (V.has_null_descriptor) return VK_NULL_HANDLE;
    static VkImageView views[11][2]; static VkImage imgs[2][2]; static vk_alloc mems[2][2];
    int t = target < 0 || target > 10 ? 1 : target, ii = is_int ? 1 : 0;
    if (views[t][ii]) return views[t][ii];
    int cube = t == 8 || t == 9, three = t == 2;
    int slot = three ? 1 : 0;
    if (!imgs[slot][ii]) {
        VkImageCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .flags = slot ? 0 : VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT, .imageType = slot ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D,
            .format = ii ? VK_FORMAT_R8G8B8A8_UINT : VK_FORMAT_R8G8B8A8_UNORM, .extent = {1, 1, 1}, .mipLevels = 1, .arrayLayers = slot ? 1 : 6, .samples = VK_SAMPLE_COUNT_1_BIT,
            .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT};
        VKCHECK(vkCreateImage(V.dev, &ci, NULL, &imgs[slot][ii]));
        VkMemoryRequirements req; vkGetImageMemoryRequirements(V.dev, imgs[slot][ii], &req);
        if (!vk_mem_alloc(&req, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &mems[slot][ii])) DIE("dummy texture memory");
        VKCHECK(vkBindImageMemory(V.dev, imgs[slot][ii], mems[slot][ii].mem, mems[slot][ii].off));
        VkCommandBuffer cb = vk_pre_cb(); image_barrier_init(cb, imgs[slot][ii], VK_IMAGE_ASPECT_COLOR_BIT);
        VkClearColorValue z = {{0}}; VkImageSubresourceRange rr = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, VK_REMAINING_ARRAY_LAYERS};
        vkCmdClearColorImage(cb, imgs[slot][ii], VK_IMAGE_LAYOUT_GENERAL, &z, 1, &rr); vk_barrier_all(cb);
    }
    VkImageViewType vt = t == 0 ? VK_IMAGE_VIEW_TYPE_1D : t == 3 ? VK_IMAGE_VIEW_TYPE_1D_ARRAY : t == 4 || t == 6 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : cube ? (t == 9 ? VK_IMAGE_VIEW_TYPE_CUBE_ARRAY : VK_IMAGE_VIEW_TYPE_CUBE) : three ? VK_IMAGE_VIEW_TYPE_3D : VK_IMAGE_VIEW_TYPE_2D;
    if (vt == VK_IMAGE_VIEW_TYPE_1D || vt == VK_IMAGE_VIEW_TYPE_1D_ARRAY) vt = vt == VK_IMAGE_VIEW_TYPE_1D ? VK_IMAGE_VIEW_TYPE_2D : VK_IMAGE_VIEW_TYPE_2D_ARRAY;   /* a 2D image has no 1D views; 1D slots are rare */
    VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = imgs[slot][ii], .viewType = vt, .format = ii ? VK_FORMAT_R8G8B8A8_UINT : VK_FORMAT_R8G8B8A8_UNORM,
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, cube ? 6 : 1}};
    VKCHECK(vkCreateImageView(V.dev, &vi, NULL, &views[t][ii]));
    return views[t][ii];
}
VkBufferView vk_dummy_tbuf(void) {
    if (V.has_null_descriptor) return VK_NULL_HANDLE;
    static VkBufferView bv;
    if (!bv) {
        static VkBuffer keep; static VkDeviceMemory km;
        VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = 256, .usage = VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT};
        VKCHECK(vkCreateBuffer(V.dev, &bci, NULL, &keep)); VkMemoryRequirements req; vkGetBufferMemoryRequirements(V.dev, keep, &req);
        VkMemoryAllocateInfo mai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size, .memoryTypeIndex = (uint32_t)vk_pick_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)};
        VKCHECK(vkAllocateMemory(V.dev, &mai, NULL, &km)); VKCHECK(vkBindBufferMemory(V.dev, keep, km, 0));
        void *m; VKCHECK(vkMapMemory(V.dev, km, 0, VK_WHOLE_SIZE, 0, &m)); memset(m, 0, 256);
        VkBufferViewCreateInfo ci = {VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO, .buffer = keep, .format = VK_FORMAT_R32_UINT, .range = 256};
        VKCHECK(vkCreateBufferView(V.dev, &ci, NULL, &bv)); }
    return bv;
}

/* ---------- CPU texel access (ReadTexels / WriteTexels): nvn_mtl.m mtl_read_texels / mtl_write_texels ---------- */
void vk_end_rendering(void);
static void read_texels(const tex_t *t, unsigned level, unsigned layer0, const NVNcopyRegion *r, uint8_t *dst, size_t row, size_t img) {
    int bw, bh, bpb = nvn_fmt_bpb(t->d.format, &bw, &bh);
    size_t wb = (size_t)((r->width + bw - 1) / bw) * bpb, hb = (r->height + bh - 1) / bh, x0 = (size_t)(r->xoffset / bw) * bpb, y0 = r->yoffset / bh;
    int nz = r->depth ? r->depth : 1, is3d = t->d.target == 2;
    vk_tex *x = vk_tex_get(t);
    if (tex_mirrored(t) && !(x && x->im && x->im->gpu_written)) {   /* the pool bytes are the content */
        uint32_t lw = MAXV(t->d.width >> level, 1u), lh = MAXV(t->d.height >> level, 1u), lz = is3d ? MAXV(t->d.depth >> level, 1u) : 1;
        size_t lp = (size_t)((lw + bw - 1) / bw) * bpb, lr = (lh + bh - 1) / bh;
        uint8_t *lin = malloc(lp * lr * lz);
        for (int z = 0; z < nz; z++) {
            unsigned layer = is3d ? 0 : layer0 + r->zoffset + z;
            if ((is3d && z == 0) || !is3d) if (nvn_deswizzle(&t->d, level, layer, t->mem, lin, lp)) { RED(4, "ReadTexels: deswizzle failed"); free(lin); return; }
            const uint8_t *src = lin + (is3d ? (size_t)(r->zoffset + z) * lp * lr : 0);
            for (size_t y = 0; y < hb; y++) memcpy(dst + z * img + y * row, src + (y0 + y) * lp + x0, wb);
        }
        free(lin);
        return;
    }
    if (!x || !x->im) { memset(dst, 0, img * nz); return; }
    vk_end_rendering();
    VkCommandBuffer cb = vk_cb();
    VkBuffer sb; VkDeviceSize so; uint8_t *stg = vk_transient(wb * hb * nz, 16, &sb, &so);
    vk_barrier_all(cb);
    for (int z = 0; z < nz; z++) {
        VkBufferImageCopy c = {.bufferOffset = so + (VkDeviceSize)z * wb * hb, .imageSubresource = {x->im->aspect == (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT) ? VK_IMAGE_ASPECT_DEPTH_BIT : x->im->aspect, level, is3d ? 0 : layer0 + r->zoffset + z, 1},
            .imageOffset = {r->xoffset, r->yoffset, is3d ? r->zoffset + z : 0}, .imageExtent = {(uint32_t)r->width, (uint32_t)r->height, 1}};
        vkCmdCopyImageToBuffer(cb, x->im->img, VK_IMAGE_LAYOUT_GENERAL, sb, 1, &c);
    }
    vk_flush_wait();
    for (int z = 0; z < nz; z++) for (size_t y = 0; y < hb; y++) memcpy(dst + z * img + y * row, stg + (size_t)z * wb * hb + y * wb, wb);
}
static void write_texels(const tex_t *t, unsigned level, unsigned layer0, const NVNcopyRegion *r, const uint8_t *src, size_t row, size_t img) {
    int bw, bh, bpb = nvn_fmt_bpb(t->d.format, &bw, &bh);
    if (!bpb || !r->width || !r->height) return;
    size_t wb = (size_t)((r->width + bw - 1) / bw) * bpb, hb = (r->height + bh - 1) / bh, x0 = (size_t)(r->xoffset / bw) * bpb, y0 = r->yoffset / bh;
    int nz = r->depth ? r->depth : 1, is3d = t->d.target == 2;
    vk_tex *x = vk_tex_get(t);
    if (tex_mirrored(t)) {   /* pool bytes in block-linear layout; dirty tracking re-uploads them */
        uint32_t lw = MAXV(t->d.width >> level, 1u), lh = MAXV(t->d.height >> level, 1u), lz = is3d ? MAXV(t->d.depth >> level, 1u) : 1;
        size_t lp = (size_t)((lw + bw - 1) / bw) * bpb, lr = (lh + bh - 1) / bh;
        uint8_t *lin = malloc(lp * lr * lz);
        for (int z = 0; z < nz; z++) {
            unsigned layer = is3d ? 0 : layer0 + r->zoffset + z;
            if ((is3d && z == 0) || !is3d) if (nvn_deswizzle(&t->d, level, layer, t->mem, lin, lp)) { free(lin); RED(4, "WriteTexels: unsupported layout"); return; }
            uint8_t *d = lin + (is3d ? (size_t)(r->zoffset + z) * lp * lr : 0);
            for (size_t y = 0; y < hb; y++) memcpy(d + (y0 + y) * lp + x0, src + z * img + y * row, wb);
            if (!is3d || z == nz - 1) nvn_swizzle(&t->d, level, layer, lin, lp, (void *)t->mem);
        }
        free(lin);
        if (!(x && x->im && x->im->gpu_written)) return;
    }
    if (!x || !x->im) return;
    vk_end_rendering();
    VkCommandBuffer cb = vk_cb();
    VkBuffer sb; VkDeviceSize so; uint8_t *stg = vk_transient(wb * hb * nz, 16, &sb, &so);
    for (int z = 0; z < nz; z++) for (size_t y = 0; y < hb; y++) memcpy(stg + (size_t)z * wb * hb + y * wb, src + z * img + y * row, wb);
    vk_barrier_all(cb);
    for (int z = 0; z < nz; z++) {
        VkBufferImageCopy c = {.bufferOffset = so + (VkDeviceSize)z * wb * hb, .imageSubresource = {x->im->aspect, level, is3d ? 0 : layer0 + r->zoffset + z, 1},
            .imageOffset = {r->xoffset, r->yoffset, is3d ? r->zoffset + z : 0}, .imageExtent = {(uint32_t)r->width, (uint32_t)r->height, 1}};
        vkCmdCopyBufferToImage(cb, sb, x->im->img, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
    }
    vk_flush_wait();
}
void vk_res_counters(uint64_t v[7]) { v[0] = g_stat_images; v[1] = g_stat_shared_hits; v[2] = g_stat_uploads; v[3] = g_stat_reuploads; v[4] = g_stat_swept; v[5] = g_stat_published; v[6] = g_stat_switched; }
void vk_res_mem(uint64_t v[8]) {
    pthread_mutex_lock(&g_mem_mu); v[0] = g_mem_block_bytes; v[1] = g_mem_dedicated_bytes; pthread_mutex_unlock(&g_mem_mu);
    pthread_mutex_lock(&g_tex_mu); v[2] = (uint64_t)g_nretired; v[3] = g_by_serial.n; v[4] = g_by_storage.n; v[5] = (uint64_t)g_nwb; v[6] = (uint64_t)g_capretired; pthread_mutex_unlock(&g_tex_mu);
    v[7] = g_stat_swept;
}
void vk_res_stats(void) {
    pthread_mutex_lock(&g_tex_mu);
    if (g_wb_done || g_nwb || g_wb_skip_cpu) LOGV("GPU write-back: %llu renderings written back (%llu MiB), %llu skipped (CPU wrote since), %llu superseded by GPU copies, %llu orphans aged out (%llu live kept), %llu unsupported; %d waiting",
        (unsigned long long)g_wb_done, (unsigned long long)(g_wb_bytes >> 20), (unsigned long long)g_wb_skip_cpu, (unsigned long long)g_wb_superseded, (unsigned long long)g_wb_aged, (unsigned long long)g_wb_age_kept, (unsigned long long)g_wb_skip_fmt, g_nwb);
    LOGV("textures: %u live, %llu images made (%llu storage-shared reuses; %llu CPU-pool images the GPU rendered into shared, %llu serials switched to them; %llu compressible format splits, %llu cross-format shares), %llu uploads + %llu re-uploads (%llu MiB); GPU memory %llu MiB in blocks + %llu MiB dedicated",
         g_by_serial.n, (unsigned long long)g_stat_images, (unsigned long long)g_stat_shared_hits, (unsigned long long)g_stat_published, (unsigned long long)g_stat_switched, (unsigned long long)g_stat_compsplit, (unsigned long long)g_stat_xfmt, (unsigned long long)g_stat_uploads, (unsigned long long)g_stat_reuploads,
         (unsigned long long)(g_stat_upload_bytes >> 20), (unsigned long long)(g_mem_block_bytes >> 20), (unsigned long long)(g_mem_dedicated_bytes >> 20));
    pthread_mutex_unlock(&g_tex_mu);
}

/* game threads: one recorder at a time (vk_rec_lock) */
void mtl_read_texels(const tex_t *t, unsigned level, unsigned layer0, const NVNcopyRegion *r, uint8_t *dst, size_t row, size_t img) { vk_rec_lock(); read_texels(t, level, layer0, r, dst, row, img); vk_rec_unlock(); }
void mtl_write_texels(const tex_t *t, unsigned level, unsigned layer0, const NVNcopyRegion *r, const uint8_t *src, size_t row, size_t img) { vk_rec_lock(); write_texels(t, level, layer0, r, src, row, img); vk_rec_unlock(); }
