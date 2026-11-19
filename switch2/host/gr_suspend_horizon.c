/* Suspend and resume on the Switch 2 (see gr_suspend_horizon.h).
 *
 * libnx's own focus handling only blocks the thread that runs appletMainLoop; the rest of the
 * process keeps running. To freeze the game as the system freezes an official one, every thread
 * the process creates registers its kernel handle (pthread_create is wrapped: the link uses
 * -Wl,--wrap=pthread_create, so std::thread and the GPU library's threads register too), and on
 * suspend the applet thread pauses all of them with svcSetThreadActivity, the kernel's own
 * mechanism. The sound stops first (listeners), so nothing keeps playing from queued buffers.
 *
 * Suspend: focus state Background (the HOME menu is open, or the console is going to sleep).
 * A library applet the game opens itself (software keyboard, error dialog) is OutOfFocus and
 * does not suspend: the game is waiting for it. Resume: back InFocus (also after waking).
 * The time spent suspended is added to runtime_suspended_ticks (src/runtime_kernel.c), which
 * the guest's monotonic clocks and its TSC (RDTSC through Box64's ReadTSC) leave out.
 *
 * While the threads are frozen the applet thread must not take a lock one of them may hold:
 * no malloc, no stdio. The registry is a lock-free table for that reason. */
#include "gr_suspend_horizon.h"

#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <switch.h>

#include "box64e.h"

extern volatile uint64_t runtime_suspended_ticks;
int runtime_savedata_busy(void); /* src/runtime_savedata.c */

#define MAX_THREADS 512
static _Atomic Handle threads[MAX_THREADS]; /* 0: free slot */
static Thread* _Atomic thread_objects[MAX_THREADS]; /* libnx's Thread of each slot (its stack) */
static void* _Atomic thread_entries[MAX_THREADS]; /* the function each thread runs (for the exit log) */
static _Atomic(pthread_t) thread_ids[MAX_THREADS];  /* its pthread_t, for pthread_detach */
static atomic_bool thread_detached[MAX_THREADS];
static Handle applet_thread;
static atomic_bool frozen;
static uint64_t frozen_at;
static Handle paused[MAX_THREADS];
static int paused_count;

#define MAX_LISTENERS 8
static GrSuspendFn listeners[MAX_LISTENERS];
static int listener_count;

static uint64_t ticks_now(void)
{
    uint64_t v;
    __asm__ volatile("isb\n mrs %0, cntpct_el0" : "=r"(v)); /* Horizon: EL0 may read the physical counter (libnx's armGetSystemTick), not the virtual one */
    return v;
}

static int register_thread(Handle h)
{
    for (int i = 0; i < MAX_THREADS; ++i) {
        Handle expected = 0;
        if (atomic_compare_exchange_strong(&threads[i], &expected, h)) return i;
    }
    return -1; /* table full: this thread will not freeze (logged once by the caller) */
}

static atomic_bool quitting; /* the game is exiting: every thread ends at its next quit point */
void gr_heap_report(const char* why); /* gr_heap_horizon.c */
void gr_crumb(const char* what); /* gr_switch.c: boot.txt */
void gr_quit_point(void);
/* Console bring-up: thread starts and ends in boot.txt, which survives a crash. The function is
 * given as an offset from gr_quit_point (add its ELF address to look it up). */
static void thread_crumb(const char* what, void* fn)
{
    char line[96];
    snprintf(line, sizeof line, "thread %s: gr_quit_point%+lld", what, (long long)((intptr_t)fn - (intptr_t)gr_quit_point));
    gr_crumb(line);
}

/* --- threads that ended. Nothing of a thread may be left when the process returns to the
 * homebrew loader (see "exiting" below), so every thread that ends is listed here until its
 * libnx Thread is closed (stack mirror unmapped, stack freed, kernel handle closed):
 * - joined threads: pthread_join closes it, and the entry is dropped;
 * - detached threads: the applet thread closes them as they end (reap_finished), since
 *   newlib on libounce has no pthread_detach (it fails with ENOSYS, which std::thread::detach
 *   turns into an exception);
 * - the rest (threads that ended while the game exits): end_process closes them. */
typedef struct { Thread* thread; Handle handle; pthread_t id; bool detached; } Finished;
#define MAX_FINISHED 4096
static Finished finished[MAX_FINISHED];
static int finished_count;
static atomic_flag finished_lock = ATOMIC_FLAG_INIT; /* a spin lock: the applet thread only tries it */
static void finished_acquire(void) { while (atomic_flag_test_and_set_explicit(&finished_lock, memory_order_acquire)) svcSleepThread(10000); }
static bool finished_try(void) { return !atomic_flag_test_and_set_explicit(&finished_lock, memory_order_acquire); }
static void finished_release(void) { atomic_flag_clear_explicit(&finished_lock, memory_order_release); }

static void add_finished(bool detached)
{
    Thread* t = threadGetSelf();
    if (!t) return;
    finished_acquire();
    if (finished_count < MAX_FINISHED)
        finished[finished_count++] = (Finished){t, t->handle, pthread_self(), detached};
    finished_release();
}

