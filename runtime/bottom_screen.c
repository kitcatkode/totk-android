/* The second screen (TOTK, 2026-10-23): the AYN Thor's bottom display (Android display 4, "Screen-2", 1240x1080,
   FLAG_PRESENTATION) shows the app's own UI. The APK declares an activity-alias "<package>.Bottom" of the same
   NativeActivity and library (android/apk/build_apk.py); the game's activity starts it on the first presentation
   display, ANativeActivity_onCreate tells the two apart by the component name, and this file runs the alias: it draws
   into its own window (software, only when something changed, and once a second for the stats line) and takes its own
   input queue, where touch goes to the UI and every gamepad event to the game (handle_input), so the controller keeps
   working whichever screen has focus. Text: stb_truetype (third_party/stb, public domain / MIT) with the system's
   RobotoStatic-Regular.ttf. Included into frontend_android.c (its statics). TOTK_BOTTOM=0: no second screen.

   The Settings tab edits totk.env in the game folder (the game reads it at start): each row cycles its values on a
   tap. The frame generation rows also apply at once (settings_apply); a row whose saved value differs from the one the
   running game uses says "applies after a restart". Keys of
   totk.env the screen does not manage are kept as they are. The Map tab shows Link on a grid (android/BOTTOM_SCREEN.md
   step A); Inventory lists the pouches as of the newest save (bottom_inventory.c, step D). */
#include <android/native_window.h>
#include <math.h>
#include "fs_host.h"
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#define STBTT_STATIC
#define STB_TRUETYPE_IMPLEMENTATION
#include "../third_party/stb/stb_truetype.h"
#pragma GCC diagnostic pop

static ANativeActivity *g_bottom;              /* the alias's activity (NULL when none) */
static ANativeWindow *g_bwin;                  /* its window */
static pthread_mutex_t g_bmu = PTHREAD_MUTEX_INITIALIZER; static pthread_cond_t g_bcv = PTHREAD_COND_INITIALIZER;
static int g_bdirty = 1;                       /* redraw wanted */

static jstring jstr(JNIEnv *env, const char *s) { return (*env)->NewStringUTF(env, s); }
static int jok(JNIEnv *env, const char *what) {   /* clears a pending Java exception; 0 when there was one */
    if (!(*env)->ExceptionCheck(env)) return 1;
    (*env)->ExceptionClear(env); fprintf(stderr, "[bottom] %s failed (Java exception)\n", what); return 0;
}

/* Is this activity the alias? (getIntent().getComponent().getClassName() ends with ".Bottom") */
static int bottom_is(ANativeActivity *a) {
    JNIEnv *env = a->env; jobject act = a->clazz; int is = 0;
    jclass ac = (*env)->GetObjectClass(env, act);
    jobject it = (*env)->CallObjectMethod(env, act, (*env)->GetMethodID(env, ac, "getIntent", "()Landroid/content/Intent;"));
    if (jok(env, "getIntent") && it) {
        jobject cn = (*env)->CallObjectMethod(env, it, (*env)->GetMethodID(env, (*env)->GetObjectClass(env, it), "getComponent", "()Landroid/content/ComponentName;"));
        if (jok(env, "getComponent") && cn) {
            jstring s = (*env)->CallObjectMethod(env, cn, (*env)->GetMethodID(env, (*env)->GetObjectClass(env, cn), "getClassName", "()Ljava/lang/String;"));
            if (jok(env, "getClassName") && s) { const char *c = (*env)->GetStringUTFChars(env, s, NULL); size_t n = strlen(c); is = n > 7 && !strcmp(c + n - 7, ".Bottom"); (*env)->ReleaseStringUTFChars(env, s, c); }
        }
    }
    return is;
}

