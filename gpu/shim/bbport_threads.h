// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: helper thread sizing. Counts follow the hardware threads this process may run on
// (the affinity mask, so `taskset` can emulate a Steam Deck), and speculative helpers run as
// SCHED_IDLE: they use cores the game leaves idle and never take time from its threads.

#pragma once

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <mutex>
#include <vector>
#include <thread>
#ifdef _WIN32
#include <bit>
#include <cstdio>
#include <string>
#include <utility>
#if defined(_MSC_VER) && !defined(__clang__)
#include <intrin.h>
#elif defined(__x86_64__) || defined(__i386__)
#include <cpuid.h>
#endif
#include <windows.h>
#include <tlhelp32.h>
#else
#include <sched.h>
#include <sys/resource.h>
#include <unistd.h>
#endif

namespace BbThreads {
#ifdef _WIN32
/// Hardware threads available to the process (its affinity mask).
inline unsigned Available() {
    DWORD_PTR process = 0, system = 0;
    if (GetProcessAffinityMask(GetCurrentProcess(), &process, &system) && process) {
        return std::max(1, std::popcount(static_cast<unsigned long long>(process)));
    }
    return std::max(1u, std::thread::hardware_concurrency());
}

/// The calling thread only runs when cores are otherwise idle (lowest Windows priority).
inline void MakeBackground() {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_IDLE);
}

/// The calling thread is one the frame waits on (GPU command thread, recorder): above normal priority, and
/// exempt from the power throttling that moves threads to efficiency cores on hybrid CPUs and handhelds.
inline void MakeImportant() {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
    THREAD_POWER_THROTTLING_STATE state{THREAD_POWER_THROTTLING_CURRENT_VERSION,
                                        THREAD_POWER_THROTTLING_EXECUTION_SPEED, 0};
    SetThreadInformation(GetCurrentThread(), ThreadPowerThrottling, &state, sizeof(state));
}

/// GPU threads that may own a physical core. BB_CORE_OWNERS is a mask of them; the default 1 gives it to the GPU
/// command thread, the bottleneck again once the draw recording thread got cheaper (81.5 / 81.0 FPS, 1% low
/// 55.2 / 53.6, vs 80.5 / 54.4 with 2 = the draw recording thread). Never 4 or 7: it starves the guest.
enum CoreOwner : unsigned {
    kGpuCommandThread = 1,
    kDrawThread = 2,
    kRecorderThread = 4,
};

inline unsigned CoreOwners() {
    static const unsigned owners = [] {
        const char* env = std::getenv("BB_CORE_OWNERS");
        return env ? static_cast<unsigned>(std::strtoul(env, nullptr, 0)) : 1u;
    }();
    return owners;
}

inline const char* CoreOwnerName(CoreOwner owner) {
    switch (owner) {
    case kGpuCommandThread:
        return "GPU command thread";
    case kDrawThread:
        return "draw recording thread";
    default:
        return "recorder thread";
    }
}

/// One physical core as Windows reports it: its logical CPUs and its efficiency class (higher = faster; the same
/// for every core of a CPU that is not hybrid).
struct CoreInfo {
    DWORD_PTR mask;
    BYTE efficiency;
};

/// Whether the processor says it mixes core types (CPUID leaf 7, EDX bit 15: Intel hybrid).
inline bool CpuIsHybrid() {
    unsigned regs[4]{};
#if defined(_MSC_VER) && !defined(__clang__)
    int info[4]{};
    __cpuid(info, 0);
    if (info[0] < 7) {
        return false;
    }
    __cpuidex(info, 7, 0);
    regs[3] = static_cast<unsigned>(info[3]);
#elif defined(__x86_64__) || defined(__i386__)
    if (!__get_cpuid_count(7, 0, &regs[0], &regs[1], &regs[2], &regs[3])) {
        return false;
    }
#endif
    return (regs[3] >> 15) & 1;
}

/// The cores a GPU thread may own: only the fastest kind. Fastest = the highest efficiency class (Intel P-cores over
/// E-cores and low-power E-cores), then the most logical CPUs per core (P-cores have hyperthreads and E-cores none).
/// None when the CPU is hybrid but Windows reports one class for all cores: which ones are fast is unknown then (with
/// hyperthreading off P- and E-cores look the same). In Windows' order, except that the first one is given out last:
/// it tends to take the interrupts.
inline std::vector<DWORD_PTR> FastCores(const std::vector<CoreInfo>& cores, bool hybrid_cpu) {
    const auto rank = [](const CoreInfo& core) {
        return std::pair{core.efficiency, std::popcount(static_cast<unsigned long long>(core.mask))};
    };
    std::vector<DWORD_PTR> fast;
    if (cores.empty()) {
        return fast;
    }
    const bool one_class = std::all_of(cores.begin(), cores.end(), [&](const CoreInfo& core) {
        return core.efficiency == cores.front().efficiency;
    });
    if (hybrid_cpu && one_class) {
        return fast;
    }
    auto best = rank(cores.front());
    for (const auto& core : cores) {
        best = std::max(best, rank(core));
    }
    for (const auto& core : cores) {
        if (rank(core) == best) {
            fast.push_back(core.mask);
        }
    }
    std::rotate(fast.begin(), fast.begin() + 1, fast.end());
    return fast;
}

