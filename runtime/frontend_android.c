/* The Android app's frontend (NativeActivity, no Java of its own): the game runs inside the app process as on iOS.
   It shares the boot log, nn::vi and vsync of frontend_headless.c (included below) and adds:
   - the game folder: the app's folder on shared storage (/sdcard/Android/data/<package>/files), holding what run.sh's
     folder holds (rewritten/, extracted/, shaders/, driver.so, user-data/); totk.env there sets TOTK_* options, one
     NAME=value per line, like the variables run.sh takes in front;
   - the log: stdout and stderr go to totk.log in that folder and to logcat (tag "totk");
   - the window: handed to the Vulkan backend, which copies each presented frame into it (vk_draw.c);
   - input: the built-in controller (the AYN Thor's) or any gamepad, mapped to the Switch's buttons by position;
   - a full-screen landscape window kept on while the game runs.
   The game starts once, when the activity is first created, on a thread of its own (totk_host_main is loader.c's
   main, renamed by the build); closing the activity ends the process. */
#if defined(__ANDROID__) && defined(TOTK_ANDROID_APP)
#include "frontend_headless.c"
#include <android/input.h>
#include <android/keycodes.h>
#include <android/log.h>
#include <android/looper.h>
#include <android/native_activity.h>
#include <android/native_window.h>
#include <android/sensor.h>
#include <android/window.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <jni.h>
#include <sys/stat.h>

int totk_host_main(int argc, char **argv);
void vk_window_set(ANativeWindow *window);   /* android/gfx/vk_draw.c */

static char g_data[1024];                    /* the game folder (shared storage) */
static ANativeActivity *g_activity;

/* ---------- log: stdout / stderr -> totk.log ----------
   Straight into the file, not through a pipe and a reader thread: a failure that ends the process with _exit (or a
   signal) would lose whatever the thread had not yet copied, which is exactly the message that says why. */
static int g_log_fd = -1;
static void log_start(void) {
    char path[1100]; snprintf(path, sizeof path, "%s/totk.log", g_data);
    g_log_fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_APPEND | O_CLOEXEC, 0644);
    if (g_log_fd < 0) { __android_log_print(ANDROID_LOG_ERROR, "totk", "cannot open %s: %s", path, strerror(errno)); return; }
    dup2(g_log_fd, 1); dup2(g_log_fd, 2);
    setvbuf(stdout, NULL, _IOLBF, 0); setvbuf(stderr, NULL, _IONBF, 0);
    __android_log_print(ANDROID_LOG_INFO, "totk", "log: %s", path);
}

