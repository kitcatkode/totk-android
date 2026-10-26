/* Draws through the whole Vulkan backend without the game: shaders found in a seed by their NVN keys, a texture uploaded
   from block-linear guest memory, a render target, a clear, a textured draw with uniforms and a vertex buffer, and a
   GPU copy of the result back into a memory pool, checked pixel by pixel.
     python3 android/gfx/test_vkdraw_seed.py /tmp/vkdraw-seed       (glslangValidator, on the PC)
     TOTK_SHADER_SEED_DIR=/tmp/vkdraw-seed ./vkdrawtest              (lavapipe here; the device with TOTK_VK_DRIVER) */
#include "nvn_dirty.h"
#include "vk_int.h"
#include "../../third_party/d2r_gfx/gfx/tex/nvn_deswizzle.h"
#include <sys/mman.h>
#include <unistd.h>
#ifdef TOTK_ANDROID_APP
#include <android/native_window.h>
#endif

void mtl_pool_invalidate(const pool_t *p, uint64_t off, uint64_t n);
static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL: " __VA_ARGS__); printf("\n"); } } while (0)
static uint8_t rec[1 << 16]; static size_t rn;
static void op(uint32_t code, const void *payload, uint32_t n) {
    uint32_t size = (8 + n + 7) & ~7u; op_hdr h = {code, size};
    memcpy(rec + rn, &h, 8); memcpy(rec + rn + 8, payload, n); memset(rec + rn + 8 + n, 0, size - 8 - n); rn += size;
}
static uint64_t run(void) { op(OP_END, NULL, 0); mtl_sub_begin(); mtl_exec(rec); rn = 0; return mtl_sub_commit(); }

/* the same synthetic shader binaries as test_vkdraw_seed.py */
static void shader_binary(int stage, int salt, uint8_t control[0x880], uint8_t *code, uint32_t *len) {
    memset(control, 0, 0x880);
    *len = 0x230;
    for (int i = 0; i < 0x30; i++) code[i] = (uint8_t)salt;
    for (int i = 0; i < 0x200; i++) code[0x30 + i] = (uint8_t)((i * 7 + salt) & 255);
    uint32_t v = 0x880; memcpy(control + 0x20, &v, 4);
    memcpy(control + 0x700, len, 4); memcpy(control + 0x704, len, 4);
    uint32_t st = (uint32_t)stage; memcpy(control + 0x714, &st, 4);
}

