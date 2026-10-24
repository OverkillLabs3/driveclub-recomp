// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <condition_variable>
#include <functional>
#include <mutex>
#include <span>
#include <unordered_map>
#include <unordered_set>

#include "bbport_copy.h"

#include <deque>
#include <boost/container/small_vector.hpp>

#include "common/interval_set.h"
#include "common/types.h"
#include "video_core/buffer_cache/buffer.h"
#include "video_core/buffer_cache/fault_manager.h"
#include "video_core/buffer_cache/range_set.h"
#include "video_core/buffer_cache/gpu_byte_map.h"
#include "video_core/renderer_vulkan/vk_semaphore.h"
#include "video_core/renderer_vulkan/vk_staging_buffer_pool.h"

namespace AmdGpu {
struct Liverpool;
}

namespace Core {
class MemoryManager;
}

namespace Vulkan {
class GraphicsPipeline;
struct SubmitInfo;
class Runtime;
class StagingBufferPool;
} // namespace Vulkan

namespace VideoCore {

class TextureCache;
class MemoryTracker;
class PageManager;

class BufferCache {
    static constexpr u64 ADDRESS_SPACE_BITS = 40;
    // bbport: arena pages are 4 GiB unless the driver's buffer size limit is lower (AMD's
    // Windows driver: 2 GiB); an arena spans up to two pages, so they shrink to half the limit.
    static constexpr u64 MAX_ARENA_PAGE_BITS = 32;
    static constexpr u64 MIN_ARENA_PAGE_BITS = 30;
    static constexpr u64 MAX_ARENA_PAGES = u64{1} << (ADDRESS_SPACE_BITS - MIN_ARENA_PAGE_BITS);
    static constexpr u64 MIN_BLOCK_SIZE = 16_KB;

public:
    /// Read-only bindings up to this size are copied into a stream buffer (bbport: public for
    /// the draw pipeline's constant ring).
    static constexpr u64 STREAM_THRESHOLD = 16_KB;

    explicit BufferCache(const Vulkan::Instance& instance, Vulkan::Scheduler& scheduler,
                         Vulkan::Runtime& runtime, AmdGpu::Liverpool* liverpool,
                         TextureCache& texture_cache, PageManager& tracker);
    ~BufferCache();

    /// Returns a pointer to GDS device local buffer.
    [[nodiscard]] const Buffer* GetGdsBuffer() const noexcept {
        return &gds_buffer;
    }

    /// Retrieves the device local DBA page table buffer.
    [[nodiscard]] Buffer* GetBdaPageTableBuffer() noexcept {
        return bda_pagetable_buffer.get();
    }

    /// Retrieves the fault buffer.
    [[nodiscard]] Buffer* GetFaultBuffer() noexcept {
        return fault_manager->GetFaultBuffer();
    }

    /// Retrieves the stream buffer.
    StreamBuffer& GetStreamBuffer() noexcept {
        return stream_buffer;
    }

    /// bbport: Rasterizer installs this after it created the draw pipe (see drain_gpu_pipe).
    void SetDrainGpuPipe(std::function<void()>&& drain) {
        drain_gpu_pipe = std::move(drain);
    }

    /// bbport: whether the GPU wrote (or a download is under way for) any byte of the range: a guest read of
    /// other bytes on the same protected pages needs no download (BB_FAULT_EMULATE). Any thread, lock-free.
    [[nodiscard]] bool IsRangeGpuWritten(VAddr addr, u64 size) const {
        return gpu_byte_map.Any(addr, size);
    }

    /// bbport: BB_FAULT_EMULATE, any thread: a guest store to bytes no GPU write holds, on a page protected for
    /// other bytes. Written to guest memory now; the arena copy is updated by the cache owner before it records
    /// further work (ApplyQueuedStores). False when not eligible.
    bool QueueUntrackedStore(VAddr addr, const void* data, u32 size);
    /// The checks of QueueUntrackedStore without the store (any thread).
    bool CanQueueUntrackedStore(VAddr addr, u32 size);
    /// The cache owner (GPU command thread or draw recording thread): inline updates of the queued stores.
    void ApplyQueuedStores();

    /// bbport: see gpu_byte_map.h.
    GpuByteMap gpu_byte_map;
    struct QueuedStore {
        VAddr lo, hi;
    };
    std::mutex queued_store_mutex;
    std::vector<QueuedStore> queued_stores;
    std::atomic<u32> num_queued_stores{0};

    /// bbport: the stream buffer: only the host writes it, no GPU hazard to track.
    [[nodiscard]] bool IsHostWrittenBuffer(const Buffer* buffer) const noexcept;