/* ---------- options ---------- */
static void env_file(const char *path) {   /* NAME=value per line; # comments; existing variables are replaced */
    FILE *f = fopen(path, "r"); if (!f) return;
    char line[2048];
    while (fgets(line, sizeof line, f)) {
        char *s = line; while (*s == ' ' || *s == '\t') s++;
        size_t n = strlen(s); while (n && (s[n - 1] == '\n' || s[n - 1] == '\r' || s[n - 1] == ' ')) s[--n] = 0;
        if (!*s || *s == '#') continue;
        if (!strncmp(s, "export ", 7)) s += 7;
        char *eq = strchr(s, '='); if (!eq || eq == s) { fprintf(stderr, "[totk-app] totk.env: ignored line \"%s\"\n", s); continue; }
        *eq = 0; char *v = eq + 1;
        size_t vn = strlen(v); if (vn >= 2 && (v[0] == '"' || v[0] == '\'') && v[vn - 1] == v[0]) { v[vn - 1] = 0; v++; }
        setenv(s, v, 1); fprintf(stderr, "[totk-app] totk.env: %s=%s\n", s, v);
    }
    fclose(f);
}
static void env_default(const char *name, const char *fmt, const char *a) {
    if (getenv(name)) return;
    char v[1200]; snprintf(v, sizeof v, fmt, a); setenv(name, v, 0);
}
static int is_dir(const char *p) { struct stat st; return !stat(p, &st) && S_ISDIR(st.st_mode); }
static int is_file(const char *p) { struct stat st; return !stat(p, &st) && S_ISREG(st.st_mode); }
/* copy a folder recursively (dst made); returns the number of files copied, -1 on failure */
static int copy_tree(const char *src, const char *dst) {
    if (mkdir(dst, 0700) && errno != EEXIST) return -1;
    DIR *d = opendir(src); if (!d) return -1;
    int n = 0; struct dirent *e;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char s2[2048], d2[2048]; snprintf(s2, sizeof s2, "%s/%s", src, e->d_name); snprintf(d2, sizeof d2, "%s/%s", dst, e->d_name);
        if (is_dir(s2)) { int k = copy_tree(s2, d2); if (k > 0) n += k; continue; }
        int in = open(s2, O_RDONLY | O_CLOEXEC), out = in < 0 ? -1 : open(d2, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600), ok = in >= 0 && out >= 0;
        char buf[1 << 16]; ssize_t r;
        while (ok && (r = read(in, buf, sizeof buf)) > 0) if (write(out, buf, (size_t)r) != r) ok = 0;
        if (in >= 0) close(in); if (out >= 0) close(out);
        if (ok) n++; else fprintf(stderr, "[totk-app] copying %s: %s\n", s2, strerror(errno));
    }
    closedir(d);
    return n;
}
/* the single folder under rewritten/ (the game version this folder was built for) */
static int game_version(char *out, size_t n) {
    char p[1100]; snprintf(p, sizeof p, "%s/rewritten", g_data);
    DIR *d = opendir(p); if (!d) return 0;
    struct dirent *e; int found = 0;
    while ((e = readdir(d))) if (e->d_name[0] != '.') { char q[1400]; snprintf(q, sizeof q, "%s/%s", p, e->d_name); if (is_dir(q)) { snprintf(out, n, "%s", e->d_name); found++; } }
    closedir(d);
    return found == 1;
}
/* Files on shared storage cannot be loaded as code, so the Vulkan driver is copied into the app's private folder. */
static void driver_setup(const char *private_dir) {
    const char *want = getenv("TOTK_VK_DRIVER");
    if (want && !strcmp(want, "system")) return;
    char src[1200];
    if (want && *want && want[0] == '/') snprintf(src, sizeof src, "%s", want);
    else if (want && *want) snprintf(src, sizeof src, "%s/%s", g_data, want);   /* relative to the game folder */
    else snprintf(src, sizeof src, "%s/driver.so", g_data);
    if (!is_file(src)) { if (want && *want) fprintf(stderr, "[totk-app] TOTK_VK_DRIVER: no file %s; the system driver is used\n", src); unsetenv("TOTK_VK_DRIVER"); return; }
    char dst[1200]; snprintf(dst, sizeof dst, "%s/driver.so", private_dir);
    int in = open(src, O_RDONLY | O_CLOEXEC), out = in < 0 ? -1 : open(dst, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0755);
    int ok = in >= 0 && out >= 0;
    char buf[1 << 16]; ssize_t n;
    while (ok && (n = read(in, buf, sizeof buf)) > 0) if (write(out, buf, (size_t)n) != n) ok = 0;
    if (in >= 0) close(in); if (out >= 0) close(out);
    if (!ok) { fprintf(stderr, "[totk-app] copying the driver %s: %s; the system driver is used\n", src, strerror(errno)); unsetenv("TOTK_VK_DRIVER"); return; }
    setenv("TOTK_VK_DRIVER", dst, 1);
    char libs[1200]; snprintf(libs, sizeof libs, "%s/driver-libs", private_dir); setenv("TOTK_VK_LIBDIR", libs, 1);   /* vk_load.c */
    fprintf(stderr, "[totk-app] Vulkan driver: %s (copied from %s)\n", dst, src);
}

