/* Runs the real loader (runtime/loader.c, gate.S, hle_stack.S/.c, host_tp.c, host_os.c) on the synthetic modules
   from make_fixture.py: map, relocate, seal, check permissions, then the loader's own cross-module probe (4 threads x
   1000 calls main PLT -> sdk -> timer gate), then every rewritten instruction kind on several threads at once.
   Everything the loader links against outside those files is a stub here. No game data.

   aarch64 Linux (qemu-aarch64 works) or Android. Usage: harness <dir with *.patched.bin> */
#define main loader_main
#include "loader.c"
#undef main
#include <stdatomic.h>

/* ---- stubs for what loader.c links against ---- */
void runtime_game_patches(Module *m) { (void)m; }
void *runtime_observer(const char *n) { (void)n; return NULL; }
void *runtime_override(const char *n) { (void)n; return NULL; }
int runtime_gfx_fault(void *a, int w, void *c) { (void)a; (void)w; (void)c; return 0; }
void runtime_boot_status(const char *a, const char *b) { (void)a; (void)b; }
void runtime_game_slot_acquire(const char *m) { (void)m; }
uint32_t svc_thread_main_init(void) { return 0; }
void runtime_front_run(void (*f)(void)) { f(); }
void runtime_gpu_path_probe(const char *w) { (void)w; }
void runtime_memory_probe(void) {}
void runtime_gfx_probe(int n) { (void)n; }
void runtime_vpool_probe(void) {}
void runtime_audio_probe(void) {}
void runtime_fs_probe(void) {}
void runtime_swkbd_probe(void) {}
void runtime_save_probe(void) {}
void runtime_thread_note_hle(unsigned i) { (void)i; }
uint32_t runtime_thread_current_handle(void) { return 0; }
uint64_t runtime_svc(unsigned num, uint64_t *regs, const char *module, uint64_t offset) {
    (void)module; (void)offset;
    regs[0] = regs[0] * 3 + num;   /* fx_svc: x0 * 3 + 0x7e */
    return 0;
}

/* ---- tests ---- */
typedef uint64_t (*F0)(void);
typedef uint64_t (*F1)(uint64_t);
typedef uint64_t (*F2)(uint64_t, uint64_t);
typedef uint64_t (*F3)(uint64_t, uint64_t, uint64_t);
static atomic_uint failures, checks;
#define CHECK(c, what) do { atomic_fetch_add(&checks, 1); if (!(c)) { atomic_fetch_add(&failures, 1); \
    fprintf(stderr, "[fixture] FAIL %s (thread %lu, line %d)\n", what, (unsigned long)arg, __LINE__); } } while (0)
static void *sym(const char *n) { void *p = runtime_symbol(n); if (!p) { fprintf(stderr, "missing %s\n", n); exit(2); } return p; }
static uint64_t host_callee(uint64_t a, uint64_t b) { return a * 7 + b; }
static atomic_ulong shared_counter;

static void *worker(void *argp) {
    uintptr_t arg = (uintptr_t)argp;
    runtime_thread_init();
    F1 x18_set = sym("fx_x18_set"); F1 x18_add = sym("fx_x18_add"); F0 x18_load = sym("fx_x18_load");
    F0 x18_cbz = sym("fx_x18_cbz"); F0 x18_adr = sym("fx_x18_adr"); F0 atomic_inc = sym("fx_x18_atomic_inc");
    F2 x18_call = sym("fx_x18_call"); F0 tls_get = sym("fx_tls_get"); F1 tls_set = sym("fx_tls_set");
    F0 tlr_get = sym("fx_tlr_get"); F1 svc = sym("fx_svc"); F3 regs = sym("fx_regs_survive");
    F0 main_x18 = sym("fx_main_x18"); F0 sub_tick = sym("fx_sub_x18_plus_tick"); F0 main_tick = sym("fx_main_tick");
    uint64_t host_x18; __asm__ volatile("mov %0, x18" : "=r"(host_x18));
    uint64_t box[2] = {0, 0xabc0 + arg};
    for (unsigned i = 0; i < 2000; i++) {
        uint64_t v = (arg << 32) | i;
        x18_set(v);
        CHECK(x18_add(5) == v + 5, "x18 read (add)");
        CHECK(main_x18() == v, "x18 is per thread and shared by all modules");
        CHECK(sub_tick() == v + 1, "x18 + PLT call from subsdk0");
        x18_set(0); CHECK(x18_cbz() == 2, "cbz x18 taken");
        x18_set(1); CHECK(x18_cbz() == 1, "cbz x18 not taken");
        uint64_t here = x18_adr();
        CHECK(here == (uint64_t)(uintptr_t)x18_adr + 4, "adr x18 (pc-relative into the slot)");
        x18_set((uintptr_t)box); CHECK(x18_load() == 0xabc0 + arg, "ldr through x18");
        x18_set((uintptr_t)&shared_counter); atomic_inc();
        x18_set((uintptr_t)host_callee); CHECK(x18_call(i, 3) == (uint64_t)i * 7 + 3, "blr x18 call window");
        tls_set(0x1000 + v); CHECK(tls_get() == 0x1000 + v, "guest TPIDR_EL0 write/read");
        CHECK(tlr_get() == (uintptr_t)runtime_thread_local_region(), "guest TPIDRRO_EL0 = Horizon TLR");
        CHECK(svc(i) == (uint64_t)i * 3 + 0x7e, "svc through the host gate");
        CHECK(regs(11, 22, 0x4000000000000000ull + i) == 33 + 0x4000000000000000ull + i, "registers survive a timer gate");
        CHECK(main_tick() > 0, "main PLT tail call -> sdk timer");
        uint64_t now_x18; __asm__ volatile("mov %0, x18" : "=r"(now_x18));
        CHECK(now_x18 == host_x18, "host x18 untouched by guest code");
    }
    return NULL;
}

int main(int argc, char **argv) {
    if (argc != 2) { fprintf(stderr, "usage: harness <rewritten dir>\n"); return 2; }
    char *args[] = {argv[0], argv[1], "probe", NULL};
    setenv("TOTK_HEADLESS", "1", 1);
    loader_main(3, args);   /* maps, relocates, seals, checks permissions, runs the loader's probe; prints its JSON */
    enum { N = 6 };
    pthread_t t[N];
    for (uintptr_t i = 0; i < N; i++) if (pthread_create(&t[i], NULL, worker, (void *)i)) return 3;
    for (unsigned i = 0; i < N; i++) pthread_join(t[i], NULL);
    unsigned f = atomic_load(&failures), c = atomic_load(&checks);
    if (atomic_load(&shared_counter) != N * 2000) { f++; fprintf(stderr, "[fixture] FAIL exclusive window lost increments: %lu\n", (unsigned long)atomic_load(&shared_counter)); }
    printf("{\"status\":\"%s\",\"threads\":%d,\"checks\":%u,\"failures\":%u,\"atomic_increments\":%lu,"
           "\"scope\":\"synthetic modules: x18 / tpidr / tpidrro / svc / cntpct / cntvct / exclusive window / blr x18 sites on host thread slots\"}\n",
           f ? "fail" : "pass", N, c, f, (unsigned long)atomic_load(&shared_counter));
    return f ? 1 : 0;
}
