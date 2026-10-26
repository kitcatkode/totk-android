/* Shaders of the Vulkan backend (vk_int.h): the shader seed on the device, variants per bind state, SPIR-V modules,
   descriptor set layouts, pipeline layouts and compute pipelines.

   The seed (android/build_android.py --shaders) holds every shader of the game translated on the PC for its default
   bind state and for every bind state of the shared shader list, as on the iPhone (no translator on the device):
     <variant>.spv   Vulkan SPIR-V from the Ryujinx translator (tools/totk_xlate)
     <variant>.vk    its Vulkan reflection (android/vk_reflect.py)
     <key>.vv        the shader's variants with the answers each was translated for
   A shader is identified as on the Mac: SHA-256 of its control section (ctl_key_len) and code. A variant is chosen by
   the same answers (texture slot types, image formats, vertex input and colour output classes) and the same per-stage
   signature as nvn_mtl.m prog_fit / stage_sig. A variant the seed lacks skips its draws (as on the iPhone) and is
   recorded once under shader-requests/ for a later fill on the PC.

   Every stage of a translation uses descriptor set 0; the vertex (or compute) stage keeps it and the fragment stage's
   SPIR-V is patched to set 1, so one pipeline layout holds both. */
#include "vk_int.h"
#include "nvn_hmap.h"
#include <errno.h>
#include <sys/stat.h>

#include "host_os.h"
int totk_depth_near_minus_w(void) __attribute__((weak));

static const char *seed_dir(void) {
    static const char *d; static int init;
    if (!init) { init = 1; d = getenv("TOTK_SHADER_SEED_DIR"); if (!d || !*d) d = "shaders"; }
    return d;
}
static int g_seed_ok = -1;
int vk_shaders_ready(void) {
    if (g_seed_ok < 0) { struct stat st; g_seed_ok = !stat(seed_dir(), &st) && S_ISDIR(st.st_mode);
        if (!g_seed_ok) LOGV("【红】no shader seed at %s (build_android.py --shaders, then sh push.sh shaders): nothing is drawn", seed_dir());
        else LOGV("shader seed: %s", seed_dir()); }
    return g_seed_ok;
}
static char *read_file(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb"); if (!f) return NULL;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    char *b = malloc((size_t)n + 1); if (!b) { fclose(f); return NULL; }
    if (fread(b, 1, (size_t)n, f) != (size_t)n) { free(b); fclose(f); return NULL; }
    fclose(f); b[n] = 0; if (len) *len = (size_t)n; return b;
}

/* ---------- answers (nvn_mtl.m rt_answers) as flat "kind:slot=value" items ---------- */
static const char *target_name(int t) {
    switch (t) { case 0: return "1d"; case 2: return "3d"; case 3: return "1darray"; case 4: return "2darray"; case 5: return "2dms";
                 case 6: return "2dmsarray"; case 8: return "cube"; case 9: return "cubearray"; case 10: return "buffer"; default: return "2d"; }
}
static const char *image_format(int f) {
    switch (f) {
        case 0x01: return "R8Unorm"; case 0x0D: return "R8G8Unorm"; case 0x25: return "R8G8B8A8Unorm"; case 0x27: return "R8G8B8A8Uint";
        case 0x05: return "R16Float"; case 0x11: return "R16G16Float"; case 0x29: return "R16G16B16A16Float"; case 0x08: return "R16Uint"; case 0x2C: return "R16G16B16A16Uint";
        case 0x0A: return "R32Float"; case 0x16: return "R32G32Float"; case 0x2E: return "R32G32B32A32Float"; case 0x0B: return "R32Uint"; case 0x0C: return "R32Sint";
        case 0x17: return "R32G32Uint"; case 0x2F: return "R32G32B32A32Uint"; case 0x3D: return "R10G10B10A2Unorm"; case 0x3F: return "R11G11B10Float";
    }
    return NULL;
}
static const char *class_name(int f) { int c = vk_fmt_class(f); return c == 1 ? "uint" : c == 2 ? "sint" : "float"; }
typedef struct { char tex[128][24]; char img[32][48]; char attr[16][8]; char fout[8][8]; uint8_t has_tex[128], has_img[32], has_attr[16], has_fout[8]; } answers_t;
static int bound_target(const vk_treg *r) { return r->has_v && r->v.set_target ? r->v.target : r->t.d.target; }
static void answers_of(int st, answers_t *a) {
    memset(a, 0, sizeof *a);
    for (int r = 0; r < 128; r++) if (S.tex[st][r]) {
        vk_treg tr_, *t = &tr_; if (!vk_treg_get((int)(S.tex[st][r] & 0xfffff), t)) continue;
        int f = t->has_v && t->v.set_format ? t->v.format : t->t.d.format;
        snprintf(a->tex[r], sizeof a->tex[r], "%s%s", target_name(bound_target(t)), f >= 0x32 && f <= 0x36 ? "_depth" : ""); a->has_tex[r] = 1;
    }
    for (int r = 0; r < 32; r++) if (S.img[st][r]) {
        vk_treg tr_, *t = &tr_; if (!vk_treg_get((int)(S.img[st][r] & 0xfffff), t)) continue;
        int f = t->has_v && t->v.set_format ? t->v.format : t->t.d.format; const char *fn = image_format(f);
        snprintf(a->img[r], sizeof a->img[r], "%s%s%s", target_name(bound_target(t)), fn ? "/" : "", fn ? fn : ""); a->has_img[r] = 1;
    }
    if (st == 0) for (int i = 0; i < S.nattr && i < 16; i++) { snprintf(a->attr[i], sizeof a->attr[i], "%s", class_name(S.attr[i].fmt)); a->has_attr[i] = 1; }
    if (st == 1 && S.has_rt) for (int i = 0; i < S.rt.n && i < 8; i++) if (S.rt.color[i]) {
        int f = S.rt.has_cview[i] && S.rt.cview[i].set_format ? S.rt.cview[i].format : S.rt.color[i]->d.format;
        snprintf(a->fout[i], sizeof a->fout[i], "%s", class_name(f)); a->has_fout[i] = 1;
    }
}
/* one flat dependency "kind:slot=value" against the answers (deps_match: a missing answer is the default) */
static int dep_ok(const answers_t *a, const char *kind, int slot, const char *want) {
    const char *have;
    if (!strcmp(kind, "tex")) have = slot >= 0 && slot < 128 && a->has_tex[slot] ? a->tex[slot] : "2d";
    else if (!strcmp(kind, "img")) have = slot >= 0 && slot < 32 && a->has_img[slot] ? a->img[slot] : "";
    else if (!strcmp(kind, "attr")) have = slot >= 0 && slot < 16 && a->has_attr[slot] ? a->attr[slot] : "float";
    else if (!strcmp(kind, "fout")) have = slot >= 0 && slot < 8 && a->has_fout[slot] ? a->fout[slot] : "float";
    else return 0;
    return !strcmp(have, want);
}
static int deps_match(const char *deps, const answers_t *a) {
    char buf[4096]; snprintf(buf, sizeof buf, "%s", deps);
    for (char *save = NULL, *it = strtok_r(buf, ";", &save); it; it = strtok_r(NULL, ";", &save)) {
        char *colon = strchr(it, ':'), *eq = colon ? strchr(colon, '=') : NULL;
        if (!colon || !eq) return 0;
        *colon = 0; *eq = 0;
        if (!dep_ok(a, it, atoi(colon + 1), eq + 1)) return 0;
    }
    return 1;
}