/* From the game's activity: start the alias on the first presentation display. */
static void bottom_launch(ANativeActivity *a) {
    const char *e = getenv("TOTK_BOTTOM"); if (e && !strcmp(e, "0")) { fprintf(stderr, "[bottom] off (TOTK_BOTTOM=0)\n"); return; }
    JNIEnv *env = a->env; jobject act = a->clazz;
    jclass ac = (*env)->GetObjectClass(env, act);
    /* DisplayManager.getDisplays("android.hardware.display.category.PRESENTATION")[0].getDisplayId() */
    jobject dm = (*env)->CallObjectMethod(env, act, (*env)->GetMethodID(env, ac, "getSystemService", "(Ljava/lang/String;)Ljava/lang/Object;"), jstr(env, "display"));
    if (!jok(env, "getSystemService") || !dm) return;
    jobjectArray ds = (*env)->CallObjectMethod(env, dm, (*env)->GetMethodID(env, (*env)->GetObjectClass(env, dm), "getDisplays", "(Ljava/lang/String;)[Landroid/view/Display;"),
                                               jstr(env, "android.hardware.display.category.PRESENTATION"));
    if (!jok(env, "getDisplays") || !ds || (*env)->GetArrayLength(env, ds) < 1) { fprintf(stderr, "[bottom] no presentation display: no second screen\n"); return; }
    jobject d = (*env)->GetObjectArrayElement(env, ds, 0);
    jint id = (*env)->CallIntMethod(env, d, (*env)->GetMethodID(env, (*env)->GetObjectClass(env, d), "getDisplayId", "()I"));
    if (!jok(env, "getDisplayId")) return;
    /* new Intent().setClassName(activity, package + ".Bottom").addFlags(NEW_TASK | MULTIPLE_TASK) */
    jstring pkg = (*env)->CallObjectMethod(env, act, (*env)->GetMethodID(env, ac, "getPackageName", "()Ljava/lang/String;"));
    if (!jok(env, "getPackageName")) return;
    const char *p = (*env)->GetStringUTFChars(env, pkg, NULL); char cls[256]; snprintf(cls, sizeof cls, "%s.Bottom", p); (*env)->ReleaseStringUTFChars(env, pkg, p);
    jclass ic = (*env)->FindClass(env, "android/content/Intent");
    jobject it = (*env)->NewObject(env, ic, (*env)->GetMethodID(env, ic, "<init>", "()V"));
    (*env)->CallObjectMethod(env, it, (*env)->GetMethodID(env, ic, "setClassName", "(Landroid/content/Context;Ljava/lang/String;)Landroid/content/Intent;"), act, jstr(env, cls));
    (*env)->CallObjectMethod(env, it, (*env)->GetMethodID(env, ic, "addFlags", "(I)Landroid/content/Intent;"), (jint)(0x10000000 | 0x08000000));
    if (!jok(env, "Intent")) return;
    /* ActivityOptions.makeBasic().setLaunchDisplayId(id).toBundle() */
    jclass oc = (*env)->FindClass(env, "android/app/ActivityOptions");
    jobject o = (*env)->CallStaticObjectMethod(env, oc, (*env)->GetStaticMethodID(env, oc, "makeBasic", "()Landroid/app/ActivityOptions;"));
    (*env)->CallObjectMethod(env, o, (*env)->GetMethodID(env, oc, "setLaunchDisplayId", "(I)Landroid/app/ActivityOptions;"), id);
    jobject bundle = (*env)->CallObjectMethod(env, o, (*env)->GetMethodID(env, oc, "toBundle", "()Landroid/os/Bundle;"));
    if (!jok(env, "ActivityOptions")) return;
    (*env)->CallVoidMethod(env, act, (*env)->GetMethodID(env, ac, "startActivity", "(Landroid/content/Intent;Landroid/os/Bundle;)V"), it, bundle);
    if (jok(env, "startActivity")) fprintf(stderr, "[bottom] started %s on display %d\n", cls, (int)id);
}

