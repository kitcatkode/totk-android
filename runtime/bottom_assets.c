/* Game pictures for the bottom screen (android/BOTTOM_SCREEN.md steps C and E; 2026-10-23): the map tiles and the item
   icons, read from the RomFS in-process (romfs.c) and decoded on the CPU. Included by bottom_screen.c. Read-only.

   A .zs file is zstd with the dictionary zs.zsdic, from Pack/ZsDic.pack.zs (plain zstd: a SARC holding the
   dictionaries). A .bntx holds one texture here: BRTI +0x1C format (high byte 0x2d..0x3a: ASTC 4x4 .. 12x12), +0x24
   width, +0x28 height, +0x34 block height log2; the data (one mip) at BRTD + 0x10, Tegra block-linear. Pixels come out
   as the bottom screen's 0xAABBGGRR. */
#include "../third_party/zstd/zstd.h"
int totk_decode_astc(const uint8_t *data, long w, long h, int bw, int bh, uint32_t *image);   /* third_party/astc/astc_c.cpp */

static ZSTD_DDict *g_zsdict; static int g_zs_state;   /* 0 not tried, 1 ready, -1 failed */
static pthread_mutex_t g_asset_mu = PTHREAD_MUTEX_INITIALIZER;   /* the map builder's thread and the drawing thread */

