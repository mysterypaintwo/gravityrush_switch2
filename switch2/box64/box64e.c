/* box64e: Box64 embedded in the Gravity Rush ARM64 host (see box64e.h). This plays the part of
 * Box64's own main()/core.c (Linux) or wowbox64.c (Windows WoW64) for a host that loads the
 * guest itself. */
#include <pthread.h>
#include <stddef.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#include "box64e.h"

#include "debug.h"
#include "os.h"
#include "env.h"
#include "custommem.h"
#include "dynablock.h"
#include "box64context.h"
#include "box64cpu.h"
#include "box64cpu_util.h"
#include "emu/x64emu_private.h"
#include "emu/x87emu_private.h"
#include "x64emu.h"
#include "build_info.h"
#include "hostext.h"
#include "rbtree.h"
#include "sysinfo.h"
#include "x64trace.h"

/* --- globals that Box64's main program normally defines */
uintptr_t box64_pagesize = 4096;
uint32_t default_gs = 0;
uint32_t default_fs = 0;
/* RDTSC calls ReadTSC (os_embed.c) instead of reading the counter inline, so the time the
 * console spent suspended (HOME menu, sleep) can be taken out of the guest's clock. */
int box64_rdtsc = 1;
volatile uint64_t* box64e_tsc_offset;
uint8_t box64_rdtsc_shift = 0;
int box64_is32bits = 0;
int box64_unittest_mode = 0;
int box64_wine = 0;
int box64_nolibs = 1;
sysinfo_t box64_sysinfo = {0};
cpu_ext_t cpuext = {0};

static box64context_t box64_context;
box64context_t* my_context = &box64_context;

/* Guest memory is ordinary memory here: no unaligned-only or auto-SMC ranges. */
int is_addr_unaligned(uintptr_t addr) { (void)addr; return 0; }
int is_addr_autosmc(uintptr_t addr) { (void)addr; return 0; }
int nUnalignedRange(uintptr_t start, size_t size) { (void)start; (void)size; return 0; }
void getUnalignedRange(uintptr_t start, size_t size, uintptr_t addrs[]) { (void)start; (void)size; (void)addrs; }

/* --- bridges */
typedef void (*wrapper_t)(x64emu_t* emu, uintptr_t fnc);

typedef float gr_v4f __attribute__((vector_size(16)));
typedef struct { uint64_t rax, rdx; } gr_ret2;

#define XMM(i) (*(gr_v4f*)&emu->xmm[i])

/* --- call tracing (diagnostics, GR_TRACE_BRIDGES=1): each guest thread's last system call,
 * its call count, and whether it is inside it now; box64e_trace_report prints them. The
 * dynarec's direct calls to simple wrappers are off then, so every call passes a wrapper. */
#include <stdatomic.h>
#define TRACE_SLOTS 128
typedef struct {
    _Atomic uintptr_t fn;
    _Atomic uint64_t calls;
    _Atomic int inside;
    uint64_t reported;
    uint64_t host;      /* box64e_thread_host(): the host thread, for sampling */
    int still;          /* reports in a row with no call (in guest code) */
    char name[32];
} trace_slot_t;
static trace_slot_t trace_slots[TRACE_SLOTS];
static atomic_int trace_used;
static int trace_on = -1;
static _Thread_local int trace_index = -1;
const char* (*box64e_thread_name)(void);
uint64_t (*box64e_thread_host)(void);
uintptr_t box64e_trace_image;
static int tracing(void)
{
    if (trace_on < 0) {
        const char* v = getenv("GR_TRACE_BRIDGES");
        trace_on = v && v[0] == '1';
    }
    return trace_on;
}
static trace_slot_t* trace_enter(uintptr_t fnc)
{
    if (trace_on != 1 && !tracing()) return NULL;
    if (trace_index < 0) {
        const int i = atomic_fetch_add(&trace_used, 1);
        if (i >= TRACE_SLOTS) { trace_index = TRACE_SLOTS; return NULL; }
        const char* n = box64e_thread_name ? box64e_thread_name() : NULL;
        snprintf(trace_slots[i].name, sizeof trace_slots[i].name, "%s", n ? n : "?");
        trace_slots[i].host = box64e_thread_host ? box64e_thread_host() : 0;
        trace_index = i;
    }
    if (trace_index >= TRACE_SLOTS) return NULL;
    trace_slot_t* t = &trace_slots[trace_index];
    atomic_store(&t->fn, fnc);
    atomic_fetch_add(&t->calls, 1);
    atomic_store(&t->inside, 1);
    return t;
}
static void trace_leave(trace_slot_t* t)
{
    if (t) atomic_store(&t->inside, 0);
}

