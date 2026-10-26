/* Checks the Vulkan backend's stage 1 without the game: memory pools over "guest" memory, GPU copies and uniform
   updates in command order, CPU steps, events and counters released only after the GPU finished, pool removal, and
   virtual pool aliases. Runs on the device (TOTK_VK_DRIVER picks the driver) or on a desktop with lavapipe.
     make -C android vktest NDK=...   then   adb shell TOTK_VK_DRIVER=/data/local/tmp/driver.so /data/local/tmp/vktest */
#include "nvn_obj.h"
#include "nvn_mtl.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>


static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL: " __VA_ARGS__); printf("\n"); } } while (0)

static uint8_t rec[1 << 16]; static size_t rn;
static void op(uint32_t code, const void *payload, uint32_t n) {
    uint32_t size = (8 + n + 7) & ~7u; op_hdr h = {code, size};
    memcpy(rec + rn, &h, 8); memcpy(rec + rn + 8, payload, n); memset(rec + rn + 8 + n, 0, size - 8 - n); rn += size;
}
static void op_copy(uint64_t s, uint64_t d, uint64_t n) { uint64_t a[3] = {s, d, n}; op(OP_COPY_B2B, a, 24); }
static void op_event(volatile uint32_t *v, uint32_t mode, uint32_t val) { uint8_t a[16]; memcpy(a, &v, 8); memcpy(a + 8, &mode, 4); memcpy(a + 12, &val, 4); op(OP_SIGNAL_EVENT, a, 16); }
static void op_counter(uint64_t type, uint64_t addr) { uint64_t a[2] = {type, addr}; op(OP_REPORT_COUNTER, a, 16); }
static void op_ub(uint64_t dst, const void *data, uint64_t n) { uint8_t a[16 + 256]; memcpy(a, &dst, 8); memcpy(a + 8, &n, 8); memcpy(a + 16, data, n); op(OP_UPDATE_UB, a, (uint32_t)(16 + n)); }
static uint64_t run(void) { op(OP_END, NULL, 0); mtl_sub_begin(); mtl_exec(rec); rn = 0; return mtl_sub_commit(); }

