// SPDX-FileCopyrightText: Copyright 2025 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "common/bit_array.h"
#include "common/types.h"

namespace VideoCore {

// gow3: the tracked page is the host's protection page: 4K, but 16K on Apple Silicon, where
// mprotect works on 16K pages. With 4K tracking there, making one 4K page writable and then
// protecting its neighbour re-protected the whole 16K page, so the CPU write that faulted kept
// faulting forever (the black screen when gameplay starts, on the Mac).
// gow3 (Switch 2): 16K too. Pages are hashed, not protected, there (page_manager.cpp), and the
// per-page state of the 40-bit address space is 128 MiB at 16K instead of 512 MiB at 4K.
#if (defined(__APPLE__) && defined(__aarch64__)) || defined(__SWITCH__)
constexpr u64 TRACKER_PAGE_BITS = 14; // 16K pages
#else
constexpr u64 TRACKER_PAGE_BITS = 12; // 4K pages
#endif
constexpr u64 TRACKER_BYTES_PER_PAGE = 1ULL << TRACKER_PAGE_BITS;

constexpr u64 TRACKER_HIGHER_PAGE_BITS = 22; // each region is 4MB
constexpr u64 TRACKER_HIGHER_PAGE_SIZE = 1ULL << TRACKER_HIGHER_PAGE_BITS;
constexpr u64 TRACKER_HIGHER_PAGE_MASK = TRACKER_HIGHER_PAGE_SIZE - 1ULL;
constexpr u64 NUM_PAGES_PER_REGION = TRACKER_HIGHER_PAGE_SIZE / TRACKER_BYTES_PER_PAGE;

enum class Type {
    CPU,
    GPU,
};

using RegionBits = Common::BitArray<NUM_PAGES_PER_REGION>;

} // namespace VideoCore
