// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <unordered_map>

#include "common/recursive_lock.h"
#include "common/shared_first_mutex.h"
#include "video_core/buffer_cache/buffer_cache.h"
#include "video_core/page_manager.h"
#include "video_core/renderer_vulkan/vk_bind_helper.h"
#include "video_core/renderer_vulkan/vk_camera_motion.h"
#include "video_core/renderer_vulkan/vk_drive_aa.h"
#include "video_core/renderer_vulkan/vk_constant_ring.h"
#include "video_core/renderer_vulkan/vk_scene_resolution.h"
#include "video_core/renderer_vulkan/vk_object_motion.h"
#include "video_core/renderer_vulkan/vk_draw_pipe.h"
#include "video_core/renderer_vulkan/vk_draw_prep.h"
#include "video_core/renderer_vulkan/vk_temporal_upscaler.h"
#include "video_core/renderer_vulkan/vk_pipeline_cache.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/texture_cache/texture_cache.h"

namespace AmdGpu {
struct Liverpool;
}

namespace Core {
class MemoryManager;
}

namespace Vulkan {

class GraphicsPipeline;
class Runtime;

class Rasterizer {
public:
    explicit Rasterizer(const Instance& instance, Scheduler& scheduler, Runtime& runtime,
                        AmdGpu::Liverpool* liverpool);
    ~Rasterizer();

    [[nodiscard]] Runtime& GetRuntime() noexcept {
        return runtime;
    }

    [[nodiscard]] VideoCore::BufferCache& GetBufferCache() noexcept {
        return buffer_cache;
    }

    [[nodiscard]] TemporalUpscaler& GetUpscaler() noexcept {
        return *upscaler;
    }

    [[nodiscard]] VideoCore::TextureCache& GetTextureCache() noexcept {
        return texture_cache;
    }

    /// bbport: HDR output: the game's picture before its UI for the display buffer at `address`
    /// (a UNORM view of an 8-bit copy, ShaderReadOnlyOptimal), or null when that frame had no 3D
    /// scene (menus). The presenter reads it after the frame, on the same queue.
    [[nodiscard]] vk::ImageView HdrHudless(VAddr address) const;
    /// The same frame's 3D scene before tone mapping (linear HDR, ShaderReadOnlyOptimal), or null.
    [[nodiscard]] vk::ImageView HdrScene(VAddr address) const;

    void Draw(bool is_indexed, u32 index_offset = 0, const PreparedDraw* prepared = nullptr);

    /// bbport: draw preparation workers (vk_draw_prep.h), fed and consumed by Liverpool.
    DrawPreparation& GetDrawPreparation() {
        return *draw_prep;
    }
    void DrawIndirect(bool is_indexed, VAddr arg_address, u32 offset, u32 size, u32 max_count,
                      VAddr count_address, u16 vertex_sgpr_offset, u16 instance_sgpr_offset);

    void DispatchDirect();
    void DispatchIndirect(VAddr address, u32 offset, u32 size);

    void ScopeMarker(fmt::string_view fmt, fmt::format_args args, auto&& func) {
        if (host_markers_enabled) {
            ScopeMarkerBegin(fmt::vformat(fmt, args));
            func();
            ScopeMarkerEnd();
        } else {
            func();
        }
    }

    void ScopeMarkerBegin(const std::string_view& str, bool from_guest = false);
    void ScopeMarkerEnd(bool from_guest = false);
    void ScopedMarkerInsert(const std::string_view& str, bool from_guest = false);
    void ScopedMarkerInsertColor(const std::string_view& str, const u32 color,
                                 bool from_guest = false);

    void FillBuffer(VAddr address, u32 num_bytes, u32 value, bool is_gds);
    void CopyBuffer(VAddr dst, VAddr src, u32 num_bytes, bool dst_gds, bool src_gds);
    u32 ReadDataFromGds(u32 gsd_offset);
    /// bbport: the next submission ends with a device -> host memory barrier (a host read of GPU-written memory
    /// after waiting for it, e.g. ReadDataFromGds after Finish()).
    void RequestHostReadBarrier() {
        scheduler.RequestHostReadBarrier();
    }
    bool InvalidateMemory(VAddr addr, u64 size, bool assume_locks = false);
    /// GPU thread, before a write the guest can observe (see Scheduler::WaitHostCopies).
    void WaitHostCopies() {
        DrainDrawPipe();
        scheduler.WaitHostCopies();
    }

