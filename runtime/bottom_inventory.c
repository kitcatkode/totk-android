/* The bottom screen's Inventory tab (android/BOTTOM_SCREEN.md step D; 2026-10-23): the pouches as of the newest save.
   Included by bottom_screen.c after its drawing helpers. Read-only: the save is opened for reading and never changed.

   progress.sav is GameData (TotkMods/Research Formats/Sav): u32 magic 0x01020304, u32 version, u32 the data section's
   offset; from 0x20 to there {u32 hash, u32 value} pairs, a pair with hash 0 starting a section of one type (the value);
   an array's value is a file offset to u32 count + the elements (String64: 64 bytes). Keys are MurmurHash3-32 (seed 0)
   of their names; the pouch keys are from marcrobledo/savegame-editors zelda-totk.class.pouch.js. The game writes its
   saves through fs_host.c into TOTK_PROFILE_DIR/save/<user>/slot_NN/: the newest progress.sav is read again when its
   time changes (a save being written fails the checks and the previous one stays). Names and icons come from the game
   files (bottom_assets.c: item_name, item_icon). */
#include <dirent.h>
#include <sys/stat.h>


static uint32_t mm3(const char *key) {
    const uint8_t *d = (const uint8_t *)key; size_t n = strlen(key); uint32_t h = 0, c1 = 0xcc9e2d51, c2 = 0x1b873593, k;
    for (size_t i = 0; i + 4 <= n; i += 4) {
        memcpy(&k, d + i, 4); k *= c1; k = k << 15 | k >> 17; k *= c2;
        h ^= k; h = h << 13 | h >> 19; h = h * 5 + 0xe6546b64;
    }
    k = 0; for (size_t i = n & ~(size_t)3, s = 0; i < n; i++, s += 8) k |= (uint32_t)d[i] << s;
    if (n & 3) { k *= c1; k = k << 15 | k >> 17; k *= c2; h ^= k; }
    h ^= (uint32_t)n; h ^= h >> 16; h *= 0x85ebca6b; h ^= h >> 13; h *= 0xc2b2ae35; h ^= h >> 16;
    return h;
}

enum { INV_POUCHES = 9 };
typedef enum { V_LIFE, V_STOCK, V_NONE } inv_value;
static const struct { const char *tab, *key; inv_value v; } g_pouch[INV_POUCHES] = {
    {"Weapons", "Weapon", V_LIFE}, {"Bows", "Bow", V_LIFE}, {"Shields", "Shield", V_LIFE}, {"Armor", "Armor", V_NONE},
    {"Arrows", "Arrow", V_STOCK}, {"Materials", "Material", V_STOCK}, {"Food", "Food", V_STOCK},
    {"Devices", "SpecialParts", V_STOCK}, {"Key items", "KeyItem", V_STOCK},
};
typedef struct { char name[64], fuse[64]; int value; } inv_item;
static struct { inv_item *it; int n; } g_inv[INV_POUCHES];
static char g_inv_path[1700]; static time_t g_inv_time; static int g_inv_state;   /* 0 not read, 1 read, -1 no save */
static int g_inv_tab, g_inv_scroll[INV_POUCHES];

