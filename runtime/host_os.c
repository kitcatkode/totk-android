/* Linux / Android versions of the host services in host_os.h. */
#if !defined(__APPLE__)
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "host_os.h"
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

/* Guarded read without a fault: the kernel copies from our address space into a pipe and reports EFAULT for an
   address that is not mapped readable, so a bad pointer ends a diagnostic walk instead of the process. One pipe per
   thread (created on first use; pipe2/write/read are async-signal-safe, so crash handlers may use it). */
static __thread int read_pipe[2];
static __thread int read_pipe_ready;
int host_safe_read(uint64_t a, void *out, size_t n) {
    if (!read_pipe_ready) {
        if (pipe2(read_pipe, O_NONBLOCK | O_CLOEXEC)) return 0;
        read_pipe_ready = 1;
    }
    uint8_t *dst = out;
    while (n) {
        size_t chunk = n > 4096 ? 4096 : n;
        ssize_t w = write(read_pipe[1], (const void *)(uintptr_t)a, chunk);
        if (w <= 0) return 0;
        ssize_t r = read(read_pipe[0], dst, (size_t)w);
        if (r != w) return 0;
        a += (uint64_t)w; dst += w; n -= (size_t)w;
    }
    return 1;
}

uint64_t host_now_ns(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000u + (uint64_t)t.tv_nsec;
}

/* Clean and invalidate to the point of coherency (Linux enables dc civac at EL0). */
void host_dcache_flush(void *p, size_t n) {
    uint64_t ctr; __asm__ volatile("mrs %0, ctr_el0" : "=r"(ctr));
    uintptr_t line = (uintptr_t)4 << ((ctr >> 16) & 15);
    for (uintptr_t a = (uintptr_t)p & ~(line - 1); a < (uintptr_t)p + n; a += line) __asm__ volatile("dc civac, %0" :: "r"(a) : "memory");
    __asm__ volatile("dsb ish" ::: "memory");
}

/* /proc/self/maps, read with plain system calls (usable from a signal handler). */
static int hex_field(const char **s, uint64_t *v) {
    uint64_t x = 0; int any = 0;
    for (;; (*s)++) {
        char c = **s; unsigned d;
        if (c >= '0' && c <= '9') d = (unsigned)(c - '0');
        else if (c >= 'a' && c <= 'f') d = (unsigned)(c - 'a' + 10);
        else break;
        x = x << 4 | d; any = 1;
    }
    *v = x; return any;
}
int host_region(uint64_t a, uint64_t *start, uint64_t *size, uint64_t *prot) {
    int fd = open("/proc/self/maps", O_RDONLY | O_CLOEXEC);
    if (fd < 0) return 0;
    char buf[8192], line[512]; size_t ll = 0; ssize_t got; int found = 0;
    uint64_t best_lo = 0, best_hi = 0, best_prot = 0;
    while (!found && (got = read(fd, buf, sizeof buf)) > 0) {
        for (ssize_t i = 0; i < got && !found; i++) {
            if (buf[i] != '\n') { if (ll < sizeof line - 1) line[ll++] = buf[i]; continue; }
            line[ll] = 0; ll = 0;
            const char *s = line; uint64_t lo, hi;
            if (!hex_field(&s, &lo) || *s++ != '-' || !hex_field(&s, &hi) || *s++ != ' ') continue;
            if (hi <= a) continue;
            /* first mapping that ends above a: the one containing it, or the next one up (mach_vm_region's rule) */
            best_lo = lo; best_hi = hi;
            best_prot = (s[0] == 'r' ? PROT_READ : 0) | (s[1] == 'w' ? PROT_WRITE : 0) | (s[2] == 'x' ? PROT_EXEC : 0);
            found = 1;
        }
    }
    close(fd);
    if (!found) return 0;
    *start = best_lo; *size = best_hi - best_lo; *prot = best_prot;
    return 1;
}

uint64_t host_uc_esr(ucontext_t *u) {
    const uint8_t *p = (const uint8_t *)u->uc_mcontext.__reserved;
    const uint8_t *end = p + sizeof u->uc_mcontext.__reserved;
    while (p + 8 <= end) {
        uint32_t magic, size;
        memcpy(&magic, p, 4); memcpy(&size, p + 4, 4);
        if (!magic || size < 8 || p + size > end) break;
        if (magic == 0x45535201u && size >= 16) { uint64_t esr; memcpy(&esr, p + 8, 8); return esr; }   /* ESR_MAGIC */
        p += size;
    }
    return 0;
}

/* SHA-256 (FIPS 180-4), for the image checks that CommonCrypto does on Darwin. */
static const uint32_t K[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,
    0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,
    0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,
    0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};