    /// bbport: GPU command thread: waits until the draw recording thread has recorded every
    /// draw handed to it (vk_draw_pipe.h); no-op on other threads.
    /// `line`/`function`: the caller, for the statistics of where stage A waits.
    void DrainDrawPipe(u32 reason = DrawPipe::ReasonRasterizer, u32 line = __builtin_LINE(),
                       const char* function = __builtin_FUNCTION());
    /// Runs `task(rasterizer, copy of data)` in order with the draws: on the draw recording
    /// thread while the draw pipeline is in use (PipelinedTasks), else here after a drain.
    using OrderedTask = void (*)(Rasterizer& rasterizer, const u8* data);
    /// Returns true when the task was handed to the recording thread (not run yet).
    /// `writes_guest_memory`: a PipelinedMemoryWrites task that writes guest memory, so the next
    /// pipeline selection waits for it (WaitQueuedMemoryWrites); a flip signal or a DMA into GDS does not.
    bool RunInOrder(OrderedTask task, const void* data, u32 size,
                    u64 toggle = BbToggle::PipelinedTasks, bool writes_guest_memory = true);
    /// bbport: stage A, before a pipeline selection: WriteData/DMA queued for stage B may write the
    /// descriptor tables the selection walks; wait until stage B has run the last one.
    void WaitQueuedMemoryWrites();
    u64 last_memory_task = 0;
    /// Stage A: the draw pipe position after the last handed-over packet, and whether the
    /// recording thread has run everything before a position.
    [[nodiscard]] u64 DrawPipeHead() const {
        return draw_pipe ? draw_pipe->Head() : 0;
    }
    [[nodiscard]] bool DrawPipeReached(u64 position) const {
        return !draw_pipe || draw_pipe->Reached(position);
    }
    /// Stage A: no draws waiting for the recording thread.
    [[nodiscard]] bool DrawPipeIdle() const {
        return !draw_pipe || draw_pipe->Idle();
    }
    /// Stage A, end of a submission: its prepared draws stay alive until the recording thread
    /// has recorded them (instead of a drain).
    void RetireSubmission();
    /// Stage A: guest memory the recording thread will write for work handed to it (storage
    /// buffers, DMA, WriteData, fences); constants overlapping it are bound there, not copied here.
    void NotePendingGpuWrite(VAddr address, u64 size);
    /// Before a guest-visible write: fences deferred earlier are written first.
    void WaitDeferredSignals() {
        scheduler.WaitDeferredSignals();
    }
    /// Runs `signal` after the guest memory copies issued so far, without waiting here.
    void SignalAfterHostCopies(std::function<void()> signal) {
        scheduler.SignalAfterHostCopies(std::move(signal));
    }
    /// The GPU command thread or the draw recording thread (fault handling runs inline there).
    bool IsGpuSideThread() const;
    bool IsGpuSideThreadId(u32 tid) const;
    /// A guest write hit a protected page.
    bool OnWriteFault(VAddr addr, bool assume_locks);
    bool ReadMemory(VAddr addr, u64 size, bool assume_locks = false);
    /// bbport: BB_FAULT_EMULATE: true unless every byte of the range is GPU mapped and not GPU written.
    bool IsRangeGpuWritten(VAddr addr, u64 size);
    /// bbport: BufferCache::SnapshotGpuRange (background readers).
    bool SnapshotGpuRange(VAddr addr, u64 size, void* out) {
        return buffer_cache.SnapshotGpuRange(addr, size, out);
    }
    /// bbport: BufferCache::RecordTaggedSnapshot / ReadTaggedSnapshot (BB_EXPOSURE_EXACT).
    bool RecordTaggedSnapshot(VAddr addr, u32 size, u32 tag, u64 id) {
        return buffer_cache.RecordTaggedSnapshot(addr, size, tag, id);
    }
    /// bbport: BB_EXACT_EOP_FLUSH: ends the submission after an EOP's snapshot copies (on the thread that runs the
    /// EOP, which owns the scheduler as for the draw-batch flush).
    void FlushAtExactEop() {
        scheduler.Flush();
    }
    bool ReadTaggedSnapshot(VAddr addr, u32 size, u32 tag, u64 id, void* out, bool* pending, u64* ticks,
                            u64 wait_ns = 0) {
        return buffer_cache.ReadTaggedSnapshot(addr, size, tag, id, out, pending, ticks, wait_ns);
    }
    /// bbport: BB_FAULT_EMULATE: queues a guest store that touches no GPU-written byte (BufferCache::
    /// QueueUntrackedStore); false when the usual write fault path must run.
    bool StoreUntracked(VAddr addr, const void* data, u32 size);
    /// bbport: BB_FAULT_EMULATE: whether the page holds GPU data (lock-free, any thread).
    bool IsPageGpuModified(VAddr addr);
    void ProcessDownloadImages();
    bool IsMapped(VAddr addr, u64 size);
    void MapMemory(VAddr addr, u64 size);
    void RegisterMemory(VAddr addr, u64 size);
    void UnmapMemory(VAddr addr, u64 size);

