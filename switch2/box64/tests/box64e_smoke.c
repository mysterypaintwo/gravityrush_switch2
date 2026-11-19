/* box64e smoke test: hand-assembled x86-64 functions translated by the embedded Box64 dynarec.
 * Covers a leaf function, a loop, guest -> native calls through bridges (integer and float),
 * and native -> guest callbacks from inside a bridge. Run with BOX64_DYNAREC=0 to use the
 * interpreter instead. Exit status 0 means every check passed. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>

#include "box64e.h"

#ifdef __SWITCH__
#include <stdarg.h>
#include <switch.h>
/* On the console: every line goes to the screen and to sdmc:/box64e_smoke.txt (flushed per
 * line, so a crash still leaves the lines before it). */
static FILE* smoke_log;
static int smoke_printf(const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    char line[512];
    const int n = vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    fputs(line, stdout);
    consoleUpdate(NULL);
    if (smoke_log) { fputs(line, smoke_log); fflush(smoke_log); }
    return n;
}
static int smoke_puts(const char* s) { return smoke_printf("%s\n", s); }
/* smoke_main below prints through these (main keeps the plain console printf). */
#define printf smoke_printf
#define puts smoke_puts
#endif

static const uint8_t code_add[] = {0x48, 0x8d, 0x04, 0x37, 0xc3}; /* lea rax,[rdi+rsi]; ret */

static const uint8_t code_sum[] = {  /* rax = n + (n-1) + ... + 1 */
    0x31, 0xc0,             /* xor eax, eax */
    0x48, 0x85, 0xff,       /* test rdi, rdi */
    0x74, 0x08,             /* je end */
    0x48, 0x01, 0xf8,       /* loop: add rax, rdi */
    0x48, 0xff, 0xcf,       /* dec rdi */
    0x75, 0xf8,             /* jne loop */
    0xc3,                   /* end: ret */
};

static const uint8_t code_call_native[] = { /* f(x, g) = g(x, 3) + 5, rbx preserved */
    0x53,                         /* push rbx */
    0x48, 0x89, 0xf3,             /* mov rbx, rsi */
    0xbe, 0x03, 0x00, 0x00, 0x00, /* mov esi, 3 */
    0xff, 0xd3,                   /* call rbx */
    0x48, 0x83, 0xc0, 0x05,       /* add rax, 5 */
    0x5b,                         /* pop rbx */
    0xc3,                         /* ret */
};

static const uint8_t code_call_fp[] = { /* h(fn, a, b) = fn(a, b) * 2 (doubles in xmm0/xmm1) */
    0x48, 0x83, 0xec, 0x08, /* sub rsp, 8 */
    0xff, 0xd7,             /* call rdi */
    0xf2, 0x0f, 0x58, 0xc0, /* addsd xmm0, xmm0 */
    0x48, 0x83, 0xc4, 0x08, /* add rsp, 8 */
    0xc3,                   /* ret */
};

static const uint8_t code_callback[] = { /* k(cb) = cb() + 1 */
    0x48, 0x83, 0xec, 0x08, /* sub rsp, 8 */
    0xff, 0xd7,             /* call rdi */
    0x48, 0x83, 0xc0, 0x01, /* add rax, 1 */
    0x48, 0x83, 0xc4, 0x08, /* add rsp, 8 */
    0xc3,                   /* ret */
};

static uintptr_t guest_add;

static uint64_t native_mul_add(uint64_t x, uint64_t y) { return x * y + 1; }
static double native_fmul(double a, double b) { return a * b; }
static uint64_t native_calls_back(void) { return box64e_call_guest(guest_add, 2, (uint64_t)20, (uint64_t)22); }

static int failures;
static void check(const char* what, uint64_t got, uint64_t want)
{
    printf("%-44s %llu (want %llu) %s\n", what, (unsigned long long)got, (unsigned long long)want,
           got == want ? "ok" : "FAIL");
    if (got != want) failures++;
}