static uint8_t *rom_read(const char *path, size_t *n) {
    char p[300]; snprintf(p, sizeof p, "content:/%s", path);
    uint64_t h, got = 0; int64_t size = 0;
    if (runtime_romfs_open(&h, p, 1)) return NULL;
    uint8_t *b = NULL;
    if (!runtime_romfs_size(&size, h) && size > 0 && size < (256 << 20) && (b = malloc((size_t)size)) &&
        (runtime_romfs_read(&got, h, 0, b, (uint64_t)size) || got != (uint64_t)size)) { free(b); b = NULL; }
    runtime_romfs_close(h);
    *n = (size_t)size; return b;
}
static uint8_t *zs_decompress(const uint8_t *src, size_t sn, const ZSTD_DDict *dict, size_t *n) {
    unsigned long long full = ZSTD_getFrameContentSize(src, sn);
    if (full == ZSTD_CONTENTSIZE_ERROR || full == ZSTD_CONTENTSIZE_UNKNOWN || full > (256u << 20)) return NULL;
    uint8_t *d = malloc(full ? (size_t)full : 1); ZSTD_DCtx *dc = ZSTD_createDCtx();
    size_t r = !d || !dc ? 0 : dict ? ZSTD_decompress_usingDDict(dc, d, (size_t)full, src, sn, dict) : ZSTD_decompressDCtx(dc, d, (size_t)full, src, sn);
    ZSTD_freeDCtx(dc);
    if (!d || ZSTD_isError(r) || r != full) { free(d); return NULL; }
    *n = r; return d;
}
static int sarc_find(const uint8_t *d, size_t n, const char *name, const uint8_t **out, size_t *on) {
    if (n < 0x20 || memcmp(d, "SARC", 4)) return 0;
    uint16_t hl; uint32_t doff; memcpy(&hl, d + 4, 2); memcpy(&doff, d + 0xc, 4);   /* header: magic, length, BOM, file size, data offset */
    size_t o = hl; uint16_t nn; if (o + 12 > n || memcmp(d + o, "SFAT", 4)) return 0;
    memcpy(&nn, d + o + 6, 2); size_t nodes = o + 12, names = nodes + (size_t)nn * 16 + 8;
    if (names > n || memcmp(d + names - 8, "SFNT", 4)) return 0;
    for (unsigned i = 0; i < nn; i++) {
        uint32_t attr, s, e; memcpy(&attr, d + nodes + i * 16 + 4, 4); memcpy(&s, d + nodes + i * 16 + 8, 4); memcpy(&e, d + nodes + i * 16 + 12, 4);
        size_t no = names + (size_t)(attr & 0xffff) * 4;
        if (!(attr >> 24) || no >= n || strncmp((const char *)d + no, name, n - no)) continue;
        if ((size_t)doff + e > n || s > e) return 0;
        *out = d + doff + s; *on = e - s; return 1;
    }
    return 0;
}
static int zs_init(void) {
    if (g_zs_state) return g_zs_state > 0;
    g_zs_state = -1;
    size_t n, pn, dn; const uint8_t *dic;
    uint8_t *z = rom_read("Pack/ZsDic.pack.zs", &n), *pack = z ? zs_decompress(z, n, NULL, &pn) : NULL;
    if (pack && sarc_find(pack, pn, "zs.zsdic", &dic, &dn) && (g_zsdict = ZSTD_createDDict(dic, dn))) g_zs_state = 1;
    free(z); free(pack);
    fprintf(stderr, "[bottom] game pictures: %s\n", g_zs_state > 0 ? "zstd dictionary loaded" : "no zstd dictionary (Pack/ZsDic.pack.zs): no map pictures or icons");
    return g_zs_state > 0;
}
/* the texture of a .bntx.zs, w x h pixels 0xAABBGGRR (malloc'd); NULL when missing or not one this reads */
static uint32_t *asset_texture(const char *path, int *w, int *h) {
    pthread_mutex_lock(&g_asset_mu); int ok = zs_init(); pthread_mutex_unlock(&g_asset_mu);
    if (!ok) return NULL;
    size_t zn, n; uint8_t *z = rom_read(path, &zn), *d = z ? zs_decompress(z, zn, g_zsdict, &n) : NULL; free(z);
    if (!d) return NULL;
    static const uint8_t blk[][2] = {{4, 4}, {5, 4}, {5, 5}, {6, 5}, {6, 6}, {8, 5}, {8, 6}, {8, 8}, {10, 5}, {10, 6}, {10, 8}, {10, 10}, {12, 10}, {12, 12}};
    uint8_t *ti = memmem(d, n, "BRTI", 4), *td = memmem(d, n, "BRTD", 4); uint32_t fmt = 0, tw = 0, th = 0, bhl = 0;
    if (ti && ti + 0x38 <= d + n) { memcpy(&fmt, ti + 0x1c, 4); memcpy(&tw, ti + 0x24, 4); memcpy(&th, ti + 0x28, 4); memcpy(&bhl, ti + 0x34, 4); }
    unsigned t = fmt >> 8;
    if (!td || t < 0x2d || t > 0x3a || !tw || !th || tw > 4096 || th > 4096 || bhl > 5) { free(d); return NULL; }
    int bw = blk[t - 0x2d][0], bh = blk[t - 0x2d][1]; uint32_t wb = (tw + bw - 1) / bw, hb = (th + bh - 1) / bh, gx = (wb * 16 + 63) / 64, gh = 1u << bhl;
    const uint8_t *src = td + 0x10; size_t avail = (size_t)(d + n - src);
    /* + 16: the decoder reads a block's bits 4 or 8 bytes at a time, up to 3 bytes past the block (astc.cpp getbits);
       past the last one that left the buffer, a fault where it ended at a page (zoomed map tiles on the device) */
    uint8_t *lin = calloc((size_t)wb * hb * 16 + 16, 1); uint32_t *px = malloc((size_t)tw * th * 4);
    for (uint32_t y = 0; lin && y < hb; y++) for (uint32_t x = 0; x < wb; x++) {   /* block-linear: GOBs of 64 bytes x 8 rows */
        uint32_t xb = x * 16; size_t a = (size_t)(y / (8 * gh)) * gx * 512 * gh + (xb / 64) * 512 * gh + ((y % (8 * gh)) / 8) * 512 +
                                         ((xb % 64) / 32) * 256 + ((y % 8) / 2) * 64 + ((xb % 32) / 16) * 32 + (y % 2) * 16;
        if (a + 16 <= avail) memcpy(lin + ((size_t)y * wb + x) * 16, src + a, 16); else memset(lin + ((size_t)y * wb + x) * 16, 0, 16);
    }
    if (lin && px) totk_decode_astc(lin, tw, th, bw, bh, px);
    free(lin); free(d);
    if (!px) return NULL;
    for (size_t i = 0; i < (size_t)tw * th; i++) { uint32_t c = px[i]; px[i] = (c & 0xff00ff00u) | (c >> 16 & 0xff) | (c & 0xff) << 16; }   /* B G R A -> R G B A */
    *w = (int)tw; *h = (int)th; return px;
}
/* box-filtered copy of src (sw x sh) into dst (dw x dh, stride ds), with or without alpha (premultiplied over bg) */
static void asset_scale(const uint32_t *src, int sw, int sh, uint32_t *dst, int dw, int dh, int ds) {
    for (int y = 0; y < dh; y++) for (int x = 0; x < dw; x++) {
        int x0 = x * sw / dw, x1 = (x + 1) * sw / dw, y0 = y * sh / dh, y1 = (y + 1) * sh / dh; if (x1 <= x0) x1 = x0 + 1; if (y1 <= y0) y1 = y0 + 1;
        unsigned s[4] = {0}, cnt = 0;
        for (int yy = y0; yy < y1; yy++) for (int xx = x0; xx < x1; xx++) { uint32_t c = src[(size_t)yy * sw + xx]; unsigned a = c >> 24;
            s[0] += (c & 255) * a; s[1] += (c >> 8 & 255) * a; s[2] += (c >> 16 & 255) * a; s[3] += a; cnt++; }
        uint32_t o = 0; if (s[3]) o = (s[0] / s[3]) | (s[1] / s[3]) << 8 | (s[2] / s[3]) << 16;
        dst[(size_t)y * ds + x] = o | (s[3] / cnt) << 24;
    }
}