    u64 Flush();
    void Finish();
    void OnSubmit();

    PipelineCache& GetPipelineCache() {
        return pipeline_cache;
    }

    template <typename Func>
    void ForEachMappedRangeInRange(VAddr addr, u64 size, Func&& func) {
        const auto range = decltype(mapped_ranges)::interval_type::right_open(addr, addr + size);
        Common::RecursiveSharedLock lock{mapped_ranges_mutex};
        for (const auto& mapped_range : (mapped_ranges & range)) {
            func(mapped_range);
        }
    }

    std::thread::id GetGpuCommandProcessorThread();
#if defined(__linux__) || defined(_WIN32)
    u32 GetGpuCommandProcessorThreadId();
#endif

private:
    void PrepareRenderState(const GraphicsPipeline* pipeline);
    RenderState BeginRendering(const GraphicsPipeline* pipeline);
    RenderState BeginRenderingFull(const GraphicsPipeline* pipeline);
    /// bbport: a draw continuing the open render pass with the same inputs gets the same render
    /// state (RenderStateMemo); nothing can have broken the pass in between (barriers, copies and
    /// dispatches end it).
    struct BeginSignature {
        std::array<VideoCore::ImageId, AmdGpu::NUM_COLOR_BUFFERS + 1> ids{};
        std::array<VideoCore::ImageViewInfo, AmdGpu::NUM_COLOR_BUFFERS + 1> views{};
        u32 mrt_mask = 0;
        u32 num_samples = 0;
        std::array<u8, AmdGpu::NUM_COLOR_BUFFERS> color_samples{};
        bool motion = false;
        bool scene_started = false;
        bool raster_scaling = false;
        u64 upscaler_state = 0;
        u64 generation = 0;
        u32 depth_control = 0;
        bool depth_valid = false;
        bool stencil_valid = false;
        bool operator==(const BeginSignature&) const = default;
    };
    BeginSignature MakeBeginSignature(const GraphicsPipeline* pipeline) const;
    struct BeginMemo {
        bool valid = false;
        BeginSignature signature;
        RenderState state;
        u32 scene_size = 0;
        std::array<float, 2> target_scale{};
    } begin_memo;
    u64 begin_memo_hits = 0, begin_memo_misses = 0;
    /// bbport: inputs of the last UpdateDynamicState: the register words its five
    /// Update* functions read, and the members they use. Equal inputs give equal dynamic state, which is still
    /// in DynamicState (only those functions write it), so they are skipped; CommitWith runs every draw.
    /// BB_DYN_MEMO=0 turns it off.
    struct DynMemo {
        static constexpr std::array<std::pair<u32, u32>, 9> Ranges{{
            {0xA000, 20}, {0xA080, 52}, {0xA103, 108}, {0xA200, 7}, {0xA282, 1},
            {0xA292, 1}, {0xA2A5, 1}, {0xA2DF, 5}, {0xC242, 1}}};
        static constexpr u32 Words = 196;
        bool valid = false;
        const GraphicsPipeline* pipeline = nullptr;
        bool is_indexed = false;
        bool feedback_loop = false;
        std::array<float, 2> jitter{};
        std::array<float, 2> scale{};
        std::array<u32, Words> words{};
    };
    mutable DynMemo dyn_memo;
    mutable u64 dyn_memo_hits = 0, dyn_memo_misses = 0;
    void Resolve();
    void DepthStencilCopy(bool is_depth, bool is_stencil);
    void EliminateFastClear();