int main(void) {
    const size_t G = 32u << 20;
    uint8_t *g = mmap(NULL, G, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    uint8_t *cpu_pool = g, *gpu_pool = g + (16u << 20);
    setenv("TOTK_PIPE_WAIT_MAX", "0", 0);   /* every check reads its draw in the same submission: build pipelines at once */
    setenv("TOTK_VK_WB_AGE", "20", 0);   /* write-back records age after 20 submissions here (the aging check below) */
#ifdef TOTK_ANDROID_APP
    setenv("TOTK_VK_PASSPROBE", "2", 1);
#endif
    mtl_init();
    mtl_pool_add(cpu_pool, 16u << 20);
    mtl_pool_add(gpu_pool, 16u << 20);
    static pool_t P_cpu = {.flags = NVN_MEMORY_POOL_FLAGS_CPU_CACHED | NVN_MEMORY_POOL_FLAGS_GPU_CACHED}, P_gpu = {.flags = NVN_MEMORY_POOL_FLAGS_CPU_NO_ACCESS | NVN_MEMORY_POOL_FLAGS_GPU_CACHED};
    P_cpu.mem = cpu_pool; P_cpu.size = 16u << 20; P_gpu.mem = gpu_pool; P_gpu.size = 16u << 20;

    /* the sampled texture: 64x64 RGBA8, block-linear in the CPU pool, texel (x, y) = (4x, 4y, 128, 255) */
    tex_t tx; memset(&tx, 0, sizeof tx);
    tx.d.target = 1; tx.d.format = 0x25; tx.d.width = 64; tx.d.height = 64; tx.d.depth = 1; tx.d.levels = 1;
    nvn_tex_layout(&tx.d, &tx.L); tx.mem = cpu_pool + 0x10000; tx.pool = &P_cpu; tx.serial = 101;
    uint8_t lin[64 * 64 * 4];
    for (int y = 0; y < 64; y++) for (int x = 0; x < 64; x++) { uint8_t *p = lin + (y * 64 + x) * 4; p[0] = (uint8_t)(4 * x); p[1] = (uint8_t)(4 * y); p[2] = 128; p[3] = 255; }
    CHECK(!nvn_swizzle(&tx.d, 0, 0, lin, 256, tx.mem), "swizzle the test texture");
    /* the render target: 64x64 RGBA8 in the GPU-only pool */
    tex_t rt; memset(&rt, 0, sizeof rt);
    rt.d = tx.d; nvn_tex_layout(&rt.d, &rt.L); rt.mem = gpu_pool; rt.pool = &P_gpu; rt.serial = 102;
    mtl_reg_tex(1, &tx, NULL);
    samp_par sp; memset(&sp, 0, sizeof sp); sp.lod_max = 1000; mtl_reg_samp(1, &sp);

    /* program */
    static uint8_t vctl[0x880], fctl[0x880], vcode[0x230], fcode[0x230]; uint32_t vlen, flen;
    shader_binary(0, 1, vctl, vcode, &vlen); shader_binary(1, 2, fctl, fcode, &flen);
    uint8_t *vc = cpu_pool + 0x100000, *fc = cpu_pool + 0x101000; memcpy(vc, vcode, vlen); memcpy(fc, fcode, flen);
    prog_t pg; memset(&pg, 0, sizeof pg); pg.nstage = 2; pg.serial = 7;
    pg.sd[0].data = (NVNbufferAddress)(uintptr_t)vc; pg.sd[0].control = vctl; pg.sd[1].data = (NVNbufferAddress)(uintptr_t)fc; pg.sd[1].control = fctl;

    /* uniforms and vertices */
    float *vub = (float *)(cpu_pool + 0x200000), *fub = (float *)(cpu_pool + 0x200100), *vtx = (float *)(cpu_pool + 0x201000);
    vub[0] = vub[1] = vub[2] = vub[3] = 0;
    fub[0] = 1; fub[1] = 0.5f; fub[2] = 1; fub[3] = 1;
    /* a triangle covering the upper half (clip y >= 0 is the top, as on Metal and NVN) */
    float tri[6] = {-1, 0, 3, 0, -1, 4}; memcpy(vtx, tri, sizeof tri);
    uint8_t *readback = cpu_pool + 0x300000; memset(readback, 0xcd, 64 * 64 * 4);

    p_set_rt srt; memset(&srt, 0, sizeof srt); srt.n = 1; srt.color[0] = &rt; op(OP_SET_RT, &srt, sizeof srt);
    p_clear_color cc = {0, 0xf, {0, 0, 1, 1}}; op(OP_CLEAR_COLOR, &cc, sizeof cc);
    p_bind_program bp = {&pg, 3}; op(OP_BIND_PROGRAM, &bp, sizeof bp);
    p_bind_vattr va; memset(&va, 0, sizeof va); va.n = 1; va_st a0 = {0x16, 0, 0}; memcpy(&va.st[0], &a0, 4); op(OP_BIND_VATTR, &va, sizeof va);
    p_bind_vstream vs; memset(&vs, 0, sizeof vs); vs.n = 1; vs.st[0].stride = 8; op(OP_BIND_VSTREAM, &vs, sizeof vs);
    p_bind_vbuf vb; memset(&vb, 0, sizeof vb); vb.n = 1; vb.r[0].address = (NVNbufferAddress)(uintptr_t)vtx; vb.r[0].size = 64; op(OP_BIND_VBUF, &vb, sizeof vb);
    p_bind_buf u0 = {0, 0, (uint64_t)(uintptr_t)vub, 256}, u1 = {1, 0, (uint64_t)(uintptr_t)fub, 256}; op(OP_BIND_UBO, &u0, sizeof u0); op(OP_BIND_UBO, &u1, sizeof u1);
    p_bind_handles th; memset(&th, 0, sizeof th); th.stage = 1; th.first = 0; th.n = 1; th.h[0] = 1 | (1u << 20); op(OP_BIND_TEX, &th, sizeof th);
    p_viewports vp; memset(&vp, 0, sizeof vp); vp.n = 1; vp.v[0][2] = 64; vp.v[0][3] = 64; vp.v[0][5] = 1; op(OP_VIEWPORTS, &vp, sizeof vp);
    p_draw dw = {4, 0, 3, 0, 0}; op(OP_DRAW, &dw, sizeof dw);
    p_copy_b2t cp; memset(&cp, 0, sizeof cp); cp.src = (uint64_t)(uintptr_t)readback; cp.tex = &rt; cp.r = (NVNcopyRegion){0, 0, 0, 64, 64, 1}; op(OP_COPY_T2B, &cp, sizeof cp);
    uint64_t f = run();
    CHECK(mtl_wait(f, 5000000000ull), "the draw submission completes");
    int bad_top = 0, bad_bottom = 0;
    for (int y = 0; y < 64; y++) for (int x = 0; x < 64; x++) {
        const uint8_t *p = readback + (y * 64 + x) * 4;
        if (y < 32) { int er = abs(p[0] - 4 * x), eg = abs(p[1] - 2 * y), eb = abs(p[2] - 128), ea = abs(p[3] - 255); if (er > 2 || eg > 2 || eb > 2 || ea > 2) { if (!bad_top++) printf("  top (%d,%d) = %d %d %d %d, want %d %d 128 255\n", x, y, p[0], p[1], p[2], p[3], 4 * x, 2 * y); } }
        else if (p[0] || p[1] || p[2] != 255 || p[3] != 255) { if (!bad_bottom++) printf("  bottom (%d,%d) = %d %d %d %d, want the clear 0 0 255 255\n", x, y, p[0], p[1], p[2], p[3]); }
    }
    CHECK(!bad_top, "textured upper half (%d wrong pixels): orientation, texture upload, sampler, uniforms", bad_top);
    CHECK(!bad_bottom, "cleared lower half (%d wrong pixels): clear and Y orientation", bad_bottom);

    /* the texture changes in guest memory: re-uploaded at its next use in a later submission */
    for (int i = 0; i < 64 * 64; i++) { lin[4 * i] = 200; lin[4 * i + 1] = 100; }
    nvn_swizzle(&tx.d, 0, 0, lin, 256, tx.mem);
    op(OP_SET_RT, &srt, sizeof srt); op(OP_BIND_PROGRAM, &bp, sizeof bp); op(OP_BIND_VATTR, &va, sizeof va); op(OP_BIND_VSTREAM, &vs, sizeof vs); op(OP_BIND_VBUF, &vb, sizeof vb);
    op(OP_BIND_UBO, &u0, sizeof u0); op(OP_BIND_UBO, &u1, sizeof u1); op(OP_BIND_TEX, &th, sizeof th); op(OP_VIEWPORTS, &vp, sizeof vp);
    op(OP_DRAW, &dw, sizeof dw); op(OP_COPY_T2B, &cp, sizeof cp);
    f = run(); mtl_wait(f, 5000000000ull);
    const uint8_t *p = readback + (5 * 64 + 7) * 4;
    CHECK(abs(p[0] - 200) <= 2 && abs(p[1] - 50) <= 2, "re-upload after a guest write: (7,5) = %d %d, want 200 50", p[0], p[1]);

    /* a uniform update between two draws of one pass: the first draw sees the old bytes, the second the new ones,
       and memory has the new ones after the submission (renaming, nvn_vk.c) */
    {
        float full[6] = {-1, -1, 3, -1, -1, 3}; float *vtx2 = (float *)(cpu_pool + 0x202000); memcpy(vtx2, full, sizeof full);
        p_bind_vbuf vb2 = vb; vb2.r[0].address = (NVNbufferAddress)(uintptr_t)vtx2;
        fub[0] = 1;
        op(OP_SET_RT, &srt, sizeof srt); op(OP_BIND_PROGRAM, &bp, sizeof bp); op(OP_BIND_VATTR, &va, sizeof va); op(OP_BIND_VSTREAM, &vs, sizeof vs); op(OP_BIND_VBUF, &vb2, sizeof vb2);
        op(OP_BIND_UBO, &u0, sizeof u0); op(OP_BIND_UBO, &u1, sizeof u1); op(OP_BIND_TEX, &th, sizeof th); op(OP_VIEWPORTS, &vp, sizeof vp);
        p_scissor top = {0, 0, 64, 32}, bottom = {0, 32, 64, 32};
        op(OP_SCISSOR, &top, sizeof top); op(OP_DRAW, &dw, sizeof dw);
        struct { uint64_t dst, n; float v[4]; } upd = {(uint64_t)(uintptr_t)fub, 16, {0.5f, 0.5f, 1, 1}};
        op(OP_UPDATE_UB, &upd, sizeof upd);
        op(OP_SCISSOR, &bottom, sizeof bottom); op(OP_DRAW, &dw, sizeof dw);
        p_scissor all = {0, 0, 64, 64}; op(OP_SCISSOR, &all, sizeof all);
        op(OP_COPY_T2B, &cp, sizeof cp);
        f = run(); mtl_wait(f, 5000000000ull);
        const uint8_t *a = readback + (10 * 64 + 3) * 4, *b = readback + (50 * 64 + 3) * 4;
        CHECK(abs(a[0] - 200) <= 2 && abs(b[0] - 100) <= 2, "uniform update between two draws: before %d (want 200), after %d (want 100)", a[0], b[0]);
        CHECK(fub[0] == 0.5f, "the update reached memory after the submission (%g)", fub[0]);
    }

    /* the rename memo across log changes: an update elsewhere keeps the first draw's answer valid, a later one over the
       bound range must not (vk_ub_rename scans the changes since the memo) */
    {
        float full[6] = {-1, -1, 3, -1, -1, 3}; float *vtx2 = (float *)(cpu_pool + 0x202000); memcpy(vtx2, full, sizeof full);
        p_bind_vbuf vb2 = vb; vb2.r[0].address = (NVNbufferAddress)(uintptr_t)vtx2;
        fub[0] = 1; fub[1] = 0.5f;
        float *other = (float *)(cpu_pool + 0x208000); other[0] = 0;
        op(OP_SET_RT, &srt, sizeof srt); op(OP_BIND_PROGRAM, &bp, sizeof bp); op(OP_BIND_VATTR, &va, sizeof va); op(OP_BIND_VSTREAM, &vs, sizeof vs); op(OP_BIND_VBUF, &vb2, sizeof vb2);
        op(OP_BIND_UBO, &u0, sizeof u0); op(OP_BIND_UBO, &u1, sizeof u1); op(OP_BIND_TEX, &th, sizeof th); op(OP_VIEWPORTS, &vp, sizeof vp);
        p_scissor top = {0, 0, 64, 32}, bottom = {0, 32, 64, 32};
        op(OP_SCISSOR, &top, sizeof top); op(OP_DRAW, &dw, sizeof dw);
        struct { uint64_t dst, n; float v[4]; } elsewhere = {(uint64_t)(uintptr_t)other, 16, {9, 9, 9, 9}}, upd = {(uint64_t)(uintptr_t)fub, 16, {0.5f, 0.5f, 1, 1}};
        op(OP_UPDATE_UB, &elsewhere, sizeof elsewhere);
        op(OP_SCISSOR, &bottom, sizeof bottom); op(OP_DRAW, &dw, sizeof dw);   /* memo hit: still red 200 */
        op(OP_UPDATE_UB, &upd, sizeof upd);
        p_scissor mid = {0, 48, 64, 16}; op(OP_SCISSOR, &mid, sizeof mid); op(OP_DRAW, &dw, sizeof dw);   /* must see 0.5: red 100 */
        p_scissor all = {0, 0, 64, 64}; op(OP_SCISSOR, &all, sizeof all);
        op(OP_COPY_T2B, &cp, sizeof cp);
        f = run(); mtl_wait(f, 5000000000ull);
        const uint8_t *a = readback + (10 * 64 + 3) * 4, *b = readback + (40 * 64 + 3) * 4, *c = readback + (56 * 64 + 3) * 4;
        CHECK(abs(a[0] - 200) <= 2 && abs(b[0] - 200) <= 2 && abs(c[0] - 100) <= 2, "rename memo across an unrelated update: %d %d %d, want 200 200 100", a[0], b[0], c[0]);
        CHECK(other[0] == 9, "the unrelated update reached memory (%g)", other[0]);
    }

    /* an update earlier in the same submission survives a mid-submission submit (split, CPU step): with a snapshot, the
       bindings after it must still lay it over the snapshot (2026-10-23: sealed entries were taken as "already in the
       snapshot", so draws after a split read stale uniforms) */
    {
        float full[6] = {-1, -1, 3, -1, -1, 3}; float *vtx2 = (float *)(cpu_pool + 0x202000); memcpy(vtx2, full, sizeof full);
        p_bind_vbuf vb2 = vb; vb2.r[0].address = (NVNbufferAddress)(uintptr_t)vtx2;
        fub[0] = 1; fub[1] = 0.5f;
        uint8_t *odd = cpu_pool + 0x20a000;
        op(OP_SET_RT, &srt, sizeof srt); op(OP_BIND_PROGRAM, &bp, sizeof bp); op(OP_BIND_VATTR, &va, sizeof va); op(OP_BIND_VSTREAM, &vs, sizeof vs); op(OP_BIND_VBUF, &vb2, sizeof vb2);
        op(OP_BIND_UBO, &u0, sizeof u0); op(OP_BIND_UBO, &u1, sizeof u1); op(OP_BIND_TEX, &th, sizeof th); op(OP_VIEWPORTS, &vp, sizeof vp);
        p_scissor top = {0, 0, 64, 32}, bottom = {0, 32, 64, 32};
        op(OP_SCISSOR, &top, sizeof top); op(OP_DRAW, &dw, sizeof dw);
        struct { uint64_t dst, n; float v[4]; } upd = {(uint64_t)(uintptr_t)fub, 16, {0.5f, 0.5f, 1, 1}};
        op(OP_UPDATE_UB, &upd, sizeof upd);
        struct { uint64_t dst, n; uint8_t v[8]; } step = {(uint64_t)(uintptr_t)odd, 2, {1, 2}};   /* odd size: a CPU step (a submit mid-replay) */
        op(OP_UPDATE_UB, &step, sizeof step);
        op(OP_SCISSOR, &bottom, sizeof bottom); op(OP_DRAW, &dw, sizeof dw);
        p_scissor all = {0, 0, 64, 64}; op(OP_SCISSOR, &all, sizeof all);
        op(OP_COPY_T2B, &cp, sizeof cp); op(OP_END, NULL, 0);
        uint64_t hrec = (uint64_t)(uintptr_t)rec;
        mtl_sub_begin(); mtl_sub_snapshot_ubos_cap(&hrec, 1, NULL);
        mtl_exec(rec); rn = 0; f = mtl_sub_commit(); mtl_wait(f, 5000000000ull);
        const uint8_t *a = readback + (10 * 64 + 3) * 4, *b = readback + (50 * 64 + 3) * 4;
        CHECK(abs(a[0] - 200) <= 2 && abs(b[0] - 100) <= 2, "an update before a mid-submission submit, with a snapshot: before %d (want 200), after %d (want 100)", a[0], b[0]);
        CHECK(odd[0] == 1 && odd[1] == 2, "the CPU-step update reached memory (%d %d)", odd[0], odd[1]);
    }

    /* uniforms as of the submit: the game rewrites them right after submitting, before the GPU gets to the draw */
    {
        float full[6] = {-1, -1, 3, -1, -1, 3}; float *vtx2 = (float *)(cpu_pool + 0x202000); memcpy(vtx2, full, sizeof full);
        p_bind_vbuf vb2 = vb; vb2.r[0].address = (NVNbufferAddress)(uintptr_t)vtx2;
        fub[0] = 1; fub[1] = 0.5f;
        op(OP_SET_RT, &srt, sizeof srt); op(OP_BIND_PROGRAM, &bp, sizeof bp); op(OP_BIND_VATTR, &va, sizeof va); op(OP_BIND_VSTREAM, &vs, sizeof vs); op(OP_BIND_VBUF, &vb2, sizeof vb2);
        op(OP_BIND_UBO, &u0, sizeof u0); op(OP_BIND_UBO, &u1, sizeof u1); op(OP_BIND_TEX, &th, sizeof th); op(OP_VIEWPORTS, &vp, sizeof vp);
        op(OP_DRAW, &dw, sizeof dw); op(OP_COPY_T2B, &cp, sizeof cp); op(OP_END, NULL, 0);
        uint64_t hrec = (uint64_t)(uintptr_t)rec;
        mtl_sub_begin(); mtl_sub_snapshot_ubos_cap(&hrec, 1, NULL);   /* the submit call */
        fub[0] = 0.25f;                                               /* the game's next frame, before the GPU ran this one */
        mtl_exec(rec); rn = 0; f = mtl_sub_commit(); mtl_wait(f, 5000000000ull);
        const uint8_t *a = readback + (10 * 64 + 3) * 4;
        CHECK(abs(a[0] - 200) <= 2, "uniforms as of the submit: red %d, want 200 (50 = the rewritten value)", a[0]);
    }

    /* a replay of more submissions than the ring has slots (CPU steps, TOTK_VK_SPLIT_MS=-1): its snapshot stays intact
       for the draws after the ring wrapped (2026-10-23: the snapshot lived in one slot's transient memory, which the
       wrap reset and overwrote; a loop count read as 1.0f's bits hung the GPU) */
    {
        float full[6] = {-1, -1, 3, -1, -1, 3}; float *vtx2 = (float *)(cpu_pool + 0x202000); memcpy(vtx2, full, sizeof full);
        p_bind_vbuf vb2 = vb; vb2.r[0].address = (NVNbufferAddress)(uintptr_t)vtx2;
        fub[0] = 1; fub[1] = 0.5f;
        fub[0] = 0.5f;
        uint8_t *odd = cpu_pool + 0x20a000;
        op(OP_SET_RT, &srt, sizeof srt); op(OP_BIND_PROGRAM, &bp, sizeof bp); op(OP_BIND_VATTR, &va, sizeof va); op(OP_BIND_VSTREAM, &vs, sizeof vs); op(OP_BIND_VBUF, &vb2, sizeof vb2);
        op(OP_BIND_UBO, &u0, sizeof u0); op(OP_BIND_UBO, &u1, sizeof u1); op(OP_BIND_TEX, &th, sizeof th); op(OP_VIEWPORTS, &vp, sizeof vp);
        p_scissor top = {0, 0, 64, 32}, bottom = {0, 32, 64, 32};
        op(OP_SCISSOR, &bottom, sizeof bottom);
        for (int i = 0; i < 40; i++) {   /* a draw (its support buffer: transient data) and a CPU step (a submission) each */
            op(OP_DRAW, &dw, sizeof dw);
            struct { uint64_t dst, n; uint8_t v[8]; } step = {(uint64_t)(uintptr_t)odd, 2, {(uint8_t)i, 2}};
            op(OP_UPDATE_UB, &step, sizeof step);
        }
        op(OP_SCISSOR, &top, sizeof top); op(OP_DRAW, &dw, sizeof dw);
        p_scissor all = {0, 0, 64, 64}; op(OP_SCISSOR, &all, sizeof all);
        op(OP_COPY_T2B, &cp, sizeof cp); op(OP_END, NULL, 0);
        uint64_t hrec = (uint64_t)(uintptr_t)rec;
        mtl_sub_begin(); mtl_sub_snapshot_ubos_cap(&hrec, 1, NULL);
        fub[0] = 0.25f;   /* the game's next frame: only the snapshot has 0.5 */
        mtl_exec(rec); rn = 0; f = mtl_sub_commit(); mtl_wait(f, 5000000000ull);
        const uint8_t *a = readback + (10 * 64 + 3) * 4, *b = readback + (50 * 64 + 3) * 4;
        CHECK(abs(a[0] - 100) <= 2 && abs(b[0] - 100) <= 2, "a snapshot read after 40 submissions of one replay: red %d (want 100; 50 = memory, 200 = a support buffer's 1.0 over it), early draws %d", a[0], b[0]);
    }

    /* a compare sampler on a plainly sampled texture (Maxwell ignores the compare outside TEX.DC): bound without it */
    {
        samp_par sc = sp; sc.cmp_mode = 1; sc.cmp_func = 1; mtl_reg_samp(2, &sc);   /* compare NEVER: 0 wherever it applies */
        CHECK(vk_samp_cmp(2) && !vk_samp_cmp(1), "compare flags of the samplers: %d %d", vk_samp_cmp(2), vk_samp_cmp(1));
        CHECK(vk_samp_by_id_nocmp(2) == vk_samp_by_id(1) && vk_samp_by_id(2) != vk_samp_by_id(1), "the compare-free variant is the identical plain sampler");
        float full[6] = {-1, -1, 3, -1, -1, 3}; float *vtx2 = (float *)(cpu_pool + 0x202000); memcpy(vtx2, full, sizeof full);
        p_bind_vbuf vb2 = vb; vb2.r[0].address = (NVNbufferAddress)(uintptr_t)vtx2;
        fub[0] = 1; fub[1] = 0.5f;
        p_bind_handles th2 = th; th2.h[0] = 1 | (2u << 20);
        op(OP_SET_RT, &srt, sizeof srt); op(OP_BIND_PROGRAM, &bp, sizeof bp); op(OP_BIND_VATTR, &va, sizeof va); op(OP_BIND_VSTREAM, &vs, sizeof vs); op(OP_BIND_VBUF, &vb2, sizeof vb2);
        op(OP_BIND_UBO, &u0, sizeof u0); op(OP_BIND_UBO, &u1, sizeof u1); op(OP_BIND_TEX, &th2, sizeof th2); op(OP_VIEWPORTS, &vp, sizeof vp);
        op(OP_DRAW, &dw, sizeof dw); op(OP_COPY_T2B, &cp, sizeof cp);
        f = run(); mtl_wait(f, 5000000000ull);
        const uint8_t *a = readback + (10 * 64 + 3) * 4;
        CHECK(abs(a[0] - 200) <= 2 && abs(a[1] - 50) <= 2, "a compare sampler on a plain texture reads the texels: %d %d, want 200 50", a[0], a[1]);
    }

    /* two texture objects over one CPU-pool storage: what the GPU renders through one is what the other reads
       (the per-frame render-target object and the persistent sampling object of a G-buffer) */
    {
        tex_t ta, tb; memset(&ta, 0, sizeof ta);
        ta.d = tx.d; nvn_tex_layout(&ta.d, &ta.L); ta.mem = cpu_pool + 0x400000; ta.pool = &P_cpu; ta.serial = 201;
        tb = ta; tb.serial = 202;
        memset(ta.mem, 0, ta.L.size);
        p_copy_b2t cb0; memset(&cb0, 0, sizeof cb0); cb0.src = (uint64_t)(uintptr_t)readback; cb0.tex = &tb; cb0.r = (NVNcopyRegion){0, 0, 0, 64, 64, 1};
        op(OP_COPY_T2B, &cb0, sizeof cb0);   /* the sampling object exists first (uploaded from the zero pool bytes) */
        p_set_rt art; memset(&art, 0, sizeof art); art.n = 1; art.color[0] = &ta;
        p_clear_color cr = {0, 0xf, {1, 0, 1, 1}};
        p_set_rt no_rt; memset(&no_rt, 0, sizeof no_rt);
        op(OP_SET_RT, &art, sizeof art); op(OP_CLEAR_COLOR, &cr, sizeof cr); op(OP_SET_RT, &no_rt, sizeof no_rt);
        f = run(); mtl_wait(f, 5000000000ull);
        memset(readback, 0xcd, 64 * 64 * 4);
        op(OP_COPY_T2B, &cb0, sizeof cb0);
        f = run(); mtl_wait(f, 5000000000ull);
        const uint8_t *q = readback + (20 * 64 + 20) * 4;
        CHECK(q[0] == 255 && q[1] == 0 && q[2] == 255, "a GPU-written CPU-pool storage read through another object: %d %d %d, want the magenta clear", q[0], q[1], q[2]);
        /* GPU write-back (nvn_mtl.m wb_note): after an invalidate the CPU sees the rendering in the NVN layout
           (this part of the pool is dirty-tracked as the game's pools are: CPU writes since a rendering are seen) */
        nvn_dirty_add_range(cpu_pool + 0x400000, 0x200000);
        op(OP_SET_RT, &art, sizeof art); op(OP_CLEAR_COLOR, &cr, sizeof cr); op(OP_SET_RT, &no_rt, sizeof no_rt);
        f = run(); mtl_wait(f, 5000000000ull);
        uint8_t lin2[64 * 64 * 4];
        mtl_pool_invalidate(&P_cpu, 0x400000, ta.L.size);
        nvn_deswizzle(&ta.d, 0, 0, ta.mem, lin2, 256);
        q = lin2 + (20 * 64 + 20) * 4;
        CHECK(q[0] == 255 && q[1] == 0 && q[2] == 255 && q[3] == 255, "GPU rendering written back on invalidate: %d %d %d %d, want the magenta clear", q[0], q[1], q[2], q[3]);
        /* a CPU write after the rendering is newer: the next rendering is not written back over it */
        p_clear_color cg = {0, 0xf, {0, 1, 0, 1}};
        op(OP_SET_RT, &art, sizeof art); op(OP_CLEAR_COLOR, &cg, sizeof cg); op(OP_SET_RT, &no_rt, sizeof no_rt);
        f = run(); mtl_wait(f, 5000000000ull);
        ta.mem[ta.L.size - 1] = 7;
        mtl_pool_invalidate(&P_cpu, 0x400000, ta.L.size);
        nvn_deswizzle(&ta.d, 0, 0, ta.mem, lin2, 256);
        q = lin2 + (20 * 64 + 20) * 4;
        CHECK(q[0] == 255 && q[1] == 0 && ta.mem[ta.L.size - 1] == 7, "no write-back over a CPU write since the rendering: %d %d %d, want magenta kept", q[0], q[1], q[2]);
        /* a GPU copy into the memory after the rendering supersedes it */
        op(OP_SET_RT, &art, sizeof art); op(OP_CLEAR_COLOR, &cg, sizeof cg); op(OP_SET_RT, &no_rt, sizeof no_rt);
        memset(cpu_pool + 0x500000, 0x5a, ta.L.size);
        uint64_t cp[3] = {(uint64_t)(uintptr_t)(cpu_pool + 0x500000), (uint64_t)(uintptr_t)ta.mem, ta.L.size};
        op(OP_COPY_B2B, cp, sizeof cp);
        f = run(); mtl_wait(f, 5000000000ull);
        mtl_pool_invalidate(&P_cpu, 0x400000, ta.L.size);
        CHECK(ta.mem[0] == 0x5a && ta.mem[ta.L.size - 1] == 0x5a, "a GPU copy after the rendering is not overwritten by its write-back: %#x %#x", ta.mem[0], ta.mem[ta.L.size - 1]);
        /* a rendering read back long after it was made (TOTK, 2026-10-23: records aged out after 3000 submissions and
           the ground lost its grass): its texture object still holds the image, so the record must not age */
        op(OP_SET_RT, &art, sizeof art); op(OP_CLEAR_COLOR, &cr, sizeof cr); op(OP_SET_RT, &no_rt, sizeof no_rt);
        f = run(); mtl_wait(f, 5000000000ull);
        for (int i = 0; i < 50; i++) { op(OP_SET_RT, &no_rt, sizeof no_rt); f = run(); }
        mtl_wait(f, 5000000000ull);
        mtl_pool_invalidate(&P_cpu, 0x400000, ta.L.size);
        nvn_deswizzle(&ta.d, 0, 0, ta.mem, lin2, 256);
        q = lin2 + (20 * 64 + 20) * 4;
        CHECK(q[0] == 255 && q[1] == 0 && q[2] == 255, "a rendering written back 50 submissions later (records age after 20): %d %d %d, want the magenta clear", q[0], q[1], q[2]);
    }

    /* compressible storage changing format (nvn_mtl.m stor_compressed): the linear depth (R32F) moved away, its old memory
       now an RGBA8 target cleared to white; a stale descriptor of the depth must still read depth, not 0xFFFFFFFF = NaN */
    {
        static pool_t P_cz; P_cz = P_gpu; P_cz.flags |= NVN_MEMORY_POOL_FLAGS_COMPRESSIBLE;
        tex_t zd, zc; memset(&zd, 0, sizeof zd);
        zd.d = tx.d; zd.d.format = 0xA; zd.d.flags = NVN_TEXTURE_FLAGS_COMPRESSIBLE; nvn_tex_layout(&zd.d, &zd.L);
        zd.mem = gpu_pool + 0x600000; zd.pool = &P_cz; zd.serial = 211;
        zc = zd; zc.d.format = 0x25; nvn_tex_layout(&zc.d, &zc.L); zc.serial = 212;
        p_set_rt zr; memset(&zr, 0, sizeof zr); zr.n = 1; zr.color[0] = &zd;
        p_set_rt cr2; memset(&cr2, 0, sizeof cr2); cr2.n = 1; cr2.color[0] = &zc;
        p_set_rt no_rt; memset(&no_rt, 0, sizeof no_rt);
        p_clear_color half = {0, 0xf, {0.5f, 0, 0, 0}}, white = {0, 0xf, {1, 1, 1, 1}};
        op(OP_SET_RT, &zr, sizeof zr); op(OP_CLEAR_COLOR, &half, sizeof half); op(OP_SET_RT, &no_rt, sizeof no_rt);
        op(OP_SET_RT, &cr2, sizeof cr2); op(OP_CLEAR_COLOR, &white, sizeof white); op(OP_SET_RT, &no_rt, sizeof no_rt);
        p_copy_b2t zb; memset(&zb, 0, sizeof zb); zb.src = (uint64_t)(uintptr_t)readback; zb.tex = &zd; zb.r = (NVNcopyRegion){0, 0, 0, 64, 64, 1};
        memset(readback, 0xcd, 64 * 64 * 4);
        op(OP_COPY_T2B, &zb, sizeof zb);
        f = run(); mtl_wait(f, 5000000000ull);
        float z; memcpy(&z, readback + (30 * 64 + 30) * 4, 4);
        CHECK(z == 0.5f, "compressible R32F storage taken by an RGBA8 target: the old depth object reads %g (bits %08x), want 0.5", z, *(uint32_t *)(readback + (30 * 64 + 30) * 4));
        zb.tex = &zc; memset(readback, 0xcd, 64 * 64 * 4);
        op(OP_COPY_T2B, &zb, sizeof zb);
        f = run(); mtl_wait(f, 5000000000ull);
        const uint8_t *w = readback + (30 * 64 + 30) * 4;
        CHECK(w[0] == 255 && w[1] == 255 && w[2] == 255 && w[3] == 255, "the RGBA8 target on that storage: %d %d %d %d, want white", w[0], w[1], w[2], w[3]);
    }

    p_set_rt none_rt0; memset(&none_rt0, 0, sizeof none_rt0);
    /* conditional rendering in GPU order: reports recorded in this submission decide a conditional in it (their memory
       is only written when it completes); samples passed counts the draws between reports ("everything visible") */
    {
        uint64_t *cnt = (uint64_t *)(cpu_pool + 0x300000); memset(cnt, 0x55, 64);   /* stale memory: would say "not equal" */
        uint64_t rs = NVN_COUNTER_TYPE_SAMPLES_PASSED, rep0[2] = {rs, (uint64_t)(uintptr_t)cnt}, rep1[2] = {rs, (uint64_t)(uintptr_t)(cnt + 2)};
        p_render_cond eq = {0, 0, (uint64_t)(uintptr_t)cnt}; p_flag on = {1};
        p_clear_color red = {0, 0xf, {1, 0, 0, 1}}, blue = {0, 0xf, {0, 0, 1, 1}};
        float full[6] = {-1, -1, 3, -1, -1, 3}; float *vtx2 = (float *)(cpu_pool + 0x202000); memcpy(vtx2, full, sizeof full);
        p_bind_vbuf vb2 = vb; vb2.r[0].address = (NVNbufferAddress)(uintptr_t)vtx2;
        /* 1: no draw between the reports -> equal -> the red clear runs */
        op(OP_RESET_COUNTER, &rs, 8); op(OP_REPORT_COUNTER, rep0, 16); op(OP_REPORT_COUNTER, rep1, 16);
        op(OP_SET_RT, &srt, sizeof srt); op(OP_CLEAR_COLOR, &blue, sizeof blue); op(OP_SET_RT, &none_rt0, sizeof none_rt0);
        op(OP_RENDER_COND, &eq, sizeof eq); op(OP_SET_RT, &srt, sizeof srt); op(OP_CLEAR_COLOR, &red, sizeof red); op(OP_SET_RT, &none_rt0, sizeof none_rt0); op(OP_RENDER_ENABLE, &on, sizeof on);
        op(OP_COPY_T2B, &cp, sizeof cp); f = run(); mtl_wait(f, 5000000000ull);
        const uint8_t *q = readback + (20 * 64 + 20) * 4;
        CHECK(q[0] == 255 && q[2] == 0, "conditional clear after two reports with no draw between (equal): %d %d %d, want red", q[0], q[1], q[2]);
        /* 2: a draw between the reports -> not equal -> the red clear is skipped */
        memset(cnt, 0x55, 64);
        op(OP_SET_RT, &srt, sizeof srt); op(OP_CLEAR_COLOR, &blue, sizeof blue);
        op(OP_RESET_COUNTER, &rs, 8); op(OP_REPORT_COUNTER, rep0, 16);
        op(OP_BIND_PROGRAM, &bp, sizeof bp); op(OP_BIND_VATTR, &va, sizeof va); op(OP_BIND_VSTREAM, &vs, sizeof vs); op(OP_BIND_VBUF, &vb2, sizeof vb2);
        op(OP_BIND_UBO, &u0, sizeof u0); op(OP_BIND_UBO, &u1, sizeof u1); op(OP_BIND_TEX, &th, sizeof th); op(OP_VIEWPORTS, &vp, sizeof vp);
        p_scissor corner = {0, 0, 4, 4}, all = {0, 0, 64, 64}; op(OP_SCISSOR, &corner, sizeof corner); op(OP_DRAW, &dw, sizeof dw); op(OP_SCISSOR, &all, sizeof all);
        op(OP_REPORT_COUNTER, rep1, 16); op(OP_SET_RT, &none_rt0, sizeof none_rt0);
        op(OP_RENDER_COND, &eq, sizeof eq); op(OP_SET_RT, &srt, sizeof srt); op(OP_CLEAR_COLOR, &red, sizeof red); op(OP_SET_RT, &none_rt0, sizeof none_rt0); op(OP_RENDER_ENABLE, &on, sizeof on);
        op(OP_COPY_T2B, &cp, sizeof cp); f = run(); mtl_wait(f, 5000000000ull);
        CHECK(q[0] == 0 && q[2] == 255, "conditional clear after a draw between the reports (not equal): %d %d %d, want blue", q[0], q[1], q[2]);
        CHECK(cnt[0] == 0 && cnt[2] > 0, "reported samples: %llu then %llu (want 0, then the draw's)", (unsigned long long)cnt[0], (unsigned long long)cnt[2]);
    }

    /* a clear with no draw after it, then other targets: the clear still happens */
    p_clear_color cg = {0, 0xf, {0, 1, 0, 1}};
    op(OP_SET_RT, &srt, sizeof srt); op(OP_CLEAR_COLOR, &cg, sizeof cg);
    p_set_rt none; memset(&none, 0, sizeof none); op(OP_SET_RT, &none, sizeof none);
    op(OP_COPY_T2B, &cp, sizeof cp);
    f = run(); mtl_wait(f, 5000000000ull);
    p = readback + (40 * 64 + 9) * 4;
    CHECK(p[0] == 0 && p[1] == 255 && p[2] == 0 && p[3] == 255, "a clear without a draw: (9,40) = %d %d %d %d, want 0 255 0 255", p[0], p[1], p[2], p[3]);

    /* rendering into a BC4 texture through an RGBA16UI view whose texel is one block (TOTK) */
    tex_t bc; memset(&bc, 0, sizeof bc);
    bc.d.target = 1; bc.d.format = 0x49; bc.d.width = 64; bc.d.height = 64; bc.d.depth = 1; bc.d.levels = 1;
    nvn_tex_layout(&bc.d, &bc.L); bc.mem = gpu_pool + 0x100000; bc.pool = &P_gpu; bc.serial = 103;
    p_set_rt brt; memset(&brt, 0, sizeof brt); brt.n = 1; brt.color[0] = &bc; brt.has_cview[0] = 1;
    brt.cview[0].set_format = 1; brt.cview[0].format = 0x2c;
    p_clear_color cu = {0, 0xf, {0x1111, 0x2222, 0x3333, 0x4444}};
    op(OP_SET_RT, &brt, sizeof brt); op(OP_CLEAR_COLOR, &cu, sizeof cu);
    p_copy_b2t cb4; memset(&cb4, 0, sizeof cb4); cb4.src = (uint64_t)(uintptr_t)readback; cb4.tex = &bc; cb4.r = (NVNcopyRegion){0, 0, 0, 64, 64, 1};
    memset(readback, 0xcd, 16 * 16 * 8);
    op(OP_COPY_T2B, &cb4, sizeof cb4);
    f = run(); mtl_wait(f, 5000000000ull);
    const uint16_t *blk = (const uint16_t *)(readback + (7 * 16 + 5) * 8);
    CHECK(blk[0] == 0x1111 && blk[1] == 0x2222 && blk[2] == 0x3333 && blk[3] == 0x4444, "BC4 block (5,7) written through the RGBA16UI view = %04x %04x %04x %04x", blk[0], blk[1], blk[2], blk[3]);
    /* the same in a CPU pool, rendered and invalidated: the blocks reach memory (TOTK's terrain bakes) */
    {
        tex_t bcc = bc; bcc.mem = cpu_pool + 0x600000; bcc.pool = &P_cpu; bcc.serial = 105;
        nvn_dirty_add_range(cpu_pool + 0x600000, 0x100000);
        memset(bcc.mem, 0, bcc.L.size);
        p_set_rt crt = brt; crt.color[0] = &bcc;
        p_set_rt no_rt2; memset(&no_rt2, 0, sizeof no_rt2);
        op(OP_SET_RT, &crt, sizeof crt); op(OP_CLEAR_COLOR, &cu, sizeof cu); op(OP_SET_RT, &no_rt2, sizeof no_rt2);
        f = run(); mtl_wait(f, 5000000000ull);
        mtl_pool_invalidate(&P_cpu, 0x600000, bcc.L.size);
        static uint8_t lin4[16 * 16 * 8];
        nvn_deswizzle(&bcc.d, 0, 0, bcc.mem, lin4, 16 * 8);
        const uint16_t *b2 = (const uint16_t *)(lin4 + (7 * 16 + 5) * 8);
        CHECK(b2[0] == 0x1111 && b2[1] == 0x2222 && b2[2] == 0x3333 && b2[3] == 0x4444, "CPU-pool BC4 rendering written back on invalidate: block (5,7) = %04x %04x %04x %04x", b2[0], b2[1], b2[2], b2[3]);
    }

    /* a render target in one depth slice of a 3D texture */
    tex_t v3; memset(&v3, 0, sizeof v3);
    v3.d.target = 2; v3.d.format = 0x25; v3.d.width = 16; v3.d.height = 16; v3.d.depth = 4; v3.d.levels = 1;
    nvn_tex_layout(&v3.d, &v3.L); v3.mem = gpu_pool + 0x200000; v3.pool = &P_gpu; v3.serial = 104;
    p_set_rt vrt; memset(&vrt, 0, sizeof vrt); vrt.n = 1; vrt.color[0] = &v3; vrt.has_cview[0] = 1;
    vrt.cview[0].set_layers = 1; vrt.cview[0].min_layer = 2; vrt.cview[0].nlayers = 1;
    p_clear_color cz = {0, 0xf, {1, 0, 0, 1}}, cz0 = {0, 0xf, {0, 0, 0, 0}};
    p_set_rt vrt0 = vrt; vrt0.cview[0].set_layers = 0;
    vrt0.has_cview[0] = 1; vrt0.cview[0].set_layers = 1; vrt0.cview[0].min_layer = 0; vrt0.cview[0].nlayers = 4;
    op(OP_SET_RT, &vrt0, sizeof vrt0); op(OP_CLEAR_COLOR, &cz0, sizeof cz0);
    op(OP_SET_RT, &vrt, sizeof vrt); op(OP_CLEAR_COLOR, &cz, sizeof cz);
    p_copy_b2t c3; memset(&c3, 0, sizeof c3); c3.src = (uint64_t)(uintptr_t)readback; c3.tex = &v3; c3.r = (NVNcopyRegion){0, 0, 0, 16, 16, 4};
    memset(readback, 0xcd, 16 * 16 * 4 * 4);
    op(OP_COPY_T2B, &c3, sizeof c3);
    f = run(); mtl_wait(f, 5000000000ull);
    const uint8_t *s1 = readback + 1 * 16 * 16 * 4 + (3 * 16 + 3) * 4, *s2 = readback + 2 * 16 * 16 * 4 + (3 * 16 + 3) * 4;
    CHECK(s2[0] == 255 && s2[1] == 0 && s2[3] == 255 && !s1[0] && !s1[3], "3D slice 2 as a render target: slice 2 = %d %d %d %d, slice 1 = %d %d %d %d", s2[0], s2[1], s2[2], s2[3], s1[0], s1[1], s1[2], s1[3]);

#ifdef TOTK_ANDROID_APP
    /* the app's presenter: the window crop of the presented texture reaches the window, row by row */
    {
        extern ANativeWindow *mock_window(void); extern uint8_t *mock_window_pixels; extern int32_t mock_window_w, mock_window_h, mock_window_stride; extern int mock_window_posts;
        void vk_window_set(ANativeWindow *w);
        setenv("TOTK_APP_WIDTH", "96", 1);   /* the window picture's width: the mock window's (96x32) */
        vk_window_set(mock_window());
        int crop[4] = {8, 4, 32, 16};
        mtl_present(&rt, 1.0 / 60, crop);
        for (int i = 0; i < 200 && !mock_window_posts; i++) usleep(10000);
        CHECK(mock_window_posts == 1 && mock_window_w == 96 && mock_window_h == 32, "a frame posted to the window at TOTK_APP_WIDTH and the window's shape (%d posts, %dx%d)", mock_window_posts, mock_window_w, mock_window_h);
        /* the 32x16 crop scaled 2x into the middle 64x32 of the 96x32 window, black bars left and right; rt was last cleared green */
        const uint8_t *in = mock_window_pixels ? mock_window_pixels + ((size_t)10 * mock_window_stride + 48) * 4 : NULL, *bar = mock_window_pixels ? mock_window_pixels + ((size_t)10 * mock_window_stride + 5) * 4 : NULL;
        CHECK(in && in[0] == 0 && in[1] == 255 && in[2] == 0, "window middle: %d %d %d, want the green clear", in ? in[0] : -1, in ? in[1] : -1, in ? in[2] : -1);
        CHECK(bar && bar[0] == 0 && bar[1] == 0 && bar[2] == 0, "window bar: %d %d %d, want black", bar ? bar[0] : -1, bar ? bar[1] : -1, bar ? bar[2] : -1);
        /* the pass probe (TOTK_VK_PASSPROBE=1, set at the start): a green frame then a black one differ in their one pass */
        int vk_probe_report(uint64_t flash, uint64_t before); uint64_t vk_present_count(void);
        p_clear_color green = {0, 0xf, {0, 1, 0, 1}}, black = {0, 0xf, {0, 0, 0, 1}};
        op(OP_SET_RT, &srt, sizeof srt); op(OP_CLEAR_COLOR, &green, sizeof green); op(OP_SET_RT, &none, sizeof none); f = run();
        mtl_present(&rt, 1.0 / 60, crop); uint64_t fa = vk_present_count();
        op(OP_SET_RT, &srt, sizeof srt); op(OP_CLEAR_COLOR, &black, sizeof black); op(OP_SET_RT, &none, sizeof none); f = run();
        mtl_present(&rt, 1.0 / 60, crop); uint64_t fb = vk_present_count();
        mtl_wait(mtl_last_fence(), 5000000000ull);
        CHECK(vk_probe_report(fb, fa) == 1, "pass probe: the black frame's one pass differs from the green frame's");
        CHECK(vk_probe_report(fa, fa) == 0, "pass probe: a frame matches itself");
        /* a draw whose uniform differs between two frames: the report names the uniform (look for "uniform 1" ... "differs") */
        float full[6] = {-1, -1, 3, -1, -1, 3}; float *vtx2 = (float *)(cpu_pool + 0x202000); memcpy(vtx2, full, sizeof full);
        p_bind_vbuf vb2 = vb; vb2.r[0].address = (NVNbufferAddress)(uintptr_t)vtx2;
        for (int k = 0; k < 2; k++) {
            fub[0] = k ? 0.1f : 0.9f;
            op(OP_SET_RT, &srt, sizeof srt); op(OP_BIND_PROGRAM, &bp, sizeof bp); op(OP_BIND_VATTR, &va, sizeof va); op(OP_BIND_VSTREAM, &vs, sizeof vs); op(OP_BIND_VBUF, &vb2, sizeof vb2);
            op(OP_BIND_UBO, &u0, sizeof u0); op(OP_BIND_UBO, &u1, sizeof u1); op(OP_BIND_TEX, &th, sizeof th); op(OP_VIEWPORTS, &vp, sizeof vp);
            op(OP_DRAW, &dw, sizeof dw); op(OP_SET_RT, &none, sizeof none); f = run(); mtl_wait(f, 5000000000ull);
            mtl_present(&rt, 1.0 / 60, crop); if (k) fb = vk_present_count(); else fa = vk_present_count();
        }
        mtl_wait(mtl_last_fence(), 5000000000ull);
        CHECK(vk_probe_report(fb, fa) == 1, "pass probe: a draw whose uniform changed differs");
    }
#endif
    /* "passdump 1": the next frame but one, each pass's targets as PNG with an index */
    {
        void dm_gfx_passdump_next(long n); uint64_t vk_present_count(void);
        int crop0[4] = {0, 0, 0, 0};
        dm_gfx_passdump_next(1); uint64_t target = vk_present_count() + 2;
        mtl_present(&rt, 1.0 / 60, crop0);
        p_set_rt prt; memset(&prt, 0, sizeof prt); prt.n = 1; prt.color[0] = &rt; p_set_rt pnone; memset(&pnone, 0, sizeof pnone);
        p_clear_color pc = {0, 0xf, {0.25f, 0.5f, 1, 1}};
        op(OP_SET_RT, &prt, sizeof prt); op(OP_CLEAR_COLOR, &pc, sizeof pc); op(OP_SET_RT, &pnone, sizeof pnone); f = run();
        mtl_present(&rt, 1.0 / 60, crop0);
        char path[128]; snprintf(path, sizeof path, "passdump/f%llu/p000_c0_64x64.png", (unsigned long long)target);
        FILE *pf = fopen(path, "rb"); uint8_t hdr[8] = {0}; if (pf) { fread(hdr, 1, 8, pf); fclose(pf); }
        CHECK(pf && hdr[1] == 'P', "passdump wrote %s", path);
        snprintf(path, sizeof path, "passdump/f%llu/index.txt", (unsigned long long)target);
        pf = fopen(path, "r"); char idx[512] = {0}; if (pf) { fread(idx, 1, sizeof idx - 1, pf); fclose(pf); }
        CHECK(strstr(idx, "pass 0") && strstr(idx, "c0: ser 102 64x64"), "passdump index: %.200s", idx);
    }
    /* more descriptor sets in a submission than one pool holds, many times: the slots' pools are reused, not added to */
    {
        int vk_dpool_count(void);
        VkDescriptorSetLayoutBinding lb = {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_ALL, NULL};
        VkDescriptorSetLayoutCreateInfo li = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 1, .pBindings = &lb};
        VkDescriptorSetLayout dsl; vkCreateDescriptorSetLayout(V.dev, &li, NULL, &dsl);
        int after_first = 0;
        for (int round = 0; round < 100; round++) {   /* more rounds than ring slots, twice: every slot has been through once by round 50 */
            mtl_sub_begin(); for (int i = 0; i < 2500; i++) (void)vk_alloc_set(dsl); f = mtl_sub_commit(); mtl_wait(f, 5000000000ull);
            if (round == 49) after_first = vk_dpool_count();
        }
        CHECK(vk_dpool_count() == after_first, "descriptor pools reused across submissions: %d after 50 rounds, %d after 100", after_first, vk_dpool_count());
    }
    /* the pipeline cache reaches disk (loaded at the next start; TOTK_VK_PCACHE) */
    { remove("pipeline_cache.bin"); vk_pcache_save_async(); FILE *pf = NULL;
      for (int i = 0; i < 200 && !(pf = fopen("pipeline_cache.bin", "rb")); i++) usleep(10000);
      long l = 0; if (pf) { fseek(pf, 0, SEEK_END); l = ftell(pf); fclose(pf); }
      CHECK(l > 0, "pipeline cache saved (%ld bytes)", l); }
    { void vk_mem_report(void); vk_mem_report(); }   /* the memory line runs */
    printf("%s: %d checks, %d failed\n", fails ? "FAILED" : "ok", checks, fails);
    return fails != 0;
}
