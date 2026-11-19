// SPDX-FileCopyrightText: Copyright 2023 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "common/arch.h"

#ifdef _MSC_VER
#include <intrin.h>
#endif

#include "common/types.h"

namespace Common {

#ifdef _MSC_VER
__forceinline static u64 FencedRDTSC() {
#ifdef ARCH_X86_64
    _mm_lfence();
    _ReadWriteBarrier();
    const u64 result = __rdtsc();
    _mm_lfence();
    _ReadWriteBarrier();
    return result;
#else
#error "Missing FencedRDTSC() implementation for target CPU architecture."
#endif
}
#else
static inline u64 FencedRDTSC() {
#ifdef ARCH_X86_64
    u64 eax;
    u64 edx;
    asm volatile("lfence\n\t"
                 "rdtsc\n\t"
                 "lfence\n\t"
                 : "=a"(eax), "=d"(edx));
    return (edx << 32) | eax;
#elif defined(ARCH_ARM64)
    u64 ret;
    // gow3 (Switch 2): EL0 may read only the physical counter there (libnx's armGetSystemTick).
    asm volatile("isb\n\t"
#ifdef __SWITCH__
                 "mrs %0, cntpct_el0\n\t"
#else
                 "mrs %0, cntvct_el0\n\t"
#endif
                 "isb\n\t"
                 : "=r"(ret)::"memory");
    return ret;
#else
#error "Missing FencedRDTSC() implementation for target CPU architecture."
#endif
}
#endif

u64 EstimateRDTSCFrequency();

} // namespace Common
