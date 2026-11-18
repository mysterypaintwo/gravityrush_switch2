/* box64e: the embedded Box64 core for the Gravity Rush ARM64 host (macOS development, Horizon).
 *
 * The host maps the PS4 game's x86-64 code itself and tells box64e which ranges are guest code.
 * Guest calls to PS4 system libraries go to bridges: small x86 stubs (int3 'S' 'C' wrapper fn
 * ret) that the dynarec turns into direct native calls. Native code calls guest code with
 * box64e_call_guest. Each host thread that runs guest code has its own emulator state. */
#ifndef GR_BOX64E_H
#define GR_BOX64E_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct x64emu_s x64emu_t;

/* How a bridge passes the guest's x86-64 SysV call to the native function. */
typedef enum {
    /* Integer/pointer return in rax (and rdx): f(rdi, rsi, rdx, rcx, r8, r9) with the low lanes
     * of xmm0-7 in v0-v7. AAPCS64 assigns integer and FP registers independently, as SysV does,
     * so this covers any function with up to 6 integer and 8 float/double/vector arguments. */
    BOX64E_INT = 0,
    /* As BOX64E_INT, but the result is float/double/vector and goes to xmm0. */
    BOX64E_FP,
    /* Up to 6 more integer arguments from the guest stack (7 to 12 in total), integer return. */
    BOX64E_STACK,
} box64e_kind;

/* Exec/write views of the JIT code memory: write = exec + jit_wdelta. The host reserves this
 * before box64e_init (box64e_jit_*.c). */
typedef struct {
    uintptr_t exec_base;
    intptr_t wdelta;
    size_t size;
} box64e_jit;

/* Reserves size bytes of split JIT memory (box64e_jit_macos.c, box64e_jit_horizon.c). */
int box64e_jit_create(size_t size, box64e_jit* out);

/* Sets up Box64 (environment, CPU features, allocator). Call once, before anything else. */
int box64e_init(const box64e_jit* jit);

/* Tells the dynarec that [addr, addr+size) holds guest memory with these PROT_* rights.
 * Guest code is only translated where PROT_EXEC is set. */
void box64e_set_protection(uintptr_t addr, size_t size, int prot);
/* Throws away translations of [addr, addr+size) (after the host rewrites guest code). */
void box64e_invalidate(uintptr_t addr, size_t size);

/* A guest-callable address that runs fn natively. Bridges are never freed. */
uintptr_t box64e_bridge(void* fn, box64e_kind kind, const char* name);
/* A bridge whose native side receives the emulator: void fn(x64emu_t*) can read and write
 * every guest register through the accessors below (varargs and unusual ABIs). */
uintptr_t box64e_bridge_emu(void (*fn)(x64emu_t* emu), const char* name);
const char* box64e_bridge_name(uintptr_t guest_addr);

/* The calling thread's emulator, created on first use with a guest stack of stack_size bytes
 * (0: 1 MiB). fs_base/gs_base set the guest segment bases (TLS). */
x64emu_t* box64e_thread_emu(void);
void box64e_set_stack(x64emu_t* emu, void* stack_base, size_t stack_size);
void box64e_set_segment_bases(x64emu_t* emu, uint64_t fs_base, uint64_t gs_base);
/* Frees the calling thread's emulator (call before the host thread exits). */
void box64e_thread_exit(void);

/* Calls guest code at fn with up to 6 integer arguments on the calling thread's emulator and
 * returns rax. Re-entrant: a bridge may call back into the guest. */
uint64_t box64e_call_guest(uintptr_t fn, int nargs, ...);
/* As box64e_call_guest, with float/double arguments in xmm0-7 and the xmm0 result in *fp_ret. */
uint64_t box64e_call_guest_fp(uintptr_t fn, int nargs, const uint64_t* args, int nfp, const double* fp_args,
                              double* fp_ret);
/* Nonzero once the calling thread's emulator stopped on an error (bad opcode, guest fault...). */
int box64e_guest_error(void);

/* Register access for bridge_emu functions: 0-15 = rax rcx rdx rbx rsp rbp rsi rdi r8-r15. */
uint64_t box64e_reg(x64emu_t* emu, int reg);
void box64e_set_reg(x64emu_t* emu, int reg, uint64_t value);
uint64_t box64e_xmm_lo(x64emu_t* emu, int reg);
void box64e_set_xmm_lo(x64emu_t* emu, int reg, uint64_t value);
uint64_t box64e_rip(x64emu_t* emu);
/* Writes the guest registers and a few stack words to stderr. */
void box64e_dump(x64emu_t* emu, const char* why);

#ifdef __cplusplus
}
#endif
#endif