/* ---------- settings: totk.env keys the screen manages ---------- */
enum { NOPT = 4 };
typedef struct {
    const char *label, *key;
    int n; const char *name[NOPT], *val[NOPT];   /* val NULL: the key is removed (the app's default) */
    int live;                                    /* applied to the running game at once (settings_apply), no restart */
    int sel, run;                                /* the saved choice; the one the running game uses (-1: other) */
} b_setting;
static b_setting g_set[] = {
    {"Frame generation", "TOTK_LSFG", 3, {"Off", "2x", "3x"}, {NULL, "2", "3"}, 1},
    {"Frame generation mode", "TOTK_LSFG_PERF", 2, {"Quality", "Performance"}, {NULL, "1"}, 1},
    {"HUD in frame generation", "TOTK_LSFG_HUD", 2, {"Kept out", "Mixed in"}, {NULL, "0"}, 1},
    {"Frame generation flow scale", "TOTK_LSFG_FLOW", 2, {"Full", "Half (lighter)"}, {NULL, "0.5"}, 1},
    {"Frame cap", "TOTK_UNCAPPED_HZ", 3, {"30 fps", "40 fps", "60 fps"}, {"30", "40", "60"}},
    {"Render resolution", "TOTK_RENDER_RES", 3, {"720p (native)", "612p", "540p"}, {NULL, "1088x612", "960x540"}},
    {"Shadow resolution", "TOTK_SHADOW_RES", 2, {"720 (native)", "1024 (sharper)"}, {NULL, "1024"}},   /* game_patches_v143.c patch_shadow_res: geometry-bound on the Thor, 1024 costs ~0.2 ms of GPU, 512 saves nothing */
    {"Gyro aiming", "TOTK_GYRO", 2, {"On", "Off"}, {NULL, "0"}},
};
enum { NSET = sizeof g_set / sizeof *g_set };
static int opt_of(const b_setting *s, const char *v) {   /* which option a value is (-1: none of them) */
    for (int i = 0; i < s->n; i++) if (s->val[i] ? v && !strcmp(v, s->val[i]) : !v || !*v) return i;
    if (!v || !*v) return 0;
    return -1;
}
static void env_path(char *out, size_t n) { snprintf(out, n, "%s/totk.env", g_data); }
static char *env_read(void) {   /* totk.env as a string (empty if none) */
    char path[1100]; env_path(path, sizeof path);
    FILE *f = fopen(path, "rb"); if (!f) return strdup("");
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    char *s = malloc((size_t)n + 1); size_t got = fread(s, 1, (size_t)n, f); s[got] = 0; fclose(f);
    return s;
}
static int env_value(const char *text, const char *key, char *out, size_t n) {   /* the last KEY=value line's value */
    size_t kl = strlen(key); int found = 0;
    for (const char *l = text; *l; ) {
        const char *e = strchr(l, '\n'); size_t len = e ? (size_t)(e - l) : strlen(l);
        if (len > kl && !strncmp(l, key, kl) && l[kl] == '=') { size_t vl = len - kl - 1; if (vl && l[kl + vl] == '\r') vl--; if (vl >= n) vl = n - 1; memcpy(out, l + kl + 1, vl); out[vl] = 0; found = 1; }
        l = e ? e + 1 : l + len;
    }
    return found;
}
static void settings_load(void) {
    char *text = env_read();
    for (int i = 0; i < NSET; i++) {
        b_setting *s = &g_set[i]; char v[128];
        s->sel = opt_of(s, env_value(text, s->key, v, sizeof v) ? v : NULL); if (s->sel < 0) s->sel = 0;
        s->run = opt_of(s, getenv(s->key));   /* what the game started with (totk.env was applied then) */
    }
    free(text);
}
/* Write a setting into totk.env: every line of that key is dropped, the new one appended (none for the default);
   the other lines stay. Through a temporary file, so a crash never leaves half a file. */