    /// Returns minimum granularity of a sparse memory bind.
    u32 GetSparsePageShift() const noexcept {
        return block_shift;
    }

    /// Invalidates any buffer in the logical page range.
    void InvalidateMemory(VAddr device_addr, u64 size, bool assume_locks = false);

    /// bbport: a guest write fault at `device_addr` was handled; unprotect its neighbourhood.
    void ExtendWriteFault(VAddr device_addr);

    /// Flushes any GPU modified buffer in the logical page range back to CPU memory.
    void ReadMemory(VAddr device_addr, u64 size, bool is_write = false, bool assume_locks = false);

    /// Finds a buffer for the specified region.
    [[nodiscard]] std::pair<const Buffer*, u64> ObtainBuffer(VAddr device_addr, u32 size,
                                                             bool is_written,
                                                             bool is_texel_buffer = false);

    /// Attempts to obtain a buffer without modifying the cache contents.
    [[nodiscard]] std::pair<const Buffer*, u64> ObtainBufferForImage(VAddr device_addr, u32 size);

    /// Return true when a region is modified from the CPU
    [[nodiscard]] bool IsRegionCpuModified(VAddr addr, size_t size);

    /// Return true when a region is modified from the GPU
    [[nodiscard]] bool IsRegionGpuModified(VAddr addr, size_t size);

    /// Processes the fault buffer.
    void ProcessFaultBuffer();

    /// Synchronizes all buffers needed for DMA.
    void SynchronizeDmaBuffers();

    /// Commits pending sparse buffer memory binds. Must be called before every scheduler submit.
    void SubmitPendingArenaBinds(Vulkan::SubmitInfo& info);

    /// bbport: BB_READBACK_EARLY: at the end of each submission (submit callback, GPU thread), copies the
    /// GPU-modified ranges of the readback windows it wrote that the guest read recently into staging. A guest
    /// read then waits for that submission only, not for everything recorded until the read fault.
    void RecordEarlyReadbacks();
    /// bbport: the draw or dispatch whose bindings were stamped is recorded (Rasterizer::ResetBindings).
    void NoteBindingsRecorded() {
        unrecorded_windows.clear();
        any_unrecorded.store(false, std::memory_order_relaxed);
    }

private:
    struct ArenaBinds {
        const Buffer* arena;
        boost::container::small_vector<vk::SparseMemoryBind, 32> binds;
    };

    ArenaBinds* BindsForArena(const Buffer* arena) {
        auto it = std::ranges::find(pending_binds, arena, &ArenaBinds::arena);
        if (it != pending_binds.end()) {
            return std::addressof(*it);
        }
        return &pending_binds.emplace_back(arena);
    }

    void GuardWholeArena(const Buffer* arena);

public:
    std::string DescribeFault(u64 gpu_address);

private:
    const Buffer* GetArena(u64 first_block, u64 last_block);

    void EnsureResident(const Buffer* arena, u64 first_block, u64 last_block);

    void DownloadMemory(const Buffer* arena, VAddr device_addr, u64 size);

    struct Readback;
    using DownloadCopies = boost::container::small_vector<vk::BufferCopy, 1>;
    /// Moves the GPU modified ranges of a span out of gpu_modified_ranges into copies from the arena,
    /// packed from offset 0. Returns the packed size.
    u64 CollectDownloads(const Buffer* arena, VAddr device_addr, u64 size, DownloadCopies& copies);
    /// Downloads a request inside one readback window while the GPU thread keeps running: the faulting
    /// thread (not the GPU command thread) waits for the copy. False: take the synchronous download.
    bool OffloadReadback(VAddr device_addr, u64 size, bool is_write);
    void RecordReadback(Readback& job);
    void FinishReadback(const Readback& job);
    bool IsReadbackPending(VAddr addr, u64 size) const;
    void MergeReadbackReturns();

    bool SynchronizeMemory(const Buffer* arena, VAddr device_addr, u32 size, bool is_written,
                           bool is_texel_buffer);

    /// bbport: batched small copies on the copy threads (BbCopy::QueueCopy).
    static void RunGuestCopy(const BbCopy::Item& item);
    void SmallGuestCopy(const BbCopy::Item& item);

    const Buffer* UploadCopies(const Buffer* arena, std::span<vk::BufferCopy> copies,
                               size_t total_size_bytes);

    bool SynchronizeMemoryFromImage(const Buffer* arena, VAddr device_addr, u32 size);