static void drop_finished(int i) { finished[i] = finished[--finished_count]; }

/* --- pthread_create wrapper: every thread registers itself before running its function */
int __real_pthread_create(pthread_t* thread, const pthread_attr_t* attr, void* (*fn)(void*), void* arg);

#define CREATOR_FRAMES 5
typedef struct {
    void* (*fn)(void*);
    void* arg;
    uintptr_t creator[CREATOR_FRAMES]; /* return addresses in the creating thread (bring-up) */
} Start;

static pthread_key_t slot_key;
static pthread_once_t slot_once = PTHREAD_ONCE_INIT;

/* The slot's thread is ending (it returned, or the key's destructor runs in pthread_exit). */
static void unregister_slot(void* p)
{
    const intptr_t slot = (intptr_t)p - 1;
    if (slot >= 0 && slot < MAX_THREADS) {
        add_finished(atomic_load(&thread_detached[slot]));
        atomic_store(&thread_detached[slot], false);
        atomic_store(&thread_ids[slot], (pthread_t)0);
        atomic_store(&thread_objects[slot], NULL);
        atomic_store(&thread_entries[slot], NULL);
        atomic_store(&threads[slot], 0);
    }
}
static void make_key(void) { pthread_key_create(&slot_key, unregister_slot); }

/* Cores: libnx creates every thread on the process's default core, and Horizon never moves a
 * thread off its allowed cores. Box64, the game's ~40 threads, the GPU library and the shader
 * compilers then share one core, and a high-priority thread spinning on a lock starves its
 * holder for good. Each thread may run on every core the process has (no ideal core: the
 * kernel balances them). */
static void spread_cores(void)
{
    static u64 mask;
    static atomic_bool logged;
    static int one_core = -1;
    if (one_core < 0) { const char* v = getenv("GR_ONE_CORE"); one_core = v && v[0] == '1'; }
    if (one_core) return;
    if (!mask && R_FAILED(svcGetInfo(&mask, InfoType_CoreMask, CUR_PROCESS_HANDLE, 0))) mask = 0;
    if (!mask) return;
    Result rc = svcSetThreadCoreMask(CUR_THREAD_HANDLE, -1, (u32)mask);
    if (R_FAILED(rc)) rc = svcSetThreadCoreMask(CUR_THREAD_HANDLE, -3, (u32)mask); /* keep the ideal core */
    if (!atomic_exchange(&logged, true)) {
        char line[80];
        snprintf(line, sizeof line, "threads may run on cores 0x%llx (0x%x)", (unsigned long long)mask, rc);
        gr_crumb(line);
    }
}

static void* thread_start(void* p)
{
    Start start = *(Start*)p;
    free(p);
    spread_cores();
    const int slot = register_thread(threadGetCurHandle());
    if (slot >= 0) {
        atomic_store(&thread_objects[slot], threadGetSelf());
        atomic_store(&thread_entries[slot], (void*)start.fn);
        atomic_store(&thread_ids[slot], pthread_self());
    }
    static atomic_bool warned;
    if (slot < 0 && !atomic_exchange(&warned, true))
        fprintf(stderr, "Suspend: thread table full, a thread will not freeze on HOME\n");
    /* The key's destructor also runs on pthread_exit, so the slot is freed on every way out. */
    pthread_once(&slot_once, make_key);
    if (slot >= 0) pthread_setspecific(slot_key, (void*)(intptr_t)(slot + 1));
    {
        char line[200];
        int n = snprintf(line, sizeof line, "thread starts (made by");
        for (int f = 0; f < CREATOR_FRAMES && start.creator[f] && n < (int)sizeof line - 24; ++f)
            n += snprintf(line + n, sizeof line - n, " %+lld", (long long)((intptr_t)start.creator[f] - (intptr_t)gr_quit_point));
        snprintf(line + n, sizeof line - n, ")");
        gr_crumb(line);
    }
    void* result = start.fn(start.arg);
    thread_crumb("returned", (void*)start.fn);
    if (slot >= 0) {
        pthread_setspecific(slot_key, NULL);
        unregister_slot((void*)(intptr_t)(slot + 1));
    }
    return result;
}

/* Stacks: libnx's default is small, and the GPU library and the Vulkan driver's shader compiler
 * expect the desktop's (8 MiB on Linux). A thread created without a size gets that; one with a
 * smaller size than 1 MiB gets 1 MiB (Box64 and the runtime run on these stacks too). */
