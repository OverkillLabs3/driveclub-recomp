// SPDX-License-Identifier: GPL-2.0-or-later
// MD5, for checking extracted files against the archive index (not for security).
#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <string>

namespace dcmod {

class Md5 {
public:
    Md5() { Reset(); }

    void Reset() {
        state_ = {0x67452301u, 0xefcdab89u, 0x98badcfeu, 0x10325476u};
        length_ = 0;
        used_ = 0;
    }

    void Update(const uint8_t* data, size_t size) {
        length_ += size;
        while (size) {
            const size_t take = std::min(size, size_t(64) - used_);
            std::memcpy(block_ + used_, data, take);
            used_ += take;
            data += take;
            size -= take;
            if (used_ == 64) {
                Block(block_);
                used_ = 0;
            }
        }
    }

    std::array<uint8_t, 16> Final() {
        const uint64_t bits = length_ * 8;
        const uint8_t pad = 0x80;
        Update(&pad, 1);
        const uint8_t zero = 0;
        while (used_ != 56) Update(&zero, 1);
        uint8_t len[8];
        for (int i = 0; i < 8; ++i) len[i] = uint8_t(bits >> (8 * i));
        Update(len, 8);
        std::array<uint8_t, 16> out{};
        for (int i = 0; i < 4; ++i) {
            for (int j = 0; j < 4; ++j) out[size_t(i * 4 + j)] = uint8_t(state_[size_t(i)] >> (8 * j));
        }
        return out;
    }

    static std::string Hex(const std::array<uint8_t, 16>& d) {
        static const char* digits = "0123456789abcdef";
        std::string s;
        for (uint8_t b : d) {
            s.push_back(digits[b >> 4]);
            s.push_back(digits[b & 15]);
        }
        return s;
    }

private:
    static uint32_t Rol(uint32_t x, int c) { return (x << c) | (x >> (32 - c)); }

    void Block(const uint8_t* p) {
        static const uint32_t k[64] = {
            0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
            0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
            0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
            0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
            0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
            0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
            0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
            0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391};
        static const int s[64] = {7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 5, 9,  14, 20, 5, 9,
                                  14, 20, 5, 9,  14, 20, 5, 9,  14, 20, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
                                  4, 11, 16, 23, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21};
        uint32_t m[16];
        for (int i = 0; i < 16; ++i) m[i] = uint32_t(p[i * 4]) | (uint32_t(p[i * 4 + 1]) << 8) | (uint32_t(p[i * 4 + 2]) << 16) | (uint32_t(p[i * 4 + 3]) << 24);
        uint32_t a = state_[0], b = state_[1], c = state_[2], d = state_[3];
        for (int i = 0; i < 64; ++i) {
            uint32_t f;
            int g;
            if (i < 16) {
                f = (b & c) | (~b & d);
                g = i;
            } else if (i < 32) {
                f = (d & b) | (~d & c);
                g = (5 * i + 1) % 16;
            } else if (i < 48) {
                f = b ^ c ^ d;
                g = (3 * i + 5) % 16;
            } else {
                f = c ^ (b | ~d);
                g = (7 * i) % 16;
            }
            const uint32_t tmp = d;
            d = c;
            c = b;
            b = b + Rol(a + f + k[i] + m[g], s[i]);
            a = tmp;
        }
        state_[0] += a;
        state_[1] += b;
        state_[2] += c;
        state_[3] += d;
    }

    std::array<uint32_t, 4> state_{};
    uint64_t length_ = 0;
    uint8_t block_[64]{};
    size_t used_ = 0;
};

}  // namespace dcmod
