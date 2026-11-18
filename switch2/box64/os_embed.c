/* Box64's OS layer for the embedded build (macOS development host and Horizon).
 * Replaces os_linux.c, hostext_linux.c, freq_linux.c, symbolfuncs_linux.c, random_linux.c,
 * emit_signals_linux.c, perfmap.c and tools/cpumask.c. There is no ELF loader, no syscall
 * emulation and no signal delivery: the guest only reaches the host through bridges
 * (box64e.c), and a fault anywhere ends the process. */
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "os.h"
#include "debug.h"
#include "env.h"
#include "hostext.h"
#include "x64emu.h"
#include "emu/x64emu_private.h"
#include "box64context.h"

int box64_isAddressSpace32 = 0;

/* --- threads and scheduling */
int GetTID(void)
{
#ifdef __APPLE__
    uint64_t tid = 0;
    pthread_threadid_np(NULL, &tid);
    return (int)tid;
#else
    return (int)(uintptr_t)pthread_self();
#endif
}

int SchedYield(void) { return sched_yield(); }
int sched_getcpu(void) { return 0; }

/* --- bridges: int3 'S' 'C' wrapper function ret (box64e.c builds them) */
int IsBridgeSignature(char s, char c) { return s == 'S' && c == 'C'; }

typedef void (*wrapper_t)(x64emu_t* emu, uintptr_t fnc);

/* addr points just after the 0xCC. A zero wrapper means "leave the emulator" (box64e_call_guest
 * returns); otherwise the wrapper runs and the guest continues at the bridge's ret. */
void EmuInt3(void* e, void* a)
{
    x64emu_t* emu = (x64emu_t*)e;
    uintptr_t* addr = (uintptr_t*)a;
    if (!IsBridgeSignature(*(char*)(*addr), *(char*)(*addr + 1))) {
        printf_log(LOG_NONE, "Box64: int3 at %p is not a bridge\n", (void*)(*addr - 1));
        emu->quit = 1;
        emu->error = 1;
        return;
    }
    *addr += 2;
    wrapper_t w = *(wrapper_t*)(*addr);
    *addr += 8;
    if (!w) {
        R_RIP = *addr;
        emu->quit = 1;
        return;
    }
    uintptr_t fnc = *(uintptr_t*)(*addr);
    *addr += 8;
    R_RIP = *addr;
    w(emu, fnc);
}

int IsNativeCall(uintptr_t addr, int is32bits, uintptr_t* calladdress, uint16_t* retn)
{
    (void)addr; (void)is32bits; (void)calladdress; (void)retn;
    return 0;
}

const char* GetBridgeName(void* p) { (void)p; return NULL; }
const char* GetNativeName(void* p, int lib)
{
    (void)lib;
    static __thread char name[32];
    snprintf(name, sizeof name, "native(%p)", p);
    return name;
}
void* GetNativeFnc(uintptr_t fnc) { (void)fnc; return NULL; }

/* --- no syscalls, forks or Linux segments in a PS4 guest */
void* EmuFork(void* emu, int forktype) { (void)emu; (void)forktype; return NULL; }
void EmuX64Syscall(void* emu) { printf_log(LOG_NONE, "Box64: guest syscall at %p (not supported)\n", (void*)((x64emu_t*)emu)->ip.q[0]); ((x64emu_t*)emu)->quit = 1; }
void EmuX64Syscall_linux(void* emu) { EmuX64Syscall(emu); }
void EmuX86Syscall(void* emu) { EmuX64Syscall(emu); }
void x86Int(void* emu, int code) { printf_log(LOG_NONE, "Box64: guest int 0x%x (not supported)\n", code); ((x64emu_t*)emu)->quit = 1; }
void* GetSeg43Base(void* emu) { (void)emu; return NULL; }
void* GetSegmentBase(void* emu, uint32_t desc)
{
    int base = desc >> 3;
    if (!desc || base > 15) return NULL;
    base_segment_t* segs = (desc & 4) ? ((x64emu_t*)emu)->segldt : my_context->seggdt;
    return (void*)segs[base].base;
}
void PersonalityAddrLimit32Bit(void) { }
int IsAddrElfOrFileMapped(uintptr_t addr) { (void)addr; return 0; }

/* --- memory */
void* InternalMmap(void* addr, unsigned long length, int prot, int flags, int fd, ssize_t offset)
{
    return mmap(addr, length, prot, flags, fd, offset);
}
int InternalMunmap(void* addr, unsigned long length) { return munmap(addr, length); }
int VolatileRangesContains(uintptr_t addr) { (void)addr; return 0; }
int VolatileOpcodesHas(uintptr_t addr) { (void)addr; return 0; }

/* --- logging, environment, files */
void PrintfFtrace(int prefix, const char* fmt, ...)
{
    va_list args;
    if (prefix) fputs(prefix > 1 ? "[\033[31mBOX64\033[0m] " : "[BOX64] ", stderr);
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
}
void* GetEnv(const char* name) { return getenv(name); }
int FileExist(const char* filename, int flags)
{
    struct stat sb;
    if (stat(filename, &sb) == -1) return 0;
    if (flags == -1) return 1;
    if ((flags & IS_FILE) ? !S_ISREG(sb.st_mode) : !S_ISDIR(sb.st_mode)) return 0;
    if ((flags & IS_EXECUTABLE) && !(sb.st_mode & S_IXUSR)) return 0;
    return 1;
}
int MakeDir(const char* folder) { return !mkdir(folder, 0755) || errno == EEXIST; }
size_t FileSize(const char* filename)
{
    struct stat sb;
    return stat(filename, &sb) == -1 ? 0 : (size_t)sb.st_size;
}
int PrintFunctionAddr(uintptr_t nextaddr, const char* text) { (void)nextaddr; (void)text; return 0; }
void writePerfMap(uintptr_t func_addr, uintptr_t code_addr, size_t code_size, const char* inst_name)
{
    (void)func_addr; (void)code_addr; (void)code_size; (void)inst_name;
}