#define DEFAULT_STACK ((size_t)8 << 20)
#define MIN_STACK ((size_t)1 << 20)
int __wrap_pthread_create(pthread_t* thread, const pthread_attr_t* attr, void* (*fn)(void*), void* arg)
{
    if (atomic_load(&quitting)) return EAGAIN;
    Start* start = malloc(sizeof *start);
    if (!start) return EAGAIN;
    start->fn = fn;
    start->arg = arg;
    memset(start->creator, 0, sizeof start->creator);
    {
        Thread* self = threadGetSelf();
        uintptr_t fp = (uintptr_t)__builtin_frame_address(0);
        const uintptr_t lo = self && self->stack_mirror ? (uintptr_t)self->stack_mirror : 0;
        const uintptr_t hi = lo ? lo + self->stack_sz : 0;
        for (int f = 0; f < CREATOR_FRAMES && lo && fp >= lo && fp + 16 <= hi && !(fp & 15); ++f) {
            start->creator[f] = ((const uintptr_t*)fp)[1];
            const uintptr_t next = ((const uintptr_t*)fp)[0];
            if (next <= fp) break;
            fp = next;
        }
    }
    pthread_attr_t sized;
    size_t size = 0;
    if (attr) {
        sized = *attr;
        pthread_attr_getstacksize(&sized, &size);
        if (size < MIN_STACK) pthread_attr_setstacksize(&sized, MIN_STACK);
    } else {
        pthread_attr_init(&sized);
        pthread_attr_setstacksize(&sized, DEFAULT_STACK);
    }
    const int r = __real_pthread_create(thread, &sized, thread_start, start);
    if (!attr) pthread_attr_destroy(&sized);
    if (r) free(start);
    return r;
}

/* pthread_detach: the thread is closed by the applet thread once it ends (see above). */
int __wrap_pthread_detach(pthread_t id)
{
    for (int i = 0; i < MAX_THREADS; ++i)
        if (atomic_load(&threads[i]) && atomic_load(&thread_ids[i]) == id) {
            atomic_store(&thread_detached[i], true);
            /* It may have ended meanwhile: then its entry is in the list already. */
            break;
        }
    finished_acquire();
    for (int i = 0; i < finished_count; ++i)
        if (finished[i].id == id) finished[i].detached = true;
    finished_release();
    return 0;
}

/* Closes the detached threads that have ended. Applet thread, never while the game is frozen
 * (threadClose frees the stack: a paused thread may hold malloc's lock). */
static void reap_finished(void)
{
    if (!finished_try()) return;
    for (int i = 0; i < finished_count;) {
        if (finished[i].detached && R_SUCCEEDED(svcWaitSynchronizationSingle(finished[i].handle, 0))) {
            gr_crumb("closing a finished detached thread");
            const Result rc = threadClose(finished[i].thread);
            char line[64];
            snprintf(line, sizeof line, "closed it: 0x%x", rc);
            gr_crumb(line);
            drop_finished(i);
        } else {
            ++i;
        }
    }
    finished_release();
}

/* Bring-up (the call trace): pauses a thread just long enough to read its registers. */
static int sample_regs(uint64_t host, uint64_t regs[34])
{
    const Handle h = (Handle)host;
    if (!h || h == threadGetCurHandle()) return -1;
    if (R_FAILED(svcSetThreadActivity(h, ThreadActivity_Paused))) return -1;
    ThreadContext ctx;
    const Result rc = svcGetThreadContext3(&ctx, h);
    svcSetThreadActivity(h, ThreadActivity_Runnable);
    if (R_FAILED(rc)) return -1;
    for (int i = 0; i < 29; ++i) regs[i] = ctx.cpu_gprs[i].x;
    regs[29] = ctx.fp;
    regs[30] = ctx.lr;
    regs[31] = ctx.pc.x;
    regs[32] = ctx.sp;
    regs[33] = 0;
    return 0;
}
static int sample_thread(uint64_t host, uint64_t regs[34])
{
    const int r = sample_regs(host, regs);
    if (!r) svcSleepThread(2000000); /* 2 ms between samples */
    return r;
}

/* Readable memory of this process (no fault handler: a bad read would end it). */
static bool readable(uintptr_t at, size_t size)
{
    MemoryInfo info;
    u32 page;
    if (R_FAILED(svcQueryMemory(&info, &page, at))) return false;
    if ((info.type & 0xff) == MemType_Unmapped || !(info.perm & Perm_R)) return false;
    return at + size <= info.addr + info.size;
}

static long long rel(uintptr_t a) { return (long long)((intptr_t)a - (intptr_t)gr_quit_point); }

static void backtrace_of(const char* who, uintptr_t fp, uintptr_t lr, uintptr_t pc, void (*line)(const char*))
{
    char out[240];
    int n = snprintf(out, sizeof out, "  %s: pc %+lld lr %+lld frames", who, rel(pc), rel(lr));
    for (int f = 0; f < 14 && fp && !(fp & 15) && readable(fp, 16) && n < (int)sizeof out - 16; ++f) {
        const uintptr_t next = ((const uintptr_t*)fp)[0], ret = ((const uintptr_t*)fp)[1];
        n += snprintf(out + n, sizeof out - (size_t)n, " %+lld", rel(ret));
        if (next <= fp) break;
        fp = next;
    }
    line(out);
}