    const Vulkan::Instance& instance;
    Vulkan::Scheduler& scheduler;
    // bbport: texel buffer ranges known not to alias an image, per image registry generation.
    struct ImageMiss {
        VAddr address = 0;
        u32 size = 0;
        u64 generation = ~0ULL;
    };
    std::array<ImageMiss, 256> image_miss_cache{};
    Vulkan::Runtime& runtime;
    Vulkan::StagingBufferPool& staging_pool;
    AmdGpu::Liverpool* liverpool;
    Core::MemoryManager* memory;
    TextureCache& texture_cache;
    std::unique_ptr<MemoryTracker> memory_tracker;

    StreamBuffer stream_buffer;
    Buffer gds_buffer;
    RangeSet gpu_modified_ranges;

    std::unique_ptr<FaultManager> fault_manager;
    std::unique_ptr<Buffer> bda_pagetable_buffer;

    std::array<const Buffer*, MAX_ARENA_PAGES> address_space{};
    u64 arena_page_bits{MAX_ARENA_PAGE_BITS};
    u64 arena_page_size{u64{1} << MAX_ARENA_PAGE_BITS};
    u64 num_arena_pages{u64{1} << (ADDRESS_SPACE_BITS - MAX_ARENA_PAGE_BITS)};
    std::deque<Buffer> arenas;
    std::vector<ArenaBinds> pending_binds;
    Vulkan::Semaphore memory_semaphore;

    struct Backing : public Interval {
        vk::DeviceMemory memory;
        u64 offset;
        /// Host mapping of the whole allocation (host visible arena memory), or null.
        u8* base{};
        constexpr bool CanMergeWith(const Backing& other) const noexcept {
            return memory == other.memory && base == other.base && offset + (end - start) == other.offset;
        }
        constexpr Backing SubRange(u64 a, u64 b) const noexcept {
            return {{a, b}, memory, offset + (a - start), base};
        }
    };
    IntervalList<Backing> resident_ranges;

    u32 arena_memory_type_index{};
    /// System memory type used when video memory is full, so a small card keeps playing slowly
    /// instead of ending. ~0u when the driver offers none.
    u32 spill_memory_type_index{~0u};
    /// One zeroed block, bound in the page next to a buffer's last one. A shader that reads one
    /// block past the end then sees zeros instead of faulting the device, which is what AMD does
    /// to an unbound page. Only used when the driver supports aliased sparse binding.
    vk::DeviceMemory guard_memory{};
    u32 block_size{};
    u32 block_shift{};
    u32 blocks_per_arena_page{};
    u32 blocks_per_arena_page_shift{};

    /// The arena memory can be mapped by the CPU (resizable BAR, or memory shared with the CPU): finished GPU writes
    /// are then read straight from the mapping instead of being copied down by the GPU first.
    bool arena_host_visible{false};
    /// bbport: the memory types the arena buffer may use (diagnostic for direct guest reads).
    u32 arena_memory_type_bits{};
    std::atomic<bool> direct_readback{true};
    std::atomic<u32> direct_reads{0};
    std::array<u64, 4096> window_write_tick{};
    /// bbport: BB_READBACK_LOG: per window (hashed like window_write_tick) the last writer's tag, when it was
    /// recorded (steady ns), and how many write bindings since the window was last read back.
    std::array<u64, 4096> window_writer{};
    std::array<s64, 4096> window_write_ns{};
    std::array<u32, 4096> window_writes{};
    void StampGpuWrite(VAddr device_addr, u64 size);

    /// bbport: early readbacks (RecordEarlyReadbacks). Hot: the window was read back in the last ticks.
    struct EarlyReadback {
        VAddr window{};
        DownloadCopies copies{}; ///< srcOffset from the arena, dstOffset into staging.
        Vulkan::StagingBufferRef staging{};
        VAddr arena_base{};
        u64 write_seq{};
        u64 tick{};
    };
    bool early_readback{false};
    std::array<VAddr, 4096> hot_window{};
    std::array<u64, 4096> hot_tick{};
    std::vector<VAddr> early_pending;
    /// bbport: how many windows after a hot one get an early copy too (BB_READBACK_PREFETCH).
    u32 prefetch_windows{};
    /// bbport: windows stamped by bindings whose draw/dispatch is not recorded yet. A submission made
    /// inside the binding (an allocation-induced flush) must not take their early copy: the draw writes after it,
    /// in the next tick, without a new write sequence. They stay pending until NoteBindingsRecorded.
    std::vector<VAddr> unrecorded_windows;
    /// Set while unrecorded_windows is not empty: read by guest threads (TryFastWindowRead).
    std::atomic<bool> any_unrecorded{false};
    bool early_binding_guard = true;
    std::vector<EarlyReadback> early_readbacks;
    void DropEarlyReadback(VAddr window);