/* ---------- the game ---------- */
static char g_rewritten[1200];
static void *game_thread(void *arg) {
    (void)arg;
    char *argv[] = {"totk-native", g_rewritten, "boot", NULL};
    int r = totk_host_main(3, argv);
    fprintf(stderr, "[totk-app] the game returned %d\n", r);
    _exit(r);
    return NULL;
}
static void motion_start(void);   /* below: the device IMU -> the Switch six-axis sensor */
static int start_game(const ANativeActivity *a) {
    snprintf(g_data, sizeof g_data, "%s", a->externalDataPath ? a->externalDataPath : a->internalDataPath);
    mkdir(g_data, 0755);
    log_start();
    fprintf(stderr, "[totk-app] build %s %s; game folder %s (Android SDK %d)\n", __DATE__, __TIME__, g_data, a->sdkVersion);
    if (chdir(g_data)) fprintf(stderr, "[totk-app] chdir %s: %s\n", g_data, strerror(errno));
    char p[1200]; snprintf(p, sizeof p, "%s/totk.env", g_data); env_file(p);
    { extern void runtime_input_file_start(void); setenv("TOTK_INPUT_FILE_NEW", "1", 0); runtime_input_file_start(); }   /* TOTK_INPUT_FILE from totk.env: "threads" there dumps every guest thread (hangs) */
    { extern void runtime_autoload_start(void); runtime_autoload_start(); }   /* TOTK_AUTOLOAD from totk.env (devrun.py sets it) */
    char version[256];
    if (!game_version(version, sizeof version)) {
        fprintf(stderr, "[totk-app] 【红】no game in %s: copy the TOTK_android folder's contents here (rewritten/, extracted/, ...); see android/APP.md\n", g_data);
        return 0;
    }
    snprintf(g_rewritten, sizeof g_rewritten, "%s/rewritten/%s", g_data, version);
    /* Saves, the account and run state live in the app's private storage: files on shared storage that adb shell
       copied in belong to the shell user, and the app cannot open them for writing (the account file's lock). The
       game folder's user-data/ is imported once, when the private one does not exist yet. */
    const char *priv = a->internalDataPath;
    char pu[1200]; snprintf(pu, sizeof pu, "%s/user-data", priv);
    snprintf(p, sizeof p, "%s/user-data", g_data);
    if (!is_dir(pu) && is_dir(p)) { int n = copy_tree(p, pu); fprintf(stderr, "[totk-app] saves: imported %d files from %s into %s\n", n, p, pu); }
    env_default("TOTK_PROFILE_DIR", "%s", pu);
    env_default("TOTK_ROMFS_PROOF_DIR", "%s/state", priv);
    env_default("TOTK_GAME_SLOT_DIR", "%s/state/game-slot", priv);
    env_default("TOTK_SHARED_FONTS", "%s/extracted/firmware/shared-fonts", g_data);
    env_default("TOTK_OPERATION_MODE", "%s", "handheld");
    env_default("TOTK_ROMFS_MMAP", "%s", "1");
    /* The translation thread (the iPhone default): submits and presents replay on their own thread, so the game's
       next frame overlaps the replay. Without it the frame is recording + GPU summed - the logs' median frame matched
       that sum exactly (17.5 + 15.8 = 33.5 ms), and the game's threads paid thousands of fence waits serially.
       totk.env can still set TOTK_ASYNC_REPLAY=0 (env_file above runs first, env_default never overrides). */
    env_default("TOTK_ASYNC_REPLAY", "%s", "1");
    /* Turnip: round texture coordinates to nearest-even, as Maxwell does (driconf option; Mesa reads an environment
       variable of the same name before vkCreateInstance). Without it Adreno picks the neighbouring texel at exact
       half-texel coordinates and TOTK's lighting-tile classifier leaves grey 64 px squares (user-confirmed 2026-10-23:
       this option alone fixes them on the Apex driver). Balemuni's Turnip also sets it for apps named like an emulator
       (nvn_vk.c reports "yuzu Emulator"); this covers drivers that do not. totk.env =false turns it off. */
    env_default("tu_use_tex_coord_round_nearest_even_mode", "%s", "true");
    /* Frame cap: 30 fps, the game's shipping rate (2026-10-23). The port runs "uncapped" (TOTK_UNCAPPED, the 60 FPS patch
       and the real-time frame step) up to 120 fps; on the Thor that swung between 8, 16 and 45+ ms frames (the perf
       summary's frame-time percentiles), which is what made 27 fps feel unstable, and the light scenes' extra frames only
       heat the chip. The cap paces presents at 33.3 ms (hle_nvn.c nvn_pace); slower frames still go out at once.
       TOTK_UNCAPPED_INTERVAL=1 is needed with it: otherwise the period is the game's own present interval (2 or 3)
       divided by TOTK_UNCAPPED_HZ, and 30 meant 67-100 ms frames (the first try, 2026-10-23: 13.6 fps). With it the period
       is exactly 1 / TOTK_UNCAPPED_HZ; the real-time frame step keeps game time right. totk.env TOTK_UNCAPPED_HZ=60 raises
       the cap. */
    env_default("TOTK_UNCAPPED_HZ", "%s", "30");
    env_default("TOTK_UNCAPPED_INTERVAL", "%s", "1");
    snprintf(p, sizeof p, "%s/shaders/%s", g_data, version); if (is_dir(p)) env_default("TOTK_SHADER_SEED_DIR", "%s", p);
    mkdir(pu, 0700);
    snprintf(p, sizeof p, "%s/state", priv); mkdir(p, 0700);
    driver_setup(a->internalDataPath);
    pthread_t t; pthread_attr_t at; pthread_attr_init(&at); pthread_attr_setstacksize(&at, 64u << 20);
    motion_start();
    if (pthread_create(&t, &at, game_thread, NULL)) { fprintf(stderr, "[totk-app] game thread: %s\n", strerror(errno)); return 0; }
    pthread_attr_destroy(&at); pthread_detach(t);
    return 1;
}

