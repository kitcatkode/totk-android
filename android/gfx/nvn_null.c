/* The replay interface of the NVN layer (third_party/d2r_gfx/gfx/nvn/nvn_mtl.h, implemented for Metal by nvn_mtl.m)
   without a GPU: Linux / Android headless runs, to bring the game up before the Vulkan backend draws anything.

   What it keeps exact is everything the CPU can observe of the GPU: the command stream of each submission is walked in
   order; buffer copies and uniform updates reach memory; event values and counter reports (NVNcounterData {counter,
   timestamp}) are written when the submission "completes", which is at its commit, so every sync, event and fence
   the game waits for is released in submission order. Draws, clears, texture copies and state are skipped. Occlusion
   queries report everything visible, so the game culls nothing on their account. The present path counts frames (and
   logs the rate every 300); hle_nvn.c paces them by its own clock.

   The texture, program and present entry points and the statistics are shared with the Vulkan backend (nvn_stubs.c). */
#include "nvn_obj.h"
#include "nvn_mtl.h"
#include "nvn_tex.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

uint64_t host_now_ns(void);   /* runtime/host_os.c */
#define LOGN(...) do { fprintf(stderr, "[nvn-null] " __VA_ARGS__); fputc('\n', stderr); } while (0)

/* ---------- memory pools (mtl_addr: which pool holds an address) ---------- */
typedef struct { uintptr_t lo, hi; } pool_ent;
static pool_ent *g_pools; static size_t g_npool, g_cappool;
static pthread_mutex_t g_pool_mu = PTHREAD_MUTEX_INITIALIZER;
void mtl_pool_add(const void *mem, uint64_t size) {
    pthread_mutex_lock(&g_pool_mu);
    if (g_npool == g_cappool) { g_cappool = g_cappool ? 2 * g_cappool : 64; g_pools = realloc(g_pools, g_cappool * sizeof *g_pools); }
    g_pools[g_npool++] = (pool_ent){(uintptr_t)mem, (uintptr_t)mem + size};
    pthread_mutex_unlock(&g_pool_mu);
}
void mtl_pool_del(const void *mem) {
    pthread_mutex_lock(&g_pool_mu);
    for (size_t i = 0; i < g_npool; i++) if (g_pools[i].lo == (uintptr_t)mem) { g_pools[i] = g_pools[--g_npool]; break; }
    pthread_mutex_unlock(&g_pool_mu);
}
int mtl_addr(uint64_t addr, uint64_t len, void **buf, uint64_t *off) {
    int found = 0;
    pthread_mutex_lock(&g_pool_mu);
    for (size_t i = 0; i < g_npool; i++)
        if (addr >= g_pools[i].lo && addr + len <= g_pools[i].hi && addr + len >= addr) { *buf = (void *)g_pools[i].lo; *off = addr - g_pools[i].lo; found = 1; break; }
    pthread_mutex_unlock(&g_pool_mu);
    return found;
}
void mtl_pool_invalidate(const pool_t *p, uint64_t off, uint64_t n) { (void)p; (void)off; (void)n; }   /* CPU memory is the only copy */

/* ---------- submissions ---------- */
typedef struct { int kind; volatile uint32_t *val; uint32_t mode, v; uint64_t addr, type; } post_op;   /* 1 event, 2 counter */
static post_op *g_post; static int g_npost, g_postcap;
static uint64_t g_fence, g_done;
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER; static pthread_cond_t g_cv = PTHREAD_COND_INITIALIZER;
static uint64_t g_stat_sub, g_stat_draw, g_stat_skip_copy;


static uint64_t gpu_ticks(void) { return (uint64_t)((__uint128_t)host_now_ns() * 12 / 625); }   /* 19.2 MHz, the Switch's tick */
static void post(post_op o) {
    if (g_npost == g_postcap) { g_postcap = g_postcap ? g_postcap * 2 : 64; g_post = realloc(g_post, (size_t)g_postcap * sizeof *g_post); }
    g_post[g_npost++] = o;
}
void mtl_init(void) {}
void mtl_sub_begin(void) { g_npost = 0; }
void mtl_sub_copy(uint64_t src, uint64_t dst, uint64_t n) { memmove((void *)(uintptr_t)dst, (const void *)(uintptr_t)src, (size_t)n); }
void mtl_sub_event(volatile uint32_t *val, uint32_t mode, uint32_t v) { post((post_op){1, val, mode, v, 0, 0}); }
void mtl_sub_counter(uint64_t addr, uint32_t type) { post((post_op){2, NULL, 0, 0, addr, type}); }
void *mtl_sub_capture_ubos(const uint64_t *h, int n) { (void)h; (void)n; return NULL; }
void mtl_sub_snapshot_ubos_cap(const uint64_t *h, int n, void *cap) { (void)h; (void)n; free(cap); }
void mtl_sub_index(const uint64_t *h, int n) { (void)h; (void)n; }
void mtl_exec_rest(const uint64_t *h, int n) { (void)h; (void)n; }

