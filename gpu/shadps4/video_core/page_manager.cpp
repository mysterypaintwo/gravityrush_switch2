// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <boost/container/small_vector.hpp>
#include "gow3_toggles.h"
#include "common/assert.h"
#include "common/debug.h"
#include "common/div_ceil.h"
#include "common/error.h"
#include "common/range_lock.h"
#include "common/signal_context.h"
#include "core/emulator_settings.h"
#include "core/memory.h"
#include "core/signals.h"
#include "video_core/page_manager.h"
#include "video_core/page_state.h"
#include "video_core/renderer_vulkan/vk_rasterizer.h"
#ifdef __SWITCH__
#include <algorithm>
#include <cstdlib>
#include <map>
#include <unordered_map>
#include <unordered_set>
#endif

#ifndef _WIN64
#include <sys/mman.h>
#include "common/adaptive_mutex.h"
#else
#include <windows.h>
#endif

#ifdef __linux__
#include <thread>
#include <fcntl.h>
#include <linux/userfaultfd.h>
#include <poll.h>
#include <unordered_set>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <mutex>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#else
#include "common/spin_lock.h"
#endif
#ifdef __SWITCH__
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <unordered_map>
#include <vector>
#include <xxhash.h>
#include "gow3_copy.h"
extern "C" int runtime_memory_is_mapped(uintptr_t address, uint64_t size); // runtime_memory.c
#endif

namespace VideoCore {

constexpr size_t PM_PAGE_SIZE = TRACKER_BYTES_PER_PAGE; // gow3: host protection page
constexpr size_t PM_PAGE_BITS = TRACKER_PAGE_BITS;

struct PageManager::Impl {
    using PageState = VideoCore::PageState;

#ifdef __SWITCH__
    // gow3 (Switch 2): a 39-bit address space; one state per 16K page is 64 MiB instead of 128.
    static constexpr size_t ADDRESS_BITS = 39;
#else
    static constexpr size_t ADDRESS_BITS = 40;
#endif
    static constexpr size_t NUM_ADDRESS_PAGES = 1ULL << (ADDRESS_BITS - PM_PAGE_BITS);
    static constexpr size_t NUM_ADDRESS_LOCKS = NUM_ADDRESS_PAGES / PAGES_PER_LOCK;
    inline static Vulkan::Rasterizer* rasterizer;

    Impl() = default;
    virtual ~Impl() = default;

    virtual void OnMap(VAddr address, size_t size) {
        // No-op
    }

    virtual void OnUnmap(VAddr address, size_t size) {
        // No-op
    }

    virtual void Protect(VAddr address, size_t size, Core::MemoryPermission perms) = 0;

    /// Guest writes the host could not see as faults (Switch 2), handled before a submission.
    virtual void CheckGuestWrites() {}

    static bool GuestFaultSignalHandler(void* context, void* fault_address) {
        const auto addr = reinterpret_cast<VAddr>(fault_address);
        // gow3: the draw recording thread handles its faults inline too (vk_draw_pipe.h).
        const auto is_gpu_thread = rasterizer->IsGpuSideThread();
        if (is_gpu_thread) {
            Gow3Stats::gpu_signal_faults.fetch_add(1, std::memory_order_relaxed);
        }
        if (Common::IsWriteError(context)) {
            Gow3Stats::Timer timer{Gow3Stats::t_write_faults};
            const bool handled = rasterizer->OnWriteFault(addr, is_gpu_thread);
            // gow3: a write that keeps faulting on the same page (the handler does not make it
            // writable) hangs its thread; report it once.
            thread_local VAddr last_page = 0;
            thread_local u64 repeats = 0;
            if ((addr >> PM_PAGE_BITS) == last_page) {
                if (++repeats == 100000) {
                    std::fprintf(stderr, "PageManager: write fault at 0x%llx repeats (handled %d, GPU mapped %d)\n",
                                 (unsigned long long)addr, int(handled),
                                 int(rasterizer->IsMapped(addr, 8)));
                }
            } else {
                last_page = addr >> PM_PAGE_BITS;
                repeats = 0;
            }
            return handled;
        } else {
            Gow3Stats::read_faults.fetch_add(1, std::memory_order_relaxed);
            Gow3Stats::Timer timer{Gow3Stats::t_read_faults};
            return rasterizer->ReadMemory(addr, 8, is_gpu_thread);
        }
        return false;
    }

