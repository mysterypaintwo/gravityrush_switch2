/* Split JIT memory on Horizon (Switch 2 under kaiHEN): libounce's Jit gives a writable staging
 * view (rw_addr) and the executable mapping (rx_addr). Box64 writes translated code at
 * exec + wdelta, which is the staging view, and each finished block is published to the
 * executable mapping with one kaihenJitPublishRanges request (console run 1: about 24 us plus
 * 1.6 us per KiB). */
#include <pthread.h>
#include <stdio.h>
#include <switch.h>
#include <kaihen.h>

#include "box64e.h"

static Jit jit;
static uintptr_t jit_exec, jit_next, jit_end;
static pthread_mutex_t jit_lock = PTHREAD_MUTEX_INITIALIZER;

int box64e_jit_create(size_t size, box64e_jit* out)
{
    Result rc = jitCreate(&jit, size);
    if (R_FAILED(rc)) {
        fprintf(stderr, "box64e: jitCreate(%zu MiB) failed: 0x%x\n", size >> 20, rc);
        return -1;
    }
    /* The staging view is the one Box64 writes; the executable mapping is published to. */
    rc = jitTransitionToExecutable(&jit);
    if (R_FAILED(rc)) {
        fprintf(stderr, "box64e: jitTransitionToExecutable failed: 0x%x\n", rc);
        jitClose(&jit);
        return -1;
    }
    jit_exec = jit_next = (uintptr_t)jitGetRxAddr(&jit);
    jit_end = jit_exec + size;
    out->exec_base = jit_exec;
    out->wdelta = (intptr_t)((uintptr_t)jitGetRwAddr(&jit) - jit_exec);
    out->size = size;
    return 0;
}

/* Bring-up: JIT bytes handed out so far, and the size. */
size_t box64e_jit_used(size_t* size)
{
    if (size) *size = (size_t)(jit_end - jit_exec);
    return (size_t)(jit_next - jit_exec);
}

/* Before the process exits (no thread runs translated code any more). */
void box64e_jit_release(void)
{
    if (!jit_exec) return;
    jitClose(&jit);
    jit_exec = jit_next = jit_end = 0;
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

/* Publishes run one at a time: Box64 translates on several threads, and two publishes into the
 * same page at once made one fail with 0xd401 (invalid memory state), which left its block
 * unpublished and the thread that ran it crashed. That was not all: a publish was also refused
 * for 25 ms straight, at the same moment a thread's stack could not be mapped (console boot,
 * 2026-11-19). So a refused publish is retried for about 1 s, then sent the other way kaiHEN
 * offers (the bytes copied through the request: kaihenPublishJitAlias). A failure that persists is
 * written to boot.txt before the crash. */
static pthread_mutex_t publish_lock = PTHREAD_MUTEX_INITIALIZER;
void gr_crumb(const char* what) __attribute__((weak)); /* switch2/host/gr_switch.c, when linked */
void JitPublish(void* exec_addr, size_t size)
{
    const size_t offset = (uintptr_t)exec_addr - jit_exec;
    const KaihenJitRange range = {offset, size};
    pthread_mutex_lock(&publish_lock);
    Result rc = kaihenJitPublishRanges(&jit, &range, 1);
    const Result first = rc;
    int tries = 1;
    for (int retry = 0; R_FAILED(rc) && retry < 12; ++retry, ++tries) {
        svcSleepThread(250000ull << (retry < 6 ? retry : 6)); /* 0.25 ms doubling to 16 ms: ~1 s */
        rc = kaihenJitPublishRanges(&jit, &range, 1);
    }
    static bool alias_ready;
    if (R_FAILED(rc)) {
        if (!alias_ready) {
            KaihenJitAliasInfo info;
            alias_ready = R_SUCCEEDED(kaihenMapJitAlias(&jit, &info));
        }
        if (alias_ready) rc = kaihenPublishJitAlias(&jit, offset, (const char*)jitGetRwAddr(&jit) + offset, size);
    }
    pthread_mutex_unlock(&publish_lock);
    if (R_FAILED(first) && gr_crumb) {
        char line[160];
        snprintf(line, sizeof line, "box64e: JIT publish of %zu bytes at %p refused (0x%x): %s after %d tries%s", size,
                 exec_addr, first, R_SUCCEEDED(rc) ? "published" : "FAILED", tries, alias_ready ? ", alias tried" : "");
        gr_crumb(line);
    }
    if (R_FAILED(rc)) {
        fprintf(stderr, "box64e: JIT publish of %zu bytes at %p failed: 0x%x\n", size, exec_addr, rc);
        return;
    }
    /* kaiHEN invalidates the calling thread's instruction cache; IC IVAU is broadcast in the
     * inner shareable domain, so this also covers the other cores before they jump in. */
    const uintptr_t line = 64;
    for (uintptr_t a = (uintptr_t)exec_addr & ~(line - 1); a < (uintptr_t)exec_addr + size; a += line)
        __asm__ volatile("ic ivau, %0" ::"r"(a) : "memory");
    __asm__ volatile("dsb ish\n isb" ::: "memory");
}
