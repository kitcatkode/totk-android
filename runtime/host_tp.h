/* Host thread slots: where rewritten guest code finds the per-thread values it uses instead of x18, TPIDR_EL0 and
   TPIDRRO_EL0 (virtual x18, the guest TLS pointer, the Horizon thread-local region, the host gate) and where the
   host-stack switch keeps its stack top.

   The rewriter (third_party/d2r/hostreg.py) emits "slot base" + key-page offset. The slot base is
     Darwin (macOS / iOS):  TPIDRRO_EL0, the TSD array; slot i is pthread key runtime_keys[i], offset key * 8.
     Linux / Android:       *(TPIDR_EL0 + 16). Bionic reserves that TCB word for apps from API level 29
                            (TLS_SLOT_APP, bionic/libc/platform/bionic/tls_defines.h); glibc puts the executable's
                            first TLS word there (host_tp.c keeps one there and checks it). It holds this thread's
                            own slot block, allocated by runtime_slots_thread_attach; slot i is at offset i * 8.
   Images rewritten for one of these cannot run on the other: the loader checks which register an image reads. */
#pragma once
#include <stdint.h>
#include <pthread.h>

#define RUNTIME_NSLOTS 8        /* key-page entries; slot 4 doubles as the host-stack slot (no TOTK parameter gate uses it) */
#define RUNTIME_SLOT_PTR_OFF 16 /* Linux / Android: offset of the slot-block pointer from TPIDR_EL0 */

#if !defined(__APPLE__)
extern uint64_t runtime_slot_off[RUNTIME_NSLOTS];   /* byte offset of slot i from runtime_host_slots(): key-page values */
#endif

static inline uintptr_t runtime_host_slots(void) {
    uintptr_t tp;
#if defined(__APPLE__)
    __asm__ volatile("mrs %0, tpidrro_el0" : "=r"(tp));
    return tp;
#else
    __asm__ volatile("mrs %0, tpidr_el0" : "=r"(tp));
    return *(volatile uintptr_t *)(tp + RUNTIME_SLOT_PTR_OFF);
#endif
}

#if !defined(__APPLE__)
void runtime_slots_process_init(void);   /* once, before any module is mapped */
void runtime_slots_thread_attach(void);  /* every thread that runs guest code, before it does */
void runtime_slots_thread_detach(void);
void runtime_slot_set(unsigned i, void *value);
void *runtime_slot_get(unsigned i);
#endif