/* A thread stopped in host code: its backtrace, and for a lock wait, the owner's too. */
static void describe_native(uint64_t host, const uint64_t r[34], void (*line)(const char*))
{
    char out[240];
    snprintf(out, sizeof out, "  host code; x0 %#llx x1 %#llx x2 %#llx sp %#llx", (unsigned long long)r[0],
             (unsigned long long)r[1], (unsigned long long)r[2], (unsigned long long)r[32]);
    line(out);
    backtrace_of("stuck", r[29], r[30], r[31], line);
    /* svcArbitrateLock(owner, address, self): libnx's mutexLock (pthread mutexes, rwlocks). */
    const Handle owner = (Handle)(r[0] & ~(u64)0x40000000);
    if (owner && owner != (Handle)host && (r[31] & 3) == 0) {
        uint64_t o[34];
        const char* name = box64e_trace_name_of(owner);
        void* entry = NULL;
        for (int i = 0; i < MAX_THREADS; ++i)
            if (atomic_load(&threads[i]) == owner) entry = atomic_load(&thread_entries[i]);
        if (r[1] && readable(r[1], 4)) {
            snprintf(out, sizeof out, "  lock word at %#llx: %#x; owner handle %#x '%s' (entry %+lld)", (unsigned long long)r[1],
                     *(const volatile u32*)(uintptr_t)r[1], owner, name ? name : "?", entry ? rel((uintptr_t)entry) : 0);
            line(out);
        }
        if (!sample_thread(owner, o)) {
            snprintf(out, sizeof out, "  owner x0 %#llx x1 %#llx", (unsigned long long)o[0], (unsigned long long)o[1]);
            line(out);
            backtrace_of("owner", o[29], o[30], o[31], line);
        }
    }
}

/* Every run, every 20 s (the heartbeat): a thread waiting in svcArbitrateLock (a pthread mutex)
 * on two samples in a row is in a deadlock. Its backtrace goes to boot.txt, and the lock's
 * owner: in a system call only x19-x28 are kept, so each of them that points at a readable
 * word holding a known thread's handle names the owner (libnx's lock word is the owner's
 * handle, with bit 30 set while others wait). Reported once per thread. */
static void watch_locks(void)
{
    static int seen[MAX_THREADS];
    static bool told[MAX_THREADS];
    for (int i = 0; i < MAX_THREADS; ++i) {
        const Handle h = atomic_load(&threads[i]);
        if (!h || h == applet_thread || h == threadGetCurHandle()) { seen[i] = 0; told[i] = false; continue; }
        uint64_t r[34];
        if (sample_regs(h, r) || r[31] != (uintptr_t)svcArbitrateLock) { seen[i] = 0; continue; }
        if (++seen[i] < 2 || told[i]) continue;
        told[i] = true;
        char out[240];
        void* entry = atomic_load(&thread_entries[i]);
        snprintf(out, sizeof out, "DEADLOCK: thread %#x (entry %+lld) has waited for a lock for 20 s or more", h,
                 entry ? rel((uintptr_t)entry) : 0);
        gr_crumb(out);
        backtrace_of("stuck", r[29], r[30], r[31], gr_crumb);
        int n = snprintf(out, sizeof out, "  x19-x28:");
        for (int k = 19; k <= 28 && n < (int)sizeof out - 20; ++k) n += snprintf(out + n, sizeof out - (size_t)n, " %#llx", (unsigned long long)r[k]);
        gr_crumb(out);
        for (int k = 19; k <= 28; ++k) {
            const uintptr_t at = (uintptr_t)r[k];
            if (at & 3 || at < 0x1000 || !readable(at, 4)) continue;
            const u32 word = *(const volatile u32*)at;
            const Handle owner = (Handle)(word & ~0x40000000u);
            int o = -1;
            for (int j = 0; j < MAX_THREADS && owner; ++j)
                if (atomic_load(&threads[j]) == owner) o = j;
            if (o < 0) continue;
            void* owner_entry = atomic_load(&thread_entries[o]);
            snprintf(out, sizeof out, "  x%d -> lock word %#x: owner thread %#x (entry %+lld)", k, word, owner,
                     owner_entry ? rel((uintptr_t)owner_entry) : 0);
            gr_crumb(out);
            uint64_t q[34];
            if (!sample_regs(owner, q)) backtrace_of("owner", q[29], q[30], q[31], gr_crumb);
        }
    }
}

static uint64_t current_host(void) { return threadGetCurHandle(); }
void gr_trace_hooks(void) { box64e_thread_host = current_host; box64e_trace_native = describe_native; }

/* --- freezing */
static void freeze(void)
{
    if (atomic_load(&frozen)) return;
    /* A save being written finishes first (up to 3 s), so closing the game from the HOME menu
     * while it is frozen cannot leave a half-written save behind. */
    for (int waited = 0; runtime_savedata_busy() && waited < 300; ++waited) svcSleepThread(10 * 1000000ull);
    atomic_store(&frozen, true);
    frozen_at = ticks_now();
    for (int i = 0; i < listener_count; ++i) listeners[i](true);
    paused_count = 0;
    for (int i = 0; i < MAX_THREADS; ++i) {
        const Handle h = atomic_load(&threads[i]);
        if (!h || h == applet_thread) continue;
        if (R_SUCCEEDED(svcSetThreadActivity(h, ThreadActivity_Paused))) paused[paused_count++] = h;
    }
}