    void UpdateDynamicState(const GraphicsPipeline* pipeline, bool is_indexed) const;
    void UpdateViewportScissorState() const;
    void UpdateDepthStencilState() const;
    void UpdatePrimitiveState(bool is_indexed) const;
    void UpdateRasterizationState() const;
    void UpdateColorBlendingState(const GraphicsPipeline* pipeline) const;

    bool FilterDraw();
    bool FilterDrawPasses() const;
    void FlushLongBatch();
    void CaptureHdrHudless(VideoCore::ImageId display, VAddr address);
    void InvalidateHdrHudless(VAddr address);
    u32 batch_draws{};
    u64 batch_tick{};
    /// Everything of a direct draw after the pipeline selection (GPU thread or stage B).
    void DrawRecord(const GraphicsPipeline* pipeline, const PreparedDraw* used_prepared,
                    bool is_indexed, u32 index_offset);
    static bool DrawPipeWanted();
    bool UseDrawPipe() const;
    bool OnStageA() const;
    /// Arguments of an indirect draw: `args` holds `max_count` commands `stride` apart.
    struct IndirectDraw {
        VAddr args;
        VAddr count; ///< the draw count, or 0 for max_count draws
        u32 stride;
        u32 max_count;
    };
    /// Everything of an indirect draw after the pipeline selection (GPU thread or stage B).
    void DrawIndirectRecord(const GraphicsPipeline* pipeline, bool is_indexed,
                            const IndirectDraw& indirect);
    /// Everything of an indirect dispatch after the pipeline selection.
    void DispatchIndirectRecord(const ComputePipeline* pipeline, VAddr args, u32 size);
    /// Hands a draw (or, with `cs`, a dispatch) to the recording thread.
    void PostDraw(const Pipeline* pipeline, const PreparedDraw* used_prepared, bool is_indexed,
                  u32 index_offset, const AmdGpu::ComputeProgram* cs = nullptr,
                  const IndirectDraw* indirect = nullptr);
    /// Everything of a direct dispatch after the pipeline selection (GPU thread or stage B).
    void DispatchRecord(const ComputePipeline* pipeline);
    /// The compute registers of the dispatch being recorded.
    const AmdGpu::ComputeProgram& CsRegs() const;
    static void RunDrawPacket(void* rasterizer, const u8* packet, u32 size);
    struct RingBinding;
    void CollectRingBindings(const Shader::Info& stage, const PreparedDraw* prepared,
                             boost::container::static_vector<RingBinding, Shader::NUM_BUFFERS>& out);
    void PrintPipeStats();
    /// The registers of the draw being recorded: stage B's copy there, else Liverpool's.
    const AmdGpu::Regs& Regs() const;
    AmdGpu::CbDbExtent CbExtent(u32 index) const;
    AmdGpu::CbDbExtent DbExtent() const;