    template <bool track, bool is_read>
    void UpdatePageWatchers(VAddr addr, u64 size) {
        RENDERER_TRACE;

        size_t page = addr >> PM_PAGE_BITS;
        const u64 page_end = Common::DivCeil(addr + size, PM_PAGE_SIZE);

        // Acquire locks for the range of pages
        const auto lock_start = locks.begin() + (page / PAGES_PER_LOCK);
        const auto lock_end = locks.begin() + Common::DivCeil(page_end, PAGES_PER_LOCK);
        Common::RangeLockGuard lk(lock_start, lock_end);

        auto perms = cached_pages[page].Perms();
        u64 range_begin = 0;
        u64 range_bytes = 0;
        u64 potential_range_bytes = 0;

        const auto release_pending = [&] {
            if (range_bytes > 0) {
                RENDERER_TRACE;
                // Perform pending (un)protect action
                Protect(range_begin << PM_PAGE_BITS, range_bytes, perms);
                range_bytes = 0;
                potential_range_bytes = 0;
            }
        };

        // Iterate requested pages
        const u64 aligned_addr = page << PM_PAGE_BITS;
        const u64 aligned_end = page_end << PM_PAGE_BITS;
        if (!rasterizer->IsMapped(aligned_addr, aligned_end - aligned_addr)) {
            LOG_WARNING(Render,
                        "Tracking memory region {:#x} - {:#x} which is not fully GPU mapped.",
                        aligned_addr, aligned_end);
        }

        for (; page != page_end; ++page) {
            PageState& state = cached_pages[page];

            // Apply the change to the page state
            const u8 new_count = state.AddDelta<track ? 1 : -1, is_read>();

            if (auto new_perms = state.Perms(); new_perms != perms) [[unlikely]] {
                // If the protection changed add pending (un)protect action
                release_pending();
                perms = new_perms;
            } else if (range_bytes != 0) {
                // If the protection did not change, extend the potential range
                potential_range_bytes += PM_PAGE_SIZE;
            }

            // Only start a new range if the page must be (un)protected
            if ((new_count == 0 && !track) || (new_count == 1 && track)) {
                if (range_bytes == 0) {
                    // Start a new potential range
                    range_begin = page;
                    potential_range_bytes = PM_PAGE_SIZE;
                }
                // Extend current range up to potential range
                range_bytes = potential_range_bytes;
            }
        }

        // Add pending (un)protect action
        release_pending();
    }

    template <bool track, bool is_read>
    void UpdatePageWatchersForRegion(VAddr base_addr, RegionBits& mask) {
        RENDERER_TRACE;
        auto start_range = mask.FirstRange();
        auto end_range = mask.LastRange();

        if (start_range.second == end_range.second) {
            // if all pages are contiguous, use the regular UpdatePageWatchers
            const VAddr start_addr = base_addr + (start_range.first << PM_PAGE_BITS);
            const u64 size = (start_range.second - start_range.first) << PM_PAGE_BITS;
            return UpdatePageWatchers<track, is_read>(start_addr, size);
        }

        size_t base_page = (base_addr >> PM_PAGE_BITS);
        ASSERT(base_page % PAGES_PER_LOCK == 0);
        std::scoped_lock lk(locks[base_page / PAGES_PER_LOCK]);
        auto perms = cached_pages[base_page + start_range.first].Perms();
        u64 range_begin = 0;
        u64 range_bytes = 0;
        u64 potential_range_bytes = 0;

        const auto release_pending = [&] {
            if (range_bytes > 0) {
                RENDERER_TRACE;
                // Perform pending (un)protect action
                Protect((range_begin << PM_PAGE_BITS), range_bytes, perms);
                range_bytes = 0;
                potential_range_bytes = 0;
            }
        };

        // Iterate pages
        for (size_t page = start_range.first; page < end_range.second; ++page) {
            PageState& state = cached_pages[base_page + page];
            const bool update = mask.Get(page);

            // Apply the change to the page state
            const u8 new_count =
                update ? state.AddDelta<track ? 1 : -1, is_read>() : state.AddDelta<0, is_read>();

            if (auto new_perms = state.Perms(); new_perms != perms) [[unlikely]] {
                // If the protection changed add pending (un)protect action
                release_pending();
                perms = new_perms;
            } else if (range_bytes != 0) {
                // If the protection did not change, extend the potential range
                potential_range_bytes += PM_PAGE_SIZE;
            }

            // If the page is not being updated, skip it
            if (!update) {
                continue;
            }

            // If the page must be (un)protected
            if ((new_count == 0 && !track) || (new_count == 1 && track)) {
                if (range_bytes == 0) {
                    // Start a new potential range
                    range_begin = base_page + page;
                    potential_range_bytes = PM_PAGE_SIZE;
                }
                // Extend current rango up to potential range
                range_bytes = potential_range_bytes;
            }
        }

        // Add pending (un)protect action
        release_pending();
    }

