// SPDX-License-Identifier: GPL-2.0-or-later
// gow3 (Switch 2 bring-up): GOW3_BRINGUP("step") writes the first two times a step runs to
// boot.txt (switch2/host/gr_switch.c), which survives a crash. Nothing elsewhere.
#pragma once
#ifdef __SWITCH__
#include <atomic>
extern "C" void gr_crumb(const char* what);
#define GOW3_BRINGUP(what)                                                                     \
    do {                                                                                       \
        static std::atomic<int> gow3_bringup_count{0};                                         \
        if (gow3_bringup_count.fetch_add(1, std::memory_order_relaxed) < 2) gr_crumb(what);    \
    } while (0)
/// Flushes `scheduler` and waits for the GPU, with a crumb before and after: a GPU fault in
/// the work so far ends the process between the two (GR_GPU_SYNC=1 only).
#include <cstdlib>
#define GOW3_GPU_SYNC(scheduler, what)                                                         \
    do {                                                                                       \
        static const bool gow3_sync_on = [] {                                                  \
            const char* v = std::getenv("GR_GPU_SYNC");                                        \
            return v && v[0] == '1';                                                           \
        }();                                                                                   \
        if (gow3_sync_on) {                                                                    \
            gr_crumb(what ": waiting for the GPU");                                            \
            (scheduler).Finish();                                                              \
            gr_crumb(what ": GPU idle");                                                       \
        }                                                                                      \
    } while (0)
/// Steps of a traced submission: on while the thread has gow3_trace_submit set.
inline thread_local bool gow3_trace_submit = false;
#define GOW3_TRACE(what)                                                                       \
    do {                                                                                       \
        if (gow3_trace_submit) gr_crumb(what);                                                 \
    } while (0)
#else
#define GOW3_BRINGUP(what) ((void)0)
#define GOW3_GPU_SYNC(scheduler, what) ((void)0)
#define GOW3_TRACE(what) ((void)0)
#endif
