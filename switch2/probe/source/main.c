/* Gravity Rush Switch 2 port, phase 0 probe v6 (see switch2/PLAN.md).
 * Measures what a kaiHEN game-role NRO gets: address-space regions, memory budget, the memory
 * svcs the port needs, user exception delivery and recovery, kaiHEN JIT cost, and CPU speed.
 * Results go to the screen and to sdmc:/gr_probe.txt (flushed after every line).
 *
 * v2 changes after the first console run:
 * - The heap is cut to PROBE_HEAP (here and with svcSetHeapSize), because hbloader gives the whole
 *   budget to the heap, which made svcMapPhysicalMemory fail with LimitReached (0x10801).
 * - The exception handler no longer returns (libounce calls svcBreak after it returns). It resumes
 *   the thread with probe_resume, or longjmps out of a guarded test.
 * - The svcMapMemory destination is found under virtmemLock, in the stack region.
 * - Adds a transfer-memory aliasing test, a clock estimate and a 6-core run. */
#include <switch.h>
#include <kaihen.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <unistd.h>

#define MIB (UINT64_C(1) << 20)
#define GIB (UINT64_C(1) << 30)
#define PROBE_HEAP (2 * GIB)

static FILE *logf;

static void out(const char *fmt, ...) {
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    printf("%s\n", line);
    consoleUpdate(NULL);
    if (logf) { fprintf(logf, "%s\n", line); fflush(logf); fsync(fileno(logf)); }
}

static u64 info(u32 type) {
    u64 v = 0;
    Result r = svcGetInfo(&v, type, CUR_PROCESS_HANDLE, 0);
    return R_SUCCEEDED(r) ? v : ~(u64)0;
}

/* InfoType_DebuggerAttached takes no handle (v5 passed the process handle and always got an error). */
static u64 debugger_attached(void) {
    u64 v = 0;
    Result r = svcGetInfo(&v, InfoType_DebuggerAttached, INVALID_HANDLE, 0);
    return R_SUCCEEDED(r) ? v : ~(u64)0;
}

static double ms_since(u64 t0) {
    return (double)(armGetSystemTick() - t0) * 1000.0 / (double)armGetSystemTickFreq();
}

static void mem_line(const char *when) {
    out("memory %s: total %lu MiB, used %lu MiB", when, info(InfoType_TotalMemorySize) / MIB,
        info(InfoType_UsedMemorySize) / MIB);
}

/* --- heap: use only the first PROBE_HEAP of hbloader's heap, then give the rest back. */
extern char *fake_heap_start, *fake_heap_end;
static u64 loader_heap_size;

void __libnx_initheap(void) {
    char *addr = envGetHeapOverrideAddr();
    u64 size = envGetHeapOverrideSize();
    loader_heap_size = size;
    if (size > PROBE_HEAP) size = PROBE_HEAP;
    fake_heap_start = addr;
    fake_heap_end = addr + size;
}

static int heap_shrunk;
static u64 heap_full_size;

/* The current heap extent: the Heap-type blocks from the heap region's start. */
static u64 heap_extent(void) {
    u64 base = info(InfoType_HeapRegionAddress), addr = base;
    MemoryInfo mi; u32 pi;
    while (R_SUCCEEDED(svcQueryMemory(&mi, &pi, addr)) && mi.type == MemType_Heap && mi.addr + mi.size > addr)
        addr = mi.addr + mi.size;
    return addr - base;
}

static void shrink_heap(void) {
    out("== heap");
    u64 base = info(InfoType_HeapRegionAddress), loader = (u64)envGetHeapOverrideAddr();
    heap_full_size = heap_extent();
    out("heap region 0x%lx, heap size %lu MiB; loader heap 0x%lx +%lu MiB (ends %s heap end)", base,
        heap_full_size / MIB, loader, loader_heap_size / MIB,
        loader + loader_heap_size == base + heap_full_size ? "at the" : "NOT at the");
    mem_line("before shrinking");
    if (loader < base || loader + loader_heap_size > base + heap_full_size || loader_heap_size <= PROBE_HEAP) {
        out("loader heap is not inside the heap; not shrinking");
        return;
    }
    u64 keep = (loader + PROBE_HEAP - base + 0x1fffff) & ~UINT64_C(0x1fffff);
    void *a = NULL;
    Result r = svcSetHeapSize(&a, keep);
    out("svcSetHeapSize(%lu MiB): 0x%x at %p", keep / MIB, r, a);
    heap_shrunk = R_SUCCEEDED(r);
    mem_line("after shrinking");
    void *p = malloc(64 * MIB);
    out("64 MiB malloc at %p (must be below %p)", p, fake_heap_end);
    free(p);
}