/* --- host CPU features. Both hosts report the Switch 2's Cortex-A78C (Armv8.2-A with LSE,
 * CRC32 and crypto), so the code Box64 generates on the Mac is the code the console runs:
 * no LSE2 (uscat), FlagM, FRINTTS, AFP or RNDR, which the M-series cores have. */
int DetectHostCpuFeatures(void)
{
    cpuext.asimd = 1;
    cpuext.atomics = 1;
    cpuext.crc32 = 1;
    cpuext.aes = cpuext.pmull = cpuext.sha1 = cpuext.sha2 = 1;
    cpuext.uscat = cpuext.flagm = cpuext.flagm2 = cpuext.frintts = cpuext.afp = cpuext.rndr = 0;
    return 1;
}

/* --- time stamp counter: the ARM generic timer */
uint64_t ReadTSC(x64emu_t* emu)
{
    (void)emu;
    uint64_t v;
    __asm__ volatile("isb\n mrs %0, cntvct_el0" : "=r"(v));
    return v;
}
uint64_t ReadTSCFrequency(x64emu_t* emu)
{
    (void)emu;
    uint64_t f;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(f));
    return f;
}

/* --- random numbers (RDRAND/RDSEED) */
uint32_t get_random32(void) { return (uint32_t)arc4random(); }
uint64_t get_random64(void) { return ((uint64_t)arc4random() << 32) | arc4random(); }

/* --- CPU masks (sched_setaffinity emulation is not used) */
void cpumask_shiftleft(void* mask, uint32_t sz, uint32_t n) { (void)mask; (void)sz; (void)n; }
void cpumask_shiftright(void* mask, uint32_t sz, uint32_t n) { (void)mask; (void)sz; (void)n; }
void cpumask_maxcpu(void* mask, uint32_t sz, uint32_t n) { (void)mask; (void)sz; (void)n; }
void cpumask_apply(uint32_t skip, uint32_t max) { (void)skip; (void)max; }

/* --- guest exceptions: there is no signal delivery, so a guest fault, int n or divide error
 * stops that emulator with an error (box64e_call_guest reports it). */
static void guest_stop(x64emu_t* emu, const char* what, int code, void* addr)
{
    printf_log(LOG_NONE, "Box64: guest %s (%d) at %p, rip %p\n", what, code, addr, (void*)R_RIP);
    emu->quit = 1;
    emu->error = 1;
}
void EmitSignal(x64emu_t* emu, int sig, void* addr, int code) { (void)code; guest_stop(emu, "signal", sig, addr); }
void CheckExec(x64emu_t* emu, uintptr_t addr) { (void)emu; (void)addr; }
void EmitInterruption(x64emu_t* emu, int num, void* addr) { guest_stop(emu, "interrupt", num, addr); }
void EmitDiv0(x64emu_t* emu, void* addr, int code) { guest_stop(emu, "divide error", code, addr); }
void EmitWineInt(x64emu_t* emu, int num, void* addr) { guest_stop(emu, "wine interrupt", num, addr); }

/* --- Box64 core references to parts of Box64 that the embedded build leaves out: the ELF
 * loader and library paths (core.c, pathcoll.c), PE/Unity detection (pe_tools.c,
 * wine_tools.c), the rolling log (box64context.c), signal critical sections (signals.c),
 * the dynacache compressor and the bridge allocator (bridge.c). None of them run here. */
#include "pathcoll.h"
#include "bridge.h"
char* ftrace_name = NULL;
int box64_mapclean = 0;
int box64_cycle_log_initialized = 0;
void initCycleLog(box64context_t* context) { (void)context; }
void AppendList(path_collection_t* collection, const char* List, int folder) { (void)collection; (void)List; (void)folder; }
void AddNewLibs(const char* list) { (void)list; }
void ParseVolatileMetadata(char* filename, void* addr) { (void)filename; (void)addr; }
void DetectUnityPlayer(char* filename) { (void)filename; }
void add_unaligned_address(uintptr_t addr) { (void)addr; }
uintptr_t AddAltJump(bridge_t* bridge, uintptr_t addr, uintptr_t jump_to_addr) { (void)bridge; (void)addr; (void)jump_to_addr; return 0; }
void enter_critical_section(void) { }
void leave_critical_section(void) { }
void* dc_compress(void* src, size_t src_size, int type, size_t* dest_size) { (void)src; (void)src_size; (void)type; (void)dest_size; return NULL; }
int dc_uncompress(void* src, size_t src_size, int type, void* dest, size_t dest_size) { (void)src; (void)src_size; (void)type; (void)dest; (void)dest_size; return -1; }
int isnanf(float f) { return __builtin_isnan(f); }
int isinff(float f) { return __builtin_isinf(f); }