/* ---------- the map: one picture per layer, every tile at level 02 scaled to MAP_TILE pixels ---------- */
/* The Depths tiles are masks the game tints: grey-blue relief (blue channel), gloom as magenta (red and blue over green),
   a few features in their own colours (mines, structures: brown). Tinted here as the game shows the Depths: the relief
   in dark stone, the gloom in crimson, the features kept. */
static void depths_tint(uint32_t *px, size_t n) {
    for (size_t i = 0; i < n; i++) {
        uint32_t c = px[i]; int r = c & 255, g = c >> 8 & 255, bl = c >> 16 & 255;
        int lo = r < g ? r : g, hi = r > g ? r : g; if (bl < lo) lo = bl; if (bl > hi) hi = bl;
        int l = (r + g + bl) / 3, sr = l * 160 >> 8, sg = l * 150 >> 8, sb = l * 136 >> 8;   /* the relief: warm stone */
        int gloom = (r < bl ? r : bl) - g, out[3];
        if (gloom > 24) {   /* crimson, as strong as the magenta, still shaded by the relief */
            int k = gloom >= 104 ? 256 : (gloom - 24) * 256 / 80, d = 48 + bl / 3;
            out[0] = ((d * 150 >> 8) * k + sr * (256 - k)) >> 8; out[1] = ((d * 34 >> 8) * k + sg * (256 - k)) >> 8; out[2] = ((d * 50 >> 8) * k + sb * (256 - k)) >> 8;
        } else if (hi - lo > 40 && bl < hi) { out[0] = r * 4 / 5; out[1] = g * 4 / 5; out[2] = bl * 4 / 5; }   /* a feature: its own colour */
        else { out[0] = sr; out[1] = sg; out[2] = sb; }
        px[i] = (c & 0xff000000u) | (uint32_t)out[0] | (uint32_t)out[1] << 8 | (uint32_t)out[2] << 16;
    }
}
enum { MAP_TILE = 100, MAP_COLS = 12, MAP_ROWS = 10, MAP_REGIONS = 15, MAP_ALL = (1 << MAP_REGIONS) - 1 };
/* What is revealed (android/BOTTOM_SCREEN.md step G): a surface or sky tile's name has one letter per region it overlaps,
   T revealed or F not, in ascending region order. Region n is Skyview Tower n's (GameData IsActivateCannon.TowerNN, read
   with the inventory); its outline is UI/Map/MainFieldOpenMask/G_OpenMask_NN, cut to whole tiles (135 px a tile) and
   placed at g_mask_at. The places were found on the PC by matching each mask with the pixels that change between the
   tiles' T and F versions (every mask at least 87 % alike, 117 of the 120 tiles' letter counts reproduced; the sky has
   the same regions per tile). A tile's letters are the regions whose masks cover most of it, as many as it has letters;
   a letter none is found for (two sea tiles in the east) follows the tile's first. */
static const signed char g_mask_at[MAP_REGIONS][2] = {{4, 3}, {1, 2}, {2, 0}, {6, 0}, {8, 0}, {6, 3}, {7, 2}, {3, 5},
                                                       {0, 6}, {0, 4}, {7, 6}, {4, 0}, {4, 5}, {8, 5}, {0, 0}};