static void restore_heap(void) {
    if (!heap_shrunk) return;
    void *a = NULL;
    out("restore heap for hbloader: 0x%x", svcSetHeapSize(&a, heap_full_size));
}

/* --- exceptions */
alignas(16) u8 __nx_exception_stack[0x8000];
u64 __nx_exception_stack_size = sizeof(__nx_exception_stack);
u32 __nx_exception_ignoredebug = 1;

void probe_resume(const ThreadExceptionDump *d) __attribute__((noreturn));
_Static_assert(offsetof(ThreadExceptionDump, cpu_gprs) == 16, "x0");
_Static_assert(offsetof(ThreadExceptionDump, fp) == 248, "fp");
_Static_assert(offsetof(ThreadExceptionDump, sp) == 264, "sp");
_Static_assert(offsetof(ThreadExceptionDump, pc) == 272, "pc");
_Static_assert(offsetof(ThreadExceptionDump, fpu_gprs) == 288, "q0");
_Static_assert(offsetof(ThreadExceptionDump, pstate) == 800, "pstate");

static volatile u8 *fault_page;
static volatile u64 fault_page_size;
static volatile int fault_count;
static volatile u64 fault_addr, fault_tick;
static jmp_buf guard_jb;
static volatile int guard_active;
static volatile u32 guard_desc;
static volatile u64 guard_far, guard_pc;

static volatile int handler_entries;

void __libnx_exception_handler(ThreadExceptionDump *ctx) {
    u64 far = ctx->far.x;
    if (handler_entries == 0) svcSleepThread(5000000000LL); /* second 5 s: the C handler was reached */
    if (handler_entries++ < 3 && logf) {
        fprintf(logf, "handler entered: desc 0x%x esr 0x%x pc 0x%lx far 0x%lx\n", ctx->error_desc, ctx->esr,
                ctx->pc.x, far);
        fflush(logf); fsync(fileno(logf));
    }
    if (fault_page && far >= (u64)fault_page && far < (u64)fault_page + fault_page_size) {
        fault_count++;
        fault_addr = far;
        fault_tick = armGetSystemTick();
        svcSetMemoryPermission((void *)((far & ~UINT64_C(0x3fff))), 0x4000, Perm_Rw);
        probe_resume(ctx);
    }
    if (guard_active) {
        guard_active = 0;
        guard_desc = ctx->error_desc;
        guard_far = far;
        guard_pc = ctx->pc.x;
        longjmp(guard_jb, 1);
    }
    if (logf) {
        fprintf(logf, "unexpected exception 0x%x at pc 0x%lx far 0x%lx\n", ctx->error_desc, ctx->pc.x, far);
        fflush(logf); fsync(fileno(logf));
    }
    /* returning ends in svcBreak */
}

/* A guarded access: a fault inside it is reported, not fatal. */
#define GUARDED(what, stmt) do { \
    if (setjmp(guard_jb) == 0) { guard_active = 1; stmt; guard_active = 0; } \
    else out("%s: faulted, desc 0x%x far 0x%lx pc 0x%lx", what, guard_desc, guard_far, guard_pc); \
} while (0)

/* A store that keeps nothing live in x16, as probe_resume requires. */
static inline void store_byte(volatile u8 *p, u8 v) {
    __asm__ volatile("strb %w1, [%0]" :: "r"(p), "r"(v) : "x16", "memory");
}

