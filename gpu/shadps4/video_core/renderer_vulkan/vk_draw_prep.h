// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: speculative draw preparation on worker threads (docs/parallel_gpu.md, steps 1-2).
//
// Every submitted graphics command buffer is copied. A scanner thread replays the register
// writes of the whole stream in order (ApplyGraphicsRegisterPacket, the same code the GPU
// thread runs) and records, per buffer, its starting checksum and a delta of the register
// blocks it wrote. Any number of workers (one per spare hardware thread) claim the nearest
// buffers ahead of the GPU thread, reach their starting state by applying deltas, and select
// the graphics pipeline and resource sharps of each direct draw. All helpers run as
// SCHED_IDLE: they only use idle cores. The GPU thread uses a prepared draw only when the
// running register checksums match and the flattened user data it computes itself equals the
// worker's; otherwise it takes the regular path. Workers never create or compile anything.

#pragma once

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <span>
#include <thread>
#include <utility>
#include <vector>

#include "common/types.h"
#include "video_core/amdgpu/regs.h"
#include "video_core/renderer_vulkan/vk_pipeline_cache.h"

namespace Vulkan {

struct PreparedStage {
    const Program* program;
    u64 hash;
    Shader::HwStage hw_stage;
    VAddr pgm_base;
    const u32* flat; ///< flattened user data the worker computed, owned by the submission
    u32 flat_size;
    // Resource sharps read from `flat` in the order of the program's resource lists, and the
    // texture description hash of each image (docs/parallel_gpu.md, step 2). Valid for the
    // GPU thread exactly when `flat` matched: sharps depend on nothing else.
    const AmdGpu::Image* image_sharps;
    const u64* image_hashes;
    const AmdGpu::Sampler* sampler_sharps;
    const AmdGpu::Buffer* buffer_sharps;
    u32 num_images, num_samplers, num_buffers;
    const Shader::Info::PointerRead* pointer_reads;
    u32 num_pointer_reads;
    VAddr fetch_code_address;
    const u32* fetch_code_snapshot;
    u32 fetch_code_words;
};

/// Key of Rasterizer's texture description cache for a T# bound through `res`.
inline u64 ImageDescHash(const AmdGpu::Image& sharp, const Shader::ImageResource& res) {
    std::array<u64, 4> key;
    std::memcpy(key.data(), &sharp, sizeof(key));
    const u32 flags = u32(res.is_written) | u32(res.is_depth) << 1 | u32(res.is_array) << 2;
    u64 hash = flags * 0x9E3779B97F4A7C15ull;
    for (const u64 word : key) {
        hash = (hash ^ word) * 0xFF51AFD7ED558CCDull;
        hash ^= hash >> 32;
    }
    return hash;
}

/// Vertex inputs of a draw (the dynamic vertex input path): the attribute and binding
/// descriptions, the V# of each stream, and the stream memory merged into ranges.
struct PreparedVertexInputs {
    struct Range {
        VAddr base, end;
    };
    const vk::VertexInputAttributeDescription2EXT* attributes;
    const vk::VertexInputBindingDescription2EXT* bindings;
    const AmdGpu::Buffer* buffers;
    const u8* range_index; ///< per stream with memory: its entry in `ranges`
    const Range* ranges;
    u32 count, num_ranges;
    u64 buffers_hash; ///< XXH3 of `buffers` (object motion stream identity)
    bool valid;
};

struct PreparedDraw {
    enum : u32 { Pending = 0, Ready = 1, Unavailable = 2 };
    std::atomic<u32> state{Pending};
    u64 reg_checksum{};
    GraphicsPipelineKey key{};
    u32 num_stages{};
    std::array<PreparedStage, MaxShaderStages> stages{};
    PreparedVertexInputs vertex{};
};

class DrawPreparation {
public:
    explicit DrawPreparation(PipelineCache& pipeline_cache);
    ~DrawPreparation();

    DrawPreparation(const DrawPreparation&) = delete;
    DrawPreparation& operator=(const DrawPreparation&) = delete;

    /// False while preparation has stood down (see SetAside).
    [[nodiscard]] bool Enabled() const noexcept;

    struct Submission;
    /// Game submit thread, outside the queue lock: copies and scans a top-level graphics
    /// command buffer. Null when disabled.
    std::shared_ptr<Submission> Build(std::span<const u32> commands);
    /// Game submit thread, under the queue lock: hands it to the workers in submission order.
    void Enqueue(u64 seq, std::shared_ptr<Submission> submission);

