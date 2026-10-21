// Performance aid: with BB_PROFILE=1 the busiest threads are sampled about every millisecond and the
// most frequent code locations are written to the log every 10 seconds, as module + offset (the
// offsets of DriveClubGame.exe can be looked up with a symbolizer on an unstripped build).
#ifdef _WIN32
#include <windows.h>
#include <tlhelp32.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <thread>
#include <unordered_map>
#include <vector>

namespace {

struct Sampled {
    HANDLE handle{};
    std::uint64_t last_cpu{};
    std::uint64_t window_cpu{};
    std::unordered_map<std::uintptr_t, std::uint32_t> hits;
    std::uint32_t samples{};
    std::unordered_map<std::uintptr_t, std::uint32_t> callers;  // innermost game-code return address found on the stack
    std::unordered_map<std::uintptr_t, std::array<std::uintptr_t, 5>> chains;  // one example call chain per code bucket
    std::wstring name;
};

std::uint64_t CpuTime(HANDLE h) {
    FILETIME c, e, k, u;
    if (!GetThreadTimes(h, &c, &e, &k, &u)) {
        return 0;
    }
    return ((std::uint64_t(k.dwHighDateTime) << 32 | k.dwLowDateTime) +
            (std::uint64_t(u.dwHighDateTime) << 32 | u.dwLowDateTime));
}

std::string Where(std::uintptr_t rip, std::uintptr_t exe_base) {
    HMODULE module{};
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(rip), &module) || !module) {
        return "?";
    }
    char path[MAX_PATH]{};
    GetModuleFileNameA(module, path, MAX_PATH);
    const char* base = path;
    for (const char* p = path; *p; ++p) {
        if (*p == '\\' || *p == '/') {
            base = p + 1;
        }
    }
    char out[MAX_PATH + 32];
    std::snprintf(out, sizeof(out), "%s+0x%llx", base,
                  static_cast<unsigned long long>(rip - reinterpret_cast<std::uintptr_t>(module)));
    (void)exe_base;
    return out;
}