/* --- tests */
static void test_regions(void) {
    static const struct { u32 a, s; const char *name; } regions[] = {
        {InfoType_AslrRegionAddress, InfoType_AslrRegionSize, "aslr"},
        {InfoType_HeapRegionAddress, InfoType_HeapRegionSize, "heap"},
        {InfoType_AliasRegionAddress, InfoType_AliasRegionSize, "alias"},
        {InfoType_StackRegionAddress, InfoType_StackRegionSize, "stack"},
    };
    out("== regions (GR guest range wants 0x1000000000-0xfc00000000, below 1 TiB)");
    for (size_t i = 0; i < sizeof regions / sizeof regions[0]; i++) {
        u64 a = info(regions[i].a), s = info(regions[i].s);
        out("%-6s 0x%010lx-0x%010lx (%lu MiB)", regions[i].name, a, a + s, s / MIB);
    }
    out("core mask 0x%lx, system resource %lu KiB", info(InfoType_CoreMask),
        info(InfoType_SystemResourceSizeTotal) / 1024);

    MemoryInfo mi; u32 pi; u64 addr = 0, mapped = 0, end = info(InfoType_AddressSpaceSize);
    int n = 0;
    while (addr < end && R_SUCCEEDED(svcQueryMemory(&mi, &pi, addr)) && n < 4096) {
        u64 next = mi.addr + mi.size;
        if (next <= addr) break;
        if (next > end) next = end;
        if (mi.type != MemType_Unmapped) mapped += next - addr;
        n++;
        addr = next;
    }
    out("memory map: %d entries, %lu MiB mapped (includes reserved and inaccessible)", n, mapped / MIB);
}

static void test_map_physical(void) {
    out("== svcMapPhysicalMemory at fixed alias-region addresses");
    u64 alias = info(InfoType_AliasRegionAddress), alias_size = info(InfoType_AliasRegionSize);
    void *at = (void *)(alias + 16 * GIB);
    Result r = svcMapPhysicalMemory(at, 16 * MIB);
    out("map 16 MiB at %p: 0x%x", at, r);
    if (R_FAILED(r)) return;
    memset(at, 0xab, 16 * MIB);
    out("write ok");
    svcUnmapPhysicalMemory(at, 16 * MIB);

    /* The PS4 direct memory pool is 5056 MiB. */
    u64 dmem = 5056 * MIB;
    r = svcMapPhysicalMemory(at, dmem);
    out("map 5056 MiB (PS4 direct pool) at %p: 0x%x", at, r);
    if (R_SUCCEEDED(r)) {
        mem_line("with the pool mapped");
        u8 *page = (u8 *)at + 3 * GIB;
        page[8] = 1;
        Result pr = svcSetMemoryPermission(page, 0x4000, Perm_R);
        out("set read-only inside the pool: 0x%x; back to rw: 0x%x", pr, svcSetMemoryPermission(page, 0x4000, Perm_Rw));
        svcUnmapPhysicalMemory(at, dmem);
    }

    u64 ok = 0;
    for (u64 size = 256 * MIB; size <= alias_size - 32 * GIB; size += 256 * MIB) {
        if (R_FAILED(svcMapPhysicalMemory(at, size))) break;
        ok = size;
        svcUnmapPhysicalMemory(at, size);
    }
    out("largest physical map (256 MiB steps): %lu MiB", ok / MIB);
}

static void test_map_memory(void) {
    out("== svcMapMemory (heap -> stack region)");
    void *src = aligned_alloc(0x200000, 16 * MIB);
    if (!src) { out("alloc failed"); return; }
    memset(src, 0x5a, 16 * MIB);
    virtmemLock();
    void *dst = virtmemFindStack(16 * MIB, 0x4000);
    Result r = dst ? svcMapMemory(dst, src, 16 * MIB) : MAKERESULT(Module_Libnx, LibnxError_OutOfMemory);
    virtmemUnlock();
    out("map %p -> %p: 0x%x", src, dst, r);
    if (R_SUCCEEDED(r)) {
        u8 v = 0;
        GUARDED("read through the stack-region view", v = ((volatile u8 *)dst)[12345]);
        out("view reads 0x%02x (want 0x5a)", v);
        MemoryInfo mi; u32 pi;
        svcQueryMemory(&mi, &pi, (u64)src);
        out("source while mapped: perm %u, attr 0x%x", mi.perm, mi.attr);
        out("unmap: 0x%x", svcUnmapMemory(dst, src, 16 * MIB));
    }
    free(src);
}

static u32 perm_at(const void *p) {
    MemoryInfo mi; u32 pi;
    return R_SUCCEEDED(svcQueryMemory(&mi, &pi, (u64)p)) ? mi.perm : 0;
}

/* Two views of the same pages, both usable by this process, would let the runtime alias direct
 * memory the way the PC port's memfd does. */
