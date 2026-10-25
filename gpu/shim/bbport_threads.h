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

/// Gives the calling thread a physical core of its own when `owner` is in BB_CORE_OWNERS: it runs on both
/// logical CPUs of one core, and every other thread of the process is kept off the reserved cores, re-checked
/// every 5 s so threads started later are covered. Without it Windows can park a busy thread on the hyperthread
/// sibling, which costs a quarter to a third of its speed. Each owner gets its own core (the first fast core takes
/// the interrupts and comes last). On hybrid CPUs the fastest cores are chosen. Hosts with fewer than 6 logical
/// CPUs or 4 physical cores are left alone; BB_CORE_RESERVE=0 turns it off.
inline void ReserveCoreForCurrentThread(CoreOwner owner) {
    const char* env = std::getenv("BB_CORE_RESERVE");
    if ((env && env[0] == '0') || !(CoreOwners() & owner)) {
        return;
    }
    struct Reserved {
        DWORD tid;
        DWORD_PTR mask;
    };
    static std::mutex mutex;
    static std::vector<DWORD_PTR> free_cores; // fast cores not reserved yet, in the order they are given out
    static std::vector<Reserved> reserved;    // read by the re-pin thread under `mutex`
    static unsigned taken = 0;                // owners that already have a core
    static bool initialised = false, usable = false;
    std::scoped_lock lk{mutex};
    if (taken & owner) {
        return; // one core per owner (the presenter's recorder thread does not get a second)
    }
    taken |= owner;
    DWORD_PTR process = 0, system = 0;
    GetProcessAffinityMask(GetCurrentProcess(), &process, &system);
    if (!initialised) {
        initialised = true;
        DWORD length = 0;
        GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &length);
        std::vector<char> buffer(length);
        if (!length || !GetLogicalProcessorInformationEx(
                           RelationProcessorCore,
                           reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(buffer.data()), &length)) {
            return;
        }
        struct Core {
            DWORD_PTR mask;
            BYTE efficiency;
        };
        std::vector<Core> cores;
        unsigned logical = 0;
        for (DWORD at = 0; at < length;) {
            const auto* info = reinterpret_cast<const SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(buffer.data() + at);
            at += info->Size;
            if (info->Relationship != RelationProcessorCore || info->Processor.GroupCount != 1) {
                continue;
            }
            const DWORD_PTR mask = info->Processor.GroupMask[0].Mask;
            cores.push_back({mask, info->Processor.EfficiencyClass});
            logical += std::popcount(static_cast<unsigned long long>(mask));
        }
        if (logical < 6 || cores.size() < 4 || process != system) {
            return;
        }
        BYTE best = 0;
        for (const auto& core : cores) {
            best = std::max(best, core.efficiency);
        }
        for (const auto& core : cores) {
            if (core.efficiency == best) {
                free_cores.push_back(core.mask);
            }
        }
        // The first fast core tends to take the interrupts: given out last.
        std::rotate(free_cores.begin(), free_cores.begin() + 1, free_cores.end());
        usable = true;
        std::thread([process] {
            const DWORD pid = GetCurrentProcessId();
            for (;;) {
                std::vector<Reserved> owners;
                DWORD_PTR all = 0;
                {
                    std::scoped_lock lk{mutex};
                    owners = reserved;
                }
                for (const auto& r : owners) {
                    all |= r.mask;
                }
                if (HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0); snap != INVALID_HANDLE_VALUE) {
                    THREADENTRY32 te{.dwSize = sizeof(te)};
                    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
                        if (te.th32OwnerProcessID != pid ||
                            std::any_of(owners.begin(), owners.end(),
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
                std::this_thread::sleep_for(std::chrono::seconds(5));
            }
        }).detach();
    }
    // Two physical cores stay for everything else.
    if (!usable || free_cores.size() <= 2) {
        return;
    }
    const DWORD_PTR mask = free_cores.front();
    free_cores.erase(free_cores.begin());
    SetThreadAffinityMask(GetCurrentThread(), mask);
    reserved.push_back({GetCurrentThreadId(), mask});
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