static void settings_save(const b_setting *s) {
    char *text = env_read(), path[1100], tmp[1110]; env_path(path, sizeof path); snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "wb"); if (!f) { fprintf(stderr, "[bottom] cannot write %s: %s\n", tmp, strerror(errno)); free(text); return; }
    size_t kl = strlen(s->key);
    for (const char *l = text; *l; ) {
        const char *e = strchr(l, '\n'); size_t len = e ? (size_t)(e - l) : strlen(l);
        if (!(len > kl && !strncmp(l, s->key, kl) && l[kl] == '=') && len) { fwrite(l, 1, len, f); fputc('\n', f); }
        l = e ? e + 1 : l + len;
    }
    if (s->val[s->sel]) fprintf(f, "%s=%s\n", s->key, s->val[s->sel]);
    int bad = fclose(f) != 0;
    if (bad || rename(tmp, path)) fprintf(stderr, "[bottom] cannot replace %s: %s\n", path, strerror(errno));
    else fprintf(stderr, "[bottom] totk.env: %s = %s\n", s->key, s->val[s->sel] ? s->val[s->sel] : "(default)");
    free(text);
}

/* The live rows to the running game: frame generation restarts itself with them at its next frame (android/gfx/vk_fg.c;
   weak, for a build without that backend). */
void vk_fg_configure(int mult, int perf, int hud, int flow_pct) __attribute__((weak));
uint64_t vk_window_posts(void) __attribute__((weak));   /* pictures shown (the stats line) */
static void settings_apply(void) {
    if (!vk_fg_configure) return;
    static const int mult[] = {1, 2, 3};
    vk_fg_configure(mult[g_set[0].sel], g_set[1].sel == 1, g_set[2].sel == 0, g_set[3].sel == 1 ? 50 : 100);
    for (int i = 0; i < NSET; i++) if (g_set[i].live) g_set[i].run = g_set[i].sel;
}

/* ---------- drawing (software, into the window's buffer: RGBX bytes, so a pixel is 0xXXBBGGRR) ---------- */
#define RGB(r, g, b) (0xff000000u | (uint32_t)(b) << 16 | (uint32_t)(g) << 8 | (uint32_t)(r))
enum { C_BG = 0, C_BAR, C_TAB, C_TABOFF, C_ROW, C_TEXT, C_DIM, C_ACCENT, C_PILL, C_WARN, C_N };
static const uint32_t g_col[C_N] = {RGB(18, 20, 26), RGB(30, 33, 42), RGB(70, 132, 220), RGB(44, 48, 60), RGB(28, 31, 40),
                                     RGB(236, 238, 242), RGB(140, 146, 160), RGB(110, 170, 255), RGB(52, 58, 74), RGB(240, 180, 80)};
