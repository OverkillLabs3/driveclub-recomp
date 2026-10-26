#include "bbport_diagnostics.h"
// bbport: optimizations that can be switched off while the game runs (BB_TOGGLE_FILE,
// see runtime_memory.c), to find which one changes rendering without restarting.
#pragma once
#include <atomic>
#include <cstdio>
#include <csetjmp>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#ifdef _WIN32
// bbport (Windows): no signals. The loader's exception handler resumes the faulting thread
// in bb_longjmp (src/compat_win.c: no unwinding through the faulting frames).
typedef unsigned long long sigjmp_buf[32];
extern "C" __attribute__((returns_twice)) int bb_setjmp(sigjmp_buf buffer);
#define sigsetjmp(buffer, save) bb_setjmp(buffer)
#endif

extern "C" std::uint64_t runtime_disabled_optimizations;
/// Recovery point for speculative guest memory reads on this thread (runtime_memory.c).
extern "C" __thread sigjmp_buf* runtime_fault_recover;

namespace BbToggle {
enum : std::uint64_t {
    RegionCache = 1,
    FetchShaderCache = 2,
    PageTrackingEarlyExit = 4,
    PendingPollLimit = 8,
    ThreadedRecording = 16,
    ImageDescCache = 32,
    LockFreeUploadCheck = 64,
    FindImageCache = 128,
    DeferredUploads = 256,
    AccessMemo = 512,
    TextureBindingMemo = 1024,
    CoarseReadTracking = 2048,
    PreparedResources = 4096,
    DrawPreparation = 8192,
    DeferredStreamCopies = 16384,
    HotPages = 32768,
    FaultWindow = 65536,
    ParallelCopies = 131072,
    AsyncFences = 262144,
    PoolSmallCopies = 524288,
    RecordPrefetch = 1ull << 32,
    TextureViewMemo = 1ull << 33,
    TextureBindHelper = 1ull << 34,
    EarlyDrawInputs = 1ull << 35,
    ConstantRing = 1ull << 36,
    DrawPipeline = 1ull << 37,
    PipelinedTasks = 1ull << 38,
    PendingFenceWaits = 1ull << 39,
    PipelinedDispatch = 1ull << 40,
    RecorderFences = 1ull << 41,
    PipelinedMemoryWrites = 1ull << 42,
    MultiCopyShader = 1ull << 43,
    RenderStateMemo = 1ull << 44,
    TextureSetMemo = 1ull << 45,
    PipelinedIndirectDraws = 1ull << 46,
    SceneAttachmentsOnly = 1ull << 47,
    SampleSceneProxies = 1ull << 48,
    OrderedGuestWrites = 1ull << 49,
    SceneHalfRes = 1ull << 50, ///< live scaling also reduces the 960x540 post targets
    UpdateImageFastPath = 1u << 30,
    // TAA A/B in one run: optional techniques, off by default (no measured gain, 2026-10-02).
    TaaTonemapBlend = 1ull << 51,
    TaaClip = 1ull << 52,
    TaaVariance = 1ull << 53,
    TaaFilter = 1ull << 54,
    // On by default (bit set: off): history of a thin feature this jitter phase missed is kept
    // when nothing moves. Static-camera flicker of railings/window bars p99.9 -45% (2026-10-03).
    TaaKeepNearerHistory = 1ull << 55,
    SceneMipBias = 1ull << 57, ///< negative LOD bias of G-buffer samplers at reduced scene sizes
    MappedRangeCopies = 1ull << 58, ///< set: IsMapped takes the shared lock again (A/B)
    DepthFormSwap = 1ull << 59, ///< an image used as depth and as color keeps both forms (texture_cache.cpp)
    // Bits 20-29 are used as raw debug toggles by the camera/object motion and the upscaler.
};
inline bool Disabled(std::uint64_t bit) {
    return (__atomic_load_n(&runtime_disabled_optimizations, __ATOMIC_RELAXED) & bit) != 0;
}
} // namespace BbToggle