/* ---------- shaders ---------- */
static nvn_hmap g_shaders = {.klen = 80};   /* variant name -> vk_shader */
static VkShaderStageFlags stage_flags(int st) { return st == 0 ? VK_SHADER_STAGE_VERTEX_BIT : st == 1 ? VK_SHADER_STAGE_FRAGMENT_BIT : VK_SHADER_STAGE_COMPUTE_BIT; }
static void patch_set(uint32_t *w, size_t nw, uint32_t set) {
    for (size_t i = 5; i < nw; ) {
        uint32_t op = w[i] & 0xffff, n = w[i] >> 16; if (!n) break;
        if (op == 71 && n == 4 && w[i + 2] == 34) w[i + 3] = set;   /* OpDecorate <id> DescriptorSet <set> */
        i += n;
    }
}
/* Storage bindings the shader writes (TOTK, 2026-10-22). The reflection's rw was never set for storage buffers (the
   translator tested the wrong flag bit; nvn_mtl.m msl_stores_through found the same on the Mac), so every storage
   buffer counted as read-only: no "GPU writes here" mark, and uniform snapshots taken at submit kept the CPU's older
   bytes of buffers the GPU writes. The material shaders store a per-tile class flag that the next frame's class marker
   reads to decide which lighting draws light a pixel. Here the SPIR-V itself is read: a binding is rw when an OpStore,
   OpCopyMemory target, atomic or function-call argument reaches it through access chains. */
static int spirv_mark_stores(vk_shader *sh, const uint32_t *w, size_t nw) {
    if (nw < 5 || w[0] != 0x07230203) return 0;
    uint32_t bound = w[3]; if (!bound || bound > (1u << 22)) return 0;
    int32_t *bind = malloc(bound * sizeof *bind); uint32_t *root = malloc(bound * sizeof *root); uint8_t *isbuf = calloc(bound, 1), *stored = calloc(bound, 1);
    if (!bind || !root || !isbuf || !stored) { free(bind); free(root); free(isbuf); free(stored); return 0; }
    for (uint32_t i = 0; i < bound; i++) { bind[i] = -1; root[i] = 0; }
    #define ID_OK(x) ((x) && (x) < bound)
    for (size_t i = 5; i < nw; ) {
        uint32_t op = w[i] & 0xffff, n = w[i] >> 16; if (!n || i + n > nw) break;
        const uint32_t *a = w + i + 1;
        switch (op) {
            case 71: if (n >= 4 && a[1] == 33 && ID_OK(a[0])) bind[a[0]] = (int32_t)a[2]; break;   /* OpDecorate Binding */
            case 59: if (n >= 4 && ID_OK(a[1]) && (a[2] == 12 || a[2] == 2)) { isbuf[a[1]] = 1; root[a[1]] = a[1]; } break;   /* OpVariable StorageBuffer / Uniform */
            case 65: case 66: case 67: case 70: case 83: case 124:   /* access chains, OpCopyObject, OpBitcast */
                if (n >= 4 && ID_OK(a[1]) && ID_OK(a[2]) && root[a[2]]) root[a[1]] = root[a[2]]; break;
            case 62: case 63: if (n >= 3 && ID_OK(a[0]) && root[a[0]]) stored[root[a[0]]] = 1; break;   /* OpStore, OpCopyMemory target */
            case 228: if (n >= 2 && ID_OK(a[0]) && root[a[0]]) stored[root[a[0]]] = 1; break;   /* OpAtomicStore */
            case 57: for (uint32_t k = 3; k + 1 < n; k++) if (ID_OK(a[k]) && root[a[k]]) stored[root[a[k]]] = 1; break;   /* OpFunctionCall: a pointer argument may be written */
            default:
                if (((op >= 229 && op <= 242) || op == 6035 || op == 5614 || op == 5615) && n >= 4 && ID_OK(a[2]) && root[a[2]]) stored[root[a[2]]] = 1;   /* atomics */
                break;
        }
        i += n;
    }
    #undef ID_OK
    int marked = 0;
    for (uint32_t v = 1; v < bound; v++) if (isbuf[v] && stored[v] && bind[v] >= 0)
        for (int k = 0; k < sh->nb; k++) if (sh->b[k].binding == (uint16_t)bind[v] && !sh->b[k].rw && (sh->b[k].dtype == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER || sh->b[k].dtype == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC)) { sh->b[k].rw = 1; marked++; }
    free(bind); free(root); free(isbuf); free(stored);
    return marked;
}
/* Texture bindings the shader samples as shadow (depth-compare) textures: the variable's OpTypeImage has Depth = 1
   (Ryujinx declares it from SamplerType.Shadow, i.e. a TEX.DC / TLD4.DC instruction). vk_draw.c uses it to keep
   compare samplers off textures the shader reads plainly (Maxwell ignores the sampler's compare there; Vulkan does not). */
