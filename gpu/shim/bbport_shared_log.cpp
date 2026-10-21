// driveclub: BB_SHARED_LOG=<file> (off by default): which allocations share
// 4 KB pages with GPU-written bytes in the game's CustomHeap(SystemShared) (0x3000000000, 49 MB). The loader retargets
// the six call/jmp sites of its allocator (eboot FUN_00753a80) to Allocate below, which calls the allocator and logs
//   A t_ms site size align tag address ret0 ret1 ret2 ret3
// (ret*: eboot addresses found on the guest stack, nearest first: for the trueSKY tail-call sites ret0 is the trueSKY
// caller). BufferCache::StampGpuWrite logs each distinct GPU write range in the heap once:
//   W t_ms lo hi writer
// (writer: shader hash ^ stage << 60, 0xF111.. fill, 0xC0B7.. CP DMA; as BB_READBACK_LOG).
// BB_SHARED_PAD=<mask> (default 3, both bits; BB_SHARED_PAD=0 turns it off): bit 1, the engine's GPU-buffer payloads (FUN_00758450, sites 0x75848e and
// 0x7584c4: every GPU-written range of the heap in the logged race) are rounded up to whole 4 KB pages, 4 KB aligned, so
// no page holds both GPU-written bytes and CPU objects (trueSKY keyframers): those pages are then never read-protected.
// Bit 2: the same for the 4-byte GDS counts in VideoShared (site 0x7584e1, FUN_00753b40): the two slots' counts of a
// buffer object otherwise share a page, so reading one count waits for the GPU writing the other.
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <unordered_set>
#include "../bbgpu.h"
#include "bbport_shared_log.h"
#include "common/types.h"

namespace {
using Alloc = u64(PS4_SYSV_ABI*)(u32 size, u32 align, u32 tag);
Alloc g_real = nullptr;
Alloc g_real_video = nullptr; // FUN_00753b40, CustomHeap(VideoShared): the 4-byte GDS counts (site 0x7584e1)
u32 g_pad = 0;                // BB_SHARED_PAD mask: 1 payloads (0x75848e, 0x7584c4), 2 counts (0x7584e1)
std::atomic<u64> g_padded{0}, g_pad_bytes{0}, g_misaligned{0}, g_failed{0};
uintptr_t g_image = 0;
constexpr u64 kImageCode = 0x1560000; // eboot text ends at 0x1551ab8
std::mutex g_mutex;
FILE* g_file = nullptr;
std::unordered_set<u64> g_writes; // GPU thread only

double NowMs() {
    using namespace std::chrono;
    return duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count() / 1000.0;
}

FILE* File() {
    static FILE* f = [] {
        const char* path = std::getenv("BB_SHARED_LOG");
        FILE* file = path && path[0] ? std::fopen(path, "w") : nullptr;
        if (file) {
            std::setvbuf(file, nullptr, _IOFBF, 1 << 16);
            std::fprintf(file, "# A t_ms site size align tag address ret0 ret1 ret2 ret3 | W t_ms lo hi writer\n");
        }
        return file;
    }();
    return f;
}

u64 PS4_SYSV_ABI Allocate(u32 size, u32 align, u32 tag, u32 site) {
    const u32 asked = size;
    // Not padded: size 0 (kept as the game asked) and sizes that would overflow when rounded.
    const bool padded = (((g_pad & 1) && (site == 0x75848e || site == 0x7584c4)) || ((g_pad & 2) && site == 0x7584e1)) &&
                        size != 0 && size <= 0xfffff000u;
    if (padded) {
        size = (size + 4095u) & ~4095u;
        align = align > 4096 ? align : 4096;
    }
    const u64 result = (site == 0x7584e1 ? g_real_video : g_real)(size, align, tag);
    if (padded && result == 0) {
        g_failed.fetch_add(1, std::memory_order_relaxed);
        std::printf("BB_SHARED_PAD: allocation of %u (asked %u) at site %x FAILED (%llu failures)\n", size, asked, site,
                    (unsigned long long)g_failed.load());
    } else if (padded) {
        const u64 n = g_padded.fetch_add(1, std::memory_order_relaxed) + 1;
        g_pad_bytes.fetch_add(size - asked, std::memory_order_relaxed);
        if (result & 4095) {
            g_misaligned.fetch_add(1, std::memory_order_relaxed);
        }
        if (n <= 3 || n % 50 == 0 || (result & 4095)) {
            std::printf("BB_SHARED_PAD: %llu payloads padded (+%llu bytes), %llu not page-aligned; last %u -> %u bytes at %#llx\n",
                        (unsigned long long)n, (unsigned long long)g_pad_bytes.load(), (unsigned long long)g_misaligned.load(),
                        asked, size, (unsigned long long)result);
        }
    }
    if (!g_file) {
        return result;
    }
    // Return addresses: eboot code addresses on the guest stack above this frame (a scan, so a stale value can appear).
    u64 rets[4] = {};
    int n = 0;
    const auto* sp = reinterpret_cast<const u64*>(__builtin_frame_address(0));
    for (int i = 0; i < 96 && n < 4; ++i) {
        const u64 v = sp[i];
        if (v > g_image && v - g_image < kImageCode) {
            rets[n++] = v - g_image;
        }
    }
    std::scoped_lock lk{g_mutex};
    std::fprintf(g_file, "A %.3f %x %u %u %u %llx %llx %llx %llx %llx\n", NowMs(), site, size, align, tag,
                 (unsigned long long)result, (unsigned long long)rets[0], (unsigned long long)rets[1],
                 (unsigned long long)rets[2], (unsigned long long)rets[3]);
    return result;
}
} // namespace

namespace BbSharedLog {
bool Enabled() {
    static const bool on = File() != nullptr;
    return on;
}

void GpuWrite(u64 addr, u64 size, u64 writer) {
    constexpr u64 lo = 0x3000000000ull, hi = 0x3003200000ull;
    if (addr >= hi || addr + size <= lo || g_writes.size() >= 400000) {
        return; // outside the heap, or the cap (a ring buffer would add ranges every frame)
    }
    const u64 key = (addr * 0x9E3779B97F4A7C15ull) ^ (size << 7) ^ writer;
    if (!g_writes.insert(key).second) {
        return;
    }
    std::scoped_lock lk{g_mutex};
    std::fprintf(File(), "W %.3f %llx %llx %llx\n", NowMs(), (unsigned long long)addr,
                 (unsigned long long)(addr + size), (unsigned long long)writer);
}
} // namespace BbSharedLog

extern "C" uintptr_t bbgpu_shared_alloc_hook(uintptr_t real_alloc, uintptr_t image_base) {
    g_file = File();
    const char* pad = std::getenv("BB_SHARED_PAD");
    g_pad = pad ? u32(std::strtoul(pad, nullptr, 0)) : 3u;
    if ((!g_file && !g_pad) || !real_alloc) {
        return 0;
    }
    g_real = reinterpret_cast<Alloc>(real_alloc);
    g_real_video = reinterpret_cast<Alloc>(image_base + 0x753b40);
    g_image = image_base;
    return reinterpret_cast<uintptr_t>(&Allocate);
}

extern "C" void bbgpu_shared_log_flush(void) {
    if (g_file) {
        std::scoped_lock lk{g_mutex};
        std::fflush(g_file);
    }
}