static _Atomic uint32_t g_reveal = MAP_ALL;   /* bit n-1: region n revealed (the newest save's towers; all without one) */
static _Atomic int g_reveal_all;              /* the map bar's "Reveal all" */
static uint32_t map_reveal(void) { return atomic_load(&g_reveal_all) ? MAP_ALL : atomic_load(&g_reveal); }
/* per layer (ground, sky), found by the first build: letters per tile (-1: no tile), the version ("", "_A_A", "_A": a
   few tiles come only as _A / _B, before / after a change in the world, _A taken for now; a few sky tiles also as _A_A
   .. _B_B, the whole tile, where their _A is the islands alone), each letter's region */
static signed char g_tile_k[2][MAP_COLS][MAP_ROWS], g_tile_reg[2][MAP_COLS][MAP_ROWS][4]; static char g_tile_var[2][MAP_COLS][MAP_ROWS][6];
static _Atomic int g_tiles_known[3];
static int rom_exists(const char *path) {
    char p[300]; uint64_t h; snprintf(p, sizeof p, "content:/%s", path);
    if (runtime_romfs_open(&h, p, 1)) return 0;
    runtime_romfs_close(h); return 1;
}
static void tiles_probe(int layer) {   /* the builder's thread, once per layer */
    static int cover[MAP_REGIONS][MAP_COLS][MAP_ROWS];   /* each mask's pixels per tile; both layers use it: under the lock */
    static int covered; static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
    for (int r = 0; r < MAP_ROWS; r++) for (int c = 0; c < MAP_COLS; c++) {
        g_tile_k[layer][c][r] = -1;
        for (int k = 1; k <= 4 && g_tile_k[layer][c][r] < 0; k++) for (int v = 0; v < 3; v++) {
            const char *var = (const char *[]){"", "_A_A", "_A"}[v]; char p[96];
            snprintf(p, sizeof p, "UI/Map/MainField/%c_%02d-%02d_02_%.*s%s.bntx.zs", "GS"[layer], c, r, k, "TTTT", var);
            if (rom_exists(p)) { g_tile_k[layer][c][r] = (signed char)k; snprintf(g_tile_var[layer][c][r], 6, "%s", var); break; }
        }
    }
    pthread_mutex_lock(&mu);
    for (int n = 0; !covered && n < MAP_REGIONS; n++) {
        char p[96]; int w, h; snprintf(p, sizeof p, "UI/Map/MainFieldOpenMask/G_OpenMask_%02d.bntx.zs", n + 1);
        uint32_t *m = asset_texture(p, &w, &h); if (!m) { fprintf(stderr, "[bottom] map: no %s\n", p); continue; }
        for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {
            int c = g_mask_at[n][0] + x / 135, r = g_mask_at[n][1] + y / 135;
            if (m[(size_t)y * w + x] >> 24 > 128 && c >= 0 && c < MAP_COLS && r >= 0 && r < MAP_ROWS) cover[n][c][r]++;
        }
        free(m);
    }
    covered = 1;
    for (int r = 0; r < MAP_ROWS; r++) for (int c = 0; c < MAP_COLS; c++) {
        int k = g_tile_k[layer][c][r], got = 0; signed char *reg = g_tile_reg[layer][c][r]; int used[MAP_REGIONS] = {0};
        for (; got < k; got++) {   /* the most covering masks first */
            int best = -1; for (int n = 0; n < MAP_REGIONS; n++) if (!used[n] && cover[n][c][r] && (best < 0 || cover[n][c][r] > cover[best][c][r])) best = n;
            if (best < 0) break;
            used[best] = 1; reg[got] = (signed char)best;
        }
        for (int i = 1; i < got; i++) for (int j = i; j > 0 && reg[j] < reg[j - 1]; j--) { signed char t = reg[j]; reg[j] = reg[j - 1]; reg[j - 1] = t; }
        for (int i = got; i < k; i++) reg[i] = -1;
    }
    pthread_mutex_unlock(&mu);
    atomic_store(&g_tiles_known[layer], 1);
}
/* a tile's name after "<layer>_<c>-<r>_<level>" for what is revealed; NULL: no such tile (or not known yet) */
static const char *map_tile_suffix(int layer, int c, int r, uint32_t reveal, char out[12]) {
    if (layer == 2) { out[0] = 0; return atomic_load(&g_tiles_known[2]) ? out : NULL; }
    if (!atomic_load(&g_tiles_known[layer])) return NULL;
    int k = g_tile_k[layer][c][r]; if (k < 0) return NULL;
    const signed char *reg = g_tile_reg[layer][c][r]; int first = reg[0] >= 0 ? reveal >> reg[0] & 1 : 1;
    out[0] = '_'; for (int i = 0; i < k; i++) out[1 + i] = (reg[i] >= 0 ? reveal >> reg[i] & 1 : first) ? 'T' : 'F';
    snprintf(out + 1 + k, 12 - 1 - k, "%s", g_tile_var[layer][c][r]);
    return out;
}
/* the whole-map pictures: built on a thread, again when what is revealed changes; the drawing thread takes a new one */
static uint32_t *g_mapimg[3], *_Atomic g_mapimg_new[3]; static uint32_t g_map_shown[3]; static _Atomic uint32_t g_map_made[3];
static _Atomic int g_map_building[3];   /* 0 ground, 1 sky, 2 Depths */
static void *map_build(void *arg) {
    int layer = (int)(intptr_t)arg; static const char pre[3] = {'G', 'S', 'U'};
    if (layer < 2 && !atomic_load(&g_tiles_known[layer])) tiles_probe(layer);
    if (layer == 2) atomic_store(&g_tiles_known[2], 1);
    uint32_t reveal = layer == 2 ? 0 : map_reveal();
    size_t W = MAP_COLS * MAP_TILE, H = MAP_ROWS * MAP_TILE; uint32_t *img = calloc(W * H, 4); int tiles = 0;
    for (int r = 0; img && r < MAP_ROWS; r++) for (int c = 0; c < MAP_COLS; c++) {
        char suf[12], p[96]; int w = 0, h = 0; uint32_t *px = NULL;
        if (!map_tile_suffix(layer, c, r, reveal, suf)) continue;
        snprintf(p, sizeof p, "UI/Map/MainField/%c_%02d-%02d_02%s.bntx.zs", pre[layer], c, r, suf);
        if (!(px = asset_texture(p, &w, &h))) { fprintf(stderr, "[bottom] map: no %s\n", p); continue; }
        asset_scale(px, w, h, img + (size_t)r * MAP_TILE * W + (size_t)c * MAP_TILE, MAP_TILE, MAP_TILE, (int)W); free(px); tiles++;
    }
    if (img && layer == 2) depths_tint(img, W * H);
    fprintf(stderr, "[bottom] map: layer %c, %d tiles, revealed %04x\n", pre[layer], tiles, (unsigned)reveal);
    atomic_store(&g_map_made[layer], reveal);   /* before the picture: the drawing thread reads it after taking one */
    free(atomic_exchange(&g_mapimg_new[layer], img));   /* one not taken yet is out of date */
    atomic_store(&g_map_building[layer], 0);
    bottom_redraw();
    return NULL;
}
static const uint32_t *map_image(int layer) {   /* the drawing thread; NULL until the first picture is made */
    uint32_t *n = atomic_exchange(&g_mapimg_new[layer], NULL);
    if (n) { free(g_mapimg[layer]); g_mapimg[layer] = n; g_map_shown[layer] = atomic_load(&g_map_made[layer]); }
    uint32_t want = layer == 2 ? 0 : map_reveal();
    if ((!g_mapimg[layer] || g_map_shown[layer] != want) && !atomic_load(&g_mapimg_new[layer]) && !atomic_exchange(&g_map_building[layer], 1)) {
        pthread_t th; if (!pthread_create(&th, NULL, map_build, (void *)(intptr_t)layer)) pthread_detach(th); else atomic_store(&g_map_building[layer], 0);
    }
    return g_mapimg[layer];
}