/* ---------- input ----------
   By name by default: Android's BUTTON_A is the Switch's A and so on. The AYN Thor's built-in controller reports its
   buttons by their Nintendo-style labels (A on the right). TOTK_FACE_BUTTONS=position maps by Xbox position instead
   (BUTTON_A at the bottom = Switch B, right = A, left = Y, top = X), for Xbox-layout gamepads. */
enum { SW_A = 0, SW_B, SW_X, SW_Y, SW_LS, SW_RS, SW_L, SW_R, SW_ZL, SW_ZR, SW_PLUS, SW_MINUS, SW_LEFT, SW_UP, SW_RIGHT, SW_DOWN };
static pthread_mutex_t g_pad_mu = PTHREAD_MUTEX_INITIALIZER;
static uint64_t g_keys, g_hat, g_trig, g_edges;   /* buttons from key events, from the hat axes, from trigger axes; presses since the last read */
static int32_t g_axes[4];
static int g_pad_seen;
static int switch_bit(int keycode) {
    static int by_label = -1; if (by_label < 0) { const char *e = getenv("TOTK_FACE_BUTTONS"); by_label = !(e && !strcmp(e, "position")); }
    switch (keycode) {
        case AKEYCODE_BUTTON_A: return by_label ? SW_A : SW_B;
        case AKEYCODE_BUTTON_B: return by_label ? SW_B : SW_A;
        case AKEYCODE_BUTTON_X: return by_label ? SW_X : SW_Y;
        case AKEYCODE_BUTTON_Y: return by_label ? SW_Y : SW_X;
        case AKEYCODE_BUTTON_L1: return SW_L; case AKEYCODE_BUTTON_R1: return SW_R;
        case AKEYCODE_BUTTON_L2: return SW_ZL; case AKEYCODE_BUTTON_R2: return SW_ZR;
        case AKEYCODE_BUTTON_THUMBL: return SW_LS; case AKEYCODE_BUTTON_THUMBR: return SW_RS;
        case AKEYCODE_BUTTON_START: return SW_PLUS; case AKEYCODE_BUTTON_SELECT: return SW_MINUS;
        case AKEYCODE_DPAD_LEFT: return SW_LEFT; case AKEYCODE_DPAD_UP: return SW_UP;
        case AKEYCODE_DPAD_RIGHT: return SW_RIGHT; case AKEYCODE_DPAD_DOWN: return SW_DOWN;
    }
    return -1;
}
static int32_t stick(float v) {   /* gamepad.m stick_axis: 12 % dead zone, rescaled to the Switch's range */
    const float dz = 0.12f; float m = v < 0 ? -v : v;
    if (m <= dz) return 0; if (m > 1) m = 1;
    int32_t r = (int32_t)((m - dz) / (1 - dz) * 32767.0f);
    return v < 0 ? -r : r;
}
static int handle_input(AInputEvent *ev) {
    int type = AInputEvent_getType(ev), src = AInputEvent_getSource(ev);
    int pad = (src & AINPUT_SOURCE_GAMEPAD) == AINPUT_SOURCE_GAMEPAD || (src & AINPUT_SOURCE_JOYSTICK) == AINPUT_SOURCE_JOYSTICK || (src & AINPUT_SOURCE_DPAD) == AINPUT_SOURCE_DPAD;
    if (type == AINPUT_EVENT_TYPE_KEY) {
        int code = AKeyEvent_getKeyCode(ev), act = AKeyEvent_getAction(ev);
        if (code == AKEYCODE_BACK) return 1;   /* not "leave the game" */
        int bit = switch_bit(code); if (bit < 0) return 0;
        pthread_mutex_lock(&g_pad_mu);
        if (act == AKEY_EVENT_ACTION_DOWN) { if (!(g_keys >> bit & 1)) g_edges |= 1ull << bit; g_keys |= 1ull << bit; }
        else if (act == AKEY_EVENT_ACTION_UP) g_keys &= ~(1ull << bit);
        g_pad_seen = 1;
        pthread_mutex_unlock(&g_pad_mu);
        return 1;
    }
    if (type == AINPUT_EVENT_TYPE_MOTION && pad) {
        float lx = AMotionEvent_getAxisValue(ev, AMOTION_EVENT_AXIS_X, 0), ly = AMotionEvent_getAxisValue(ev, AMOTION_EVENT_AXIS_Y, 0);
        float rx = AMotionEvent_getAxisValue(ev, AMOTION_EVENT_AXIS_Z, 0), ry = AMotionEvent_getAxisValue(ev, AMOTION_EVENT_AXIS_RZ, 0);
        if (!rx && !ry) { rx = AMotionEvent_getAxisValue(ev, AMOTION_EVENT_AXIS_RX, 0); ry = AMotionEvent_getAxisValue(ev, AMOTION_EVENT_AXIS_RY, 0); }
        float hx = AMotionEvent_getAxisValue(ev, AMOTION_EVENT_AXIS_HAT_X, 0), hy = AMotionEvent_getAxisValue(ev, AMOTION_EVENT_AXIS_HAT_Y, 0);
        float lt = AMotionEvent_getAxisValue(ev, AMOTION_EVENT_AXIS_LTRIGGER, 0), rt = AMotionEvent_getAxisValue(ev, AMOTION_EVENT_AXIS_RTRIGGER, 0);
        float br = AMotionEvent_getAxisValue(ev, AMOTION_EVENT_AXIS_BRAKE, 0), gas = AMotionEvent_getAxisValue(ev, AMOTION_EVENT_AXIS_GAS, 0);
        if (br > lt) lt = br; if (gas > rt) rt = gas;
        uint64_t hat = (hx < -0.5f ? 1ull << SW_LEFT : 0) | (hx > 0.5f ? 1ull << SW_RIGHT : 0) | (hy < -0.5f ? 1ull << SW_UP : 0) | (hy > 0.5f ? 1ull << SW_DOWN : 0);
        uint64_t trig = (lt > 0.5f ? 1ull << SW_ZL : 0) | (rt > 0.5f ? 1ull << SW_ZR : 0);
        pthread_mutex_lock(&g_pad_mu);
        g_edges |= (hat & ~g_hat) | (trig & ~g_trig);
        g_hat = hat; g_trig = trig;
        g_axes[0] = stick(lx); g_axes[1] = -stick(ly); g_axes[2] = stick(rx); g_axes[3] = -stick(ry);   /* Android: down is +y; Switch: up is + */
        g_pad_seen = 1;
        pthread_mutex_unlock(&g_pad_mu);
        return 1;
    }
    return 0;
}
void runtime_gamepad_start(void) {}
void runtime_gamepad_clear(void) { pthread_mutex_lock(&g_pad_mu); g_keys = g_hat = g_trig = g_edges = 0; memset(g_axes, 0, sizeof g_axes); pthread_mutex_unlock(&g_pad_mu); }
void runtime_gamepad_read(RuntimeGamepadState *out) {
    pthread_mutex_lock(&g_pad_mu);
    out->buttons = g_keys | g_hat | g_trig | g_edges; g_edges = 0;   /* a press shorter than one read still reaches the game */
    out->lx = g_axes[0]; out->ly = g_axes[1]; out->rx = g_axes[2]; out->ry = g_axes[3];
    out->connected = g_pad_seen;
    pthread_mutex_unlock(&g_pad_mu);
}
void runtime_front_request_close(void) { fprintf(stderr, "[totk] close requested: exiting\n"); _exit(0); }

