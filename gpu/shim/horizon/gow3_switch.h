// SPDX-License-Identifier: GPL-2.0-or-later
// gow3 (Switch 2): <switch.h> for the GPU library. libnx's types.h defines u128 as an integer,
// shadPS4's common/types.h as std::array<u64, 2>; libnx's is renamed while its headers load.
#pragma once
#define u128 nx_u128
#define s128 nx_s128
#include <switch.h>
#undef u128
#undef s128