int main(void) {
    const size_t G = 64u << 20;   /* guest memory: shared anonymous, like runtime/services.c */
    uint8_t *g = mmap(NULL, G, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    for (size_t i = 0; i < 16u << 20; i += 4) *(uint32_t *)(g + i) = (uint32_t)i ^ 0x9e3779b9u;
    uint8_t *P = g, *Q = g + (32u << 20);            /* pool P: 16 MiB at the start; Q: 1 MiB, later */
    mtl_init();
    mtl_pool_add(P, 16u << 20);
    int same = 1; for (size_t i = 0; i < 16u << 20; i += 4) if (*(uint32_t *)(P + i) != ((uint32_t)i ^ 0x9e3779b9u)) { same = 0; break; }
    CHECK(same, "pool contents changed when the pool was registered");
    void *b; uint64_t off;
    CHECK(mtl_addr((uint64_t)(uintptr_t)(P + 0x1234), 16, &b, &off) && off == 0x1234, "mtl_addr inside the pool");
    CHECK(!mtl_addr((uint64_t)(uintptr_t)(Q), 16, &b, &off), "mtl_addr outside every pool");
    mtl_pool_add(P + (1u << 20), 1u << 20);   /* a pool carved from P: a view */
    CHECK(mtl_addr((uint64_t)(uintptr_t)(P + (1u << 20) + 8), 8, &b, &off) && off == (1u << 20) + 8, "view pool resolves into the owner");

    /* 1. GPU copy, uniform update, event and counter in one submission; the event must not be set before completion. */
    volatile uint32_t *ev = (volatile uint32_t *)(Q + 0x100); *ev = 7;
    uint64_t ctr = (uint64_t)(uintptr_t)(Q + 0x200); memset(Q + 0x200, 0xee, 16);
    memset(P + (4u << 20), 0, 4096);
    op_copy((uint64_t)(uintptr_t)P, (uint64_t)(uintptr_t)(P + (4u << 20)), 4096);
    uint32_t ub[16]; for (int i = 0; i < 16; i++) ub[i] = 0xc0de0000u + (uint32_t)i;
    op_ub((uint64_t)(uintptr_t)(P + (5u << 20)), ub, sizeof ub);
    op_copy((uint64_t)(uintptr_t)(P + (5u << 20)), (uint64_t)(uintptr_t)(P + (5u << 20) + 4096), sizeof ub);   /* ordered after the update */
    op_event(ev, 0, 42);
    op_event(ev, 1, 1);
    op_counter(NVN_COUNTER_TYPE_SAMPLES_PASSED, ctr);
    uint64_t f = run();
    CHECK(mtl_wait(f, 2000000000ull), "submission 1 completes");
    CHECK(*ev == 43, "event write then add: %u", *ev);
    uint64_t c[2]; memcpy(c, Q + 0x200, 16);
    CHECK(c[0] == 0 && c[1] != 0xeeeeeeeeeeeeeeeeull, "counter report written (no draw before it: 0 samples)");
    CHECK(!memcmp(P + (4u << 20), P, 4096), "GPU copy inside the pool");
    CHECK(!memcmp(P + (5u << 20), ub, sizeof ub), "GPU uniform update");
    CHECK(!memcmp(P + (5u << 20) + 4096, ub, sizeof ub), "copy after the update sees the update");

    /* 2. Copies the GPU cannot do run on the CPU in order: GPU writes X, CPU copies X out of the pool, GPU copies back. */
    memset(Q, 0, 4096);
    op_ub((uint64_t)(uintptr_t)(P + (6u << 20)), ub, 64);
    op_copy((uint64_t)(uintptr_t)(P + (6u << 20)), (uint64_t)(uintptr_t)Q, 64);          /* destination outside: CPU step */
    op_copy((uint64_t)(uintptr_t)Q, (uint64_t)(uintptr_t)(P + (6u << 20) + 128), 64);    /* source outside: CPU step */
    op_event(ev, 0, 100);
    f = run(); mtl_wait(f, 2000000000ull);
    CHECK(!memcmp(Q, ub, 64), "CPU step sees the GPU update before it");
    CHECK(!memcmp(P + (6u << 20) + 128, ub, 64), "CPU step back into the pool");
    CHECK(*ev == 100, "event after CPU steps");

    /* 3. Many submissions in flight: fences complete in order and mtl_finish waits for all. */
    for (int i = 0; i < 200; i++) { op_copy((uint64_t)(uintptr_t)P, (uint64_t)(uintptr_t)(P + (8u << 20) + (size_t)(i % 16) * 65536), 65536); op_event(ev, 1, 1); f = run(); }
    mtl_finish();
    CHECK(*ev == 300, "200 submissions each add 1: %u", *ev);
    CHECK(mtl_wait(f, 0), "last fence done after mtl_finish");

    /* 4. Virtual pool aliases (export / heap pools only: the host path aliases with mremap in totk_vpool.c). */
    uint8_t *V = g + (48u << 20);
    if (mtl_pool_alias(V, P + (2u << 20), 1u << 20)) {
        mtl_vpool_add(V, 2u << 20);
        CHECK(!memcmp(V, P + (2u << 20), 1u << 20), "alias shows the physical pages");
        P[(2u << 20) + 5] ^= 0xff; CHECK(V[5] == P[(2u << 20) + 5], "CPU write through the physical view shows in the alias");
        CHECK(mtl_addr((uint64_t)(uintptr_t)(V + 64), 64, &b, &off) && off == (2u << 20) + 64, "alias resolves to the physical pool's buffer");
        op_ub((uint64_t)(uintptr_t)(V + 256), ub, 64);   /* GPU write through the virtual address */
        f = run(); mtl_wait(f, 2000000000ull);
        CHECK(!memcmp(P + (2u << 20) + 256, ub, 64), "GPU write via the virtual pool lands in the physical pool");
        mtl_vpool_del(V);
        printf("virtual pool aliases: checked\n");
    } else printf("virtual pool aliases: host-pointer pools (aliased by totk_vpool.c with mremap), not checked here\n");

    /* 5. Removing pools keeps their contents in ordinary memory. */
    uint8_t keep[4096]; memcpy(keep, P + (4u << 20), 4096);
    mtl_pool_del(P + (1u << 20));
    mtl_pool_del(P);
    CHECK(!memcmp(P + (4u << 20), keep, 4096), "contents kept when the pool is removed");
    CHECK(!mtl_addr((uint64_t)(uintptr_t)P, 16, &b, &off), "removed pool no longer resolves");
    P[0] = 0x55; CHECK(P[0] == 0x55, "guest memory writable after removal");
    printf("%s: %d checks, %d failed\n", fails ? "FAILED" : "ok", checks, fails);
    return fails != 0;
}
