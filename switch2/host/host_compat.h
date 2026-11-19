/* Force-included into the runtime sources (src/runtime*.c) when they are built for the ARM64
 * hosts (switch2/host/CMakeLists.txt): macOS (development) and Horizon (the Switch 2). The
 * runtime is written for glibc and Windows; this fills in what those lack. */
#ifndef GR_HOST_COMPAT_H
#define GR_HOST_COMPAT_H

#ifdef __APPLE__
#include <stddef.h>
#include <stdint.h>
#include <time.h>
#include <pthread.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>

/* struct stat: glibc's st_*tim are st_*timespec. */
#define st_atim st_atimespec
#define st_mtim st_mtimespec
#define st_ctim st_ctimespec

#ifndef PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP
#define PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP PTHREAD_RECURSIVE_MUTEX_INITIALIZER
#endif

/* memfd: an unlinked POSIX shared memory object. fallocate only punches holes (a no-op). */
#define MFD_CLOEXEC 1u
int memfd_create(const char* name, unsigned flags);
#define FALLOC_FL_KEEP_SIZE 1
#define FALLOC_FL_PUNCH_HOLE 2
int fallocate(int fd, int mode, off_t offset, off_t len);

/* mmap with MAP_FIXED_NOREPLACE: map at the hint or fail, never replace a mapping. */
#define MAP_FIXED_NOREPLACE 0x40000000
void* compat_mmap(void* addr, size_t len, int prot, int flags, int fd, off_t off);
#define mmap compat_mmap

/* Timed pthread locks (polling, 0.1 ms steps) and absolute monotonic sleeps. */
int pthread_mutex_timedlock(pthread_mutex_t* m, const struct timespec* abstime);
int pthread_rwlock_timedrdlock(pthread_rwlock_t* l, const struct timespec* abstime);
int pthread_rwlock_timedwrlock(pthread_rwlock_t* l, const struct timespec* abstime);
#define TIMER_ABSTIME 1
int clock_nanosleep(clockid_t clock, int flags, const struct timespec* request, struct timespec* remain);
/* Condition variables only have CLOCK_REALTIME: the runtime's waits use it on macOS. */
int pthread_condattr_setclock(pthread_condattr_t* attr, clockid_t clock);
#define CLOCK_REALTIME_COARSE CLOCK_REALTIME

void compat_sleep_ns(uint64_t ns);

/* Per-thread CPU time is not available through getrusage: report the process. */
#include <sys/resource.h>
#define RUSAGE_THREAD RUSAGE_SELF
#include <stdlib.h>
static inline long getrandom(void* buf, size_t len, unsigned flags) { (void)flags; arc4random_buf(buf, len); return (long)len; }
/* macOS names only the calling thread (the runtime always names itself). */
#define pthread_setname_np(thread, name) pthread_setname_np(name)
#endif

#ifdef __SWITCH__
/* Horizon: newlib on libounce (libnx). */
/* newlib declares posix_close and posix_getdents with other types; its declarations are renamed
 * while its headers load (first: other headers pull sys/unistd.h in), so the runtime's guest functions of the same names keep theirs. */
#define posix_close newlib_posix_close
#define posix_getdents newlib_posix_getdents
#include <unistd.h>
#include <dirent.h>
#undef posix_close
#undef posix_getdents
#include <stddef.h>
#include <stdint.h>
#include <time.h>
#include <errno.h>
#include <pthread.h>
#include <setjmp.h>
#include <sys/socket.h>

/* No signals: the runtime's recovery points are plain jumps (nothing delivers a fault). */
typedef jmp_buf sigjmp_buf;
#define sigsetjmp(buffer, save) setjmp(buffer)
#define siglongjmp(buffer, value) longjmp(buffer, value)

#include <sys/resource.h>

#ifndef PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP
#define PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP PTHREAD_RECURSIVE_MUTEX_INITIALIZER
#endif

/* Timed mutex lock: polled in 0.1 ms steps until the absolute CLOCK_REALTIME deadline. */
static inline int pthread_mutex_timedlock(pthread_mutex_t* m, const struct timespec* abstime)
{
    for (;;) {
        const int r = pthread_mutex_trylock(m);
        if (r != EBUSY) return r;
        struct timespec now;
        clock_gettime(CLOCK_REALTIME, &now);
        if (now.tv_sec > abstime->tv_sec || (now.tv_sec == abstime->tv_sec && now.tv_nsec >= abstime->tv_nsec))
            return ETIMEDOUT;
        const struct timespec step = {0, 100000};
        nanosleep(&step, NULL);
    }
}

/* struct tm has no tm_gmtoff: the offset of local time from UTC, in seconds. */
static inline long compat_utc_offset(time_t t)
{
    struct tm local, utc;
    localtime_r(&t, &local);
    gmtime_r(&t, &utc);
    long days = (long)(local.tm_yday - utc.tm_yday);
    if (local.tm_year != utc.tm_year) days = local.tm_year > utc.tm_year ? 1 : -1;
    return ((days * 24 + (local.tm_hour - utc.tm_hour)) * 60 + (local.tm_min - utc.tm_min)) * 60 +
           (local.tm_sec - utc.tm_sec);
}

/* A number for the calling thread (sceKernelGetCurrentThreadId and friends). */
uint64_t compat_thread_id(void); /* switch2/host/host_compat_horizon.c */

#define CLOCK_REALTIME_COARSE CLOCK_REALTIME
/* No per-thread or per-process CPU clocks: the guest's CPU-time queries read the monotonic clock. */
#ifndef CLOCK_THREAD_CPUTIME_ID
#define CLOCK_THREAD_CPUTIME_ID CLOCK_MONOTONIC
#endif
#ifndef CLOCK_PROCESS_CPUTIME_ID
#define CLOCK_PROCESS_CPUTIME_ID CLOCK_MONOTONIC
#endif
#define RUSAGE_THREAD RUSAGE_SELF
/* Thread names are for debugging only; Horizon threads keep theirs. */
#undef pthread_setname_np
#define pthread_setname_np(thread, name) ((void)(thread), (void)(name), 0)
void compat_sleep_ns(uint64_t ns); /* host_compat_horizon.c */
#endif

#endif