/* Walk one command list (hle_nvn.c records them; nvn_obj.h has the format). */
void mtl_exec(const uint8_t *p) {
    for (;;) {
        op_hdr h; memcpy(&h, p, sizeof h);
        if (h.size < 8 || (h.size & 7) || h.op >= OP_COUNT_) { LOGN("【红】corrupt command record at %p: op %u size %u", (const void *)p, h.op, h.size); abort(); }
        const uint8_t *a = p + 8;
        switch (h.op) {
            case OP_END: return;
            case OP_JUMP: { const uint8_t *t; memcpy(&t, a, 8); p = t; continue; }
            case OP_CALL: { const uint8_t *t; memcpy(&t, a, 8); mtl_exec(t); break; }
            case OP_SIGNAL_EVENT: { volatile uint32_t *val; uint32_t m, v; memcpy(&val, a, 8); memcpy(&m, a + 8, 4); memcpy(&v, a + 12, 4); mtl_sub_event(val, m, v); break; }
            case OP_REPORT_COUNTER: { uint64_t t, ad; memcpy(&t, a, 8); memcpy(&ad, a + 8, 8); mtl_sub_counter(ad, (uint32_t)t); break; }
            case OP_COPY_B2B: { uint64_t s, d, n; memcpy(&s, a, 8); memcpy(&d, a + 8, 8); memcpy(&n, a + 16, 8); mtl_sub_copy(s, d, n); break; }
            case OP_UPDATE_UB: { uint64_t d, n; memcpy(&d, a, 8); memcpy(&n, a + 8, 8); memcpy((void *)(uintptr_t)d, a + 16, (size_t)n); break; }
            case OP_DRAW: case OP_DRAW_IDX: case OP_DISPATCH: g_stat_draw++; break;
            case OP_COPY_B2T: case OP_COPY_T2B: case OP_COPY_T2T: case OP_CLEAR_TEX: g_stat_skip_copy++; break;
            default: break;   /* state, targets, clears, barriers */
        }
        p += h.size;
    }
}
/* Completion is immediate: event values and counters land now, in submission order, then the fence is done. */
uint64_t mtl_sub_commit(void) {
    for (int i = 0; i < g_npost; i++) {
        post_op *o = &g_post[i];
        if (o->kind == 1) { if (o->mode == 1) *o->val += o->v; else *o->val = o->v; }
        else {
            uint64_t rec[2] = {o->type == NVN_COUNTER_TYPE_SAMPLES_PASSED ? (1u << 20) : 0, gpu_ticks()};
            memcpy((void *)(uintptr_t)o->addr, rec, sizeof rec);
        }
    }
    g_npost = 0; g_stat_sub++;
    pthread_mutex_lock(&g_mu); uint64_t f = ++g_fence; g_done = f; pthread_cond_broadcast(&g_cv); pthread_mutex_unlock(&g_mu);
    return f;
}
uint64_t mtl_last_fence(void) { return g_fence; }
int mtl_wait(uint64_t fence, uint64_t timeout_ns) { (void)timeout_ns; pthread_mutex_lock(&g_mu); int ok = fence <= g_done; pthread_mutex_unlock(&g_mu); return ok; }
void mtl_finish(void) {}
void mtl_drain(void) {}

uint64_t gpu_stat_submits(void) { return g_stat_sub; }
uint64_t gpu_stat_draws(void) { return g_stat_draw; }
void gpu_pool_totals(uint64_t *n, uint64_t *bytes) { pthread_mutex_lock(&g_pool_mu); *n = g_npool; *bytes = 0; for (size_t i = 0; i < g_npool; i++) *bytes += g_pools[i].hi - g_pools[i].lo; pthread_mutex_unlock(&g_pool_mu); }
