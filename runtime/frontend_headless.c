/* Frontend without a window (Linux / Android command line): the boot screen goes to the log, nn::vi gets a display and
   a layer whose native window is a placeholder, the display's vsync event is signalled at 60 Hz by a thread of its own,
   and there is no input (no controller connected). The game runs on its own 8 MiB thread, as under the Mac frontend.
   This is how the game is brought up on the AYN Thor from adb shell before the Android app (window, Vulkan, touch and
   controller input) exists; the app's frontend implements the same functions. */
#if !defined(__APPLE__)
#include "runtime.h"
#include "../third_party/d2r_gfx/host/front.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

void hle_bind_system_event(void *, int);
void hle_signal_system_event(void *);
static uint64_t boot_start_ns;
static _Atomic uint64_t presented;
static pthread_mutex_t vsync_lock = PTHREAD_MUTEX_INITIALIZER;
static void *vsync_events[16];
static unsigned vsync_count;
static int vi_ready;
static int display_identity, layer_identity, native_window;   /* addresses only */

static double since_boot(void) { return (runtime_now_ns() - boot_start_ns) / 1e9; }
void runtime_boot_progress(const char *stage, uint64_t done, uint64_t total) {
    static uint64_t last; uint64_t now = runtime_now_ns();
    if (done != total && now - last < 1000000000ull) return;   /* at most once a second, and the last step */
    last = now; fprintf(stderr, "[totk-boot] %.3f %s %llu/%llu\n", since_boot(), stage, (unsigned long long)done, (unsigned long long)total);
}
void runtime_boot_count(const char *stage, uint64_t done, uint64_t total) { runtime_boot_progress(stage, done, total); }
void runtime_boot_status(const char *stage, const char *detail) { fprintf(stderr, "[totk-boot] %.3f %s: %s\n", since_boot(), stage, detail ? detail : ""); }
void runtime_warm_progress(uint64_t done, uint64_t total, uint64_t shaders, uint64_t shader_total, uint64_t pipelines, uint64_t pipeline_total) {
    (void)shaders; (void)shader_total; (void)pipelines; (void)pipeline_total; runtime_boot_progress("shader warm-up", done, total);
}
void runtime_warm_done(void) { fprintf(stderr, "[totk-boot] %.3f shader warm-up done\n", since_boot()); }
void front_frame_presented(void) { if (atomic_fetch_add(&presented, 1) == 0) fprintf(stderr, "[totk-boot] %.3f first presented frame\n", since_boot()); }
void *front_metal_layer(void) { return NULL; }
uint64_t front_game_res(int docked) { (void)docked; return (720ULL << 32) | 1280; }   /* handheld 1280x720 */

int runtime_front_focus(void) { return 1; }
#ifndef TOTK_ANDROID_APP   /* the app frontend (frontend_android.c) includes this file and provides these itself */
void runtime_front_request_close(void) { fprintf(stderr, "[totk] close requested: exiting\n"); exit(0); }
#endif
void runtime_front_text_test(const char *op, const char *text) { (void)op; (void)text; }
int runtime_front_text_input(const RuntimeTextRequest *req, char *out, size_t out_size) { (void)req; if (out_size) out[0] = 0; return 0; }
void runtime_shortcut_help_toggle(void) {}

/* nn::vi: one display, one layer. */
static void vi_initialize(void) { vi_ready = 1; fprintf(stderr, "[totk] headless VI display initialized\n"); }
static uint32_t vi_open(void **out) { if (!vi_ready || !out) return 0xf601; *out = &display_identity; return 0; }
static uint32_t vi_layer(void **out, void *display) { if (!out || display != &display_identity) return 0xf601; *out = &layer_identity; return 0; }
static uint32_t vi_window(void **out, void *layer) { if (!out || layer != &layer_identity) return 0xf601; *out = &native_window; return 0; }
static uint32_t vi_vsync(void *ev, void *display) {
    if (!ev || display != &display_identity) return 0xf601;
    hle_bind_system_event(ev, 1);
    pthread_mutex_lock(&vsync_lock); if (vsync_count >= 16) runtime_fail("VI display event limit"); vsync_events[vsync_count++] = ev; pthread_mutex_unlock(&vsync_lock);
    return 0;
}
#ifdef GAME_BOTW
static uint32_t vi_scaling_mode(void *layer, int mode) { (void)layer; (void)mode; return 0; }
#endif
void *runtime_front_override(const char *name) {
    static const struct { const char *name; void *fn; } entries[] = {
        {"_ZN2nn2vi10InitializeEv", (void *)vi_initialize}, {"_ZN2nn2vi18OpenDefaultDisplayEPPNS0_7DisplayE", (void *)vi_open},
        {"_ZN2nn2vi11CreateLayerEPPNS0_5LayerEPNS0_7DisplayE", (void *)vi_layer}, {"_ZN2nn2vi15GetNativeWindowEPPvPNS0_5LayerE", (void *)vi_window},
        {"_ZN2nn2vi20GetDisplayVsyncEventEPNS_2os15SystemEventTypeEPNS0_7DisplayE", (void *)vi_vsync},
#ifdef GAME_BOTW
        {"_ZN2nn2vi19SetLayerScalingModeEPNS0_5LayerENS0_11ScalingModeE", (void *)vi_scaling_mode},
#endif
    };
    for (unsigned i = 0; i < sizeof entries / sizeof *entries; i++) if (!strcmp(name, entries[i].name)) return entries[i].fn;
    return NULL;
}

#ifndef TOTK_ANDROID_APP
/* Input: nothing connected. */
void runtime_gamepad_start(void) {}
void runtime_gamepad_clear(void) {}
void runtime_gamepad_read(RuntimeGamepadState *out) { memset(out, 0, sizeof *out); }
#endif
void runtime_rumble_set(unsigned side, float al, float fl, float ah, float fh) { (void)side; (void)al; (void)fl; (void)ah; (void)fh; }
void runtime_feedback_quiet(void) {}

/* Mac GPU probes (Metal) do not exist here. */
void runtime_gpu_path_probe(const char *which) { (void)which; runtime_fail("GPU path probes need the Metal backend"); }
void runtime_vpool_probe(void) { runtime_fail("the virtual pool probe needs the Metal backend"); }

static void *vsync_thread(void *arg) {
    (void)arg;
    struct timespec next; clock_gettime(CLOCK_MONOTONIC, &next);
    for (;;) {
        next.tv_nsec += 16666667; if (next.tv_nsec >= 1000000000) { next.tv_sec++; next.tv_nsec -= 1000000000; }
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL);
        pthread_mutex_lock(&vsync_lock); for (unsigned i = 0; i < vsync_count; i++) hle_signal_system_event(vsync_events[i]); pthread_mutex_unlock(&vsync_lock);
    }
    return NULL;
}
static void *guest_main(void *cb) { ((void (*)(void))cb)(); return NULL; }
void runtime_front_run(void (*callback)(void)) {
    boot_start_ns = runtime_now_ns();
    runtime_gamepad_start();
    pthread_t v; if (pthread_create(&v, NULL, vsync_thread, NULL)) runtime_fail("vsync thread"); pthread_detach(v);
    pthread_t g; pthread_attr_t a; pthread_attr_init(&a); pthread_attr_setstacksize(&a, 8u << 20);
    if (pthread_create(&g, &a, guest_main, (void *)callback)) runtime_fail("game thread creation");
    pthread_attr_destroy(&a);
    pthread_join(g, NULL);
    runtime_fail("game thread returned");
}
#endif
