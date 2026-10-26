/* Host thread services threads.c and the audio threads use: Darwin (Mach ports, QoS classes) and Linux / Android
   (thread ids, nice values). */
#pragma once
#include <pthread.h>
#include <stdint.h>
#include <time.h>

#if defined(__APPLE__)
#include <mach/mach.h>
#include <pthread/qos.h>
typedef mach_port_t host_thread_port_t;
static inline host_thread_port_t host_thread_port_self(void) { return pthread_mach_thread_np(pthread_self()); }
static inline host_thread_port_t host_thread_port_of(pthread_t t) { return pthread_mach_thread_np(t); }
/* Game threads: the highest QoS class, as on the Mac. */
static inline void host_thread_boost_self(void) { pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0); }
static inline uint32_t host_thread_qos_self(void) {
    qos_class_t qc = QOS_CLASS_UNSPECIFIED; int rp = 0;
    pthread_get_qos_class_np(pthread_self(), &qc, &rp); return (uint32_t)qc;
}
static inline uint64_t host_thread_id(pthread_t t, host_thread_port_t port) { (void)port; uint64_t id = 0; pthread_threadid_np(t, &id); return id; }
/* The calling thread's own (host pthread) stack: top address and size. */
static inline void host_thread_stack(uintptr_t *top, size_t *size) {
    *top = (uintptr_t)pthread_get_stackaddr_np(pthread_self()); *size = pthread_get_stacksize_np(pthread_self());
}
/* pc / lr / fp of another thread (diagnostics): 1 when read. */
static inline int host_thread_sample(host_thread_port_t port, uint64_t *pc, uint64_t *lr, uint64_t *fp) {
    if (thread_suspend(port) != KERN_SUCCESS) return 0;
    arm_thread_state64_t st; mach_msg_type_number_t cnt = ARM_THREAD_STATE64_COUNT;
    kern_return_t kr = thread_get_state(port, ARM_THREAD_STATE64, (thread_state_t)&st, &cnt);
    thread_resume(port);
    if (kr != KERN_SUCCESS) return 0;
    *pc = arm_thread_state64_get_pc(st); *lr = arm_thread_state64_get_lr(st); *fp = arm_thread_state64_get_fp(st);
    return 1;
}

#else /* Linux / Android */
#include <errno.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>
#ifndef QOS_CLASS_UNSPECIFIED
#define QOS_CLASS_UNSPECIFIED 0u
#endif
typedef pid_t host_thread_port_t;   /* the kernel thread id */
static inline host_thread_port_t host_thread_port_self(void) { return (pid_t)syscall(SYS_gettid); }
/* Another thread's id is only known to itself; threads.c records it when the thread starts. */
static inline host_thread_port_t host_thread_port_of(pthread_t t) { return pthread_equal(t, pthread_self()) ? host_thread_port_self() : 0; }
/* Game threads: Android's "urgent display" niceness (-8, what its render threads use). An app may lower its own nice
   value this far; if the kernel refuses, the thread keeps the default. */
static inline void host_thread_boost_self(void) { int saved = errno; setpriority(PRIO_PROCESS, (id_t)host_thread_port_self(), -8); errno = saved; }
static inline uint32_t host_thread_qos_self(void) { return (uint32_t)getpriority(PRIO_PROCESS, (id_t)host_thread_port_self()); }
static inline uint64_t host_thread_id(pthread_t t, host_thread_port_t port) { (void)t; return (uint64_t)port; }
static inline void host_thread_stack(uintptr_t *top, size_t *size) {
    pthread_attr_t a; void *lo = NULL; size_t n = 0;
    if (!pthread_getattr_np(pthread_self(), &a)) { pthread_attr_getstack(&a, &lo, &n); pthread_attr_destroy(&a); }
    *top = (uintptr_t)lo + n; *size = n;
}
/* Not available without ptrace (diagnostics print "?" instead). */
static inline int host_thread_sample(host_thread_port_t port, uint64_t *pc, uint64_t *lr, uint64_t *fp) { (void)port; (void)pc; (void)lr; (void)fp; return 0; }
/* Darwin's relative timed wait, on the condition variable's default clock (CLOCK_REALTIME). */
static inline int pthread_cond_timedwait_relative_np(pthread_cond_t *c, pthread_mutex_t *m, const struct timespec *rel) {
    struct timespec dl; clock_gettime(CLOCK_REALTIME, &dl);
    dl.tv_sec += rel->tv_sec; dl.tv_nsec += rel->tv_nsec;
    if (dl.tv_nsec >= 1000000000) { dl.tv_sec++; dl.tv_nsec -= 1000000000; }
    return pthread_cond_timedwait(c, m, &dl);
}
#endif