    std::array<PageState, NUM_ADDRESS_PAGES> cached_pages{};
#ifdef PTHREAD_ADAPTIVE_MUTEX_INITIALIZER_NP
    using LockType = Common::AdaptiveMutex;
#else
    using LockType = Common::SpinLock;
#endif
    std::array<LockType, NUM_ADDRESS_LOCKS> locks{};
};

#ifdef __linux__
// gow3: write tracking with userfaultfd write-protection instead of mprotect. mprotect takes
// the address space lock for writing and splits mappings; while guest threads fault on
// per-frame buffers and the GPU thread re-protects uploaded pages, every page fault in the
// process (copy threads included) waits for it. UFFDIO_WRITEPROTECT changes page table bits
// under the lock for reading. Read protection (readbacks) still uses mprotect: userfaultfd
// write-protection cannot deny reads. GOW3_UFFD=1.
struct UffdImpl : public PageManager::Impl {
private:
    std::jthread ufd_thread;
    int uffd;
    std::mutex read_revoked_mutex;
    std::unordered_set<u64> read_revoked_pages; ///< 4 KiB pages denied reads with mprotect
    std::atomic<size_t> num_read_revoked{0};

public:
    UffdImpl(Vulkan::Rasterizer* rasterizer_) : Impl() {
        rasterizer = rasterizer_;
        uffd = syscall(SYS_userfaultfd, O_CLOEXEC | O_NONBLOCK | UFFD_USER_MODE_ONLY);
        if (uffd == -1) {
            LOG_ERROR(Common_Memory,
                      "userfaultfd syscall failed: {}, falling back to signal implementation",
                      Common::GetLastErrorMsg());
            throw std::runtime_error("userfaultfd");
        }

        // Guest memory is a shared memfd mapping: write-protection there needs the shmem
        // feature, and unpopulated pages must be protectable too.
        uffdio_api api{};
        api.api = UFFD_API;
        api.features =
            UFFD_FEATURE_THREAD_ID | UFFD_FEATURE_WP_HUGETLBFS_SHMEM | UFFD_FEATURE_WP_UNPOPULATED;
        if (ioctl(uffd, UFFDIO_API, &api) != 0) {
            LOG_ERROR(Common_Memory,
                      "uffdio_api call failed: {}, falling back to signal implementation",
                      Common::GetLastErrorMsg());
            close(uffd);
            throw std::runtime_error("uffdio_api");
        }

        // Read faults (readbacks) still arrive as signals.
        Core::Signals::Instance()->RegisterAccessViolationHandler(
            GuestFaultSignalHandler, std::numeric_limits<u32>::min());

        ufd_thread = std::jthread([this](std::stop_token token) { UffdHandler(token); });
        std::printf("GPU: memory tracking with userfaultfd write-protection\n");
    }

    ~UffdImpl() = default;

    void OnMap(VAddr address, size_t size) override {
        uffdio_register reg{};
        reg.range.start = address;
        reg.range.len = size;
        reg.mode = UFFDIO_REGISTER_MODE_WP;
        if (ioctl(uffd, UFFDIO_REGISTER, &reg) == -1) {
            LOG_ERROR(Common_Memory, "Uffdio register {:#x}+{:#x} failed: {}", address, size,
                      Common::GetLastErrorMsg());
        }
    }

    void OnUnmap(VAddr address, size_t size) override {
        uffdio_range range{};
        range.start = address;
        range.len = size;
        ioctl(uffd, UFFDIO_UNREGISTER, &range);
    }