static int spirv_mark_shadow(vk_shader *sh, const uint32_t *w, size_t nw) {
    if (nw < 5 || w[0] != 0x07230203) return 0;
    uint32_t bound = w[3]; if (!bound || bound > (1u << 22)) return 0;
    int32_t *bind = malloc(bound * sizeof *bind); uint8_t *dep = calloc(bound, 1), *var = calloc(bound, 1);
    if (!bind || !dep || !var) { free(bind); free(dep); free(var); return 0; }
    for (uint32_t i = 0; i < bound; i++) bind[i] = -1;
    #define ID_OK(x) ((x) && (x) < bound)
    for (size_t i = 5; i < nw; ) {
        uint32_t op = w[i] & 0xffff, n = w[i] >> 16; if (!n || i + n > nw) break;
        const uint32_t *a = w + i + 1;
        switch (op) {
            case 71: if (n >= 4 && a[1] == 33 && ID_OK(a[0])) bind[a[0]] = (int32_t)a[2]; break;   /* OpDecorate Binding */
            case 25: if (n >= 5 && ID_OK(a[0])) dep[a[0]] = a[3] == 1; break;   /* OpTypeImage: Depth operand */
            case 27: case 28: case 29: if (n >= 3 && ID_OK(a[0]) && ID_OK(a[1])) dep[a[0]] = dep[a[1]]; break;   /* SampledImage, Array, RuntimeArray */
            case 32: if (n >= 4 && ID_OK(a[0]) && ID_OK(a[2])) dep[a[0]] = dep[a[2]]; break;   /* OpTypePointer */
            case 59: if (n >= 4 && ID_OK(a[0]) && ID_OK(a[1]) && a[2] == 0) var[a[1]] = dep[a[0]]; break;   /* OpVariable UniformConstant */
        }
        i += n;
    }
    #undef ID_OK
    int marked = 0;
    for (uint32_t v = 1; v < bound; v++) if (var[v] && bind[v] >= 0)
        for (int k = 0; k < sh->nb; k++) if (sh->b[k].binding == (uint16_t)bind[v] && !sh->b[k].shadow) { sh->b[k].shadow = 1; marked++; }
    free(bind); free(dep); free(var);
    return marked;
}
/* Uniform buffers the shader reads only at constant indices: the bytes it can read from the start (vk_bind.used).
   A binding of the whole 64 KiB block for a shader reading a few vec4s made every renamed copy (an inline update
   overlapping the block) 64 KiB: ~2.9 MB of memcpy a frame on the replay thread (2026-10-23). The translator declares
   a uniform block as { vec4 data[4096] } (ArrayStride 16) and reads it as AccessChain(block, 0, index[, component]);
   any other use of the block, a non-constant index or another stride leaves the binding whole. */
