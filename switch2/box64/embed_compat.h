/* Force-included into every Box64 source of the embedded build (switch2/box64/CMakeLists.txt).
 * The embedded build has no ELF loader, no library wrapping and no signal handling: the host
 * program loads the guest and calls Box64 through box64e.h. It runs on macOS (development) and on
 * Horizon (the console), so this header fills the gaps both have compared with glibc. */
#ifndef GR_BOX64_EMBED_COMPAT_H
#define GR_BOX64_EMBED_COMPAT_H

#include <stddef.h>
#include <stdint.h>
#include <math.h>

#ifdef __APPLE__
/* glibc's sincos is __sincos in Apple's libm. */
#define sincos __sincos
#define sincosf __sincosf
#endif
#ifdef __SWITCH__
/* newlib declares sincos only for _GNU_SOURCE builds; Box64 needs nothing more than this. */
static inline void gr_sincos(double x, double* s, double* c) { *s = sin(x); *c = cos(x); }
static inline void gr_sincosf(float x, float* s, float* c) { *s = sinf(x); *c = cosf(x); }
#define sincos gr_sincos
#define sincosf gr_sincosf
#endif

/* Box64 asks for these mmap flags, which glibc defines and the embedded hosts do not. */
#include <sys/mman.h>
#ifndef MAP_32BIT
#define MAP_32BIT 0
#endif
#ifndef MAP_NORESERVE
#define MAP_NORESERVE 0
#endif
#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE MAP_FIXED
#endif
#ifndef MAP_GROWSDOWN
#define MAP_GROWSDOWN 0
#endif
#ifndef MAP_STACK
#define MAP_STACK 0
#endif
#ifndef MAP_HUGETLB
#define MAP_HUGETLB 0
#endif
#ifndef MAP_ANONYMOUS
#define MAP_ANONYMOUS MAP_ANON
#endif

/* Split JIT write view (third_party/box64/src/include/custommem.h): every source sees it,
 * including the dynarec passes, which do not include custommem.h. */
extern intptr_t box64_jit_wdelta;
#define JITW(p) ((void*)((uintptr_t)(p) + box64_jit_wdelta))

/* The current CPU number, for CPUID's APIC id (os_embed.c). */
int sched_getcpu(void);

#endif
