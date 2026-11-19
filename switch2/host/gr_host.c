/* Gravity Rush ARM64 host: the PS4 game's x86-64 code under the embedded Box64 dynarec
 * (switch2/box64), with the PC port's HLE runtime (src/runtime*.c) behind bridges.
 *
 * Same boot file and options as the PC loader (src/probe.c), which jumps into the game
 * natively; here every jump into game code goes through box64e_call_guest, every import the
 * runtime resolves becomes a bridge, and the guest TLS base is a Box64 segment base. */
#include <inttypes.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <execinfo.h>
#include <dlfcn.h>
#ifdef __APPLE__
#include <sys/ucontext.h>
#endif

#include "runtime.h"
#include "gpu/gow3gpu.h"
#include "box64e.h"

typedef struct { uint64_t address, size, flags; } Segment;
typedef struct { uint64_t target, kind, value, addend; } Reloc;
typedef struct { uint64_t base, size, init, tls_address, tls_memsz, tls_filesz, tls_module; } LinkedModule;

static char (*names)[128];
static uint64_t import_count;
static unsigned char *image;
static uint64_t image_size;
static unsigned char *data_traps;
static size_t page_size;
static LinkedModule modules[16];
static uint64_t module_count;
static int entered_game;

unsigned char *runtime_image_base(void) { return image; }
/* For the GPU library's calls through game-supplied function pointers (gow3_guest_call.h). */
int runtime_is_guest_code(uintptr_t address) {
    return image && address >= (uintptr_t)image && address - (uintptr_t)image < image_size;
}

#ifdef __SWITCH__
void gr_crumb(const char *what); /* gr_switch.c: boot.txt, kept through a crash */
#define STEP(what) gr_crumb(what)
#else
#define STEP(what) ((void)0)
#endif
static void fail(const char *message) { fprintf(stderr, "ERROR: %s\n", message); exit(1); }
static uint64_t read64(FILE *f) {
    unsigned char b[8];
    if (fread(b, 1, 8, f) != 8) fail("truncated boot file");
    uint64_t n = 0;
    for (int i = 7; i >= 0; --i) n = (n << 8) | b[i];
    return n;
}
static size_t round_page(size_t size) { return (size + page_size - 1) & ~(page_size - 1); }
static void *allocate(size_t size) {
    void *p = runtime_low_map(size, PROT_READ | PROT_WRITE);
    if (!p) fail("cannot map guest memory below 64 GiB");
    return p;
}
static int mapped(Segment *segments, uint64_t count, uint64_t address, uint64_t bytes) {
    for (uint64_t i = 0; i < count; ++i)
        if (address >= segments[i].address && bytes <= segments[i].size &&
            address - segments[i].address <= segments[i].size - bytes) return 1;
    return 0;
}

/* --- runtime hooks (runtime.h, ARM64 hosts) */
uint64_t runtime_guest_call(uintptr_t fn, int nargs, ...) {
    uint64_t args[16];
    va_list va;
    if (nargs > 16) nargs = 16;
    va_start(va, nargs);
    for (int i = 0; i < nargs; i++) args[i] = va_arg(va, uint64_t);
    va_end(va);
    uint64_t r = box64e_call_guest_args(fn, nargs, args);
    if (box64e_guest_error()) {
        box64e_dump(box64e_thread_emu(), "guest call stopped on an error");
        fflush(NULL);
        _exit(22);
    }
    return r;
}
void runtime_guest_set_tls_base(void *tcb) {
    /* link_modules.py rewrote fs:[0] loads to gs:[0]; FS gets the same base for any it missed. */
    box64e_set_segment_bases(box64e_thread_emu(), (uint64_t)(uintptr_t)tcb, (uint64_t)(uintptr_t)tcb);
}
void runtime_guest_set_mxcsr(uint32_t mxcsr) { box64e_set_mxcsr(box64e_thread_emu(), mxcsr); }

/* --- imports */
/* Functions with more than 6 integer arguments, whose bridges read arguments 7-12 from the guest
 * stack: the runtime's (src/runtime*.c) and the GPU library's (gpu/shadps4/core/libraries).
 * A function missing here gets 0 for those arguments. */