    void Protect(VAddr address, size_t size, Core::MemoryPermission perms) override {
        auto& address_space = Core::Memory::Instance()->GetAddressSpace();
        const bool allow_read = True(perms & Core::MemoryPermission::Read);
        const bool allow_write = True(perms & Core::MemoryPermission::Write);
        const u64 first = address >> 12, last = (address + size + 4095) >> 12;
        if (!allow_read) {
            // Readbacks: deny all access with mprotect.
            address_space.Protect(address, size, Core::MemoryPermission::None);
            std::scoped_lock lk{read_revoked_mutex};
            for (u64 page = first; page < last; ++page) {
                read_revoked_pages.insert(page);
            }
            num_read_revoked.store(read_revoked_pages.size(), std::memory_order_release);
            return;
        }
        if (num_read_revoked.load(std::memory_order_acquire) != 0) {
            std::scoped_lock lk{read_revoked_mutex};
            bool restore = false;
            for (u64 page = first; page < last; ++page) {
                restore |= read_revoked_pages.erase(page) != 0;
            }
            num_read_revoked.store(read_revoked_pages.size(), std::memory_order_release);
            if (restore) {
                address_space.Protect(address, size, Core::MemoryPermission::ReadWrite);
            }
        }
        uffdio_writeprotect wp{};
        wp.range.start = address;
        wp.range.len = size;
        wp.mode = allow_write ? UFFDIO_WRITEPROTECT_MODE_DONTWAKE : UFFDIO_WRITEPROTECT_MODE_WP;
        if (ioctl(uffd, UFFDIO_WRITEPROTECT, &wp) == -1) {
            LOG_ERROR(Common_Memory, "Uffdio writeprotect {:#x}+{:#x} failed: {}", address, size,
                      Common::GetLastErrorMsg());
        }
    }

    void UffdHandler(std::stop_token token) {
        Common::SetCurrentThreadName("gow3:Uffd");
        while (!token.stop_requested()) {
            pollfd pollfd{};
            pollfd.fd = uffd;
            pollfd.events = POLLIN;
            // Short timeout so the stop request is seen.
            const int pollres = poll(&pollfd, 1, 100);
            if (pollres <= 0 || !(pollfd.revents & POLLIN)) {
                continue;
            }
            uffd_msg msg;
            const int readret = read(uffd, &msg, sizeof(msg));
            if (readret != sizeof(msg)) {
                continue;
            }
            if (msg.event != UFFD_EVENT_PAGEFAULT) {
                continue;
            }
            const VAddr addr = msg.arg.pagefault.address;
            const auto ptid = msg.arg.pagefault.feat.ptid;
            {
                Gow3Stats::Timer timer{Gow3Stats::t_write_faults};
                rasterizer->OnWriteFault(addr, rasterizer->IsGpuSideThreadId(ptid));
            }
            // Protect() clears with DONTWAKE (it may run for pages nobody waits on).
            uffdio_range wake{};
            wake.start = addr & ~u64(PM_PAGE_SIZE - 1);
            wake.len = PM_PAGE_SIZE;
            ioctl(uffd, UFFDIO_WAKE, &wake);
        }
    }
};
#endif // __linux__

struct SignalImpl : public PageManager::Impl {
    SignalImpl(Vulkan::Rasterizer* rasterizer_) : Impl() {
        rasterizer = rasterizer_;

        // Should be called first.
        constexpr auto priority = std::numeric_limits<u32>::min();
        Core::Signals::Instance()->RegisterAccessViolationHandler(GuestFaultSignalHandler,
                                                                  priority);
    }

    void Protect(VAddr address, size_t size, Core::MemoryPermission perms) override {
        RENDERER_TRACE;
        auto* memory = Core::Memory::Instance();
        auto& impl = memory->GetAddressSpace();
        ASSERT_MSG(perms != Core::MemoryPermission::Write,
                   "Attempted to protect region as write-only which is not a valid permission");
        impl.Protect(address, size, perms);
    }

};

#ifdef __SWITCH__
// gow3 (Switch 2): Horizon never delivers a fault to the process (switch2/PLAN.md), so a page
// is never really write-protected. A page the trackers want to watch for CPU writes keeps a
// hash of its contents instead, and before each submission (Liverpool::SubmitGfx/SubmitAsc,
// on the submitting guest thread, as a fault handler would run) every watched page is hashed
// again: a page that changed is handled as a write fault on it would have been
// (Rasterizer::InvalidateMemory), which also ends the watch. Writes are seen at the next
// submission, which is when the GPU could first use them.
// Read watches (readbacks of GPU-written data when the CPU reads it) cannot be emulated: they
// are counted and the CPU reads the memory as it is.
struct HashImpl : public PageManager::Impl {
    HashImpl(Vulkan::Rasterizer* rasterizer_) : Impl() {
        rasterizer = rasterizer_;
        std::printf("GPU: memory tracking by page hashes before each submission (no faults on Horizon)\n");
    }

    static u64 HashPage(VAddr page_addr) {
        return XXH3_64bits(reinterpret_cast<const void*>(page_addr), PM_PAGE_SIZE);
    }

