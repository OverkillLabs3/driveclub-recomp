// The last draws and dispatches that were recorded, kept in a small ring so that a crash, a freeze or a lost
// device can say what the GPU was working on. One atomic increment and a few stores per call.
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace BbRecent {

struct Entry {
    std::uint64_t a = 0, b = 0, ticks = 0;
    std::uint32_t x = 0, y = 0, z = 0;
    char kind = 0;
};

inline constexpr std::uint32_t Size = 64;
inline Entry entries[Size];
inline std::atomic<std::uint32_t> next{0};

inline std::uint64_t Now() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

inline void Draw(std::uint64_t vs, std::uint64_t ps, std::uint32_t indices, std::uint32_t instances) {
    entries[next.fetch_add(1, std::memory_order_relaxed) & (Size - 1)] = {vs, ps, 0, indices, instances, 0, 'D'};
}

inline void Dispatch(std::uint64_t cs, std::uint32_t x, std::uint32_t y, std::uint32_t z) {
    entries[next.fetch_add(1, std::memory_order_relaxed) & (Size - 1)] = {cs, 0, 0, x, y, z, 'C'};
}

} // namespace BbRecent

// Compute shaders that were running when the GPU was lost on an earlier run. A lost device cannot be
// recovered, so the trailing run of dispatches is written to a file next to the settings and the next
// launch skips them: the game keeps playing without that one effect instead of crashing again.
// Further crashes add more entries. BB_QUARANTINE=0 turns it off, BB_COMPUTE_SKIP=<hex,hex> adds
// entries by hand, deleting gpu_quarantine.txt starts over.
namespace BbQuarantine {

inline constexpr std::uint32_t Max = 32;
inline std::uint32_t list[Max];
inline std::atomic<std::uint32_t> count{0};
inline std::atomic<int> state{0}; // 0 not loaded, 1 loading, 2 ready
inline std::atomic<std::uint32_t> skipped{0};

inline std::string File() {
    if (const char* f = std::getenv("BB_QUARANTINE_FILE")) {
        return f;
    }
    const char* toggles = std::getenv("BB_TOGGLE_FILE");
    if (!toggles) {
        return {};
    }
    std::string dir = toggles;
    const size_t cut = dir.find_last_of("\\/");
    return (cut == std::string::npos ? std::string() : dir.substr(0, cut + 1)) + "gpu_quarantine.txt";
}

inline std::string Sibling(const char* name) {
    const std::string path = File();
    const size_t cut = path.find_last_of("\\/");
    return (cut == std::string::npos ? std::string() : path.substr(0, cut + 1)) + name;
}

inline bool Enabled() {
    const char* on = std::getenv("BB_QUARANTINE");
    return on && on[0] == '1';
}

inline void Add(std::uint32_t hash) {
    const std::uint32_t n = count.load();
    for (std::uint32_t i = 0; i < n; ++i) {
        if (list[i] == hash) {
            return;
        }
    }
    if (n < Max) {
        list[n] = hash;
        count.store(n + 1);
    }
}

inline void Load() {
    int expected = 0;
    if (!state.compare_exchange_strong(expected, 1)) {
        while (state.load() != 2) {
        }
        return;
    }
    // Off unless BB_QUARANTINE=1: skipping the dispatches that ended a crashed run left the
    // picture dark on AMD cards and did not stop the crashes.
    if (Enabled()) {
        const std::string path = File();
        if (!path.empty()) {
            if (FILE* f = std::fopen(path.c_str(), "r")) {
                unsigned v;
                while (std::fscanf(f, "%x", &v) == 1) {
                    Add(v);
                }
                std::fclose(f);
            }
        }
        if (const char* env = std::getenv("BB_COMPUTE_SKIP")) {
            for (const char* p = env; *p;) {
                char* end;
                const unsigned long v = std::strtoul(p, &end, 16);
                if (end == p) {
                    ++p;
                    continue;
                }
                Add(static_cast<std::uint32_t>(v));
                p = end;
            }
        }
        if (count.load()) {
            std::printf("GPU: skipping %u compute shader(s) that lost the device on an earlier run:", count.load());
            for (std::uint32_t i = 0; i < count.load(); ++i) {
                std::printf(" %08x", list[i]);
            }
            std::printf(" (delete gpu_quarantine.txt or set BB_QUARANTINE=0 to run them again)\n");
            std::fflush(stdout);
        }
    }
    state.store(2);
}

inline bool Contains(std::uint64_t hash) {
    if (state.load(std::memory_order_acquire) != 2) {
        Load();
    }
    const std::uint32_t n = count.load(std::memory_order_relaxed);
    for (std::uint32_t i = 0; i < n; ++i) {
        if (list[i] == static_cast<std::uint32_t>(hash)) {
            if (skipped.fetch_add(1) == 0) {
                std::printf("GPU: first skipped dispatch of quarantined compute shader %08x\n", list[i]);
                std::fflush(stdout);
            }
            return true;
        }
    }
    return false;
}

// The device was lost: remember the dispatches that ended the recorded work (up to 8 in a row).
inline void NoteDeviceLost() {
    if (state.load() != 2) {
        Load();
    }
    const std::uint32_t end = BbRecent::next.load(std::memory_order_relaxed);
    std::uint32_t found = 0;
    for (std::uint32_t back = 1; back <= 8 && back <= end; ++back) {
        const auto& e = BbRecent::entries[(end - back) & (BbRecent::Size - 1)];
        if (e.kind != 'C') {
            break;
        }
        Add(static_cast<std::uint32_t>(e.a));
        ++found;
    }
    const std::string path = File();
    if (!Enabled()) {
        std::printf("GPU: device lost after %u trailing dispatch(es); not saved (BB_QUARANTINE=1 skips them next time)\n",
                    found);
        std::fflush(stdout);
        return;
    }
    std::printf("GPU: device lost, %u trailing dispatch(es) will be skipped on the next launch%s\n", found,
                path.empty() ? " (no settings folder known, not saved)" : "");
    if (!path.empty()) {
        if (FILE* f = std::fopen(path.c_str(), "w")) {
            for (std::uint32_t i = 0; i < count.load(); ++i) {
                std::fprintf(f, "%08x\n", list[i]);
            }
            std::fclose(f);
        }
    }
    std::fflush(stdout);
}

} // namespace BbQuarantine