#define ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))
static void block(uint32_t h[8], const uint8_t *p) {
    uint32_t w[64];
    for (int i = 0; i < 16; i++) w[i] = (uint32_t)p[4*i] << 24 | (uint32_t)p[4*i+1] << 16 | (uint32_t)p[4*i+2] << 8 | p[4*i+3];
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = ROR(w[i-15], 7) ^ ROR(w[i-15], 18) ^ (w[i-15] >> 3), s1 = ROR(w[i-2], 17) ^ ROR(w[i-2], 19) ^ (w[i-2] >> 10);
        w[i] = w[i-16] + s0 + w[i-7] + s1;
    }
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], k = h[7];
    for (int i = 0; i < 64; i++) {
        uint32_t t1 = k + (ROR(e, 6) ^ ROR(e, 11) ^ ROR(e, 25)) + ((e & f) ^ (~e & g)) + K[i] + w[i];
        uint32_t t2 = (ROR(a, 2) ^ ROR(a, 13) ^ ROR(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        k = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += k;
}
void host_sha256_init(host_sha256_ctx *c) {
    static const uint32_t h0[8] = {0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
    memcpy(c->h, h0, sizeof h0); c->bytes = 0; c->used = 0;
}
void host_sha256_update(host_sha256_ctx *c, const void *data, size_t n) {
    const uint8_t *p = data; c->bytes += n;
    if (c->used) {
        size_t k = 64 - c->used < n ? 64 - c->used : n;
        memcpy(c->buf + c->used, p, k); c->used += (unsigned)k; p += k; n -= k;
        if (c->used < 64) return;
        block(c->h, c->buf); c->used = 0;
    }
    while (n >= 64) { block(c->h, p); p += 64; n -= 64; }
    memcpy(c->buf, p, n); c->used = (unsigned)n;
}
void host_sha256_final(host_sha256_ctx *c, unsigned char out[32]) {
    uint64_t bits = c->bytes * 8;
    uint8_t pad[72] = {0x80}; size_t padn = (c->used < 56 ? 56 : 120) - c->used;
    host_sha256_update(c, pad, padn);
    uint8_t len[8]; for (int i = 0; i < 8; i++) len[i] = (uint8_t)(bits >> (56 - 8 * i));
    host_sha256_update(c, len, 8);
    for (int i = 0; i < 8; i++) { out[4*i] = (uint8_t)(c->h[i] >> 24); out[4*i+1] = (uint8_t)(c->h[i] >> 16); out[4*i+2] = (uint8_t)(c->h[i] >> 8); out[4*i+3] = (uint8_t)c->h[i]; }
}
void host_sha256(const void *data, size_t n, unsigned char out[32]) {
    host_sha256_ctx c; host_sha256_init(&c); host_sha256_update(&c, data, n); host_sha256_final(&c, out);
}

#if defined(__ANDROID__)
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
/* The replay thread (totk-translate) on one fast core, called by it once a frame (hle_nvn.c x_thread). Android has no
   time-constraint policy (darwin_compat's thread_policy_set fails). Measured on the Thor (2026-10-25, Lookout Landing,
   standard performance mode, thor-tools bench runs 1024_m_*): the replay takes ~17.5 ms a frame on the Cortex-X3
   (CPU 7) against ~24 ms where the scheduler puts it, +1-3 fps.
   A pin set once does not last: the thread's affinity goes back to all CPUs during a run (masks like df / 5f seen
   with thor-tune threads; presumably Android re-attaching the thread to its cpuset, which resets it), and a run that
   pinned once replayed at ~24.8 ms against ~18.9 with a check every frame. So this checks every frame (one
   sched_getaffinity) and pins again when it changed. Qualcomm's core control also pauses idle CPUs, the prime core
   included: a paused CPU is missing from sched_getaffinity's answer and pinning to it fails with EINVAL until it
   is back. Both are counted and summarised every 1800 frames, not logged each time.
   TOTK_REPLAY_CPU=<n> picks the CPU (default the last one: the prime core on Snapdragons), -1 leaves it to the scheduler. */
int host_thp(const char *part) {
    static const char *e; static int read;
    if (!read) { e = getenv("TOTK_THP"); read = 1; }
    if (!e || !*e || !strcmp(e, "0")) return 0;
    if (!strcmp(e, "1") || !strcmp(e, "all")) return 1;
    size_t n = strlen(part);
    for (const char *p = e; *p; ) {
        const char *c = strchr(p, ','); size_t l = c ? (size_t)(c - p) : strlen(p);
        if (l == n && !strncmp(p, part, n)) return 1;
        p += l + (c != NULL);
    }
    return 0;
}
void host_replay_cpu_tick(void) {
    static int cpu = -2; static unsigned long frames, repins, paused;   // only the replay thread calls this
    if (cpu == -2) {
        const char *e = getenv("TOTK_REPLAY_CPU");
        cpu = e && *e ? atoi(e) : (int)sysconf(_SC_NPROCESSORS_CONF) - 1;
        fprintf(stderr, cpu < 0 ? "[replay] CPU placement off: the scheduler places the replay\n"
                                : "[replay] pinned to CPU %d, checked every frame\n", cpu);
    }
    if (cpu < 0) return;
    cpu_set_t cur, want; CPU_ZERO(&want); CPU_SET(cpu, &want);
    if (sched_getaffinity(0, sizeof cur, &cur) != 0 || !CPU_EQUAL(&cur, &want)) {
        if (sched_setaffinity(0, sizeof want, &want) == 0) repins++;
        else if (errno == EINVAL) paused++;
        else { fprintf(stderr, "[replay] pinning to CPU %d failed: %s; placement off\n", cpu, strerror(errno)); cpu = -1; }
    }
    if (++frames % 1800 == 0)
        fprintf(stderr, "[replay] CPU %d over %lu frames: pinned again %lu times, core paused %lu times\n", cpu, frames, repins, paused);
}
#endif
#endif
