/* Switch 2 bring-up: who holds the host heap. Every allocation of TRACK_MIN bytes or more is
 * recorded with its call site (3 return addresses); gr_heap_report() prints the sites holding
 * the most, plus the totals, to boot.txt. The heap is fixed (gr_switch.c: about 3.2 GiB) and
 * OunceVK takes GPU memory from it too, so a full heap ends the game with ErrorOutOfDeviceMemory.
 *
 * Linked with --wrap for malloc, calloc, realloc, free, aligned_alloc, memalign and
 * posix_memalign (switch2/host/CMakeLists.txt). Addresses are offsets from gr_quit_point, as in
 * the other crumbs. */
#include <switch.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

void* __real_malloc(size_t size);
void* __real_calloc(size_t n, size_t size);
void* __real_realloc(void* p, size_t size);
void __real_free(void* p);
void* __real_aligned_alloc(size_t align, size_t size);
void* __real_memalign(size_t align, size_t size);
int __real_posix_memalign(void** out, size_t align, size_t size);
void gr_crumb(const char* what);
void gr_quit_point(void);
size_t box64e_jit_used(size_t* size); /* switch2/box64/box64e_jit_horizon.c */
void gow3gpu_memory_report(void (*line)(const char*)) __attribute__((weak)); /* gpu/shim/gow3gpu.cpp */

#define TRACK_MIN ((size_t)256 << 10)
#define STRIPES 64
#define PER_STRIPE 1024 /* 65536 large allocations live at most */
#define SITES 1024
#define FRAMES 3

typedef struct { uintptr_t ptr; size_t size; int site; } Entry;
typedef struct { atomic_flag lock; int used; Entry e[PER_STRIPE]; } Stripe;
typedef struct { uintptr_t at[FRAMES]; size_t live, peak; unsigned count, total; } Site;

static Stripe stripes[STRIPES];
static Site sites[SITES];
static int site_count;
static atomic_flag site_lock = ATOMIC_FLAG_INIT;
static _Atomic size_t live_large, peak_large;
static _Atomic unsigned failures, untracked;
static _Thread_local int inside;

static void spin(atomic_flag* f) { while (atomic_flag_test_and_set_explicit(f, memory_order_acquire)) {} }
static void unspin(atomic_flag* f) { atomic_flag_clear_explicit(f, memory_order_release); }
static unsigned stripe_of(uintptr_t p) { return (unsigned)((p >> 12) * 0x9E3779B1u >> 26) % STRIPES; }

static __attribute__((noinline)) void caller_frames(uintptr_t at[FRAMES])
{
    memset(at, 0, sizeof(uintptr_t) * FRAMES);
    Thread* self = threadGetSelf();
    const uintptr_t lo = self && self->stack_mirror ? (uintptr_t)self->stack_mirror : 0;
    const uintptr_t hi = lo ? lo + self->stack_sz : 0;
    uintptr_t fp = (uintptr_t)__builtin_frame_address(0);
    /* Skip this function and the wrapper: frame 2 on is the code that allocated. */
    for (int f = 0, kept = 0; f < FRAMES + 2 && lo && fp >= lo && fp + 16 <= hi && !(fp & 15); ++f) {
        if (f >= 2) at[kept++] = ((const uintptr_t*)fp)[1];
        const uintptr_t next = ((const uintptr_t*)fp)[0];
        if (next <= fp) break;
        fp = next;
    }
}

static int site_index(const uintptr_t at[FRAMES])
{
    spin(&site_lock);
    int i = 0;
    for (; i < site_count; ++i)
        if (!memcmp(sites[i].at, at, sizeof sites[i].at)) break;
    if (i == site_count) {
        if (site_count == SITES) i = SITES - 1; /* full: the last site collects the rest */
        else memcpy(sites[site_count++].at, at, sizeof sites[i].at);
    }
    unspin(&site_lock);
    return i;
}

static void record(void* p, size_t size)
{
    if (!p || size < TRACK_MIN || inside) return;
    inside = 1;
    uintptr_t at[FRAMES];
    caller_frames(at);
    const int site = site_index(at);
    spin(&site_lock);
    Site* s = &sites[site];
    s->live += size;
    if (s->live > s->peak) s->peak = s->live;
    ++s->count;
    ++s->total;
    unspin(&site_lock);
    const size_t now = atomic_fetch_add(&live_large, size) + size;
    size_t peak = atomic_load(&peak_large);
    while (now > peak && !atomic_compare_exchange_weak(&peak_large, &peak, now)) {}
    Stripe* st = &stripes[stripe_of((uintptr_t)p)];
    spin(&st->lock);
    if (st->used < PER_STRIPE) st->e[st->used++] = (Entry){(uintptr_t)p, size, site};
    else atomic_fetch_add(&untracked, 1);
    unspin(&st->lock);
    inside = 0;
}

