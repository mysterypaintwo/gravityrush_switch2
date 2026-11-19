// SPDX-License-Identifier: GPL-2.0-or-later
// gow3: CPU helpers that differ between x86-64 (PC) and ARM64 (Switch 2 port, Mac host).
#pragma once
#include <cstdint>
#if defined(__x86_64__) || defined(_M_X64)
#include <x86intrin.h>
#define GOW3_CPU_PAUSE() __builtin_ia32_pause()
inline std::uint64_t Gow3ReadTsc() { return __rdtsc(); }
#elif defined(__aarch64__)
#define GOW3_CPU_PAUSE() __asm__ volatile("yield" ::: "memory")
inline std::uint64_t Gow3ReadTsc() {
    std::uint64_t v;
#ifdef __SWITCH__
    __asm__ volatile("isb\n mrs %0, cntpct_el0" : "=r"(v)); // the only counter EL0 may read there
#else
    __asm__ volatile("isb\n mrs %0, cntvct_el0" : "=r"(v));
#endif
    return v;
}
#endif
