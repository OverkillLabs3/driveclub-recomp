// SPDX-License-Identifier: GPL-2.0-or-later
// driveclub: which 16-byte blocks of guest memory hold bytes the GPU wrote and the guest has not been given yet, readable
// from any thread without locks (BB_FAULT_EMULATE). The owner of the buffer cache marks blocks when it records a GPU
// write and when ranges return from a readback; downloads clear the blocks they fully delivered. A guest access to
// bytes in unmarked blocks needs no download even though its page is protected for other bytes.

#pragma once

#include <array>
#include <atomic>
#include <memory>

#include "common/types.h"

namespace VideoCore {

class GpuByteMap {
public:
    static constexpr u32 BlockShift = 4;    // 16 bytes
    static constexpr u32 RegionShift = 22;  // 4 MiB per lazily allocated region
    static constexpr u32 AddressBits = 40;
    static constexpr u64 WordsPerRegion = (1ull << (RegionShift - BlockShift)) / 64;

    GpuByteMap() : top{std::make_unique<std::atomic<std::atomic<u64>*>[]>(NumRegions)} {}
    ~GpuByteMap() {
        for (u64 i = 0; i < NumRegions; ++i) {
            delete[] top[i].load(std::memory_order_relaxed);
        }
    }

    /// Every block that overlaps the range (owner thread).
    void Mark(VAddr addr, u64 size) {
        if (size == 0) {
            return;
        }
        ForBlocks(addr >> BlockShift, (addr + size - 1) >> BlockShift, true,
                  [](std::atomic<u64>& word, u64 bits) {
                      if ((word.load(std::memory_order_relaxed) & bits) != bits) {
                          word.fetch_or(bits, std::memory_order_release);
                      }
                  });
    }

    /// Only the blocks the range covers completely (a partly covered block may hold other GPU bytes).
    void Clear(VAddr addr, u64 size) {
        const u64 first = (addr + (1ull << BlockShift) - 1) >> BlockShift;
        const u64 end = (addr + size) >> BlockShift;
        if (end <= first) {
            return;
        }
        ForBlocks(first, end - 1, false,
                  [](std::atomic<u64>& word, u64 bits) { word.fetch_and(~bits, std::memory_order_release); });
    }

    /// Whether any block overlapping the range is marked (any thread).
    [[nodiscard]] bool Any(VAddr addr, u64 size) const {
        if (size == 0) {
            return false;
        }
        bool any = false;
        const_cast<GpuByteMap*>(this)->ForBlocks(
            addr >> BlockShift, (addr + size - 1) >> BlockShift, false,
            [&](std::atomic<u64>& word, u64 bits) { any = any || (word.load(std::memory_order_acquire) & bits); });
        return any;
    }

private:
    static constexpr u64 NumRegions = 1ull << (AddressBits - RegionShift);

    template <typename Func>
    void ForBlocks(u64 first, u64 last, bool create, Func&& func) {
        for (u64 block = first; block <= last;) {
            const u64 region = block >> (RegionShift - BlockShift);
            if (region >= NumRegions) {
                return;
            }
            const u64 region_end = (region + 1) << (RegionShift - BlockShift);
            const u64 stop = std::min(last + 1, region_end);
            std::atomic<u64>* words = top[region].load(std::memory_order_acquire);
            if (!words && create) {
                auto* fresh = new std::atomic<u64>[WordsPerRegion]();
                std::atomic<u64>* expected = nullptr;
                if (top[region].compare_exchange_strong(expected, fresh, std::memory_order_acq_rel)) {
                    words = fresh;
                } else {
                    delete[] fresh;
                    words = expected;
                }
            }
            if (words) {
                for (u64 b = block; b < stop;) {
                    const u64 local = b - (region << (RegionShift - BlockShift));
                    const u64 bit = local & 63;
                    const u64 count = std::min<u64>(64 - bit, stop - b);
                    const u64 bits = (count == 64 ? ~0ull : ((1ull << count) - 1)) << bit;
                    func(words[local >> 6], bits);
                    b += count;
                }
            }
            block = stop;
        }
    }

    std::unique_ptr<std::atomic<std::atomic<u64>*>[]> top;
};

} // namespace VideoCore