    /// GPU thread: brackets the processing of submission `seq`. The first call hands the
    /// workers the exact register state and checksum they start replaying from.
    void BeginSubmission(u64 seq, const AmdGpu::Regs& regs, u64 reg_checksum);
    void EndSubmission();

    /// GPU thread: starts speculative preparation for a nested indirect buffer from its exact
    /// entry register state. The caller brackets recursive IB execution with these methods.
    void BeginNested(std::span<const u32> commands, const AmdGpu::Regs& regs,
                     u64 reg_checksum);
    void EndNested();

    /// GPU thread: the prepared state for the next direct draw of the current submission.
    const PreparedDraw* NextDraw();

    /// GPU thread: counts whether a prepared draw was used; prints every 5 s (BB_FRAME_STATS).
    void Count(bool used);

    struct Submission {
        u64 seq{};
        std::vector<u32> commands;
        std::unique_ptr<PreparedDraw[]> draws;
        u32 num_draws{};
        std::vector<std::unique_ptr<u32[]>> flat_chunks; ///< from the claiming worker
        std::unique_ptr<AmdGpu::Regs> seed_regs; ///< set for an independently scanned nested IB
        std::vector<std::shared_ptr<Submission>> nested_jobs; ///< retained with the parent submit
        u64 start_checksum{}, end_checksum{}; ///< set by the scanner
        AmdGpu::RegDelta delta;                ///< registers written, values at the end
        std::atomic<bool> scanned{false};
        std::atomic<bool> claimed{false};
        std::atomic<bool> gpu_done{false};
    };

private:
    void ScannerLoop(std::stop_token stop);
    void WorkerLoop(std::stop_token stop, u32 index);
    void Collect();
    std::shared_ptr<Submission> BuildNested(std::span<const u32> commands,
                                            const AmdGpu::Regs& regs, u64 reg_checksum);

    /// bbport: a prepared draw is only worth the worker time, the per-submission copy and the
    /// walk when the GPU thread uses it. In DriveClub the tables a worker reads ahead of the GPU
    /// thread (the flattened user data and the resource descriptors inside it) are rewritten per
    /// draw, so most prepared draws cannot be used and the preparation itself costs ~7% of the
    /// frame. When a sample window shows the workers are wasting their time, preparation stands
    /// down for the rest of the session; this costs nothing when the feature does pay.
    void SetAsideFor(u64 samples, u64 usable);
    /// Called once per submission; kept for the API, the set-aside is for the session.
    void ConsiderResuming() {}
    /// True while preparation has stood down.
    [[nodiscard]] bool SetAside() const noexcept {
        return set_aside.load(std::memory_order_relaxed);
    }

    PipelineCache& pipeline_cache;
    std::unique_ptr<AmdGpu::Regs> initial_regs; ///< state at the start of `baseline_seq`
    u64 initial_checksum{};
    u64 baseline_seq{};
    /// State at the start of the oldest queued buffer (`tail_seq`): a worker whose position
    /// was collected restarts from here. Guarded by `mutex`.
    std::unique_ptr<AmdGpu::Regs> tail_regs;
    u64 tail_seq{};
    u64 tail_checksum{};
    u64 rebases = 0; ///< scanner restarts from the GPU thread state (BB_FRAME_STATS)
    bool baseline_ready{}; ///< guarded by `mutex`
    std::mutex mutex;
    std::condition_variable_any cv;
    std::deque<std::shared_ptr<Submission>> submissions; ///< ordered by seq
    std::deque<std::shared_ptr<Submission>> nested_submissions; ///< seeded from live IB entry state
    std::atomic<u64> gpu_seq{0};
    std::shared_ptr<Submission> current;

public:
    /// bbport: the current submission (prepared draws), for the draw recording thread to keep
    /// it alive while it still records its draws.
    [[nodiscard]] std::shared_ptr<const void> KeepCurrent() const {
        return current;
    }

private:
    u32 current_draw = 0;
    Submission* active_draws{};
    std::array<std::pair<Submission*, u32>, 32> nested_draw_stack{};
    u32 nested_draw_depth{};
    u64 used = 0, unused = 0;
    /// Draws counted since the last decision: how many prepared draws the GPU thread used.
    u64 window_used = 0, window_unused = 0;
    std::atomic<bool> set_aside{false};
    std::atomic<u64> resume_at_ns{0};
    u32 worker_count = 0; ///< fixed before the worker threads start
    std::jthread scanner;
    std::vector<std::jthread> workers;
};

} // namespace Vulkan
