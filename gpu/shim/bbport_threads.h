// SPDX-License-Identifier: GPL-2.0-or-later
// Based on Supermedo's bloodborne_pc (https://github.com/Supermedo/bloodborne_pc).
// Modified for this port.
//
// bbport: helper thread sizing. Counts follow the hardware threads this process may run on
// (the affinity mask, so `taskset` can emulate a Steam Deck), and speculative helpers run as
// SCHED_IDLE: they use cores the game leaves idle and never take time from its threads.

#pragma once

#include <algorithm>
#include <chrono>
#include <cstdlib>
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

/// Gives the calling thread (the GPU command thread) a physical core of its own: it runs on both logical CPUs of
/// one core, and every other thread of the process is kept off that core, re-checked every 5 s so threads started
/// later are covered. Without it Windows can park a busy thread on the command thread's hyperthread sibling,
/// which costs it a quarter to a third of its speed. On hybrid CPUs the fastest cores are chosen. Hosts with
/// fewer than 6 logical CPUs or 4 physical cores are left alone; BB_CORE_RESERVE=0 turns it off.
inline void ReserveCoreForCurrentThread() {
    static bool started = false;
    const char* env = std::getenv("BB_CORE_RESERVE");
    if (started || (env && env[0] == '0')) {
        return;
    }
    started = true;
    DWORD length = 0;
    GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &length);
    std::vector<char> buffer(length);
    if (!length || !GetLogicalProcessorInformationEx(
                       RelationProcessorCore, reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(buffer.data()),
                       &length)) {
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
    DWORD_PTR process = 0, system = 0;
    GetProcessAffinityMask(GetCurrentProcess(), &process, &system);
    if (logical < 6 || cores.size() < 4 || process != system) {
        return;
    }
    BYTE best = 0;
    for (const auto& core : cores) {
        best = std::max(best, core.efficiency);
    }
    // The second fastest core: the first one tends to take the interrupts.
    std::vector<DWORD_PTR> fast;
    for (const auto& core : cores) {
        if (core.efficiency == best) {
            fast.push_back(core.mask);
        }
    }
    const DWORD_PTR reserved = fast.size() > 1 ? fast[1] : fast[0];
    const DWORD self = GetCurrentThreadId();
    SetThreadAffinityMask(GetCurrentThread(), reserved);
    std::thread([self, reserved, process] {
        const DWORD pid = GetCurrentProcessId();
        for (;;) {
            if (HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0); snap != INVALID_HANDLE_VALUE) {
                THREADENTRY32 te{.dwSize = sizeof(te)};
                for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
                    if (te.th32OwnerProcessID != pid || te.th32ThreadID == self) {
                        continue;
                    }
                    if (HANDLE h = OpenThread(THREAD_SET_INFORMATION | THREAD_QUERY_INFORMATION, FALSE, te.th32ThreadID)) {
                        SetThreadAffinityMask(h, process & ~reserved);
                        CloseHandle(h);
                    }
                }
                CloseHandle(snap);
            }
            std::this_thread::sleep_for(std::chrono::seconds(5));
        }
    }).detach();
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
inline void ReserveCoreForCurrentThread() {}
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