void Run() {
    const std::uintptr_t exe_base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    const DWORD pid = GetCurrentProcessId();
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(exe_base);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(exe_base + dos->e_lfanew);
    std::vector<std::pair<std::uintptr_t, std::uintptr_t>> code;  // executable sections of the exe
    {
        const auto* sec = IMAGE_FIRST_SECTION(nt);
        for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
            if (sec[i].Characteristics & IMAGE_SCN_MEM_EXECUTE) {
                code.emplace_back(exe_base + sec[i].VirtualAddress, exe_base + sec[i].VirtualAddress + sec[i].Misc.VirtualSize);
            }
        }
    }
    const auto in_code = [&](std::uintptr_t a) {
        for (auto& [lo, hi] : code) {
            if (a >= lo && a < hi) {
                return true;
            }
        }
        return false;
    };
    std::map<DWORD, Sampled> threads;
    auto last_scan = std::chrono::steady_clock::now() - std::chrono::seconds(10);
    auto last_report = std::chrono::steady_clock::now();
    std::vector<DWORD> busy;
    std::printf("Profile: sampling on, executable base %p\n", reinterpret_cast<void*>(exe_base));
    for (;;) {
        const auto now = std::chrono::steady_clock::now();
        if (now - last_scan >= std::chrono::seconds(1)) {
            last_scan = now;
            HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
            THREADENTRY32 te{.dwSize = sizeof(te)};
            std::vector<std::pair<std::uint64_t, DWORD>> load;
            for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
                if (te.th32OwnerProcessID != pid || te.th32ThreadID == GetCurrentThreadId()) {
                    continue;
                }
                auto it = threads.find(te.th32ThreadID);
                if (it == threads.end()) {
                    HANDLE h = OpenThread(THREAD_QUERY_INFORMATION | THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT,
                                          FALSE, te.th32ThreadID);
                    if (!h) {
                        continue;
                    }
                    it = threads.emplace(te.th32ThreadID, Sampled{.handle = h}).first;
                }
                const std::uint64_t cpu = CpuTime(it->second.handle);
                const std::uint64_t delta = cpu - it->second.last_cpu;
                it->second.last_cpu = cpu;
                it->second.window_cpu += delta;
                if (it->second.name.empty()) {
                    PWSTR n{};
                    if (SUCCEEDED(GetThreadDescription(it->second.handle, &n)) && n) {
                        it->second.name = n;
                        LocalFree(n);
                    }
                }
                load.emplace_back(delta, te.th32ThreadID);
            }
            CloseHandle(snap);
            std::sort(load.rbegin(), load.rend());
            busy.clear();
            for (std::size_t i = 0; i < load.size() && i < 4; ++i) {
                if (load[i].first > 1000000) {  // over 10 ms of CPU in the last second
                    busy.push_back(load[i].second);
                }
            }
        }
        for (DWORD tid : busy) {
            auto& t = threads[tid];
            if (SuspendThread(t.handle) == DWORD(-1)) {
                continue;
            }
            CONTEXT c{};
            c.ContextFlags = CONTEXT_CONTROL;
            const bool got = GetThreadContext(t.handle, &c) != 0;
            std::array<std::uintptr_t, 5> chain{};
            if (got) {  // return addresses along the frame pointers (the build keeps them)
                std::uintptr_t fp = static_cast<std::uintptr_t>(c.Rbp);
                for (auto& slot : chain) {
                    std::uintptr_t frame[2]{};
                    SIZE_T n = 0;
                    if (!fp || (fp & 7) || !ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(fp), frame, sizeof(frame), &n) ||
                        n != sizeof(frame)) {
                        break;
                    }
                    slot = frame[1];
                    fp = frame[0];
                }
            }
            std::uintptr_t caller = 0;
            if (got) {  // innermost word on the stack that points into the game's code
                std::uintptr_t words[256]{};
                SIZE_T n = 0;
                if (ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(c.Rsp), words, sizeof(words), &n)) {
                    for (SIZE_T i = 0; i < n / sizeof(words[0]); ++i) {
                        if (in_code(words[i])) {
                            caller = words[i];
                            break;
                        }
                    }
                }
            }
            ResumeThread(t.handle);
            if (caller) {
                ++t.callers[caller & ~std::uintptr_t(0x3f)];
            }
            if (got) {  // recorded after the resume: the thread may have held the heap lock
                const std::uintptr_t rip = static_cast<std::uintptr_t>(c.Rip);
                ++t.hits[rip];
                ++t.samples;
                t.chains.try_emplace(rip & ~std::uintptr_t(0x3f), chain);
            }
        }
        if (now - last_report >= std::chrono::seconds(10)) {
            last_report = now;
            std::vector<std::pair<std::uint64_t, DWORD>> order;
            for (auto& [tid, t] : threads) {
                if (t.samples) {
                    order.emplace_back(t.window_cpu, tid);
                }
            }
            std::sort(order.rbegin(), order.rend());
            for (std::size_t n = 0; n < order.size() && n < 3; ++n) {
                auto& t = threads[order[n].second];
                // Group by location, 64-byte buckets, so one function shows as a few lines.
                std::map<std::string, std::uint32_t> by_place;
                for (auto& [rip, hits] : t.hits) {
                    by_place[Where(rip & ~std::uintptr_t(0x3f), exe_base)] += hits;
                }
                std::vector<std::pair<std::uint32_t, std::string>> top;
                for (auto& [place, hits] : by_place) {
                    top.emplace_back(hits, place);
                }
                std::sort(top.rbegin(), top.rend());
                std::printf("Profile: thread %lu '%ls' cpu %.1f s, %u samples\n",
                            static_cast<unsigned long>(order[n].second), t.name.c_str(),
                            order[n].first / 1e7, t.samples);
                for (std::size_t i = 0; i < top.size() && i < 25; ++i) {
                    std::printf("Profile:   %5.1f%% %s\n", 100.0 * top[i].first / t.samples,
                                top[i].second.c_str());
                }
                // Who in the game's code was running (or waiting) when the sample landed.
                std::vector<std::pair<std::uint32_t, std::uintptr_t>> who;
                for (auto& [addr, hits] : t.callers) {
                    who.emplace_back(hits, addr);
                }
                std::sort(who.rbegin(), who.rend());
                for (std::size_t i = 0; i < who.size() && i < 20; ++i) {
                    std::printf("Profile:   caller %5.1f%% DriveClubGame.exe+0x%llx\n", 100.0 * who[i].first / t.samples,
                                static_cast<unsigned long long>(who[i].second - exe_base));
                }
                // Call chains of the hottest locations.
                std::size_t shown = 0;
                for (auto& entry : top) {
                    if (shown++ >= 6) {
                        break;
                    }
                    for (auto& [bucket, chain] : t.chains) {
                        if (Where(bucket, exe_base) != entry.second) {
                            continue;
                        }
                        std::printf("Profile:   from %s:", entry.second.c_str());
                        for (auto ra : chain) {
                            if (ra) {
                                std::printf(" %s", Where(ra, exe_base).c_str());
                            }
                        }
                        std::printf("\n");
                        break;
                    }
                }
            }
            for (auto& [tid, t] : threads) {
                t.hits.clear();
                t.callers.clear();
                t.chains.clear();
                t.samples = 0;
                t.window_cpu = 0;
            }
            std::fflush(stdout);
        }
        std::this_thread::sleep_for(std::chrono::microseconds(900));
    }
}

} // namespace