static void wrap_int(x64emu_t* emu, uintptr_t fnc)
{
    trace_slot_t* traced = trace_enter(fnc);
    gr_ret2 (*f)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t,
                 gr_v4f, gr_v4f, gr_v4f, gr_v4f, gr_v4f, gr_v4f, gr_v4f, gr_v4f) = (void*)fnc;
    gr_ret2 r = f(R_RDI, R_RSI, R_RDX, R_RCX, R_R8, R_R9,
                  XMM(0), XMM(1), XMM(2), XMM(3), XMM(4), XMM(5), XMM(6), XMM(7));
    R_RAX = r.rax;
    R_RDX = r.rdx;
    trace_leave(traced);
}

static void wrap_fp(x64emu_t* emu, uintptr_t fnc)
{
    trace_slot_t* traced = trace_enter(fnc);
    gr_v4f (*f)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t,
                gr_v4f, gr_v4f, gr_v4f, gr_v4f, gr_v4f, gr_v4f, gr_v4f, gr_v4f) = (void*)fnc;
    XMM(0) = f(R_RDI, R_RSI, R_RDX, R_RCX, R_R8, R_R9,
               XMM(0), XMM(1), XMM(2), XMM(3), XMM(4), XMM(5), XMM(6), XMM(7));
    trace_leave(traced);
}

/* Arguments 7-12 are at [rsp+8]... (rsp points at the return address). Integer arguments 7 and 8
 * go in x6/x7 and the rest on the native stack in 8-byte slots, which is right for 64-bit and
 * pointer arguments on every host (macOS packs smaller stack arguments). */
static void wrap_stack(x64emu_t* emu, uintptr_t fnc)
{
    trace_slot_t* traced = trace_enter(fnc);
    const uint64_t* s = (const uint64_t*)(R_RSP + 8);
    gr_ret2 (*f)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t,
                 uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t,
                 gr_v4f, gr_v4f, gr_v4f, gr_v4f, gr_v4f, gr_v4f, gr_v4f, gr_v4f) = (void*)fnc;
    gr_ret2 r = f(R_RDI, R_RSI, R_RDX, R_RCX, R_R8, R_R9, s[0], s[1], s[2], s[3], s[4], s[5],
                  XMM(0), XMM(1), XMM(2), XMM(3), XMM(4), XMM(5), XMM(6), XMM(7));
    R_RAX = r.rax;
    R_RDX = r.rdx;
    trace_leave(traced);
}

static void wrap_emu(x64emu_t* emu, uintptr_t fnc)
{
    trace_slot_t* traced = trace_enter(fnc);
    ((void (*)(x64emu_t*))fnc)(emu);
    trace_leave(traced);
}

/* The dynarec calls a "simple" wrapper's function directly: 1 + the number of xmm arguments
 * to load, negative when the result is in xmm0 (dynarec_arm64_helper.c call_n). */
int isSimpleWrapper(wrapper_t fun)
{
    if (tracing()) return 0;
    if (fun == wrap_int) return 1 + 8;
    if (fun == wrap_fp) return -(1 + 8);
    return 0;
}
int isRetX87Wrapper(wrapper_t fun) { (void)fun; return 0; }

#pragma pack(push, 1)
typedef struct {
    uint8_t cc, s, c;
    wrapper_t w;
    uintptr_t fn;
    uint8_t ret;
    uint8_t pad[12];
} gr_bridge_t;
#pragma pack(pop)
_Static_assert(sizeof(gr_bridge_t) == 32, "one bridge is 32 bytes");

typedef struct {
    uintptr_t addr;
    const char* name;
    uintptr_t fn;
} bridge_name_t;

#define BRIDGE_PAGE (64 * 1024)
static pthread_mutex_t bridge_lock = PTHREAD_MUTEX_INITIALIZER;
static gr_bridge_t* bridge_cur;
static gr_bridge_t* bridge_end;
static bridge_name_t* bridge_names;
static size_t bridge_count, bridge_cap;
static uintptr_t exit_bridge;