static int spirv_ubo_extent(vk_shader *sh, const uint32_t *w, size_t nw) {
    if (nw < 5 || w[0] != 0x07230203) return 0;
    uint32_t bound = w[3]; if (!bound || bound > (1u << 22)) return 0;
    int32_t *bind = malloc(bound * sizeof *bind); uint32_t *cval = malloc(bound * sizeof *cval), *maxi = calloc(bound, sizeof *maxi);
    uint8_t *isc = calloc(bound, 1), *ubo = calloc(bound, 1), *bad = calloc(bound, 1), *s16 = calloc(bound, 1);
    if (!bind || !cval || !maxi || !isc || !ubo || !bad || !s16) { free(bind); free(cval); free(maxi); free(isc); free(ubo); free(bad); free(s16); return 0; }
    for (uint32_t i = 0; i < bound; i++) bind[i] = -1;
    #define ID_OK(x) ((x) && (x) < bound)
    int other_stride = 0;
    for (size_t i = 5; i < nw; ) {
        uint32_t op = w[i] & 0xffff, n = w[i] >> 16; if (!n || i + n > nw) break;
        const uint32_t *a = w + i + 1;
        switch (op) {
            case 71:   /* OpDecorate: Binding, ArrayStride */
                if (n >= 4 && a[1] == 33 && ID_OK(a[0])) bind[a[0]] = (int32_t)a[2];
                if (n >= 4 && a[1] == 6 && ID_OK(a[0])) { if (a[2] == 16) s16[a[0]] = 1; else other_stride = 1; }
                break;
            case 43: if (n >= 4 && ID_OK(a[1])) { isc[a[1]] = 1; cval[a[1]] = a[2]; } break;   /* OpConstant (32-bit) */
            case 59: if (n >= 4 && ID_OK(a[1]) && a[2] == 2) ubo[a[1]] = 1; break;   /* OpVariable Uniform */
            case 65: case 66:   /* OpAccessChain / InBounds: (block, 0, index, ...) */
                if (n >= 4 && ID_OK(a[2]) && ubo[a[2]]) {
                    if (n < 6 || !ID_OK(a[4]) || !isc[a[4]] || cval[a[4]] >= 4096) bad[a[2]] = 1;
                    else if (cval[a[4]] + 1 > maxi[a[2]]) maxi[a[2]] = cval[a[4]] + 1;
                }
                break;
            case 61: case 83: case 124: if (n >= 4 && ID_OK(a[2]) && ubo[a[2]]) bad[a[2]] = 1; break;   /* OpLoad, OpCopyObject, OpBitcast of the block */
            case 62: if (n >= 3 && ID_OK(a[0]) && ubo[a[0]]) bad[a[0]] = 1; break;   /* OpStore */
            case 63: for (int k = 0; k < 2 && k + 1 < (int)n; k++) if (ID_OK(a[k]) && ubo[a[k]]) bad[a[k]] = 1; break;   /* OpCopyMemory */
            case 67: case 70: if (n >= 4 && ID_OK(a[2]) && ubo[a[2]]) bad[a[2]] = 1; break;   /* pointer access chains */
            case 57: for (uint32_t k = 3; k + 1 < n; k++) if (ID_OK(a[k]) && ubo[a[k]]) bad[a[k]] = 1; break;   /* OpFunctionCall */
        }
        i += n;
    }
    #undef ID_OK
    int trimmed = 0, any16 = 0;
    for (uint32_t v = 1; v < bound; v++) any16 |= s16[v];
    if (!other_stride && any16) for (uint32_t v = 1; v < bound; v++) if (ubo[v] && !bad[v] && bind[v] >= 0)
        for (int k = 0; k < sh->nb; k++) if (sh->b[k].binding == (uint16_t)bind[v] && sh->b[k].dtype == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER) {
            sh->b[k].used = maxi[v] ? maxi[v] * 16 : 16; trimmed++;
        }
    free(bind); free(cval); free(maxi); free(isc); free(ubo); free(bad); free(s16);
    return trimmed;
}
/* DEPTH16 nudge for depth the fragment shader writes (TOTK, 2026-10-22; vk_draw.c d16tie_on). Every store to the
   FragDepth built-in becomes value + a float specialization constant (id D16_SPEC_ID, default 0), set to the nudge in
   pipelines that draw into a DEPTH16 target. Returns a new word array (caller frees) or NULL when nothing to patch. */
static uint32_t *spirv_depth_nudge(const uint32_t *w, size_t nw, size_t *out_nw) {
    if (nw < 5 || w[0] != 0x07230203) return NULL;
    uint32_t bound = w[3], fragdepth = 0, tfloat = 0; size_t after_float = 0, first_dec = 0; int stores = 0;
    for (size_t i = 5; i < nw; ) {
        uint32_t op = w[i] & 0xffff, n = w[i] >> 16; if (!n || i + n > nw) return NULL;
        if (op == 71 && !first_dec) first_dec = i;
        if (op == 71 && n >= 4 && w[i + 2] == 11 && w[i + 3] == 22) fragdepth = w[i + 1];   /* BuiltIn FragDepth */
        if (op == 22 && n >= 3 && w[i + 2] == 32 && !tfloat) { tfloat = w[i + 1]; after_float = i + n; }   /* OpTypeFloat 32 */
        i += n;
    }
    if (!fragdepth || !tfloat || !first_dec) return NULL;
    for (size_t i = 5; i < nw; ) { uint32_t op = w[i] & 0xffff, n = w[i] >> 16; if (op == 62 && n >= 3 && w[i + 1] == fragdepth) stores++; i += n; }
    if (!stores) return NULL;
    uint32_t *o = malloc((nw + 4 + 4 + (size_t)stores * 5) * sizeof *o); size_t k = 0;
    uint32_t spec = bound++;
    for (size_t i = 0; i < 5; i++) o[k++] = w[i];
    for (size_t i = 5; i < nw; ) {
        uint32_t op = w[i] & 0xffff, n = w[i] >> 16;
        if (i == first_dec) { o[k++] = 4u << 16 | 71; o[k++] = spec; o[k++] = 1; o[k++] = D16_SPEC_ID; }   /* OpDecorate spec SpecId */
        if (op == 62 && n >= 3 && w[i + 1] == fragdepth) {
            uint32_t t = bound++;
            o[k++] = 5u << 16 | 129; o[k++] = tfloat; o[k++] = t; o[k++] = w[i + 2]; o[k++] = spec;   /* OpFAdd */
            o[k++] = w[i]; o[k++] = w[i + 1]; o[k++] = t; for (uint32_t j = 3; j < n; j++) o[k++] = w[i + j];   /* OpStore FragDepth t */
        } else for (uint32_t j = 0; j < n; j++) o[k++] = w[i + j];
        i += n;
        if (i == after_float) { o[k++] = 4u << 16 | 50; o[k++] = tfloat; o[k++] = spec; o[k++] = 0; }   /* OpSpecConstant float 0.0 */
    }
    o[3] = bound; *out_nw = k;
    return o;
}
static int hexval(int c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; }
static uint64_t g_shbias_total;   /* lifetime: shaders whose SPIR-V carries the gather bias (marker in shader_load) */
/* Push descriptors (TOTK_VK_PUSH=1, 2026-10-23): the fragment and compute stages' bindings are pushed into the command
   buffer per draw (vkCmdPushDescriptorSetKHR) instead of a descriptor set allocated, written and cached per draw; a
   pipeline layout may hold only one push set, so the vertex stage (set 0 beside the fragment's set 1) keeps sets. */