/* ---------- item icons: decoded on first use, kept small ---------- */
enum { ICON_MAX = 2048 };
static struct { char id[64]; int size; uint32_t *px; } g_icons[ICON_MAX]; static int g_nicons;
static const uint32_t *item_icon(const char *id, int size) {   /* size x size; the drawing thread only; NULL if the game has none */
    for (int i = 0; i < g_nicons; i++) if (g_icons[i].size == size && !strcmp(g_icons[i].id, id)) return g_icons[i].px;
    if (g_nicons == ICON_MAX) return NULL;
    char p[128]; int w, h; snprintf(p, sizeof p, "UI/Tex/Icon/%s.bntx.zs", id);
    uint32_t *big = asset_texture(p, &w, &h), *px = big ? malloc((size_t)size * size * 4) : NULL;
    if (px) asset_scale(big, w, h, px, size, size, size);
    free(big);
    snprintf(g_icons[g_nicons].id, sizeof g_icons[0].id, "%s", id); g_icons[g_nicons].size = size; g_icons[g_nicons].px = px; g_nicons++;
    return px;
}
static void b_image(ANativeWindow_Buffer *b, int x0, int y0, const uint32_t *px, int w, int h) {   /* alpha-blended */
    for (int y = 0; y < h; y++) {
        int py = y0 + y; if (py < 0 || py >= b->height) continue;
        uint32_t *row = (uint32_t *)b->bits + (size_t)py * b->stride;
        for (int x = 0; x < w; x++) {
            int qx = x0 + x; uint32_t c = px[(size_t)y * w + x]; unsigned a = c >> 24; if (qx < 0 || qx >= b->width || !a) continue;
            uint32_t d = row[qx], o = 0xff000000u;
            for (int k = 0; k < 24; k += 8) o |= (((c >> k & 255) * a + (d >> k & 255) * (255 - a)) / 255) << k;
            row[qx] = o;
        }
    }
}

