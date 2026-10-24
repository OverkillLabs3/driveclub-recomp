// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: speculative draw preparation on worker threads (see vk_draw_prep.h).

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "bbport_threads.h"
#include "bbport_toggles.h"
#include "common/thread.h"
#include "video_core/amdgpu/liverpool.h"
#include "video_core/amdgpu/pm4_cmds.h"
#include "video_core/renderer_vulkan/vk_draw_prep.h"
#include "video_core/renderer_vulkan/liverpool_to_vk.h"
#include <xxhash.h>

namespace Vulkan {

namespace {

bool IsDirectDraw(AmdGpu::PM4ItOpcode opcode) {
    using AmdGpu::PM4ItOpcode;
    return opcode == PM4ItOpcode::DrawIndex2 || opcode == PM4ItOpcode::DrawIndexOffset2 ||
           opcode == PM4ItOpcode::DrawIndexAuto;
}

/// Walks the type-3 packets of a command buffer (type-2 padding skipped).
template <typename Func>
void ForEachPacket(std::span<const u32> commands, Func&& func) {
    for (size_t at = 0; at < commands.size();) {
        const auto* header = reinterpret_cast<const AmdGpu::PM4Header*>(commands.data() + at);
        if (header->type == 2) {
            ++at;
            continue;
        }
        if (header->type != 3) {
            return;
        }
        const size_t words = header->type3.NumWords() + 1;
        if (at + words > commands.size()) {
            return;
        }
        func(header);
        at += words;
    }
}

u32 DefaultWorkerCount() {
    if (const char* env = std::getenv("BB_PREP_WORKERS")) {
        return static_cast<u32>(std::clamp(std::atoi(env), 0, 16));
    }
    // Reserve capacity for the game and GPU threads; preparation workers run below normal
    // priority and claim whole buffers (Steam Deck 4, 16-thread desktop 8).
    return std::clamp<u32>(BbThreads::Available() / 2, 1, 8);
}

/// Flattened user data of one submission: chunks never move, so the GPU thread can read a
/// prepared draw's data while the worker keeps appending.
struct FlatArena {
    static constexpr size_t ChunkWords = 16 * 1024;
    std::vector<std::unique_ptr<u32[]>> chunks;
    size_t used = ChunkWords;

    const u32* Append(const std::vector<u32>& data) {
        return AppendArray(data.data(), data.size());
    }