namespace BbStats {
/// Guest writes caught by page protection, and pages currently left unprotected as hot.
inline std::atomic<std::uint64_t> tracker_faults{0};
inline std::atomic<std::int64_t> hot_pages{0};
/// Stall diagnostics (BB_FRAME_STATS): per-frame deltas printed for frames over 40 ms.
inline std::atomic<std::uint64_t> images_registered{0};
inline std::atomic<std::uint64_t> image_upload_bytes{0};
inline std::atomic<std::uint64_t> buffer_upload_bytes{0};
inline std::atomic<int> gpu_thread_clock{-1}; ///< clockid_t of the GPU command thread
inline std::atomic<std::uint64_t> draws{0}, dispatches{0}, submissions{0};
/// BB_PIPE_CE_NODRAIN: the exact guest bytes constant-RAM dumps have written (16-byte blocks, gpu_byte_map.h)
/// and whether a resource read live by the recording thread or the copy threads ever overlapped them; once one
/// has, dumps drain. Defined in buffer_cache.cpp.
inline std::atomic<bool> ce_dump_conflict{false};
void NoteCeDump(std::uint64_t addr, std::uint64_t size);
bool CeDumpOverlaps(std::uint64_t addr, std::uint64_t size);
/// A resource read live (buffer binding, upload): if it overlaps dumped bytes, dumps drain from now on.
inline void CheckCeConflict(std::uint64_t addr, std::uint64_t size, const char* what) {
    if (!ce_dump_conflict.load(std::memory_order_relaxed) && CeDumpOverlaps(addr, size) &&
        !ce_dump_conflict.exchange(true)) {
        std::printf("Draw pipe: %s at %llx (%llu bytes) overlaps constant-RAM dump data: dumps drain the pipe "
                    "from now on\n",
                    what, static_cast<unsigned long long>(addr), static_cast<unsigned long long>(size));
    }
}
/// BB_PIPE_CHECK=1 (two-stage draw pipe debugging). While draw packets are in flight, stage B owns the draw
/// scheduler and the caches: a call from any other thread is reported (bbport_sampler.cpp). `object` restricts the
/// scheduler checks to the draw scheduler (the presenter has its own).
inline std::atomic<std::uint32_t> pipe_owner{0};
inline std::atomic<std::uint32_t> pipe_stage_b{0};
inline std::atomic<const void*> pipe_scheduler{nullptr};
inline bool (*pipe_in_flight)() = nullptr;
/// Read once at startup: the checks on per-draw paths are a load of a constant when off.
inline const bool pipe_check = [] {
    const char* env = BB_DIAGNOSTIC_ENV("BB_PIPE_CHECK");
    return env && env[0] == '1';
}();
inline bool PipeCheckEnabled() {
    return pipe_check;
}
void PipeCheckViolation(const char* what);
#define BB_PIPE_CHECK_OBJ(what, object)                                                             \
    do {                                                                                           \
        if (BbStats::PipeCheckEnabled() && BbStats::pipe_in_flight &&                              \
            (!(object) || (object) == BbStats::pipe_scheduler.load(std::memory_order_relaxed))) {  \
            const std::uint32_t tid_ = static_cast<std::uint32_t>(gettid());                       \
            if (tid_ != BbStats::pipe_stage_b.load(std::memory_order_relaxed) &&                   \
                (BbStats::pipe_owner.load(std::memory_order_acquire) != 0 ||                       \
                 BbStats::pipe_in_flight())) {                                                     \
                BbStats::PipeCheckViolation(what);                                                 \
            }                                                                                      \
        }                                                                                          \
    } while (0)
#define BB_PIPE_CHECK(what) BB_PIPE_CHECK_OBJ(what, static_cast<const void*>(nullptr))
/// BB_FAULT_EMULATE=1: guest read faults on bytes the GPU did not write, served by emulating the load
/// (page_manager.cpp); declined (instruction not handled) and GPU-written bytes take the usual download.
inline std::atomic<std::uint64_t> fault_emulated{0}, fault_emulate_declined{0}, fault_emulate_gpu{0};
/// BB_FAULT_EMULATE=1: guest write faults on bytes the GPU did not write, done as a store plus an inline
/// update of the GPU copy; and those that took the usual download.
inline std::atomic<std::uint64_t> store_emulated{0}, store_emulate_declined{0};
/// Why stores were not emulated: 0 instruction, 1 operands, 2 address, 3 source, 4 page without GPU data,
/// 5 GPU-written bytes, 6 download in flight, 7 arena.
inline std::atomic<std::uint64_t> store_why[8]{};
/// Instructions of write faults the emulation does not handle (rip, count), for BB_FRAME_STATS.
/// Read faults whose load the emulator declined (BB_FRAME_STATS): the faulting instruction, the
/// name of the instruction decoded there (a static Zydis string) and a count, so the missing
/// instruction family is named in the log.
inline std::atomic<std::uint64_t> read_decline_rip[32]{}, read_decline_name[32]{},
    read_decline_count[32]{}, read_decline_reason[32]{};
inline void NoteReadDecline(std::uint64_t rip, const char* name, std::uint64_t reason) {
    for (std::uint32_t i = 0, h = std::uint32_t((rip >> 2) * 2654435761u) & 31; i < 32; ++i, h = (h + 1) & 31) {
        std::uint64_t cur = read_decline_rip[h].load(std::memory_order_relaxed);
        if (cur == 0 && read_decline_rip[h].compare_exchange_strong(cur, rip)) {
            cur = rip;
        }
        if (cur == rip) {
            read_decline_name[h].store(reinterpret_cast<std::uint64_t>(name), std::memory_order_relaxed);
            read_decline_reason[h].store(reason, std::memory_order_relaxed);
            read_decline_count[h].fetch_add(1, std::memory_order_relaxed);
            return;
        }
    }
}
/// Store faults the emulator declined (BB_FRAME_STATS).
inline std::atomic<std::uint64_t> store_skip_rip[64]{}, store_skip_count[64]{};
inline void NoteStoreSkip(std::uint64_t rip) {
    if constexpr (!BB_DIAGNOSTICS) return;
    for (std::uint32_t i = 0, h = std::uint32_t((rip >> 2) * 2654435761u) & 63; i < 64; ++i, h = (h + 1) & 63) {
        std::uint64_t cur = store_skip_rip[h].load(std::memory_order_relaxed);
        if (cur == 0 && store_skip_rip[h].compare_exchange_strong(cur, rip)) {
            cur = rip;
        }
        if (cur == rip) {
            store_skip_count[h].fetch_add(1, std::memory_order_relaxed);
            return;
        }
    }
}
/// Frames the GPU command thread has started (display pass), for per-frame diagnostics.
inline std::atomic<std::uint64_t> gpu_frames{0};
/// Wall time spent in operations suspected of stalls (ns, all threads).
inline std::atomic<std::uint64_t> t_resident{0}, t_protect{0}, t_image_create{0}, t_refresh{0},
    t_staging{0}, t_host_wait{0}, t_copy{0}, copy_bytes{0}, t_read_faults{0}, read_faults{0},
    t_write_faults{0}, t_copy_cpu{0}, copy_sys_us{0}, copy_minflt{0};
/// Diagnostics are collected only with BB_FRAME_STATS=1.
inline const bool enabled = [] {
    const char* env = BB_DIAGNOSTIC_ENV("BB_FRAME_STATS");
    return env && env[0] == '1';
}();
struct Timer {
    std::atomic<std::uint64_t>& total;
    std::chrono::steady_clock::time_point start =
        enabled ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    ~Timer() {
        if (!enabled) {
            return;
        }
        total.fetch_add(std::chrono::duration_cast<std::chrono::nanoseconds>(
                            std::chrono::steady_clock::now() - start)
                            .count(),
                        std::memory_order_relaxed);
    }
};
/// The phase and packet timers cost a clock read each, many per draw: BB_PHASES=1 only.
inline const bool phases_enabled = [] {
    const char* env = BB_DIAGNOSTIC_ENV("BB_PHASES");
    return enabled && env && env[0] == '1';
}();
/// BB_PHASE_ONE=<n>: time only the timer tagged bit 1<<n, so the clock reads of the others do not
/// inflate it (bit 0 is the set of general phase timers). Unset: every timer runs.
inline const std::uint32_t phase_one_mask = [] {
    const char* env = std::getenv("BB_PHASE_ONE");
    if (!enabled || !env || !env[0]) {
        return ~std::uint32_t{0};
    }
    return 1u << (std::strtoul(env, nullptr, 0) & 31u);
}();
inline bool PhaseWanted(std::uint32_t tag) {
    return (phase_one_mask & (tag ? tag : 1u)) != 0;
}
struct PhaseTimer {
    std::atomic<std::uint64_t>& total;
    bool on = false;
    std::chrono::steady_clock::time_point start{};
    explicit PhaseTimer(std::atomic<std::uint64_t>& total_, std::uint32_t tag = 0)
        : total(total_), on(phases_enabled && PhaseWanted(tag)) {
        if (on) {
            start = std::chrono::steady_clock::now();
        }
    }
    ~PhaseTimer() {
        if (!on) {
            return;
        }
        total.fetch_add(std::chrono::duration_cast<std::chrono::nanoseconds>(
                            std::chrono::steady_clock::now() - start)
                            .count(),
                        std::memory_order_relaxed);
    }
};
/// Time blocked in Scheduler::Wait by call site (ns): 0 other, 1 buffer watch, 2 fault buffer,
/// 3 present pacing, 4 Finish, 5 WaitSubmitted.
inline std::atomic<std::uint64_t> wait_site_ns[8]{};
/// Why an early readback copy was or was not used (BB_FRAME_STATS): 0 used, 1 no copy for the
/// window, 2 a GPU write was marked since the copy, 3 the copy's ranges differ.
inline std::atomic<std::uint64_t> readback_early[4]{};
/// Guest read paths into BufferCache::ReadMemory: 0 offloaded, 1 window split, 2 unresolved (sync fallback),
/// 3 locked caller (sync), 4 offload disabled, 5 direct, 6 direct mismatch, 7 early copy used, 8 early copy made,
/// 9 early copy verified, 10 early copy deferred (the draw writing the window was not recorded yet).
inline std::atomic<std::uint64_t> readback_paths[11]{};
/// WAIT_REG_MEM on memory (BB_FRAME_STATS): [0] graphics / [1] compute waits whose first test failed, and the
/// ns they waited. By polled address (8 slots): reads through the guest view (the 32nd unmet poll), of those
/// the ones that met the condition, their ns, and function << 32 | reference.
inline std::atomic<std::uint64_t> wrm_waits[2]{}, wrm_wait_ns[2]{};
inline std::atomic<std::uint64_t> wrm_addr[8]{}, wrm_forced[8]{}, wrm_forced_met[8]{}, wrm_forced_ns[8]{},
    wrm_info[8]{};
inline void NoteWaitViewRead(std::uint64_t addr, std::uint64_t info, bool met, std::uint64_t ns) {
    if constexpr (!BB_DIAGNOSTICS) return;
    for (unsigned i = 0, at = static_cast<unsigned>((addr >> 2) * 0x9E3779B1u) & 7; i < 8; ++i, at = (at + 1) & 7) {
        std::uint64_t expected = 0;
        if (wrm_addr[at].load(std::memory_order_relaxed) == addr ||
            wrm_addr[at].compare_exchange_strong(expected, addr) || expected == addr) {
            wrm_forced[at].fetch_add(1, std::memory_order_relaxed);
            wrm_forced_met[at].fetch_add(met ? 1 : 0, std::memory_order_relaxed);
            wrm_forced_ns[at].fetch_add(ns, std::memory_order_relaxed);
            wrm_info[at].store(info, std::memory_order_relaxed);
            return;
        }
    }
}
/// Read faults taken by the GPU command thread itself, by faulting instruction (BB_FRAME_STATS).
inline std::atomic<std::uint64_t> gpu_fault_rip[32]{}, gpu_fault_count[32]{};
inline void NoteGpuReadFault(std::uint64_t rip) {
    if constexpr (!BB_DIAGNOSTICS) return;
    for (unsigned i = 0, at = static_cast<unsigned>((rip >> 2) * 0x9E3779B1u) & 31; i < 32; ++i, at = (at + 1) & 31) {
        std::uint64_t expected = 0;
        if (gpu_fault_rip[at].load(std::memory_order_relaxed) == rip ||
            gpu_fault_rip[at].compare_exchange_strong(expected, rip) || expected == rip) {
            gpu_fault_count[at].fetch_add(1, std::memory_order_relaxed);
            return;
        }
    }
}
/// Device memory created while running: 0 cache buffers, 1 images, 2 runtime temporary buffers.
inline std::atomic<std::uint64_t> alloc_count[4]{}, alloc_bytes[4]{};
/// Draws into the small mirror scene, and how many of them were skipped (BB_MIRROR_RATE).
inline std::atomic<std::uint64_t> mirror_seen{0}, mirror_skipped{0}, mirror_targets{0}, mirror_depth_only{0};
/// The mirror scene's render targets (guest address), whether each is being skipped this frame, and the
/// shaders that sample them: shader hash -> draws, and how many of those came while the target was stale.
inline std::atomic<std::uint64_t> mirror_keys[8]{};
inline std::atomic<bool> mirror_stale[8]{};
inline std::atomic<std::uint64_t> mirror_consumer_hash[16]{}, mirror_consumer_count[16]{}, mirror_consumer_stale[16]{};
/// True while the mirror scene is being skipped (BB_MIRROR_RATE other than 1); images refreshed from guest memory
/// that were blocked because they are one of its targets.
inline std::atomic<bool> mirror_skipping{false};
/// Small-scene color draws by index count: <100, <300, <1000, <3000, <10000, more; and with several instances.
inline std::atomic<std::uint64_t> mirror_size_hist[8]{};
inline std::atomic<std::uint64_t> mirror_refresh_blocked{0};
inline std::atomic<std::uint64_t> mirror_dispatch_skipped{0};
/// 768x512 images created (0 overlap recreate, 1 expand, 2 new) and freed, in total and while their target was
/// being skipped, to see whether the mirror pictures are rebuilt (and lost) between frames.
inline std::atomic<std::uint64_t> mirror_img_created[3]{}, mirror_img_created_stale{0}, mirror_img_freed{0},
    mirror_img_freed_stale{0};
/// Shaders of draws into the mirror scene that sample the mirror scene's own pictures (a feedback pass such as a
/// water and wiper simulation, which reads last frame's picture): those draws are never skipped.
inline std::atomic<std::uint64_t> mirror_feedback[64]{};
inline std::atomic<std::uint32_t> mirror_feedback_count{0};
inline std::atomic<std::uint64_t> mirror_feedback_kept{0};
inline void NoteMirrorFeedback(std::uint64_t hash) {
    if (!hash) {
        return;
    }
    for (unsigned i = 0, at = static_cast<unsigned>(hash * 0x9E3779B1u >> 9) & 63; i < 64; ++i, at = (at + 1) & 63) {
        std::uint64_t expected = 0;
        if (mirror_feedback[at].load(std::memory_order_relaxed) == hash) {
            return;
        }
        if (mirror_feedback[at].compare_exchange_strong(expected, hash)) {
            mirror_feedback_count.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        if (expected == hash) {
            return;
        }
    }
}
inline bool IsMirrorFeedback(std::uint64_t hash) {
    for (unsigned i = 0, at = static_cast<unsigned>(hash * 0x9E3779B1u >> 9) & 63; i < 64; ++i, at = (at + 1) & 63) {
        const std::uint64_t v = mirror_feedback[at].load(std::memory_order_relaxed);
        if (v == hash) {
            return true;
        }
        if (v == 0) {
            return false;
        }
    }
    return false;
}
inline bool MirrorStale(std::uint64_t address) {
    for (unsigned i = 0; i < 8; ++i) {
        if (mirror_keys[i].load(std::memory_order_relaxed) == address) {
            return mirror_stale[i].load(std::memory_order_relaxed);
        }
    }
    return false;
}
inline bool IsMirrorTarget(std::uint64_t address) {
    for (unsigned i = 0; i < 8; ++i) {
        if (mirror_keys[i].load(std::memory_order_relaxed) == address) {
            return true;
        }
    }
    return false;
}
inline void NoteMirrorConsumer(std::uint64_t address, std::uint64_t hash) {
    if constexpr (!BB_DIAGNOSTICS) return;
    for (unsigned i = 0; i < 8; ++i) {
        if (mirror_keys[i].load(std::memory_order_relaxed) != address) {
            continue;
        }
        for (unsigned j = 0, at = static_cast<unsigned>(hash * 0x9E3779B1u >> 7) & 15; j < 16; ++j, at = (at + 1) & 15) {
            std::uint64_t expected = 0;
            if (mirror_consumer_hash[at].load(std::memory_order_relaxed) == hash ||
                mirror_consumer_hash[at].compare_exchange_strong(expected, hash) || expected == hash) {
                mirror_consumer_count[at].fetch_add(1, std::memory_order_relaxed);
                mirror_consumer_stale[at].fetch_add(mirror_stale[i].load(std::memory_order_relaxed), std::memory_order_relaxed);
                return;
            }
        }
        return;
    }
}
/// Texture cache churn: images created by site (0 overlap recreate, 1 expand, 2 new, 3 stencil), page tracking
/// changes (0 track, 1 untrack) and the code that frees images, by return address.
inline std::atomic<std::uint64_t> image_creates[4]{}, image_tracking[2]{};
inline std::atomic<std::uint64_t> image_free_rip[16]{}, image_free_count[16]{};
inline void NoteImageFree(std::uint64_t rip) {
    if constexpr (!BB_DIAGNOSTICS) return;
    for (unsigned i = 0, at = static_cast<unsigned>((rip >> 2) * 0x9E3779B1u) & 15; i < 16; ++i, at = (at + 1) & 15) {
        std::uint64_t expected = 0;
        if (image_free_rip[at].load(std::memory_order_relaxed) == rip ||
            image_free_rip[at].compare_exchange_strong(expected, rip) || expected == rip) {
            image_free_count[at].fetch_add(1, std::memory_order_relaxed);
            return;
        }
    }
}
/// Wall time the GPU command thread waited for guest submissions (ns).
inline std::atomic<std::uint64_t> gpu_idle_ns{0};
/// Draws recorded into the reduced scene targets, and draws after the scene started.
inline std::atomic<std::uint64_t> reduced_draws{0}, scene_draws{0};
/// Wall time spent blocked in the scheduler (ns): waiting for the recording thread to drain,
/// for host copies before a submission or fence, and for GPU ticks.
inline std::atomic<std::uint64_t> sync_recording_ns{0}, host_copies_wait_ns{0}, tick_wait_ns{0},
    copy_threads_wait_ns{0}, host_copy_waits{0};
struct WaitTimer {
    std::atomic<std::uint64_t>& total;
    std::chrono::steady_clock::time_point start =
        enabled ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    ~WaitTimer() {
        if (!enabled) return;
        total.fetch_add(std::chrono::duration_cast<std::chrono::nanoseconds>(
                            std::chrono::steady_clock::now() - start).count(),
                        std::memory_order_relaxed);
    }
};
/// Per-phase wall time of the GPU command thread draw path (ns), BB_FRAME_STATS only:
/// 0 filter, 1 pipeline lookup, 2 render state, 3 bind resources, 4 begin rendering, 5 emit + bind, 6 record + flush.
inline std::atomic<std::uint64_t> phase_ns[8]{};
/// Focused CPU timings for the Vulkan draw-recording path. Enabled only by
/// BB_MOTION_PROFILE=1; separate from BB_PHASES so profiling can be toggled
/// independently when diagnosing the object-motion path.
inline const bool motion_profile_enabled = [] {
    const char* env = std::getenv("BB_MOTION_PROFILE");
    return env && env[0] == '1';
}();
enum MotionProfilePhase : unsigned {
    MotionDrawRecord,
    MotionPrepareRenderState,
    MotionBindResources,
    MotionBeginRendering,
    MotionGate,
    MotionIndexRange,
    MotionPrepareDraw,
    MotionEmitDraw,
    MotionBeginMemoLookup,
    MotionBeginFull,
    MotionBeginSetup,
    MotionBeginColorAttachments,
    MotionBeginDepthAttachment,
    MotionBeginTail,
    MotionBeginMemoStore,
    MotionAttach,
    MotionAttachEnsureImage,
    MotionAttachEndRendering,
    MotionAttachRecord,
    MotionEnsureImageCreate,
    MotionEnsureViewCreate,
    MotionEnsureFinish,
    MotionPipelinePreparedCheck,
    MotionPipelineRefreshKey,
    MotionPipelineMapLookup,
    MotionPipelineRuntimeInfo,
    MotionPipelineFlatBuffer,
    MotionPipelineSpecialization,
    MotionPipelinePermutationLookup,
    MotionPipelineAddBindings,
    MotionPipelineFetchParse,
    MotionPipelineVertexAttribSpecs,
    MotionPipelineBufferSpecs,
    MotionPipelineImageSpecs,
    MotionPipelineFmaskSpecs,
    MotionPipelineSamplerSpecs,
    MotionPipelineTessSpec,
    MotionPipelineSpecEquality,
    MotionPipelineSpecMemoCheck,
    MotionProfilePhaseCount,
};
inline std::atomic<std::uint64_t> motion_profile_ns[MotionProfilePhaseCount]{};
inline std::atomic<std::uint64_t> motion_attach_calls{0}, motion_attach_transitions{0};
inline std::atomic<std::uint64_t> motion_image_recreates{0}, motion_image_finishes{0};
inline std::atomic<std::uint64_t> pipeline_prepared_hits{0}, pipeline_prepared_misses{0},
    pipeline_unprepared{0};
/// Draw preparation workers (BB_FRAME_STATS): submissions scanned, draws prepared, time spent and
/// claimed-but-idle time (ns), and the queue depth at the last print.
inline std::atomic<std::uint64_t> prep_scans{0}, prep_scan_ns{0}, prep_claims{0}, prep_draws{0},
    prep_apply_ns{0}, prep_wait_ns{0};
inline std::atomic<std::uint64_t> prep_queue_depth{0}, prep_backlog{0};
/// Draws a worker dropped because the GPU thread had already passed the submission, the sum of
/// how far ahead of the GPU thread each claimed submission was at claim time, and how often a
/// claim reached the packet loop with no draw left to prepare.
inline std::atomic<std::uint64_t> prep_skipped{0}, prep_ahead_sum{0}, prep_empty_claims{0};
/// Draws a worker walked, and the draws it dropped because the GPU thread had passed the
/// submission (as opposed to beyond the counted draws).
inline std::atomic<std::uint64_t> prep_seen{0}, prep_passed{0};
/// Why PrepareGraphicsPipeline failed in a worker (BB_FRAME_STATS): 0 the key could not be
/// refreshed, 1 program not compiled yet, 2 a guest memory read faulted, 3 no matching shader
/// permutation, 4 the worker gave up for another reason.
inline std::atomic<std::uint64_t> prep_failed[8]{};
inline void NotePrepFailure(unsigned reason) {
    if (enabled) {
        prep_failed[reason & 7].fetch_add(1, std::memory_order_relaxed);
    }
}
/// Why a prepared draw was not used (BB_FRAME_STATS): 0 not ready, 1 register checksum, 2 no program,
/// 3 program hash, 4 flattened user data, 5 pointer read, 6 fetch shader, 7 pipeline not in the map.
inline std::atomic<std::uint64_t> prepared_miss[8]{};
inline void NotePreparedMiss(unsigned reason) {
    if (enabled) {
        prepared_miss[reason & 7].fetch_add(1, std::memory_order_relaxed);
    }
}
inline std::atomic<std::uint64_t> pipeline_spec_memo_hits{0}, pipeline_spec_memo_misses{0};
struct MotionProfileTimer {
    std::atomic<std::uint64_t>& total;
    bool active = motion_profile_enabled;
    std::chrono::steady_clock::time_point start =
        motion_profile_enabled ? std::chrono::steady_clock::now()
                               : std::chrono::steady_clock::time_point{};
    void Stop() {
        if (active) {
            total.fetch_add(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                std::chrono::steady_clock::now() - start)
                                .count(),
                            std::memory_order_relaxed);
            active = false;
        }
    }
    ~MotionProfileTimer() { Stop(); }
};
/// Wall time per graphics packet opcode on the GPU command thread (ns), BB_FRAME_STATS only.
inline std::atomic<std::uint64_t> opcode_ns[256]{};
/// Wall time of the parts of the stage A draw hand-off (Rasterizer::PostDraw, ns), BB_PHASES only:
/// 0 fetch V# pointer reads, 1 written-buffer walk (NotePendingGpuWrite), 2 the stage loop
/// (constant ring copies and the packet size), 3 Begin() and packet assembly (memcpy of the
/// register blocks and user data), 4 Commit and FlushBatch.
inline std::atomic<std::uint64_t> post_ns[8]{};
/// Wall time in Rasterizer::Draw on stage A around those: 0 from the entry through
/// WaitQueuedMemoryWrites (mirror filter and the wait for the draw pipe), 1 the pipeline lookup
/// and the filter checks, 2 PostDraw.
inline std::atomic<std::uint64_t> draw_ns[3]{};
/// GPU thread rusage, refreshed after each graphics submission.
inline std::atomic<std::uint64_t> gpu_sys_us{0}, gpu_user_us{0}, gpu_invol_switches{0},
    gpu_vol_switches{0}, gpu_minor_faults{0};
/// Protection faults (signals) taken by the GPU thread itself.
inline std::atomic<std::uint64_t> gpu_signal_faults{0};
/// Protection changes: calls and pages, those removing write access (TLB shootdowns) apart.
inline std::atomic<std::uint64_t> protect_calls{0}, protect_pages{0}, protect_revoke_calls{0},
    protect_revoke_pages{0};
} // namespace BbStats