static AInputQueue *g_queue;
static int queue_cb(int fd, int events, void *data) {
    (void)fd; (void)events; AInputQueue *q = data; AInputEvent *ev;
    while (AInputQueue_getEvent(q, &ev) >= 0) {
        if (AInputQueue_preDispatchEvent(q, ev)) continue;
        AInputQueue_finishEvent(q, ev, handle_input(ev));
    }
    return 1;
}

/* ---------- motion: the device's own accelerometer and gyroscope (2026-10-23) ----------
   The AYN Thor's IMU feeds the Switch six-axis sensor the game reads (runtime/motion_fusion.c, hid_feedback.c; on
   unless TOTK_GYRO=0; the game's own Motion Controls options decide what it is used for). Android's frame for a
   landscape-native handheld is the Switch's (x right, y toward the top edge, z out of the screen) except for the
   accelerometer's sign: Android reports the push against gravity (+1 G on z lying face up), the Switch the gravity
   vector (-1 G). Rates are rad/s, right-handed, as on the Switch. 200 Hz.
   Android reports the sensors in the device's NATURAL orientation, which need not be the screen's: the AYN Thor's is
   portrait (2026-10-23, user: steering like a wheel moved the view up and down; a logged session showed gravity on x
   when held in landscape, tilting on y, turning on x). The axes therefore follow the display rotation, as
   SensorManager.remapCoordinateSystem does: 90 deg -> "-y,x,z", 180 -> "-x,-y,z", 270 -> "y,-x,z" (screen right, up,
   out of the screen). TOTK_GYRO_AXES overrides it; TOTK_GYRO_LOG=1 logs gravity and raw rates to find a device's axes. */