    /// Trivially copyable elements, 8-byte aligned (sharps hold u64 bit fields).
    template <typename T>
    const T* AppendArray(const T* data, size_t count) {
        static_assert(std::is_trivially_copyable_v<T> && alignof(T) <= 8);
        const size_t words = (count * sizeof(T) + sizeof(u32) - 1) / sizeof(u32);
        used = (used + 1) & ~size_t(1);
        if (chunks.empty() || words > ChunkWords - std::min(used, ChunkWords)) {
            chunks.push_back(std::make_unique<u32[]>(std::max(ChunkWords, words)));
            used = 0;
        }
        u32* dst = chunks.back().get() + used;
        if (count) {
            std::memcpy(dst, data, count * sizeof(T));
        }
        used += words;
        return reinterpret_cast<const T*>(dst);
    }
};

/// Reads the stage's resource sharps from the worker's flattened user data, so the GPU thread
/// only binds them (the sharp fetches and texture-description hashing were ~7% of it).
void PrepareResources(const Shader::Info& info, PreparedStage& stage, FlatArena& arena) {
    thread_local std::vector<AmdGpu::Image> images;
    thread_local std::vector<u64> hashes;
    thread_local std::vector<AmdGpu::Sampler> samplers;
    thread_local std::vector<AmdGpu::Buffer> buffers;
    images.clear();
    hashes.clear();
    samplers.clear();
    buffers.clear();
    for (const auto& res : info.images) {
        images.push_back(res.GetSharp(info));
        hashes.push_back(ImageDescHash(images.back(), res));
    }
    for (const auto& res : info.samplers) {
        samplers.push_back(res.GetSharp(info));
    }
    for (const auto& res : info.buffers) {
        buffers.push_back(res.IsSpecial() ? AmdGpu::Buffer::Null() : res.GetSharp(info));
    }
    stage.image_sharps = arena.AppendArray(images.data(), images.size());
    stage.image_hashes = arena.AppendArray(hashes.data(), hashes.size());
    stage.sampler_sharps = arena.AppendArray(samplers.data(), samplers.size());
    stage.buffer_sharps = arena.AppendArray(buffers.data(), buffers.size());
    stage.num_images = static_cast<u32>(images.size());
    stage.num_samplers = static_cast<u32>(samplers.size());
    stage.num_buffers = static_cast<u32>(buffers.size());
}

/// Fetch inputs may live behind user-data pointers, outside the flattened snapshot. Preserve
/// those exact reads so the GPU thread can reject stale speculative pipeline/resource data.
void CaptureFetchReadState(const Shader::Info& info,
                           const Shader::Gcn::FetchShaderData& fetch, PreparedStage& stage,
                           FlatArena& arena) {
    thread_local std::vector<Shader::Info::PointerRead> reads;
    reads.clear();
    reads.reserve(fetch.attributes.size());
    for (const auto& attrib : fetch.attributes) {
        Shader::Info::PointerRead read{attrib.sgpr_base, attrib.dword_offset, 0, {}};
        const auto sharp = info.ReadUdReg<AmdGpu::Buffer>(attrib.sgpr_base, attrib.dword_offset);
        std::memcpy(read.words, &sharp, sizeof(sharp));
        reads.push_back(read);
    }
    stage.pointer_reads = arena.AppendArray(reads.data(), reads.size());
    stage.num_pointer_reads = static_cast<u32>(reads.size());

    const u32* code = Shader::Gcn::GetFetchShaderCode(info, info.fetch_shader_sgpr_base);
    const u32 words = (fetch.size + sizeof(u32) - 1) / sizeof(u32);
    stage.fetch_code_address = reinterpret_cast<VAddr>(code);
    stage.fetch_code_words = words;
    stage.fetch_code_snapshot = arena.AppendArray(code, words);
}

/// Rasterizer::BindVertexBuffers' pure part (GraphicsPipeline::GetVertexInputs for the dynamic
/// vertex input path, and the merge of the stream memory into ranges).
void PrepareVertexInputs(const Shader::Info& vs, const Shader::Gcn::FetchShaderData& fetch,
                         const AmdGpu::Regs& regs, PreparedVertexInputs& out, FlatArena& arena) {
    using InstanceIdType = Shader::Gcn::VertexAttribute::InstanceIdType;
    thread_local std::vector<vk::VertexInputAttributeDescription2EXT> attributes;
    thread_local std::vector<vk::VertexInputBindingDescription2EXT> bindings;
    thread_local std::vector<AmdGpu::Buffer> buffers;
    thread_local std::vector<PreparedVertexInputs::Range> ranges, merged;
    thread_local std::vector<u8> range_index;
    attributes.clear();
    bindings.clear();
    buffers.clear();
    ranges.clear();
    merged.clear();
    range_index.clear();
    for (const auto& attrib : fetch.attributes) {
        const auto step_rate = attrib.GetStepRate();
        const auto buffer = attrib.GetSharp(vs);
        // The V# may come from user data the guest is still writing: an unknown format leaves
        // the inputs to the GPU thread, which reads them once the draw is submitted (the
        // assertion in SurfaceFormat stopped the process here).
        const vk::Format format =
            LiverpoolToVK::TrySurfaceFormat(buffer.GetDataFmt(), buffer.GetNumberFmt());
        if (format == vk::Format::eUndefined &&
            buffer.GetDataFmt() != AmdGpu::DataFormat::FormatInvalid) {
            out.valid = false;
            return;
        }
        attributes.push_back({
            .location = attrib.semantic,
            .binding = attrib.semantic,
            .format = format,
            .offset = 0,
        });
        bindings.push_back({
            .binding = attrib.semantic,
            .stride = buffer.GetStride(),
            .inputRate = step_rate == InstanceIdType::None ? vk::VertexInputRate::eVertex
                                                           : vk::VertexInputRate::eInstance,
            .divisor = step_rate == InstanceIdType::OverStepRate0 ? regs.vgt_instance_step_rate_0
                       : step_rate == InstanceIdType::OverStepRate1
                           ? regs.vgt_instance_step_rate_1
                           : 1u,
        });
        buffers.push_back(buffer);
        if (buffer.base_address != 0 && buffer.GetSize() > 0) {
            ranges.push_back({buffer.base_address, buffer.base_address + buffer.GetSize()});
        }
    }
    std::ranges::sort(ranges, {}, &PreparedVertexInputs::Range::base);
    for (const auto& range : ranges) {
        if (merged.empty() || merged.back().end < range.base) {
            merged.push_back(range);
        } else {
            merged.back().end = std::max(merged.back().end, range.end);
        }
    }
    for (const auto& buffer : buffers) {
        if (buffer.base_address != 0 && buffer.GetSize() > 0) {
            const auto it = std::ranges::find_if(merged, [&](const auto& range) {
                return buffer.base_address >= range.base && buffer.base_address < range.end;
            });
            range_index.push_back(static_cast<u8>(it - merged.begin()));
        }
    }
    out.attributes = arena.AppendArray(attributes.data(), attributes.size());
    out.bindings = arena.AppendArray(bindings.data(), bindings.size());
    out.buffers = arena.AppendArray(buffers.data(), buffers.size());
    out.range_index = arena.AppendArray(range_index.data(), range_index.size());
    out.ranges = arena.AppendArray(merged.data(), merged.size());
    out.count = static_cast<u32>(attributes.size());
    out.num_ranges = static_cast<u32>(merged.size());
    out.buffers_hash =
        buffers.empty() ? 0 : XXH3_64bits(buffers.data(), buffers.size() * sizeof(AmdGpu::Buffer));
    out.valid = true;
}

} // namespace

DrawPreparation::DrawPreparation(PipelineCache& pipeline_cache_)
    : pipeline_cache{pipeline_cache_} {
    // Prepared draws are only handed out while the feature is on: with it off the workers
    // would copy and prepare every submission for nothing.
    worker_count = BbToggle::Disabled(BbToggle::DrawPreparation) ? 0 : DefaultWorkerCount();
    if (worker_count) {
        scanner = std::jthread([this](std::stop_token stop) { ScannerLoop(stop); });
    }
    for (u32 i = 0; i < worker_count; ++i) {
        workers.emplace_back([this, i](std::stop_token stop) { WorkerLoop(stop, i); });
    }
    std::printf("GPU: draw preparation workers: %u (+1 scanner, %u hardware threads)\n",
                worker_count, BbThreads::Available());
}

DrawPreparation::~DrawPreparation() {
    for (auto& worker : workers) {
        worker.request_stop();
    }
    if (scanner.joinable()) {
        scanner.request_stop();
    }
    cv.notify_all();
    workers.clear();
    scanner = {};
}

bool DrawPreparation::Enabled() const noexcept {
    // The runtime toggle file is loaded after Rasterizer construction, so worker_count alone
    // is not enough: workers can exist even though this path is disabled for the session.
    // Avoid copying and preparing submissions whose results the GPU thread will reject.
    return worker_count != 0 && !BbToggle::Disabled(BbToggle::DrawPreparation) && !SetAside();
}

void DrawPreparation::SetAsideFor(u64 samples, u64 usable) {
    static const bool adaptive = [] {
        const char* env = std::getenv("BB_PREP_ADAPT");
        return !env || env[0] != '0';
    }();
    if (!adaptive) {
        return;
    }
    if (set_aside.exchange(true, std::memory_order_relaxed)) {
        return;
    }
    // One way for the session: the workers park, Build() stops copying command buffers and
    // BeginSubmission() hands the GPU thread no prepared draws, which is the same state the
    // BB_TOGGLE_FILE switch leaves behind. Nothing has to be resumed in the middle of a
    // submission, which is where a second attempt at this crashed.
    std::printf("Draw preparation: set aside for the rest of the session (%llu of %llu prepared "
                "draws were used; the workers read tables the game rewrites per draw)\n",
                static_cast<unsigned long long>(usable),
                static_cast<unsigned long long>(samples));
}

std::shared_ptr<DrawPreparation::Submission> DrawPreparation::Build(
    std::span<const u32> commands) {
    ConsiderResuming();
    if (!Enabled()) {
        return {};
    }
    auto submission = std::make_shared<Submission>();
    submission->commands.assign(commands.begin(), commands.end());
    ForEachPacket(submission->commands, [&](const AmdGpu::PM4Header* header) {
        submission->num_draws += IsDirectDraw(header->type3.opcode);
    });
    submission->draws = std::make_unique<PreparedDraw[]>(submission->num_draws);
    return submission;
}

void DrawPreparation::Enqueue(u64 seq, std::shared_ptr<Submission> submission) {
    if (!submission) {
        return;
    }
    submission->seq = seq;
    {
        std::scoped_lock lk{mutex};
        submissions.push_back(std::move(submission));
    }
    cv.notify_all();
}

void DrawPreparation::BeginSubmission(u64 seq, const AmdGpu::Regs& regs, u64 reg_checksum) {
    current_draw = 0;
    active_draws = nullptr;
    nested_draw_depth = 0;
    current.reset();
    if (!Enabled()) {
        return;
    }
    gpu_seq.store(seq, std::memory_order_release);
    std::scoped_lock lk{mutex};
    if (!baseline_ready) {
        initial_regs = std::make_unique<AmdGpu::Regs>(regs);
        tail_regs = std::make_unique<AmdGpu::Regs>(regs);
        initial_checksum = tail_checksum = reg_checksum;
        baseline_seq = tail_seq = seq;
        baseline_ready = true;
        cv.notify_all();
    }
    // Idle helpers can starve on a busy CPU. Rather than queueing without bound, restart
    // them from the GPU thread's own state (one register copy) once the scanner lags.
    constexpr u64 MaxScanLag = 64;
    if (!submissions.empty() && seq > tail_seq + MaxScanLag &&
        submissions.front()->seq < seq && !submissions.front()->scanned.load()) {
        *tail_regs = regs;
        tail_seq = seq;
        tail_checksum = reg_checksum;
        while (!submissions.empty() && submissions.front()->seq < seq) {
            submissions.pop_front();
        }
        ++rebases;
        cv.notify_all();
    }
    if (!submissions.empty() && seq >= submissions.front()->seq &&
        seq - submissions.front()->seq < submissions.size()) {
        current = submissions[seq - submissions.front()->seq];
    }
    active_draws = current.get();
}

void DrawPreparation::EndSubmission() {
    if (current) {
        current->gpu_done.store(true, std::memory_order_release);
        current.reset();
    }
    active_draws = nullptr;
    nested_draw_depth = 0;
    if (Enabled()) {
        Collect();
    }
}

std::shared_ptr<DrawPreparation::Submission> DrawPreparation::BuildNested(
    std::span<const u32> commands, const AmdGpu::Regs& regs, u64 reg_checksum) {
    if (!Enabled() || !current || BbToggle::Disabled(BbToggle::DrawPreparation)) {
        return {};
    }

    auto submission = std::make_shared<Submission>();
    submission->seq = current->seq;
    submission->commands.assign(commands.begin(), commands.end());
    submission->seed_regs = std::make_unique<AmdGpu::Regs>(regs);
    submission->start_checksum = reg_checksum;
    ForEachPacket(submission->commands, [&](const AmdGpu::PM4Header* header) {
        submission->num_draws += IsDirectDraw(header->type3.opcode);
    });
    if (submission->num_draws == 0) {
        return {};
    }
    submission->draws = std::make_unique<PreparedDraw[]>(submission->num_draws);
    submission->scanned.store(true, std::memory_order_release);
    current->nested_jobs.push_back(submission);
    {
        std::scoped_lock lk{mutex};
        nested_submissions.push_back(submission);
    }
    cv.notify_all();
    return submission;
}

void DrawPreparation::BeginNested(std::span<const u32> commands, const AmdGpu::Regs& regs,
                                  u64 reg_checksum) {
    ASSERT(nested_draw_depth < nested_draw_stack.size());
    nested_draw_stack[nested_draw_depth++] = {active_draws, current_draw};
    auto nested = BuildNested(commands, regs, reg_checksum);
    active_draws = nested.get();
    current_draw = 0;
}

void DrawPreparation::EndNested() {
    ASSERT(nested_draw_depth > 0);
    if (active_draws) {
        active_draws->gpu_done.store(true, std::memory_order_release);
        if (active_draws->seed_regs) {
            std::scoped_lock lk{mutex};
            std::erase_if(nested_submissions, [&](const auto& submission) {
                return submission.get() == active_draws;
            });
        }
    }
    const auto [previous_draws, previous_index] = nested_draw_stack[--nested_draw_depth];
    active_draws = previous_draws;
    current_draw = previous_index;
}

const PreparedDraw* DrawPreparation::NextDraw() {
    if (!active_draws || current_draw >= active_draws->num_draws) {
        return nullptr;
    }
    const PreparedDraw* draw = &active_draws->draws[current_draw++];
    if (BbToggle::Disabled(BbToggle::DrawPreparation)) {
        return nullptr;
    }
    return draw;
}

void DrawPreparation::Collect() {
    std::scoped_lock lk{mutex};
    while (!submissions.empty()) {
        const auto& front = submissions.front();
        if (!baseline_ready || !front->gpu_done.load(std::memory_order_acquire)) {
            break;
        }
        if (front->seq >= baseline_seq) {
            // Its delta moves the restart state to the start of the next buffer.
            if (!front->scanned.load(std::memory_order_acquire)) {
                break;
            }
            front->delta.Apply(*tail_regs);
            tail_seq = front->seq + 1;
            tail_checksum = front->end_checksum;
        }
        submissions.pop_front();
    }
}

void DrawPreparation::ScannerLoop(std::stop_token stop) {
    Common::SetCurrentThreadName("bb:DrawScan");
    BbThreads::MakePreparationWorker();
    std::unique_ptr<AmdGpu::Regs> regs;
    u64 checksum = 0;
    u64 next_seq = 0;
    {
        std::unique_lock lk{mutex};
        cv.wait(lk, stop, [&] { return baseline_ready; });
        if (stop.stop_requested()) {
            return;
        }
        regs = std::make_unique<AmdGpu::Regs>(*initial_regs);
        checksum = initial_checksum;
        next_seq = baseline_seq;
    }
    AmdGpu::RegDirty dirty;
    while (!stop.stop_requested()) {
        std::shared_ptr<Submission> submission;
        {
            std::unique_lock lk{mutex};
            cv.wait(lk, stop, [&] {
                return next_seq < tail_seq ||
                       (!submissions.empty() && submissions.back()->seq >= next_seq);
            });
            if (stop.stop_requested()) {
                return;
            }
            if (next_seq < tail_seq) {
                // Rebased by the GPU thread: continue from its state.
                *regs = *tail_regs;
                checksum = tail_checksum;
                next_seq = tail_seq;
                continue;
            }
            // Unscanned buffers are only collected by a rebase, so `next_seq` is still queued.
            submission = submissions[next_seq - submissions.front()->seq];
        }
        submission->start_checksum = checksum;
        dirty.Clear();
        const auto scan_start = std::chrono::steady_clock::now();
        ForEachPacket(submission->commands, [&](const AmdGpu::PM4Header* header) {
            AmdGpu::Liverpool::ApplyGraphicsRegisterPacket(*regs, header, checksum, &dirty);
        });
        if (BbStats::enabled) {
            BbStats::prep_scans.fetch_add(1, std::memory_order_relaxed);
            BbStats::prep_scan_ns.fetch_add(
                u64(std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now() - scan_start)
                        .count()),
                std::memory_order_relaxed);
        }
        submission->delta.Capture(*regs, dirty);
        submission->end_checksum = checksum;
        {
            std::scoped_lock lk{mutex};
            submission->scanned.store(true, std::memory_order_release);
        }
        ++next_seq;
        cv.notify_all();
    }
}

