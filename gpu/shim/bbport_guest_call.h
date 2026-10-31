// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: a call of the game's code (a function pointer the game handed over: callbacks, the
// game's allocators and getters) from host code. A plain call when the game runs on the host CPU
// (x86-64), through the guest CPU (src/guest_cpu.h, FEXCore) otherwise. Integer, enum and pointer
// arguments and results only (a structure or a float argument fails to compile on such hosts).
#pragma once

#include <cstdint>
#include <cstring>
#include <type_traits>

#include "../../src/guest_cpu.h"

namespace BbGuest {
#ifndef GUEST_CPU_NATIVE
template <typename P>
std::uint64_t Pack(P value) {
    static_assert(std::is_integral_v<P> || std::is_enum_v<P> || std::is_pointer_v<P> ||
                      std::is_null_pointer_v<P>,
                  "guest calls take integer, enum and pointer arguments only");
    if constexpr (std::is_pointer_v<P>) {
        return reinterpret_cast<std::uintptr_t>(value);
    } else if constexpr (std::is_null_pointer_v<P>) {
        return 0;
    } else {
        return static_cast<std::uint64_t>(value);
    }
}

template <typename R>
R Unpack(std::uint64_t rax) {
    if constexpr (std::is_void_v<R>) {
        return;
    } else if constexpr (std::is_pointer_v<R>) {
        return reinterpret_cast<R>(static_cast<std::uintptr_t>(rax));
    } else if constexpr (std::is_same_v<R, bool>) {
        return (rax & 0xff) != 0; // only AL is defined
    } else {
        static_assert(std::is_integral_v<R> || std::is_enum_v<R>,
                      "guest calls return integers, enums and pointers only");
        // Only the low bytes of RAX are defined for narrower results.
        if constexpr (sizeof(R) == 1) {
            return static_cast<R>(static_cast<std::uint8_t>(rax));
        } else if constexpr (sizeof(R) == 2) {
            return static_cast<R>(static_cast<std::uint16_t>(rax));
        } else if constexpr (sizeof(R) == 4) {
            return static_cast<R>(static_cast<std::uint32_t>(rax));
        } else {
            return static_cast<R>(rax);
        }
    }
}

template <typename R, typename... P, typename... A>
R Call(R (*fn)(P...), A... args) {
    static_assert(sizeof...(P) == sizeof...(A), "argument count");
    const std::uint64_t packed[sizeof...(P) ? sizeof...(P) : 1] = {Pack<P>(P(args))...};
    return Unpack<R>(guest_cpu_call(reinterpret_cast<std::uintptr_t>(fn), sizeof...(P), packed));
}
#endif
} // namespace BbGuest

template <typename F, typename... A>
decltype(auto) GuestCall(F fn, A&&... args) {
#ifdef GUEST_CPU_NATIVE
    return fn(static_cast<A&&>(args)...);
#else
    return BbGuest::Call(fn, args...);
#endif
}