typedef struct { const uint8_t *b; size_t n; uint32_t doff; } sav;
static int sav_find(const sav *s, uint32_t hash, uint32_t *type, uint32_t *value) {
    uint32_t t = 0;
    for (size_t o = 0x20; o + 8 <= s->doff; o += 8) {
        uint32_t h, v; memcpy(&h, s->b + o, 4); memcpy(&v, s->b + o + 4, 4);
        if (!h) t = v; else if (h == hash) { *type = t; *value = v; return 1; }
    }
    return 0;
}
static int sav_array(const sav *s, const char *key, uint32_t want, uint32_t *count, const uint8_t **el, size_t size) {
    uint32_t t, v;
    if (!sav_find(s, mm3(key), &t, &v) || t != want || (size_t)v + 4 > s->n) return 0;
    memcpy(count, s->b + v, 4);
    if (*count > 4096 || (size_t)v + 4 + (size_t)*count * size > s->n) return 0;
    *el = s->b + v + 4; return 1;
}
static int newest_save(char *out, size_t outn, time_t *when) {
    const char *prof = getenv("TOTK_PROFILE_DIR"); if (!prof) return 0;
    char dir[1100]; snprintf(dir, sizeof dir, "%s/save", prof);
    DIR *users = opendir(dir); if (!users) return 0;
    struct dirent *u; time_t best = 0;
    while ((u = readdir(users))) {
        if (u->d_name[0] == '.') continue;
        char ud[1400]; snprintf(ud, sizeof ud, "%s/%s", dir, u->d_name);
        DIR *slots = opendir(ud); if (!slots) continue;
        struct dirent *sl;
        while ((sl = readdir(slots))) {
            if (strncmp(sl->d_name, "slot_", 5)) continue;
            char p[1700]; struct stat st; snprintf(p, sizeof p, "%s/%s/progress.sav", ud, sl->d_name);
            if (stat(p, &st) || st.st_mtime <= best) continue;
            best = st.st_mtime; snprintf(out, outn, "%s", p);
        }
        closedir(slots);
    }
    closedir(users);
    *when = best; return best != 0;
}
static void inv_load(void) {   /* the bottom screen's thread: draws only, so no lock */
    char path[1700]; time_t when;
    if (!newest_save(path, sizeof path, &when)) { if (!g_inv_state) g_inv_state = -1; return; }
    if (g_inv_state == 1 && when == g_inv_time && !strcmp(path, g_inv_path)) return;
    FILE *f = fopen(path, "rb"); if (!f) return;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *b = n > 0x20 && n < (64 << 20) ? malloc((size_t)n) : NULL;
    int ok = b && fread(b, 1, (size_t)n, f) == (size_t)n; fclose(f);
    sav s = {b, (size_t)n, 0}; uint32_t magic = 0;
    if (ok) { memcpy(&magic, b, 4); memcpy(&s.doff, b + 8, 4); ok = magic == 0x01020304 && s.doff > 0x20 && s.doff <= s.n; }
    inv_item *it[INV_POUCHES] = {0}; int cnt[INV_POUCHES] = {0};
    for (int p = 0; ok && p < INV_POUCHES; p++) {
        char k[96]; uint32_t nn, nv = 0, nf = 0; const uint8_t *names, *vals = NULL, *fuse = NULL;
        snprintf(k, sizeof k, "Pouch.%s.Content.Name", g_pouch[p].key);
        if (!sav_array(&s, k, 17, &nn, &names, 64)) { ok = 0; break; }
        if (g_pouch[p].v != V_NONE) {
            snprintf(k, sizeof k, "Pouch.%s.Content.%s", g_pouch[p].key, g_pouch[p].v == V_LIFE ? "Life" : "StockNum");
            if (!sav_array(&s, k, 3, &nv, &vals, 4)) vals = NULL;
        }
        snprintf(k, sizeof k, "Pouch.%s.Content.Combined.Name", g_pouch[p].key);
        if (!sav_array(&s, k, 17, &nf, &fuse, 64)) fuse = NULL;
        it[p] = calloc(nn ? nn : 1, sizeof **it);
        for (uint32_t i = 0; i < nn; i++) {
            const char *nm = (const char *)names + i * 64; if (!nm[0]) continue;
            inv_item *x = &it[p][cnt[p]++];
            memcpy(x->name, nm, 63);
            if (vals && i < nv) memcpy(&x->value, vals + i * 4, 4); else x->value = INT32_MIN;
            if (fuse && i < nf) memcpy(x->fuse, fuse + i * 64, 63);
        }
    }
    uint32_t towers = 0; int found = 0;   /* the Skyview Towers launched from: their regions show revealed on the map */
    for (int n = 1; ok && n <= MAP_REGIONS; n++) {
        char k[48]; uint32_t t, v; snprintf(k, sizeof k, "IsActivateCannon.Tower%02d", n);
        if (sav_find(&s, mm3(k), &t, &v) && t == 0) { found++; if (v) towers |= 1u << (n - 1); }   /* section type 0: Bool, the value inline */
    }
    free(b);
    if (ok) atomic_store(&g_reveal, found == MAP_REGIONS ? towers : MAP_ALL);
    if (!ok) { for (int p = 0; p < INV_POUCHES; p++) free(it[p]); if (!g_inv_state) g_inv_state = -1; fprintf(stderr, "[bottom] inventory: %s not readable yet\n", path); return; }
    for (int p = 0; p < INV_POUCHES; p++) { free(g_inv[p].it); g_inv[p].it = it[p]; g_inv[p].n = cnt[p]; }
    snprintf(g_inv_path, sizeof g_inv_path, "%s", path); g_inv_time = when; g_inv_state = 1;
    fprintf(stderr, "[bottom] inventory from %s: %d weapons, %d materials; towers %04x\n", path, g_inv[0].n, g_inv[5].n, (unsigned)atomic_load(&g_reveal));
}

/* layout: pouch chips under the stats line; under them a grid of the game's icons (a tap picks an item) beside a panel
   with the picked item (its icon large, name, durability or count, fused material, the game's description); a footer
   with the save's time */
enum { INV_CHIP_Y = ROW_Y0, INV_CHIP_H = 64, INV_LIST_Y = INV_CHIP_Y + INV_CHIP_H + 16, INV_COLS = 7, INV_TILE = 112,
       INV_ICON = 88, INV_BADGE = 40, INV_BIG = 176, INV_FOOT = 56, INV_GRID_W = INV_COLS * INV_TILE };