void DrawPreparation::WorkerLoop(std::stop_token stop, u32 index) {
    Common::SetCurrentThreadName(("bb:DrawPrep" + std::to_string(index)).c_str());
    BbThreads::MakePreparationWorker();
    auto regs = std::make_unique<AmdGpu::Regs>();
    constexpr u64 NoPosition = ~0ull;
    u64 position = NoPosition; ///< `regs` holds the state at the start of this buffer
    PipelineSelection sel{};
    sel.regs = regs.get();
    PrepWorker prep_worker{};
    sel.worker = &prep_worker;
    FlatArena arena;
    std::vector<std::shared_ptr<Submission>> path;

    // The nearest scanned buffer the GPU thread has not reached and no worker has claimed.
    const auto claimable = [&]() -> std::shared_ptr<Submission> {
        const u64 gpu = gpu_seq.load(std::memory_order_acquire);
        for (const auto& submission : nested_submissions) {
            if (submission->seq >= gpu &&
                submission->scanned.load(std::memory_order_acquire) &&
                !submission->gpu_done.load(std::memory_order_acquire) &&
                !submission->claimed.load(std::memory_order_relaxed)) {
                return submission;
            }
        }
        for (const auto& submission : submissions) {
            if (submission->seq >= gpu && submission->seq >= tail_seq &&
                submission->scanned.load(std::memory_order_acquire) &&
                !submission->claimed.load(std::memory_order_relaxed)) {
                return submission;
            }
        }
        return {};
    };

    while (!stop.stop_requested()) {
        std::shared_ptr<Submission> target;
        const auto wait_start = std::chrono::steady_clock::now();
        {
            std::unique_lock lk{mutex};
            // gpu_seq moves without the lock: keep the candidate the predicate found.
            cv.wait(lk, stop, [&] {
                target = baseline_ready ? claimable() : nullptr;
                return target != nullptr;
            });
            if (stop.stop_requested() || !target) {
                return;
            }
            target->claimed.store(true, std::memory_order_relaxed);
            if (BbStats::enabled) {
                BbStats::prep_wait_ns.fetch_add(
                    u64(std::chrono::duration_cast<std::chrono::nanoseconds>(
                            std::chrono::steady_clock::now() - wait_start)
                            .count()),
                    std::memory_order_relaxed);
                BbStats::prep_claims.fetch_add(1, std::memory_order_relaxed);
                BbStats::prep_queue_depth.store(submissions.size(), std::memory_order_relaxed);
                const u64 gpu_now = gpu_seq.load(std::memory_order_acquire);
                BbStats::prep_ahead_sum.fetch_add(target->seq > gpu_now ? target->seq - gpu_now : 0,
                                                  std::memory_order_relaxed);
            }
            if (target->seed_regs) {
                // Nested IB preparation starts from the GPU thread's exact entry snapshot;
                // do not replay top-level deltas across this independent work item.
                *regs = *target->seed_regs;
                position = NoPosition;
                std::erase(nested_submissions, target);
            } else {
                // Buffers between this worker's state and the target: their deltas. A position
                // already collected (or past the target) restarts from the queue's tail state.
                if (position == NoPosition || position < tail_seq || position > target->seq) {
                    *regs = *tail_regs;
                    position = tail_seq;
                }
                const u64 front = submissions.front()->seq;
                path.clear();
                for (u64 seq = position; seq < target->seq; ++seq) {
                    path.push_back(submissions[seq - front]);
                }
            }
        }
        for (const auto& step : path) {
            step->delta.Apply(*regs);
        }
        path.clear();
        const auto apply_start = std::chrono::steady_clock::now();
        u64 checksum = target->start_checksum;
        u32 ordinal = 0;
        arena = FlatArena{};
        ForEachPacket(target->commands, [&](const AmdGpu::PM4Header* header) {
            AmdGpu::Liverpool::ApplyGraphicsRegisterPacket(*regs, header, checksum);
            if (!IsDirectDraw(header->type3.opcode)) {
                return;
            }
            if (BbStats::enabled) {
                BbStats::prep_seen.fetch_add(1, std::memory_order_relaxed);
                if (ordinal >= target->num_draws) {
                    BbStats::prep_skipped.fetch_add(1, std::memory_order_relaxed);
                } else if (!(target->seq >= gpu_seq.load(std::memory_order_acquire))) {
                    BbStats::prep_passed.fetch_add(1, std::memory_order_relaxed);
                }
            }
            // Past the GPU thread the draws are no longer worth preparing.
            if (ordinal < target->num_draws &&
                target->seq >= gpu_seq.load(std::memory_order_acquire)) {
                auto& draw = target->draws[ordinal];
                prep_worker.stages.clear();
                prep_worker.failed = false;
                prep_worker.failure_reason = 0;
                sel.draw_indirect_params = {};
                if (pipeline_cache.PrepareGraphicsPipeline(sel)) {
                    draw.reg_checksum = checksum;
                    draw.key = sel.graphics_key;
                    draw.num_stages = static_cast<u32>(prep_worker.stages.size());
                    for (u32 i = 0; i < draw.num_stages; ++i) {
                        const auto& stage = prep_worker.stages[i];
                        draw.stages[i] = {
                            .program = stage.program,
                            .hash = stage.hash,
                            .hw_stage = stage.hw_stage,
                            .pgm_base = stage.pgm_base,
                            .flat = arena.Append(*stage.flat),
                            .flat_size = static_cast<u32>(stage.flat->size()),
                        };
                    }
                    bool resource_snapshot_valid = true;
                    sigjmp_buf resource_recover;
                    sigjmp_buf* const previous_recover = runtime_fault_recover;
                    if (sigsetjmp(resource_recover, 0) == 0) {
                        runtime_fault_recover = &resource_recover;
                        const auto* vs = sel.infos[static_cast<u32>(Shader::SwStage::Vertex)];
                        if (vs && sel.fetch_shader) {
                            for (u32 i = 0; i < draw.num_stages; ++i) {
                                auto& stage = draw.stages[i];
                                auto& info = prep_worker.infos.at(stage.program);
                                if (&info == vs) {
                                    CaptureFetchReadState(info, *sel.fetch_shader, stage, arena);
                                    break;
                                }
                            }
                        }
                        for (u32 i = 0; i < draw.num_stages; ++i) {
                            const auto& stage = draw.stages[i];
                            // Descriptor reads and vertex pointer tables are tied to the flat
                            // snapshot; the GPU thread revalidates both before using this data.
                            if (!BbToggle::Disabled(BbToggle::PreparedResources)) {
                                PrepareResources(prep_worker.infos.at(stage.program),
                                                 draw.stages[i], arena);
                            }
                        }
                        draw.vertex.valid = false;
                        if (!BbToggle::Disabled(BbToggle::PreparedResources)) {
                            const auto* vs = sel.infos[static_cast<u32>(Shader::SwStage::Vertex)];
                            if (vs && sel.fetch_shader) {
                                PrepareVertexInputs(*vs, *sel.fetch_shader, *regs, draw.vertex,
                                                    arena);
                            }
                        }
                        runtime_fault_recover = previous_recover;
                    } else {
                        runtime_fault_recover = previous_recover;
                        resource_snapshot_valid = false;
                    }
                    draw.state.store(resource_snapshot_valid ? PreparedDraw::Ready
                                                              : PreparedDraw::Unavailable,
                                     std::memory_order_release);
                    if (BbStats::enabled) {
                        BbStats::prep_draws.fetch_add(1, std::memory_order_relaxed);
                    }
                } else {
                    draw.state.store(PreparedDraw::Unavailable, std::memory_order_release);
                }
            }
            ++ordinal;
        });
        if (BbStats::enabled) {
            BbStats::prep_apply_ns.fetch_add(
                u64(std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now() - apply_start)
                        .count()),
                std::memory_order_relaxed);
        }
        // The arena's chunks live as long as the submission.
        target->flat_chunks = std::move(arena.chunks);
        if (!target->seed_regs) {
            position = target->seq + 1;
        }
    }
}