static stbtt_fontinfo g_font; static unsigned char *g_font_data; static int g_font_ok = -1;
static void font_load(void) {
    if (g_font_ok >= 0) return;
    g_font_ok = 0;
    const char *paths[] = {"/system/fonts/RobotoStatic-Regular.ttf", "/system/fonts/Roboto-Regular.ttf", "/system/fonts/DroidSans.ttf"};
    for (unsigned i = 0; i < sizeof paths / sizeof *paths && !g_font_ok; i++) {
        FILE *f = fopen(paths[i], "rb"); if (!f) continue;
        fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
        g_font_data = malloc((size_t)n); if (fread(g_font_data, 1, (size_t)n, f) == (size_t)n && stbtt_InitFont(&g_font, g_font_data, stbtt_GetFontOffsetForIndex(g_font_data, 0))) g_font_ok = 1;
        fclose(f);
        if (g_font_ok) fprintf(stderr, "[bottom] font %s\n", paths[i]); else { free(g_font_data); g_font_data = NULL; }
    }
    if (!g_font_ok) fprintf(stderr, "[bottom] no system font: no text\n");
}
/* a glyph cache per (character, pixel size): sizes are few (the layout's) */
typedef struct { int size, ch, w, h, xo, yo, adv; unsigned char *bm; } glyph;
static glyph g_gl[512]; static int g_ngl;
static const glyph *glyph_get(int ch, int size) {
    for (int i = 0; i < g_ngl; i++) if (g_gl[i].ch == ch && g_gl[i].size == size) return &g_gl[i];
    if (g_ngl == 512) return NULL;
    glyph *g = &g_gl[g_ngl++]; float sc = stbtt_ScaleForPixelHeight(&g_font, (float)size); int adv, lsb;
    g->size = size; g->ch = ch; g->bm = stbtt_GetCodepointBitmap(&g_font, sc, sc, ch, &g->w, &g->h, &g->xo, &g->yo);
    stbtt_GetCodepointHMetrics(&g_font, ch, &adv, &lsb); g->adv = (int)(adv * sc + 0.5f);
    return g;
}
static int utf8_next(const char **ps) {   /* the next code point (0 at the end); a broken sequence reads as '?' */
    const unsigned char *s = (const unsigned char *)*ps; int c = *s;
    if (!c) return 0;
    int n = c < 0x80 ? 0 : (c & 0xe0) == 0xc0 ? 1 : (c & 0xf0) == 0xe0 ? 2 : (c & 0xf8) == 0xf0 ? 3 : -1;
    if (n < 0) { *ps += 1; return '?'; }
    c &= (0x7f >> n) >> (n ? 1 : 0);
    for (int i = 1; i <= n; i++) { if ((s[i] & 0xc0) != 0x80) { *ps += i; return '?'; } c = c << 6 | (s[i] & 0x3f); }
    *ps += n + 1; return c;
}
static int text_width(const char *s, int size) { int w = 0, ch; if (g_font_ok > 0) while ((ch = utf8_next(&s))) { const glyph *g = glyph_get(ch, size); if (g) w += g->adv; } return w; }
static void b_fill(ANativeWindow_Buffer *b, int x0, int y0, int x1, int y1, uint32_t c) {
    if (x0 < 0) x0 = 0; if (y0 < 0) y0 = 0; if (x1 > b->width) x1 = b->width; if (y1 > b->height) y1 = b->height;
    for (int y = y0; y < y1; y++) { uint32_t *row = (uint32_t *)b->bits + (size_t)y * b->stride; for (int x = x0; x < x1; x++) row[x] = c; }
}
static void b_text(ANativeWindow_Buffer *b, int x, int baseline, const char *s, int size, uint32_t c) {   /* alpha-blended */
    if (g_font_ok <= 0) return;
    for (int ch; (ch = utf8_next(&s));) {
        const glyph *g = glyph_get(ch, size); if (!g) continue;
        for (int j = 0; j < g->h; j++) {
            int y = baseline + g->yo + j; if (y < 0 || y >= b->height) continue;
            uint32_t *row = (uint32_t *)b->bits + (size_t)y * b->stride;
            for (int i = 0; i < g->w; i++) {
                int px = x + g->xo + i; unsigned a = g->bm[j * g->w + i]; if (px < 0 || px >= b->width || !a) continue;
                uint32_t d = row[px]; uint32_t o = 0xff000000u;
                for (int k = 0; k < 24; k += 8) { unsigned dv = d >> k & 255, sv = c >> k & 255; o |= ((sv * a + dv * (255 - a)) / 255) << k; }
                row[px] = o;
            }
        }
        x += g->adv;
    }
}
/* layout (the window is 1240x1080) */
enum { TAB_H = 110, STATS_H = 70, ROW_Y0 = TAB_H + STATS_H + 10, ROW_H = 104, ROW_GAP = 4, PAD = 32 };   /* 8 rows fit the 1080-pixel screen */
static const char *g_tabs[] = {"Settings", "Map", "Inventory"};
static int g_tab;   /* Settings, Map (bottom_map.c), Inventory (bottom_inventory.c) */
int runtime_player_pose(float xyz[3], float *yaw);   /* world_survey.c: read-only */
static void b_tri(ANativeWindow_Buffer *b, const float x[3], const float y[3], uint32_t c) {   /* filled, by scanlines */
    float y0 = fminf(y[0], fminf(y[1], y[2])), y1 = fmaxf(y[0], fmaxf(y[1], y[2]));
    for (int py = (int)ceilf(y0); py <= (int)y1; py++) {
        float lo = 1e9f, hi = -1e9f;
        for (int e = 0; e < 3; e++) {
            float ax = x[e], ay = y[e], bx = x[(e + 1) % 3], by = y[(e + 1) % 3];
            if ((py < ay) == (py < by)) continue;
            float t = (py - ay) / (by - ay), xx = ax + t * (bx - ax); lo = fminf(lo, xx); hi = fmaxf(hi, xx);
        }
        if (lo <= hi) b_fill(b, (int)lo, py, (int)hi + 1, py + 1, c);
    }
}
static void bottom_redraw(void);
#include "bottom_assets.c"
#include "bottom_map.c"
#include "bottom_inventory.c"
static void bottom_draw(ANativeWindow *w) {
    ANativeWindow_Buffer b;
    if (ANativeWindow_lock(w, &b, NULL)) return;
    font_load();
    b_fill(&b, 0, 0, b.width, b.height, g_col[C_BG]);
    /* tab bar */
    b_fill(&b, 0, 0, b.width, TAB_H, g_col[C_BAR]);
    int tw = b.width / 3;
    for (int i = 0; i < 3; i++) {
        int x0 = i * tw; uint32_t tc = i == g_tab ? g_col[C_TEXT] : g_col[C_DIM];
        if (i == g_tab) b_fill(&b, x0 + 20, TAB_H - 8, x0 + tw - 20, TAB_H, g_col[C_TAB]);
        b_text(&b, x0 + (tw - text_width(g_tabs[i], 44)) / 2, 70, g_tabs[i], 44, tc);
    }
    /* stats: the game's frames a second (the presents counted by frontend_headless.c) and the pictures shown, generated
       ones included (the window's posts, android/gfx/vk_draw.c; weak, for a build without that backend) */
    { static uint64_t last_n, last_p, last_t; static double fps, shown;
      uint64_t n = atomic_load(&presented), p = vk_window_posts ? vk_window_posts() : 0, t = (uint64_t)(since_boot() * 1000);
      if (last_t && t > last_t) { fps = (double)(n - last_n) * 1000.0 / (double)(t - last_t); shown = (double)(p - last_p) * 1000.0 / (double)(t - last_t); }
      last_n = n; last_p = p; last_t = t;
      char s[160];
      if (fps <= 0.5) snprintf(s, sizeof s, "starting...");
      else if (shown > fps * 1.3) snprintf(s, sizeof s, "%.0f fps   %.0f shown with frame generation   %.1f ms a frame", fps, shown, 1000.0 / fps);
      else snprintf(s, sizeof s, "%.0f fps   %.1f ms a frame", fps, 1000.0 / fps);
      b_text(&b, PAD, TAB_H + 48, s, 36, g_col[C_DIM]); }
    if (g_tab == 0) {
        for (int i = 0; i < NSET; i++) {
            const b_setting *s = &g_set[i]; int y0 = ROW_Y0 + i * (ROW_H + ROW_GAP);
            if (y0 + ROW_H > b.height) break;
            b_fill(&b, PAD, y0, b.width - PAD, y0 + ROW_H, g_col[C_ROW]);
            b_text(&b, PAD + 28, y0 + 52, s->label, 38, g_col[C_TEXT]);
            int pending = s->run != s->sel;
            b_text(&b, PAD + 28, y0 + 94, pending ? "applies after a restart" : "", 26, g_col[C_WARN]);
            const char *v = s->name[s->sel]; int vw = text_width(v, 38), px1 = b.width - PAD - 28, px0 = px1 - vw - 56;
            b_fill(&b, px0, y0 + 24, px1, y0 + ROW_H - 24, g_col[C_PILL]);
            b_text(&b, px0 + 28, y0 + 70, v, 38, g_col[C_ACCENT]);
        }
    } else {
        if (g_tab == 1) map_draw(&b);
        else if (g_tab == 2) inv_draw(&b);
        else { const char *m = "Coming later"; b_text(&b, (b.width - text_width(m, 48)) / 2, b.height / 2, m, 48, g_col[C_DIM]); }
    }
    ANativeWindow_unlockAndPost(w);
}
static void bottom_redraw(void) { pthread_mutex_lock(&g_bmu); g_bdirty = 1; pthread_cond_signal(&g_bcv); pthread_mutex_unlock(&g_bmu); }
static void *bottom_thread(void *arg) {
    (void)arg;
    for (;;) {
        pthread_mutex_lock(&g_bmu);
        while (!g_bwin) pthread_cond_wait(&g_bcv, &g_bmu);
        if (!g_bdirty) {   /* the stats line: once a second; the map 10 times */
            struct timespec dl; clock_gettime(CLOCK_REALTIME, &dl);
            if (g_tab == 1) { dl.tv_nsec += 100000000; if (dl.tv_nsec >= 1000000000) { dl.tv_sec++; dl.tv_nsec -= 1000000000; } } else dl.tv_sec += 1;
            pthread_cond_timedwait(&g_bcv, &g_bmu, &dl);
        }
        ANativeWindow *w = g_bwin; if (!w) { pthread_mutex_unlock(&g_bmu); continue; }
        ANativeWindow_acquire(w); g_bdirty = 0;
        pthread_mutex_unlock(&g_bmu);
        bottom_draw(w);
        ANativeWindow_release(w);
    }
    return NULL;
}
static void bottom_tap(float x, float y) {
    int width = g_bwin ? ANativeWindow_getWidth(g_bwin) : 1240;
    if (y < TAB_H) { int t = (int)(x * 3 / (width ? width : 1)); if (t >= 0 && t < 3 && t != g_tab) { g_tab = t; bottom_redraw(); } return; }
    if (g_tab == 2) { inv_tap(x, y, width); return; }
    if (g_tab == 1) { map_tap(x, y, width, g_bwin ? ANativeWindow_getHeight(g_bwin) : 1080); return; }
    if (g_tab != 0) return;
    int i = ((int)y - ROW_Y0) / (ROW_H + ROW_GAP);
    if (y < ROW_Y0 || i < 0 || i >= NSET || (int)y - ROW_Y0 - i * (ROW_H + ROW_GAP) >= ROW_H) return;
    b_setting *s = &g_set[i];
    s->sel = (s->sel + 1) % s->n;
    settings_save(s);
    if (s->live) settings_apply();
    bottom_redraw();
}