static uintptr_t add_bridge(wrapper_t w, void* fn, const char* name)
{
    pthread_mutex_lock(&bridge_lock);
    if (bridge_cur == bridge_end) {
        void* page = mmap(NULL, BRIDGE_PAGE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
        if (page == MAP_FAILED) {
            pthread_mutex_unlock(&bridge_lock);
            return 0;
        }
        /* The x86 bytes are only read by Box64, never run natively, but Box64 only translates
         * guest memory it believes executable. */
        setProtection((uintptr_t)page, BRIDGE_PAGE, PROT_READ | PROT_EXEC);
        bridge_cur = page;
        bridge_end = (gr_bridge_t*)((char*)page + BRIDGE_PAGE);
    }
    gr_bridge_t* b = bridge_cur++;
    b->cc = 0xcc;
    b->s = 'S';
    b->c = 'C';
    b->w = w;
    b->fn = (uintptr_t)fn;
    b->ret = 0xc3;
    memset(b->pad, 0xcc, sizeof b->pad);
    if (bridge_count == bridge_cap) {
        bridge_cap = bridge_cap ? bridge_cap * 2 : 1024;
        bridge_names = realloc(bridge_names, bridge_cap * sizeof *bridge_names);
    }
    bridge_names[bridge_count++] = (bridge_name_t){(uintptr_t)b, name, (uintptr_t)fn};
    pthread_mutex_unlock(&bridge_lock);
    return (uintptr_t)b;
}

uintptr_t box64e_bridge(void* fn, box64e_kind kind, const char* name)
{
    wrapper_t w = kind == BOX64E_FP ? wrap_fp : kind == BOX64E_STACK ? wrap_stack : wrap_int;
    return add_bridge(w, fn, name);
}

uintptr_t box64e_bridge_emu(void (*fn)(x64emu_t* emu), const char* name)
{
    return add_bridge(wrap_emu, (void*)fn, name);
}

static const char* name_of_fn(uintptr_t fn)
{
    for (size_t i = 0; i < bridge_count; ++i)
        if (bridge_names[i].fn == fn) return bridge_names[i].name ? bridge_names[i].name : "?";
    return "?";
}

/* x86 code address of a native address in translated code (0 when it is not). */
static uintptr_t x64_of(uintptr_t native)
{
    dynablock_t* db = FindDynablockFromNativeAddress((void*)native);
    return db ? getX64Address(db, native) : 0;
}

/* The traced thread name of a host thread, or NULL. */
const char* box64e_trace_name_of(uint64_t host)
{
    int used = atomic_load(&trace_used);
    if (used > TRACE_SLOTS) used = TRACE_SLOTS;
    for (int i = 0; i < used; ++i)
        if (trace_slots[i].host == host) return trace_slots[i].name;
    return NULL;
}

void (*box64e_trace_native)(uint64_t host, const uint64_t regs[34], void (*line)(const char*));

void box64e_trace_report(void (*line)(const char*), int (*sample)(uint64_t host, uint64_t regs[34]))
{
    if (!tracing()) return;
    int used = atomic_load(&trace_used);
    if (used > TRACE_SLOTS) used = TRACE_SLOTS;
    char out[240];
    for (int i = 0; i < used; ++i) {
        trace_slot_t* t = &trace_slots[i];
        const uint64_t calls = atomic_load(&t->calls);
        const int inside = atomic_load(&t->inside);
        const char* state = inside ? "IN" : (calls == t->reported ? "guest code since" : "after");
        pthread_mutex_lock(&bridge_lock);
        snprintf(out, sizeof out, "trace %-28s %s %s (%llu calls, +%llu)", t->name, state,
                 name_of_fn(atomic_load(&t->fn)), (unsigned long long)calls,
                 (unsigned long long)(calls - t->reported));
        pthread_mutex_unlock(&bridge_lock);
        t->still = !inside && calls == t->reported ? t->still + 1 : 0;
        t->reported = calls;
        line(out);
        /* In guest code for a whole period: where (3 samples), and its first 8 registers. */
        if (t->still >= 1 && sample && t->host) {
            for (int k = 0; k < 3; ++k) {
                uint64_t r[34];
                if (sample(t->host, r)) break;
                const uintptr_t x = x64_of((uintptr_t)r[31]);
                if (!x && box64e_trace_native) { /* host code: a backtrace instead */
                    box64e_trace_native(t->host, r, line);
                    break;
                }
                const long long rel = x && box64e_trace_image ? (long long)(x - box64e_trace_image) : -1;
                snprintf(out, sizeof out, "  x86 %#llx (image +%#llx) native %#llx; rax %#llx rcx %#llx rdx %#llx rbx %#llx rsp %#llx rbp %#llx rsi %#llx rdi %#llx",
                         (unsigned long long)x, rel, (unsigned long long)r[31], (unsigned long long)r[10],
                         (unsigned long long)r[11], (unsigned long long)r[12], (unsigned long long)r[13],
                         (unsigned long long)r[14], (unsigned long long)r[15], (unsigned long long)r[16],
                         (unsigned long long)r[17]);
                line(out);
            }
        }
    }
}

const char* box64e_bridge_name(uintptr_t guest_addr)
{
    const char* name = NULL;
    pthread_mutex_lock(&bridge_lock);
    for (size_t i = 0; i < bridge_count; i++)
        if (guest_addr >= bridge_names[i].addr && guest_addr < bridge_names[i].addr + sizeof(gr_bridge_t)) {
            name = bridge_names[i].name;
            break;
        }
    pthread_mutex_unlock(&bridge_lock);
    return name;
}


/* --- setup */
int box64e_init(const box64e_jit* jit)
{
    long page = sysconf(_SC_PAGESIZE);
    box64_pagesize = page > 0 ? (uintptr_t)page : 4096;

    LoadEnvVariables();
    InitializeSystemInfo();
    /* Features that need signal delivery are off: CALLRET patches live code and recovers
     * through SIGILL, the dynacache maps blocks back from disk, strongmem is tuned later. */
    SET_BOX64ENV(dynarec_callret, 0);
    SET_BOX64ENV(dynacache, 0);
    if (!DetectHostCpuFeatures()) SET_BOX64ENV(dynarec, 0);
    if (!BOX64ENV(nobanner)) PrintBox64Version(1);
    PrintHostCpuFeatures();

    box64_jit_wdelta = jit ? jit->wdelta : 0;

    init_custommem_helper(&box64_context);
    box64_context.db_sizes = rbtree_init("db_sizes");
    pthread_mutex_init(&box64_context.mutex_dyndump, NULL);
    pthread_mutex_init(&box64_context.mutex_trace, NULL);
    pthread_mutex_init(&box64_context.mutex_tls, NULL);
    pthread_mutex_init(&box64_context.mutex_thread, NULL);
    pthread_mutex_init(&box64_context.mutex_bridge, NULL);
    pthread_mutex_init(&box64_context.mutex_lock, NULL);
    InitX64Trace(&box64_context);

    /* DynaCall's return address (PushExit): a bridge with a null wrapper makes the emulator quit. */
    exit_bridge = add_bridge(NULL, NULL, "ExitEmulation");
    box64_context.exit_bridge = exit_bridge;
    return exit_bridge ? 0 : -1;
}

void box64e_set_protection(uintptr_t addr, size_t size, int prot)
{
    setProtection(addr, size, (uint32_t)prot);
}

void box64e_invalidate(uintptr_t addr, size_t size)
{
    cleanDBFromAddressRange(addr, size, 1);
}

/* --- per-thread emulators */
static pthread_key_t emu_key;
static pthread_once_t emu_once = PTHREAD_ONCE_INIT;

typedef struct {
    x64emu_t* emu;
    void* own_stack;
} thread_emu_t;

static void emu_destroy(void* p)
{
    thread_emu_t* t = p;
    if (!t) return;
    if (t->emu) FreeX64Emu(&t->emu);
    free(t->own_stack);
    free(t);
}

static void emu_key_init(void) { pthread_key_create(&emu_key, emu_destroy); }

#define DEFAULT_GUEST_STACK (1024 * 1024)

x64emu_t* box64e_thread_emu(void)
{
    pthread_once(&emu_once, emu_key_init);
    thread_emu_t* t = pthread_getspecific(emu_key);
    if (t) return t->emu;
    t = calloc(1, sizeof *t);
    t->own_stack = aligned_alloc(16, DEFAULT_GUEST_STACK);
    t->emu = NewX64Emu(my_context, 0, (uintptr_t)t->own_stack, DEFAULT_GUEST_STACK, 0);
    pthread_setspecific(emu_key, t);
    return t->emu;
}

void box64e_set_stack(x64emu_t* emu, void* stack_base, size_t stack_size)
{
    emu->init_stack = stack_base;
    emu->size_stack = stack_size;
    R_RSP = ((uintptr_t)stack_base + stack_size) & ~(uintptr_t)15;
}

void box64e_set_segment_bases(x64emu_t* emu, uint64_t fs_base, uint64_t gs_base)
{
    emu->segs_offs[_FS] = fs_base;
    emu->segs_offs[_GS] = gs_base;
}

void box64e_thread_exit(void)
{
    pthread_once(&emu_once, emu_key_init);
    thread_emu_t* t = pthread_getspecific(emu_key);
    pthread_setspecific(emu_key, NULL);
    emu_destroy(t);
}

/* --- calling guest code (Box64's RunFunctionWithEmu) */
static uint64_t call_guest(x64emu_t* emu, uintptr_t fn, int nargs, const uint64_t* args)
{
    static const int regs[] = {_DI, _SI, _DX, _CX, _R8, _R9};
    int stackn = nargs > 6 ? nargs - 6 : 0;
    int align = stackn & 1;

    Push64(emu, R_RBP);
    R_RBP = R_RSP;
    R_RSP -= (stackn + align) * sizeof(uint64_t);
    uint64_t* p = (uint64_t*)R_RSP;
    for (int i = 0; i < nargs; i++) {
        if (i < 6) emu->regs[regs[i]].q[0] = args[i];
        else *p++ = args[i];
    }
    uintptr_t old_ip = R_RIP;
    int old_quit = emu->quit;
    emu->quit = 0;
    DynaCall(emu, fn, 0);
    if (old_ip == R_RIP) {
        R_RSP = R_RBP;
        R_RBP = Pop64(emu);
    }
    emu->quit = old_quit;
    return R_RAX;
}

uint64_t box64e_call_guest(uintptr_t fn, int nargs, ...)
{
    uint64_t args[16];
    va_list va;
    if (nargs > 16) nargs = 16;
    va_start(va, nargs);
    for (int i = 0; i < nargs; i++) args[i] = va_arg(va, uint64_t);
    va_end(va);
    return call_guest(box64e_thread_emu(), fn, nargs, args);
}

uint64_t box64e_call_guest_args(uintptr_t fn, int nargs, const uint64_t* args)
{
    return call_guest(box64e_thread_emu(), fn, nargs > 16 ? 16 : nargs, args);
}

void box64e_set_mxcsr(x64emu_t* emu, uint32_t mxcsr)
{
    emu->mxcsr.x32 = mxcsr;
}

uintptr_t box64e_guest_address(uintptr_t native_pc)
{
    dynablock_t* db = FindDynablockFromNativeAddress((void*)native_pc);
    return db ? getX64Address(db, native_pc) : 0;
}

uint64_t box64e_call_guest_fp(uintptr_t fn, int nargs, const uint64_t* args, int nfp, const double* fp_args,
                              double* fp_ret)
{
    x64emu_t* emu = box64e_thread_emu();
    for (int i = 0; i < nfp && i < 8; i++) emu->xmm[i].d[0] = fp_args[i];
    uint64_t r = call_guest(emu, fn, nargs, args);
    if (fp_ret) *fp_ret = emu->xmm[0].d[0];
    return r;
}

int box64e_guest_error(void) { return box64e_thread_emu()->error; }

void box64e_set_dynarec(int on) { box64env.dynarec = on; }

void box64e_set_dynarec_test(int on, uintptr_t start, uintptr_t end)
{
    /* Plain fields (no override bookkeeping): the passes read them when a block is translated. */
    box64env.dynarec_test_start = start;
    box64env.dynarec_test_end = end;
    box64env.dynarec_test = on;
}

/* --- guest CPU state: everything in x64emu_t before the parity table pointer */
size_t box64e_state_size(void) { return offsetof(x64emu_t, x64emu_parity_tab); }
void box64e_save_state(x64emu_t* emu, void* buffer) { memcpy(buffer, emu, box64e_state_size()); }
void box64e_load_state(x64emu_t* emu, const void* buffer) { memcpy(emu, buffer, box64e_state_size()); }

/* --- register access */
uint64_t box64e_reg(x64emu_t* emu, int reg) { return emu->regs[reg & 15].q[0]; }
void box64e_set_reg(x64emu_t* emu, int reg, uint64_t value) { emu->regs[reg & 15].q[0] = value; }
uint64_t box64e_xmm_lo(x64emu_t* emu, int reg) { return emu->xmm[reg & 15].q[0]; }
void box64e_set_xmm_lo(x64emu_t* emu, int reg, uint64_t value) { emu->xmm[reg & 15].q[0] = value; }
uint64_t box64e_rip(x64emu_t* emu) { return R_RIP; }

void box64e_dump(x64emu_t* emu, const char* why)
{
    static const char* names[] = {"rax", "rcx", "rdx", "rbx", "rsp", "rbp", "rsi", "rdi",
                                  "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15"};
    fprintf(stderr, "guest state (%s): rip %016llx\n", why, (unsigned long long)R_RIP);
    for (int i = 0; i < 16; i++)
        fprintf(stderr, "  %-3s %016llx%s", names[i], (unsigned long long)emu->regs[i].q[0], (i & 3) == 3 ? "\n" : "");
    const uint64_t* s = (const uint64_t*)R_RSP;
    if (s)
        for (int i = 0; i < 4; i++) fprintf(stderr, "  [rsp+%02x] %016llx\n", i * 8, (unsigned long long)s[i]);
}
