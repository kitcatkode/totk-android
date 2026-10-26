/* Linux / Android stand-ins for the Darwin interfaces the graphics layer (third_party/d2r_gfx) uses, so those files
   build unchanged for Android. Forced into every graphics C file of the Linux / Android build (-include) and reached
   through the stub <mach/...>, <pthread/qos.h>, <malloc/malloc.h>, <mach-o/...> headers next to it.

   What is real here: clocks (mach_absolute_time and friends count nanoseconds on CLOCK_MONOTONIC, so timebase is
   1/1), mach_wait_until, clock_gettime_nsec_np, malloc_size, the calling thread's id, system CPU time and the
   process footprint (resident memory), guarded memory reads, and getsectiondata for the linker tables the graphics
   layer registers. What only reports failure: Mach thread policies, other threads' registers and counters, task
   thread lists. Those feed diagnostics and the Darwin present pacing, which the Android frontend replaces. */
#pragma once
#if !defined(__APPLE__)
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <errno.h>
#include <fcntl.h>
#include <malloc.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

/* ---- clocks ---- */
#ifndef CLOCK_UPTIME_RAW
#define CLOCK_UPTIME_RAW CLOCK_MONOTONIC_RAW
#endif
static inline uint64_t clock_gettime_nsec_np(clockid_t c) {
    struct timespec t; if (clock_gettime(c, &t)) return 0;
    return (uint64_t)t.tv_sec * 1000000000u + (uint64_t)t.tv_nsec;
}
typedef struct { uint32_t numer, denom; } mach_timebase_info_data_t;
typedef mach_timebase_info_data_t *mach_timebase_info_t;
static inline int mach_timebase_info(mach_timebase_info_data_t *t) { t->numer = 1; t->denom = 1; return 0; }
static inline uint64_t mach_absolute_time(void) { return clock_gettime_nsec_np(CLOCK_MONOTONIC); }
static inline uint64_t mach_continuous_time(void) { return clock_gettime_nsec_np(CLOCK_BOOTTIME); }
static inline int mach_wait_until(uint64_t deadline) {
    struct timespec t = {(time_t)(deadline / 1000000000u), (long)(deadline % 1000000000u)};
    while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &t, NULL) == EINTR) {}
    return 0;
}

/* ---- Mach basics ---- */
typedef int kern_return_t;
#define KERN_SUCCESS 0
#define KERN_FAILURE 5
typedef unsigned int mach_port_t, natural_t, mach_msg_type_number_t, thread_act_t, task_t;
typedef thread_act_t *thread_act_array_t;
typedef int boolean_t, integer_t, vm_prot_t, thread_flavor_t, task_flavor_t;
typedef uintptr_t vm_address_t, vm_size_t;
typedef uint64_t mach_vm_address_t, mach_vm_size_t;
#define MACH_PORT_NULL 0u
#define VM_PROT_NONE 0
#define VM_PROT_READ 1
#define VM_PROT_WRITE 2
#define VM_PROT_EXECUTE 4
#define VM_PROT_ALL 7
static inline mach_port_t mach_task_self(void) { return 1; }
static inline mach_port_t mach_host_self(void) { return 1; }
static inline mach_port_t mach_thread_self(void) { return (mach_port_t)syscall(SYS_gettid); }
static inline kern_return_t mach_port_deallocate(mach_port_t task, mach_port_t port) { (void)task; (void)port; return KERN_SUCCESS; }
static inline mach_port_t pthread_mach_thread_np(pthread_t t) { return pthread_equal(t, pthread_self()) ? mach_thread_self() : 0; }
static inline pthread_t pthread_from_mach_thread_np(mach_port_t p) { (void)p; return 0; }
/* Darwin's pthread_threadid_np takes NULL for the calling thread: the argument may be a pointer or a pthread_t here. */
static inline int darwin_threadid(uintptr_t t, uint64_t *id) {
    if (t && !pthread_equal((pthread_t)t, pthread_self())) return ESRCH;
    *id = (uint64_t)syscall(SYS_gettid); return 0;
}
#define pthread_threadid_np(t, id) darwin_threadid((uintptr_t)(t), (id))
/* Darwin names only the calling thread (one argument); Linux takes the thread too. Both spellings work. */
static inline int darwin_setname_self(const char *name) { char n[16]; snprintf(n, sizeof n, "%s", name); return pthread_setname_np(pthread_self(), n); }
#define DARWIN_SETNAME_PICK(_1, _2, which, ...) which
#define pthread_setname_np(...) DARWIN_SETNAME_PICK(__VA_ARGS__, pthread_setname_np, darwin_setname_self, x)(__VA_ARGS__)
#define weak_import weak   /* __attribute__((weak_import)): Mach-O spelling of a weak reference */

