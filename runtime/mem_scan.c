/* Memory search for finding the game's live data (android/BOTTOM_SCREEN.md step F: the pouches in GameData), as
   diagnostic commands on TOTK_INPUT_FILE. Read-only: every read goes through host_safe_read, so a mapping that goes
   away during a scan costs nothing. Results go to the log, "[memscan] ...".
     memscan u32 <hex>      4-byte aligned words equal to it (a key hash)
     memscan u64 <hex>      8-byte aligned words equal to it (pointers to an address)
     memscan ptr <lo> <hi> [game]  8-byte aligned words in [lo, hi) (pointers into an object); "game": only in the
                            game module's own mappings (its static data: where a chain of pointers starts)
     memscan str <text>     the bytes of the text, anywhere (an item id)
     memdump <hex addr> <n> n bytes (up to 4096) as hex and text
   Scans every readable and writable mapping but the scan's own buffer; at most MEMSCAN_HITS hits are listed. */
#include "runtime.h"
#include "host_os.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
    if (sscanf(line, "memdump %llx %u", &a, &n) == 2) {
        if (n > 4096) n = 4096;
        fprintf(stderr, "[memscan] dump 0x%llx, %u bytes\n", a, n);
        for (unsigned o = 0; o < n; o += 32) dump_line("  ", a + o, n - o < 32 ? n - o : 32);
        return 1;
    }
    return 0;
}
