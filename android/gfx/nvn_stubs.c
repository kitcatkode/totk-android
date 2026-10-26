/* What the null and Vulkan backends share while the Vulkan one does not draw yet: texture, program and present entry
   points with nothing on the host side, frame counting, and zeros for the statistics and Mac diagnostics the NVN layer
   logs. Each entry moves into nvn_vk.c as the Vulkan backend implements it (then GPU_STUB_* hides it here). */
#include "nvn_obj.h"
#include "nvn_mtl.h"
#include "nvn_tex.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

uint64_t host_now_ns(void);   /* runtime/host_os.c */
#ifndef GPU_LOG_PREFIX
#define GPU_LOG_PREFIX "[nvn-null] "
#endif
#define LOGN(...) do { fprintf(stderr, GPU_LOG_PREFIX __VA_ARGS__); fputc('\n', stderr); } while (0)
uint64_t gpu_stat_submits(void), gpu_stat_draws(void);
void gpu_pool_totals(uint64_t *n, uint64_t *bytes);

#ifndef GPU_VK   /* the Vulkan backend has its own (vk_res.c, vk_shader.c, vk_draw.c) */
/* ---------- textures, samplers, programs: nothing on the host side ---------- */
void mtl_reg_tex(int tid, const tex_t *t, const view_t *v) { (void)tid; (void)t; (void)v; }
void mtl_reg_samp(int sid, const samp_par *sp) { (void)sid; (void)sp; }
void mtl_set_tex_pool_n(int n) { (void)n; }
void mtl_tex_release(tex_t *t) { (void)t; }
void mtl_prog_release(prog_t *p) { (void)p; }
#endif
void mtl_dead_forget(const uint64_t *serials, size_t n) { (void)serials; (void)n; }
void mtl_pin_refs(void (*mark)(const void *)) { (void)mark; }
void mtl_xv_window_tex(uint64_t serial) { (void)serial; }
void mtl_probe_pin_rt(const tex_t *t) { (void)t; }
#ifndef GPU_VK
/* Texel transfers of textures in CPU-visible pools would need the block-linear layout (gfx/tex): not without a GPU. */
void mtl_write_texels(const tex_t *t, unsigned level, unsigned layer0, const NVNcopyRegion *r, const uint8_t *src, size_t row, size_t img) { (void)t; (void)level; (void)layer0; (void)r; (void)src; (void)row; (void)img; }
void mtl_read_texels(const tex_t *t, unsigned level, unsigned layer0, const NVNcopyRegion *r, uint8_t *dst, size_t row, size_t img) {
    (void)t; (void)level; (void)layer0; (void)row;
    if (dst && r && img) memset(dst, 0, img * (size_t)(r->depth > 0 ? r->depth : 1));
}

/* ---------- present ---------- */
static uint64_t g_present_n, g_present_t0;
static double g_pres_t[64];
uint64_t mtl_present(const tex_t *t, double period, const int crop[4]) {
    (void)t; (void)period; (void)crop;
    uint64_t now = host_now_ns(), n = ++g_present_n;
    g_pres_t[n % 64] = now / 1e9;
    if (!g_present_t0) g_present_t0 = now;
    if (n % 300 == 0) { LOGN("frame %llu: %.1f fps over the last 300, %llu submissions, %llu draws skipped", (unsigned long long)n, 300e9 / (double)(now - g_present_t0), (unsigned long long)gpu_stat_submits(), (unsigned long long)gpu_stat_draws()); g_present_t0 = now; }
    return n;
}
void mtl_present_wait(uint64_t tk) { (void)tk; }
double mtl_presented_time(uint64_t n) { return n && n + 64 > g_present_n ? g_pres_t[n % 64] : 0; }
#endif
uint32_t mtl_present_qos(void) { return 0; }
void mtl_shot_at_present(const tex_t *t) { (void)t; }
int mtl_has_window(void) { return 0; }
int mtl_display_hz(void) { return 60; }
int mtl_link_active(void) { return 0; }
int mtl_link_wait_tick(uint64_t timeout_ns, unsigned frame_ticks) { (void)timeout_ns; (void)frame_ticks; return 0; }
int mtl_warm_wait(int timeout_ms) { (void)timeout_ms; return 0; }
void mtl_note_world(void) {}
uint64_t g_draw_skip[16];   /* hle_nvn.c logs it: draws skipped by reason (none here) */
static _Atomic uint64_t g_cur_frame;
void mtl_set_frame(uint64_t f) { atomic_store(&g_cur_frame, f); }
uint64_t gpu_cur_frame(void) { return atomic_load(&g_cur_frame); }
void mtl_ub_present(double iv) { (void)iv; }
void mtl_mem_stat(unsigned long long frame, unsigned long long rss_mb) { LOGN("frame %llu: resident %llu MiB", frame, rss_mb); }