/* Guarded reads (runtime/host_os.c). */
int host_safe_read(uint64_t a, void *out, size_t n);
static inline kern_return_t vm_read_overwrite(mach_port_t task, vm_address_t a, vm_size_t n, vm_address_t out, vm_size_t *got) {
    (void)task; if (!host_safe_read(a, (void *)out, n)) return KERN_FAILURE; if (got) *got = n; return KERN_SUCCESS;
}
static inline kern_return_t mach_vm_read_overwrite(mach_port_t task, mach_vm_address_t a, mach_vm_size_t n, mach_vm_address_t out, mach_vm_size_t *got) {
    (void)task; if (!host_safe_read(a, (void *)(uintptr_t)out, (size_t)n)) return KERN_FAILURE; if (got) *got = n; return KERN_SUCCESS;
}
static inline kern_return_t vm_deallocate(mach_port_t task, vm_address_t a, vm_size_t n) { (void)task; return munmap((void *)a, n) ? KERN_FAILURE : KERN_SUCCESS; }

/* ---- thread_info / task_info ---- */
typedef integer_t *thread_info_t, *task_info_t, *thread_policy_t;
typedef struct { int seconds, microseconds; } time_value_t;
typedef struct { time_value_t user_time, system_time; integer_t cpu_usage, policy, run_state, flags, suspend_count, sleep_time; } thread_basic_info_data_t;
typedef struct { uint64_t pth_user_time, pth_system_time; int32_t pth_cpu_usage, pth_policy, pth_run_state, pth_flags, pth_sleep_time, pth_curpri, pth_priority, pth_maxpriority; char pth_name[64]; } thread_extended_info_data_t;
typedef struct { uint64_t thread_id, thread_handle, dispatch_qaddr; } thread_identifier_info_data_t;
typedef struct { uint64_t virtual_size; integer_t region_count, page_size; uint64_t resident_size, resident_size_peak, device, device_peak, internal, internal_peak, external, external_peak, reusable, reusable_peak, purgeable_volatile_pmap, purgeable_volatile_resident, purgeable_volatile_virtual, compressed, compressed_peak, compressed_lifetime, phys_footprint; } task_vm_info_data_t;
#define THREAD_BASIC_INFO 3
#define THREAD_IDENTIFIER_INFO 4
#define THREAD_EXTENDED_INFO 5
#define THREAD_BASIC_INFO_COUNT ((mach_msg_type_number_t)(sizeof(thread_basic_info_data_t) / sizeof(natural_t)))
#define THREAD_IDENTIFIER_INFO_COUNT ((mach_msg_type_number_t)(sizeof(thread_identifier_info_data_t) / sizeof(natural_t)))
#define THREAD_EXTENDED_INFO_COUNT ((mach_msg_type_number_t)(sizeof(thread_extended_info_data_t) / sizeof(natural_t)))
#define TASK_VM_INFO 22
#define TASK_VM_INFO_COUNT ((mach_msg_type_number_t)(sizeof(task_vm_info_data_t) / sizeof(natural_t)))
/* Only the calling thread can be asked (Linux keeps no per-thread port to another thread). */
static inline kern_return_t thread_info(mach_port_t t, thread_flavor_t flavor, thread_info_t out, mach_msg_type_number_t *count) {
    if (t != mach_thread_self()) return KERN_FAILURE;
    if (flavor == THREAD_BASIC_INFO) {
        struct rusage u; if (getrusage(RUSAGE_THREAD, &u)) return KERN_FAILURE;
        thread_basic_info_data_t *b = (thread_basic_info_data_t *)(void *)out; memset(b, 0, sizeof *b);
        b->user_time.seconds = (int)u.ru_utime.tv_sec; b->user_time.microseconds = (int)u.ru_utime.tv_usec;
        b->system_time.seconds = (int)u.ru_stime.tv_sec; b->system_time.microseconds = (int)u.ru_stime.tv_usec;
        if (count) *count = THREAD_BASIC_INFO_COUNT; return KERN_SUCCESS;
    }
    if (flavor == THREAD_IDENTIFIER_INFO) {
        thread_identifier_info_data_t *i = (thread_identifier_info_data_t *)(void *)out; memset(i, 0, sizeof *i);
        i->thread_id = t; if (count) *count = THREAD_IDENTIFIER_INFO_COUNT; return KERN_SUCCESS;
    }
    return KERN_FAILURE;
}
/* Footprint = resident set (/proc/self/statm), what Android's low-memory killer weighs as well. */
static inline kern_return_t task_info(mach_port_t task, task_flavor_t flavor, task_info_t out, mach_msg_type_number_t *count) {
    (void)task; if (flavor != TASK_VM_INFO) return KERN_FAILURE;
    int fd = open("/proc/self/statm", O_RDONLY | O_CLOEXEC); if (fd < 0) return KERN_FAILURE;
    char b[128]; ssize_t n = read(fd, b, sizeof b - 1); close(fd); if (n <= 0) return KERN_FAILURE; b[n] = 0;
    unsigned long long size = 0, rss = 0; if (sscanf(b, "%llu %llu", &size, &rss) != 2) return KERN_FAILURE;
    task_vm_info_data_t *v = (task_vm_info_data_t *)(void *)out; memset(v, 0, sizeof *v);
    uint64_t page = (uint64_t)sysconf(_SC_PAGESIZE);
    v->virtual_size = size * page; v->resident_size = v->phys_footprint = rss * page; v->page_size = (integer_t)page;
    if (count) *count = TASK_VM_INFO_COUNT; return KERN_SUCCESS;
}
static inline kern_return_t task_threads(mach_port_t task, thread_act_array_t *list, mach_msg_type_number_t *n) { (void)task; *list = NULL; *n = 0; return KERN_FAILURE; }