    void BindBuffers(const Shader::Info& stage, const PreparedStage* prepared,
                     Shader::Backend::Bindings& binding,
                     Shader::PushData& push_data, u32& write_index);
    /// `on_helper`: run by the texture binding helper; out-of-date images are not refreshed.
    void BindTextures(const Shader::Info& stage, const PreparedStage* prepared,
                      Shader::Backend::Bindings& binding, u32& write_index, bool& barrier,
                      bool on_helper = false);
    bool BindResources(const Pipeline* pipeline);
    void BindSamplers(const Shader::Info& stage, const PreparedStage* prepared,
                      Shader::Backend::Bindings& binding, u32& write_index, u32 sampler0 = 0);
    /// bbport: a stage's resolved textures remembered by its prepared T# hashes
    /// (TextureSetMemo): a hit only redoes the per-draw effects (binding flags, transitions).
    struct TextureSetEntry {
        VideoCore::ImageId id{}; ///< after the depth redirect; null descriptor when invalid
        vk::ImageView view;
        const void* backing = nullptr;
        VideoCore::SubresourceRange range;
        bool proxy = false; ///< sampled from the scene proxy (view: SampleProxy on each use)
    };
    struct TextureSet {
        u64 key = 0;
        const Shader::Info* stage = nullptr;
        u64 generation = ~0ull;
        u64 scene_generation = 0; ///< SceneTargets::Generation of the proxy views
        u32 count = 0;
        static constexpr u32 MaxImages = 16; ///< larger sets are not memoized
        std::array<u64, MaxImages> hashes{};
        std::array<TextureSetEntry, MaxImages> entries{};
    };
    std::array<TextureSet, 32768> texture_sets{};
    u64 texture_set_hits = 0, texture_set_misses = 0;
    std::array<u64, 4> texture_set_why{}; ///< misses: other key, generation, image check, new
    /// Returns true when the stage's images were bound from the memo.
    bool BindTexturesFromSet(const Shader::Info& stage, const PreparedStage* prepared,
                             u32 first_image_idx, bool& barrier, TextureSet*& slot);
    /// The prepared sharps of `stage` in the current prepared draw, if they fit it.
    const PreparedStage* FindPreparedStage(const Shader::Info& stage) const;
    /// Whether the helper may bind textures for this pipeline at all (fixed descriptor layout,
    /// no storage writes that invalidate cached images).
    bool HelperEligible(const Pipeline* pipeline) const;
    /// Whether every texture of `stage` takes the memoized path without refreshing, creating or
    /// redirecting anything that records commands or touches the buffer cache.
    bool TexturesBindableOnHelper(const Shader::Info& stage, const PreparedStage* prepared);
    static void RunTextureTask(void* rasterizer);
    static void JoinBindHelper(void* rasterizer);

    /// Display pass: counts the frame (BbStats::gpu_frames) and, with BB_BUFFER_STATS, the lag.
    void NoteFrameStart();
    /// BB_GPU_PROFILE: a timestamp where a render pass starts (vk_gpu_profiler.h).
    void MarkPass(const GraphicsPipeline* pipeline, const RenderState& state);
    void BindVertexBuffers(const GraphicsPipeline* pipeline,
                           const PreparedDraw* prepared = nullptr);
    void BindIndexBuffer(u32 index_offset = 0);
    /// bbport: BindVertexBuffers/BindIndexBuffer in two halves: obtaining the buffers
    /// (vertex_binds, index_bind) and recording the binds.
    void ResolveVertexBuffers(const GraphicsPipeline* pipeline, const PreparedDraw* prepared);
    void EmitVertexBuffers();
    void ResolveIndexBuffer(u32 index_offset);
    void EmitIndexBuffer();

    void ResetBindings(bool is_compute);

    bool IsComputeMetaClear(const Pipeline* pipeline);
    bool IsComputeImageCopy(const Pipeline* pipeline);
    bool IsComputeImageClear(const Pipeline* pipeline);

private:
    friend class VideoCore::BufferCache;

