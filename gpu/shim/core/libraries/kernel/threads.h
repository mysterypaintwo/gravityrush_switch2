// gow3: host threads that may call guest code (AvPlayer allocator callbacks).
// Each thread gets a guest TCB (GS base, TLS) from the C runtime before running.
#pragma once
#include <atomic>
#include <chrono>
#include <cstdio>
#include <functional>
#include <memory>
#include <mutex>
#include <stop_token>
#include <system_error>
#include <thread>
#include "common/types.h"

extern "C" void runtime_thread_attach_host(const char* name);

namespace Libraries::Kernel {
class Thread {
public:
    Thread() = default;
    ~Thread() {
        // A thread destroying its own Thread object (AvPlayer does) cannot join itself.
        if (current == this) {
            std::scoped_lock lk{lock};
            if (thread.joinable()) {
                thread.request_stop();
                thread.detach();
            }
            has_thread = false;
            return;
        }
        Stop();
    }
    void Run(std::function<void(std::stop_token)>&& func) {
        std::scoped_lock lk{lock};
        finished = std::make_shared<std::atomic<bool>>(false);
        thread = std::jthread([this, func = std::move(func), done = finished](std::stop_token stop) {
            current = this;
            runtime_thread_attach_host("gow3:hle");
            func(stop);
            done->store(true);
            current = nullptr;
        });
        has_thread = true;
    }
    // gow3: AvPlayer's threads Join their own Thread object as they return, while the demuxer
    // joins the decoders at the end of the file and sceAvPlayerStop joins all three from the
    // game thread. A std::jthread is not safe to join and detach from several threads at once:
    // the thread detached itself while the game thread was in pthread_join, which then read the
    // freed thread (the crash when the first gameplay movie stops, on the Mac). So:
    // - a thread joining itself does nothing (it is returning; whoever stops it joins it),
    // - one caller takes the std::jthread out under the lock and joins it outside the lock,
    //   others wait for the thread's function to return; no lock is held while waiting (a
    //   decoder can be waiting for the game thread, which may be asking Joinable()).
    // These are called from guest code, which a C++ exception cannot unwind: a failed join is
    // logged and the thread detached instead.
    void Join() {
        if (current == this) return;
        std::jthread owned;
        std::shared_ptr<std::atomic<bool>> done;
        {
            std::scoped_lock lk{lock};
            if (thread.joinable()) owned = std::move(thread);
            done = finished;
            has_thread = false;
        }
        if (owned.joinable()) {
            try {
                owned.join();
                return;
            } catch (const std::system_error& error) {
                // Windows (libc++ on Win32 threads): the join can fail although the thread is
                // fine. Wait for its function to return, so nothing it uses is freed under it.
                std::fprintf(stderr, "HLE thread: join failed (%s); waiting for it to finish\n", error.what());
            }
        }
        for (int waited = 0; done && !done->load() && waited < 5000; ++waited) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        if (owned.joinable()) owned.detach();
    }
    bool Joinable() const { return has_thread.load(); }
    void Stop() {
        {
            std::scoped_lock lk{lock};
            if (!thread.joinable()) return;
            thread.request_stop();
        }
        // A thread stopping itself only asks its loop to end; whoever stops it next joins it.
        if (current != this) Join();
    }

private:
    static inline thread_local Thread* current = nullptr; // the Thread object running this thread
    std::mutex lock; // guards `thread` and `finished`, never held while waiting
    std::jthread thread;
    std::shared_ptr<std::atomic<bool>> finished;
    std::atomic<bool> has_thread{false};
};
} // namespace Libraries::Kernel
