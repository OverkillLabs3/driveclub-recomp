// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: equality of fixed-size, padding-free structs compared on every draw (render state,
// pipeline key). The Windows CRT memcmp is an out-of-line call that compares 8 bytes per step
// with byte loops for the head and tail; this compares 16-byte lanes inline and branch-free.

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#if defined(__SSE2__) || defined(_M_X64)
#include <emmintrin.h>
#endif

namespace Common {

#if !(defined(__SSE2__) || defined(_M_X64))
// bbport: hosts without SSE2 (AArch64): 8-byte lanes, the compiler vectorizes the loop.
template <std::size_t Size>
[[gnu::always_inline]] inline bool BytesEqual(const void* a, const void* b) noexcept {
    const auto* pa = static_cast<const unsigned char*>(a);
    const auto* pb = static_cast<const unsigned char*>(b);
    std::uint64_t diff = 0;
    std::size_t i = 0;
    for (; i + 8 <= Size; i += 8) {
        std::uint64_t x, y;
        std::memcpy(&x, pa + i, 8);
        std::memcpy(&y, pb + i, 8);
        diff |= x ^ y;
    }
    for (; i < Size; ++i) {
        diff |= pa[i] ^ pb[i];
    }
    return diff == 0;
}
#else
template <std::size_t Size>
[[gnu::always_inline]] inline bool BytesEqual(const void* a, const void* b) noexcept {
    const auto* pa = static_cast<const unsigned char*>(a);
    const auto* pb = static_cast<const unsigned char*>(b);
    __m128i diff = _mm_setzero_si128();
    std::size_t i = 0;
    for (; i + 16 <= Size; i += 16) {
        const __m128i x = _mm_loadu_si128(reinterpret_cast<const __m128i*>(pa + i));
        const __m128i y = _mm_loadu_si128(reinterpret_cast<const __m128i*>(pb + i));
        diff = _mm_or_si128(diff, _mm_xor_si128(x, y));
    }
    std::uint64_t tail = 0;
    if constexpr (Size % 16 >= 8) {
        std::uint64_t x, y;
        std::memcpy(&x, pa + i, 8);
        std::memcpy(&y, pb + i, 8);
        tail |= x ^ y;
        i += 8;
    }
    if constexpr (Size % 8 >= 4) {
        std::uint32_t x, y;
        std::memcpy(&x, pa + i, 4);
        std::memcpy(&y, pb + i, 4);
        tail |= x ^ y;
        i += 4;
    }
    for (; i < Size; ++i) {
        tail |= pa[i] ^ pb[i];
    }
    return _mm_movemask_epi8(_mm_cmpeq_epi8(diff, _mm_setzero_si128())) == 0xFFFF && tail == 0;
}
#endif

} // namespace Common