extern "C" void BbDescribeAddress(unsigned long long rip, char* out, unsigned long size) {
    const std::string text = Where(static_cast<std::uintptr_t>(rip), 0);
    std::snprintf(out, size, "%s", text.c_str());
}

extern "C" void BbStartSampler() {
    static bool started = false;
    if (started) {
        return;
    }
    started = true;
    const char* env = std::getenv("BB_PROFILE");
    if (env && env[0] == '1') {
        std::thread(Run).detach();
    }
}


#else
extern "C" void BbDescribeAddress(unsigned long long, char* out, unsigned long size) {
    if (size) out[0] = 0;
}
extern "C" void BbStartSampler() {}
#endif

// driveclub: BB_PIPE_CHECK=1 (see bbport_toggles.h): a scheduler or cache entry point called by a thread other than
// stage B while stage B runs a draw packet. Each distinct call stack is printed once (first 64).
namespace BbStats {
void PipeCheckViolation(const char* what) {
    void* frames[10]{};
    const USHORT n = CaptureStackBackTrace(1, 10, frames, nullptr);
    std::uint64_t key = reinterpret_cast<std::uintptr_t>(what);
    for (USHORT i = 0; i < n && i < 6; ++i) {
        key = key * 0x9E3779B97F4A7C15ull + reinterpret_cast<std::uintptr_t>(frames[i]);
    }
    static std::atomic<std::uint64_t> seen[64]{};
    static std::atomic<std::uint32_t> count{0};
    static std::atomic<std::uint64_t> total{0};
    const std::uint64_t t = total.fetch_add(1, std::memory_order_relaxed) + 1;
    for (auto& slot : seen) {
        std::uint64_t cur = slot.load(std::memory_order_relaxed);
        if (cur == key) {
            return;
        }
        if (cur == 0 && slot.compare_exchange_strong(cur, key)) {
            break;
        }
        if (cur == key) {
            return;
        }
    }
    if (count.fetch_add(1) >= 64) {
        return;
    }
    const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    std::printf("Pipe check: %s called by thread %lu while stage B runs a packet (violation %llu):", what,
                GetCurrentThreadId(), static_cast<unsigned long long>(t));
    for (USHORT i = 0; i < n; ++i) {
        const auto a = reinterpret_cast<std::uintptr_t>(frames[i]);
        if (a >= base && a < base + 0x10000000) {
            std::printf(" exe+%llx", static_cast<unsigned long long>(a - base));
        } else {
            std::printf(" %llx", static_cast<unsigned long long>(a));
        }
    }
    std::printf("\n");
    std::fflush(stdout);
}
} // namespace BbStats
