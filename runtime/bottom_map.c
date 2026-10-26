/* The bottom screen's Map tab (android/BOTTOM_SCREEN.md steps A and C; 2026-10-23): the game's map under Link's
   marker, with pinch zoom, drag to pan, double-tap to zoom in, and "Whole map" / "Follow Link" buttons. Included by
   bottom_screen.c after bottom_assets.c.

   World x -6000..6000 (east), z -5000..5000 (south); north up; tile (c, r) covers x -6000+1000c, z -5000+1000r, 1000
   units square. The layer from the height until the game's own is read (Depths below -100, the sky above 1000). The
   whole-map picture (map_image, 100 px a tile) draws first; when a tile shows larger than that, the game's level 02
   (300 px), 01 (840 px) or 00 (3000 px, kept at 1500) tile is decoded on a thread into a cache (MAP_CACHE_BYTES) and
   drawn once ready. */
enum { MAP_W = 12000, MAP_H = 10000, MAP_MAX_PX_TILE = 1500, MAP_BAR = 70 };
static const struct { int level, px; } g_map_lv[3] = {{2, 300}, {1, 840}, {0, MAP_MAX_PX_TILE}};

/* ---------- the view: centre, scale (screen px a world unit), following Link; set by the touch thread ---------- */
static pthread_mutex_t g_mv_mu = PTHREAD_MUTEX_INITIALIZER;
static struct { float cx, cz, ppu; int follow, init; } g_mv;
static struct { int x0, y0, x1, y1; } g_mrect;   /* the map's area on the screen (the last draw) */
static float map_fit(void) { return fminf((float)(g_mrect.x1 - g_mrect.x0) / MAP_W, (float)(g_mrect.y1 - g_mrect.y0) / MAP_H); }
static void map_clamp(void) {   /* g_mv_mu held: the scale between the whole map and MAP_MAX_PX_TILE, the centre on the map */
    float fit = map_fit(), mx = MAP_MAX_PX_TILE / 1000.0f;
    if (g_mv.ppu < fit) g_mv.ppu = fit; if (g_mv.ppu > mx) g_mv.ppu = mx;
    float hw = (g_mrect.x1 - g_mrect.x0) / 2.0f / g_mv.ppu, hh = (g_mrect.y1 - g_mrect.y0) / 2.0f / g_mv.ppu;
    g_mv.cx = hw >= MAP_W / 2 ? 0 : fminf(fmaxf(g_mv.cx, -MAP_W / 2 + hw), MAP_W / 2 - hw);
    g_mv.cz = hh >= MAP_H / 2 ? 0 : fminf(fmaxf(g_mv.cz, -MAP_H / 2 + hh), MAP_H / 2 - hh);
}
static void map_zoom_at(float sx, float sy, float factor) {   /* the world point under (sx, sy) stays there */
    pthread_mutex_lock(&g_mv_mu);
    float mx = (g_mrect.x0 + g_mrect.x1) / 2.0f, my = (g_mrect.y0 + g_mrect.y1) / 2.0f;
    float wx = g_mv.cx + (sx - mx) / g_mv.ppu, wz = g_mv.cz + (sy - my) / g_mv.ppu;
    g_mv.ppu *= factor; map_clamp();
    if (!g_mv.follow) { g_mv.cx = wx - (sx - mx) / g_mv.ppu; g_mv.cz = wz - (sy - my) / g_mv.ppu; map_clamp(); }
    pthread_mutex_unlock(&g_mv_mu); bottom_redraw();
}
static void map_pan(float dx, float dy) {
    pthread_mutex_lock(&g_mv_mu); g_mv.follow = 0; g_mv.cx -= dx / g_mv.ppu; g_mv.cz -= dy / g_mv.ppu; map_clamp(); pthread_mutex_unlock(&g_mv_mu);
    bottom_redraw();
}