/// The logical CPUs of a mask for the log, e.g. "0-1,4-19".
inline std::string CpuList(DWORD_PTR mask) {
    std::string out;
    for (int cpu = 0; cpu < 64;) {
        if (!((static_cast<unsigned long long>(mask) >> cpu) & 1)) {
            ++cpu;
            continue;
        }
        int last = cpu;
        while (last + 1 < 64 && ((static_cast<unsigned long long>(mask) >> (last + 1)) & 1)) {
            ++last;
        }
        out += (out.empty() ? "" : ",") + std::to_string(cpu) + (last > cpu ? "-" + std::to_string(last) : "");
        cpu = last + 1;
    }
    return out.empty() ? "none" : out;
}

/// Gives the calling thread a physical core of its own when `owner` is in BB_CORE_OWNERS: it runs on both
/// logical CPUs of one core, and every other thread of the process is kept off the reserved cores, re-checked
/// every 5 s so threads started later are covered. Without it Windows can park a busy thread on the hyperthread
/// sibling, which costs a quarter to a third of its speed. Each owner gets its own core, always one of FastCores
/// (no efficiency core when Windows or the CPU reports the core types). Hosts with fewer than 6 logical CPUs or 4
/// physical cores are left alone; BB_CORE_RESERVE=0 turns it off. The log's "Cores:" lines say what was found and
/// chosen.
inline void ReserveCoreForCurrentThread(CoreOwner owner) {
    const char* env = std::getenv("BB_CORE_RESERVE");
    if (env && env[0] == '0') {
        if (owner == kGpuCommandThread) {
            std::printf("Cores: no reserved core (BB_CORE_RESERVE=0)\n");
        }
        return;
    }
    if (!(CoreOwners() & owner)) {
        return;
    }
    struct Reserved {
        DWORD tid;
        DWORD_PTR mask;
    };
    static std::mutex mutex;
    static std::vector<DWORD_PTR> free_cores; // fast cores not reserved yet, in the order they are given out
    static std::vector<Reserved> reserved;    // read by the re-pin thread under `mutex`
    static DWORD_PTR process = 0;             // the logical CPUs of the process
    static unsigned taken = 0;                // owners that already have a core
    static bool initialised = false, usable = false;
    std::scoped_lock lk{mutex};
    if (taken & owner) {
        return; // one core per owner (the presenter's recorder thread does not get a second)
    }
    taken |= owner;
    if (!initialised) {
        initialised = true;
        const auto skip = [](const char* why) { std::printf("Cores: no reserved core (%s)\n", why); };
        DWORD_PTR system = 0;
        if (!GetProcessAffinityMask(GetCurrentProcess(), &process, &system) || !process) {
            return skip("the process's CPUs are unknown");
        }
        // Affinity masks are per processor group: one group only (up to 64 logical CPUs).
        if (GetActiveProcessorGroupCount() != 1) {
            return skip("more than one processor group");
        }
        DWORD length = 0;
        GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &length);
        std::vector<char> buffer(length);
        if (!length || !GetLogicalProcessorInformationEx(
                           RelationProcessorCore,
                           reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(buffer.data()), &length)) {
            return skip("the core list is unknown");
        }
        std::vector<CoreInfo> cores;
        unsigned logical = 0;
        BYTE lowest = 255, highest = 0;
        for (DWORD at = 0; at < length;) {
            const auto* info = reinterpret_cast<const SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(buffer.data() + at);
            at += info->Size;
            if (info->Relationship != RelationProcessorCore || info->Processor.GroupCount != 1) {
                continue;
            }
            const DWORD_PTR mask = info->Processor.GroupMask[0].Mask;
            cores.push_back({mask, info->Processor.EfficiencyClass});
            logical += std::popcount(static_cast<unsigned long long>(mask));
            lowest = std::min(lowest, info->Processor.EfficiencyClass);
            highest = std::max(highest, info->Processor.EfficiencyClass);
        }
        const bool hybrid_cpu = CpuIsHybrid();
        free_cores = FastCores(cores, hybrid_cpu);
        std::printf("Cores: %zu physical / %u logical, %s", cores.size(), logical,
                    lowest != highest ? "hybrid"
                    : hybrid_cpu      ? "hybrid with one efficiency class for all cores"
                                      : "one efficiency class");
        if (!free_cores.empty()) {
            std::printf("; fast cores: %zu (efficiency class %u, %d logical CPUs each)", free_cores.size(),
                        static_cast<unsigned>(highest),
                        std::popcount(static_cast<unsigned long long>(free_cores.front())));
        }
        std::printf("\n");
        if (free_cores.empty()) {
            return skip("which cores are fast is unknown");
        }
        if (logical < 6 || cores.size() < 4) {
            return skip("fewer than 6 logical CPUs or 4 cores");
        }
        if (process != system) {
            return skip("the process may only run on some CPUs");
        }
        usable = true;
        std::thread([] {
            const DWORD pid = GetCurrentProcessId();
            for (;;) {
                {
                    // Under the lock for the whole pass, so an owner that gets its core meanwhile is not moved.
                    std::scoped_lock lk{mutex};
                    DWORD_PTR all = 0;
                    for (const auto& r : reserved) {
                        all |= r.mask;
                    }
                    if (HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
                        snap != INVALID_HANDLE_VALUE) {
                        THREADENTRY32 te{.dwSize = sizeof(te)};
                        for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
                            if (te.th32OwnerProcessID != pid ||
                                std::any_of(reserved.begin(), reserved.end(),
                                            [&](const Reserved& r) { return r.tid == te.th32ThreadID; })) {
                                continue;
                            }
                            if (HANDLE h = OpenThread(THREAD_SET_INFORMATION | THREAD_QUERY_INFORMATION, FALSE,
                                                      te.th32ThreadID)) {
                                SetThreadAffinityMask(h, process & ~all);
                                CloseHandle(h);
                            }
                        }
                        CloseHandle(snap);
                    }
                }
                std::this_thread::sleep_for(std::chrono::seconds(5));
            }
        }).detach();
    }
    if (!usable) {
        return;
    }
    // Two fast cores stay for everything else.
    if (free_cores.size() <= 2) {
        std::printf("Cores: the %s has no core of its own (%zu fast cores left, two stay for the game)\n",
                    CoreOwnerName(owner), free_cores.size());
        return;
    }
    const DWORD_PTR mask = free_cores.front();
    if (!SetThreadAffinityMask(GetCurrentThread(), mask)) {
        const DWORD error = GetLastError();
        std::printf("Cores: the %s could not be moved to CPUs %s (error %lu)\n", CoreOwnerName(owner),
                    CpuList(mask).c_str(), error);
        return;
    }
    free_cores.erase(free_cores.begin());
    reserved.push_back({GetCurrentThreadId(), mask});
    DWORD_PTR all = 0;
    for (const auto& r : reserved) {
        all |= r.mask;
    }
    std::printf("Cores: the %s owns CPUs %s (a fast core); other threads are kept to CPUs %s\n",
                CoreOwnerName(owner), CpuList(mask).c_str(), CpuList(process & ~all).c_str());
}