void DrawPreparation::Count(bool was_used) {
    ++(was_used ? used : unused);
    ++(was_used ? window_used : window_unused);
    // Decide on the sample window. A prepared draw has to be usable most of the time to pay for
    // the per-submission copy and the workers: in DriveClub about a quarter are, and turning the
    // preparation off was 7% faster, so the bar is three in four.
    constexpr u64 WindowSamples = 2048;
    if (window_used + window_unused >= WindowSamples) {
        const u64 samples = window_used + window_unused;
        if (window_used * 4 < samples * 3) {
            SetAsideFor(samples, window_used);
        }
        window_used = window_unused = 0;
    }
    static const bool stats = std::getenv("BB_FRAME_STATS") != nullptr;
    // The clock is read every 1024 draws: per draw it was 8% of the GPU thread.
    if (!stats || ((used + unused) & 1023) != 0) {
        return;
    }
    static auto window = std::chrono::steady_clock::now();
    const auto now = std::chrono::steady_clock::now();
    if (now - window < std::chrono::seconds(5)) {
        return;
    }
    window = now;
    std::printf("Draw preparation: %llu prepared pipelines used, %llu not (%u workers, "
                "%llu scanner rebases)\n",
                static_cast<unsigned long long>(used), static_cast<unsigned long long>(unused),
                worker_count, static_cast<unsigned long long>(rebases));
    if (BbStats::enabled) {
        // bbport: what the workers and the scanner did with their time.
        const double claims = double(BbStats::prep_claims.exchange(0));
        const double draws = double(BbStats::prep_draws.exchange(0));
        const double skipped = double(BbStats::prep_skipped.exchange(0));
        const double ahead = double(BbStats::prep_ahead_sum.exchange(0));
        std::printf("  workers: %.0f claims/s, %.0f draws prepared/s, %.0f drawn packets/s "
                    "(%.0f beyond the counted draws, %.0f dropped after the GPU thread passed), "
                    "%.0f draws per claim, %.1f submissions ahead at claim, %.1f ms/s applying, "
                    "%.1f ms/s waiting (all workers); scanner %.0f submissions/s (%.1f ms/s); "
                    "queue %llu\n",
                    claims / 5.0, draws / 5.0, double(BbStats::prep_seen.exchange(0)) / 5.0,
                    skipped / 5.0, double(BbStats::prep_passed.exchange(0)) / 5.0,
                    claims > 0 ? draws / claims : 0.0, claims > 0 ? ahead / claims : 0.0,
                    double(BbStats::prep_apply_ns.exchange(0)) / 5e6,
                    double(BbStats::prep_wait_ns.exchange(0)) / 5e6,
                    double(BbStats::prep_scans.exchange(0)) / 5.0,
                    double(BbStats::prep_scan_ns.exchange(0)) / 5e6,
                    static_cast<unsigned long long>(
                        BbStats::prep_queue_depth.load(std::memory_order_relaxed)));
        std::printf("  not prepared/s: key refresh failed %.0f, program not compiled %.0f, guest "
                    "read faulted %.0f, no matching permutation %.0f, other %.0f\n",
                    double(BbStats::prep_failed[0].exchange(0)) / 5.0,
                    double(BbStats::prep_failed[1].exchange(0)) / 5.0,
                    double(BbStats::prep_failed[2].exchange(0)) / 5.0,
                    double(BbStats::prep_failed[3].exchange(0)) / 5.0,
                    double(BbStats::prep_failed[4].exchange(0)) / 5.0);
    }
    used = unused = 0;
}

} // namespace Vulkan