    const Instance& instance;
    Scheduler& scheduler;
    Runtime& runtime;
    VideoCore::PageManager page_manager;
    VideoCore::BufferCache buffer_cache;
    VideoCore::TextureCache texture_cache;
    // Host-only expansion: high-resolution tile lists must not overwrite guest allocations.
    std::unordered_map<VAddr, std::unique_ptr<VideoCore::Buffer>> lighting_lists;
    AmdGpu::Liverpool* liverpool;
    Core::MemoryManager* memory;
    boost::icl::interval_set<VAddr> mapped_ranges;
    Common::SharedFirstMutex mapped_ranges_mutex;
    std::atomic<u64> mapped_ranges_generation{0}; ///< bbport: bumped by every map and unmap
    PipelineCache pipeline_cache;
    std::unique_ptr<DrawPreparation> draw_prep;
    std::unique_ptr<DriveAA> drive_aa;
    std::unique_ptr<CameraMotion> camera_motion; // bbport: motion vectors (docs/upscaler.md)
    std::unique_ptr<SceneTargets> scene_targets;
    bool scene_started = false;
    std::unique_ptr<ObjectMotion> object_motion;
    std::unique_ptr<VideoCore::Buffer> crumb_buffer; ///< AMD buffer marker target (see BbRecent::crumb)
    bool motion_draw = false;
    u64 motion_geometry{};    ///< vertex-stream identity of the current direct draw
    bool gbuffer_draw = false;
    int vr_draw_targets = 0; // bbport: VR: the colour targets of the current draw (its G-buffer pass has three)
    /// Frame generation: the scene depth (the G-buffer pass), the first draw into the display buffer
    /// since other work (the UI starts: the picture before it is the HUD-less frame).
    VideoCore::ImageId fg_depth{};
    u64 fg_depth_area = 0;
    /// HDR output: a 3D scene since the last display draw; the next display draw (the UI's first)
    /// copies the display buffer first; the copies, one per display buffer.
    bool hdr_scene = false, hdr_capture_next = false, hdr_capture_pending = false;
    /// The composite's HDR scene: a game image, or DriveAA's upscaled picture.
    VideoCore::ImageId hdr_scene_image{};
    bool hdr_scene_upscaled = false;
    struct HdrHudlessCopy {
        VAddr address{};
        VideoCore::UniqueImage image;
        vk::UniqueImageView view;
        u32 width{}, height{};
        vk::Format format{};
        std::atomic<bool> valid{false};
        /// The scene before tone mapping (linear), copied at the same time; its view samples it.
        VideoCore::UniqueImage scene;
        vk::UniqueImageView scene_view;
        u32 scene_width{}, scene_height{};
        vk::Format scene_format{};
        std::atomic<bool> scene_valid{false};
    };
    std::array<HdrHudlessCopy, 4> hdr_hudless;
    u32 hdr_hudless_next = 0;
    bool fg_in_display = false;
    u32 fg_display_draws = 0; ///< draws into the display buffer since other work
    bool fg_capture_pending = false;
    bool vr_hud_pending = false; ///< VR: the next display draw that is not a scene overlay starts the HUD
    VideoCore::ImageId vr_depth{}; ///< VR: this frame's scene depth (the biggest G-buffer pass's)
    u64 vr_depth_area = 0;
    /// A compute pass writes one of the mirror scene's targets while that scene is skipped: it would filter
    /// an already filtered picture again, so the dispatch is dropped as well.
    bool skip_mirror_dispatch = false;
    std::unique_ptr<TemporalUpscaler> upscaler; // bbport: FSR (docs/upscaler.md)
    std::array<float, 2> draw_jitter{};         ///< viewport offset of the current draw, pixels
    std::array<float, 2> target_scale{1.0f, 1.0f}; ///< pass drawn into the upscaler's output-size images
    const bool host_markers_enabled;
    const bool guest_markers_enabled;

    using RenderTargetInfo = std::pair<VideoCore::ImageId, VideoCore::TextureCache::ImageDesc>;
    std::array<RenderTargetInfo, AmdGpu::NUM_COLOR_BUFFERS> cb_descs;
    std::pair<VideoCore::ImageId, VideoCore::TextureCache::ImageDesc> db_desc;
    boost::container::static_vector<vk::DescriptorImageInfo, Shader::NUM_IMAGES> image_infos;
    boost::container::static_vector<vk::DescriptorBufferInfo, Shader::NUM_BUFFERS * 2>
        buffer_infos; // VR multiview: two per guest buffer (the eyes)
    boost::container::static_vector<VideoCore::ImageId, Shader::NUM_IMAGES> bound_images;
    struct BoundBuffer {
        const VideoCore::Buffer* buffer;
        u64 offset;
        u32 size;
        bool is_written;
    };
    boost::container::static_vector<BoundBuffer, Shader::NUM_BUFFERS> bound_buffers;

    u32 set_write_index{};
    Pipeline::DescriptorWrites set_writes;
    Shader::PushData push_data;

