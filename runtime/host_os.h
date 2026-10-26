/* The few host-OS services the loader and its diagnostics need, for Darwin (macOS / iOS) and Linux / Android.
   On Darwin each one is the call loader.c always made (Mach VM, CommonCrypto, libkern, mach_absolute_time); the
   Linux / Android versions are in host_os.c. */
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <signal.h>
#include <sys/ucontext.h>

#if defined(__APPLE__)
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <libkern/OSCacheControl.h>
#include <CommonCrypto/CommonDigest.h>
#include "mach_vm_compat.h"

/* Copy n bytes from address a without faulting on a bad address: 1 on success. */
static inline int host_safe_read(uint64_t a, void *out, size_t n) {
    mach_vm_size_t got = 0;
    return mach_vm_read_overwrite(mach_task_self(), a, n, (mach_vm_address_t)(uintptr_t)out, &got) == KERN_SUCCESS && got == n;
}
static inline void host_icache_flush(void *p, size_t n) { sys_icache_invalidate(p, n); }
static inline void host_dcache_flush(void *p, size_t n) { sys_dcache_flush(p, n); }
static inline void host_sha256(const void *p, size_t n, unsigned char out[32]) { CC_SHA256(p, (CC_LONG)n, out); }
typedef CC_SHA256_CTX host_sha256_ctx;
static inline void host_sha256_init(host_sha256_ctx *c) { CC_SHA256_Init(c); }
static inline void host_sha256_update(host_sha256_ctx *c, const void *p, size_t n) { CC_SHA256_Update(c, p, (CC_LONG)n); }
static inline void host_sha256_final(host_sha256_ctx *c, unsigned char out[32]) { CC_SHA256_Final(out, c); }
static inline uint64_t host_now_ns(void) {
    mach_timebase_info_data_t tb; mach_timebase_info(&tb);
    return (uint64_t)((__uint128_t)mach_absolute_time() * tb.numer / tb.denom);
}
/* The mapping that contains a (or the next one above it): 1 with its start, size and protection
   (PROT_* bits, max protection << 8 where the OS has one). */
static inline int host_region(uint64_t a, uint64_t *start, uint64_t *size, uint64_t *prot) {
    mach_vm_address_t ra = a; mach_vm_size_t rs = 0; vm_region_basic_info_data_64_t ri;
    mach_msg_type_number_t rc = VM_REGION_BASIC_INFO_COUNT_64; mach_port_t ro = 0;
    if (mach_vm_region(mach_task_self(), &ra, &rs, VM_REGION_BASIC_INFO_64, (vm_region_info_t)&ri, &rc, &ro) != KERN_SUCCESS) return 0;
    if (ro) mach_port_deallocate(mach_task_self(), ro);
    *start = ra; *size = rs; *prot = (uint64_t)ri.protection | ((uint64_t)ri.max_protection << 8);
    return 1;
}
#define UC_PC(u) ((u)->uc_mcontext->__ss.__pc)
#define UC_SP(u) ((u)->uc_mcontext->__ss.__sp)
#define UC_FP(u) ((u)->uc_mcontext->__ss.__fp)
#define UC_LR(u) ((u)->uc_mcontext->__ss.__lr)
#define UC_X(u) ((u)->uc_mcontext->__ss.__x)
static inline uint64_t host_uc_esr(ucontext_t *u) { return u->uc_mcontext->__es.__esr; }

#else /* Linux / Android */

int host_safe_read(uint64_t a, void *out, size_t n);
void host_sha256(const void *p, size_t n, unsigned char out[32]);
typedef struct { uint32_t h[8]; uint64_t bytes; uint8_t buf[64]; unsigned used; } host_sha256_ctx;
void host_sha256_init(host_sha256_ctx *c);
void host_sha256_update(host_sha256_ctx *c, const void *p, size_t n);
void host_sha256_final(host_sha256_ctx *c, unsigned char out[32]);
int host_region(uint64_t a, uint64_t *start, uint64_t *size, uint64_t *prot);
uint64_t host_now_ns(void);
static inline void host_icache_flush(void *p, size_t n) { __builtin___clear_cache((char *)p, (char *)p + n); }
void host_dcache_flush(void *p, size_t n);
#if defined(__x86_64__)   /* desktop builds of the graphics tests (android/gfx/test_*.c); the game is arm64 only */
#define UC_PC(u) ((uint64_t)(u)->uc_mcontext.gregs[REG_RIP])
#define UC_SP(u) ((uint64_t)(u)->uc_mcontext.gregs[REG_RSP])
#define UC_FP(u) ((uint64_t)(u)->uc_mcontext.gregs[REG_RBP])
#define UC_LR(u) ((uint64_t)0)
#else
#define UC_PC(u) ((u)->uc_mcontext.pc)
#define UC_SP(u) ((u)->uc_mcontext.sp)
#define UC_FP(u) ((u)->uc_mcontext.regs[29])
#define UC_LR(u) ((u)->uc_mcontext.regs[30])
#define UC_X(u) ((uint64_t *)(u)->uc_mcontext.regs)
#endif
uint64_t host_uc_esr(ucontext_t *u);   /* the kernel's esr_context record, 0 if the frame has none */
/* TOTK_THP (experiment, off by default): transparent huge pages for guest memory. "1" / "all", or a comma list of the
   parts: "heap" (the guest heap, shmem) and "code" (the game's modules). Needs the kernel's THP set to madvise / advise
   (thor-tools tune profile thp). 1 when part is asked for. */
int host_thp(const char *part);

#endif

static inline void host_sha256_hex(const void *p, size_t n, char hex[65]) {
    static const char digits[] = "0123456789abcdef";
    unsigned char d[32]; host_sha256(p, n, d);
    for (unsigned i = 0; i < 32; i++) { hex[2 * i] = digits[d[i] >> 4]; hex[2 * i + 1] = digits[d[i] & 15]; }
    hex[64] = 0;
}