static void forget(void* p)
{
    if (!p) return;
    Stripe* st = &stripes[stripe_of((uintptr_t)p)];
    spin(&st->lock);
    for (int i = 0; i < st->used; ++i)
        if (st->e[i].ptr == (uintptr_t)p) {
            const Entry e = st->e[i];
            st->e[i] = st->e[--st->used];
            unspin(&st->lock);
            atomic_fetch_sub(&live_large, e.size);
            spin(&site_lock);
            sites[e.site].live -= e.size;
            --sites[e.site].count;
            unspin(&site_lock);
            return;
        }
    unspin(&st->lock);
}

void gr_heap_report(const char* why);
static void failed(size_t size)
{
    if (inside || size < TRACK_MIN) return;
    if (atomic_fetch_add(&failures, 1) == 0) {
        char line[96];
        snprintf(line, sizeof line, "heap: an allocation of %zu KiB failed", size >> 10);
        gr_crumb(line);
        gr_heap_report("first failed allocation");
    }
}

void* __wrap_malloc(size_t size)
{
    void* p = __real_malloc(size);
    if (p) record(p, size); else failed(size);
    return p;
}
void* __wrap_calloc(size_t n, size_t size)
{
    void* p = __real_calloc(n, size);
    if (p) record(p, n * size); else failed(n * size);
    return p;
}
void* __wrap_realloc(void* old, size_t size)
{
    forget(old); /* a failed realloc keeps the block, untracked */
    void* p = __real_realloc(old, size);
    if (p) record(p, size); else if (size) failed(size);
    return p;
}
void __wrap_free(void* p)
{
    forget(p);
    __real_free(p);
}
void* __wrap_aligned_alloc(size_t align, size_t size)
{
    void* p = __real_aligned_alloc(align, size);
    if (p) record(p, size); else failed(size);
    return p;
}
void* __wrap_memalign(size_t align, size_t size)
{
    void* p = __real_memalign(align, size);
    if (p) record(p, size); else failed(size);
    return p;
}
int __wrap_posix_memalign(void** out, size_t align, size_t size)
{
    const int r = __real_posix_memalign(out, align, size);
    if (!r) record(*out, size); else failed(size);
    return r;
}

/* The 24 sites holding the most, then the totals. */
void gr_heap_report(const char* why)
{
    if (inside) return;
    inside = 1;
    enum { TOP = 24 };
    static Site copy[SITES];
    static Site top[TOP];
    spin(&site_lock);
    const int count = site_count;
    memcpy(copy, sites, sizeof(Site) * (size_t)count);
    unspin(&site_lock);
    int n = 0;
    for (; n < TOP; ++n) {
        int best = -1;
        for (int i = 0; i < count; ++i)
            if (copy[i].live && (best < 0 || copy[i].live > copy[best].live)) best = i;
        if (best < 0) break;
        top[n] = copy[best];
        copy[best].live = 0;
    }
    u64 used = 0, total = 0;
    svcGetInfo(&used, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0);
    svcGetInfo(&total, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
    char line[200];
    snprintf(line, sizeof line, "heap (%s): %zu MiB in allocations of %zu KiB or more (peak %zu MiB), %d sites; process %llu of %llu MiB; %u failed, %u untracked",
             why, atomic_load(&live_large) >> 20, TRACK_MIN >> 10, atomic_load(&peak_large) >> 20, count,
             (unsigned long long)(used >> 20), (unsigned long long)(total >> 20), atomic_load(&failures), atomic_load(&untracked));
    gr_crumb(line);
    size_t jit_size = 0;
    const size_t jit_used = box64e_jit_used(&jit_size);
    snprintf(line, sizeof line, "heap: JIT %zu of %zu MiB used", jit_used >> 20, jit_size >> 20);
    gr_crumb(line);
    if (gow3gpu_memory_report) gow3gpu_memory_report(gr_crumb);
    for (int i = 0; i < n; ++i) {
        int k = snprintf(line, sizeof line, "heap %6zu MiB in %4u (peak %zu MiB, %u made) at", top[i].live >> 20, top[i].count,
                         top[i].peak >> 20, top[i].total);
        for (int f = 0; f < FRAMES && top[i].at[f]; ++f)
            k += snprintf(line + k, sizeof line - (size_t)k, " %+lld", (long long)((intptr_t)top[i].at[f] - (intptr_t)gr_quit_point));
        gr_crumb(line);
    }
    inside = 0;
}