/// Once per process: no execution speed throttling for the game, and a priority class above normal.
inline void TuneProcess() {
    static const bool done = [] {
        PROCESS_POWER_THROTTLING_STATE state{PROCESS_POWER_THROTTLING_CURRENT_VERSION,
                                             PROCESS_POWER_THROTTLING_EXECUTION_SPEED, 0};
        SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &state, sizeof(state));
        SetPriorityClass(GetCurrentProcess(), ABOVE_NORMAL_PRIORITY_CLASS);
        return true;
    }();
    (void)done;
}
#else

/// Hardware threads available to the process.
inline unsigned Available() {
    cpu_set_t set;
    CPU_ZERO(&set);
    if (sched_getaffinity(0, sizeof(set), &set) == 0) {
        return std::max(1, CPU_COUNT(&set));
    }
    return std::max(1u, std::thread::hardware_concurrency());
}

inline void MakeImportant() {}
enum CoreOwner : unsigned {
    kGpuCommandThread = 1,
    kDrawThread = 2,
    kRecorderThread = 4,
};
inline unsigned CoreOwners() {
    return 0;
}
inline void ReserveCoreForCurrentThread(CoreOwner) {}
inline void TuneProcess() {}

/// The calling thread only runs on otherwise idle cores (falls back to the lowest nice level).
inline void MakeBackground() {
    sched_param param{};
    if (sched_setscheduler(0, SCHED_IDLE, &param) != 0) {
        setpriority(PRIO_PROCESS, static_cast<id_t>(gettid()), 19);
    }
}
#endif

} // namespace BbThreads