    void Protect(VAddr address, size_t size, Core::MemoryPermission perms) override {
        const VAddr first = address >> PM_PAGE_BITS, end = (address + size + PM_PAGE_SIZE - 1) >> PM_PAGE_BITS;
        if (!True(perms & Core::MemoryPermission::Read)) {
            read_watches.fetch_add(end - first, std::memory_order_relaxed);
        }
        const bool watch = !True(perms & Core::MemoryPermission::Write);
        // Hashed outside the lock; a page that is not mapped is not watched (reading it would
        // end the process).
        thread_local std::vector<std::pair<u64, u64>> hashed;
        hashed.clear();
        if (watch && runtime_memory_is_mapped(first << PM_PAGE_BITS, (end - first) << PM_PAGE_BITS)) {
            for (VAddr page = first; page < end; ++page) {
                hashed.emplace_back(page, HashPage(page << PM_PAGE_BITS));
            }
        }
        std::scoped_lock lock{watch_mutex};
        if (watch) {
            for (const auto& [page, hash] : hashed) {
                watched[page] = hash;
                if (changed_before.contains(page)) {
                    hot[page] = hash;
                }
            }
        } else {
            for (VAddr page = first; page < end; ++page) {
                watched.erase(page);
                hot.erase(page);
            }
        }
    }

    // Each check hashes every "hot" page (one that changed before: data the game rewrites) and
    // the next budget's worth of the other watched pages, round-robin, so a write to a page the
    // game rarely touches is seen within a few submissions instead of hashing everything each
    // time (226 MiB: 20 ms per submission). GOW3_HASH_BUDGET_MIB (default 8; 0: everything).
    static size_t BudgetPages() {
        static const size_t pages = [] {
            const char* v = std::getenv("GOW3_HASH_BUDGET_MIB");
            const long mib = v ? std::atol(v) : 8;
            return mib <= 0 ? SIZE_MAX : size_t(mib) * 1048576 / PM_PAGE_SIZE;
        }();
        return pages;
    }

    void CheckGuestWrites() override {
        std::scoped_lock scan_lock{scan_mutex};
        const auto begin = std::chrono::steady_clock::now();
        size_t watched_now = 0;
        {
            std::scoped_lock lock{watch_mutex};
            watched_now = watched.size();
            pages.clear();
            for (const auto& entry : hot) {
                pages.push_back(entry);
            }
            const size_t budget = std::min(BudgetPages(), watched.size());
            auto it = watched.lower_bound(cursor);
            for (size_t n = 0; n < budget; ++n) {
                if (it == watched.end()) {
                    it = watched.begin();
                }
                if (!hot.contains(it->first)) {
                    pages.push_back(*it);
                }
                ++it;
            }
            cursor = it == watched.end() ? 0 : it->first;
        }
        std::sort(pages.begin(), pages.end());
        pages.erase(std::unique(pages.begin(), pages.end()), pages.end());
        changed.assign(pages.size(), 0);
        // Runs of contiguous pages, each checked as mapped once, hashed in parallel chunks.
        constexpr size_t Chunk = 256;
        const size_t chunks = (pages.size() + Chunk - 1) / Chunk;
        const auto hash_chunk = [this](size_t c) {
            const size_t from = c * Chunk, to = std::min(pages.size(), from + Chunk);
            for (size_t i = from; i < to;) {
                size_t j = i + 1;
                while (j < to && pages[j].first == pages[j - 1].first + 1) {
                    ++j;
                }
                const VAddr run = pages[i].first << PM_PAGE_BITS;
                if (runtime_memory_is_mapped(run, (j - i) << PM_PAGE_BITS)) {
                    for (size_t k = i; k < j; ++k) {
                        changed[k] = HashPage(pages[k].first << PM_PAGE_BITS) != pages[k].second;
                    }
                }
                i = j;
            }
        };
        if (chunks > 1 && Gow3Copy::Enabled()) {
            Gow3Copy::ParallelFor(chunks, hash_chunk);
        } else {
            for (size_t c = 0; c < chunks; ++c) {
                hash_chunk(c);
            }
        }
        // Each changed run is one write "fault" (the watch lock is not held: the invalidation
        // ends the watches through Protect).
        u64 changed_pages = 0;
        for (size_t i = 0; i < pages.size();) {
            if (!changed[i]) {
                ++i;
                continue;
            }
            size_t j = i + 1;
            while (j < pages.size() && changed[j] && pages[j].first == pages[j - 1].first + 1) {
                ++j;
            }
            {
                std::scoped_lock lock{watch_mutex};
                for (size_t k = i; k < j; ++k) {
                    changed_before.insert(pages[k].first);
                }
            }
            changed_pages += j - i;
            rasterizer->InvalidateMemory(pages[i].first << PM_PAGE_BITS, (j - i) << PM_PAGE_BITS);
            i = j;
        }
        const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                            std::chrono::steady_clock::now() - begin).count();
        scans++;
        scan_ns += u64(ns);
        scanned_pages += pages.size();
        changed_total += changed_pages;
        max_watched = std::max<u64>(max_watched, watched_now);
        if (begin - last_report > std::chrono::seconds(5)) {
            std::printf("GPU: write checks: %llu submissions, %.1f MiB hashed on average, %.1f MiB watched, "
                        "%zu hot pages, %.2f ms per check, %llu pages changed, %llu read watches ignored\n",
                        (unsigned long long)scans,
                        scans ? double(scanned_pages) / double(scans) * PM_PAGE_SIZE / 1048576.0 : 0.0,
                        double(max_watched) * PM_PAGE_SIZE / 1048576.0, hot.size(),
                        scans ? double(scan_ns) / double(scans) / 1e6 : 0.0,
                        (unsigned long long)changed_total,
                        (unsigned long long)read_watches.load(std::memory_order_relaxed));
            last_report = begin;
            scans = scan_ns = scanned_pages = changed_total = max_watched = 0;
        }
    }