static const char *const stack_functions[] = {
    /* runtime */
    "_sceFiberInitializeImpl", "sceAjmBatchJobControlBufferRa", "sceAjmBatchJobRunBufferRa",
    "sceAjmBatchJobRunSplitBufferRa", "sceKernelMapNamedDirectMemory",
    /* GPU library */
    "sceFontMemoryInit", "sceGnmDrawIndexIndirect", "sceGnmDrawIndexIndirectCountMulti",
    "sceGnmDrawIndexIndirectMulti", "sceGnmDrawIndirect", "sceGnmDrawIndirectMulti",
    "sceGnmRegisterResource", "sceGnmSubmitAndFlipCommandBuffers",
    "sceGnmSubmitAndFlipCommandBuffersForWorkload", "sceVideoOutSetBufferAttribute",
};
static box64e_kind import_kind(const char *nid) {
    const char *api = runtime_import_name(nid);
    for (size_t i = 0; i < sizeof stack_functions / sizeof *stack_functions; i++)
        if (api && !strcmp(api, stack_functions[i])) return BOX64E_STACK;
    return BOX64E_INT;
}

/* The bridge of an import nothing provides: the first call stops the game, like the PC loader. */
static __attribute__((noreturn)) void unresolved(x64emu_t *emu) {
    const char *name = box64e_bridge_name(box64e_rip(emu));
    uint64_t ret = *(uint64_t *)(uintptr_t)box64e_reg(emu, 4);
    printf("STOP: first unsupported PS4 import: %s\n", name ? name : "?");
    printf("API: %s\n", name ? runtime_import_name(name) : "?");
    printf("Caller return offset: 0x%" PRIx64 "; first argument: 0x%" PRIx64 "\n",
           ret - (uint64_t)(uintptr_t)image, box64e_reg(emu, 7));
    for (uint64_t m = 0; m < module_count; ++m)
        if (ret - (uint64_t)(uintptr_t)image >= modules[m].base && ret - (uint64_t)(uintptr_t)image - modules[m].base < modules[m].size)
            printf("Caller in linked module %" PRIu64 " (%s): +0x%" PRIx64 "\n", m, m == 0 ? "libc.prx" : "system module",
                   ret - (uint64_t)(uintptr_t)image - modules[m].base);
    runtime_report();
    puts(entered_game ? "Original guest entry instructions executed; game initialization is incomplete." :
                        "Native libc initialization is incomplete; game entry has not run.");
    fflush(NULL);
    _exit(20);
}

