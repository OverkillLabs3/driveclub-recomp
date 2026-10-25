// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <condition_variable>
#include <mutex>

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

    /// bbport: whether the GPU wrote (or a download is under way for) any byte of the range: a guest read of
    /// other bytes on the same protected pages needs no download (BB_FAULT_EMULATE). Any thread, lock-free.
    [[nodiscard]] bool IsRangeGpuWritten(VAddr addr, u64 size) const {
        return gpu_byte_map.Any(addr, size);
    }

    /// bbport: for a background reader (the stale-serving helper, page_manager.cpp): the GPU's bytes of a GPU
    /// modified range as of everything recorded so far, copied to staging and waited for here. Nothing is delivered
    /// to guest memory (the range stays GPU modified), so it never retries or falls back to a synchronous download.
    /// False when the range is not GPU modified (guest memory holds it). Any thread but the GPU threads.
    bool SnapshotGpuRange(VAddr addr, u64 size, void* out);
    /// bbport: BB_EXPOSURE_EXACT: records a copy of [addr, addr + size) now, after the work recorded before it,
    /// tagged with the generation the label written next carries and the registration's lifetime id (cache owner
    /// thread: at the EOP).
    bool RecordTaggedSnapshot(VAddr addr, u32 size, u32 tag, u64 id);
    /// Any thread: copies the snapshot of addr with that tag and id into out once the GPU has done it. False when
    /// there is none (*pending false) or the GPU has not done it yet (*pending true). ticks (optional) gets the
    /// snapshot's tick and the GPU tick known when it was checked. wait_ns: when the GPU has not done it but its
    /// submission has gone (tick below the current one), wait up to that long for it (ticks[2] gets the ns waited).
    bool ReadTaggedSnapshot(VAddr addr, u32 size, u32 tag, u64 id, void* out, bool* pending, u64* ticks,
                            u64 wait_ns = 0);
    /// The entry `match` finds and, in *done, whether the GPU has done its copy (is_done(entry.tick)); null when
    /// none. ReadTaggedSnapshot's lookup (before and after its wait), separate for the exact-readback self-test.
    template <typename Entries, typename Match, typename IsDone>
    static auto LookupSnapshot(Entries& entries, Match&& match, IsDone&& is_done, bool* done) {
        auto it = std::ranges::find_if(entries, match);
        *done = it != entries.end() && is_done(it->tick);
        return it == entries.end() ? nullptr : &*it;
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
        u64 offset; ///< into `memory`, in blocks like start and end
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
    std::atomic<bool> direct_readback{true};
    u32 direct_reads{};
    std::array<u64, 4096> window_write_tick{};
    /// bbport: the newest tick of a lazy (background, not flushed) readback copy; a caller that has to wait for
    /// pending readbacks flushes first when it is not submitted yet (it may be the one that would submit it).
    u64 lazy_readback_tick{};
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
    /// bbport: windows stamped by bindings whose draw/dispatch is not recorded yet. A submission made
    /// inside the binding (an allocation-induced flush) must not take their early copy: the draw writes after it,
    /// in the next tick, without a new write sequence. They stay pending until NoteBindingsRecorded.
    std::vector<VAddr> unrecorded_windows;
    bool early_binding_guard = true;
    std::vector<EarlyReadback> early_readbacks;
    /// bbport: RecordTaggedSnapshot. Written by the cache owner, read by the guest's threads: under tagged_mutex.
    struct TaggedSnapshot {
        VAddr addr{};
        u32 size{};
        u32 tag{};
        u64 id{};
        Vulkan::StagingBufferRef staging{};
        u64 tick{};
    };
    std::mutex tagged_mutex;
    std::vector<TaggedSnapshot> tagged_snapshots;
    void DropEarlyReadback(VAddr window);

    bool readback_offload{true};
    std::mutex readback_mutex;
    std::condition_variable readback_cv;
    /// Windows with a submitted download that the faulting thread has yet to finish.
    std::vector<VAddr> pending_readbacks;
    /// Ranges of vetoed readbacks, merged back into gpu_modified_ranges on the GPU thread.
    std::vector<std::pair<VAddr, u64>> readback_returns;
};

} // namespace VideoCore