/* ---- thread policies: not available (the Android frontend paces presentation itself) ---- */
typedef struct { uint32_t period, computation, constraint; boolean_t preemptible; } thread_time_constraint_policy_data_t;
#define THREAD_TIME_CONSTRAINT_POLICY 2
#define THREAD_TIME_CONSTRAINT_POLICY_COUNT ((mach_msg_type_number_t)(sizeof(thread_time_constraint_policy_data_t) / sizeof(integer_t)))
static inline kern_return_t thread_policy_set(mach_port_t t, int flavor, thread_policy_t p, mach_msg_type_number_t n) { (void)t; (void)flavor; (void)p; (void)n; return KERN_FAILURE; }
static inline kern_return_t thread_policy_get(mach_port_t t, int flavor, thread_policy_t p, mach_msg_type_number_t *n, boolean_t *def) { (void)t; (void)flavor; (void)p; (void)n; if (def) *def = 1; return KERN_FAILURE; }
/* Per-thread {instructions, cycles} (a private libSystem call on Darwin): zeros. */
static inline int thread_selfcounts(int type, void *buf, size_t n) { (void)type; memset(buf, 0, n); return 0; }

/* ---- QoS classes: nice values ---- */
typedef unsigned int qos_class_t;
#define QOS_CLASS_USER_INTERACTIVE 0x21u
#define QOS_CLASS_USER_INITIATED 0x19u
#define QOS_CLASS_DEFAULT 0x15u
#define QOS_CLASS_UTILITY 0x11u
#define QOS_CLASS_BACKGROUND 0x09u
#define QOS_CLASS_UNSPECIFIED 0x00u
static inline int qos_nice(qos_class_t q) { return q >= QOS_CLASS_USER_INTERACTIVE ? -8 : q >= QOS_CLASS_USER_INITIATED ? -4 : q >= QOS_CLASS_DEFAULT || !q ? 0 : q >= QOS_CLASS_UTILITY ? 5 : 10; }
static inline int pthread_set_qos_class_self_np(qos_class_t q, int rel) {
    (void)rel; int saved = errno; setpriority(PRIO_PROCESS, (id_t)syscall(SYS_gettid), qos_nice(q)); errno = saved; return 0;
}
static inline int pthread_get_qos_class_np(pthread_t t, qos_class_t *q, int *rel) { (void)t; if (q) *q = QOS_CLASS_UNSPECIFIED; if (rel) *rel = 0; return 0; }
static inline int pthread_attr_set_qos_class_np(pthread_attr_t *a, qos_class_t q, int rel) { (void)a; (void)q; (void)rel; return 0; }

/* ---- caches ---- */
static inline void sys_icache_invalidate(void *p, size_t n) { __builtin___clear_cache((char *)p, (char *)p + n); }

/* ---- malloc ---- */
static inline size_t malloc_size(const void *p) { return malloc_usable_size((void *)p); }

/* ---- linker tables: getsectiondata for the sections the graphics layer registers (TOTK_SECTION) ---- */
struct mach_header_64 { int unused; };
extern const struct mach_header_64 _mh_execute_header __attribute__((weak, visibility("hidden")));
extern const char __start_swnvn[] __attribute__((weak, visibility("hidden"))), __stop_swnvn[] __attribute__((weak, visibility("hidden")));
extern const char __start_totknvnx[] __attribute__((weak, visibility("hidden"))), __stop_totknvnx[] __attribute__((weak, visibility("hidden")));
extern const char __start_totkgfx[] __attribute__((weak, visibility("hidden"))), __stop_totkgfx[] __attribute__((weak, visibility("hidden")));
static inline uint8_t *getsectiondata(const struct mach_header_64 *mh, const char *seg, const char *sect, unsigned long *size) {
    (void)mh; (void)seg; const char *b = NULL, *e = NULL;
    if (!strcmp(sect, "swnvn")) { b = __start_swnvn; e = __stop_swnvn; }
    else if (!strcmp(sect, "totknvnx")) { b = __start_totknvnx; e = __stop_totknvnx; }
    else if (!strcmp(sect, "totkgfx")) { b = __start_totkgfx; e = __stop_totkgfx; }
    *size = b ? (unsigned long)(e - b) : 0; return (uint8_t *)b;
}
#endif