/* --- faults (development host only: Horizon cannot catch them) */
#ifndef __SWITCH__
static void fault(int sig, siginfo_t *info, void *context) {
#ifdef GR_HOST_GPU
    /* GPU page tracking: the library write-protects guest pages and unprotects them here
     * (macOS raises SIGBUS for these). Horizon cannot catch faults: there the dynarec will have
     * to report guest writes instead (switch2/PLAN.md). */
    if ((sig == SIGSEGV || sig == SIGBUS) && gow3gpu_handle_fault(context, info->si_addr)) return;
#endif
    char line[512];
    uintptr_t pc = 0;
#if defined(__APPLE__)
    pc = (uintptr_t)((ucontext_t *)context)->uc_mcontext->__ss.__pc;
#endif
    uintptr_t addr = (uintptr_t)info->si_addr, guest = box64e_guest_address(pc);
    int n = snprintf(line, sizeof line, "Fault (signal %d) at native pc %p, address %p", sig, (void *)pc, (void *)addr);
    if (guest && image) n += snprintf(line + n, sizeof line - n, "; guest code at %p (image offset 0x%lx)",
                                      (void *)guest, (unsigned long)(guest - (uintptr_t)image));
    if (data_traps && addr >= (uintptr_t)data_traps && addr < (uintptr_t)data_traps + (import_count + 1) * page_size) {
        uint64_t index = (addr - (uintptr_t)data_traps) / page_size;
        if (index < import_count)
            n += snprintf(line + n, sizeof line - n, "; unsupported data import %s (%s)", names[index], runtime_import_name(names[index]));
    }
    snprintf(line + n, sizeof line - n, "\n");
    ssize_t w = write(2, line, strlen(line));
    (void)w;
#if defined(__APPLE__)
    if (runtime_is_guest_code(pc)) {
        /* host code called a game function pointer natively instead of through Box64 */
        uintptr_t lr = (uintptr_t)((ucontext_t *)context)->uc_mcontext->__ss.__lr;
        Dl_info where;
        n = snprintf(line, sizeof line, "  host code jumped into game code (image offset 0x%lx) from %p",
                     (unsigned long)(pc - (uintptr_t)image), (void *)lr);
        if (dladdr((void *)lr, &where) && where.dli_sname)
            n += snprintf(line + n, sizeof line - n, " (%s+0x%lx)", where.dli_sname,
                          (unsigned long)(lr - (uintptr_t)where.dli_saddr));
        snprintf(line + n, sizeof line - n, "\n");
        w = write(2, line, strlen(line));
    }
    if (guest) {
        /* In translated code the guest registers live in native ones (Box64's arm64_mapping.h,
         * r8 moved to x9): rax..rdi = x10..x17, r8 = x9, r9..r15 = x19..x25. */
        const uint64_t *x = ((ucontext_t *)context)->uc_mcontext->__ss.__x;
        static const char *const regs[16] = {"rax", "rcx", "rdx", "rbx", "rsp", "rbp", "rsi", "rdi",
                                             "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15"};
        uint64_t g[16];
        for (int i = 0; i < 16; i++) g[i] = i == 8 ? x[9] : i < 8 ? x[10 + i] : x[10 + i];
        for (int i = 0; i < 16; i++) {
            n = snprintf(line, sizeof line, "  %-3s %016llx%s", regs[i], (unsigned long long)g[i], (i & 3) == 3 ? "\n" : "");
            w = write(2, line, n);
        }
        /* guest frames: [rbp] = caller's rbp, [rbp+8] = return address */
        uint64_t rbp = g[5];
        for (int depth = 0; depth < 16 && rbp && !(rbp & 7); depth++) {
            const uint64_t *frame = (const uint64_t *)(uintptr_t)rbp;
            uint64_t ret = frame[1];
            if (!runtime_is_guest_code((uintptr_t)ret)) break;
            n = snprintf(line, sizeof line, "  guest frame %d: return to image offset 0x%llx\n", depth,
                         (unsigned long long)(ret - (uintptr_t)image));
            w = write(2, line, n);
            if (frame[0] <= rbp) break;
            rbp = frame[0];
        }
    }
#endif
    void *frames[24];
    backtrace_symbols_fd(frames, backtrace(frames, 24), 2);
    _exit(128 + sig);
}
#endif

/* --- param.sfo and patches, as in src/probe.c */
static int sfo_value(const char *path, const char *key, char *text, size_t text_size, uint32_t *number) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    static unsigned char data[65536];
    size_t n = fread(data, 1, sizeof(data), f);
    fclose(f);
    if (n < 20 || memcmp(data, "\0PSF", 4)) return 0;
    uint32_t keys, values, count;
    memcpy(&keys, data + 8, 4); memcpy(&values, data + 12, 4); memcpy(&count, data + 16, 4);
    for (uint32_t i = 0; i < count && 20 + i * 16 + 16 <= n; ++i) {
        const unsigned char *e = data + 20 + i * 16;
        uint16_t key_offset, format; uint32_t length, offset;
        memcpy(&key_offset, e, 2); memcpy(&format, e + 2, 2); memcpy(&length, e + 4, 4); memcpy(&offset, e + 12, 4);
        if (keys + key_offset >= n || values + offset + length > n || strcmp((const char *)data + keys + key_offset, key)) continue;
        if (format == 0x0404 && number && length >= 4) { memcpy(number, data + values + offset, 4); return 1; }
        if (text && text_size) {
            size_t copy = length < text_size - 1 ? length : text_size - 1;
            memcpy(text, data + values + offset, copy);
            text[copy] = 0;
        }
        return 1;
    }
    return 0;
}