/* ---------- detailed tiles: a cache filled by one thread ---------- */
enum { MAP_SLOTS = 48 }; static const size_t MAP_CACHE_BYTES = 80u << 20;
typedef struct { int layer, li, c, r, state; uint32_t *px; int w, h; uint64_t used; } map_slot;   /* state 1 wanted, 2 ready, 3 missing */
static map_slot g_ms[MAP_SLOTS]; static pthread_mutex_t g_ms_mu = PTHREAD_MUTEX_INITIALIZER; static pthread_cond_t g_ms_cv = PTHREAD_COND_INITIALIZER;
static uint64_t g_ms_tick; static int g_ms_thread;
static void *map_tiles_thread(void *arg) {
    (void)arg;
    for (;;) {
        pthread_mutex_lock(&g_ms_mu);
        map_slot *job = NULL;
        for (;;) {   /* the most recently wanted first */
            for (int i = 0; i < MAP_SLOTS; i++) if (g_ms[i].state == 1 && (!job || g_ms[i].used > job->used)) job = &g_ms[i];
            if (job) break;
            pthread_cond_wait(&g_ms_cv, &g_ms_mu);
        }
        map_slot want = *job; pthread_mutex_unlock(&g_ms_mu);
        char p[128]; int w = 0, h = 0; uint32_t *px = NULL;
        const char *suffix = map_tile_suffix(want.layer, want.c, want.r);
        if (suffix) { snprintf(p, sizeof p, "UI/Map/MainField/%c_%02d-%02d_%02d%s.bntx.zs", "GSU"[want.layer], want.c, want.r, g_map_lv[want.li].level, suffix); px = asset_texture(p, &w, &h); }
        if (px && w > MAP_MAX_PX_TILE) {   /* level 00: kept at half size */
            uint32_t *s = malloc((size_t)MAP_MAX_PX_TILE * MAP_MAX_PX_TILE * 4);
            if (s) asset_scale(px, w, h, s, MAP_MAX_PX_TILE, MAP_MAX_PX_TILE, MAP_MAX_PX_TILE);
            free(px); px = s; w = h = MAP_MAX_PX_TILE;
        }
        if (px && want.layer == 2) depths_grey(px, (size_t)w * h);
        pthread_mutex_lock(&g_ms_mu);
        if (job->state == 1 && job->layer == want.layer && job->li == want.li && job->c == want.c && job->r == want.r) { job->px = px; job->w = w; job->h = h; job->state = px ? 2 : 3; px = NULL; }
        size_t total = 0; for (int i = 0; i < MAP_SLOTS; i++) if (g_ms[i].px) total += (size_t)g_ms[i].w * g_ms[i].h * 4;
        while (total > MAP_CACHE_BYTES) {   /* the least recently drawn go */
            map_slot *old = NULL; for (int i = 0; i < MAP_SLOTS; i++) if (g_ms[i].px && (!old || g_ms[i].used < old->used)) old = &g_ms[i];
            if (!old || old->used == g_ms_tick) break;
            total -= (size_t)old->w * old->h * 4; free(old->px); memset(old, 0, sizeof *old);
        }
        pthread_mutex_unlock(&g_ms_mu);
        free(px);
        bottom_redraw();
    }
    return NULL;
}
/* the tile if decoded (copied out under the lock: pointer, size); else it is asked for and NULL */
static const uint32_t *map_tile(int layer, int li, int c, int r, int *w, int *h) {
    pthread_mutex_lock(&g_ms_mu);
    if (!g_ms_thread) { g_ms_thread = 1; pthread_t th; if (!pthread_create(&th, NULL, map_tiles_thread, NULL)) pthread_detach(th); }
    map_slot *s = NULL, *free_s = NULL, *old = NULL;
    for (int i = 0; i < MAP_SLOTS; i++) {
        map_slot *m = &g_ms[i];
        if (m->state && m->layer == layer && m->li == li && m->c == c && m->r == r) { s = m; break; }
        if (!m->state) { if (!free_s) free_s = m; } else if (m->state != 2 && (!old || m->used < old->used)) old = m;
    }
    if (!s && !free_s && !old)   /* every slot holds a decoded tile: the least recently drawn one not drawn now goes */
        for (int i = 0; i < MAP_SLOTS; i++) if (g_ms[i].used != g_ms_tick && (!old || g_ms[i].used < old->used)) old = &g_ms[i];
    if (!s && (s = free_s ? free_s : old)) { free(s->px); *s = (map_slot){layer, li, c, r, 1, NULL, 0, 0, 0}; pthread_cond_signal(&g_ms_cv); }
    const uint32_t *px = NULL;
    if (s) { s->used = g_ms_tick; if (s->state == 2) { px = s->px; *w = s->w; *h = s->h; } }
    pthread_mutex_unlock(&g_ms_mu);
    return px;   /* stays valid for this draw: eviction skips tiles used in the current tick */
}

