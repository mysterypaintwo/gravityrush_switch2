// SPDX-License-Identifier: GPL-2.0-or-later
// gow3: calls through function pointers the game hands to the HLE libraries (memory hooks,
// AvPlayer allocators and events). On x86-64 hosts the game's code runs natively, so these are
// plain calls. On ARM64 hosts (switch2/host) the game runs under Box64: a pointer into the
// game's code goes through runtime_guest_call, any other pointer is a host function and is
// called directly. Integer, pointer and enum arguments only (up to 6).
#pragma once
#include <cstdint>
#include <type_traits>

#if defined(__aarch64__)
extern "C" std::uint64_t runtime_guest_call(std::uintptr_t fn, int nargs, ...);
extern "C" int runtime_is_guest_code(std::uintptr_t address);

template <typename T>
inline std::uint64_t Gow3GuestArg(T v) {
    if constexpr (std::is_pointer_v<T> || std::is_null_pointer_v<T>) {
        return static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(v));
    } else if constexpr (std::is_enum_v<T>) {
        return static_cast<std::uint64_t>(static_cast<std::int64_t>(static_cast<std::underlying_type_t<T>>(v)));
    } else if constexpr (std::is_signed_v<T>) {
        return static_cast<std::uint64_t>(static_cast<std::int64_t>(v));
    } else {
        return static_cast<std::uint64_t>(v);
    }
}
#endif

template <typename Fn, typename... Args>
inline auto Gow3GuestCall(Fn fn, Args... args) -> std::invoke_result_t<Fn, Args...> {
    using R = std::invoke_result_t<Fn, Args...>;
#if defined(__aarch64__)
    if (runtime_is_guest_code(reinterpret_cast<std::uintptr_t>(fn))) {
        static_assert(sizeof...(Args) <= 6, "guest calls take up to 6 arguments");
        static_assert(((std::is_integral_v<Args> || std::is_pointer_v<Args> || std::is_enum_v<Args> ||
                        std::is_null_pointer_v<Args>) && ...),
                      "guest calls take integer, pointer and enum arguments");
        const std::uint64_t r = runtime_guest_call(reinterpret_cast<std::uintptr_t>(fn),
                                                   static_cast<int>(sizeof...(Args)), Gow3GuestArg(args)...);
        if constexpr (std::is_void_v<R>) {
            return;
        } else if constexpr (std::is_pointer_v<R>) {
            return reinterpret_cast<R>(static_cast<std::uintptr_t>(r));
        } else {
            return static_cast<R>(r);
        }
    }
#endif
    return fn(args...);
}
