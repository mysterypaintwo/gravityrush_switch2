// SPDX-License-Identifier: GPL-2.0-or-later
// gow3 (Switch 2): the part of boost::asio the event queue's timers use (equeue.cpp: a steady
// timer per timer event, one async_wait at a time, run by the kernel service thread in
// gow3gpu.cpp). Asio itself needs sockets and scatter I/O, which newlib on Horizon does not
// have. Destroying a timer cancels its wait without calling the handler (asio would call it
// with operation_aborted, which equeue only logs).
#pragma once
#include <chrono>
#include <condition_variable>
#include <functional>
#include <list>
#include <mutex>
#include <string>

namespace Gow3Timers {

enum class errc { success, operation_canceled };
struct error_code {
    errc value = errc::success;
    explicit operator bool() const { return value != errc::success; }
    bool operator==(errc e) const { return value == e; }
    std::string message() const { return value == errc::success ? "success" : "canceled"; }
};

class steady_timer;

class io_context {
public:
    using clock = std::chrono::steady_clock;
    /// Runs the handlers as their times come, until no wait is left.
    void run() {
        std::unique_lock lock{mutex};
        while (!waits.empty()) {
            auto first = waits.begin();
            for (auto it = waits.begin(); it != waits.end(); ++it) {
                if (it->when < first->when) first = it;
            }
            if (clock::now() < first->when) {
                cv.wait_until(lock, first->when); // a new or cancelled wait wakes it early
                continue;
            }
            auto handler = std::move(first->handler);
            waits.erase(first);
            lock.unlock();
            handler(error_code{});
            lock.lock();
        }
    }
    void restart() {}

private:
    friend class steady_timer;
    struct Wait {
        steady_timer* timer;
        clock::time_point when;
        std::function<void(const error_code&)> handler;
    };
    void add(steady_timer* t, clock::time_point when, std::function<void(const error_code&)> h) {
        std::scoped_lock lock{mutex};
        waits.push_back({t, when, std::move(h)});
        cv.notify_all();
    }
    void cancel(steady_timer* t) {
        std::scoped_lock lock{mutex};
        waits.remove_if([t](const Wait& w) { return w.timer == t; });
        cv.notify_all();
    }
    std::mutex mutex;
    std::condition_variable cv;
    std::list<Wait> waits;
};

class steady_timer {
public:
    steady_timer(io_context& context_, std::chrono::nanoseconds after)
        : context{context_}, deadline{io_context::clock::now() + after} {}
    ~steady_timer() { context.cancel(this); }
    steady_timer(const steady_timer&) = delete;
    steady_timer& operator=(const steady_timer&) = delete;
    io_context::clock::time_point expiry() const { return deadline; }
    void expires_at(io_context::clock::time_point when) { deadline = when; }
    template <typename Handler>
    void async_wait(Handler&& handler) {
        context.add(this, deadline, std::forward<Handler>(handler));
    }

private:
    io_context& context;
    io_context::clock::time_point deadline;
};

} // namespace Gow3Timers