static void thaw(void)
{
    if (!atomic_load(&frozen)) return;
    /* The clocks first, so no thread sees the suspended time when it runs again. */
    runtime_suspended_ticks += ticks_now() - frozen_at;
    for (int i = 0; i < paused_count; ++i) svcSetThreadActivity(paused[i], ThreadActivity_Runnable);
    paused_count = 0;
    atomic_store(&frozen, false);
    for (int i = 0; i < listener_count; ++i) listeners[i](false);
}

static void on_applet_message(AppletHookType hook, void* param)
{
    (void)param;
    switch (hook) {
    case AppletHookType_OnFocusState:
    case AppletHookType_OnResume:
        if (appletGetFocusState() == AppletFocusState_Background) freeze();
        else if (appletGetFocusState() == AppletFocusState_InFocus) thaw();
        break;
    default:
        break;
    }
}

static AppletHookCookie hook_cookie;

/* During the boot (until the game has shown ~10 s of frames: gr_suspend_boot_done, from
 * gpu/shim/gow3_overlay.cpp) HOME and sleep wait: suspending then could end the whole homebrew
 * session (the GPU and the game's threads are still being set up). HOME is blocked outright;
 * sleep waits on a sleep lock, as an official game holds one while it saves. */
static atomic_bool boot_guard;
static bool sleep_locked;
static void begin_boot_guard(void)
{
    const Result home = appletBeginBlockingHomeButton(0);
    if (R_SUCCEEDED(home)) atomic_store(&boot_guard, true);
    char line[120];
    snprintf(line, sizeof line, "boot guard: HOME blocked (%#x)", home);
    gr_crumb(line);
}
static void* acquire_sleep_lock(void* arg)
{
    (void)arg;
    /* It waits for the lock's event, so on its own thread: the boot never waits for it. */
    const Result r = appletRequestToAcquireSleepLock();
    sleep_locked = R_SUCCEEDED(r);
    char line[120];
    snprintf(line, sizeof line, "boot guard: sleep lock %s (%#x)", sleep_locked ? "held" : "not held", r);
    gr_crumb(line);
    return NULL;
}
void gr_suspend_boot_done(void)
{
    if (!atomic_exchange(&boot_guard, false)) return;
    appletEndBlockingHomeButton();
    if (sleep_locked) { appletReleaseSleepLock(); sleep_locked = false; }
    gr_crumb("boot guard: over, HOME and sleep allowed");
}

void gr_suspend_init(void)
{
    applet_thread = threadGetCurHandle();
    box64e_tsc_offset = &runtime_suspended_ticks;
    /* The applet thread keeps running out of focus (it does the freezing); the hook still sees
     * every focus change, and AppletMessage_Resume arrives after the console wakes up. */
    appletSetFocusHandlingMode(AppletFocusHandlingMode_NoSuspend);
    appletSetRestartMessageEnabled(true);
    appletHook(&hook_cookie, on_applet_message, NULL);
    if (!getenv("GR_NO_BOOT_GUARD")) {
        begin_boot_guard();
        pthread_t t;
        if (!pthread_create(&t, NULL, acquire_sleep_lock, NULL)) pthread_detach(t);
    }
}

void gr_suspend_add_listener(GrSuspendFn fn)
{
    if (listener_count < MAX_LISTENERS) listeners[listener_count++] = fn;
}

bool gr_suspend_is_frozen(void) { return atomic_load(&frozen); }

/* --- exiting
 * Console tests (switch2/logs, 2026-11-18): returning to the homebrew loader is clean only when
 * no other thread of ours still exists, down to its kernel object: a thread left behind, even
 * paused with its stack unmapped, or ended but with its handle still open, makes the next
 * program crash (with every thread ended, stacks unmapped and handles closed, all nine test
 * paths and the game return cleanly); ending the whole process instead is fatal under HEN
 * (the homebrew menu's own exit does the same). Horizon cannot end or redirect another thread,
 * so every thread ends itself: when the game exits, `quitting` is set and every place a thread
 * can block is a quit point (condition variable waits, sleeps and yields, all wrapped with
 * -Wl,--wrap=..., waits cut into 50 ms slices; spurious wake-ups are allowed there, and the
 * runtime's waits loop). A thread at a quit point unlocks the wait's mutex and exits. The applet
 * thread waits for all of them, unmaps the stacks libnx keeps for threads nobody joined, releases
 * the memory and returns to the homebrew menu. If a thread does not end (it spins in guest code,
 * or waits on a mutex a quitting thread held), the screen says so and the software is closed
 * from the HOME menu. */
void __real__exit(int code) __attribute__((noreturn));
static atomic_bool exit_requested;
static atomic_int exit_code;

static GrExitFn exit_hook;
void gr_suspend_set_exit_hook(GrExitFn fn) { exit_hook = fn; }

void gr_crumb(const char* what); /* gr_switch.c: boot.txt */
#define exit_crumb gr_crumb