/* ---------- drawing ---------- */
static void map_draw(ANativeWindow_Buffer *b) {
    g_mrect.x0 = PAD; g_mrect.y0 = ROW_Y0; g_mrect.x1 = b->width - PAD; g_mrect.y1 = b->height - MAP_BAR;
    float p[3], yaw; char line[128]; int have = runtime_player_pose(p, &yaw);
    int layer = !have ? 0 : p[1] < -100 ? 2 : p[1] > 1000 ? 1 : 0;   /* ground, sky, Depths */
    pthread_mutex_lock(&g_mv_mu);
    if (!g_mv.init) { g_mv.init = 1; g_mv.ppu = map_fit(); g_mv.follow = 1; }
    if (g_mv.follow && have) { g_mv.cx = p[0]; g_mv.cz = p[2]; }
    map_clamp(); float cx = g_mv.cx, cz = g_mv.cz, ppu = g_mv.ppu; int follow = g_mv.follow;
    pthread_mutex_unlock(&g_mv_mu);
    int vx0 = g_mrect.x0, vy0 = g_mrect.y0, vx1 = g_mrect.x1, vy1 = g_mrect.y1;
    float mx = (vx0 + vx1) / 2.0f, my = (vy0 + vy1) / 2.0f;
    #define SX(wx) (mx + ((wx) - cx) * ppu)
    #define SY(wz) (my + ((wz) - cz) * ppu)
    b_fill(b, vx0, vy0, vx1, vy1, g_col[C_ROW]);
    const uint32_t *img = map_image(layer);
    float tile_px = 1000 * ppu; int li = -1;   /* the detail level a tile this large needs (-1: the whole-map picture) */
    if (tile_px > MAP_TILE * 1.2f) { li = 0; while (li < 2 && tile_px > g_map_lv[li].px * 1.1f) li++; }
    pthread_mutex_lock(&g_ms_mu); g_ms_tick++; pthread_mutex_unlock(&g_ms_mu);
    for (int r = 0; r < MAP_ROWS; r++) for (int c = 0; c < MAP_COLS; c++) {
        float tx0 = SX(c * 1000.0f - 6000), ty0 = SY(r * 1000.0f - 5000);
        int x0 = (int)fmaxf(tx0, vx0), y0 = (int)fmaxf(ty0, vy0), x1 = (int)fminf(tx0 + tile_px, vx1), y1 = (int)fminf(ty0 + tile_px, vy1);
        if (x0 >= x1 || y0 >= y1) continue;
        const uint32_t *src = NULL; int sw = 0, sh = 0, stride = 0;
        if (li >= 0 && img) { src = map_tile(layer, li, c, r, &sw, &sh); stride = sw; }
        if (!src && img) { src = img + (size_t)r * MAP_TILE * (MAP_COLS * MAP_TILE) + (size_t)c * MAP_TILE; sw = sh = MAP_TILE; stride = MAP_COLS * MAP_TILE; }
        if (!src) continue;
        for (int y = y0; y < y1; y++) {
            int sy = (int)((y - ty0) * sh / tile_px); if (sy < 0) sy = 0; if (sy >= sh) sy = sh - 1;
            uint32_t *row = (uint32_t *)b->bits + (size_t)y * b->stride; const uint32_t *srow = src + (size_t)sy * stride;
            for (int x = x0; x < x1; x++) {
                int sx = (int)((x - tx0) * sw / tile_px); if (sx < 0) sx = 0; if (sx >= sw) sx = sw - 1;
                uint32_t v = srow[sx]; if (v >> 24) row[x] = v | 0xff000000u;
            }
        }
    }
    if (!img) b_text(b, (int)mx - text_width("Loading the map...", 36) / 2, (int)my, "Loading the map...", 36, g_col[C_DIM]);
    /* the bar: where Link is, and the buttons */
    b_fill(b, 0, vy1, b->width, b->height, g_col[C_BG]);
    for (int k = 0; k < 4; k++) {   /* the map's frame: the corners cut off the map, then the edge */
        float cx = k & 1 ? vx1 + 4 : vx0 - 4, cy = k & 2 ? vy1 + 4 : vy0 - 4, sx = k & 1 ? -1 : 1, sy = k & 2 ? -1 : 1;
        b_tri(b, (float[]){cx, cx + sx * 12, cx}, (float[]){cy, cy, cy + sy * 12}, g_col[C_BG]);
    }
    b_panel(b, vx0 - 4, vy0 - 4, vx1 + 4, vy1 + 4, 10, 0, g_col[C_EDGE]);
    const char *bt[2] = {"Whole map", "Follow Link"}; int bx = b->width - PAD;
    for (int i = 1; i >= 0; i--) {
        int w = text_width(bt[i], 30) + 40, on = i == 1 && follow; bx -= w;
        b_panel(b, bx, vy1 + 14, bx + w, b->height - 8, 8, on ? g_col[C_TABOFF] : g_col[C_PILL], on ? g_col[C_TAB] : g_col[C_EDGE]);
        if (on) b_cursor(b, bx, vy1 + 14, bx + w, b->height - 8, g_col[C_TAB]);
        b_text(b, bx + 20, b->height - 22, bt[i], 30, on ? g_col[C_TEXT] : g_col[C_DIM]); bx -= 12;
    }
    if (!have) { b_text(b, PAD, b->height - 22, "Link isn't in the world right now", 30, g_col[C_DIM]); return; }
    float lx = SX(p[0]), ly = SY(p[2]), fx = sinf(yaw), fz = cosf(yaw);   /* forward on the map: (x, z) */
    if (lx >= vx0 && lx < vx1 && ly >= vy0 && ly < vy1) {   /* the game's marker: a gold arrowhead outlined in black */
        for (int k = 1; k >= 0; k--) {
            float s = k ? 1.35f : 1.0f, tx[3] = {lx + fx * 26 * s, lx + (-fx * 12 + fz * 15) * s, lx + (-fx * 12 - fz * 15) * s},
                  ty[3] = {ly + fz * 26 * s, ly + (-fz * 12 - fx * 15) * s, ly + (-fz * 12 + fx * 15) * s};
            b_tri(b, tx, ty, k ? g_col[C_SLOT] : g_col[C_ACCENT]);
        }
    }
    const char *ln = p[1] < -100 ? "Depths" : p[1] > 1000 ? "Sky" : "Surface";
    b_diamond(b, PAD + 7, b->height - 33, 5, g_col[C_ACCENT]);
    b_text(b, PAD + 26, b->height - 22, ln, 30, g_col[C_ACCENT]);
    snprintf(line, sizeof line, "x %.0f   y %.0f   z %.0f", p[0], p[1], p[2]);
    b_text(b, PAD + 26 + text_width(ln, 30) + 24, b->height - 22, line, 30, g_col[C_DIM]);
    #undef SX
    #undef SY
}

/* ---------- touch (the activity's thread) ---------- */
static void map_tap(float x, float y, int width, int height) {
    if (y >= height - MAP_BAR) {   /* the buttons, laid out as map_draw does */
        int bx = width - PAD, w1 = text_width("Follow Link", 30) + 40, w0 = text_width("Whole map", 30) + 40;
        if (x >= bx - w1 && x < bx) { pthread_mutex_lock(&g_mv_mu); g_mv.follow = 1; pthread_mutex_unlock(&g_mv_mu); bottom_redraw(); }
        else if (x >= bx - w1 - 12 - w0 && x < bx - w1 - 12) { pthread_mutex_lock(&g_mv_mu); g_mv.follow = 0; g_mv.ppu = 0; map_clamp(); pthread_mutex_unlock(&g_mv_mu); bottom_redraw(); }
        return;
    }
    static double last; static float lx, ly; double now = since_boot();
    if (now - last < 0.35 && fabsf(x - lx) < 80 && fabsf(y - ly) < 80) { map_zoom_at(x, y, 2.0f); last = 0; return; }   /* double tap */
    last = now; lx = x; ly = y;
}
