// SPDX-License-Identifier: GPL-2.0-or-later
// gow3: force-included into the GPU library on the Switch 2 (newlib on libounce, switch2/host).
#pragma once
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <sys/resource.h>
#include <sys/types.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif
uint64_t compat_thread_id(void); // switch2/host/host_compat_horizon.c: the kernel thread id
#ifdef __cplusplus
}
#endif

// Linux thread ids: the kernel's thread id, truncated (only compared for equality).
static inline pid_t gettid(void) { return (pid_t)compat_thread_id(); }
// Per-thread CPU time is not available: the statistics that use it stay at zero.
#ifndef RUSAGE_THREAD
#define RUSAGE_THREAD RUSAGE_SELF
#endif
// No signals: recovery points are plain jumps, as in the runtime (switch2/host/host_compat.h).
#include <setjmp.h>
typedef jmp_buf sigjmp_buf;
#define sigsetjmp(buffer, save) setjmp(buffer)
#define siglongjmp(buffer, value) longjmp(buffer, value)
// The ARM generic timer's physical count, as the runtime and Box64 use for the guest's RDTSC:
// EL0 may not read the virtual count (cntvct_el0) on the Switch 2; doing so ends the process.
static inline unsigned long long __rdtsc(void) {
    unsigned long long v;
    __asm__ volatile("isb\n mrs %0, cntpct_el0" : "=r"(v));
    return v;
}
