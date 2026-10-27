/* Memory search for finding the game's live data (android/BOTTOM_SCREEN.md step F: the pouches in GameData), as
   diagnostic commands on TOTK_INPUT_FILE. Read-only: every read goes through host_safe_read, so a mapping that goes
   away during a scan costs nothing. Results go to the log, "[memscan] ...".
     memscan u32 <hex>      4-byte aligned words equal to it (a key hash)
     memscan u64 <hex>      8-byte aligned words equal to it (pointers to an address)
     memscan ptr <lo> <hi> [game]  8-byte aligned words in [lo, hi) (pointers into an object); "game": only in the
                            game module's own mappings (its static data: where a chain of pointers starts)
     memscan str <text>     the bytes of the text, anywhere (an item id)
     memdump <hex addr> <n> n bytes (up to 4096) as hex and text
     gmd int|str <key>      a GameData array of the running game by key name (gamedata_live.c), its non-empty
                            elements: IntArray values or String64Array texts
     memwatch gmd int|str <key> <seconds>   who writes that array: its pages are made read-only, each write faults,
                            the writer is noted (pc, lr, as game offsets) and the page let go; a thread protects it again
                            every 2 ms. At the end the writers are listed, the ones that wrote the array itself first.
                            Diagnostic: the game runs slower while it watches (every write to those pages faults).
   Scans every readable and writable mapping but the scan's own buffer; at most MEMSCAN_HITS hits are listed. */
#include "runtime.h"
#include "host_os.h"
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

enum { MEMSCAN_HITS = 200, MEMSCAN_CHUNK = 1 << 20 };

static void dump_line(const char *tag, uint64_t a, size_t n) {   /* n bytes at a, as hex then as text */
    uint8_t b[64]; char hex[200], txt[70]; if (n > sizeof b) n = sizeof b;
    if (!host_safe_read(a, b, n)) { fprintf(stderr, "[memscan] %s 0x%llx: not readable\n", tag, (unsigned long long)a); return; }
    for (size_t i = 0; i < n; i++) { snprintf(hex + i * 3, 4, "%02x ", b[i]); txt[i] = b[i] >= 32 && b[i] < 127 ? (char)b[i] : '.'; }
    txt[n] = 0;
    fprintf(stderr, "[memscan] %s 0x%llx: %s |%s|\n", tag, (unsigned long long)a, hex, txt);
}

/* logs each match with its region and the bytes round it; kind 'w' 4-byte word, 'q' 8-byte word, 's' bytes,
   'r' 8-byte word in [pat[0], pat[1]) */