    // bbport: bindings point at their description instead of copying it (a hot spot): into
    // image_desc_cache for memoized lookups (pinned for the current BindTextures call), else
    // into image_desc_storage.
    using ImageBindingInfo = std::pair<VideoCore::ImageId, const VideoCore::TextureCache::ImageDesc*>;
    // bbport: texture descriptions depend only on the T# and three resource flags; building
    // them (mip layout sizes) for every binding of every draw was a hot spot.
    struct ImageDescCacheEntry {
        std::array<u64, 4> sharp{};
        u32 flags = ~0u;
        VideoCore::TextureCache::ImageDesc desc;
        // Memoized FindImage for bindings without mip overrides (TextureBindingMemo).
        u64 found_generation = ~0ULL;
        VideoCore::ImageId found_id{};
        VideoCore::TextureCache::ImageDesc found_desc;
        u64 pinned = 0; ///< bind_epoch of the BindTextures call referencing found_desc
        u64 last_use = 0;
        // Memoized FindView for found_id (after the depth redirect): valid while the entry's
        // FindImage memo holds and the image keeps this backing (TextureViewMemo).
        VideoCore::TextureCache::ViewMemo view_memo;
    };
    std::array<ImageDescCacheEntry, 4096> image_desc_cache{};
    u64 desc_use_counter = 0;
    /// Entries replacing pinned cache slots during one BindTextures call.
    boost::container::static_vector<ImageDescCacheEntry, Shader::NUM_IMAGES> image_desc_overflow;
    boost::container::static_vector<VideoCore::TextureCache::ImageDesc, Shader::NUM_IMAGES * 2>
        image_desc_storage;
    u64 bind_epoch = 0;
    // bbport: render/depth target lookups memoized by their raw register bytes while image
    // registrations are unchanged (the descriptions depend only on those registers).
    struct TargetMemo {
        std::array<u8, 256> key{};
        u32 key_size = 0;
        u64 generation = ~0ULL;
        VideoCore::ImageId image_id{};
        VideoCore::TextureCache::ImageDesc desc;
    };
    std::array<TargetMemo, 64> target_memo{};
    // bbport: consecutive draws mostly keep their targets; the slot's description (cb_descs,
    // db_desc) is then still the right one and is neither looked up nor copied.
    struct LastTarget {
        std::array<u8, 256> key{};
        u32 key_size = 0;
        u64 generation = ~0ULL;
        VideoCore::ImageId image_id{};
    };
    std::array<LastTarget, AmdGpu::NUM_COLOR_BUFFERS + 1> last_targets{}; ///< CBs, then DB
    template <typename... Parts>
    VideoCore::ImageId FindTargetMemoized(VideoCore::TextureCache::ImageDesc& desc,
                                          LastTarget& last, auto&& make_desc,
                                          const Parts&... parts);
    /// The prepared draw whose pipeline the current Draw uses (its sharps are valid), or null.
    const PreparedDraw* bind_prepared = nullptr;
    /// `hash` is ImageDescHash(sharp, res), computed by a draw-preparation worker or here.
    ImageDescCacheEntry& CachedImageDescEntry(const AmdGpu::Image& sharp,
                                              const Shader::ImageResource& res, u64 hash);
    ImageDescCacheEntry& CachedImageDescEntry(const AmdGpu::Image& sharp,
                                              const Shader::ImageResource& res) {
        return CachedImageDescEntry(sharp, res, ImageDescHash(sharp, res));
    }
    const VideoCore::TextureCache::ImageDesc& CachedImageDesc(const AmdGpu::Image& sharp,
                                                              const Shader::ImageResource& res) {
        return CachedImageDescEntry(sharp, res).desc;
    }
    boost::container::static_vector<ImageBindingInfo, Shader::NUM_IMAGES> image_bindings;
    struct VertexBinds {
        VertexInputs<vk::VertexInputAttributeDescription2EXT> attributes;
        VertexInputs<vk::VertexInputBindingDescription2EXT> bindings;
        VertexInputs<vk::Buffer> host_buffers;
        VertexInputs<vk::DeviceSize> host_offsets;
        VertexInputs<vk::DeviceSize> host_sizes;
        VertexInputs<vk::DeviceSize> host_strides;
        u32 num_buffers = 0;
    } vertex_binds;
    struct IndexBind {
        vk::Buffer handle;
        u64 offset;
        vk::IndexType type;
    } index_bind{};
    /// The direct draw whose vertex/index buffers BindResources may resolve early.
    struct DrawInputs {
        const GraphicsPipeline* pipeline;
        const PreparedDraw* prepared;
        u32 index_offset;
        bool is_indexed;
        bool pending;
        bool resolved;
    } draw_inputs{};
    // bbport: textures of a draw bound on the helper thread while this thread binds buffers.
    static bool BindHelperWanted();
    BindHelper bind_helper{BindHelperWanted()};
    struct TextureTask {
        struct Stage {
            const Shader::Info* info;
            const PreparedStage* prepared;
            Shader::Backend::Bindings binding;
            u32 write_index;
        };
        std::array<Stage, Shader::MaxStageTypes> stages;
        u32 count = 0;
        u32 completed = 0; ///< stages the helper bound; the rest are bound after the join
        bool barrier = false;
    } texture_task;
    /// Draws whose textures the helper bound completely, partly or not at all (BB_FRAME_STATS).
    u64 helper_full = 0, helper_partial = 0, helper_serial = 0;
    /// The memoized cache entry of each image binding (FindImage memo taken), else null.
    boost::container::static_vector<ImageDescCacheEntry*, Shader::NUM_IMAGES> image_binding_entries;
    bool fault_process_pending{};
    bool attachment_feedback_loop{};
    bool needs_barrier{};
    // bbport: two-stage draw pipeline (stage B state; the thread is the last member so it
    // stops first).
    static inline thread_local const AmdGpu::Regs* stage_regs = nullptr;
    static inline thread_local const AmdGpu::ComputeProgram* stage_cs = nullptr;
    AmdGpu::Regs pipe_regs{};
    bool pipe_synced = false;
    std::array<AmdGpu::CbDbExtent, AmdGpu::NUM_COLOR_BUFFERS> pipe_cb_extent{};
    AmdGpu::CbDbExtent pipe_db_extent{};
    /// Stage A: small read-only guest buffers copied into the constant ring for a packet.
    struct RingBinding {
        u32 index;  ///< buffer resource of the stage
        u32 size;
        u64 offset; ///< in the ring
        VAddr address;
        bool host_copy{}; ///< also in the ring's cached copy: read it there (ConstantRing::HostCopy)
    };
    struct PendingWrite {
        VAddr begin;
        VAddr end;
        u64 position;
    };
    std::vector<PendingWrite> pending_writes;
    /// bbport: position of a write noted for the packet being built (resolved after its Commit).
    static constexpr u64 UnresolvedWrite = ~0ull;
    void ResolvePendingWrites();
    VAddr pending_min = ~VAddr{0}, pending_max = 0; ///< bounds of pending_writes
    u32 pending_checks = 0;
    u64 proxy_samples = 0; ///< texture bindings that read a scene proxy (statistics)
    float sampler_lod_bias = 0.0f; ///< bbport: extra bias of this draw's samplers
    bool scene_debug_frame = false; ///< BB_SCENE_DEBUG: this frame's passes are printed
    bool PendingWriteOverlaps(VAddr address, u64 size);
    /// Stage B: the ring bindings of the stages of the packet being recorded.
    struct RingStage {
        const Shader::Info* info;
        const RingBinding* bindings;
        u32 count;
    };
    std::array<RingStage, Shader::MaxStageTypes> ring_stages{};
    u32 num_ring_stages = 0;
    const RingBinding* FindRingBinding(const Shader::Info& stage, u32 index) const {
        for (u32 i = 0; i < num_ring_stages; ++i) {
            if (ring_stages[i].info == &stage) {
                for (u32 j = 0; j < ring_stages[i].count; ++j) {
                    if (ring_stages[i].bindings[j].index == index) {
                        return &ring_stages[i].bindings[j];
                    }
                }
                return nullptr;
            }
        }
        return nullptr;
    }
    std::unique_ptr<ConstantRing> constant_ring;
    /// Submissions (prepared draws) kept alive until stage B reaches the position.
    std::deque<std::pair<u64, std::shared_ptr<const void>>> pipe_keepalive;
    std::unique_ptr<DrawPipe> draw_pipe;
};

} // namespace Vulkan