static uintptr_t place(uint8_t* mem, size_t* off, const uint8_t* code, size_t size)
{
    uintptr_t at = (uintptr_t)mem + *off;
    memcpy(mem + *off, code, size);
    *off = (*off + size + 63) & ~(size_t)63;
    return at;
}

static int smoke_main(void)
{
    box64e_jit jit;
    if (box64e_jit_create(64 << 20, &jit)) { puts("JIT memory: FAIL"); return 1; }
    printf("JIT: exec %p, write delta %+lld\n", (void*)jit.exec_base, (long long)jit.wdelta);
    if (box64e_init(&jit)) { puts("box64e_init: FAIL"); return 1; }

    /* Guest code lives in ordinary read-write memory: only Box64 reads the x86 bytes. */
    size_t size = 1 << 16, off = 0;
    uint8_t* mem = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    guest_add = place(mem, &off, code_add, sizeof code_add);
    uintptr_t guest_sum = place(mem, &off, code_sum, sizeof code_sum);
    uintptr_t guest_call_native = place(mem, &off, code_call_native, sizeof code_call_native);
    uintptr_t guest_call_fp = place(mem, &off, code_call_fp, sizeof code_call_fp);
    uintptr_t guest_callback = place(mem, &off, code_callback, sizeof code_callback);
    box64e_set_protection((uintptr_t)mem, size, PROT_READ | PROT_EXEC);

    uintptr_t b_mul_add = box64e_bridge((void*)native_mul_add, BOX64E_INT, "native_mul_add");
    uintptr_t b_fmul = box64e_bridge((void*)native_fmul, BOX64E_FP, "native_fmul");
    uintptr_t b_calls_back = box64e_bridge((void*)native_calls_back, BOX64E_INT, "native_calls_back");

    check("add(40, 2)", box64e_call_guest(guest_add, 2, (uint64_t)40, (uint64_t)2), 42);
    check("sum(100000)", box64e_call_guest(guest_sum, 1, (uint64_t)100000), 5000050000ull);
    for (int i = 0; i < 3; i++) /* again: now from translated blocks */
        check("sum(1000) again", box64e_call_guest(guest_sum, 1, (uint64_t)1000), 500500);
    check("guest calls native: mul_add(7, 3) + 5", box64e_call_guest(guest_call_native, 2, (uint64_t)7, b_mul_add), 27);

    uint64_t args[1] = {b_fmul};
    double fp_args[2] = {3.0, 4.0}, fp_ret = 0;
    box64e_call_guest_fp(guest_call_fp, 1, args, 2, fp_args, &fp_ret);
    check("guest calls native fp: fmul(3, 4) * 2", (uint64_t)fp_ret, 24);

    check("native calls back into guest: add(20, 22) + 1", box64e_call_guest(guest_callback, 1, b_calls_back), 43);
    check("guest error flag", (uint64_t)box64e_guest_error(), 0);

    printf("%s\n", failures ? "SMOKE TEST FAILED" : "all checks passed");
    return failures ? 1 : 0;
}

#ifdef __SWITCH__
#undef printf
#undef puts
/* On the console: the checks are shown and logged as they run (smoke_printf), then the verdict. */
int main(void)
{
    consoleInit(NULL);
    smoke_log = fopen("sdmc:/box64e_smoke.txt", "w");
    const int result = smoke_main();
    if (smoke_log) fclose(smoke_log);
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    PadState pad;
    padInitializeDefault(&pad);
    printf("box64e smoke test: %s\n", result ? "FAILED" : "all checks passed");
    printf("Details: sdmc:/box64e_smoke.txt\n\nPress + to exit.\n");
    while (appletMainLoop()) {
        padUpdate(&pad);
        if (padGetButtonsDown(&pad) & HidNpadButton_Plus) break;
        consoleUpdate(NULL);
    }
    consoleExit(NULL);
    return result;
}
#else
int main(void) { return smoke_main(); }
#endif