static int g_only_game;   /* scan only the mappings that start within the game module's reach (memscan ptr ... game) */
static void scan(int kind, const void *pat, size_t pn) {
    const uint64_t *range = pat; if (kind == 'r') pn = 8;
    FILE *maps = fopen("/proc/self/maps", "r"); if (!maps) { fprintf(stderr, "[memscan] no /proc/self/maps\n"); return; }
    uint8_t *buf = malloc(MEMSCAN_CHUNK + 64); if (!buf) { fclose(maps); return; }
    uint64_t own0 = (uintptr_t)buf, own1 = own0 + MEMSCAN_CHUNK + 64, scanned = 0; int hits = 0, regions = 0;
    uint64_t game = (uintptr_t)runtime_module_base("main"); char line[512];
    fprintf(stderr, "[memscan] game module at 0x%llx\n", (unsigned long long)game);
    while (fgets(line, sizeof line, maps) && hits < MEMSCAN_HITS) {
        unsigned long long s, e; char perm[8] = "", name[256] = "";
        if (sscanf(line, "%llx-%llx %7s %*s %*s %*s %255[^\n]", &s, &e, perm, name) < 3 || perm[0] != 'r' || perm[1] != 'w') continue;
        if (e - s > (16ull << 30)) continue;
        if (g_only_game && (s < game || s >= game + (256ull << 20))) continue;
        regions++;
        for (uint64_t a = s; a < e && hits < MEMSCAN_HITS; a += MEMSCAN_CHUNK) {
            size_t n = e - a < MEMSCAN_CHUNK + pn ? (size_t)(e - a) : MEMSCAN_CHUNK + pn - 1;   /* overlap: a match across chunks */
            if (!host_safe_read(a, buf, n)) continue;
            scanned += n;
            size_t step = kind == 'w' ? 4 : kind == 'q' || kind == 'r' ? 8 : 1;
            for (size_t i = 0; i + pn <= n && hits < MEMSCAN_HITS; i += step) {
                if (kind == 'r') { uint64_t q; memcpy(&q, buf + i, 8); if (q < range[0] || q >= range[1]) continue; }
                else if (memcmp(buf + i, pat, pn)) continue;
                uint64_t at = a + i; if (at >= own0 && at < own1) continue;
                if (i >= MEMSCAN_CHUNK && a + MEMSCAN_CHUNK < e) continue;   /* found again by the next chunk */
                char rel[40] = ""; if (at >= game && at < game + (256ull << 20)) snprintf(rel, sizeof rel, ", game +0x%llx", (unsigned long long)(at - game));
                fprintf(stderr, "[memscan] hit 0x%llx (region 0x%llx +0x%llx %s%s)\n", (unsigned long long)at, s, (unsigned long long)(at - s), name[0] ? name : "anon", rel);
                dump_line("  at", at & ~15ull, 48);
                hits++;
            }
        }
    }
    free(buf); fclose(maps);
    fprintf(stderr, "[memscan] done: %d hits%s in %d regions, %llu MB\n", hits, hits == MEMSCAN_HITS ? " (the most listed)" : "", regions, (unsigned long long)(scanned >> 20));
}