static void __attribute__((noreturn)) quit_thread(pthread_mutex_t* held)
{
    if (held) pthread_mutex_unlock(held);
    pthread_exit(NULL); /* the slot key's destructor lists it as finished */
}

/* A quit point: ends the calling thread once the game is exiting (never the applet thread). */
void gr_quit_point(void)
{
    if (atomic_load_explicit(&quitting, memory_order_relaxed) && threadGetCurHandle() != applet_thread) quit_thread(NULL);
}

#define SLICE_NS 50000000ll
int __real_pthread_cond_wait(pthread_cond_t* c, pthread_mutex_t* m);
int __real_pthread_cond_timedwait(pthread_cond_t* c, pthread_mutex_t* m, const struct timespec* t);
int __real_sched_yield(void);
int __real_pthread_join(pthread_t t, void** value);

static struct timespec slice_end(clockid_t clock, const struct timespec* limit)
{
    struct timespec now;
    clock_gettime(clock, &now);
    long long ns = (long long)now.tv_nsec + SLICE_NS;
    struct timespec end = {now.tv_sec + (time_t)(ns / 1000000000ll), (long)(ns % 1000000000ll)};
    if (limit && (limit->tv_sec < end.tv_sec || (limit->tv_sec == end.tv_sec && limit->tv_nsec < end.tv_nsec))) return *limit;
    return end;
}

int __wrap_pthread_cond_wait(pthread_cond_t* c, pthread_mutex_t* m)
{
    if (threadGetCurHandle() == applet_thread) return __real_pthread_cond_wait(c, m);
    const struct timespec end = slice_end(c->clock_id, NULL);
    const int r = __real_pthread_cond_timedwait(c, m, &end);
    if (atomic_load(&quitting)) quit_thread(m);
    return r == ETIMEDOUT ? 0 : r; /* a slice that ran out is a spurious wake-up */
}

int __wrap_pthread_cond_timedwait(pthread_cond_t* c, pthread_mutex_t* m, const struct timespec* limit)
{
    if (threadGetCurHandle() == applet_thread) return __real_pthread_cond_timedwait(c, m, limit);
    const struct timespec end = slice_end(c->clock_id, limit);
    const int r = __real_pthread_cond_timedwait(c, m, &end);
    if (atomic_load(&quitting)) quit_thread(m);
    if (r == ETIMEDOUT && (end.tv_sec != limit->tv_sec || end.tv_nsec != limit->tv_nsec)) return 0; /* the slice, not the limit */
    return r;
}

int __wrap_sched_yield(void)
{
    gr_quit_point();
    /* Yield with core migration: an idle core may take this thread or one waiting behind it. */
    svcSleepThread(YieldType_WithCoreMigration);
    return 0;
}

int __wrap_pthread_join(pthread_t t, void** value)
{
    gr_quit_point();
    const int r = __real_pthread_join(t, value);
    if (r == 0) { /* pthread_join closed it: its entry goes */
        finished_acquire();
        for (int i = 0; i < finished_count; ++i)
            if (finished[i].id == t) { drop_finished(i); break; }
        finished_release();
    }
    return r;
}

/* Sleeps (host_compat_horizon.c's compat_sleep_ns) in slices, with a quit point after each. */
void gr_sleep_ns(uint64_t ns)
{
    while (ns) {
        const uint64_t step = ns > (uint64_t)SLICE_NS ? (uint64_t)SLICE_NS : ns;
        svcSleepThread((s64)step);
        ns -= step;
        gr_quit_point();
    }
}

/* nanosleep (the runtime's guest sleeps, usleep): the same, outside the applet thread. */
int __real_nanosleep(const struct timespec* req, struct timespec* rem);
int __wrap_nanosleep(const struct timespec* req, struct timespec* rem)
{
    if (threadGetCurHandle() == applet_thread || !req || req->tv_sec < 0 || req->tv_nsec < 0) return __real_nanosleep(req, rem);
    gr_sleep_ns((uint64_t)req->tv_sec * 1000000000ull + (uint64_t)req->tv_nsec);
    if (rem) *rem = (struct timespec){0, 0};
    return 0;
}

static int live_threads(void)
{
    int n = 0;
    for (int i = 0; i < MAX_THREADS; ++i) {
        const Handle h = atomic_load(&threads[i]);
        if (h && h != applet_thread) ++n;
    }
    return n;
}

