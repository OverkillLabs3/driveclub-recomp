// bbport: BB_FRAME_TRACE=<file> - one line per flip with where the frame's time went, to find
// the causes of frame-time variance (the frame log only has the interval). Written by the
// present thread (videoout/driver.cpp); the counters below are fed by the GPU command thread,
// the guest threads (submission lock, read faults) and the swap thread. All times steady_clock.
#pragma once
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>

namespace BbTrace {
inline const bool enabled = [] {
    const char* env = std::getenv("BB_FRAME_TRACE");
    return env && env[0];
}();
inline std::int64_t NowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}
inline void AtomicMax(std::atomic<std::uint64_t>& a, std::uint64_t v) {
    std::uint64_t cur = a.load(std::memory_order_relaxed);
    while (v > cur && !a.compare_exchange_weak(cur, v, std::memory_order_relaxed)) {
    }
}
/// Incremented at every flip: per-thread accumulators reset when they see a new value.
inline std::atomic<std::uint32_t> frame_gen{0};
/// Guest time blocked in WaitGpuIdle (the submission lock after sceGnmSubmitDone), all threads.
inline std::atomic<std::uint64_t> submit_lock_ns{0}, submit_lock_waits{0};
/// When the guest called sceGnmSubmitAndFlip / sceGnmSubmitDone last (ns).
inline std::atomic<std::int64_t> last_submit_flip_ns{0}, last_submit_done_ns{0};
/// GPU command thread waiting for guest work (cumulative, ns).
inline std::atomic<std::uint64_t> gpu_idle_ns{0};
/// PrepareFrame of the newest frame: total and the frames-ahead wait inside it (ns).
inline std::atomic<std::uint64_t> prep_ns{0}, ahead_wait_ns{0};
/// Read faults: cumulative ns over all threads, and the most one thread spent this frame.
inline std::atomic<std::uint64_t> read_fault_thread_max_ns{0};
/// Swap thread: duration of the latest game frame's Present (acquire, blit, queue present).
inline std::atomic<std::uint64_t> present_ns{0};
/// Offloaded readbacks (buffer_cache.cpp, cumulative): waiting for the GPU command thread to
/// record the copy, for the copy (GPU), for another thread's pending copy of the window; jobs.
inline std::atomic<std::uint64_t> rb_pickup_ns{0}, rb_gpu_ns{0}, rb_pending_ns{0}, rb_jobs{0};
/// BB_READBACK_LOG: guest instruction of the calling thread's current read fault, and the tag of
/// the GPU work that last bound guest memory for writing (shader hash; fill/copy markers).
inline thread_local std::uint64_t fault_rip = 0;
/// BB_READBACK_LOG: the 8 bytes at the stack pointer of the current read fault (a leaf function's return address).
inline thread_local std::uint64_t fault_ret = 0;
inline std::atomic<std::uint64_t> writer_tag{0};
/// Readbacks requested by this thread do not flush the open submission (background fetches, page_manager.cpp
/// BB_EXPOSURE_STALE / BB_CLOUD_STALE): they wait for the next submission instead of adding one.
inline thread_local bool lazy_readback = false;
/// Shader/pipeline compiles and texture evictions (cumulative counts).
inline std::atomic<std::uint64_t> compiles{0}, evictions{0};

/// Adds to the calling thread's read-fault time of the current frame and keeps the maximum.
/// First read fault start and last read fault end since the last flip (ns, 0 = none).
inline std::atomic<std::int64_t> read_fault_first_ns{0}, read_fault_last_end_ns{0};
inline void NoteReadFault(std::int64_t start_ns, std::uint64_t ns) {
    std::int64_t zero = 0;
    read_fault_first_ns.compare_exchange_strong(zero, start_ns, std::memory_order_relaxed);
    read_fault_last_end_ns.store(start_ns + std::int64_t(ns), std::memory_order_relaxed);
    static thread_local std::uint32_t gen = 0;
    static thread_local std::uint64_t mine = 0;
    const std::uint32_t now_gen = frame_gen.load(std::memory_order_relaxed);
    if (gen != now_gen) {
        gen = now_gen;
        mine = 0;
    }
    mine += ns;
    AtomicMax(read_fault_thread_max_ns, mine);
}
} // namespace BbTrace