static uint32_t mm3(const char *key) {   /* MurmurHash3-32, seed 0: GameData key hashes */
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
int runtime_gmd_array(uint32_t hash, unsigned type, void *out, unsigned max);
static void gmd_print(const char *kind, const char *key) {
    enum { MAX = 1024 }; static char t[MAX][64]; static int32_t v[MAX];
    int isint = !strcmp(kind, "int"), n = runtime_gmd_array(mm3(key), isint ? 3 : 17, isint ? (void *)v : (void *)t, MAX), shown = 0;
    fprintf(stderr, "[memscan] gmd %s (%08x): %d elements\n", key, mm3(key), n);
    for (int i = 0; i < n && i < MAX; i++) {
        if (isint ? v[i] == 0 : !t[i][0]) continue;
        if (isint) fprintf(stderr, "[memscan]   [%d] %d\n", i, v[i]); else fprintf(stderr, "[memscan]   [%d] %s\n", i, t[i]);
        if (++shown >= 64) break;
    }
}

/* the write watch: one at a time. The fault handler (runtime_mem_watch_fault, from loader.c's crash handler) only
   touches these atomics and mprotect */
enum { WATCH_SITES = 64, WATCH_FRAMES = 10 };
static _Atomic uint64_t g_w_page0, g_w_page1, g_w_lo, g_w_hi;   /* the protected pages, the watched bytes */
static _Atomic int g_w_open;                                    /* 1 while the pages are writable */
static struct { _Atomic uint64_t pc; uint64_t lr, frame[WATCH_FRAMES]; _Atomic unsigned n, on_target; } g_w_site[WATCH_SITES];
static _Atomic unsigned g_w_lost;
int runtime_mem_watch_fault(void *addr, void *context) {
    uint64_t a = (uintptr_t)addr, p0 = atomic_load(&g_w_page0), p1 = atomic_load(&g_w_page1);
    if (!p0 || a < p0 || a >= p1) return 0;
    ucontext_t *u = context; uint64_t pc = UC_PC(u), game = (uintptr_t)runtime_module_base("main");
    int hit = a >= atomic_load(&g_w_lo) && a < atomic_load(&g_w_hi);
    for (unsigned i = 0; i < WATCH_SITES; i++) {
        uint64_t want = 0;
        if (atomic_load(&g_w_site[i].pc) == pc - game || atomic_compare_exchange_strong(&g_w_site[i].pc, &want, pc - game)) {
            if (!want) {   /* this thread just claimed the slot: the caller chain once, through the saved frame pointers */
                g_w_site[i].lr = UC_LR(u) - game;
                uint64_t fp = UC_FP(u), rec[2];
                for (int f = 0; f < WATCH_FRAMES && fp && !(fp & 7) && host_safe_read(fp, rec, 16); f++, fp = rec[0]) {
                    g_w_site[i].frame[f] = rec[1] - game;
                    if (rec[0] <= fp) break;   /* frames go up the stack */
                }
            }
            atomic_fetch_add(&g_w_site[i].n, 1); if (hit) atomic_fetch_add(&g_w_site[i].on_target, 1);
            goto open;
        }
    }
    atomic_fetch_add(&g_w_lost, 1);
open:
    mprotect((void *)(uintptr_t)p0, p1 - p0, PROT_READ | PROT_WRITE);
    atomic_store(&g_w_open, 1);
    return 1;   /* the write runs again, now allowed */
}
static void *watch_thread(void *arg) {
    double secs = *(double *)arg; free(arg);
    uint64_t p0 = atomic_load(&g_w_page0), p1 = atomic_load(&g_w_page1);
    struct timespec t0, t; clock_gettime(CLOCK_MONOTONIC, &t0);
    for (;;) {
        clock_gettime(CLOCK_MONOTONIC, &t);
        if ((double)(t.tv_sec - t0.tv_sec) + (t.tv_nsec - t0.tv_nsec) / 1e9 > secs) break;
        if (atomic_exchange(&g_w_open, 0)) mprotect((void *)(uintptr_t)p0, p1 - p0, PROT_READ);
        usleep(2000);
    }
    mprotect((void *)(uintptr_t)p0, p1 - p0, PROT_READ | PROT_WRITE);
    atomic_store(&g_w_page0, 0);
    usleep(20000);   /* a handler that read the old pages finishes */
    fprintf(stderr, "[memscan] watch done (%.0f s); writers, those to the watched bytes first:\n", secs);
    for (int pass = 0; pass < 2; pass++)
        for (unsigned i = 0; i < WATCH_SITES; i++) {
            uint64_t pc = atomic_load(&g_w_site[i].pc); unsigned n = atomic_load(&g_w_site[i].n), on = atomic_load(&g_w_site[i].on_target);
            if (!pc || (pass == 0) != (on > 0)) continue;
            fprintf(stderr, "[memscan]   pc game+0x%llx lr game+0x%llx: %u writes, %u to the watched bytes\n", (unsigned long long)pc, (unsigned long long)g_w_site[i].lr, n, on);
            for (int f = 0; f < WATCH_FRAMES && g_w_site[i].frame[f]; f++) fprintf(stderr, "[memscan]     from game+0x%llx\n", (unsigned long long)g_w_site[i].frame[f]);
        }
    if (atomic_load(&g_w_lost)) fprintf(stderr, "[memscan]   %u writes from more places than listed\n", atomic_load(&g_w_lost));
    return NULL;
}
static void watch_start(uint64_t lo, uint64_t hi, double secs) {
    if (atomic_load(&g_w_page0)) { fprintf(stderr, "[memscan] a watch is already running\n"); return; }
    uint64_t ps = (uint64_t)sysconf(_SC_PAGESIZE), p0 = lo & ~(ps - 1), p1 = (hi + ps - 1) & ~(ps - 1), rs, rsz, prot;
    if (!host_region(p0, &rs, &rsz, &prot) || rs > p0 || rs + rsz < p1 || (prot & 3) != 3) { fprintf(stderr, "[memscan] watch: 0x%llx is not plain read-write memory\n", (unsigned long long)lo); return; }
    memset(g_w_site, 0, sizeof g_w_site); atomic_store(&g_w_lost, 0);
    atomic_store(&g_w_lo, lo); atomic_store(&g_w_hi, hi); atomic_store(&g_w_page1, p1); atomic_store(&g_w_open, 0);
    atomic_store(&g_w_page0, p0);
    double *a = malloc(sizeof *a); *a = secs;
    fprintf(stderr, "[memscan] watching writes to 0x%llx..0x%llx (pages 0x%llx..0x%llx) for %.0f s\n", (unsigned long long)lo, (unsigned long long)hi, (unsigned long long)p0, (unsigned long long)p1, secs);
    if (mprotect((void *)(uintptr_t)p0, p1 - p0, PROT_READ)) { atomic_store(&g_w_page0, 0); free(a); fprintf(stderr, "[memscan] watch: mprotect failed\n"); return; }
    pthread_t t; if (pthread_create(&t, NULL, watch_thread, a)) { mprotect((void *)(uintptr_t)p0, p1 - p0, PROT_READ | PROT_WRITE); atomic_store(&g_w_page0, 0); free(a); return; }
    pthread_detach(t);
}
int runtime_gmd_where(uint32_t hash, unsigned type, uint64_t *array, unsigned *element);

/* 1 when the line is one of these commands (then done) */
int runtime_mem_command(const char *line) {
    char text[256]; unsigned long long v, a; unsigned n; int used = 0;
    if (sscanf(line, "memscan u32 %llx %n", &v, &used) == 1 && used) { uint32_t w = (uint32_t)v; fprintf(stderr, "[memscan] u32 %08x\n", w); scan('w', &w, 4); return 1; }
    if (sscanf(line, "memscan u64 %llx %n", &v, &used) == 1 && used) { uint64_t q = v; fprintf(stderr, "[memscan] u64 %llx\n", v); scan('q', &q, 8); return 1; }
    if (sscanf(line, "memscan ptr %llx %llx %n", &v, &a, &used) == 2 && a > v) {
        uint64_t r[2] = {v, a}; g_only_game = used && !strncmp(line + used, "game", 4);
        fprintf(stderr, "[memscan] ptr %llx..%llx%s\n", v, a, g_only_game ? " (game module only)" : ""); scan('r', r, 8); g_only_game = 0; return 1;
    }
    if (!strncmp(line, "memscan str ", 12)) {
        snprintf(text, sizeof text, "%s", line + 12); text[strcspn(text, "\r\n")] = 0;
        if (!text[0]) { fprintf(stderr, "[memscan] str needs a text\n"); return 1; }
        fprintf(stderr, "[memscan] str \"%s\"\n", text); scan('s', text, strlen(text)); return 1;
    }
    if (!strncmp(line, "gmd int ", 8) || !strncmp(line, "gmd str ", 8)) {
        char kind[4] = {line[4], line[5], line[6], 0};
        snprintf(text, sizeof text, "%s", line + 8); text[strcspn(text, "\r\n")] = 0;
        gmd_print(kind, text); return 1;
    }
    { char kind[4], key[200]; double secs;
      if (sscanf(line, "memwatch gmd %3s %199s %lf", kind, key, &secs) == 3) {
          uint64_t arr; unsigned el; int isint = !strcmp(kind, "int"), len = runtime_gmd_where(mm3(key), isint ? 3 : 17, &arr, &el);
          if (len <= 0 || secs <= 0 || secs > 600) fprintf(stderr, "[memscan] memwatch: no array %s (or bad seconds)\n", key);
          else watch_start(arr, arr + (uint64_t)len * el, secs);
          return 1;
      } }
    if (sscanf(line, "memdump %llx %u", &a, &n) == 2) {
        if (n > 4096) n = 4096;
        fprintf(stderr, "[memscan] dump 0x%llx, %u bytes\n", a, n);
        for (unsigned o = 0; o < n; o += 32) dump_line("  ", a + o, n - o < 32 ? n - o : 32);
        return 1;
    }
    return 0;
}