static void apply_patches(const char *path, Segment *segments, uint64_t ns, const Reloc *relocs, uint64_t nr) {
    FILE *f = fopen(path, "rb");
    char magic[8];
    if (!f || fread(magic, 1, 8, f) != 8 || memcmp(magic, "G3PATCH2", 8)) fail("invalid patch file");
    uint64_t base = read64(f), count = read64(f), bytes = 0, rebased = 0;
    unsigned char data[4096];
    for (uint64_t i = 0; i < count; ++i) {
        uint64_t offset = read64(f), length = read64(f);
        if (!length || length > sizeof(data) || !mapped(segments, ns, offset, length) || fread(data, 1, length, f) != length)
            fail("bad patch entry");
        uint64_t slots[sizeof(data) / 8]; size_t nslots = 0;
        for (uint64_t r = 0; r < nr; ++r) {
            if (!(relocs[r].target < offset + length && offset < relocs[r].target + 8)) continue;
            uint64_t target = relocs[r].target, value;
            if (relocs[r].kind || target < offset || target + 8 > offset + length) fail("patch overlaps a relocation");
            memcpy(&value, data + (target - offset), 8);
            if (value < base || !mapped(segments, ns, value - base, 1)) fail("patch writes a pointer outside the image");
            slots[nslots++] = target;
        }
        memcpy(image + offset, data, length);
        for (size_t s = 0; s < nslots; ++s) {
            uint64_t value;
            memcpy(&value, image + slots[s], 8);
            value = (uint64_t)(uintptr_t)image + (value - base);
            memcpy(image + slots[s], &value, 8);
        }
        rebased += nslots;
        bytes += length;
    }
    if (fgetc(f) != EOF) fail("trailing data in patch file");
    fclose(f);
    printf("Patches: %" PRIu64 " writes, %" PRIu64 " bytes applied, %" PRIu64 " pointers rebased\n", count, bytes, rebased);
}

void runtime_restart(void) {
    fflush(NULL);
    puts("Runtime: restart is not supported by the ARM64 host yet");
    _exit(0);
}

#if defined(__APPLE__) && defined(GR_HOST_GPU)
/* Cocoa windows and their events belong to the main thread, so on macOS the loader below runs
 * on a game thread and hands gow3gpu_init to the main thread, which then pumps the window. */
#include <pthread.h>
static pthread_mutex_t main_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t main_cond = PTHREAD_COND_INITIALIZER;
static const Gow3GpuConfig *main_gpu_config;
static int main_state, main_result; /* 1 GPU init requested, 2 done */
static int gpu_init(const Gow3GpuConfig *config) {
    pthread_mutex_lock(&main_lock);
    main_gpu_config = config;
    main_state = 1;
    pthread_cond_broadcast(&main_cond);
    while (main_state != 2) pthread_cond_wait(&main_cond, &main_lock);
    pthread_mutex_unlock(&main_lock);
    return main_result;
}
static int host_main(int argc, char **argv);
typedef struct { int argc; char **argv; } HostArgs;
static void *game_thread(void *p) {
    HostArgs *a = p;
    exit(host_main(a->argc, a->argv));
}
/* GR_DYNAREC_TEST_TRIGGER=<file>: when the file appears (it is removed), Box64's dynarec test
 * mode starts for every block translated from then on (Box64 prints the blocks whose results
 * differ from the interpreter's). Starting it just before the part to check keeps the game
 * fast up to there. The file may hold a guest address range "start-end" (hex) to test only. */
