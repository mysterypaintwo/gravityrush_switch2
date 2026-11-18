/* Split JIT memory on macOS: one shared anonymous region mapped twice, read-write for Box64 and
 * read-execute for the CPU. This is the same model as kaiHEN's JIT on Horizon (staging buffer
 * plus executable alias), so the Mac exercises the console's code path; only publishing differs
 * (a cache flush here, an IPC copy there). */
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <libkern/OSCacheControl.h>
#include <pthread.h>
#include <stdio.h>
#include <sys/mman.h>

#include "box64e.h"

static uintptr_t jit_exec, jit_next, jit_end;
static pthread_mutex_t jit_lock = PTHREAD_MUTEX_INITIALIZER;

int box64e_jit_create(size_t size, box64e_jit* out)
{
    void* rw = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_ANON | MAP_SHARED, -1, 0);
    if (rw == MAP_FAILED) return -1;
    mach_vm_address_t rx = 0;
    vm_prot_t cur, max;
    kern_return_t kr = mach_vm_remap(mach_task_self(), &rx, size, 0, VM_FLAGS_ANYWHERE, mach_task_self(),
                                     (mach_vm_address_t)(uintptr_t)rw, FALSE, &cur, &max, VM_INHERIT_NONE);
    if (kr != KERN_SUCCESS || mprotect((void*)(uintptr_t)rx, size, PROT_READ | PROT_EXEC)) {
        fprintf(stderr, "box64e: JIT alias failed (%d)\n", kr);
        munmap(rw, size);
        return -1;
    }
    jit_exec = jit_next = (uintptr_t)rx;
    jit_end = jit_exec + size;
    out->exec_base = jit_exec;
    out->wdelta = (intptr_t)((uintptr_t)rw - (uintptr_t)rx);
    out->size = size;
    return 0;
}

void* JitAllocChunk(size_t size)
{
    pthread_mutex_lock(&jit_lock);
    uintptr_t p = jit_next;
    size = (size + 0xffff) & ~(size_t)0xffff;
    if (!jit_exec || p + size > jit_end) {
        pthread_mutex_unlock(&jit_lock);
        fprintf(stderr, "box64e: JIT memory exhausted\n");
        return NULL;
    }
    jit_next = p + size;
    pthread_mutex_unlock(&jit_lock);
    return (void*)p;
}

void JitPublish(void* exec_addr, size_t size)
{
    sys_icache_invalidate(exec_addr, size);
}
