// SPDX-License-Identifier: GPL-2.0-or-later
// gow3: force-included into the GPU library on macOS (ARM64 development host, switch2/host).
#pragma once
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <sys/resource.h>
#include <sys/types.h>
#include <time.h>

// Linux thread ids: the 64-bit Mach thread id, truncated (only compared for equality).
static inline pid_t gettid(void) {
    uint64_t tid = 0;
    pthread_threadid_np(NULL, &tid);
    return (pid_t)tid;
}
// Per-thread CPU time is not available: the statistics that use it stay at zero.
#define RUSAGE_THREAD RUSAGE_SELF
static inline int pthread_getcpuclockid(pthread_t thread, clockid_t* clock) {
    (void)thread;
    (void)clock;
    return ENOTSUP;
}
#if defined(__aarch64__)
// The ARM generic timer, as the runtime and Box64 use for the guest's RDTSC.
static inline unsigned long long __rdtsc(void) {
    unsigned long long v;
    __asm__ volatile("isb\n mrs %0, cntvct_el0" : "=r"(v));
    return v;
}
#endif