static int push_layout(const vk_shader *sh) {
    static int on = -1;
    if (on < 0) { const char *e = getenv("TOTK_VK_PUSH"); on = e && atoi(e) > 0 && V.max_push && vkCmdPushDescriptorSetKHR; if (on) LOGV("TOTK_VK_PUSH: fragment and compute bindings are pushed (up to %u descriptors)", V.max_push); }
    if (!on || sh->stage == 0) return 0;
    uint32_t n = 0; for (int i = 0; i < sh->nb; i++) n += sh->b[i].count;
    return n && n <= V.max_push;
}
static vk_shader *shader_load(const char *variant) {
    char key[80] = {0}; snprintf(key, sizeof key, "%s", variant);
    vk_shader *sh = nvn_hm_get(&g_shaders, key);
    if (sh) return sh;
    char path[1024]; size_t spv_len = 0, vk_len = 0;
    snprintf(path, sizeof path, "%s/%s.spv", seed_dir(), variant); char *spv = read_file(path, &spv_len);
    snprintf(path, sizeof path, "%s/%s.vk", seed_dir(), variant); char *txt = read_file(path, &vk_len);
    if (!spv || !txt || spv_len % 4 || spv_len < 20) { RED(16, "shader %s: SPIR-V or reflection missing in the seed", variant); free(spv); free(txt); return NULL; }
    sh = calloc(1, sizeof *sh); memcpy(sh->key, key, sizeof sh->key);
    sh->local[0] = sh->local[1] = sh->local[2] = 1;
    int capb = 0;
    for (char *save = NULL, *line = strtok_r(txt, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        if (!strncmp(line, "stage ", 6)) sh->stage = atoi(line + 6);
        else if (!strncmp(line, "local ", 6)) sscanf(line + 6, "%u %u %u", &sh->local[0], &sh->local[1], &sh->local[2]);
        else if (!strncmp(line, "depth ", 6)) sh->writes_depth = atoi(line + 6);
        else if (!strncmp(line, "c1 ", 3)) {
            const char *h = line + 3; size_t n = strlen(h) / 2;
            sh->c1 = calloc(1, n ? n : 1); sh->c1_len = (uint32_t)n;
            for (size_t i = 0; i < n; i++) { int a = hexval(h[2 * i]), b = hexval(h[2 * i + 1]); sh->c1[i] = (uint8_t)(a < 0 || b < 0 ? 0 : a << 4 | b); }
        } else if (!strncmp(line, "vin", 3)) {
            for (char *p = line + 3; *p; ) { while (*p == ' ') p++; if (!*p) break; int loc, cls; if (sscanf(p, "%d:%d", &loc, &cls) == 2 && loc >= 0 && loc < 32) { sh->vin_mask |= 1u << loc; sh->vin_class[loc] = (uint8_t)cls; } while (*p && *p != ' ') p++; }
        } else if (!strncmp(line, "fout", 4)) {
            for (char *p = line + 4; *p; ) { while (*p == ' ') p++; if (!*p) break; int i = atoi(p); if (i >= 0 && i < 8) sh->fout_mask |= 1u << i; while (*p && *p != ' ') p++; }
        } else if (line[0] == 'b' && line[1] == ' ') {
            int bnd, cnt, dt, kind, reg, sreg, off, rw, tgt, isint;
            if (sscanf(line + 2, "%d %d %d %d %d %d %d %d %d %d", &bnd, &cnt, &dt, &kind, &reg, &sreg, &off, &rw, &tgt, &isint) != 10) continue;
            if (sh->nb == capb) { capb = capb ? 2 * capb : 16; sh->b = realloc(sh->b, (size_t)capb * sizeof *sh->b); }
            sh->b[sh->nb++] = (vk_bind){(uint16_t)bnd, (uint16_t)(cnt > 0 ? cnt : 1), (uint8_t)kind, (uint8_t)dt, (uint8_t)tgt, (uint8_t)rw, (uint8_t)isint, (int16_t)reg, (int16_t)sreg, (uint16_t)off};
        }
    }
    free(txt);
    { static uint64_t nmark; int m = spirv_mark_stores(sh, (const uint32_t *)spv, spv_len / 4);
      if (m && ++nmark <= 8) LOGV("shader %s writes %d storage binding(s) (from its SPIR-V)", variant, m); }
    if (!vk_off("ubtrim")) spirv_ubo_extent(sh, (const uint32_t *)spv, spv_len / 4);
    { static uint64_t nsh; int m = spirv_mark_shadow(sh, (const uint32_t *)spv, spv_len / 4);
      if (m && ++nsh <= 8) LOGV("shader %s samples %d shadow texture binding(s)", variant, m); }
    if (sh->stage == 1) {
        size_t nnw; uint32_t *pw = spirv_depth_nudge((const uint32_t *)spv, spv_len / 4, &nnw);
        if (pw) { free(spv); spv = (char *)pw; spv_len = nnw * 4; sh->depth_spec = 1; }
        patch_set((uint32_t *)spv, spv_len / 4, 1);
    }
    {   /* a marker for the gather bias (the bias adds TextureQuerySize per coordinate: OpImageQuerySizeLod 103 / OpImageQuerySize 104) */
        const uint32_t *w = (const uint32_t *)spv; size_t nw = spv_len / 4;
        if (nw > 5 && w[0] == 0x07230203) for (size_t i = 5; i < nw; ) {
            uint32_t op = w[i] & 0xffff, n = w[i] >> 16; if (!n) break;
            if (op == 103 || op == 104) { HK_ADD(shbias, 1); g_shbias_total++; break; }   /* the bias's marker */
            i += n;
        }
    }
    VkShaderModuleCreateInfo mci = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = spv_len, .pCode = (const uint32_t *)spv};
    VkResult r = vkCreateShaderModule(V.dev, &mci, NULL, &sh->mod);
    free(spv);
    if (r) { RED(16, "vkCreateShaderModule %s: %d", variant, r); free(sh->b); free(sh->c1); free(sh); return NULL; }
    VkDescriptorSetLayoutBinding *lb = calloc((size_t)(sh->nb ? sh->nb : 1), sizeof *lb);
    for (int i = 0; i < sh->nb; i++) lb[i] = (VkDescriptorSetLayoutBinding){sh->b[i].binding, (VkDescriptorType)sh->b[i].dtype, sh->b[i].count, stage_flags(sh->stage), NULL};
    VkDescriptorSetLayoutCreateInfo dci = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = (uint32_t)sh->nb, .pBindings = lb};
    if (push_layout(sh)) { dci.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR; sh->push = 1; }
    r = vkCreateDescriptorSetLayout(V.dev, &dci, NULL, &sh->dsl);
    free(lb);
    if (r) { RED(16, "descriptor set layout of %s: %d", variant, r); vkDestroyShaderModule(V.dev, sh->mod, NULL); free(sh->b); free(sh->c1); free(sh); return NULL; }
    nvn_hm_put(&g_shaders, key, sh);
    HK_ADD(shloads, 1); HK_ADD(shbytes, spv_len + vk_len);
    return sh;
}
uint64_t vk_shbias_total(void) { return g_shbias_total; }