static int g_inv_sel[INV_POUCHES];   /* the picked item per pouch */
static int inv_rows_visible(int height) { return (height - INV_FOOT - INV_LIST_Y) / INV_TILE; }
static int inv_max_scroll(int p, int height) { int rows = (g_inv[p].n + INV_COLS - 1) / INV_COLS - inv_rows_visible(height); return rows > 0 ? rows : 0; }
static void inv_detail(ANativeWindow_Buffer *b, int p, const inv_item *x, int x0, int y0, int x1, int y1) {
    b_panel(b, x0, y0, x1, y1, 14, g_col[C_ROW], g_col[C_EDGE]);
    if (!x) return;
    int cx = (x0 + x1) / 2, iy = y0 + 22;
    b_panel(b, cx - INV_BIG / 2 - 16, iy - 8, cx + INV_BIG / 2 + 16, iy + INV_BIG + 8, 16, g_col[C_SLOT], g_col[C_EDGE]);   /* the icon's slot */
    const uint32_t *ic = item_icon(x->name, INV_BIG); if (ic) b_image(b, cx - INV_BIG / 2, iy, ic, INV_BIG, INV_BIG);
    int tx = x0 + 26, tw = x1 - x0 - 52, y = iy + INV_BIG + 56;
    y = b_wrap(b, tx, y, tw, item_name(x->name), 34, g_col[C_TEXT], 40, y1 - 20) + 6;
    char v[48] = "";
    if (x->value != INT32_MIN && x->value >= 0) snprintf(v, sizeof v, g_pouch[p].v == V_LIFE ? "%d" : "×%d", x->value);
    if (v[0]) { const char *l = g_pouch[p].v == V_LIFE ? "Durability" : "Owned"; b_text(b, tx, y, l, 26, g_col[C_DIM]); b_text(b, tx + text_width(l, 26) + 14, y, v, 26, g_col[C_ACCENT]); y += 38; }
    if (x->fuse[0]) {   /* the fused material: its icon and name */
        b_text(b, tx, y, "Fused", 26, g_col[C_DIM]); int fx = tx + text_width("Fused", 26) + 12;
        const uint32_t *fi = item_icon(x->fuse, 32); if (fi) { b_image(b, fx, y - 25, fi, 32, 32); fx += 40; }
        char f[120]; snprintf(f, sizeof f, "%s", item_name(x->fuse));
        while (strlen(f) > 4 && text_width(f, 26) > x1 - 26 - fx) { size_t k = strlen(f) - 4; while (k && ((unsigned char)f[k] & 0xc0) == 0x80) k--; strcpy(f + k, "..."); }
        b_text(b, fx, y, f, 26, g_col[C_ACCENT]); y += 38;
    }
    const char *cap = item_caption(x->name);
    if (cap) { b_rule(b, tx, x1 - 26, y - 14, g_col[C_EDGE], 255); b_wrap(b, tx, y + 22, tw, cap, 24, g_col[C_DIM], 32, y1 - 16); }
}
static void inv_draw(ANativeWindow_Buffer *b) {
    inv_load();
    int cw = (b->width - 2 * PAD) / INV_POUCHES;
    for (int p = 0; p < INV_POUCHES; p++) {
        int x0 = PAD + p * cw, on = p == g_inv_tab;
        b_panel(b, x0 + 4, INV_CHIP_Y, x0 + cw - 4, INV_CHIP_Y + INV_CHIP_H, 8, on ? g_col[C_TABOFF] : g_col[C_PILL], on ? g_col[C_TAB] : g_col[C_EDGE]);
        if (on) b_cursor(b, x0 + 4, INV_CHIP_Y, x0 + cw - 4, INV_CHIP_Y + INV_CHIP_H, g_col[C_TAB]);
        b_text(b, x0 + (cw - text_width(g_pouch[p].tab, 25)) / 2, INV_CHIP_Y + 41, g_pouch[p].tab, 25, on ? g_col[C_TEXT] : g_col[C_DIM]);
    }
    if (g_inv_state != 1) {
        const char *m = g_inv_state < 0 ? "No save yet" : "Reading the save..."; b_text(b, (b->width - text_width(m, 44)) / 2, b->height / 2, m, 44, g_col[C_DIM]);
        return;
    }
    int p = g_inv_tab, vis = inv_rows_visible(b->height), gx = PAD, gy = INV_LIST_Y;
    if (g_inv_scroll[p] > inv_max_scroll(p, b->height)) g_inv_scroll[p] = inv_max_scroll(p, b->height);
    if (g_inv_sel[p] >= g_inv[p].n) g_inv_sel[p] = g_inv[p].n ? g_inv[p].n - 1 : 0;
    if (!g_inv[p].n) { const char *m = "Empty"; b_text(b, gx + (INV_GRID_W - text_width(m, 44)) / 2, gy + 120, m, 44, g_col[C_DIM]); }
    for (int r = 0; r < vis; r++) for (int c = 0; c < INV_COLS; c++) {
        int i = (g_inv_scroll[p] + r) * INV_COLS + c; if (i >= g_inv[p].n) break;
        const inv_item *x = &g_inv[p].it[i];
        int x0 = gx + c * INV_TILE + 4, y0 = gy + r * INV_TILE + 4, x1 = x0 + INV_TILE - 8, y1 = y0 + INV_TILE - 8, on = i == g_inv_sel[p];
        b_panel(b, x0, y0, x1, y1, 8, on ? g_col[C_TABOFF] : g_col[C_ROW], on ? g_col[C_TAB] : g_col[C_EDGE]);
        const uint32_t *ic = item_icon(x->name, INV_ICON);   /* the game's icon (bottom_assets.c) */
        if (ic) b_image(b, x0 + (x1 - x0 - INV_ICON) / 2, y0 + (y1 - y0 - INV_ICON) / 2, ic, INV_ICON, INV_ICON);
        else b_wrap(b, x0 + 8, y0 + 30, x1 - x0 - 16, item_name(x->name), 20, g_col[C_DIM], 22, y1 - 8);
        const uint32_t *fi = x->fuse[0] ? item_icon(x->fuse, INV_BADGE - 4) : NULL;
        if (fi) {   /* the fused material, small in the corner as in the game */
            b_panel(b, x1 - INV_BADGE - 4, y1 - INV_BADGE - 4, x1, y1, 6, g_col[C_SLOT], g_col[C_EDGE]);
            b_image(b, x1 - INV_BADGE - 2, y1 - INV_BADGE - 2, fi, INV_BADGE - 4, INV_BADGE - 4);
        } else if (!x->fuse[0]) if (g_pouch[p].v == V_STOCK && x->value != INT32_MIN && x->value >= 0) {   /* the count, on a shade */
            char v[16]; snprintf(v, sizeof v, "×%d", x->value); int w = text_width(v, 24);
            b_blend(b, x1 - w - 14, y1 - 30, x1 - 3, y1 - 3, g_col[C_SLOT], 190);
            b_text(b, x1 - w - 8, y1 - 9, v, 24, g_col[C_TEXT]);
        }
        if (on) b_cursor(b, x0, y0, x1, y1, g_col[C_TAB]);
    }
    inv_detail(b, p, g_inv[p].n ? &g_inv[p].it[g_inv_sel[p]] : NULL, gx + INV_GRID_W + 12, gy + 4, b->width - PAD, gy + vis * INV_TILE - 4);
    char foot[160]; struct tm tm; localtime_r(&g_inv_time, &tm);
    int rows = (g_inv[p].n + INV_COLS - 1) / INV_COLS;
    snprintf(foot, sizeof foot, "%d items   as of the last save, %02d:%02d%s", g_inv[p].n, tm.tm_hour, tm.tm_min, rows > vis ? "   swipe to scroll" : "");
    b_rule(b, PAD, b->width - PAD, b->height - INV_FOOT + 4, g_col[C_ACCENT], 90);
    b_text(b, PAD, b->height - 20, foot, 28, g_col[C_DIM]);
}
static void inv_tap(float x, float y, int width) {
    if (y >= INV_CHIP_Y && y < INV_CHIP_Y + INV_CHIP_H) {
        int p = (int)((x - PAD) / ((width - 2 * PAD) / INV_POUCHES));
        if (p >= 0 && p < INV_POUCHES && p != g_inv_tab) { g_inv_tab = p; bottom_redraw(); }
        return;
    }
    int c = (int)((x - PAD) / INV_TILE), r = (int)((y - INV_LIST_Y) / INV_TILE), p = g_inv_tab;
    if (x < PAD || y < INV_LIST_Y || c >= INV_COLS || r >= inv_rows_visible(g_bwin ? ANativeWindow_getHeight(g_bwin) : 1080)) return;
    int i = (g_inv_scroll[p] + r) * INV_COLS + c;
    if (i < g_inv[p].n && i != g_inv_sel[p]) { g_inv_sel[p] = i; bottom_redraw(); }
}
static void inv_drag(float dy, int height) {   /* a swipe: rows scroll with the finger */
    static float acc; acc += dy;
    int rows = (int)(acc / INV_TILE); if (!rows) return;
    acc -= rows * INV_TILE;
    int p = g_inv_tab, s = g_inv_scroll[p] - rows, m = inv_max_scroll(p, height);
    if (s < 0) s = 0; if (s > m) s = m;
    if (s != g_inv_scroll[p]) { g_inv_scroll[p] = s; bottom_redraw(); }
}