static _Atomic int g_disp_rot = -1;   /* Display.getRotation(): 0..3 quarter turns; -1 until the window exists */
static void motion_axes_apply(int rot) {
    const char *own = getenv("TOTK_GYRO_AXES");
    static const char *by_rot[4] = {"x,y,z", "-y,x,z", "-x,-y,z", "y,-x,z"};
    const char *spec = own && *own ? own : by_rot[rot & 3];
    if (runtime_motion_axes(spec) < 0) { fprintf(stderr, "[motion] TOTK_GYRO_AXES \"%s\" is malformed (e.g. \"-y,x,z\"): axes left as they are\n", spec); return; }
    fprintf(stderr, "[motion] axes %s (%s)\n", spec, own && *own ? "TOTK_GYRO_AXES" : rot == 0 ? "display in its natural orientation" : "display rotated from the sensors' natural orientation");
}
static void *motion_thread(void *arg) {
    (void)arg;
    ALooper *looper = ALooper_prepare(ALOOPER_PREPARE_ALLOW_NON_CALLBACKS);
    ASensorManager *sm = ASensorManager_getInstanceForPackage("dev.kitcatkode.totk");
    const ASensor *acc = sm ? ASensorManager_getDefaultSensor(sm, ASENSOR_TYPE_ACCELEROMETER) : NULL;
    const ASensor *gyr = sm ? ASensorManager_getDefaultSensor(sm, ASENSOR_TYPE_GYROSCOPE) : NULL;
    if (!acc || !gyr) { fprintf(stderr, "[motion] no %s%s%s: motion controls off\n", acc ? "" : "accelerometer", !acc && !gyr ? " and no " : "", gyr ? "" : "gyroscope"); return NULL; }
    ASensorEventQueue *q = ASensorManager_createEventQueue(sm, looper, 1, NULL, NULL);
    if (!q) { fprintf(stderr, "[motion] no sensor event queue: motion controls off\n"); return NULL; }
    int period = ASensor_getMinDelay(gyr); if (period < 5000) period = 5000;
    ASensorEventQueue_registerSensor(q, acc, period, 0); ASensorEventQueue_registerSensor(q, gyr, period, 0);
    fprintf(stderr, "[motion] %s + %s at %d us\n", ASensor_getName(acc), ASensor_getName(gyr), period);
    runtime_motion_live(1);
    double a[3] = {0, 0, -1}; int have_a = 0, logged = 0;
    /* TOTK_GYRO_LOG=1: every 0.25 s the gravity and the mean rotation speed per raw axis (deg/s, Android's frame), to
       find a device's axes: hold still, then steer like a wheel, tilt the top toward / away, turn left / right */
    const char *gl = getenv("TOTK_GYRO_LOG"); int glog = gl && atoi(gl) > 0; double gsum[3] = {0}; int gn = 0; uint64_t gt0 = 0;
    for (;;) {
        ALooper_pollOnce(-1, NULL, NULL, NULL);
        ASensorEvent ev[16]; ssize_t n;
        while ((n = ASensorEventQueue_getEvents(q, ev, 16)) > 0) for (ssize_t i = 0; i < n; i++) {
            if (ev[i].type == ASENSOR_TYPE_ACCELEROMETER) {
                for (int k = 0; k < 3; k++) a[k] = -ev[i].acceleration.v[k] / 9.80665;
                if (!have_a && !logged++) fprintf(stderr, "[motion] first gravity reading %.2f %.2f %.2f G (Switch frame; lying face up should read about 0 0 -1)\n", a[0], a[1], a[2]);
                have_a = 1;
            } else if (ev[i].type == ASENSOR_TYPE_GYROSCOPE && have_a) {
                { static int applied = -2; int rot = atomic_load(&g_disp_rot); if (rot >= 0 && rot != applied) { applied = rot; motion_axes_apply(rot); } }
                const double g[3] = {ev[i].vector.x, ev[i].vector.y, ev[i].vector.z};
                uint64_t now = runtime_now_ns();
                runtime_motion_feed(a, g, now);
                if (glog) {
                    for (int k = 0; k < 3; k++) gsum[k] += g[k]; gn++;
                    if (!gt0) gt0 = now;
                    if (now - gt0 >= 250000000ull) {
                        fprintf(stderr, "[motion] gravity %+.2f %+.2f %+.2f G, rotation x %+5.0f y %+5.0f z %+5.0f deg/s (raw Android axes)\n",
                                a[0], a[1], a[2], gsum[0] / gn * 57.29578, gsum[1] / gn * 57.29578, gsum[2] / gn * 57.29578);
                        gsum[0] = gsum[1] = gsum[2] = 0; gn = 0; gt0 = now;
                    }
                }
            }
        }
    }
    return NULL;
}
static void motion_start(void) {
    const char *e = getenv("TOTK_GYRO"); if (e && !strcmp(e, "0")) { fprintf(stderr, "[motion] off (TOTK_GYRO=0)\n"); return; }
    pthread_t t; if (pthread_create(&t, NULL, motion_thread, NULL)) fprintf(stderr, "[motion] thread: %s\n", strerror(errno)); else pthread_detach(t);
}