/* Shown when a thread does not end: the process cannot go back to the homebrew menu safely. */
static void __attribute__((noreturn)) stuck_screen(int code, int left)
{
    printf("Exit: %d thread(s) did not end; waiting to be closed from the HOME menu\n", left);
    /* Where each one is: paused, its registers read (addresses as loaded; the reference symbol
     * gives the offset to the ELF's addresses), and a few frames of its frame-pointer chain. */
    printf("Exit: reference gr_quit_point at %p\n", (void*)gr_quit_point);
    for (int i = 0; i < MAX_THREADS; ++i) {
        const Handle h = atomic_load(&threads[i]);
        if (!h || h == applet_thread) continue;
        u64 id = 0;
        svcGetThreadId(&id, h);
        const Result pr = svcSetThreadActivity(h, ThreadActivity_Paused);
        svcSleepThread(1000000ull);
        ThreadContext ctx = {0};
        const Result cr = svcGetThreadContext3(&ctx, h);
        printf("Exit: thread %llu (entry %p): pause 0x%x, context 0x%x, pc %p lr %p sp %p fp %p\n",
               (unsigned long long)id, atomic_load(&thread_entries[i]), pr, cr, (void*)ctx.pc.x, (void*)ctx.lr,
               (void*)ctx.sp, (void*)ctx.fp);
        Thread* t = atomic_load(&thread_objects[i]);
        if (R_FAILED(cr) || !t || !t->stack_mirror) continue;
        const uintptr_t lo = (uintptr_t)t->stack_mirror, hi = lo + t->stack_sz;
        uintptr_t fp = ctx.fp;
        printf("Exit:   frames:");
        for (int f = 0; f < 12 && fp >= lo && fp + 16 <= hi && !(fp & 15); ++f) {
            const uintptr_t next = ((const uintptr_t*)fp)[0], ret = ((const uintptr_t*)fp)[1];
            printf(" %p", (void*)ret);
            if (next <= fp) break;
            fp = next;
        }
        printf("\n");
    }
    fflush(NULL);
    exit_crumb("threads did not end; waiting for HOME > Close");
    consoleInit(NULL);
    printf("\n  Gravity Rush Remastered stopped (code %d).\n\n", code);
    printf("  %d of its threads did not end, so it cannot go back to the\n", left);
    printf("  homebrew menu safely. Press HOME and close the software.\n\n");
    printf("  The details are in log.txt.\n");
    consoleUpdate(NULL);
    for (;;) {
        appletMainLoop();
        svcSleepThread(100 * 1000000ull);
    }
}

static void __attribute__((noreturn)) end_process(int code)
{
    for (int waited = 0; runtime_savedata_busy() && waited < 300; ++waited) svcSleepThread(10 * 1000000ull);
    if (!atomic_load(&frozen))
        for (int i = 0; i < listener_count; ++i) listeners[i](true);
    else { /* closed from the HOME menu while frozen: the threads run again, only to end */
        for (int i = 0; i < paused_count; ++i) svcSetThreadActivity(paused[i], ThreadActivity_Runnable);
        paused_count = 0;
    }
    atomic_store(&quitting, true);
    const int before = live_threads();
    int left = before;
    for (int waited = 0; left && waited < 300; ++waited) { /* up to 3 s */
        svcSleepThread(10 * 1000000ull);
        left = live_threads();
    }
    svcSleepThread(20 * 1000000ull); /* the last ones are between unregistering and svcExitThread */
    printf("Exit: %d threads to end, %d still running\n", before, left);
    if (left) stuck_screen(code, left);
    /* Every thread that ended and was not joined: waited for (its handle signals once it has
     * really terminated, past its last instruction), then closed. */
    int closed = 0, not_ended = 0;
    finished_acquire();
    for (int i = 0; i < finished_count; ++i) {
        if (R_FAILED(svcWaitSynchronizationSingle(finished[i].handle, 200 * 1000000ll))) { ++not_ended; continue; }
        if (R_SUCCEEDED(threadClose(finished[i].thread))) ++closed;
    }
    finished_count = 0;
    finished_release();
    printf("Exit: all threads ended; %d closed, %d not terminated\n", closed, not_ended);
    fflush(NULL);
    exit_crumb("all threads ended");
    if (exit_hook) exit_hook(); /* releases guest memory, the JIT and the heap */
    fflush(NULL);
    exit_crumb("returning to the homebrew loader");
    __real__exit(code);
}

static void __attribute__((noreturn)) leave(int code)
{
    if (!applet_thread || threadGetCurHandle() == applet_thread) {
        if (!applet_thread) { fflush(NULL); __real__exit(code); }
        end_process(code);
    }
    fflush(NULL);
    atomic_store(&exit_code, code);
    atomic_store(&exit_requested, true);
    while (!atomic_load(&quitting)) svcSleepThread(10 * 1000000ull);
    quit_thread(NULL);
}

/* abort (an uncaught C++ exception ends in std::terminate, then here): newlib's ends the
 * process silently with _exit(1). This logs where it came from first (addresses as loaded;
 * gr_quit_point is the reference for the ELF's addresses), then exits like the game would. */
void __attribute__((noreturn)) __wrap_abort(void)
{
    fprintf(stderr, "abort() called from %p (reference gr_quit_point at %p)\n", __builtin_return_address(0),
            (void*)gr_quit_point);
    Thread* t = threadGetSelf();
    if (t && t->stack_mirror) {
        const uintptr_t lo = (uintptr_t)t->stack_mirror, hi = lo + t->stack_sz;
        uintptr_t fp = (uintptr_t)__builtin_frame_address(0);
        fprintf(stderr, "abort() frames:");
        for (int f = 0; f < 16 && fp >= lo && fp + 16 <= hi && !(fp & 15); ++f) {
            const uintptr_t next = ((const uintptr_t*)fp)[0], ret = ((const uintptr_t*)fp)[1];
            fprintf(stderr, " %p", (void*)ret);
            if (next <= fp) break;
            fp = next;
        }
        fprintf(stderr, "\n");
    }
    leave(134);
}

