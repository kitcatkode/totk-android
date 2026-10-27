/* The running game's GameData (the values progress.sav is written from), read-only, for the bottom screen's live
   inventory (android/BOTTOM_SCREEN.md step F). Found on 1.4.3 with the memscan commands (mem_scan.c), 2026-10-26:
     main+0x3ad66b0          the GameData object (vtable main+0x39403b0)
     +8 + type * 0x60        one container per value type, numbered as in the save (0 Bool, 3 IntArray, 17
                             String64Array); its vtable main+0x39403d0 + type * 0xa0
       +0x10 the values, +0x18 an open hash table of {u32 key hash, u32 index}, slot hash % size, the next slot on a
       clash, hash 0 empty; +0x20 u32 count, +0x24 u32 table size
     a value, one per index   size and vtable per type (below); its last four words {count, the live array, count,
                             another copy}
     String64 element        0x50 bytes: a pointer to its text, u32 capacity, the 64-byte text at +0xc
   Every pointer is read through host_safe_read and every vtable checked, so a game that moved anything gives "none"
   rather than wrong data. Other game versions have no addresses: none. Reads race the game's own writes; a reader that
   needs a consistent picture reads twice. */
#include "runtime.h"
#include "host_os.h"
#include <stdlib.h>
#include <string.h>

int runtime_gmd_array(uint32_t hash, unsigned type, void *out, unsigned max);
int runtime_gmd_where(uint32_t hash, unsigned type, uint64_t *array, unsigned *element);

#if defined(TOTK_GAME_V143) && !defined(GAME_BOTW)
enum { GMD_SLOT = 0x3ad66b0, GMD_VTABLE = 0x39403b0, GMD_TYPE_VTABLE = 0x39403d0, GMD_TYPE_SIZE = 0x60, GMD_TYPE_STEP = 0xa0 };
static const struct { unsigned type, size, vtable, element; } g_kind[] = {
    {3, 0x40, 0x3941a00, 4},      /* IntArray: int32 elements */
    {17, 0x90, 0x3941f40, 0x50},  /* String64Array */
};
static uint64_t rd(uint64_t a) { uint64_t v = 0; return a && host_safe_read(a, &v, 8) ? v : 0; }

/* where the live array with this key hash is: its length (or -1), the array and its element size */
int runtime_gmd_where(uint32_t hash, unsigned type, uint64_t *array, unsigned *element) {
    unsigned k = 0; while (k < sizeof g_kind / sizeof *g_kind && g_kind[k].type != type) k++;
    if (k == sizeof g_kind / sizeof *g_kind || !hash) return -1;
    uint64_t game = (uintptr_t)runtime_module_base("main"); if (!game) return -1;
    uint64_t gd = rd(game + GMD_SLOT); if (!gd || rd(gd) != game + GMD_VTABLE) return -1;
    uint64_t c = gd + 8 + (uint64_t)type * GMD_TYPE_SIZE; uint32_t nc[2];
    if (rd(c) != game + GMD_TYPE_VTABLE + (uint64_t)type * GMD_TYPE_STEP || !host_safe_read(c + 0x20, nc, 8)) return -1;
    uint64_t values = rd(c + 0x10), table = rd(c + 0x18); uint32_t n = nc[0], size = nc[1];
    if (!values || !table || !n || n > 0x10000 || size < n || size > 0x40000) return -1;
    uint32_t index = UINT32_MAX;
    for (uint32_t i = 0, s = hash % size; i < size; i++, s = s + 1 == size ? 0 : s + 1) {
        uint32_t e[2]; if (!host_safe_read(table + (uint64_t)s * 8, e, 8)) return -1;
        if (!e[0]) break;
        if (e[0] == hash) { index = e[1]; break; }
    }
    if (index >= n) return -1;
    uint64_t v = values + (uint64_t)index * g_kind[k].size;
    if (rd(v) != game + g_kind[k].vtable) return -1;
    uint32_t len; uint64_t arr = rd(v + g_kind[k].size - 0x18);
    if (!host_safe_read(v + g_kind[k].size - 0x20, &len, 4) || !arr || len > 4096) return -1;
    *array = arr; *element = g_kind[k].element;
    return (int)len;
}
/* copies up to max elements of the array with this key hash (IntArray: int32 each; String64Array: 64-byte texts),
   returns the array's length (its slots, empty ones included) or -1 */
int runtime_gmd_array(uint32_t hash, unsigned type, void *out, unsigned max) {
    uint64_t arr; unsigned el; int got = runtime_gmd_where(hash, type, &arr, &el);
    if (got < 0) return -1;
    uint32_t len = (uint32_t)got, m = len < max ? len : max;
    if (!m) return (int)len;
    if (type == 3) return host_safe_read(arr, out, (size_t)m * el) ? (int)len : -1;
    uint8_t *raw = malloc((size_t)m * el); if (!raw) return -1;
    int ok = host_safe_read(arr, raw, (size_t)m * el);
    for (uint32_t i = 0; ok && i < m; i++) { char *t = (char *)out + i * 64ull; memcpy(t, raw + i * el + 0xc, 64); t[63] = 0; }
    free(raw);
    return ok ? (int)len : -1;
}
#else
int runtime_gmd_where(uint32_t hash, unsigned type, uint64_t *array, unsigned *element) { (void)hash; (void)type; (void)array; (void)element; return -1; }
int runtime_gmd_array(uint32_t hash, unsigned type, void *out, unsigned max) { (void)hash; (void)type; (void)out; (void)max; return -1; }
#endif