    /// bbport: BB_READBACK_WINDOW_HOSTVISIBLE: back the guest ranges the CPU reads every frame (the
    /// readback windows) with non-sparse, device-local, host-visible memory, so a guest read copies
    /// from the CPU mapping instead of waiting for a GPU copy. Only the windows that are actually
    /// read are moved (a few MiB), not the arenas.
    struct HostChunk {
        vk::DeviceMemory memory;
        u8* base;
        u64 size;
        u64 used;
    };
    std::vector<HostChunk> window_chunks;
    /// Windows backed by host-visible memory (GPU command thread only).
    std::unordered_set<VAddr> promoted_windows;
    /// Windows whose promotion was refused (uncommitted backing, no room): never retried.
    std::unordered_set<VAddr> unpromotable_windows;
    /// BB_READBACK_HOSTVISIBLE_AFTER: how many reads of a window make it worth a copy (default 2).
    u32 window_promote_after{2};
    bool window_host_visible{true};
    u32 window_memory_type_index{~0u};
    u32 window_promote_failures{};
    u32 window_promotions{};
    /// Reads of each window, and 1 while a promotion is queued. Guarded by readback_mutex.
    std::unordered_map<VAddr, u32> window_reads;
    std::unordered_set<VAddr> window_promote_queued;
    void MaybePromoteWindow(VAddr window);
    void PromoteWindow(VAddr window);
    void WriteBdaEntries(const Buffer* arena, std::span<const u64> blocks);
    bool IsPromotedWindow(VAddr window) const {
        return promoted_windows.contains(window);
    }

    /// bbport: a readback window in host visible memory. GPU writes into it are recorded with the
    /// submission tick that will make them visible, so a guest read fault can deliver them on the
    /// faulting thread: no GPU command thread round trip and no GPU copy.
    struct WindowWrite {
        VAddr addr;
        u64 size;
        u64 tick;
    };
    struct WindowState {
        VAddr window{};
        u8* base{};          ///< the host visible mapping of the window's memory
        u64 memory_offset{}; ///< the window's offset in the chunk mapping
        std::mutex mutex;
        std::vector<WindowWrite> writes;
        bool active{};
        bool overflow{};
    };
    static constexpr u32 WINDOW_SLOTS = 256;
    std::array<std::atomic<u64>, WINDOW_SLOTS> window_keys{};
    std::array<WindowState*, WINDOW_SLOTS> window_states{};
    std::array<std::unique_ptr<WindowState>, 64> window_state_pool{};
    std::atomic<u32> window_state_count{0};
    std::atomic<bool> any_promoted{false};
    WindowState* FindWindowState(VAddr window) const;
    WindowState* RegisterWindowState(VAddr window);
    void NoteWindowWrite(VAddr window, VAddr addr, u64 size, u64 tick);
public:
    /// bbport: serves a read of a promoted window from its mapping on the calling thread (the guest
    /// read fault handler calls this before draining anything). False: the normal readback path.
    bool TryFastWindowRead(VAddr addr, u64 size);
    /// bbport: downloads a window on a second queue of the graphics family, waiting only for the
    /// submission that wrote it (BB_READBACK_COPY_QUEUE). False: the normal readback path.
    bool TryCopyQueueReadback(VAddr addr, u64 size);
private:
    /// Command buffer, fence and host visible destination of TryCopyQueueReadback.
    struct CopyReadback;
    std::unique_ptr<CopyReadback> copy_readback;
    std::mutex copy_readback_mutex;
    CopyReadback& EnsureCopyReadback();
    /// bbport: waits until the draw recording thread has run its packets, so it cannot submit a draw
    /// that writes the old memory while a window is being moved (Rasterizer sets this).
    std::function<void()> drain_gpu_pipe;

    bool readback_offload{true};
    std::mutex readback_mutex;
    std::condition_variable readback_cv;
    /// Windows with a submitted download that the faulting thread has yet to finish.
    /// bbport: (address, size) of the copy each pending offloaded readback makes, so a job that
    /// covers a whole arena is visible to a fault anywhere in it instead of only in its 512 KB window.
    std::vector<std::pair<VAddr, u64>> pending_readbacks;
    /// Ranges of vetoed readbacks, merged back into gpu_modified_ranges on the GPU thread.
    std::vector<std::pair<VAddr, u64>> readback_returns;
};

} // namespace VideoCore