    std::mutex watch_mutex, scan_mutex;
    std::map<u64, u64> watched;            ///< page -> hash of its contents when the watch began
    std::unordered_map<u64, u64> hot;      ///< watched pages that changed before: checked every time
    std::unordered_set<u64> changed_before; ///< pages a check found changed
    u64 cursor{};                          ///< where the next round-robin budget starts
    std::vector<std::pair<u64, u64>> pages;
    std::vector<u8> changed;
    std::atomic<u64> read_watches{};
    u64 scans{}, scan_ns{}, scanned_pages{}, changed_total{}, max_watched{};
    std::chrono::steady_clock::time_point last_report{};
};
#endif

PageManager::PageManager(Vulkan::Rasterizer* rasterizer_) {
#ifdef __SWITCH__
    impl = std::make_unique<HashImpl>(rasterizer_);
    return;
#endif
#ifdef __linux__
    if (std::getenv("GOW3_UFFD") && std::getenv("GOW3_UFFD")[0] == '1') {
        try {
            impl = std::make_unique<UffdImpl>(rasterizer_);
            LOG_INFO(Config, "Memory tracking method: userfaultfd");
            return;
        } catch (const std::runtime_error& e) {
            // if uffd is unsupported, falls back to SignalImpl
        }
    }
    LOG_INFO(Config, "Memory tracking method: signals");
#endif
    impl = std::make_unique<SignalImpl>(rasterizer_);
}

PageManager::~PageManager() = default;

void PageManager::CheckGuestWrites() {
    impl->CheckGuestWrites();
}

void PageManager::OnGpuMap(VAddr address, size_t size) {
    impl->OnMap(address, size);
}

void PageManager::OnGpuUnmap(VAddr address, size_t size) {
    impl->OnUnmap(address, size);
}

template <bool track, bool is_read>
void PageManager::UpdatePageWatchers(VAddr addr, u64 size) const {
    impl->UpdatePageWatchers<track, is_read>(addr, size);
}

template <bool track, bool is_read>
void PageManager::UpdatePageWatchersForRegion(VAddr base_addr, RegionBits& mask) const {
    impl->UpdatePageWatchersForRegion<track, is_read>(base_addr, mask);
}

template void PageManager::UpdatePageWatchers<true>(VAddr addr, u64 size) const;
template void PageManager::UpdatePageWatchers<false>(VAddr addr, u64 size) const;
template void PageManager::UpdatePageWatchers<true, true>(VAddr addr, u64 size) const;
template void PageManager::UpdatePageWatchers<false, true>(VAddr addr, u64 size) const;
template void PageManager::UpdatePageWatchersForRegion<true, true>(VAddr base_addr,
                                                                   RegionBits& mask) const;
template void PageManager::UpdatePageWatchersForRegion<true, false>(VAddr base_addr,
                                                                    RegionBits& mask) const;
template void PageManager::UpdatePageWatchersForRegion<false, true>(VAddr base_addr,
                                                                    RegionBits& mask) const;
template void PageManager::UpdatePageWatchersForRegion<false, false>(VAddr base_addr,
                                                                     RegionBits& mask) const;

} // namespace VideoCore