/* ---------- item names: ActorMsg/PouchContent.msbt in the message archive, labels <item id>_Name (and _Caption) ----------
   Mals/<lang>.Product.<n>.sarc.zs: English (USen) for now, <n> found by trying. MSBT: "MsgStdBn", section count at
   0xE, sections from 0x20 (magic, u32 size, 8 bytes pad, data, aligned to 16). LBL1: u32 buckets, per bucket
   {u32 count, u32 offset}, at the offset count x {u8 length, name, u32 text index}. TXT2: u32 count, u32 offsets,
   UTF-16 strings; tags (0x0E group type size + size bytes, 0x0F group type) are dropped. */
typedef struct { char *id, *name; } item_name_t;
typedef struct { item_name_t *e; int n; } msg_table;
static msg_table g_names, g_captions; static int g_names_state;   /* the drawing thread only */
static int name_cmp(const void *a, const void *b) { return strcmp(((const item_name_t *)a)->id, ((const item_name_t *)b)->id); }
static char *utf16_to_utf8(const uint8_t *p, const uint8_t *end) {
    size_t cap = (size_t)(end - p) * 2 + 1, n = 0; char *o = malloc(cap); if (!o) return NULL;
    while (p + 2 <= end) {
        unsigned c = p[0] | p[1] << 8;
        if (c == 0) break;
        if (c == 0x0e && p + 8 <= end) { unsigned sz = p[6] | p[7] << 8; p += 8 + sz; continue; }
        if (c == 0x0f) { p += 6; continue; }
        p += 2;
        if (c >= 0xd800 && c < 0xdc00 && p + 2 <= end) { unsigned lo = p[0] | p[1] << 8; if (lo >= 0xdc00 && lo < 0xe000) { c = 0x10000 + ((c - 0xd800) << 10) + (lo - 0xdc00); p += 2; } }
        if (n + 4 >= cap) break;
        if (c < 0x80) o[n++] = (char)c;
        else if (c < 0x800) { o[n++] = (char)(0xc0 | c >> 6); o[n++] = (char)(0x80 | (c & 0x3f)); }
        else if (c < 0x10000) { o[n++] = (char)(0xe0 | c >> 12); o[n++] = (char)(0x80 | (c >> 6 & 0x3f)); o[n++] = (char)(0x80 | (c & 0x3f)); }
        else { o[n++] = (char)(0xf0 | c >> 18); o[n++] = (char)(0x80 | (c >> 12 & 0x3f)); o[n++] = (char)(0x80 | (c >> 6 & 0x3f)); o[n++] = (char)(0x80 | (c & 0x3f)); }
    }
    o[n] = 0; return o;
}
/* the texts whose labels are <item id><suffix>, into t (sorted by id) */
static void msg_load(const uint8_t *d, size_t n, const char *suffix, msg_table *t) {
    if (n < 0x20 || memcmp(d, "MsgStdBn", 8)) return;
    uint16_t nsec; memcpy(&nsec, d + 0xe, 2);
    const uint8_t *lbl = NULL, *txt = NULL; uint32_t lsz = 0, tsz = 0; size_t o = 0x20;
    for (unsigned i = 0; i < nsec && o + 16 <= n; i++) {
        uint32_t sz; memcpy(&sz, d + o + 4, 4); if (o + 16 + sz > n) return;
        if (!memcmp(d + o, "LBL1", 4)) { lbl = d + o + 16; lsz = sz; } else if (!memcmp(d + o, "TXT2", 4)) { txt = d + o + 16; tsz = sz; }
        o += 16 + ((sz + 15) & ~15u);
    }
    if (!lbl || !txt || lsz < 4 || tsz < 4) return;
    uint32_t nb, nt; memcpy(&nb, lbl, 4); memcpy(&nt, txt, 4);
    if ((size_t)4 + (size_t)nb * 8 > lsz || (size_t)4 + (size_t)nt * 4 > tsz) return;
    int cap = 0; size_t sl = strlen(suffix);
    for (uint32_t b = 0; b < nb; b++) {
        uint32_t cnt, off; memcpy(&cnt, lbl + 4 + b * 8, 4); memcpy(&off, lbl + 8 + b * 8, 4);
        for (uint32_t k = 0, p = off; k < cnt && p < lsz; k++) {
            unsigned l = lbl[p]; if (p + 1 + l + 4 > lsz) break;
            uint32_t ti; memcpy(&ti, lbl + p + 1 + l, 4);
            if (l > sl && !memcmp(lbl + p + 1 + l - sl, suffix, sl) && ti < nt) {
                uint32_t a, z; memcpy(&a, txt + 4 + ti * 4, 4); z = tsz; if (ti + 1 < nt) memcpy(&z, txt + 8 + ti * 4, 4);
                if (a < z && z <= tsz) {
                    if (t->n == cap) { cap = cap ? cap * 2 : 1024; item_name_t *g = realloc(t->e, (size_t)cap * sizeof *g); if (!g) break; t->e = g; }
                    char *id = strndup((const char *)lbl + p + 1, l - sl), *nm = utf16_to_utf8(txt + a, txt + z);
                    if (id && nm) { t->e[t->n].id = id; t->e[t->n].name = nm; t->n++; } else { free(id); free(nm); }
                }
            }
            p += 1 + l + 4;
        }
    }
    if (t->n) qsort(t->e, (size_t)t->n, sizeof *t->e, name_cmp);
}
static void names_init(void) {   /* names and captions (the game's descriptions), on first use */
    if (!g_names_state) {
        g_names_state = -1;
        pthread_mutex_lock(&g_asset_mu); int ok = zs_init(); pthread_mutex_unlock(&g_asset_mu);
        char p[96]; size_t zn = 0, n = 0, mn = 0; uint8_t *z = NULL; const uint8_t *msbt;
        for (int v = 160; ok && v >= 100 && !z; v--) { snprintf(p, sizeof p, "Mals/USen.Product.%d.sarc.zs", v); z = rom_read(p, &zn); }
        uint8_t *sarc = z ? zs_decompress(z, zn, g_zsdict, &n) : NULL; free(z);
        if (sarc && sarc_find(sarc, n, "ActorMsg/PouchContent.msbt", &msbt, &mn)) { msg_load(msbt, mn, "_Name", &g_names); msg_load(msbt, mn, "_Caption", &g_captions); }
        free(sarc);
        if (g_names.n) g_names_state = 1;
        fprintf(stderr, "[bottom] item names: %d, captions %d from %s\n", g_names.n, g_captions.n, z || sarc ? p : "(no message archive)");
    }
}
static const char *msg_find(const msg_table *t, const char *id) {
    names_init();
    item_name_t key = {(char *)id, NULL}, *f = t->n ? bsearch(&key, t->e, (size_t)t->n, sizeof *t->e, name_cmp) : NULL;
    return f ? f->name : NULL;
}
static const char *item_name(const char *id) { const char *s = msg_find(&g_names, id); return s ? s : id; }   /* the game's name, or the id */
static const char *item_caption(const char *id) { return msg_find(&g_captions, id); }   /* its description; NULL if none */