__attribute__((unused)) static void test_transfer_memory(void) {
    out("== transfer memory mapped into this process (aliasing)");
    static const struct { Permission perm; const char *name; } perms[] = {
        {Perm_None, "owner none"}, {Perm_R, "owner r"}, {Perm_Rw, "owner rw"},
    };
    for (size_t i = 0; i < sizeof perms / sizeof perms[0]; i++) {
        u8 *buf = aligned_alloc(0x4000, 4 * MIB);
        if (!buf) { out("alloc failed"); return; }
        memset(buf, 0x11, 4 * MIB);
        TransferMemory t;
        Result r = tmemCreateFromMemory(&t, buf, 4 * MIB, perms[i].perm);
        out("%s: create 0x%x; owner perm now %u", perms[i].name, r, perm_at(buf));
        if (R_SUCCEEDED(r)) {
            out("%s: calling tmemMap", perms[i].name);
            r = tmemMap(&t);
            out("%s: map 0x%x at %p", perms[i].name, r, t.map_addr);
            if (R_SUCCEEDED(r)) {
                u8 *view = t.map_addr;
                u32 vp = perm_at(view), op = perm_at(buf);
                out("%s: view perm %u, owner perm %u (1 r, 2 w)", perms[i].name, vp, op);
                if (vp & Perm_W) view[100] = 0x22;
                if ((vp & Perm_W) && (op & Perm_R))
                    out("%s: owner sees 0x%02x after the view wrote 0x22 (0x22 = aliasing works)", perms[i].name,
                        ((volatile u8 *)buf)[100]);
                if ((op & Perm_W) && (vp & Perm_R)) {
                    buf[200] = 0x33;
                    out("%s: view sees 0x%02x after the owner wrote 0x33", perms[i].name, ((volatile u8 *)view)[200]);
                }
                out("%s: unmap 0x%x", perms[i].name, tmemUnmap(&t));
            }
            out("%s: close 0x%x", perms[i].name, tmemCloseHandle(&t));
            out("%s: wait for owner rw 0x%x", perms[i].name, tmemWaitForPermission(&t, Perm_Rw));
        }
        free(buf);
    }
}

/* Faults in physical-mapped pool memory (shadPS4 would watch pages like these), and a guarded
 * read that recovers with longjmp. */
__attribute__((unused)) static void test_fault_more(void) {
    out("== fault in pool memory, then a longjmp-recovered fault");
    u64 alias = info(InfoType_AliasRegionAddress);
    u8 *page = (u8 *)(alias + 16 * GIB);
    if (R_SUCCEEDED(svcMapPhysicalMemory(page, 2 * MIB))) {
        Result pr = svcSetMemoryPermission(page, 0x4000, Perm_R);
        if (R_SUCCEEDED(pr)) {
            int before = fault_count;
            fault_page = page; fault_page_size = 0x4000;
            store_byte(page + 8, 3);
            fault_page = NULL;
            out("pool page fault handled: %d fault(s), value %u", fault_count - before, page[8]);
        }
        u8 v = 0;
        svcSetMemoryPermission(page, 0x4000, Perm_None);
        GUARDED("read of a no-access page", v = ((volatile u8 *)page)[0]);
        out("guarded read returned (value %u)", v);
        svcSetMemoryPermission(page, 0x4000, Perm_Rw);
        svcUnmapPhysicalMemory(page, 2 * MIB);
    }
}

/* The first test of the run: one guarded read of a no-access page. The handler logs "handler
 * entered" before anything else, so the log tells whether the exception reached this NRO. */
static void test_first_fault(void) {
    out("== first: one guarded read of a no-access heap page");
    u8 *page = aligned_alloc(0x4000, 0x4000);
    if (!page) { out("alloc failed"); return; }
    Result r = svcSetMemoryPermission(page, 0x4000, Perm_None);
    out("set no-access: 0x%x; reading it now", r);
    u8 v = 0;
    GUARDED("read of a no-access page", v = ((volatile u8 *)page)[0]);
    out("after the guarded read (value %u); handler entries so far: %d", v, handler_entries);
    svcSetMemoryPermission(page, 0x4000, Perm_Rw);
    free(page);
}