static void *dynarec_test_watch(void *p) {
    const char *path = p;
    for (;;) {
        usleep(250000);
        FILE *f = fopen(path, "r");
        if (!f) continue;
        unsigned long long start = 0, end = 0;
        char word[16] = "";
        if (fscanf(f, "%15s", word) == 1 && !strcmp(word, "interp")) {
            /* "interp": no more translation, new code runs in the interpreter */
            fclose(f);
            remove(path);
            box64e_set_dynarec(0);
            printf("Box64: dynarec off for code reached from now on\n");
            fflush(stdout);
            continue;
        }
        rewind(f);
        if (fscanf(f, "%llx-%llx", &start, &end) != 2) start = end = 0;
        fclose(f);
        remove(path);
        box64e_set_dynarec_test(1, (uintptr_t)start, (uintptr_t)end);
        printf("Box64: dynarec test mode on (0x%llx-0x%llx)\n", start, end);
        fflush(stdout);
    }
    return NULL;
}
int main(int argc, char **argv) {
    static HostArgs args;
    if (getenv("GR_DYNAREC_TEST_TRIGGER")) {
        pthread_t watch;
        pthread_create(&watch, NULL, dynarec_test_watch, getenv("GR_DYNAREC_TEST_TRIGGER"));
    }
    args = (HostArgs){argc, argv};
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 16 << 20);
    pthread_t thread;
    if (pthread_create(&thread, &attr, game_thread, &args)) fail("cannot start the game thread");
    pthread_mutex_lock(&main_lock);
    while (main_state != 1) pthread_cond_wait(&main_cond, &main_lock);
    main_result = gow3gpu_init(main_gpu_config);
    main_state = 2;
    pthread_cond_broadcast(&main_cond);
    pthread_mutex_unlock(&main_lock);
    if (!main_result) gow3gpu_main_loop();
    for (;;) pause(); /* the game thread reports the failure and exits */
}
static int host_main(int argc, char **argv) {
#elif defined(__SWITCH__)
#define gpu_init gow3gpu_init
/* The console's entry (gr_switch.c) sets up memory, files and the applet thread, then runs
 * this on the game thread. */
int gr_host_main(int argc, char **argv) {
#else
#define gpu_init gow3gpu_init
int main(int argc, char **argv) {
#endif
    setvbuf(stdout, NULL, _IONBF, 0);
    page_size = (size_t)sysconf(_SC_PAGESIZE);
    int strict_imports = 0, check_imports = 0, cpu_only = 0;
    const char *content_profile = NULL, *app0 = NULL, *user_dir = NULL, *patch_file = NULL;
    for (int i = 2; i < argc; ++i) {
        if (!strcmp(argv[i], "--cpu-only")) cpu_only = 1;
        else if (!strcmp(argv[i], "--strict-imports")) strict_imports = 1;
        else if (!strcmp(argv[i], "--check-imports")) check_imports = 1;
        else if (!strcmp(argv[i], "--content-profile") && i + 1 < argc) content_profile = argv[++i];
        else if (!strcmp(argv[i], "--app0") && i + 1 < argc) app0 = argv[++i];
        else if (!strcmp(argv[i], "--user") && i + 1 < argc) user_dir = argv[++i];
        else if (!strcmp(argv[i], "--patches") && i + 1 < argc) patch_file = argv[++i];
        else if (!strcmp(argv[i], "--timeout") && i + 1 < argc) ++i; /* no watchdog yet */
        else { fprintf(stderr, "Unknown option: %s\n", argv[i]); return 1; }
    }
    if (argc < 2) {
        fprintf(stderr, "Usage: %s boot-linked.bin [--strict-imports] [--check-imports] [--content-profile file] "
                        "[--app0 dir] [--user dir] [--patches file]\n", argv[0]);
        return 1;
    }

#ifndef __SWITCH__ /* the console never delivers memory faults to the process */
    struct sigaction sa = {0};
    sa.sa_sigaction = fault;
    sa.sa_flags = SA_SIGINFO;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, NULL); sigaction(SIGBUS, &sa, NULL); sigaction(SIGILL, &sa, NULL);
#endif

    box64e_jit jit;
    box64e_thread_name = runtime_thread_current_name;
#ifdef __SWITCH__
    { void gr_trace_hooks(void); gr_trace_hooks(); }
#endif
#ifdef __SWITCH__
    /* Both of the JIT's views come from the fixed heap (2x this); the game uses about 24 MiB
     * by the end of loading (heap report, 2026-11-18). */
    const size_t jit_size = (size_t)128 << 20;
#else
    const size_t jit_size = (size_t)256 << 20;
#endif
    if (box64e_jit_create(jit_size, &jit) || box64e_init(&jit)) fail("Box64 initialization failed");

    if (content_profile) {
        FILE *profile = fopen(content_profile, "rb");
        unsigned char data[28];
        if (!profile) fail("cannot open content profile");
        if (fread(data, 1, sizeof(data), profile) != sizeof(data) || fgetc(profile) != EOF || memcmp(data, "G3CONT01", 8))
            fail("invalid content profile");
        fclose(profile);
        uint32_t values[5];
        for (unsigned i = 0; i < 5; ++i) {
            unsigned char *p = data + 8 + i * 4;
            values[i] = (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
        }
        runtime_content_configure(values);
    }
    if (app0) {
        runtime_file_configure(app0, user_dir ? user_dir : "user");
        char sfo[4096], id[16] = "";
        snprintf(sfo, sizeof(sfo), "%s/sce_sys/param.sfo", app0);
        if (sfo_value(sfo, "INSTALL_DIR_SAVEDATA", id, sizeof(id), NULL) || sfo_value(sfo, "TITLE_ID", id, sizeof(id), NULL))
            runtime_savedata_configure(id);
    }

#ifdef GR_HOST_GPU
    gow3gpu_register_kernel();
#else
    cpu_only = 1;
#endif

    /* --- boot file (G3PROBE5 from link_modules.py) */
    FILE *f = fopen(argv[1], "rb");
    if (!f) fail("cannot open boot file; run scripts/prepare.py, link_libc.py and link_modules.py first");
    char magic[8];
    if (fread(magic, 1, 8, f) != 8 || memcmp(magic, "G3PROBE5", 8)) fail("expected a G3PROBE5 boot file (link_modules.py)");
    uint64_t size = read64(f), entry = read64(f), ns = read64(f), nr = read64(f);
    import_count = read64(f);
    uint64_t capabilities = read64(f);
    if (capabilities & ~UINT64_C(1)) fail("unknown runtime capabilities");
    runtime_start(strict_imports ? 0 : capabilities);
    if (!size || size > 512 * 1024 * 1024 || entry >= size || !ns || ns > 64 || nr > 1000000 || import_count > 100000)
        fail("boot file limits exceeded");
    int native_libc = !strict_imports && (capabilities & 1);
    uint64_t main_tls[4] = {0};
    uint64_t *bindings = calloc(import_count ? import_count : 1, sizeof(*bindings));
    uint64_t *binding_kinds = calloc(import_count ? import_count : 1, sizeof(*binding_kinds));
    if (!bindings || !binding_kinds) fail("allocation failed");
    uint64_t procparam = read64(f);
    for (int i = 0; i < 4; ++i) main_tls[i] = read64(f);
    module_count = read64(f);
    if (!module_count || module_count > sizeof(modules) / sizeof(*modules)) fail("invalid module count");
    for (uint64_t m = 0; m < module_count; ++m) {
        LinkedModule *x = &modules[m];
        x->base = read64(f); x->size = read64(f); x->init = read64(f); x->tls_address = read64(f);
        x->tls_memsz = read64(f); x->tls_filesz = read64(f); x->tls_module = read64(f);
    }
    uint64_t nb = read64(f);
    if (main_tls[1] > main_tls[2] || main_tls[2] > 1024 * 1024 || main_tls[0] > size ||
        main_tls[1] > size - main_tls[0] || (main_tls[3] & (main_tls[3] - 1)) || main_tls[3] > 4096)
        fail("invalid eboot TLS metadata");
    for (uint64_t m = 0; m < module_count; ++m) {
        LinkedModule *x = &modules[m];
        if (x->base >= size || !x->size || x->size > size - x->base || x->init < x->base || x->init - x->base >= x->size ||
            (x->tls_module && (x->tls_address >= size || x->tls_memsz > size - x->tls_address || x->tls_memsz > 1024 * 1024 ||
                               x->tls_filesz > x->tls_memsz || x->tls_module < 2 || x->tls_module > 7)))
            fail("invalid linked module metadata");
    }
    if (nb > import_count) fail("invalid binding count");
    for (uint64_t i = 0; i < nb; ++i) {
        uint64_t index = read64(f), address = read64(f), kind = read64(f);
        int inside = 0;
        for (uint64_t m = 0; m < module_count; ++m)
            if (address >= modules[m].base && address - modules[m].base < modules[m].size) inside = 1;
        if (index >= import_count || !inside || (kind != 1 && kind != 2) || bindings[index]) fail("invalid native binding");
        bindings[index] = address;
        binding_kinds[index] = kind;
    }
    Segment *segments = calloc(ns, sizeof(*segments));
    Reloc *relocs = calloc(nr ? nr : 1, sizeof(*relocs));
    names = calloc(import_count ? import_count : 1, sizeof(*names));
    if (!segments || !relocs || !names) fail("allocation failed");
    for (uint64_t i = 0; i < ns; ++i) {
        segments[i].address = read64(f);
        segments[i].size = read64(f);
        segments[i].flags = read64(f);
        if (segments[i].address > size || segments[i].size > size - segments[i].address ||
            segments[i].address % 16384 || segments[i].flags > 7) fail("bad segment");
    }
    if (fread(names, 128, import_count, f) != import_count) fail("truncated import names");
    for (uint64_t i = 0; i < import_count; ++i)
        if (!memchr(names[i], 0, 128)) fail("unterminated import name");
    for (uint64_t i = 0; i < nr; ++i) {
        relocs[i].target = read64(f);
        relocs[i].kind = read64(f);
        relocs[i].value = read64(f);
        relocs[i].addend = read64(f);
        if (!mapped(segments, ns, relocs[i].target, 8) || relocs[i].kind > 2 ||
            (relocs[i].kind && relocs[i].value >= import_count) ||
            (relocs[i].kind != 2 && relocs[i].addend) || relocs[i].addend >= page_size) fail("bad relocation");
    }
    for (uint64_t m = 0; m < module_count; ++m)
        if (!mapped(segments, ns, modules[m].init, 1) ||
            (modules[m].tls_module && !mapped(segments, ns, modules[m].tls_address, modules[m].tls_memsz)))
            fail("unmapped module metadata");
    if (!mapped(segments, ns, procparam, 64)) fail("unmapped procparam");
    image_size = size;
    image = allocate(round_page(size));
    if (fread(image, 1, size, f) != size || fgetc(f) != EOF) fail("incorrect memory image size");
    fclose(f);

    /* Console bring-up (GR_SWITCH_TEST, set by the start menu in gr_switch.c): "traps_first"
     * allocates the import tables before the GPU library starts, "wait" pauses 5 s after it
     * started. */
    const char *test = getenv("GR_SWITCH_TEST");
    uintptr_t *function_bridges = NULL;
    if (test && !strcmp(test, "traps_first")) {
        STEP("allocating import tables before the GPU library");
        function_bridges = calloc(import_count ? import_count : 1, sizeof(*function_bridges));
        data_traps = allocate((import_count + 1) * page_size);
        STEP("import tables allocated");
    }

#ifdef GR_HOST_GPU
    if (!cpu_only) {
        char title[128] = "PS4 game", serial[16] = "UNKNOWN", sfo[4096];
        uint32_t attributes = 0;
        snprintf(sfo, sizeof(sfo), "%s/sce_sys/param.sfo", app0 ? app0 : ".");
        sfo_value(sfo, "TITLE", title, sizeof(title), NULL);
        sfo_value(sfo, "TITLE_ID", serial, sizeof(serial), NULL);
        sfo_value(sfo, "ATTRIBUTE", NULL, 0, &attributes);
        uint64_t sdk = 0;
        memcpy(&sdk, image + procparam + 16, 8); /* procparam: size, magic, count, sdk_version */
        Gow3GpuConfig gpu = {title, serial, user_dir ? user_dir : "user", (uint32_t)sdk, attributes, 1920, 1080};
        if (gpu_init(&gpu)) fail("GPU initialization failed");
        printf("GPU: window and Vulkan presenter ready; SDK 0x%08x, %u HLE symbols\n", (unsigned)sdk, gow3gpu_symbol_count());
    }
#endif

    STEP("GPU ready; bridging imports");
    if (test && !strcmp(test, "wait")) {
        for (int s = 1; s <= 5; ++s) {
            sleep(1);
            char line[48];
            snprintf(line, sizeof line, "waited %d s after the GPU library started", s);
            STEP(line);
        }
    }
    /* --- imports: the runtime's functions and unresolved imports become bridges */
    if (!function_bridges) {
        function_bridges = calloc(import_count ? import_count : 1, sizeof(*function_bridges));
        STEP("import table allocated");
        data_traps = allocate((import_count + 1) * page_size);
    }
    mprotect(data_traps, (import_count + 1) * page_size, PROT_NONE);
    uint64_t bridged = 0, missing = 0;
    STEP("import traps mapped");
    for (uint64_t i = 0; i < nr; ++i) {
        uint64_t index = relocs[i].value;
        uintptr_t value;
        if (!relocs[i].kind) {
            value = (uintptr_t)image + relocs[i].value;
        } else if (relocs[i].kind == 2) {
            uintptr_t resolved = runtime_resolve(names[index], 1);
            if (resolved) value = resolved + relocs[i].addend;
            else if (native_libc && bindings[index]) value = (uintptr_t)image + bindings[index] + relocs[i].addend;
            else value = (uintptr_t)(data_traps + page_size * index + relocs[i].addend);
        } else if (native_libc && bindings[index] && !runtime_resolve(names[index], 0)) {
            value = (uintptr_t)image + bindings[index];
        } else {
            if (!function_bridges[index]) {
                uintptr_t resolved = runtime_resolve(names[index], 0);
                function_bridges[index] = resolved ? box64e_bridge((void *)resolved, import_kind(names[index]), names[index])
                                                   : box64e_bridge_emu(unresolved, names[index]);
                if (resolved) ++bridged; else ++missing;
            }
            value = function_bridges[index];
        }
        if (relocs[i].kind == 1 && native_libc && bindings[index] && binding_kinds[index] != 1) fail("native export kind mismatch");
        memcpy(image + relocs[i].target, &value, 8);
    }
    printf("Imports: %" PRIu64 " bridged to the runtime, %" PRIu64 " not provided\n", bridged, missing);
    STEP("imports bridged");
    if (check_imports) {
        for (uint64_t i = 0; i < import_count; ++i)
            if (function_bridges[i] && !runtime_resolve(names[i], 0))
                printf("MISSING: %s (%s)\n", names[i], runtime_import_name(names[i]));
        fflush(NULL);
        _exit(missing ? 1 : 0);
    }
    if (patch_file) apply_patches(patch_file, segments, ns, relocs, nr);
    runtime_camera_probe_install(image, size);
#ifdef __SWITCH__
    { void gr_resolution_install(unsigned char*, size_t); gr_resolution_install(image, size); }
#endif

    /* --- guest memory rights. Box64 reads the x86 code, so the host pages only need R/W; the
     * dynarec translates only what is marked executable. */
    mprotect(image, round_page(size), PROT_NONE);
    box64e_set_protection((uintptr_t)image, round_page(size), 0);
    int executable_entry = 0;
    for (uint64_t i = 0; i < ns; ++i) {
        int prot = ((segments[i].flags & 4) ? PROT_READ : 0) | ((segments[i].flags & 2) ? PROT_WRITE : 0);
        if (segments[i].flags & 1) prot |= PROT_READ; /* code is read by the dynarec */
        mprotect(image + segments[i].address, round_page(segments[i].size), prot);
        box64e_set_protection((uintptr_t)image + segments[i].address, round_page(segments[i].size),
                              prot | ((segments[i].flags & 1) ? PROT_EXEC : 0));
        if ((segments[i].flags & 1) && entry >= segments[i].address && entry - segments[i].address < segments[i].size)
            executable_entry = 1;
    }
    if (!executable_entry) fail("entry is not executable");
    STEP("guest memory rights set");
    box64e_trace_image = (uintptr_t)image;
    printf("Mapped %" PRIu64 " bytes at %p, %" PRIu64 " segments; applied %" PRIu64 " relocations\n", size, (void *)image, ns, nr);

    /* --- module initializers (libc first, as the PS4 dynamic linker does) */
    if (native_libc) {
        for (uint64_t m = 0; m < module_count; ++m)
            if (modules[m].tls_module)
                runtime_set_module_tls(modules[m].tls_module, image + modules[m].tls_address, modules[m].tls_filesz, modules[m].tls_memsz);
        runtime_set_main_tls(image + main_tls[0], main_tls[1], main_tls[2], main_tls[3]);
        runtime_thread_attach_main();
        runtime_set_procparam(image + procparam);
        for (uint64_t m = 0; m < module_count; ++m) {
            printf("Starting linked module %" PRIu64 " at image offset 0x%" PRIx64 "\n", m, modules[m].init);
            int result = (int)runtime_guest_call((uintptr_t)(image + modules[m].init), 3, (uint64_t)0, (uint64_t)0, (uint64_t)0);
            printf("Module %" PRIu64 " initializer returned %d\n", m, result);
            if (result) fail("module initializer failed");
        }
    }

    /* --- the game */
    printf("Entering original x86-64 code at guest offset 0x%" PRIx64 " (Box64)\n", entry);
    entered_game = 1;
    runtime_guest_fpu_init();
    enum { MAIN_STACK = 8 * 1024 * 1024 };
    unsigned char *stack = allocate(MAIN_STACK);
    box64e_set_stack(box64e_thread_emu(), stack, MAIN_STACK - 64);
    struct { uint64_t argc; const char *argv[2]; } *params = (void *)(stack + MAIN_STACK - 64);
    params->argc = 1;
    params->argv[0] = "/app0/eboot.bin";
    params->argv[1] = NULL;
    static const char *exit_name = "guest_exit";
    uintptr_t guest_exit = box64e_bridge_emu(unresolved, exit_name);
    runtime_guest_call((uintptr_t)(image + entry), 2, (uint64_t)(uintptr_t)params, (uint64_t)guest_exit);
    fail("entry unexpectedly returned");
}
