/* Force-included into the runtime sources (src/runtime*.c) when they are built for the ARM64
 * host on macOS (switch2/host/CMakeLists.txt). The runtime is written for glibc and Windows;
 * this fills in what macOS lacks. Horizon gets its own header later. */
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

#endif
