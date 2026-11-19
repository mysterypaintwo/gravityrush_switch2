/* Horizon pieces of the runtime's compatibility layer (host_compat.h). */
#include <stdint.h>
#include <switch.h>

uint64_t compat_thread_id(void)
{
    u64 id = 0;
    svcGetThreadId(&id, CUR_THREAD_HANDLE);
    return id;
}

void gr_sleep_ns(uint64_t ns); /* gr_suspend_horizon.c: sleeps in slices with quit points */
void compat_sleep_ns(uint64_t ns)
{
    gr_sleep_ns(ns);
}

/* --- libc functions newlib on Horizon declares but does not provide */
#include <dirent.h>
#include <errno.h>
#include <ftw.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>

/* pread/pwrite: seek, transfer, seek back, under one lock (the runtime's files are not shared
 * with other processes, and the lock keeps two threads from moving the same offset). */
static pthread_mutex_t positioned_io = PTHREAD_MUTEX_INITIALIZER;
static ssize_t write_all(int fd, const void* buf, size_t count) { return write(fd, buf, count); }
static ssize_t positioned(int fd, void* buf, size_t count, off_t offset, int writing)
{
    pthread_mutex_lock(&positioned_io);
    const off_t was = lseek(fd, 0, SEEK_CUR);
    ssize_t n = -1;
    if (was >= 0 && lseek(fd, offset, SEEK_SET) == offset) {
        n = writing ? write_all(fd, buf, count) : read(fd, buf, count);
        lseek(fd, was, SEEK_SET);
    }
    pthread_mutex_unlock(&positioned_io);
    return n;
}
ssize_t pread(int fd, void* buf, size_t count, off_t offset) { return positioned(fd, buf, count, offset, 0); }
ssize_t pwrite(int fd, const void* buf, size_t count, off_t offset) { return positioned(fd, (void*)buf, count, offset, 1); }

/* nftw: depth-first walk (the save data code only deletes trees with FTW_DEPTH | FTW_PHYS). */
static int walk(const char* path, int (*fn)(const char*, const struct stat*, int, struct FTW*), int flags, int level)
{
    struct stat st;
    if (stat(path, &st)) return -1;
    struct FTW ftw = {.base = (int)(strrchr(path, '/') ? strrchr(path, '/') - path + 1 : 0), .level = level};
    if (!S_ISDIR(st.st_mode)) return fn(path, &st, FTW_F, &ftw);
    if (!(flags & FTW_DEPTH)) {
        const int r = fn(path, &st, FTW_D, &ftw);
        if (r) return r;
    }
    DIR* d = opendir(path);
    if (d) {
        struct dirent* e;
        while ((e = readdir(d))) {
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
            char child[1024];
            snprintf(child, sizeof child, "%s/%s", path, e->d_name);
            const int r = walk(child, fn, flags, level + 1);
            if (r) { closedir(d); return r; }
        }
        closedir(d);
    }
    return (flags & FTW_DEPTH) ? fn(path, &st, FTW_DP, &ftw) : 0;
}
int nftw(const char* path, int (*fn)(const char*, const struct stat*, int, struct FTW*), int fds, int flags)
{
    (void)fds;
    return walk(path, fn, flags, 0);
}

/* No per-process CPU accounting: the guest's getrusage reads zero. */
int getrusage(int who, struct rusage* usage)
{
    (void)who;
    memset(usage, 0, sizeof *usage);
    return 0;
}