/* ---------- variants of a shader ---------- */
typedef struct { char *name, *deps; } variant_t;
typedef struct { variant_t *v; int n; } variant_list;
static nvn_hmap g_variants = {.klen = 64};   /* shader key (hex) -> variant_list */
static variant_list *variants_of(const char *key) {
    char k[64]; memcpy(k, key, 64);
    variant_list *vl = nvn_hm_get(&g_variants, k);
    if (vl) return vl;
    vl = calloc(1, sizeof *vl);
    char path[1024]; snprintf(path, sizeof path, "%s/%.64s.vv", seed_dir(), key);
    char *txt = read_file(path, NULL);
    int cap = 0;
    if (txt) HK_ADD(vlists, 1);
    if (txt) for (char *save = NULL, *line = strtok_r(txt, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        char *tab = strchr(line, '\t'); if (tab) *tab = 0;
        if (vl->n == cap) { cap = cap ? 2 * cap : 4; vl->v = realloc(vl->v, (size_t)cap * sizeof *vl->v); }
        vl->v[vl->n].name = strdup(line); vl->v[vl->n].deps = strdup(tab ? tab + 1 : ""); vl->n++;
    }
    free(txt);
    nvn_hm_put(&g_variants, k, vl);
    return vl;
}
/* The request a PC-side fill needs (the iPhone's shader-requests/<variant>.json, ios/tools/fill_shaders.py). */
static void answers_json(const answers_t *a, char *out, size_t cap) {
    size_t o = 0;
#define PUT(...) do { int n_ = snprintf(out + o, o < cap ? cap - o : 0, __VA_ARGS__); if (n_ > 0) o += (size_t)n_; } while (0)
    /* NSJSONSerialization / json.dumps(sort_keys) order: keys sorted as strings ("10" before "2") */
    const char *kinds[4] = {"attr", "fout", "img", "tex"};
    PUT("{");
    for (int k = 0; k < 4; k++) {
        PUT("%s\"%s\":{", k ? "," : "", kinds[k]);
        int n = k == 0 ? 16 : k == 1 ? 8 : k == 2 ? 32 : 128, first = 1;
        char names[128][8]; int idx[128], m = 0;
        for (int i = 0; i < n; i++) { int has = k == 0 ? a->has_attr[i] : k == 1 ? a->has_fout[i] : k == 2 ? a->has_img[i] : a->has_tex[i]; if (has) { snprintf(names[m], 8, "%d", i); idx[m++] = i; } }
        for (int i = 1; i < m; i++) for (int j = i; j > 0 && strcmp(names[j - 1], names[j]) > 0; j--) { char t[8]; memcpy(t, names[j], 8); memcpy(names[j], names[j - 1], 8); memcpy(names[j - 1], t, 8); int ti = idx[j]; idx[j] = idx[j - 1]; idx[j - 1] = ti; }
        for (int j = 0; j < m; j++) {
            int i = idx[j];
            if (k == 2) { char ty[24] = {0}, fm[24] = {0}; const char *slash = strchr(a->img[i], '/');
                if (slash) { snprintf(ty, sizeof ty, "%.*s", (int)(slash - a->img[i]), a->img[i]); snprintf(fm, sizeof fm, "%s", slash + 1); } else snprintf(ty, sizeof ty, "%s", a->img[i]);
                if (fm[0]) PUT("%s\"%s\":{\"fmt\":\"%s\",\"type\":\"%s\"}", first ? "" : ",", names[j], fm, ty); else PUT("%s\"%s\":{\"type\":\"%s\"}", first ? "" : ",", names[j], ty); }
            else PUT("%s\"%s\":\"%s\"", first ? "" : ",", names[j], k == 0 ? a->attr[i] : k == 1 ? a->fout[i] : a->tex[i]);
            first = 0;
        }
        PUT("}");
    }
    PUT("}");
#undef PUT
}
static void request_miss(const char *key, int st, const uint8_t *control, const uint8_t *code, const answers_t *a) {
    char aj[16384]; answers_json(a, aj, sizeof aj);
    unsigned char md[32]; host_sha256(aj, strlen(aj), md);
    char variant[80]; snprintf(variant, sizeof variant, "%.64s-", key);
    for (int i = 0; i < 6; i++) snprintf(variant + 65 + 2 * i, 3, "%02x", md[i]);
    static nvn_hmap missed = {.klen = 80}; char k[80] = {0}; memcpy(k, variant, strlen(variant));
    if (nvn_hm_get(&missed, k)) return;
    nvn_hm_put(&missed, k, (void *)1);
    const char *dir = getenv("TOTK_SHADER_REQ_DIR"); if (!dir || !*dir) dir = "shader-requests";
    mkdir(dir, 0755);
    uint32_t c1o, c1t; memcpy(&c1o, control + 0x700, 4); memcpy(&c1t, control + 0x704, 4);
    size_t skip = st == 5 ? 0 : 0x30;
    if (!c1t || c1t > (16u << 20) || c1o > c1t || c1t < skip) return;
    char path[1024]; snprintf(path, sizeof path, "%s/%s.json", dir, variant);
    FILE *f = fopen(path, "w"); if (!f) return;
    HK_ADD(sreq, 1);
    fprintf(f, "{\"variant\":\"%s\",\"stage\":\"%s\",\"depth_minus_w\":%s", variant, st == 0 ? "vs" : st == 1 ? "ps" : "cs", totk_depth_near_minus_w && totk_depth_near_minus_w() ? "true" : "false");
    if (st == 5) { uint32_t l[3]; memcpy(l, control + 0x748, 12); fprintf(f, ",\"local\":[%u,%u,%u]", l[0], l[1], l[2]); }
    fputs(",\"c1\":\"", f); for (uint32_t i = c1o; i < c1t; i++) fprintf(f, "%02x", code[i]);
    fputs("\",\"code_hex\":\"", f); for (size_t i = skip; i < c1t; i++) fprintf(f, "%02x", code[i]);
    fprintf(f, "\",%s\n", aj + 1);   /* the answers' own members (and their closing brace) after the request's */
    fclose(f);
    static uint64_t n; n++;
    if (n <= 20 || n % 100 == 0) LOGV("[shader-miss] %s %s is not in the seed: its draws are skipped (request %llu saved in %s)", st == 0 ? "vs" : st == 1 ? "ps" : "cs", variant, (unsigned long long)n, dir);
}

/* ---------- programs ---------- */
static uint64_t stage_sig(int st) {   /* nvn_mtl.m stage_sig */
    uint64_t sg = 0;
    if (st == 1 && S.has_rt) for (int i = 0; i < S.rt.n && i < 8; i++) if (S.rt.color[i])
        sg |= (uint64_t)(1 + vk_fmt_class(S.rt.has_cview[i] && S.rt.cview[i].set_format ? S.rt.cview[i].format : S.rt.color[i]->d.format)) << (3 * i);
    if (st == 0) for (int i = 0; i < S.nattr && i < 16; i++) sg |= (uint64_t)vk_fmt_class(S.attr[i].fmt) << (2 * i);
    uint64_t dm = 0; int hi = st >= 0 && st < 6 && S.tex_hi[st] ? S.tex_hi[st] : 128; if (hi > 128) hi = 128;
    /* memo per stage (TOTK, 2026-10-23: a registry format lookup for every bound texture on every draw, ~4%): the same
       bound ids and no texture registration since give the same depth mask */
    static uint64_t m_tex[6][128], m_dm[6], m_epoch[6]; static int m_hi[6] = {-1, -1, -1, -1, -1, -1};
    uint64_t ep = vk_treg_epoch(); int memo = st >= 0 && st < 6;
    if (memo && m_hi[st] == hi && m_epoch[st] == ep && !memcmp(m_tex[st], S.tex[st], (size_t)hi * sizeof S.tex[st][0])) dm = m_dm[st];
    else {
        for (int r = 0; r < hi; r++) if (S.tex[st][r]) {
            int f = vk_treg_fmt((int)(S.tex[st][r] & 0xfffff)); if (f < 0) continue;
            if (f >= 0x32 && f <= 0x36) dm ^= 0x9E3779B97F4A7C15ull * (uint64_t)(r + 1);
        }
        if (memo) { memcpy(m_tex[st], S.tex[st], (size_t)hi * sizeof S.tex[st][0]); m_hi[st] = hi; m_epoch[st] = ep; m_dm[st] = dm; }
    }
    if (dm) sg ^= dm | 1ull << 63;
    return sg;
}
typedef struct { uint64_t sig; vk_shader *sh; int missing; } alt_t;
struct vk_prog {
    char key[6][65]; const uint8_t *ctl[6], *code[6]; int has[6];
    alt_t *alt[6]; int nalt[6], capalt[6];
    vk_shader *cur[6]; uint64_t cur_sig[6]; int cur_ok[6];
};
static nvn_hmap g_progs = {.klen = 8};   /* prog_t serial -> vk_prog */
/* The game releases programs on its own threads (nvnProgramSetShaders / Finalize) while the replay thread looks them
   up and adds them; the map's growth frees its arrays, so every access holds this. */
static pthread_mutex_t g_progs_mu = PTHREAD_MUTEX_INITIALIZER;
static inline uint32_t ctl_key_len(const uint8_t *control) { uint32_t n; memcpy(&n, control + 0x20, 4); return n >= 0x7cc && n < 0x880 ? n : 0x880; }
static vk_prog *prog_of(const prog_t *p) {
    if (!p || !p->serial) return NULL;
    /* per-thread front cache (TOTK, 2026-10-23: the map's mutex on every draw): vk_progs are never freed and serials
       are never reused, so a cached pointer stays right */
    enum { PC = 512 };
    static __thread struct { uint64_t serial; vk_prog *pr; } *pc;
    if (!pc) pc = calloc(PC, sizeof *pc);
    unsigned h = (unsigned)((p->serial * 0x9E3779B97F4A7C15ull) >> 55) & (PC - 1);
    if (pc && pc[h].serial == p->serial && pc[h].pr) return pc[h].pr;
    pthread_mutex_lock(&g_progs_mu); vk_prog *pr = nvn_hm_get(&g_progs, &p->serial); pthread_mutex_unlock(&g_progs_mu);
    if (pr) { if (pc) { pc[h].serial = p->serial; pc[h].pr = pr; } return pr; }
    pr = calloc(1, sizeof *pr);
    for (unsigned i = 0; i < p->nstage && i < 6; i++) {
        const uint8_t *control = p->sd[i].control; uint32_t length = 0, st = 0;
        if (!control || !p->sd[i].data) continue;
        memcpy(&length, control + 0x704, 4); memcpy(&st, control + 0x714, 4);
        if (!length || length > (16u << 20) || (st != 0 && st != 1 && st != 5)) { RED(8, "shader stage %u (length %u) is not supported", st, length); continue; }
        host_sha256_ctx ctx; unsigned char md[32];
        host_sha256_init(&ctx); host_sha256_update(&ctx, control, ctl_key_len(control)); host_sha256_update(&ctx, (const void *)(uintptr_t)p->sd[i].data, length); host_sha256_final(&ctx, md);
        for (int j = 0; j < 32; j++) snprintf(pr->key[st] + 2 * j, 3, "%02x", md[j]);
        pr->ctl[st] = control; pr->code[st] = (const uint8_t *)(uintptr_t)p->sd[i].data; pr->has[st] = 1;
    }
    pthread_mutex_lock(&g_progs_mu); nvn_hm_put(&g_progs, &p->serial, pr); pthread_mutex_unlock(&g_progs_mu);
    return pr;
}
void mtl_prog_release(prog_t *p) { if (!p->serial) return; pthread_mutex_lock(&g_progs_mu); nvn_hm_del(&g_progs, &p->serial); pthread_mutex_unlock(&g_progs_mu); }   /* the vk_prog leaks a little; programs are few */
int vk_prog_resolve(const prog_t *p, vk_shader *out[6]) {
    memset(out, 0, 6 * sizeof *out);
    if (!vk_shaders_ready()) return -1;
    vk_prog *pr = prog_of(p);
    if (!pr) return -1;
    int missing = 0;
    for (int st = 0; st < 6; st++) {
        if (!pr->has[st]) continue;
        uint64_t sg = stage_sig(st);
        if (pr->cur_ok[st] && pr->cur_sig[st] == sg) { out[st] = pr->cur[st]; if (!out[st]) missing = 1; continue; }
        alt_t *hit = NULL;
        for (int i = 0; i < pr->nalt[st]; i++) if (pr->alt[st][i].sig == sg) { hit = &pr->alt[st][i]; break; }
        if (!hit) {
            answers_t a; answers_of(st, &a);
            variant_list *vl = variants_of(pr->key[st]);
            vk_shader *sh = NULL;
            for (int i = 0; i < vl->n && !sh; i++) if (deps_match(vl->v[i].deps, &a)) sh = shader_load(vl->v[i].name);
            if (!sh) request_miss(pr->key[st], st, pr->ctl[st], pr->code[st], &a);
            if (pr->nalt[st] == pr->capalt[st]) { pr->capalt[st] = pr->capalt[st] ? 2 * pr->capalt[st] : 2; pr->alt[st] = realloc(pr->alt[st], (size_t)pr->capalt[st] * sizeof *pr->alt[st]); }
            pr->alt[st][pr->nalt[st]++] = (alt_t){sg, sh, !sh};
            hit = &pr->alt[st][pr->nalt[st] - 1];
        }
        pr->cur[st] = hit->sh; pr->cur_sig[st] = sg; pr->cur_ok[st] = 1;
        out[st] = hit->sh; if (!hit->sh) missing = 1;
    }
    return missing ? -1 : 0;
}

/* ---------- pipeline layouts: set 0 = vertex (or compute) stage, set 1 = fragment stage ---------- */
static VkDescriptorSetLayout empty_dsl(void) {
    static VkDescriptorSetLayout e;
    if (!e) { VkDescriptorSetLayoutCreateInfo ci = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO}; VKCHECK(vkCreateDescriptorSetLayout(V.dev, &ci, NULL, &e)); }
    return e;
}
VkPipelineLayout vk_layout_of(vk_shader *vs, vk_shader *fs) {
    static nvn_hmap layouts = {.klen = 16};
    void *k[2] = {vs, fs};
    VkPipelineLayout l = (VkPipelineLayout)nvn_hm_get(&layouts, k);
    if (l) return l;
    VkDescriptorSetLayout sets[2] = {vs ? vs->dsl : empty_dsl(), fs ? fs->dsl : empty_dsl()};
    VkPipelineLayoutCreateInfo ci = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = fs ? 2 : 1, .pSetLayouts = sets};
    VKCHECK(vkCreatePipelineLayout(V.dev, &ci, NULL, &l));
    nvn_hm_put(&layouts, k, (void *)l);
    return l;
}
VkPipeline vk_compute_pipeline(vk_shader *cs, VkPipelineLayout *layout) {
    static nvn_hmap pipes = {.klen = 8};
    *layout = vk_layout_of(cs, NULL);
    VkPipeline p = (VkPipeline)nvn_hm_get(&pipes, &cs);
    if (p) return p;
    VkComputePipelineCreateInfo ci = {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = cs->mod, .pName = "main"}, .layout = *layout};
    pthread_mutex_lock(&g_pipecache_mu);   /* V.pcache is externally synchronised (the graphics worker uses it too) */
    VkResult r = vkCreateComputePipelines(V.dev, V.pcache, 1, &ci, NULL, &p);
    pthread_mutex_unlock(&g_pipecache_mu);
    if (r) { RED(8, "compute pipeline %s: %d", cs->key, r); return VK_NULL_HANDLE; }
    nvn_hm_put(&pipes, &cs, (void *)p);
    return p;
}
