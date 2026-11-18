/* macOS implementations of the glibc functions the runtime uses (host_compat.h). */
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>

#include "host_compat.h"
#undef mmap

int memfd_create(const char* name, unsigned flags)
{
    (void)flags;
    static atomic_uint serial;
    char shm[64];
    snprintf(shm, sizeof shm, "/gr-%d-%u", (int)getpid(), atomic_fetch_add(&serial, 1));
    int fd = shm_open(shm, O_RDWR | O_CREAT | O_EXCL, 0600);
    if (fd >= 0) shm_unlink(shm);
    (void)name;
    return fd;
}

int fallocate(int fd, int mode, off_t offset, off_t len)
{
    (void)fd; (void)offset; (void)len;
    return (mode & FALLOC_FL_PUNCH_HOLE) ? 0 : (errno = EOPNOTSUPP, -1);
}

/* macOS ignores mmap address hints in the low range the runtime uses, so MAP_FIXED_NOREPLACE
 * checks with mach_vm_region (the first mapping at or after addr) that nothing overlaps the
 * range and then maps it MAP_FIXED. Callers serialise their placement (the runtime's lock). */
void* compat_mmap(void* addr, size_t len, int prot, int flags, int fd, off_t off)
{
    if (!(flags & MAP_FIXED_NOREPLACE)) return mmap(addr, len, prot, flags, fd, off);
    mach_vm_address_t region = (mach_vm_address_t)(uintptr_t)addr;
    mach_vm_size_t region_size = 0;
    vm_region_basic_info_data_64_t info;
    mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
    mach_port_t object = MACH_PORT_NULL;
    kern_return_t kr = mach_vm_region(mach_task_self(), &region, &region_size, VM_REGION_BASIC_INFO_64,
                                      (vm_region_info_t)&info, &count, &object);
    if (kr == KERN_SUCCESS && region < (mach_vm_address_t)(uintptr_t)addr + len) {
        errno = EEXIST;
        return MAP_FAILED;
    }
    return mmap(addr, len, prot, (flags & ~MAP_FIXED_NOREPLACE) | MAP_FIXED, fd, off);
}

static int before(const struct timespec* deadline)
{
    struct timespec now;
    clock_gettime(CLOCK_REALTIME, &now);
    return now.tv_sec < deadline->tv_sec || (now.tv_sec == deadline->tv_sec && now.tv_nsec < deadline->tv_nsec);
}

static const struct timespec poll_step = {0, 100000};

int pthread_mutex_timedlock(pthread_mutex_t* m, const struct timespec* abstime)
{
    int e;
    while ((e = pthread_mutex_trylock(m)) == EBUSY) {
        if (!before(abstime)) return ETIMEDOUT;
        nanosleep(&poll_step, NULL);
    }
    return e;
}

int pthread_rwlock_timedrdlock(pthread_rwlock_t* l, const struct timespec* abstime)
{
    int e;
    while ((e = pthread_rwlock_tryrdlock(l)) == EBUSY) {
        if (!before(abstime)) return ETIMEDOUT;
        nanosleep(&poll_step, NULL);
    }
    return e;
}

int pthread_rwlock_timedwrlock(pthread_rwlock_t* l, const struct timespec* abstime)
{
    int e;
    while ((e = pthread_rwlock_trywrlock(l)) == EBUSY) {
        if (!before(abstime)) return ETIMEDOUT;
        nanosleep(&poll_step, NULL);
    }
    return e;
}

int clock_nanosleep(clockid_t clock, int flags, const struct timespec* request, struct timespec* remain)
{
    struct timespec t = *request;
    if (flags & TIMER_ABSTIME) {
        struct timespec now;
        clock_gettime(clock, &now);
        int64_t ns = (int64_t)(request->tv_sec - now.tv_sec) * 1000000000 + (request->tv_nsec - now.tv_nsec);
        if (ns <= 0) return 0;
        t.tv_sec = (time_t)(ns / 1000000000);
        t.tv_nsec = (long)(ns % 1000000000);
    }
    return nanosleep(&t, remain) ? errno : 0;
}

int pthread_condattr_setclock(pthread_condattr_t* attr, clockid_t clock)
{
    (void)attr;
    return clock == CLOCK_REALTIME ? 0 : EINVAL;
}

void compat_sleep_ns(uint64_t ns)
{
    struct timespec t = {(time_t)(ns / 1000000000), (long)(ns % 1000000000)};
    while (nanosleep(&t, &t) && errno == EINTR) {}
}
