/* Horizon pieces of the embedded Box64's OS layer: the anonymous-memory mmap behind
 * horizon/sys/mman.h. */
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#define MAP_ALIGN 4096

/* Live mappings, so munmap only frees what mmap returned (Box64 never unmaps part of one). */
typedef struct { void* addr; size_t size; } Mapping;
static Mapping* mappings;
static size_t mapping_count, mapping_cap;
static pthread_mutex_t mapping_lock = PTHREAD_MUTEX_INITIALIZER;

void* mmap(void* addr, size_t length, int prot, int flags, int fd, off_t offset)
{
    (void)prot; (void)offset;
    if (fd != -1 || !(flags & MAP_ANONYMOUS) || (flags & MAP_FIXED) || !length) {
        errno = ENOTSUP;
        return MAP_FAILED;
    }
    (void)addr; /* hints are ignored */
    size_t size = (length + MAP_ALIGN - 1) & ~(size_t)(MAP_ALIGN - 1);
    void* p = aligned_alloc(MAP_ALIGN, size);
    if (!p) {
        errno = ENOMEM;
        return MAP_FAILED;
    }
    memset(p, 0, size);
    pthread_mutex_lock(&mapping_lock);
    if (mapping_count == mapping_cap) {
        mapping_cap = mapping_cap ? mapping_cap * 2 : 256;
        mappings = realloc(mappings, mapping_cap * sizeof *mappings);
    }
    mappings[mapping_count++] = (Mapping){p, size};
    pthread_mutex_unlock(&mapping_lock);
    return p;
}

int munmap(void* addr, size_t length)
{
    (void)length;
    pthread_mutex_lock(&mapping_lock);
    for (size_t i = 0; i < mapping_count; i++)
        if (mappings[i].addr == addr) {
            mappings[i] = mappings[--mapping_count];
            pthread_mutex_unlock(&mapping_lock);
            free(addr);
            return 0;
        }
    pthread_mutex_unlock(&mapping_lock);
    return 0; /* part of a mapping, or not ours: keep it */
}

int mprotect(void* addr, size_t len, int prot) { (void)addr; (void)len; (void)prot; return 0; }
int madvise(void* addr, size_t length, int advice) { (void)addr; (void)length; (void)advice; return 0; }

/* --- no dynamic libraries or backtraces on Horizon (horizon/dlfcn.h, horizon/execinfo.h) */
#include <dlfcn.h>
#include <execinfo.h>
void* dlopen(const char* file, int mode) { (void)file; (void)mode; return NULL; }
void* dlsym(void* handle, const char* name) { (void)handle; (void)name; return NULL; }
int dlclose(void* handle) { (void)handle; return 0; }
char* dlerror(void) { return (char*)"no dynamic libraries on Horizon"; }
int dladdr(const void* addr, Dl_info* info) { (void)addr; (void)info; return 0; }
int backtrace(void** buffer, int size) { (void)buffer; (void)size; return 0; }
char** backtrace_symbols(void* const* buffer, int size) { (void)buffer; (void)size; return NULL; }
void backtrace_symbols_fd(void* const* buffer, int size, int fd) { (void)buffer; (void)size; (void)fd; }

/* newlib on Horizon has no sysconf, signal masks or entropy source of its own. */
#include <signal.h>
#include <unistd.h>
#include <sys/reent.h>
#include <switch.h>
long sysconf(int name)
{
    switch (name) {
    case _SC_PAGESIZE: return 4096;
    case _SC_NPROCESSORS_CONF:
    case _SC_NPROCESSORS_ONLN: return 3; /* the cores an application gets */
    default: errno = EINVAL; return -1;
    }
}
/* No signals on Horizon: the dynarec's masking around block creation has nothing to block. */
int pthread_sigmask(int how, const sigset_t* set, sigset_t* old)
{
    (void)how; (void)set;
    if (old) sigemptyset(old);
    return 0;
}
int _getentropy_r(struct _reent* r, void* buf, size_t len)
{
    (void)r;
    randomGet(buf, len);
    return 0;
}