__attribute__((unused)) static void test_fault(void) {
    out("== svcSetMemoryPermission + user exception recovery");
    u8 *page = aligned_alloc(0x4000, 0x4000);
    if (!page) { out("alloc failed"); return; }
    memset(page, 0, 0x4000);
    Result r = svcSetMemoryPermission(page, 0x4000, Perm_R);
    out("set read-only: 0x%x", r);
    if (R_FAILED(r)) { free(page); return; }
    fault_page = page; fault_page_size = 0x4000;
    out("writing to read-only page (if the log ends here, see the screen for the crash)");
    u64 t0 = armGetSystemTick();
    store_byte(page + 100, 7);
    u64 t1 = armGetSystemTick();
    out("fault handled: %d fault(s) at +0x%lx, value %u", fault_count, fault_addr - (u64)page, page[100]);
    out("first fault: %.1f us to handler, %.1f us round trip",
        (double)(fault_tick - t0) * 1e6 / armGetSystemTickFreq(), (double)(t1 - t0) * 1e6 / armGetSystemTickFreq());

    const int rounds = 1000;
    int before = fault_count;
    t0 = armGetSystemTick();
    for (int i = 0; i < rounds; i++) {
        svcSetMemoryPermission(page, 0x4000, Perm_R);
        store_byte(page + (i & 0xfff), (u8)i);
    }
    out("protect+fault+unprotect: %.2f us each (%d faults)", ms_since(t0) * 1000.0 / rounds, fault_count - before);
    t0 = armGetSystemTick();
    for (int i = 0; i < rounds; i++) {
        svcSetMemoryPermission(page, 0x4000, Perm_R);
        svcSetMemoryPermission(page, 0x4000, Perm_Rw);
    }
    out("protect+unprotect without a fault: %.2f us each", ms_since(t0) * 1000.0 / rounds);
    fault_page = NULL;
    free(page);
}

static void test_jit(void) {
    out("== kaiHEN JIT memory cost");
    u64 before = info(InfoType_UsedMemorySize);
    Jit jit;
    Result r = jitCreate(&jit, 128 * MIB);
    out("jitCreate 128 MiB: 0x%x, used memory +%lu MiB", r, (info(InfoType_UsedMemorySize) - before) / MIB);
    if (R_SUCCEEDED(r)) out("jitClose: 0x%x", jitClose(&jit));
}

/* 8 dependent adds per iteration: one cycle each, so adds per second is the clock. */
static u64 add_chain(u64 n) {
    u64 x = 0;
    __asm__ volatile("1:\n"
                     "add %0, %0, #1\n add %0, %0, #1\n add %0, %0, #1\n add %0, %0, #1\n"
                     "add %0, %0, #1\n add %0, %0, #1\n add %0, %0, #1\n add %0, %0, #1\n"
                     "subs %1, %1, #1\n b.ne 1b\n" : "+r"(x), "+r"(n) :: "cc");
    return x;
}

#define CHAIN_ITERS UINT64_C(100000000)
static void chain_thread(void *arg) { *(volatile u64 *)arg = add_chain(CHAIN_ITERS); }

__attribute__((unused)) static void test_cpu(void) { /* run 2: ~995 MHz, 6 cores in parallel */
    out("== cpu");
    u64 t0 = armGetSystemTick();
    add_chain(CHAIN_ITERS);
    double ms = ms_since(t0);
    out("one core: %.1f ms for 800M dependent adds, about %.0f MHz", ms, 800.0 / ms * 1000.0);

    Thread threads[6];
    volatile u64 sinks[6];
    int started = 0;
    t0 = armGetSystemTick();
    for (int core = 0; core < 6; core++) {
        if (!(info(InfoType_CoreMask) & (1u << core))) continue;
        if (R_FAILED(threadCreate(&threads[started], chain_thread, (void *)&sinks[started], NULL, 0x10000, 0x2c, core)))
            continue;
        threadStart(&threads[started]);
        started++;
    }
    for (int i = 0; i < started; i++) { threadWaitForExit(&threads[i]); threadClose(&threads[i]); }
    ms = ms_since(t0);
    out("%d cores at once: %.1f ms (same as one core means they all ran in parallel)", started, ms);
}

int main(void);
void __libnx_exception_entry(void);

/* Lists every code module and saves the loader's code (text + rodata) to sdmc:/gr_code_N.bin, so
 * its exception entry can be disassembled offline. Nothing faults here. */