/* ---------- the activity (callbacks on the UI thread) ---------- */
static void immersive(ANativeActivity *a) {   /* hide the status and navigation bars (View.setSystemUiVisibility) */
    JNIEnv *env = a->env; jobject act = a->clazz;
    jclass ac = (*env)->GetObjectClass(env, act);
    jmethodID gw = (*env)->GetMethodID(env, ac, "getWindow", "()Landroid/view/Window;");
    jobject w = gw ? (*env)->CallObjectMethod(env, act, gw) : NULL;
    if (w) {
        jclass wc = (*env)->GetObjectClass(env, w);
        jmethodID gd = (*env)->GetMethodID(env, wc, "getDecorView", "()Landroid/view/View;");
        jobject v = gd ? (*env)->CallObjectMethod(env, w, gd) : NULL;
        if (v) {
            jclass vc = (*env)->GetObjectClass(env, v);
            jmethodID su = (*env)->GetMethodID(env, vc, "setSystemUiVisibility", "(I)V");
            if (su) (*env)->CallVoidMethod(env, v, su, 0x1706);   /* HIDE_NAVIGATION | FULLSCREEN | LAYOUT_* | IMMERSIVE_STICKY */
        }
    }
    if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
}
static int display_rotation(ANativeActivity *a) {   /* getWindowManager().getDefaultDisplay().getRotation(); -1 on failure */
    JNIEnv *env = a->env; jobject act = a->clazz; int r = -1;
    jclass ac = (*env)->GetObjectClass(env, act);
    jmethodID gwm = (*env)->GetMethodID(env, ac, "getWindowManager", "()Landroid/view/WindowManager;");
    jobject wm = gwm ? (*env)->CallObjectMethod(env, act, gwm) : NULL;
    if (wm) {
        jclass wc = (*env)->GetObjectClass(env, wm);
        jmethodID gd = (*env)->GetMethodID(env, wc, "getDefaultDisplay", "()Landroid/view/Display;");
        jobject d = gd ? (*env)->CallObjectMethod(env, wm, gd) : NULL;
        if (d) { jclass dc = (*env)->GetObjectClass(env, d); jmethodID gr = (*env)->GetMethodID(env, dc, "getRotation", "()I"); if (gr) r = (*env)->CallIntMethod(env, d, gr); }
    }
    if ((*env)->ExceptionCheck(env)) { (*env)->ExceptionClear(env); r = -1; }
    return r;
}
#include "bottom_screen.c"   /* the second screen: the alias activity (uses handle_input) */
static void on_window_created(ANativeActivity *a, ANativeWindow *w) {
    int rot = display_rotation(a);
    if (rot != atomic_load(&g_disp_rot)) fprintf(stderr, "[motion] display rotation %d deg\n", rot < 0 ? -1 : rot * 90);
    atomic_store(&g_disp_rot, rot < 0 ? 0 : rot);
    vk_window_set(w);
    static int launched; if (!launched) { launched = 1; bottom_launch(a); }   /* once the game's own window exists */
}
static void on_window_destroyed(ANativeActivity *a, ANativeWindow *w) { (void)a; (void)w; vk_window_set(NULL); }
static void on_queue_created(ANativeActivity *a, AInputQueue *q) {
    (void)a; g_queue = q;
    AInputQueue_attachLooper(q, ALooper_forThread(), 1, queue_cb, q);
}
static void on_queue_destroyed(ANativeActivity *a, AInputQueue *q) { (void)a; AInputQueue_detachLooper(q); if (g_queue == q) g_queue = NULL; }
static void on_focus(ANativeActivity *a, int focused) {
    if (focused) immersive(a);
    else runtime_gamepad_clear();   /* releases may not arrive while another window has focus */
}
static void on_destroy(ANativeActivity *a) { (void)a; fprintf(stderr, "[totk-app] activity destroyed: exiting\n"); _exit(0); }

__attribute__((visibility("default"))) void ANativeActivity_onCreate(ANativeActivity *a, void *saved, size_t saved_size) {
    (void)saved; (void)saved_size;
    static int started;
    if (bottom_is(a)) { bottom_create(a); return; }   /* the second screen's activity: not the game's */
    g_activity = a;
    a->callbacks->onNativeWindowCreated = on_window_created;
    a->callbacks->onNativeWindowDestroyed = on_window_destroyed;
    a->callbacks->onInputQueueCreated = on_queue_created;
    a->callbacks->onInputQueueDestroyed = on_queue_destroyed;
    a->callbacks->onWindowFocusChanged = on_focus;
    a->callbacks->onDestroy = on_destroy;
    ANativeActivity_setWindowFlags(a, AWINDOW_FLAG_KEEP_SCREEN_ON | AWINDOW_FLAG_FULLSCREEN, 0);
    immersive(a);
    if (!started) { started = 1; start_game(a); }
}
#endif