/* ---------- the alias's callbacks ---------- */
static void b_window_created(ANativeActivity *a, ANativeWindow *w) {
    (void)a;
    ANativeWindow_setBuffersGeometry(w, 0, 0, WINDOW_FORMAT_RGBX_8888);
    pthread_mutex_lock(&g_bmu); if (g_bwin) ANativeWindow_release(g_bwin); g_bwin = w; ANativeWindow_acquire(w); g_bdirty = 1; pthread_cond_signal(&g_bcv); pthread_mutex_unlock(&g_bmu);
    fprintf(stderr, "[bottom] window %dx%d\n", ANativeWindow_getWidth(w), ANativeWindow_getHeight(w));
}
static void b_window_destroyed(ANativeActivity *a, ANativeWindow *w) {
    (void)a; (void)w;
    pthread_mutex_lock(&g_bmu); if (g_bwin) ANativeWindow_release(g_bwin); g_bwin = NULL; pthread_mutex_unlock(&g_bmu);
}
static int b_queue_cb(int fd, int events, void *data) {
    (void)fd; (void)events; AInputQueue *q = data; AInputEvent *ev;
    while (AInputQueue_getEvent(q, &ev) >= 0) {
        if (AInputQueue_preDispatchEvent(q, ev)) continue;
        int handled = 0;
        if (AInputEvent_getType(ev) == AINPUT_EVENT_TYPE_MOTION && (AInputEvent_getSource(ev) & AINPUT_SOURCE_TOUCHSCREEN) == AINPUT_SOURCE_TOUCHSCREEN) {
            /* one finger: a tap, or a drag (the inventory scrolls, the map pans); two: the map zooms (pinch) */
            static float dx, dy, lx, ly, pd; static int multi;
            int act = AMotionEvent_getAction(ev) & AMOTION_EVENT_ACTION_MASK, n = (int)AMotionEvent_getPointerCount(ev);
            int height = g_bwin ? ANativeWindow_getHeight(g_bwin) : 1080;
            if (act == AMOTION_EVENT_ACTION_DOWN) { lx = dx = AMotionEvent_getX(ev, 0); ly = dy = AMotionEvent_getY(ev, 0); multi = 0; pd = 0; }
            else if (act == AMOTION_EVENT_ACTION_POINTER_DOWN && n >= 2) {
                multi = 1; pd = hypotf(AMotionEvent_getX(ev, 0) - AMotionEvent_getX(ev, 1), AMotionEvent_getY(ev, 0) - AMotionEvent_getY(ev, 1));
            } else if (act == AMOTION_EVENT_ACTION_POINTER_UP) {   /* go on with the finger that stays */
                int up = (AMotionEvent_getAction(ev) & AMOTION_EVENT_ACTION_POINTER_INDEX_MASK) >> AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT, k = up == 0 ? 1 : 0;
                lx = AMotionEvent_getX(ev, (size_t)k); ly = AMotionEvent_getY(ev, (size_t)k); pd = 0;
            } else if (act == AMOTION_EVENT_ACTION_MOVE) {
                if (n >= 2) {
                    float x0 = AMotionEvent_getX(ev, 0), y0 = AMotionEvent_getY(ev, 0), x1 = AMotionEvent_getX(ev, 1), y1 = AMotionEvent_getY(ev, 1), d = hypotf(x0 - x1, y0 - y1);
                    if (g_tab == 1 && pd > 10 && d > 10) map_zoom_at((x0 + x1) / 2, (y0 + y1) / 2, d / pd);
                    pd = d;
                } else {
                    float mx = AMotionEvent_getX(ev, 0), my = AMotionEvent_getY(ev, 0);
                    if (g_tab == 2) inv_drag(my - ly, height);
                    if (g_tab == 1 && dy < height - MAP_BAR) map_pan(mx - lx, my - ly);
                    lx = mx; ly = my;
                }
            } else if (act == AMOTION_EVENT_ACTION_UP) {   /* a tap: down and up close together, one finger throughout */
                float ux = AMotionEvent_getX(ev, 0), uy = AMotionEvent_getY(ev, 0);
                if (!multi && (ux - dx) * (ux - dx) + (uy - dy) * (uy - dy) < 40 * 40) bottom_tap(ux, uy);
            }
            handled = 1;
        } else handled = handle_input(ev);   /* the controller drives the game from either screen */
        AInputQueue_finishEvent(q, ev, handled);
    }
    return 1;
}
static void b_queue_created(ANativeActivity *a, AInputQueue *q) { (void)a; AInputQueue_attachLooper(q, ALooper_forThread(), 2, b_queue_cb, q); }
static void b_queue_destroyed(ANativeActivity *a, AInputQueue *q) { (void)a; AInputQueue_detachLooper(q); }
static void b_focus(ANativeActivity *a, int focused) { if (focused) immersive(a); }
static void b_destroy(ANativeActivity *a) { (void)a; g_bottom = NULL; fprintf(stderr, "[bottom] activity destroyed\n"); }
static void bottom_create(ANativeActivity *a) {
    g_bottom = a;
    a->callbacks->onNativeWindowCreated = b_window_created;
    a->callbacks->onNativeWindowDestroyed = b_window_destroyed;
    a->callbacks->onInputQueueCreated = b_queue_created;
    a->callbacks->onInputQueueDestroyed = b_queue_destroyed;
    a->callbacks->onWindowFocusChanged = b_focus;
    a->callbacks->onDestroy = b_destroy;
    ANativeActivity_setWindowFlags(a, AWINDOW_FLAG_FULLSCREEN, 0);
    immersive(a);
    settings_load();
    static int started; if (!started) { started = 1; pthread_t t; if (!pthread_create(&t, NULL, bottom_thread, NULL)) pthread_detach(t); }
    fprintf(stderr, "[bottom] activity created\n");
}