/* ---------- statistics the NVN layer logs: nothing measured here ---------- */
void mtl_stats(uint64_t *passes, uint64_t *draws) { *passes = 0; *draws = gpu_stat_draws(); }
void mtl_alloc_stats(uint64_t *waits, uint64_t *grown, int *n) { *waits = *grown = 0; *n = 0; }
void mtl_args_stats(uint64_t *enc, uint64_t *reuse) { *enc = *reuse = 0; }
void mtl_blit_stats(uint64_t v[6]) { memset(v, 0, 6 * sizeof *v); }
void mtl_bw(uint64_t *load, uint64_t *store, uint64_t *full) { *load = *store = *full = 0; }
uint64_t mtl_chunk_commits(void) { return 0; }
void mtl_create_ns(uint64_t *lib_ns, uint64_t *pso_ns) { *lib_ns = *pso_ns = 0; }
void mtl_create_stats(uint64_t *lib, uint64_t *pso) { *lib = *pso = 0; }
uint64_t mtl_drawable_ns_take(void) { return 0; }
uint64_t mtl_ds_omit(void) { return 0; }
double mtl_gpu_ms(uint64_t f) { (void)f; return 0; }
int mtl_gpu_span(uint64_t f, uint64_t *t0, uint64_t *t1) { (void)f; *t0 = *t1 = 0; return 0; }
void mtl_keepalive_stats(uint64_t v[3]) { memset(v, 0, 3 * sizeof *v); }
void mtl_layer_stats(uint64_t v[4]) { memset(v, 0, 4 * sizeof *v); }
uint64_t mtl_pkg_miss(void) { return 0; }
void mtl_pkg_stats(uint64_t *uploads, uint64_t *reuploads) { *uploads = *reuploads = 0; }
void mtl_pool_res_stats(uint64_t v[4]) { gpu_pool_totals(&v[0], &v[1]); v[2] = v[3] = 0; }
void mtl_ren_stats(uint64_t v[6]) { memset(v, 0, 6 * sizeof *v); }
uint64_t mtl_udc_saved_bytes(void) { return 0; }
uint64_t mtl_upload_bytes(void) { return 0; }
uint64_t mtl_upload_gpu_ns_take(void) { return 0; }
uint64_t mtl_upload_ns_take(void) { return 0; }
void mtl_upload_wait_stats(uint64_t *n, uint64_t *ns) { *n = *ns = 0; }
void mtl_use_stats(uint64_t *call, uint64_t *skip) { *call = *skip = 0; }

/* ---------- graphics diagnostics and picture modes of the Mac build ---------- */
int dm_detclock_enabled(void) { return 0; }
void dm_guest_frame_tick(void) {}
void dm_gfx_anomdump(void) {}
void dm_gfx_set_flag(const char *name, int v) { (void)name; (void)v; }
void dm_gfx_shot_at(long k, const char *path) { (void)k; (void)path; }
void dm_gfx_texdump_next(const char *path) { (void)path; }
void dm_gfx_window_shot(const char *path) { (void)path; }
void dm_gfx_querylog_next(long n) { (void)n; }
#ifndef GPU_VK
void dm_gfx_passdump_next(long n) { (void)n; }   /* vk_draw.c */
#endif
void dm_nvn_probe_prediction(void) {}
int mfx_enabled(void) { return 0; }
int mfx_latch(void) { return 0; }
void mfx_set(int mode) { (void)mode; }
int mfxup_output_enabled(void) { return 0; }
void taa_toggle(void) {}