static void scan_modules(void) {
    out("== code modules (this NRO: main at %p, exception entry at %p)", (void *)&main,
        (void *)&__libnx_exception_entry);
    MemoryInfo mi, ro; u32 pi;
    u64 addr = 0, end = info(InfoType_AddressSpaceSize), self = (u64)&main;
    int n = 0, dumped = 0;
    while (addr < end && R_SUCCEEDED(svcQueryMemory(&mi, &pi, addr)) && n++ < 4096) {
        u64 next = mi.addr + mi.size;
        if (next <= addr) break;
        if ((mi.type == MemType_CodeStatic || mi.type == MemType_CodeMutable) && mi.perm == Perm_Rx) {
            u64 ro_size = 0;
            if (R_SUCCEEDED(svcQueryMemory(&ro, &pi, next)) && ro.addr == next && ro.perm == Perm_R) ro_size = ro.size;
            int is_self = self >= mi.addr && self < next;
            u64 scan = ro_size < 8 * MIB ? ro_size : 8 * MIB;
            int hb = scan && memmem((void *)next, scan, "hbloader", 8) != NULL;
            out("code 0x%lx +0x%lx type 0x%x, rodata +0x%lx%s%s", mi.addr, mi.size, mi.type, ro_size,
                is_self ? " <- this NRO" : "", hb ? " <- mentions hbloader" : "");
            if (!is_self && mi.size + ro_size <= 6 * MIB && dumped < 8) {
                char name[64];
                snprintf(name, sizeof name, "sdmc:/gr_code_%d.bin", dumped);
                FILE *f = fopen(name, "wb");
                if (f) {
                    size_t w = fwrite((void *)mi.addr, 1, mi.size + ro_size, f);
                    fclose(f);
                    out("  saved %s (%lu bytes, rodata starts at +0x%lx)", name, (u64)w, mi.size);
                    dumped++;
                }
            }
        }
        addr = next;
    }
}

int main(void) {
    consoleInit(NULL);
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    PadState pad;
    padInitializeDefault(&pad);
    /* v5's hold-B check never saw the button, so both runs took the fault path. Ask instead. */
    printf("gr_probe v6\n\n"
           "  A: safe run   (no deliberate faults, log sdmc:/gr_probe_safe.txt)\n"
           "  X: fault run  (one deliberate fault, log sdmc:/gr_probe_fault.txt)\n"
           "  +: exit\n");
    int faults = -1;
    while (faults < 0 && appletMainLoop()) {
        padUpdate(&pad);
        u64 d = padGetButtonsDown(&pad);
        if (d & HidNpadButton_A) faults = 0;
        else if (d & HidNpadButton_X) faults = 1;
        else if (d & HidNpadButton_Plus) { consoleExit(NULL); return 0; }
        consoleUpdate(NULL);
    }
    if (faults < 0) { consoleExit(NULL); return 0; }
    logf = fopen(faults ? "sdmc:/gr_probe_fault.txt" : "sdmc:/gr_probe_safe.txt", "w");

    out("gr_probe v6, %s run", faults ? "FAULT" : "SAFE");
    out("loader: %.*s", (int)envGetLoaderInfoSize(), envGetLoaderInfo() ? envGetLoaderInfo() : "?");
    out("program id 0x%016lx, debugger attached: %lu (1 = the kernel sends exceptions to a debugger, not to us)",
        info(InfoType_ProgramId), debugger_attached());
    out("user exception context address: 0x%lx", info(InfoType_UserExceptionContextAddress));
    if (faults) {
        out("on a crash, time the error screen after \"reading it now\": immediate = the exception never");
        out("reached this NRO, ~5 s = it reached the entry but not the handler, ~10 s = the handler ran");
        test_first_fault();
        out("== done. Press + to exit.");
    } else {
        test_regions();
        scan_modules();
        shrink_heap();
        test_jit();
        test_map_memory();
        test_map_physical(); /* first run with the heap shrunk */
        /* transfer memory is skipped: run 3 died inside tmemMap */
        restore_heap();
        out("== done. Press + to exit.");
    }

    while (appletMainLoop()) {
        padUpdate(&pad);
        if (padGetButtonsDown(&pad) & HidNpadButton_Plus) break;
        consoleUpdate(NULL);
    }
    if (logf) fclose(logf);
    consoleExit(NULL);
    return 0;
}