void __wrap_exit(int code) { gr_heap_report("exit"); leave(code); }
void __wrap__exit(int code) { leave(code); }
void __wrap__Exit(int code) { leave(code); }

void gr_suspend_exit(int code) { leave(code); }

/* Console bring-up: where every thread is, sampled about 5 times a second into threads.txt
 * (rewritten and closed each time, so the last sample survives a crash). Each thread is paused
 * only while its registers and a few frames are read; nothing is allocated meanwhile (a paused
 * thread may hold malloc's lock), the file is written after all run again. Addresses are offsets
 * from gr_quit_point. */
void gr_sample_path(const char* path); /* gr_switch.c sets where threads.txt goes */
static char sample_path[600];
void gr_sample_path(const char* path) { snprintf(sample_path, sizeof sample_path, "%s", path); }
#define SAMPLE_FRAMES 8
typedef struct { u64 id; intptr_t entry, pc, lr, frames[SAMPLE_FRAMES]; int nframes; Result rc; } Sample;
static Sample samples[MAX_THREADS];
static void sample_threads(void)
{
    if (!sample_path[0]) return;
    const intptr_t ref = (intptr_t)gr_quit_point;
    int n = 0;
    for (int i = 0; i < MAX_THREADS; ++i) {
        const Handle h = atomic_load(&threads[i]);
        if (!h || h == applet_thread) continue;
        Sample* sm = &samples[n++];
        *sm = (Sample){0};
        svcGetThreadId(&sm->id, h);
        sm->entry = (intptr_t)atomic_load(&thread_entries[i]) - ref;
        if (R_FAILED(sm->rc = svcSetThreadActivity(h, ThreadActivity_Paused))) continue;
        ThreadContext ctx;
        if (R_SUCCEEDED(sm->rc = svcGetThreadContext3(&ctx, h))) {
            sm->pc = (intptr_t)ctx.pc.x - ref;
            sm->lr = (intptr_t)ctx.lr - ref;
            Thread* t = atomic_load(&thread_objects[i]);
            if (t && t->stack_mirror) {
                const uintptr_t lo = (uintptr_t)t->stack_mirror, hi = lo + t->stack_sz;
                uintptr_t fp = ctx.fp;
                while (sm->nframes < SAMPLE_FRAMES && fp >= lo && fp + 16 <= hi && !(fp & 15)) {
                    const uintptr_t next = ((const uintptr_t*)fp)[0];
                    sm->frames[sm->nframes++] = (intptr_t)((const uintptr_t*)fp)[1] - ref;
                    if (next <= fp) break;
                    fp = next;
                }
            }
        }
        svcSetThreadActivity(h, ThreadActivity_Runnable);
    }
    FILE* f = fopen(sample_path, "w");
    if (!f) return;
    fprintf(f, "sample at tick %llu, %d threads (offsets from gr_quit_point)\n", (unsigned long long)ticks_now(), n);
    for (int i = 0; i < n; ++i) {
        const Sample* sm = &samples[i];
        fprintf(f, "thread %llu entry %+lld rc 0x%x pc %+lld lr %+lld frames", (unsigned long long)sm->id,
                (long long)sm->entry, sm->rc, (long long)sm->pc, (long long)sm->lr);
        for (int k = 0; k < sm->nframes; ++k) fprintf(f, " %+lld", (long long)sm->frames[k]);
        fprintf(f, "\n");
    }
    fclose(f);
}

void gr_suspend_run_applet_loop(void)
{
    /* appletMainLoop processes the messages and calls the hook; it returns false on an exit
     * request (the game closed from the HOME menu), after which the process just ends: a frozen
     * game is not resumed first, as with an official one. */
    while (!atomic_load(&exit_requested) && appletMainLoop()) {
        if (!atomic_load(&frozen)) reap_finished();
        static unsigned tick;
        if (!atomic_load(&frozen) && ++tick % 20 == 0) sample_threads();
        /* Bring-up: proof of life every 250 ms in boot.txt (GR_HEARTBEAT=1). */
        static int heartbeat = -1;
        if (heartbeat < 0) { const char* v = getenv("GR_HEARTBEAT"); heartbeat = v && v[0] == '1'; }
        if (heartbeat && tick % 25 == 0) gr_crumb("heartbeat (applet thread)");
        if (heartbeat && tick % 1250 == 0 && !atomic_load(&frozen)) {
            gr_heap_report("every 20 s");
            watch_locks();
            box64e_trace_report(gr_crumb, sample_thread);
        }
        svcSleepThread(10 * 1000000ull);
    }
    if (atomic_load(&exit_requested)) {
        printf("The game exited with code %d\n", atomic_load(&exit_code));
        end_process(atomic_load(&exit_code));
    }
    printf("Exit requested by the system\n");
    end_process(0);
}
