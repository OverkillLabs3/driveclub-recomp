// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <chrono>
#include <cstdio>
#include <functional>
#include <mutex>
#include <thread>
#include <array>
#include <map>
#include <unordered_map>
#include <algorithm>
#include <bit>
#include <cstdlib>
#include <cstring>
#include <string>
#include <magic_enum/magic_enum.hpp>
#include "bbport_copy.h"
#include "bbport_settings.h"
#include "bbport_toggles.h"
#include "bbport_frametrace.h"
#include "bbport_shared_log.h"
extern "C" int runtime_memory_read_backing(uintptr_t address, void* data, uint64_t size);
extern "C" int runtime_memory_write_backing(uintptr_t address, const void* data, uint64_t size);
#include "common/alignment.h"
#include "core/memory.h"
#include "video_core/amdgpu/liverpool.h"
#include "video_core/buffer_cache/buffer.h"
#include "video_core/buffer_cache/buffer_cache.h"
#include "video_core/buffer_cache/memory_tracker.h"
#include "video_core/renderer_vulkan/vk_graphics_pipeline.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_runtime.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_draw_pipe.h"
#include "video_core/texture_cache/texture_cache.h"

#include <vk_mem_alloc.h>

extern "C" void bbgpu_print_recent_passes(void);
extern "C" void bbgpu_note_device_lost(void);

namespace VideoCore {

static constexpr size_t GDS_BUFFER_SIZE = 64_KB;
static constexpr size_t STREAM_BUFFER_SIZE = 128_MB;

static constexpr auto ARENA_USAGE =
    vk::BufferUsageFlagBits::eTransferSrc | vk::BufferUsageFlagBits::eTransferDst |
    vk::BufferUsageFlagBits::eUniformBuffer | vk::BufferUsageFlagBits::eStorageBuffer |
    vk::BufferUsageFlagBits::eIndexBuffer | vk::BufferUsageFlagBits::eVertexBuffer |
    vk::BufferUsageFlagBits::eIndirectBuffer | vk::BufferUsageFlagBits::eShaderDeviceAddress;

std::optional<u32> FindMemoryType(const vk::PhysicalDeviceMemoryProperties& properties,
                                  vk::MemoryPropertyFlags wanted, u32 memory_type_bits) {
    for (u32 i = 0; i < properties.memoryTypeCount; ++i) {
        if (((memory_type_bits >> i) & 1) == 0) {
            continue;
        }
        const auto flags = properties.memoryTypes[i].propertyFlags;
        if ((flags & wanted) == wanted) {
            return i;
        }
    }
    return std::nullopt;
}

// Memory for the arenas that the CPU can map, chosen from what the driver reports: memory that is both device local
// and host visible. On a discrete card that only exists with resizable BAR (and then its heap is the card's whole
// video memory); a small BAR window of a few hundred MiB is left alone. Cards that share memory with the CPU always
// have it. BB_ARENA_MEMORY: auto (default), device (never), hostvisible (any such type).
static std::optional<u32> FindHostVisibleArenaType(const Vulkan::Instance& instance, u32 memory_type_bits) {
    using Flags = vk::MemoryPropertyFlagBits;
    const char* env = std::getenv("BB_ARENA_MEMORY");
    const std::string mode = env ? env : "auto";
    if (mode == "device") {
        return std::nullopt;
    }
    const auto& props = instance.GetMemoryProperties();
    const auto type = FindMemoryType(props, Flags::eDeviceLocal | Flags::eHostVisible | Flags::eHostCoherent,
                                     memory_type_bits);
    if (!type || mode == "hostvisible" || instance.IsIntegrated()) {
        return type;
    }
    u64 largest_local_heap = 0;
    for (u32 i = 0; i < props.memoryHeapCount; ++i) {
        if ((props.memoryHeaps[i].flags & vk::MemoryHeapFlagBits::eDeviceLocal) != vk::MemoryHeapFlags{}) {
            largest_local_heap = std::max<u64>(largest_local_heap, props.memoryHeaps[i].size);
        }
    }
    const u64 heap_size = props.memoryHeaps[props.memoryTypes[*type].heapIndex].size;
    if (heap_size >= 1_GB && heap_size == largest_local_heap) {
        return type;
    }
    return std::nullopt;
}

static BufferCache* g_bb_cache = nullptr;
/// Debug: the mapping of the window chunk (BB_WINDOW_BIND_DEBUG).
static u8* g_window_chunk_base = nullptr;

/// Command buffer, fence and host visible destination of TryCopyQueueReadback.
struct BufferCache::CopyReadback {
    vk::UniqueCommandPool pool;
    vk::CommandBuffer cmdbuf{};
    vk::UniqueFence fence;
    vk::UniqueBuffer staging;
    vk::UniqueDeviceMemory memory;
    u8* mapped{};
    bool ready{};
};
/// bbport: whether a submission writing a host visible readback window ends with a host read barrier.
/// Type 4 is host coherent, so the timeline wait may be enough (BB_WINDOW_BARRIER=0 tests that).
static bool WindowBarrierWanted() {
    static const bool wanted = [] {
        const char* env = std::getenv("BB_WINDOW_BARRIER");
        return !(env && env[0] == '0');
    }();
    return wanted;
}

BufferCache::BufferCache(const Vulkan::Instance& instance_, Vulkan::Scheduler& scheduler_,
                         Vulkan::Runtime& runtime_, AmdGpu::Liverpool* liverpool_,
                         TextureCache& texture_cache_, PageManager& tracker)
    : instance{instance_}, scheduler{scheduler_}, runtime{runtime_},
      staging_pool{runtime_.GetStagingPool()}, liverpool{liverpool_},
      memory{Core::Memory::Instance()}, texture_cache{texture_cache_},
      memory_tracker{std::make_unique<MemoryTracker>(tracker)},
      stream_buffer{instance, scheduler, MemoryType::Stream, STREAM_BUFFER_SIZE},
      gds_buffer{instance, 0, GDS_BUFFER_SIZE, MemoryType::Stream, "GDS Buffer"},
      memory_semaphore{instance} {
    while (arena_page_bits > MIN_ARENA_PAGE_BITS &&
           (u64{2} << arena_page_bits) > instance.GetMaxBufferSize()) {
        --arena_page_bits;
    }
    arena_page_size = u64{1} << arena_page_bits;
    num_arena_pages = u64{1} << (ADDRESS_SPACE_BITS - arena_page_bits);
    if (arena_page_bits != MAX_ARENA_PAGE_BITS) {
        LOG_INFO(Render_Vulkan, "Buffer arenas of {} MiB (driver buffer limit {} MiB)",
                 arena_page_size >> 20, instance.GetMaxBufferSize() >> 20);
    }
    const vk::BufferCreateInfo probe_ci = {
        .flags =
            vk::BufferCreateFlagBits::eSparseBinding | vk::BufferCreateFlagBits::eSparseResidency,
        .size = arena_page_size,
        .usage = ARENA_USAGE,
        .sharingMode = vk::SharingMode::eExclusive,
    };
    const vk::DeviceBufferMemoryRequirements req_info = {
        .pCreateInfo = &probe_ci,
    };
    const auto device = instance.GetDevice();
    const auto reqs = device.getBufferMemoryRequirements(req_info).memoryRequirements;
    block_size = Common::AlignUp(std::max<u64>(reqs.alignment, MIN_BLOCK_SIZE), reqs.alignment);
    ASSERT_MSG(std::popcount(block_size) == 1, "Sparse block size {} is not a power of 2",
               block_size);
    block_shift = std::bit_width(block_size) - 1;
    blocks_per_arena_page = arena_page_size / block_size;
    blocks_per_arena_page_shift = arena_page_bits - block_shift;
    arena_memory_type_index =
        FindMemoryType(instance.GetMemoryProperties(), vk::MemoryPropertyFlagBits::eDeviceLocal,
                       reqs.memoryTypeBits)
            .value();
    {
        // Type for buffer data when the card's own memory is full: system memory, which the GPU reads
        // over PCIe. Slower, but a full card should not end the game.
        const auto& props = instance.GetMemoryProperties();
        for (u32 i = 0; i < props.memoryTypeCount && spill_memory_type_index == ~0u; ++i) {
            if (((reqs.memoryTypeBits >> i) & 1) == 0) {
                continue;
            }
            const auto flags = props.memoryTypes[i].propertyFlags;
            if ((flags & (vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent)) !=
                (vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent)) {
                continue;
            }
            if ((props.memoryHeaps[props.memoryTypes[i].heapIndex].flags &
                 vk::MemoryHeapFlagBits::eDeviceLocal) != vk::MemoryHeapFlags{}) {
                continue;
            }
            spill_memory_type_index = i;
        }
    }
    {
        const char* direct = std::getenv("BB_READBACK_DIRECT");
        // Reading GPU results straight from host visible memory (resizable BAR) is untested on AMD cards, which
        // show device losses and broken frames with it: off there unless BB_READBACK_DIRECT=1 asks for it.
        const bool amd = instance.GetVendorID() == 0x1002;
        direct_readback = direct ? direct[0] != '0' : !amd;
        arena_memory_type_bits = reqs.memoryTypeBits;
    if (const auto host_type = FindHostVisibleArenaType(instance, reqs.memoryTypeBits); host_type && direct_readback) {
            arena_memory_type_index = *host_type;
            arena_host_visible = true;
        }
        std::printf("GPU: buffer arena memory type %u, host visible (direct guest reads): %s\n", arena_memory_type_index,
                    arena_host_visible ? "yes" : "no");
        // Which memory types the arena's own requirements allow, and what each offers: the reason a
        // direct-read arena is refused is only visible with both halves of that answer.
        {
            std::string allowed;
            for (u32 t = 0; t < instance.GetMemoryProperties().memoryTypeCount; ++t) {
                if ((arena_memory_type_bits >> t) & 1) {
                    allowed += (allowed.empty() ? "" : ",") + std::to_string(t);
                }
            }
            std::printf("GPU: arena memory type bits %#x allow types: %s\n",
                        arena_memory_type_bits, allowed.empty() ? "none" : allowed.c_str());
        }
        // bbport: copies of hot readback windows at the end of the submission that wrote them (BB_READBACK_EARLY=0: off).
        const char* early = std::getenv("BB_READBACK_EARLY");
        early_readback = !(early && early[0] == '0') && !arena_host_visible;
        std::printf("GPU: early readback copies at submission end: %s\n", early_readback ? "on" : "off");
        // bbport: BB_READBACK_WINDOW_HOSTVISIBLE=1: back only the guest ranges the CPU reads every
        // frame (the readback windows) with non-sparse, device-local, host-visible memory. A guest
        // read then copies from that mapping instead of waiting for a GPU copy. A sparse buffer's
        // requirement list only names the device-local type, but the driver accepts a sparse bind
        // to the CPU visible type of the same heap (probed), which is what this uses.
        // Off by default: measured 4K DLAA 61.6 FPS / sd 4.9 against 63.4 / 4.4 for the copy path,
        // because the GPU writes and reads those windows through BAR memory and reads of the
        // mapping run at ~70 MB/s. The option stays for the record and for other memory types.
        const char* windows_env = std::getenv("BB_READBACK_WINDOW_HOSTVISIBLE");
        // Only the DLAA mode (DLSS upscaler at native scale) has the GPU this close to the limit;
        // other modes have headroom and must not pay for the placement (BB_AA=dlss BB_AA_SCALE=native).
        const auto& bb = BbSettings::Get();
        const bool dlaa = bb.aa_mode.load(std::memory_order_relaxed) == BbSettings::AaDlss &&
                          bb.aa_scale.load(std::memory_order_relaxed) == BbSettings::AaScaleNative;
        window_host_visible = windows_env && windows_env[0] != '0' && !arena_host_visible &&
                              direct_readback.load(std::memory_order_relaxed) && dlaa;
        if (const char* after = std::getenv("BB_READBACK_HOSTVISIBLE_AFTER")) {
            window_promote_after = std::clamp<u32>(u32(std::atoi(after)), 1, 1000);
        } else {
            window_promote_after = 4;
        }
        if (window_host_visible) {
            const auto& props = instance.GetMemoryProperties();
            const auto type = FindMemoryType(
                props,
                vk::MemoryPropertyFlagBits::eDeviceLocal | vk::MemoryPropertyFlagBits::eHostVisible |
                    vk::MemoryPropertyFlagBits::eHostCoherent,
                ~0u);
            if (type && props.memoryTypes[*type].heapIndex ==
                            props.memoryTypes[arena_memory_type_index].heapIndex) {
                window_memory_type_index = *type;
            } else {
                window_host_visible = false;
            }
        }
        std::printf("GPU: readback windows in host visible memory: %s (memory type %d, after %u reads)\n",
                    window_host_visible ? "yes" : "no", window_host_visible ? int(window_memory_type_index) : -1,
                    window_promote_after);
        // bbport: BB_EARLY_BINDING_GUARD=0: early copies may be taken by a submission made inside a binding.
        prefetch_windows = [] {
        const char* value = std::getenv("BB_READBACK_PREFETCH");
        return value ? std::clamp<u32>(u32(std::atoi(value)), 0, 8) : 0u;
    }();
    const char* guard = std::getenv("BB_EARLY_BINDING_GUARD");
        early_binding_guard = !(guard && guard[0] == '0');
        // What the driver offers, to see whether resizable BAR is on: a CPU visible type in the card's own memory
        // that is as large as the card's memory means it is.
        const auto& mem = instance.GetMemoryProperties();
        for (u32 h = 0; h < mem.memoryHeapCount; ++h) {
            const bool local =
                (mem.memoryHeaps[h].flags & vk::MemoryHeapFlagBits::eDeviceLocal) != vk::MemoryHeapFlags{};
            std::string visible_types;
            for (u32 t = 0; t < mem.memoryTypeCount; ++t) {
                const auto f = mem.memoryTypes[t].propertyFlags;
                if (mem.memoryTypes[t].heapIndex == h && (f & vk::MemoryPropertyFlagBits::eHostVisible)) {
                    visible_types += (visible_types.empty() ? "" : ",") + std::to_string(t) +
                                     ((f & vk::MemoryPropertyFlagBits::eHostCoherent) ? "c" : "");
                }
            }
            std::printf("GPU: memory heap %u: %llu MiB%s, CPU visible types: %s\n", h,
                        static_cast<unsigned long long>(mem.memoryHeaps[h].size >> 20), local ? ", device local" : "",
                        visible_types.empty() ? "none" : visible_types.c_str());
        }
    }

    const u64 bda_pagetable_size =
        (blocks_per_arena_page * num_arena_pages) * sizeof(vk::DeviceAddress);
    fault_manager = std::make_unique<FaultManager>(instance, scheduler, *this, block_shift,
                                                   blocks_per_arena_page * num_arena_pages);
    bda_pagetable_buffer = std::make_unique<Buffer>(
        instance, 0, bda_pagetable_size, MemoryType::DeviceLocal, "BDA Page Table Buffer");
    runtime.FillBuffer(bda_pagetable_buffer.get(), 0u, bda_pagetable_size, 0u);

    Vulkan::g_bb_describe_fault = [](u64 address) -> std::string {
        return g_bb_cache ? g_bb_cache->DescribeFault(address) : std::string{};
    };
    g_bb_cache = this;

    // The zeroed guard block, on the drivers that support aliasing one memory over many ranges (AMD
    // by default in this port). AMD faults a shader that reads an unbound page; NVIDIA answers zero.
    if (sparse_aliased_enabled) {
        const auto guard_type = FindMemoryType(
            instance.GetMemoryProperties(),
            vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent,
            reqs.memoryTypeBits);
        if (guard_type) {
            const auto alloc = instance.GetDevice().allocateMemory(
                {.allocationSize = block_size, .memoryTypeIndex = *guard_type});
            if (alloc.result == vk::Result::eSuccess) {
                guard_memory = alloc.value;
                const auto map = instance.GetDevice().mapMemory(guard_memory, 0, block_size);
                if (map.result == vk::Result::eSuccess) {
                    std::memset(map.value, 0, block_size);
                    instance.GetDevice().unmapMemory(guard_memory);
                }
            }
        }
        std::printf("GPU: guard page for buffers that are read past the end: %s\n",
                    guard_memory ? "on" : "off");
    }
}

BufferCache::~BufferCache() {
    g_bb_cache = nullptr;
    for (auto& chunk : window_chunks) {
        if (chunk.base) {
            instance.GetDevice().unmapMemory(chunk.memory);
        }
        instance.GetDevice().freeMemory(chunk.memory);
    }
    if (guard_memory) {
        instance.GetDevice().freeMemory(guard_memory);
    }
}

void BufferCache::InvalidateMemory(VAddr device_addr, u64 size, bool assume_locks) {
    memory_tracker->InvalidateRegion(device_addr, size, [this, device_addr, size, assume_locks] {
        ReadMemory(device_addr, size, true, assume_locks);
    });
}

// bbport: the game fills its per-frame buffers (constants, skinning output) sequentially and
// every 4 KiB page cost a protection fault (~110k/s in Hunter's Nightmare, a fifth of each
// render worker's time in the kernel). A fault unprotects the aligned window around it instead;
// pages marked CPU-modified without being written only cost an upload when bound.
// Item: context = BufferCache, source = guest address, destination = host pointer, size,
// extra = the Buffer whose mapping holds the destination (flushed after the copy).
void BufferCache::RunGuestCopy(const BbCopy::Item& item) {
    auto* cache = static_cast<BufferCache*>(item.context);
    auto* dst = reinterpret_cast<u8*>(item.destination);
    cache->memory->CopySparseMemory(item.source, dst, item.size);
    auto* buffer = reinterpret_cast<Buffer*>(item.extra);
    buffer->Flush(dst - buffer->mapped_data.data(), item.size);
}

// Small guest copies run on the recording thread (it spins for work: no wakeup, and it is
// idle most of the time); PoolSmallCopies (toggle 524288) batches them for the copy threads.
void BufferCache::SmallGuestCopy(const BbCopy::Item& item) {
    if (scheduler.IsRecordingDeferred() && !BbToggle::Disabled(BbToggle::PoolSmallCopies)) {
        scheduler.RecordHostCopy([item] { item.run(item); });
        return;
    }
    BbCopy::QueueCopy(item);
}

void BufferCache::ExtendWriteFault(VAddr device_addr) {
    static const u64 window = [] {
        const char* env = std::getenv("BB_FAULT_WINDOW");
        const u64 kib = env ? std::strtoull(env, nullptr, 10) : 256;
        return std::bit_ceil(std::clamp<u64>(kib, 4, 1024)) * 1024;
    }();
    if (window <= TRACKER_BYTES_PER_PAGE || BbToggle::Disabled(BbToggle::FaultWindow)) {
        return;
    }
    memory_tracker->ExtendWriteFault(Common::AlignDown(device_addr, window), window);
}

static constexpr u64 READBACK_WINDOW_SIZE = 512_KB;

namespace {
FILE* ReadbackLog();
std::mutex g_readback_log_mutex;
unsigned long LogThreadId();
} // namespace

void BufferCache::ReadMemory(VAddr device_addr, u64 size, bool is_write, bool assume_locks) {
    // The faulting thread waits for the download instead of the GPU command thread: that thread
    // keeps recording the frame while the copy runs.
    static const bool offload_enabled = [] {
        const char* env = std::getenv("BB_READBACK_OFFLOAD");
        return !(env && env[0] == '0');
    }();
    if (assume_locks) {
        BbStats::readback_paths[3].fetch_add(1, std::memory_order_relaxed);
    } else if (!offload_enabled || !readback_offload) {
        BbStats::readback_paths[4].fetch_add(1, std::memory_order_relaxed);
    } else {
        // A request over several windows is read window by window: each is an offloaded copy
        // of its own instead of one synchronous download of the whole arena.
        bool all_offloaded = true;
        const VAddr end = device_addr + size;
        for (VAddr at = device_addr; at < end && all_offloaded;) {
            const VAddr window_end = Common::AlignDown(at, READBACK_WINDOW_SIZE) + READBACK_WINDOW_SIZE;
            const VAddr part_end = std::min<VAddr>(end, window_end);
            all_offloaded = OffloadReadback(at, part_end - at, is_write);
            at = part_end;
        }
        if (all_offloaded) {
            BbStats::readback_paths[0].fetch_add(1, std::memory_order_relaxed);
            return;
        }
        BbStats::readback_paths[2].fetch_add(1, std::memory_order_relaxed);
    }
    const auto flush_request = [this, device_addr, size, is_write] {
        const u32 first_block = device_addr >> block_shift;
        const u32 last_block = (device_addr + size - 1) >> block_shift;
        const auto* arena = GetArena(first_block, last_block);

        // GPU-modified ranges come as many small scattered islands,
        // so the download is widened to a window around the request
        constexpr u64 WindowSize = READBACK_WINDOW_SIZE;
        const VAddr arena_end = arena->cpu_addr + arena->size_bytes;
        // Every sync with the GPU costs a full frame of waiting, so one read downloads all the
        // GPU-modified data of the arena at once instead of one window per fault.
        static const bool whole_arena = [] {
            const char* env = std::getenv("BB_READBACK_WINDOW");
            return !(env && env[0] == '1');
        }();
        const VAddr window_start =
            whole_arena ? arena->cpu_addr
                        : std::max<VAddr>(Common::AlignDown(device_addr, WindowSize), arena->cpu_addr);
        const VAddr window_end =
            whole_arena ? arena_end
                        : std::min<VAddr>(std::max<VAddr>(window_start + WindowSize, device_addr + size),
                                          arena_end);
        DownloadMemory(arena, window_start, window_end - window_start);
        if (is_write) {
            memory_tracker->MarkRegionAsCpuModified(device_addr, size);
        }
    };
    if (assume_locks) {
        flush_request();
    } else if (FILE* log = ReadbackLog()) {
        // bbport: BB_READBACK_LOG: the synchronous download (the GPU command thread waits for the GPU).
        const s64 t_start = BbTrace::NowNs();
        liverpool->SendCommand<true>(std::move(flush_request));
        const s64 t_done = BbTrace::NowNs();
        std::scoped_lock lk{g_readback_log_mutex};
        std::fprintf(log, "%.3f %u %lu %llx %llx s %d 0 0 %.0f %llu 0 0 0 0 0 -1 0\n", t_start / 1e6,
                     BbTrace::frame_gen.load(std::memory_order_relaxed), LogThreadId(),
                     (unsigned long long)BbTrace::fault_rip, (unsigned long long)device_addr, int(is_write),
                     (t_done - t_start) / 1e3, (unsigned long long)size);
    } else {
        liverpool->SendCommand<true>(std::move(flush_request));
    }
}

u64 BufferCache::CollectDownloads(const Buffer* arena, VAddr device_addr, u64 size,
                                  DownloadCopies& copies) {
    u64 total_size_bytes = 0;
    const VAddr arena_base = arena->cpu_addr;
    memory_tracker->ForEachDownloadRange<false>(device_addr, size, [&](u64 address, u64 size) {
        const auto add_download = [&](VAddr start, VAddr end) {
            const u64 new_offset = start - arena_base;
            const u64 new_size = end - start;
            copies.push_back(vk::BufferCopy{
                .srcOffset = new_offset,
                .dstOffset = total_size_bytes,
                .size = new_size,
            });
            // Align up to avoid cache conflicts
            constexpr u64 align = 64ULL;
            constexpr u64 mask = ~(align - 1ULL);
            total_size_bytes += (new_size + align - 1) & mask;
        };
        gpu_modified_ranges.ForEachInRange(address, size, add_download);
        gpu_modified_ranges.Subtract(address, size);
    });
    return total_size_bytes;
}

void BufferCache::DownloadMemory(const Buffer* arena, VAddr device_addr, u64 size) {
    {
        // A pending offloaded readback owns the GPU modified pages of its window until it finishes.
        std::unique_lock lk{readback_mutex};
        readback_cv.wait(lk, [&] { return !IsReadbackPending(device_addr, size); });
        MergeReadbackReturns();
    }
    if (!early_readbacks.empty()) {
        for (VAddr w = Common::AlignDown(device_addr, READBACK_WINDOW_SIZE); w < device_addr + size;
             w += READBACK_WINDOW_SIZE) {
            DropEarlyReadback(w);
        }
    }
    ApplyQueuedStores();
    DownloadCopies copies;
    const VAddr arena_base = arena->cpu_addr;
    const u64 total_size_bytes = CollectDownloads(arena, device_addr, size, copies);
    if (total_size_bytes == 0) {
        return;
    }
    const auto download = staging_pool.Request(total_size_bytes, VideoCore::MemoryType::HostCached);
    for (auto& copy : copies) {
        copy.dstOffset += download.offset;
    }
    runtime.CopyBuffer(arena, download.buffer, copies);
    scheduler.Finish(1); // "buffer watch" bucket of BB_FRAME_STATS tick waits: synchronous guest downloads

    download.buffer->Invalidate(download.offset, download.size);
    for (const auto& copy : copies) {
        auto* dst_addr = std::bit_cast<u8*>(arena_base + copy.srcOffset);
        memory->TryWriteBacking(dst_addr, download.mapped + (copy.dstOffset - download.offset),
                                copy.size);
    }
    memory_tracker->UnmarkRegionAsGpuModified(device_addr, size);
    for (const auto& copy : copies) {
        gpu_byte_map.Clear(arena_base + copy.srcOffset, copy.size); // delivered to guest memory
    }
}

namespace {
// bbport: BB_READBACK_LOG=<file>: one line per guest read that downloads GPU-written data (see the header line).
FILE* ReadbackLog() {
    static FILE* file = [] {
        const char* path = std::getenv("BB_READBACK_LOG");
        FILE* f = path && path[0] ? std::fopen(path, "w") : nullptr;
        if (f) {
            std::setvbuf(f, nullptr, _IOFBF, 1 << 16);
            std::fprintf(f, "# t_ms frame tid rip addr path write pickup_us data_us copy_us bytes ncopies stamped_tick "
                            "open_tick gpu_tick writer writer_age_us writes fault_range_lo fault_range_hi page_gpu_bytes nearest_gpu_byte\n");
        }
        return f;
    }();
    return file;
}
unsigned long LogThreadId() {
    static thread_local const unsigned long tid =
        static_cast<unsigned long>(std::hash<std::thread::id>{}(std::this_thread::get_id()) & 0xffffff);
    return tid;
}
} // namespace

// The tick that completes the last GPU write into each readback window (hashed): a direct read waits for it.
void BufferCache::StampGpuWrite(VAddr device_addr, u64 size) {
    // bbport: BB_SHARED_LOG: each distinct GPU write range in the game's SystemShared heap, once.
    static const bool shared_log = BbSharedLog::Enabled();
    if (shared_log && size != 0) {
        BbSharedLog::GpuWrite(device_addr, size, BbTrace::writer_tag.load(std::memory_order_relaxed));
    }
    static const bool log = ReadbackLog() != nullptr;
    if ((!arena_host_visible && !window_host_visible && !log && !early_readback) || size == 0) {
        return;
    }
    const u64 tick = scheduler.CurrentTick();
    if (early_readback) {
        for (u64 w = device_addr / READBACK_WINDOW_SIZE; w <= (device_addr + size - 1) / READBACK_WINDOW_SIZE; ++w) {
            const VAddr window = w * READBACK_WINDOW_SIZE;
            // Hot: read back lately. Promoted windows are read straight from memory, no copy is needed.
            if (hot_window[w & 4095] == window && tick - hot_tick[w & 4095] < 64 &&
                !IsPromotedWindow(window)) {
                if (std::ranges::find(early_pending, window) == early_pending.end()) {
                    early_pending.push_back(window);
                }
                // bbport: the guest walks long lists as the car moves, so the window it will read
                // next is usually the one after the last one it read: copying the neighbours at
                // this submission's end turns their reads from a round trip into a memcpy.
                // BB_READBACK_PREFETCH=<windows> (default 0: off until it is measured).
                for (u32 ahead = 1; ahead <= prefetch_windows; ++ahead) {
                    const VAddr next = window + u64(ahead) * READBACK_WINDOW_SIZE;
                    if (std::ranges::find(early_pending, next) == early_pending.end()) {
                        early_pending.push_back(next);
                    }
                }
            }
            if (early_binding_guard && std::ranges::find(unrecorded_windows, window) == unrecorded_windows.end()) {
                unrecorded_windows.push_back(window);
                any_unrecorded.store(true, std::memory_order_relaxed);
            }
        }
    } else if (window_host_visible && early_binding_guard) {
        // The fast guest read of a promoted window waits for the tick stamped here, so it must know
        // that the draw carrying this write is recorded (and will get that tick).
        for (u64 w = device_addr / READBACK_WINDOW_SIZE; w <= (device_addr + size - 1) / READBACK_WINDOW_SIZE; ++w) {
            const VAddr window = w * READBACK_WINDOW_SIZE;
            if (!IsPromotedWindow(window) ||
                std::ranges::find(unrecorded_windows, window) != unrecorded_windows.end()) {
                continue;
            }
            unrecorded_windows.push_back(window);
            any_unrecorded.store(true, std::memory_order_relaxed);
        }
    }
    const u64 tag = log ? BbTrace::writer_tag.load(std::memory_order_relaxed) : 0;
    const s64 now = log ? BbTrace::NowNs() : 0;
    for (u64 w = device_addr / READBACK_WINDOW_SIZE; w <= (device_addr + size - 1) / READBACK_WINDOW_SIZE; ++w) {
        window_write_tick[w & 4095] = tick;
        if (any_promoted.load(std::memory_order_relaxed)) {
            const VAddr window = w * READBACK_WINDOW_SIZE;
            const VAddr lo = std::max<VAddr>(device_addr, window);
            const VAddr hi = std::min<VAddr>(device_addr + size, window + READBACK_WINDOW_SIZE);
            NoteWindowWrite(window, lo, hi - lo, tick);
        }
        if (log) {
            window_writer[w & 4095] = tag;
            window_write_ns[w & 4095] = now;
            ++window_writes[w & 4095];
        }
    }
    // bbport: type 4 is host coherent, so the timeline wait may be enough; BB_WINDOW_BARRIER=0 tests it.
    if (arena_host_visible || (!promoted_windows.empty() && WindowBarrierWanted())) {
        scheduler.RequestHostReadBarrier();
    }
}

struct DirectSpan {
    const u8* src;
    VAddr dst;
    u64 size;
};

struct BufferCache::Readback {
    VAddr window{};
    /// BB_READBACK_LOG: the faulting address; the downloaded GPU-modified range holding it (0, 0: none).
    VAddr fault{}, fault_lo{}, fault_hi{};
    /// bbport: the range this job copies and unmarks. The whole arena by default: a copy costs a
    /// full round trip whatever its size (measured 1.4-3.3 ms for 152 bytes and for 162 KB alike),
    /// and the guest faults 4-6 times a frame, so one arena-wide copy per frame replaces 4-5 round
    /// trips (the synchronous download path has always drained the arena for this reason).
    VAddr copy_base{};
    u64 copy_size{};
    u64 page_gpu_bytes{}, nearest{~0ull};
    DownloadCopies copies{};
    Vulkan::StagingBufferRef staging{};
    VAddr arena_base{};
    u64 write_seq{};
    u64 tick{};
    bool busy{};
    /// Read from the arena mapping once the tick has passed; verify: the usual copy is made too and compared.
    bool direct{};
    bool verify{};
    std::vector<DirectSpan> spans;
    /// BB_READBACK_LOG: ticks when recorded (the window's last GPU write, the open one, the GPU's), its writer.
    u64 log_stamped{}, log_current{}, log_gpu{}, log_bytes{}, log_writer{};
    s64 log_write_ns{};
    u32 log_writes{};
    /// BB_READBACK_EARLY_VERIFY: the usual copy, made too and compared with the early one.
    Vulkan::StagingBufferRef early_verify{};
};

bool BufferCache::OffloadReadback(VAddr device_addr, u64 size, bool is_write) {
    const VAddr window = Common::AlignDown(device_addr, READBACK_WINDOW_SIZE);
    if (device_addr + size > window + READBACK_WINDOW_SIZE) {
        BbStats::readback_paths[1].fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    // A request still GPU modified after two rounds of its own takes the synchronous download,
    // which copies and unmarks in one GPU thread command.
    u32 attempts = 0;
    FILE* const log = ReadbackLog();
    const bool timed = log || BbTrace::enabled;
    while (attempts < 2) {
        Readback job{.window = window, .fault = device_addr};
        // bbport: where a read fault's time goes (BB_FRAME_STATS 'Readback waits', BB_READBACK_LOG).
        const s64 t_start = timed ? BbTrace::NowNs() : 0;
        liverpool->SendCommand<true>([this, &job] { RecordReadback(job); });
        const s64 t_recorded = timed ? BbTrace::NowNs() : 0;
        if (timed) {
            BbTrace::rb_pickup_ns.fetch_add(u64(t_recorded - t_start), std::memory_order_relaxed);
        }
        if (job.busy) {
            std::unique_lock lk{readback_mutex};
            readback_cv.wait(lk, [&] { return !IsReadbackPending(window, READBACK_WINDOW_SIZE); });
            if (timed) {
                BbTrace::rb_pending_ns.fetch_add(u64(BbTrace::NowNs() - t_recorded), std::memory_order_relaxed);
            }
        } else {
            ++attempts;
            if (!job.copies.empty()) {
                // Logged: first the submission that wrote the data, then the copy (recorded after everything
                // that was open when the fault came).
                s64 t_data = t_recorded;
                if (log && job.log_stamped && job.log_stamped < job.tick) {
                    scheduler.GetWorkSemaphore()->Wait(job.log_stamped);
                    t_data = BbTrace::NowNs();
                }
                scheduler.GetWorkSemaphore()->Wait(job.tick);
                if (timed) {
                    const s64 t_done = BbTrace::NowNs();
                    BbTrace::rb_gpu_ns.fetch_add(u64(t_done - t_recorded), std::memory_order_relaxed);
                    BbTrace::rb_jobs.fetch_add(1, std::memory_order_relaxed);
                    if (log) {
                        std::scoped_lock lk{g_readback_log_mutex};
                        std::fprintf(log, "%.3f %u %lu %llx %llx o %d %.0f %.0f %.0f %llu %zu %llu %llu %llu %016llx %.0f %u %llx %llx %llu %llu\n",
                                     t_start / 1e6, BbTrace::frame_gen.load(std::memory_order_relaxed), LogThreadId(),
                                     (unsigned long long)BbTrace::fault_rip, (unsigned long long)device_addr,
                                     int(is_write), (t_recorded - t_start) / 1e3, (t_data - t_recorded) / 1e3,
                                     (t_done - t_data) / 1e3, (unsigned long long)job.log_bytes, job.copies.size(),
                                     (unsigned long long)job.log_stamped, (unsigned long long)job.log_current,
                                     (unsigned long long)job.log_gpu, (unsigned long long)job.log_writer,
                                     job.log_write_ns ? (t_start - job.log_write_ns) / 1e3 : -1.0, job.log_writes,
                                     (unsigned long long)job.fault_lo, (unsigned long long)job.fault_hi,
                                     (unsigned long long)job.page_gpu_bytes, (unsigned long long)job.nearest);
                    }
                }
                FinishReadback(job);
            }
        }
        bool resolved = true;
        if (is_write) {
            // Marks the range CPU modified if it is GPU clean.
            memory_tracker->InvalidateRegion(device_addr, size, [&resolved] { resolved = false; });
        } else {
            resolved = !memory_tracker->IsRegionGpuModified(device_addr, size);
        }
        if (resolved) {
            return true;
        }
    }
    return false;
}

void BufferCache::RecordReadback(Readback& job) {
    {
        std::scoped_lock lk{readback_mutex};
        if (IsReadbackPending(job.window, READBACK_WINDOW_SIZE)) {
            job.busy = true;
            return;
        }
        MergeReadbackReturns();
    }
    ApplyQueuedStores();
    const u64 block = job.window >> block_shift;
    const auto* arena = GetArena(block, block);
    // bbport: one copy per fault pays a round trip of 1.4-3.3 ms whatever it copies (measured at
    // 4K DLAA: 152 bytes and 162 KB cost the same), and the guest faults 4-6 times a frame, so
    // copying every GPU-modified range of the arena at once (what the synchronous path does)
    // would turn 4-5 round trips into one. BB_READBACK_OFFLOAD_ARENA=1 tries it: it crashed the
    // renderer in testing (a fault in a second window of the same arena while the job was in
    // flight), so it is off by default and one window per job stays the default.
    static const bool arena_wide = [] {
        const char* env = std::getenv("BB_READBACK_OFFLOAD_ARENA");
        return env && env[0] == '1';
    }();
    job.copy_base = arena_wide ? arena->cpu_addr : job.window;
    job.copy_size = arena_wide ? arena->size_bytes : READBACK_WINDOW_SIZE;
    const u64 total_size_bytes = CollectDownloads(arena, job.copy_base, job.copy_size, job.copies);
    if (total_size_bytes == 0) {
        return;
    }
    if (ReadbackLog()) {
        const u64 slot = (job.window / READBACK_WINDOW_SIZE) & 4095;
        job.log_stamped = window_write_tick[slot];
        job.log_current = scheduler.CurrentTick();
        scheduler.GetWorkSemaphore()->Refresh();
        job.log_gpu = scheduler.GetWorkSemaphore()->KnownGpuTick();
        job.log_bytes = total_size_bytes;
        job.log_writer = window_writer[slot];
        job.log_write_ns = window_write_ns[slot];
        job.log_writes = window_writes[slot];
        for (const auto& copy : job.copies) {
            const VAddr lo = arena->cpu_addr + copy.srcOffset;
            if (job.fault >= lo && job.fault < lo + copy.size) {
                job.fault_lo = lo;
                job.fault_hi = lo + copy.size;
            }
            // GPU-modified bytes on the fault's own 4 KB page: their total, and the closest one.
            const VAddr page = Common::AlignDown(job.fault, 4096);
            const VAddr a = std::max(lo, page), e = std::min(lo + copy.size, page + 4096);
            if (a < e) {
                job.page_gpu_bytes += e - a;
            }
            const u64 dist = job.fault < lo ? lo - job.fault : job.fault >= lo + copy.size ? job.fault - (lo + copy.size) + 1 : 0;
            job.nearest = std::min(job.nearest, dist);
        }
        window_writes[slot] = 0;
    }
    job.arena_base = arena->cpu_addr;
    job.write_seq = memory_tracker->GpuWriteSeq(job.window, READBACK_WINDOW_SIZE);
    if (early_readback) {
        const u64 slot = (job.window / READBACK_WINDOW_SIZE) & 4095;
        hot_window[slot] = job.window;
        hot_tick[slot] = scheduler.CurrentTick();
        // The copy made at the end of the submission that wrote the window holds what is downloaded now when no GPU
        // write was marked in the window since and the ranges are the same.
        const auto it = std::ranges::find(early_readbacks, job.window, &EarlyReadback::window);
        if (it != early_readbacks.end()) {
            bool same = it->write_seq == job.write_seq && it->arena_base == job.arena_base &&
                        it->copies.size() == job.copies.size();
            for (size_t i = 0; same && i < job.copies.size(); ++i) {
                same = it->copies[i].srcOffset == job.copies[i].srcOffset && it->copies[i].size == job.copies[i].size;
            }
            if (same) {
                // BB_READBACK_EARLY_VERIFY=N: every Nth use also makes the usual copy; FinishReadback compares.
                static const int verify_every = [] {
                    const char* env = std::getenv("BB_READBACK_EARLY_VERIFY");
                    return env ? std::atoi(env) : 0;
                }();
                static u64 uses = 0;
                if (verify_every > 0 && (++uses % u64(verify_every)) == 0) {
                    job.early_verify =
                        staging_pool.Request(total_size_bytes, VideoCore::MemoryType::HostCached, 0, true);
                    DownloadCopies verify_copies = job.copies;
                    for (auto& copy : verify_copies) {
                        copy.dstOffset += job.early_verify.offset;
                    }
                    runtime.CopyBuffer(arena, job.early_verify.buffer, verify_copies);
                }
                const DownloadCopies packed = job.copies;
                job.copies = it->copies;
                job.staging = it->staging;
                job.tick = it->tick;
                early_readbacks.erase(it);
                BbStats::readback_paths[7].fetch_add(1, std::memory_order_relaxed);
                if (job.early_verify.buffer) {
                    // Packed offsets of the verify copy, in the same order as job.copies.
                    job.spans.clear();
                    for (size_t i = 0; i < packed.size(); ++i) {
                        job.spans.push_back({job.early_verify.mapped + packed[i].dstOffset, 0, packed[i].size});
                    }
                    job.tick = scheduler.CurrentTick();
                    scheduler.Flush();
                }
                std::scoped_lock lk{readback_mutex};
                pending_readbacks.emplace_back(job.copy_base, job.copy_size);
                return;
            }
            staging_pool.FreeDeferred(it->staging);
            early_readbacks.erase(it);
        }
    }

    // Direct path: the data is already in memory the CPU can read once the GPU has finished writing it, so no copy
    // is recorded. Both copies only when the entire arena is host visible: the readback windows take the faster
    // TryFastWindowRead path on the faulting thread instead, and their spans were resolved through the sparse
    // residency list, which does not always describe the promoted chunk.
    if (arena_host_visible && direct_readback.load(std::memory_order_relaxed) &&
        total_size_bytes <= 1_MB) {
        bool ok = true;
        for (const auto& copy : job.copies) {
            const u64 first = (job.arena_base + copy.srcOffset) >> block_shift;
            const u64 last = (job.arena_base + copy.srcOffset + copy.size - 1) >> block_shift;
            u64 covered = 0;
            resident_ranges.ForEachInRange(first, last + 1, [&](const Backing& backing) {
                if (!backing.base) {
                    return;
                }
                const u64 from = std::max<u64>(first, backing.start);
                const u64 to = std::min<u64>(last + 1, backing.end);
                const VAddr range_begin = std::max<VAddr>(job.arena_base + copy.srcOffset, from << block_shift);
                const VAddr range_end = std::min<VAddr>(job.arena_base + copy.srcOffset + copy.size, to << block_shift);
                if (range_end <= range_begin) {
                    return;
                }
                job.spans.push_back({backing.base + backing.offset + (range_begin - (u64{backing.start} << block_shift)),
                                     range_begin, range_end - range_begin});
                covered += range_end - range_begin;
            });
            ok = ok && covered == copy.size;
        }
        if (ok) {
            job.direct = true; // A tenth of the early direct reads (then one in a hundred and twenty-eight) also make the usual copy
            // and compare the two: a driver or memory type that behaves differently switches the direct path off.
            ++direct_reads;
            if (direct_reads == 1 || direct_reads % 2048 == 0) {
                std::printf("GPU: direct readbacks: %u (window %llx, %llu bytes)\n", direct_reads.load(),
                            static_cast<unsigned long long>(job.window),
                            static_cast<unsigned long long>(total_size_bytes));
                std::fflush(stdout);
            }
            const u32 every = direct_reads <= 512 ? 8u : 128u;
            const char* verify_env = std::getenv("BB_READBACK_VERIFY");
            job.verify = verify_env ? (std::atoi(verify_env) > 0 && (direct_reads % std::atoi(verify_env)) == 0)
                                    : (direct_reads % every) == 0;
            const u64 stamped = window_write_tick[(job.window / READBACK_WINDOW_SIZE) & 4095];
            job.tick = stamped ? stamped : scheduler.CurrentTick();
            if (arena_host_visible || WindowBarrierWanted()) {
                scheduler.RequestHostReadBarrier();
            }
        } else {
            job.spans.clear();
            if (window_host_visible && IsPromotedWindow(job.window) && std::getenv("BB_WINDOW_BIND_DEBUG")) {
                static std::atomic<u32> shown{0};
                if (shown++ < 6) {
                    for (const auto& copy : job.copies) {
                        const u64 first = (job.arena_base + copy.srcOffset) >> block_shift;
                        const u64 last = (job.arena_base + copy.srcOffset + copy.size - 1) >> block_shift;
                        std::printf("GPU: promoted window %llx not direct: copy %llx+%llu blocks %llx-%llx",
                                    static_cast<unsigned long long>(job.window),
                                    static_cast<unsigned long long>(job.arena_base + copy.srcOffset),
                                    static_cast<unsigned long long>(copy.size),
                                    static_cast<unsigned long long>(first),
                                    static_cast<unsigned long long>(last));
                        resident_ranges.ForEachInRange(first, last + 1, [&](const Backing& backing) {
                            std::printf(" [%llx-%llx mem %p off %llx base %p]", backing.start, backing.end,
                                        static_cast<const void*>(backing.memory),
                                        static_cast<unsigned long long>(backing.offset),
                                        static_cast<const void*>(backing.base));
                        });
                        std::printf("\n");
                    }
                    std::fflush(stdout);
                }
            }
        }
    }
    if (!job.direct || job.verify) {
        // Held until FinishReadback releases it; the pool reuses it once the GPU passes that release.
        job.staging =
            staging_pool.Request(total_size_bytes, VideoCore::MemoryType::HostCached, 0, true);
        for (auto& copy : job.copies) {
            copy.dstOffset += job.staging.offset;
        }
        runtime.CopyBuffer(arena, job.staging.buffer, job.copies);
        job.tick = scheduler.CurrentTick();
    }
    if (job.tick >= scheduler.CurrentTick()) {
        scheduler.Flush();
    }
    std::scoped_lock lk{readback_mutex};
    pending_readbacks.emplace_back(job.copy_base, job.copy_size);
}

void BufferCache::FinishReadback(const Readback& job) {
    bool use_direct = job.direct;
    if (job.staging.buffer) {
        job.staging.buffer->Invalidate(job.staging.offset, job.staging.size);
    }
    if (job.early_verify.buffer) {
        job.early_verify.buffer->Invalidate(job.early_verify.offset, job.early_verify.size);
        const bool rewritten =
            memory_tracker->GpuWriteSeq(job.copy_base, job.copy_size) != job.write_seq;
        u64 differing = 0;
        for (size_t i = 0; i < job.copies.size() && i < job.spans.size(); ++i) {
            const u8* early = job.staging.mapped + (job.copies[i].dstOffset - job.staging.offset);
            if (std::memcmp(early, job.spans[i].src, job.copies[i].size) != 0) {
                ++differing;
                // The usual copy is the reference.
                std::memcpy(const_cast<u8*>(early), job.spans[i].src, job.copies[i].size);
            }
        }
        BbStats::readback_paths[9].fetch_add(1, std::memory_order_relaxed);
        if (differing && !rewritten) {
            std::printf("GPU: early readback copy of window %llx differs from the usual copy in %llu of %zu ranges\n",
                        static_cast<unsigned long long>(job.window), static_cast<unsigned long long>(differing),
                        job.copies.size());
        }
        liverpool->SendCommand([this, staging = job.early_verify] { staging_pool.FreeDeferred(staging); });
    }
    if (use_direct && job.verify) {
        // Both copies hold the same GPU writes unless the GPU wrote the window again in between (then the window is
        // not unmarked below and comparing says nothing).
        const bool rewritten = memory_tracker->GpuWriteSeq(job.window, READBACK_WINDOW_SIZE) != job.write_seq;
        u64 pos = 0;
        bool same = true;
        for (const auto& copy : job.copies) {
            const u8* staged = job.staging.mapped + (copy.dstOffset - job.staging.offset);
            u64 done = 0;
            for (const auto& span : job.spans) {
                if (span.dst >= job.arena_base + copy.srcOffset && span.dst < job.arena_base + copy.srcOffset + copy.size) {
                    same = same && std::memcmp(span.src, staged + (span.dst - (job.arena_base + copy.srcOffset)), span.size) == 0;
                    done += span.size;
                }
            }
            pos += done;
        }
        if (!same && !rewritten) {
            BbStats::readback_paths[6].fetch_add(1, std::memory_order_relaxed);
            std::printf("GPU: a direct read of video memory differs from the copy made by the GPU: direct reads are "
                        "switched off\n");
            // The first differing byte, and the two values: which copy holds the newer data shows whether the
            // window's bind had executed or a write landed in the old memory.
            u64 pos = 0;
            for (const auto& copy : job.copies) {
                const u8* staged = job.staging.mapped + (copy.dstOffset - job.staging.offset);
                for (const auto& span : job.spans) {
                    if (span.dst >= job.arena_base + copy.srcOffset &&
                        span.dst < job.arena_base + copy.srcOffset + copy.size) {
                        const u8* direct =
                            span.src + (span.dst - (job.arena_base + copy.srcOffset));
                        if (std::memcmp(direct, staged + (span.dst - (job.arena_base + copy.srcOffset)),
                                        span.size) != 0) {
                            size_t i = 0;
                            while (i < span.size &&
                                   direct[i] == staged[span.dst - (job.arena_base + copy.srcOffset) + i]) {
                                ++i;
                            }
                            u8 guest[4]{};
                            runtime_memory_read_backing(span.dst, guest, sizeof(guest));
                            std::printf("GPU: window %llx copy src %llx size %llu: first difference at +%zu "
                                        "direct %02x (%02x %02x %02x) staged %02x (%02x %02x %02x) guest %02x "
                                        "(%02x %02x %02x) src %p chunk %p (tick %llu current %llu)\n",
                                        static_cast<unsigned long long>(job.window),
                                        static_cast<unsigned long long>(copy.srcOffset),
                                        static_cast<unsigned long long>(copy.size), i, direct[i],
                                        i + 1 < span.size ? direct[i + 1] : 0,
                                        i + 2 < span.size ? direct[i + 2] : 0,
                                        i + 3 < span.size ? direct[i + 3] : 0,
                                        staged[span.dst - (job.arena_base + copy.srcOffset) + i],
                                        i + 1 < span.size ? staged[span.dst - (job.arena_base + copy.srcOffset) + i + 1] : 0,
                                        i + 2 < span.size ? staged[span.dst - (job.arena_base + copy.srcOffset) + i + 2] : 0,
                                        i + 3 < span.size ? staged[span.dst - (job.arena_base + copy.srcOffset) + i + 3] : 0,
                                        guest[0], guest[1], guest[2], guest[3],
                                        static_cast<const void*>(span.src),
                                        static_cast<const void*>(g_window_chunk_base),
                                        static_cast<unsigned long long>(job.tick),
                                        static_cast<unsigned long long>(scheduler.CurrentTick()));
                            goto mismatch_logged;
                        }
                    }
                }
                pos += copy.size;
            }
        mismatch_logged:
            direct_readback.store(false, std::memory_order_relaxed);
            use_direct = false;
        }
    }
    // The write-back runs ahead of the verdict: other downloads of the window wait while this one
    // is pending, and a vetoed window stays GPU modified.
    if (use_direct) {
        BbStats::readback_paths[5].fetch_add(1, std::memory_order_relaxed);
        for (const auto& span : job.spans) {
            memory->TryWriteBacking(std::bit_cast<u8*>(span.dst), span.src, span.size);
        }
    } else {
        for (const auto& copy : job.copies) {
            auto* dst_addr = std::bit_cast<u8*>(job.arena_base + copy.srcOffset);
            memory->TryWriteBacking(
                dst_addr, job.staging.mapped + (copy.dstOffset - job.staging.offset), copy.size);
        }
    }
    // The byte map is cleared inside the tracker's lock and only when the unmark succeeds: writers bump the
    // write sequence under that lock and mark the map after it, so no window shows these bytes unmarked early.
    // bbport: a job that copies more than its window (the arena-wide mode) was guarded by a write sequence
    // taken over that whole range, so a write anywhere in the arena made the guard fail; every copied range
    // was then marked GPU-modified again although its bytes had already been delivered, the faulting
    // instruction faulted a second time, and the guest never made progress. The renderer stopped that way
    // with the command thread idle. The synchronous download has always unmarked exactly what it copied,
    // without the guard, so the wide job does the same; the per-window job keeps the guarded unmark.
    const bool wide_job = job.copy_size > READBACK_WINDOW_SIZE;
    const bool unmarked =
        wide_job
            ? (memory_tracker->UnmarkRegionAsGpuModified(job.copy_base, job.copy_size), [&] {
                  for (const auto& copy : job.copies) {
                      gpu_byte_map.Clear(job.arena_base + copy.srcOffset, copy.size);
                  }
                  return true;
              }())
            : memory_tracker->TryUnmarkRegionAsGpuModified(
                  job.copy_base, job.copy_size, job.write_seq, [&] {
                      if (use_direct) {
                          for (const auto& span : job.spans) {
                              gpu_byte_map.Clear(span.dst, span.size);
                          }
                      } else {
                          for (const auto& copy : job.copies) {
                              gpu_byte_map.Clear(job.arena_base + copy.srcOffset, copy.size);
                          }
                      }
                  });
    {
        std::scoped_lock lk{readback_mutex};
        if (!unmarked) {
            for (const auto& copy : job.copies) {
                readback_returns.emplace_back(job.arena_base + copy.srcOffset, copy.size);
            }
        }
        std::erase_if(pending_readbacks, [&](const std::pair<VAddr, u64>& pending) {
            return pending.first == job.copy_base;
        });
    }
    readback_cv.notify_all();
    MaybePromoteWindow(job.window);
    // The staging pool belongs to the GPU thread.
    if (job.staging.buffer) {
        liverpool->SendCommand([this, staging = job.staging] { staging_pool.FreeDeferred(staging); });
    }
}

BufferCache::WindowState* BufferCache::FindWindowState(VAddr window) const {
    const u64 slot = (window >> 19) & (WINDOW_SLOTS - 1);
    for (u32 probe = 0; probe < 8; ++probe) {
        const u32 i = u32(slot + probe) & (WINDOW_SLOTS - 1);
        const u64 key = window_keys[i].load(std::memory_order_acquire);
        if (key == 0) {
            return nullptr;
        }
        if (key == window + 1) {
            return window_states[i];
        }
    }
    return nullptr;
}

BufferCache::WindowState* BufferCache::RegisterWindowState(VAddr window) {
    const u32 index = window_state_count.load(std::memory_order_relaxed);
    if (index >= window_state_pool.size()) {
        return nullptr;
    }
    window_state_count.fetch_add(1, std::memory_order_relaxed);
    auto state = std::make_unique<WindowState>();
    state->window = window;
    WindowState* const ptr = state.get();
    window_state_pool[index] = std::move(state);
    const u64 slot = (window >> 19) & (WINDOW_SLOTS - 1);
    for (u32 probe = 0; probe < 8; ++probe) {
        const u32 i = u32(slot + probe) & (WINDOW_SLOTS - 1);
        if (window_keys[i].load(std::memory_order_acquire) == window + 1) {
            return window_states[i];
        }
        if (window_keys[i].load(std::memory_order_relaxed) == 0) {
            // Published before the key: a reader that sees the key sees the pointer.
            window_states[i] = ptr;
            window_keys[i].store(window + 1, std::memory_order_release);
            any_promoted.store(true, std::memory_order_release);
            return ptr;
        }
    }
    return nullptr;
}

void BufferCache::NoteWindowWrite(VAddr window, VAddr addr, u64 size, u64 tick) {
    WindowState* state = FindWindowState(window);
    if (!state || size == 0) {
        return;
    }
    std::scoped_lock lk{state->mutex};
    if (!state->active) {
        return;
    }
    // One entry per byte range, holding the newest tick of every write in it. The bindings of a
    // frame overlap heavily (a pass writes the same window several times), and delivering every
    // entry separately copied the same bytes many times: 18 MB for a 512 KiB window, ~260 ms of
    // reads from video memory. Only overlap is merged: touching ranges stay apart so that a small
    // read does not wait for the tick of the neighbouring write.
    VAddr lo = addr;
    VAddr hi = addr + size;
    u64 merged_tick = tick;
    for (auto it = state->writes.begin(); it != state->writes.end();) {
        if (it->addr < hi && lo < it->addr + it->size) {
            lo = std::min(lo, it->addr);
            hi = std::max(hi, it->addr + it->size);
            merged_tick = std::max(merged_tick, it->tick);
            it = state->writes.erase(it);
        } else {
            ++it;
        }
    }
    if (state->writes.size() >= 4096) {
        // Too many writes queued (no reads consumed them): let the normal download handle the
        // window from now on.
        state->overflow = true;
        state->writes.clear();
        return;
    }
    state->writes.push_back({lo, hi - lo, merged_tick});
}

// A guest read of a promoted window is served on the faulting thread: no round trip through the
// GPU command thread and no staging copy. Every GPU write into the window is recorded with the
// submission tick that will make it visible (StampGpuWrite), so waiting for the last of those
// ticks and then reading the window's mapping delivers exactly the bytes the copy path would
// deliver. All the window's recorded writes are delivered at once (not only the faulting page):
// the copy path unmarks the whole window for the same reason, and a page-sized delivery turned
// one round trip into one per page.
bool BufferCache::TryFastWindowRead(VAddr addr, u64 size) {
    static const bool enabled = [] {
        const char* env = std::getenv("BB_FAST_WINDOW_READ");
        return !(env && env[0] == '0');
    }();
    // Nothing to report while the windows are not host visible (the default): every fault would
    // otherwise print a line every 1024 reads.
    if (!enabled || !window_host_visible) {
        return false;
    }
    static std::atomic<u64> calls{0}, ok{0}, fb_wait{0}, fb_pending{0}, fb_state{0}, fb_unrecorded{0},
        copy_ns{0}, bytes{0};
    static std::atomic<bool> switched_off{false};
    struct Report {
        ~Report() {
            const u64 n = calls.fetch_add(1, std::memory_order_relaxed) + 1;
            if ((n % 1024) == 0) {
                std::printf("GPU: fast windows: calls=%llu ok=%llu wait=%llu pending=%llu state=%llu "
                            "unrecorded=%llu off=%d copy_ms=%llu bytes=%llu\n",
                            (unsigned long long)n, (unsigned long long)ok.load(),
                            (unsigned long long)fb_wait.load(),
                            (unsigned long long)fb_pending.load(),
                            (unsigned long long)fb_state.load(),
                            (unsigned long long)fb_unrecorded.load(),
                            int(switched_off.load()),
                            (unsigned long long)(copy_ns.load() / 1000000),
                            (unsigned long long)bytes.load());
                std::fflush(stdout);
            }
        }
    } report;
    if (switched_off.load(std::memory_order_relaxed) ||
        !direct_readback.load(std::memory_order_relaxed) || size == 0 ||
        size > READBACK_WINDOW_SIZE || !any_promoted.load(std::memory_order_acquire)) {
        fb_state.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    const VAddr window = Common::AlignDown(addr, READBACK_WINDOW_SIZE);
    if (addr + size > window + READBACK_WINDOW_SIZE) {
        fb_state.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    WindowState* const state = FindWindowState(window);
    if (!state) {
        fb_state.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    {
        // A fallback readback of the same window is in flight: it owns the data.
        std::scoped_lock lk{readback_mutex};
        if (IsReadbackPending(window, READBACK_WINDOW_SIZE)) {
            fb_pending.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
    }
    // A binding stamped since the last draw was recorded may execute in a later submission than
    // the tick it recorded: let the normal path, which drains the pipe, handle the window.
    if (any_unrecorded.load(std::memory_order_relaxed)) {
        fb_unrecorded.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    static const bool wait_enabled = [] {
        const char* env = std::getenv("BB_FAST_WAIT");
        return !(env && env[0] == '0');
    }();
    // Only a window whose writes the GPU has already finished is served here. Serving one that is
    // still being written would wait for exactly what the copy path waits for, without the copy
    // path's window-wide batching: measured 5 ms of waiting per read at 4K DLAA (the GPU runs a
    // frame behind), while the reads that are already complete cost nothing but the mapping read.
    // The rest go to the copy path, which is no slower than before.
    scheduler.GetWorkSemaphore()->Refresh();
    const u64 known_gpu = scheduler.GetWorkSemaphore()->KnownGpuTick();
    u64 tick = 0;
    u64 queued = 0;
    u64 pending_bytes = 0;
    for (u32 attempt = 0; attempt < 4; ++attempt) {
        tick = 0;
        queued = 0;
        pending_bytes = 0;
        {
            std::scoped_lock lk{state->mutex};
            if (!state->active || state->overflow) {
                fb_state.fetch_add(1, std::memory_order_relaxed);
                return false;
            }
            for (const WindowWrite& write : state->writes) {
                tick = std::max(tick, write.tick);
                pending_bytes += write.size;
            }
            queued = state->writes.size();
        }
        if (tick == 0) {
            // Nothing recorded since the promotion: the normal path knows about the old writes.
            fb_state.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        if (wait_enabled && known_gpu < tick) {
            // The GPU has not finished the writes yet.
            fb_wait.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        // Reading the mapping is slow (device memory: ~70 MB/s), so a large window is left to the
        // copy path, whose GPU transfer is fast.
        static const u64 max_bytes = [] {
            const char* env = std::getenv("BB_FAST_WINDOW_MAX_KB");
            return (env ? std::strtoull(env, nullptr, 10) : 256ull) * 1024;
        }();
        if (pending_bytes > max_bytes) {
            fb_state.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        const u64 write_seq = memory_tracker->GpuWriteSeq(window, READBACK_WINDOW_SIZE);
        std::vector<WindowWrite> ready;
        {
            std::scoped_lock lk{state->mutex};
            auto split = std::partition(state->writes.begin(), state->writes.end(),
                                        [&](const WindowWrite& write) { return write.tick <= tick; });
            ready.assign(state->writes.begin(), split);
            state->writes.erase(state->writes.begin(), split);
        }
        const s64 c0 = BbTrace::NowNs();
        u64 delivered = 0;
        for (const WindowWrite& write : ready) {
            u8* const source = state->base + state->memory_offset + (write.addr - window);
            memory->TryWriteBacking(std::bit_cast<u8*>(write.addr), source, write.size);
            delivered += write.size;
        }
        const s64 c1 = BbTrace::NowNs();
        copy_ns.fetch_add(u64(c1 - c0), std::memory_order_relaxed);
        bytes.fetch_add(delivered, std::memory_order_relaxed);
        // The whole window is unmarked, exactly what the copy path does: the tracker's GPU-modified
        // state is per page, so a page holding bytes that were not delivered must not be unmarked.
        // A write marked while this ran changes the sequence and keeps the window marked.
        if (memory_tracker->TryUnmarkRegionAsGpuModified(window, READBACK_WINDOW_SIZE, write_seq, [&] {
                for (const WindowWrite& write : ready) {
                    gpu_byte_map.Clear(write.addr, write.size);
                }
            })) {
            ok.fetch_add(1, std::memory_order_relaxed);
            BbStats::readback_paths[5].fetch_add(1, std::memory_order_relaxed);
            if ((++direct_reads % 2048) == 0) {
                std::printf("GPU: fast window reads: %u (window %llx)\n", direct_reads.load(),
                            static_cast<unsigned long long>(window));
                std::fflush(stdout);
            }
            if (FILE* log = ReadbackLog()) {
                std::scoped_lock lk{g_readback_log_mutex};
                std::fprintf(log, "%.3f %u %lu %llx %llx f 0 0 0 %.0f %llu %llu %llu %llu %llu 0 -1 0 "
                                  "%llx %llx 0 0\n",
                             c0 / 1e6, BbTrace::frame_gen.load(std::memory_order_relaxed), LogThreadId(),
                             (unsigned long long)BbTrace::fault_rip, (unsigned long long)addr,
                             double((c1 - c0) / 1e3), (unsigned long long)delivered,
                             (unsigned long long)queued, (unsigned long long)tick,
                             (unsigned long long)scheduler.CurrentTick(), (unsigned long long)known_gpu,
                             (unsigned long long)window,
                             (unsigned long long)(window + READBACK_WINDOW_SIZE));
            }
            return true;
        }
        // The window was written again while the delivery ran: the entries left in the list are
        // delivered by the next attempt (usually on the next fault). Wait for the newer tick.
    }
    return false;
}

BufferCache::CopyReadback& BufferCache::EnsureCopyReadback() {
    if (!copy_readback) {
        auto c = std::make_unique<CopyReadback>();
        const vk::Device device = instance.GetDevice();
        auto [pool_result, pool] = device.createCommandPoolUnique(
            {.flags = vk::CommandPoolCreateFlagBits::eResetCommandBuffer,
             .queueFamilyIndex = instance.GetGraphicsQueueFamilyIndex()});
        if (pool_result != vk::Result::eSuccess) {
            copy_readback = std::move(c);
            return *copy_readback;
        }
        c->pool = std::move(pool);
        const vk::CommandBufferAllocateInfo alloc = {
            .commandPool = *c->pool,
            .level = vk::CommandBufferLevel::ePrimary,
            .commandBufferCount = 1,
        };
        if (device.allocateCommandBuffers(&alloc, &c->cmdbuf) != vk::Result::eSuccess) {
            copy_readback = std::move(c);
            return *copy_readback;
        }
        auto [fence_result, fence] = device.createFenceUnique({.flags = vk::FenceCreateFlagBits::eSignaled});
        if (fence_result != vk::Result::eSuccess) {
            copy_readback = std::move(c);
            return *copy_readback;
        }
        c->fence = std::move(fence);
        auto [buffer_result, buffer] = device.createBufferUnique(
            {.size = READBACK_WINDOW_SIZE,
             .usage = vk::BufferUsageFlagBits::eTransferDst,
             .sharingMode = vk::SharingMode::eExclusive});
        if (buffer_result != vk::Result::eSuccess) {
            copy_readback = std::move(c);
            return *copy_readback;
        }
        c->staging = std::move(buffer);
        const auto reqs = device.getBufferMemoryRequirements(*c->staging);
        const auto type = FindMemoryType(instance.GetMemoryProperties(),
                                         vk::MemoryPropertyFlagBits::eHostVisible |
                                             vk::MemoryPropertyFlagBits::eHostCoherent,
                                         reqs.memoryTypeBits);
        if (!type) {
            copy_readback = std::move(c);
            return *copy_readback;
        }
        auto [memory_result, memory] = device.allocateMemoryUnique(
            {.allocationSize = reqs.size, .memoryTypeIndex = *type});
        if (memory_result != vk::Result::eSuccess) {
            copy_readback = std::move(c);
            return *copy_readback;
        }
        c->memory = std::move(memory);
        device.bindBufferMemory(*c->staging, *c->memory, 0);
        auto [map_result, mapping] = device.mapMemory(*c->memory, 0, vk::WholeSize);
        if (map_result != vk::Result::eSuccess) {
            copy_readback = std::move(c);
            return *copy_readback;
        }
        c->mapped = static_cast<u8*>(mapping);
        c->ready = true;
        std::printf("GPU: readback copy queue staging: %.0f KiB host visible\n", READBACK_WINDOW_SIZE / 1024.0);
        std::fflush(stdout);
        copy_readback = std::move(c);
    }
    return *copy_readback;
}

// A second queue of the graphics family downloads a whole read window, and its submission waits
// only for the timeline value of the write that the guest is reading (StampGpuWrite's tick), not for
// everything the CPU recorded until the fault. The command thread records nothing here, so neither
// the pickup wait nor the flush of the open command buffer is paid. The wait for the write is the
// same GPU work the normal path waits for; whatever was submitted behind it (the rest of the frame)
// is not in the way.
bool BufferCache::TryCopyQueueReadback(VAddr addr, u64 size) {
    static const bool enabled = [] {
        const char* env = std::getenv("BB_READBACK_COPY_QUEUE");
        return env && env[0] != '0';
    }();
    if (!enabled || !instance.HasCopyQueue() || size == 0 || size > READBACK_WINDOW_SIZE) {
        return false;
    }
    const VAddr window = Common::AlignDown(addr, READBACK_WINDOW_SIZE);
    if (addr + size > window + READBACK_WINDOW_SIZE) {
        return false;
    }
    {
        std::scoped_lock lk{readback_mutex};
        if (IsReadbackPending(window, READBACK_WINDOW_SIZE)) {
            return false;
        }
    }
    // A binding stamped since the last recorded draw may execute after the tick it recorded.
    if (any_unrecorded.load(std::memory_order_relaxed)) {
        return false;
    }
    const u64 tick = window_write_tick[(window / READBACK_WINDOW_SIZE) & 4095];
    if (tick == 0 || tick >= scheduler.CurrentTick()) {
        return false; // nothing stamped, or the write is still in the open command buffer
    }
    const u64 block = window >> block_shift;
    const auto* arena = GetArena(block, block);
    if (window < arena->cpu_addr || window + READBACK_WINDOW_SIZE > arena->cpu_addr + arena->size_bytes) {
        return false;
    }
    std::scoped_lock lk{copy_readback_mutex};
    CopyReadback& c = EnsureCopyReadback();
    if (!c.ready) {
        return false;
    }
    const u64 write_seq = memory_tracker->GpuWriteSeq(window, READBACK_WINDOW_SIZE);
    const vk::Device device = instance.GetDevice();
    if (device.resetFences(*c.fence) != vk::Result::eSuccess) {
        return false;
    }
    // Only the pages the guest still needs: the copy is on the GPU, and copying a whole 512 KiB
    // window per read (1446 reads in a 10 s run) added more GPU work than the readback saved.
    boost::container::small_vector<vk::BufferCopy, 16> copies;
    for (VAddr page = window; page < window + READBACK_WINDOW_SIZE;) {
        if (!gpu_byte_map.Any(page, 4096)) {
            page += 4096;
            continue;
        }
        VAddr end = page + 4096;
        while (end < window + READBACK_WINDOW_SIZE && gpu_byte_map.Any(end, 4096)) {
            end += 4096;
        }
        copies.push_back(vk::BufferCopy{.srcOffset = page - arena->cpu_addr,
                                        .dstOffset = page - window,
                                        .size = end - page});
        page = end;
    }
    if (copies.empty()) {
        return false; // nothing marked: the normal path (or the backing) already has it
    }
    c.cmdbuf.reset({});
    (void)c.cmdbuf.begin({.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit});
    for (const auto& copy : copies) {
        c.cmdbuf.copyBuffer(arena->Handle(), *c.staging, copy);
    }
    (void)c.cmdbuf.end();
    const vk::Semaphore sem = scheduler.GetWorkSemaphore()->Handle();
    const vk::PipelineStageFlags stage = vk::PipelineStageFlagBits::eTransfer;
    const vk::TimelineSemaphoreSubmitInfo timeline = {
        .waitSemaphoreValueCount = 1,
        .pWaitSemaphoreValues = &tick,
    };
    const vk::SubmitInfo submit = {
        .pNext = &timeline,
        .waitSemaphoreCount = 1,
        .pWaitSemaphores = &sem,
        .pWaitDstStageMask = &stage,
        .commandBufferCount = 1,
        .pCommandBuffers = &c.cmdbuf,
    };
    if (instance.GetCopyQueue().submit(submit, *c.fence) != vk::Result::eSuccess) {
        c.ready = false;
        return false;
    }
    const s64 t_submit = BbTrace::NowNs();
    if (device.waitForFences(*c.fence, true, 100'000'000) != vk::Result::eSuccess) {
        // The write's tick never completed (a stall or a device loss): stop using the queue and let
        // the normal path deal with this read. The fence is leaked rather than reset while pending.
        c.ready = false;
        std::printf("GPU: readback copy queue stalled (tick %llu, open %llu); switched off\n",
                    static_cast<unsigned long long>(tick),
                    static_cast<unsigned long long>(scheduler.CurrentTick()));
        std::fflush(stdout);
        return false;
    }
    const s64 t_done = BbTrace::NowNs();
    u64 delivered = 0;
    for (const auto& copy : copies) {
        for (u64 at = 0; at < copy.size; at += 4096) {
            const VAddr page = window + copy.dstOffset + at;
            memory->TryWriteBacking(std::bit_cast<u8*>(page), c.mapped + copy.dstOffset + at, 4096);
            delivered += 4096;
        }
    }
    const bool unmarked = memory_tracker->TryUnmarkRegionAsGpuModified(
        window, READBACK_WINDOW_SIZE, write_seq, [&] {
            for (VAddr page = window; page < window + READBACK_WINDOW_SIZE; page += 4096) {
                gpu_byte_map.Clear(page, 4096);
            }
        });
    if (!unmarked) {
        return false; // written again while the copy ran: the window stays GPU modified
    }
    BbStats::readback_paths[6].fetch_add(1, std::memory_order_relaxed);
    if (FILE* log = ReadbackLog()) {
        std::scoped_lock lock{g_readback_log_mutex};
        std::fprintf(log, "%.3f %u %lu %llx %llx q 0 0 %.0f %.0f %llu %llu %llu %llu %llu 0 -1 0 %llx %llx 0 0\n",
                     t_submit / 1e6, BbTrace::frame_gen.load(std::memory_order_relaxed),
                     LogThreadId(), (unsigned long long)BbTrace::fault_rip,
                     (unsigned long long)addr, double((t_done - t_submit) / 1e3),
                     double((BbTrace::NowNs() - t_done) / 1e3), (unsigned long long)delivered,
                     (unsigned long long)copies.size(), (unsigned long long)tick,
                     (unsigned long long)scheduler.CurrentTick(),
                     (unsigned long long)scheduler.GetWorkSemaphore()->KnownGpuTick(),
                     (unsigned long long)window,
                     (unsigned long long)(window + READBACK_WINDOW_SIZE));
    }
    return true;
}

void BufferCache::MaybePromoteWindow(VAddr window) {
    if (!window_host_visible || !direct_readback.load(std::memory_order_relaxed)) {
        return;
    }
    bool promote = false;
    {
        std::scoped_lock lk{readback_mutex};
        if (window_promote_queued.contains(window)) {
            return;
        }
        const u32 reads = ++window_reads[window];
        if (reads >= window_promote_after) {
            window_promote_queued.insert(window);
            promote = true;
        }
    }
    if (promote) {
        liverpool->SendCommand([this, window] { PromoteWindow(window); });
    }
}

// Moves one read window into device-local host-visible memory. Runs on the GPU command thread, so no
// draw is being recorded into the window while its contents move. The guest read that asked for this
// has already brought every GPU write into the guest backing store; a write that arrived since is
// downloaded first. From the next submission on, the window's blocks are bound to the new memory and
// the CPU reads it through the mapping (RecordReadback's direct path).
void BufferCache::PromoteWindow(VAddr window) {
    if (!window_host_visible || !direct_readback.load(std::memory_order_relaxed) ||
        promoted_windows.contains(window) || unpromotable_windows.contains(window)) {
        return;
    }
    // Bounded: 128 MiB of windows; the game uses a handful.
    if (promoted_windows.size() >= 256) {
        return;
    }
    const u64 first_block = window >> block_shift;
    const u64 last_block = (window + READBACK_WINDOW_SIZE - 1) >> block_shift;
    const auto* arena = GetArena(first_block, last_block);
    // A readback of this window that the faulting thread has not finished yet owns its data.
    {
        std::unique_lock lk{readback_mutex};
        readback_cv.wait(lk, [&] { return !IsReadbackPending(window, READBACK_WINDOW_SIZE); });
        MergeReadbackReturns();
    }
    // No draw may be recorded from now until the bind is submitted: between the copy into the new
    // memory and the bind taking effect, a draw submitted by the recording thread would write the old
    // memory and its data would be lost. With the pipe drained the GPU thread is the only producer.
    if (drain_gpu_pipe) {
        drain_gpu_pipe();
    }
    scheduler.Finish();
    ApplyQueuedStores();
    if (memory_tracker->IsRegionGpuModified(window, READBACK_WINDOW_SIZE)) {
        DownloadMemory(arena, window, READBACK_WINDOW_SIZE);
    }

    // One allocation holds several windows, mapped for the life of the cache.
    if (window_chunks.empty() ||
        window_chunks.back().used + READBACK_WINDOW_SIZE > window_chunks.back().size) {
        constexpr u64 ChunkSize = 4_MB;
        const auto alloc = instance.GetDevice().allocateMemory(
            {.allocationSize = ChunkSize, .memoryTypeIndex = window_memory_type_index});
        if (alloc.result != vk::Result::eSuccess) {
            if (++window_promote_failures <= 3) {
                std::printf("GPU: no memory for a host visible readback window (%u failures)\n",
                            window_promote_failures);
            }
            unpromotable_windows.insert(window);
            return;
        }
        const auto mapping = instance.GetDevice().mapMemory(*alloc, 0, vk::WholeSize);
        if (mapping.result != vk::Result::eSuccess) {
            instance.GetDevice().freeMemory(*alloc);
            unpromotable_windows.insert(window);
            return;
        }
        window_chunks.push_back({*alloc, static_cast<u8*>(mapping.value), ChunkSize, 0});
    }
    HostChunk& chunk = window_chunks.back();
    u8* base = chunk.base + chunk.used;
    const u64 memory_offset = chunk.used;
    g_window_chunk_base = chunk.base;
    // A window can straddle the end of the guest's allocation, so only the blocks with a CPU backing
    // are copied; the others keep their sparse binding and their copy path.
    const u64 block_count = READBACK_WINDOW_SIZE >> block_shift;
    std::vector<u64> good;
    good.reserve(block_count);
    for (u64 i = 0; i < block_count; ++i) {
        if (runtime_memory_read_backing(window + i * block_size, base + i * block_size, block_size)) {
            good.push_back(i);
        }
    }
    if (good.empty()) {
        if (++window_promote_failures <= 8) {
            std::printf("GPU: readback window %llx has no CPU backing, left on the copy path\n",
                        static_cast<unsigned long long>(window));
            std::fflush(stdout);
        }
        unpromotable_windows.insert(window);
        return;
    }
    chunk.used += READBACK_WINDOW_SIZE;
    if (std::getenv("BB_WINDOW_BIND_DEBUG")) {
        std::vector<u8> check(READBACK_WINDOW_SIZE);
        const bool read = runtime_memory_read_backing(window, check.data(), READBACK_WINDOW_SIZE) != 0;
        size_t diff = 0;
        while (read && diff < check.size() && check[diff] == base[diff]) {
            ++diff;
        }
        std::printf("GPU: promotion filled from guest backing: read %d, first difference %zu of %llu\n", int(read),
                    diff, static_cast<unsigned long long>(READBACK_WINDOW_SIZE));
    }

    // One sparse bind and one residency entry per contiguous run of backed blocks.
    struct Run {
        u64 first_block, last_block;
        u64 memory_offset;
        u8* base;
    };
    std::vector<Run> runs;
    for (size_t g = 0; g < good.size();) {
        size_t e = g;
        while (e + 1 < good.size() && good[e + 1] == good[e] + 1) {
            ++e;
        }
        const u64 i0 = good[g];
        // Backing::base is the mapping of the whole allocation; the slice's position in it is the
        // offset (the direct path adds both).
        runs.push_back({first_block + i0, first_block + good[e] + 1, memory_offset + i0 * block_size,
                        chunk.base});
        g = e + 1;
    }
    auto* binds = &BindsForArena(arena)->binds;
    std::vector<u64> gaps;
    for (const Run& run : runs) {
        const u64 lo = (run.first_block << block_shift) - arena->cpu_addr;
        const u64 hi = (run.last_block << block_shift) - arena->cpu_addr;
        resident_ranges.ForEachGap(run.first_block, run.last_block, [&](u64 start, u64 end) {
            for (u64 block = start; block < end; ++block) {
                gaps.push_back(block);
            }
        });
        resident_ranges.Add(Backing{{run.first_block, run.last_block}, chunk.memory, run.memory_offset,
                                    run.base});
        // Drop pending binds that overlap this run (they would fight the new memory), keeping the
        // parts outside it.
        boost::container::small_vector<vk::SparseMemoryBind, 32> kept;
        kept.reserve(binds->size() + 1);
        for (const auto& bind : *binds) {
            const u64 bind_lo = bind.resourceOffset;
            const u64 bind_hi = bind.resourceOffset + bind.size;
            if (bind_hi <= lo || bind_lo >= hi) {
                kept.push_back(bind);
                continue;
            }
            if (bind_lo < lo) {
                kept.push_back({.resourceOffset = bind_lo, .size = lo - bind_lo, .memory = bind.memory,
                                .memoryOffset = bind.memoryOffset});
            }
            if (bind_hi > hi) {
                kept.push_back({.resourceOffset = hi, .size = bind_hi - hi, .memory = bind.memory,
                                .memoryOffset = bind.memoryOffset + (hi - bind_lo)});
            }
        }
        kept.push_back({.resourceOffset = lo,
                        .size = hi - lo,
                        .memory = chunk.memory,
                        .memoryOffset = run.memory_offset});
        *binds = std::move(kept);
    }
    if (!gaps.empty()) {
        WriteBdaEntries(arena, gaps);
    }
    DropEarlyReadback(window);
    promoted_windows.insert(window);
    if (good.size() == block_count) {
        if (WindowState* state = RegisterWindowState(window)) {
            state->base = chunk.base;
            state->memory_offset = memory_offset;
            std::scoped_lock lk{state->mutex};
            state->active = true;
        }
    }
    ++window_promotions;
    std::printf("GPU: readback window %llx is host visible (%zu of %llu blocks, memory type %u, %u windows, "
                "%u unbacked)\n",
                static_cast<unsigned long long>(window), good.size(),
                static_cast<unsigned long long>(block_count), window_memory_type_index,
                window_promotions, window_promote_failures);
    std::fflush(stdout);
    scheduler.RequestHostReadBarrier();
    if (std::getenv("BB_WINDOW_BIND_DEBUG")) {
        std::printf("GPU: promote window %llx arena %p base %llx chunk %p map %p + %llx, bind at %llx\n",
                    static_cast<unsigned long long>(window), static_cast<const void*>(arena->Handle()),
                    static_cast<unsigned long long>(arena->cpu_addr),
                    static_cast<const void*>(chunk.memory), static_cast<const void*>(chunk.base),
                    static_cast<unsigned long long>(memory_offset),
                    static_cast<unsigned long long>(window - arena->cpu_addr));
        std::fflush(stdout);
    }
    // Submit the bind now. The submit callback puts the bind into vkQueueBindSparse and makes this
    // submission wait for it, so no command recorded from here on can write the old memory.
    scheduler.Flush();
}

// Fills the BDA page table (per 64 KiB block) with the arena addresses of blocks that were not
// resident before they were bound to a host visible window.
void BufferCache::WriteBdaEntries(const Buffer* arena, std::span<const u64> blocks) {
    const auto staging = staging_pool.Request(blocks.size() * sizeof(vk::DeviceAddress),
                                              MemoryType::HostUncached);
    auto* addrs = reinterpret_cast<vk::DeviceAddress*>(staging.mapped);
    DownloadCopies copies;
    u64 offset = staging.offset;
    for (const u64 block : blocks) {
        *(addrs++) = arena->BufferDeviceAddress() + (block << block_shift) - arena->cpu_addr;
        copies.push_back(vk::BufferCopy{
            .srcOffset = offset,
            .dstOffset = block * sizeof(vk::DeviceAddress),
            .size = sizeof(vk::DeviceAddress),
        });
        offset += sizeof(vk::DeviceAddress);
    }
    staging.Flush();
    runtime.CopyBuffer(staging.buffer, bda_pagetable_buffer.get(), copies);
}

void BufferCache::DropEarlyReadback(VAddr window) {
    const auto it = std::ranges::find(early_readbacks, window, &EarlyReadback::window);
    if (it != early_readbacks.end()) {
        staging_pool.FreeDeferred(it->staging);
        early_readbacks.erase(it);
    }
}

void BufferCache::RecordEarlyReadbacks() {
    if (!early_readback || (early_pending.empty() && early_readbacks.empty())) {
        return;
    }
    // The buffer cache belongs to the GPU command thread, or to the draw recording thread while draw packets are
    // in flight (two-stage pipe); submissions from elsewhere leave the windows pending.
    if (std::this_thread::get_id() != liverpool->GetGpuCommandProcessorThread() &&
        !Vulkan::DrawPipe::OnStageB()) {
        return;
    }
    // Called from inside the submission: its tick was taken already.
    const u64 tick = scheduler.CurrentTick() - 1;
    // Copies nobody read for a while.
    std::erase_if(early_readbacks, [&](const EarlyReadback& e) {
        if (tick - e.tick < 128) {
            return false;
        }
        staging_pool.FreeDeferred(e.staging);
        return true;
    });
    std::vector<VAddr> keep_pending;
    for (const VAddr window : early_pending) {
        if (IsPromotedWindow(window)) {
            DropEarlyReadback(window); // read straight from the mapping now, the copy is wasted
            continue;
        }
        if (std::ranges::find(unrecorded_windows, window) != unrecorded_windows.end()) {
            keep_pending.push_back(window); // the draw that writes it is not recorded yet: copy at a later submission
            BbStats::readback_paths[10].fetch_add(1, std::memory_order_relaxed);
            continue;
        }
        DropEarlyReadback(window);
        const u64 block = window >> block_shift;
        const auto* arena = GetArena(block, block);
        EarlyReadback early{.window = window, .arena_base = arena->cpu_addr};
        u64 total_size_bytes = 0;
        memory_tracker->ForEachDownloadRange<false>(window, READBACK_WINDOW_SIZE, [&](u64 address, u64 size) {
            gpu_modified_ranges.ForEachInRange(address, size, [&](VAddr start, VAddr end) {
                early.copies.push_back(vk::BufferCopy{
                    .srcOffset = start - early.arena_base,
                    .dstOffset = total_size_bytes,
                    .size = end - start,
                });
                total_size_bytes += (end - start + 63) & ~u64{63};
            });
        });
        if (total_size_bytes == 0) {
            continue;
        }
        if (early_readbacks.size() >= 128) {
            staging_pool.FreeDeferred(early_readbacks.front().staging);
            early_readbacks.erase(early_readbacks.begin());
        }
        early.staging = staging_pool.Request(total_size_bytes, VideoCore::MemoryType::HostCached, 0, true);
        for (auto& copy : early.copies) {
            copy.dstOffset += early.staging.offset;
        }
        runtime.CopyBuffer(arena, early.staging.buffer, early.copies);
        early.write_seq = memory_tracker->GpuWriteSeq(window, READBACK_WINDOW_SIZE);
        early.tick = tick;
        early_readbacks.push_back(std::move(early));
        BbStats::readback_paths[8].fetch_add(1, std::memory_order_relaxed);
        scheduler.RequestHostReadBarrier();
    }
    early_pending = std::move(keep_pending);
}

bool BufferCache::IsReadbackPending(VAddr addr, u64 size) const {
    return std::ranges::any_of(pending_readbacks, [&](const std::pair<VAddr, u64>& job) {
        return job.first < addr + size && addr < job.first + job.second;
    });
}

void BufferCache::MergeReadbackReturns() {
    for (const auto& [addr, size] : readback_returns) {
        gpu_modified_ranges.Add(addr, size);
        gpu_byte_map.Mark(addr, size);
    }
    readback_returns.clear();
}

namespace {
// bbport: BB_BUFFER_STATS=1 - how buffer bindings reach the GPU, by guest region (256 MiB),
// printed every 5 s: small read-only copies into the stream buffer, arena bindings, and the
// bytes those re-upload after CPU writes. Input for engine-level short paths.
struct BufferStats {
    struct Region {
        u64 stream_count{}, stream_bytes{}, arena_count{}, arena_bytes{}, upload_bytes{};
    };
    std::map<u64, Region> regions;
    /// Hot window: frame of the last binding per 64 KiB block, and the distribution of the
    /// frames between uses (1, 2, 3, 4, 5-8, 9-16, 17+): the ring's reuse distance.
    std::unordered_map<u64, u64> last_use;
    std::array<u64, 7> reuse{};
    std::chrono::steady_clock::time_point window = std::chrono::steady_clock::now();
    u64 calls = 0;
};
/// 256 MiB regions; 1 MiB inside the hot 0x104xxxxxxx window (the engine's frame data).
BufferStats& Stats();
void NoteHotUse(VAddr address, u32 size) {
    if ((address >> 28) != 0x104) {
        return;
    }
    auto& st = Stats();
    const u64 frame = BbStats::gpu_frames.load(std::memory_order_relaxed);
    for (u64 block = address >> 16; block <= (address + size - 1) >> 16; ++block) {
        auto [it, inserted] = st.last_use.try_emplace(block, frame);
        if (!inserted && it->second != frame) {
            const u64 d = frame - it->second;
            ++st.reuse[d <= 4 ? d - 1 : d <= 8 ? 4 : d <= 16 ? 5 : 6];
            it->second = frame;
        }
    }
}
u64 RegionKey(VAddr address) {
    return (address >> 28) == 0x104 ? (address >> 20) | (1ull << 40) : address >> 28;
}
bool BufferStatsEnabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("BB_BUFFER_STATS");
        return value && value[0] == '1';
    }();
    return enabled;
}
BufferStats& Stats() {
    static BufferStats stats;
    return stats;
}
void PrintBufferStats() {
    auto& st = Stats();
    if ((++st.calls & 4095) != 0) {
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    const double seconds = std::chrono::duration<double>(now - st.window).count();
    if (seconds < 5.0) {
        return;
    }
    std::printf("Buffer stats (%.1f s, per second): region, stream copies/bytes, arena bindings, "
                "re-uploaded bytes\n", seconds);
    std::printf("  hot window reuse distance in frames (1,2,3,4,5-8,9-16,17+): %llu %llu %llu %llu "
                "%llu %llu %llu\n",
                (unsigned long long)st.reuse[0], (unsigned long long)st.reuse[1],
                (unsigned long long)st.reuse[2], (unsigned long long)st.reuse[3],
                (unsigned long long)st.reuse[4], (unsigned long long)st.reuse[5],
                (unsigned long long)st.reuse[6]);
    st.reuse = {};
    for (const auto& [region, r] : st.regions) {
        const u64 base = (region >> 40) ? (region & ((1ull << 40) - 1)) << 20 : region << 28;
        if ((region >> 40) && r.stream_bytes + r.upload_bytes < 1e6 * seconds) {
            continue; // quiet MiB of the hot window
        }
        std::printf("  %#012llx: %8.0f copies %8.2f MB, %8.0f arena %8.2f MB bound, %8.2f MB uploads\n",
                    static_cast<unsigned long long>(base), r.stream_count / seconds,
                    r.stream_bytes / seconds / 1e6, r.arena_count / seconds,
                    r.arena_bytes / seconds / 1e6,
                    r.upload_bytes / seconds / 1e6);
    }
    st.regions.clear();
    st.window = now;
}
} // namespace

bool BufferCache::CanQueueUntrackedStore(VAddr addr, u32 size) {
    const VAddr lo = Common::AlignDown(addr, 4);
    const VAddr hi = Common::AlignUp(addr + size, 4);
    if (!memory_tracker->IsRegionGpuModified(lo, hi - lo)) {
        BbStats::store_why[4].fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    if (gpu_byte_map.Any(lo, hi - lo)) {
        BbStats::store_why[5].fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    if ((lo >> block_shift) != ((hi - 1) >> block_shift)) {
        BbStats::store_why[7].fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    return true;
}

} // namespace VideoCore

namespace BbStats {
namespace {
VideoCore::GpuByteMap& CeDumpMap() {
    static VideoCore::GpuByteMap map;
    return map;
}
std::atomic<bool> ce_dumps_seen{false};
} // namespace
void NoteCeDump(std::uint64_t addr, std::uint64_t size) {
    CeDumpMap().Mark(addr, size);
    ce_dumps_seen.store(true, std::memory_order_relaxed);
}
bool CeDumpOverlaps(std::uint64_t addr, std::uint64_t size) {
    return ce_dumps_seen.load(std::memory_order_relaxed) && CeDumpMap().Any(addr, size);
}
} // namespace BbStats

namespace VideoCore {

bool BufferCache::QueueUntrackedStore(VAddr addr, const void* data, u32 size) {
    const VAddr lo = Common::AlignDown(addr, 4);
    const VAddr hi = Common::AlignUp(addr + size, 4);
    if (!memory_tracker->IsRegionGpuModified(lo, hi - lo)) {
        BbStats::store_why[4].fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    if (gpu_byte_map.Any(lo, hi - lo)) {
        BbStats::store_why[5].fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    if ((lo >> block_shift) != ((hi - 1) >> block_shift) || !runtime_memory_write_backing(addr, data, size)) {
        BbStats::store_why[7].fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    std::scoped_lock lk{queued_store_mutex};
    queued_stores.push_back({lo, hi});
    num_queued_stores.fetch_add(1, std::memory_order_release);
    return true;
}

void BufferCache::ApplyQueuedStores() {
    if (num_queued_stores.load(std::memory_order_acquire) == 0) {
        return;
    }
    // The cache owner only: the GPU command thread, or the draw recording thread while packets are in flight.
    if (std::this_thread::get_id() != liverpool->GetGpuCommandProcessorThread() &&
        !Vulkan::DrawPipe::OnStageB()) {
        return;
    }
    thread_local std::vector<QueuedStore> stores;
    {
        std::scoped_lock lk{queued_store_mutex};
        stores.swap(queued_stores);
        num_queued_stores.store(0, std::memory_order_relaxed);
    }
    // Stores sorted and merged, then one update per run of adjacent dwords that no newer GPU write holds (~3200
    // stores/s in races: one update each ended the rendering pass and checked barriers every time). The values are
    // read from guest memory now, so the order of the stores among themselves does not matter.
    std::sort(stores.begin(), stores.end(), [](const QueuedStore& a, const QueuedStore& b) { return a.lo < b.lo; });
    size_t merged = 0;
    for (size_t i = 0; i < stores.size(); ++i) {
        if (merged > 0 && stores[i].lo <= stores[merged - 1].hi) {
            stores[merged - 1].hi = std::max(stores[merged - 1].hi, stores[i].hi);
        } else {
            stores[merged++] = stores[i];
        }
    }
    stores.resize(merged);
    thread_local std::vector<u32> run;
    for (const auto& store : stores) {
        // A merged range can cross blocks: the arena is looked up per block.
        Buffer* arena = nullptr;
        u64 arena_block = ~0ull;
        VAddr run_lo = 0;
        run.clear();
        const auto flush = [&] {
            if (!run.empty()) {
                runtime.InlineBytes(arena, run_lo - arena->cpu_addr, run.data(), u32(run.size() * 4));
                run.clear();
            }
        };
        for (VAddr at = store.lo; at < store.hi; at += 4) {
            const u64 block = at >> block_shift;
            if (block != arena_block) {
                flush();
                arena_block = block;
                arena = const_cast<Buffer*>(GetArena(block, block));
            }
            if (!arena || at < arena->cpu_addr || at + 4 > arena->cpu_addr + arena->size_bytes) {
                flush();
                continue;
            }
            bool gpu = false;
            gpu_modified_ranges.ForEachInRange(at, 4, [&](VAddr, VAddr) { gpu = true; });
            if (gpu || run.size() == 16384) {
                flush();
                if (gpu) {
                    continue; // a GPU write recorded since then over this dword is newer: it stands
                }
            }
            if (run.empty()) {
                run_lo = at;
            }
            u32 value = 0;
            runtime_memory_read_backing(at, &value, 4); // the latest value the guest stored
            run.push_back(value);
        }
        flush();
    }
    stores.clear();
}

bool BufferCache::IsHostWrittenBuffer(const Buffer* buffer) const noexcept {
    return buffer == &stream_buffer;
}

std::pair<const Buffer*, u64> BufferCache::ObtainBuffer(VAddr device_addr, u32 size,
                                                        bool is_written, bool is_texel_buffer) {
    BB_PIPE_CHECK("BufferCache::ObtainBuffer");
    BbStats::CheckCeConflict(device_addr, size, "a buffer binding");
    const bool stats = BufferStatsEnabled();
    if (stats) {
        PrintBufferStats();
        NoteHotUse(device_addr, size);
    }
    // For read-only buffers use device local stream buffer to reduce renderpass breaks.
    if (!is_written && size <= STREAM_THRESHOLD && !IsRegionGpuModified(device_addr, size)) {
        if (stats) {
            auto& region = Stats().regions[RegionKey(device_addr)];
            ++region.stream_count;
            region.stream_bytes += size;
        }
        // bbport: the guest data is copied on a copy thread, started now; submission and
        // guest-visible fences wait for it (Scheduler::WaitHostCopies).
        if (!stream_buffer.mapped_data.empty() &&
            !BbToggle::Disabled(BbToggle::DeferredStreamCopies)) {
            if (const auto offset = stream_buffer.Reserve(size, instance.UniformMinAlignment())) {
                SmallGuestCopy({
                    .run = &RunGuestCopy,
                    .context = this,
                    .source = device_addr,
                    .destination = reinterpret_cast<u64>(stream_buffer.mapped_data.data() + *offset),
                    .size = size,
                    .extra = reinterpret_cast<u64>(static_cast<Buffer*>(&stream_buffer)),
                });
                return {&stream_buffer, *offset};
            }
        }
        const auto [data, offset] = stream_buffer.Map(size, instance.UniformMinAlignment());
        memory->CopySparseMemory(device_addr, data, size);
        stream_buffer.Commit();
        return {&stream_buffer, offset};
    }
    const u64 first_block = device_addr >> block_shift;
    const u64 last_block = (device_addr + size - 1) >> block_shift;
    const auto* arena = GetArena(first_block, last_block);
    EnsureResident(arena, first_block, last_block);
    const u64 uploaded_before = BbStats::buffer_upload_bytes.load(std::memory_order_relaxed);
    SynchronizeMemory(arena, device_addr, size, is_written, is_texel_buffer);
    if (stats) {
        auto& region = Stats().regions[RegionKey(device_addr)];
        ++region.arena_count;
        region.arena_bytes += size;
        region.upload_bytes +=
            BbStats::buffer_upload_bytes.load(std::memory_order_relaxed) - uploaded_before;
    }
    if (is_written) {
        gpu_modified_ranges.Add(device_addr, size);
        gpu_byte_map.Mark(device_addr, size); // after SynchronizeMemory bumped the window's write sequence
        StampGpuWrite(device_addr, size);
    }
    return {arena, arena->Offset(device_addr)};
}

std::pair<const Buffer*, u64> BufferCache::ObtainBufferForImage(VAddr device_addr, u32 size) {
    if (IsRegionGpuModified(device_addr, size)) {
        return ObtainBuffer(device_addr, size, false);
    }
    const auto staging = staging_pool.Request(size, VideoCore::MemoryType::HostUncached,
                                              instance.StorageMinAlignment());
    if (!BbToggle::Disabled(BbToggle::DeferredUploads)) {
        // bbport: texture data is copied on the copy threads (streaming: 100+ MB per frame);
        // the upload reads the staging only after submission, which waits for the copies.
        constexpr u64 Chunk = 1_MB;
        for (u64 offset = 0; offset < staging.size; offset += Chunk) {
            const BbCopy::Item item{
                .run = &RunGuestCopy,
                .context = this,
                .source = device_addr + offset,
                .destination = reinterpret_cast<u64>(staging.mapped + offset),
                .size = std::min(Chunk, staging.size - offset),
                .extra = reinterpret_cast<u64>(staging.buffer),
            };
            if (staging.size < Chunk) {
                SmallGuestCopy(item);
            } else {
                BbCopy::Async([item] { item.run(item); });
            }
        }
        return {staging.buffer, staging.offset};
    }
    memory->CopySparseMemory(device_addr, staging.mapped, staging.size);
    staging.Flush();
    return {staging.buffer, staging.offset};
}

bool BufferCache::IsRegionCpuModified(VAddr addr, size_t size) {
    return memory_tracker->IsRegionCpuModified(addr, size);
}

bool BufferCache::IsRegionGpuModified(VAddr addr, size_t size) {
    return memory_tracker->IsRegionGpuModified(addr, size);
}

void BufferCache::ProcessFaultBuffer() {
    fault_manager->ProcessFaultBuffer();
}

void BufferCache::SynchronizeDmaBuffers() {
    for (const auto& range : resident_ranges) {
        const u64 page = range.start >> (arena_page_bits - block_shift);
        const VAddr device_addr = range.start << block_shift;
        const u64 size = (range.end - range.start) << block_shift;
        SynchronizeMemory(address_space[page], device_addr, size, false, false);
    }
}

std::string BufferCache::DescribeFault(u64 gpu_address) {
    for (size_t i = 0; i < arenas.size(); ++i) {
        const auto& arena = arenas[i];
        const u64 base = arena.BufferDeviceAddress();
        if (gpu_address < base || gpu_address >= base + arena.SizeBytes()) {
            continue;
        }
        const u64 offset = gpu_address - base;
        const u64 guest = arena.cpu_addr + offset;
        const u64 block = guest >> block_shift;
        bool resident = false;
        resident_ranges.ForEachInRange(block, block + 1, [&](const Backing&) { resident = true; });
        return fmt::format("GPU: fault is in arena {} (of {}), arena offset 0x{:x}, guest address 0x{:x}, "
                           "block {}, resident {}, guard {}, aliased {}, resident ranges {}",
                           i, arenas.size(), offset, guest, block, resident ? "yes" : "no",
                           guard_memory ? "on" : "off", sparse_aliased_enabled ? "on" : "off",
                           resident_ranges.Size());
    }
    return fmt::format("GPU: fault address is in none of the {} arenas", arenas.size());
}

// Every block of a new arena starts bound to the zeroed guard block, so a shader that reads a block the
// game never made resident gets zeros like on NVIDIA instead of a page fault that loses the AMD device.
// The resident ranges are bound after this and replace it.
void BufferCache::GuardWholeArena(const Buffer* arena) {
    if (!guard_memory) {
        return;
    }
    auto* binds = BindsForArena(arena);
    binds->binds.reserve(binds->binds.size() + arena->SizeBytes() / block_size);
    for (u64 offset = 0; offset < arena->SizeBytes(); offset += block_size) {
        binds->binds.push_back(vk::SparseMemoryBind{
            .resourceOffset = offset,
            .size = block_size,
            .memory = guard_memory,
            .memoryOffset = 0,
        });
    }
}

const Buffer* BufferCache::GetArena(u64 first_block, u64 last_block) {
    const u64 first_page = first_block >> blocks_per_arena_page_shift;
    const u64 last_page = last_block >> blocks_per_arena_page_shift;
    ASSERT_MSG(last_page - first_page <= 1,
               "Buffer request cannot span more than two VA arena pages");

    const auto* first_arena = address_space[first_page];
    const auto* last_arena = address_space[last_page];
    if (first_arena == last_arena) {
        if (!first_arena) {
            const u64 base_block = Common::AlignDownPow2<u64>(first_block, blocks_per_arena_page);
            const u64 num_pages = last_page - first_page + 1;
            const auto* new_arena =
                &arenas.emplace_back(instance, base_block << block_shift,
                                     num_pages << arena_page_bits, MemoryType::Sparse);
            address_space[first_page] = new_arena;
            address_space[last_page] = new_arena;
            GuardWholeArena(new_arena);
        }
        return address_space[first_page];
    }

    LOG_WARNING(Render, "Migrating arena");

    const u64 first_addr = first_arena ? first_arena->cpu_addr : (first_page << arena_page_bits);
    const u64 first_size = first_arena ? first_arena->size_bytes : arena_page_size;
    const u64 last_size = last_arena ? last_arena->size_bytes : arena_page_size;

    const u64 base_block = first_addr >> block_shift;
    const u64 total_size = first_size + last_size;
    const u64 end_block = (first_addr + total_size) >> block_shift;
    auto* new_arena = &arenas.emplace_back(instance, first_addr, total_size, MemoryType::Sparse);
    GuardWholeArena(new_arena);
    auto* bind = BindsForArena(new_arena);
    resident_ranges.ForEachInRange(base_block, end_block, [&](const Backing& backing) {
        const u64 start = std::max(base_block, backing.start);
        const u64 end = std::min(end_block, backing.end);
        bind->binds.push_back(vk::SparseMemoryBind{
            .resourceOffset = (start - base_block) << block_shift,
            .size = (end - start) << block_shift,
            .memory = backing.memory,
            .memoryOffset = backing.offset + ((start - backing.start) << block_shift),
        });
    });

    u64 base_page = first_addr >> arena_page_bits;
    for (u32 page = 0; page < (first_size >> arena_page_bits); ++page) {
        address_space[base_page + page] = new_arena;
    }
    base_page = last_page;
    for (u32 page = 0; page < (last_size >> arena_page_bits); ++page) {
        address_space[base_page + page] = new_arena;
    }
    return new_arena;
}

void BufferCache::EnsureResident(const Buffer* arena, u64 first_block, u64 last_block) {
    u32 resident_blocks{};
    IntervalList bind_ranges;
    resident_ranges.ForEachGap(first_block, last_block + 1, [&](u64 start, u64 end) {
        resident_blocks += end - start;
        bind_ranges.Add({start, end});
    });

    if (bind_ranges.Empty()) {
        return;
    }
    BbStats::Timer timer{BbStats::t_resident};

    const vk::MemoryAllocateInfo alloc_info = {
        .allocationSize = resident_blocks << block_shift,
        .memoryTypeIndex = arena_memory_type_index,
    };
    auto alloc = instance.GetDevice().allocateMemory(alloc_info);
    if (alloc.result == vk::Result::eErrorOutOfDeviceMemory && spill_memory_type_index != ~0u) {
        static std::atomic<bool> told{false};
        if (!told.exchange(true)) {
            std::printf("GPU: the card's memory is full, buffer data goes to system memory. "
                        "The frame rate drops instead of the game ending.\n");
            std::fflush(stdout);
        }
        alloc = instance.GetDevice().allocateMemory({.allocationSize = alloc_info.allocationSize,
                                                    .memoryTypeIndex = spill_memory_type_index});
    }
    if (alloc.result != vk::Result::eSuccess) {
        // Out of room in system memory too. Leaving these pages unbound shows wrong pixels where the
        // game reads them; stopping the port is what the assert here used to do.
        static std::atomic<u32> failed{0};
        const u32 n = failed.fetch_add(1) + 1;
        if (n <= 3 || n % 512 == 0) {
            LOG_WARNING(Render_Vulkan,
                        "No memory for {} bytes of buffer data ({} failures so far); the pages stay empty",
                        alloc_info.allocationSize, n);
        }
        return;
    }
    const vk::DeviceMemory device_memory = *alloc;
    u8* mapped_base = nullptr;
    if (arena_host_visible) {
        const auto mapping = instance.GetDevice().mapMemory(device_memory, 0, vk::WholeSize);
        if (mapping.result == vk::Result::eSuccess) {
            mapped_base = static_cast<u8*>(mapping.value);
        }
    }

    boost::container::small_vector<vk::BufferCopy, 8> copies;
    const auto staging =
        staging_pool.Request(resident_blocks * sizeof(vk::DeviceAddress), MemoryType::HostUncached);

    u64 memory_offset{};
    ArenaBinds* binds = BindsForArena(arena);
    auto* bda_addrs = reinterpret_cast<vk::DeviceAddress*>(staging.mapped);
    u64 offset = staging.offset;
    for (const auto& range : bind_ranges) {
        Backing backing;
        backing.start = range.start;
        backing.end = range.end;
        backing.memory = device_memory;
        backing.offset = memory_offset;
        backing.base = mapped_base;
        resident_ranges.Add(backing);

        LOG_INFO(Render, "Making range start={}, end={} resident", backing.start, backing.end);

        const auto& bind = binds->binds.emplace_back(vk::SparseMemoryBind{
            .resourceOffset = (range.start << block_shift) - arena->cpu_addr,
            .size = (range.end - range.start) << block_shift,
            .memory = device_memory,
            .memoryOffset = memory_offset,
        });
        memory_offset += bind.size;

        for (u32 block = 0; block < bind.size; block += block_size) {
            *(bda_addrs++) = arena->BufferDeviceAddress() + bind.resourceOffset + block;
        }
        const u64 copy_size = (backing.end - backing.start) * sizeof(vk::DeviceAddress);
        copies.emplace_back(offset, backing.start * sizeof(vk::DeviceAddress), copy_size);
        offset += copy_size;
    }

    // AMD faults a shader that touches an unbound page, NVIDIA answers zero. The block just before
    // the first and just after the last of what the game asked for gets the zeroed guard page, so a
    // draw that steps one block over the end of its buffer keeps reading zeros instead of losing
    // the device.
    if (guard_memory) {
        const u64 arena_first = arena->cpu_addr >> block_shift;
        const u64 arena_last = (arena->cpu_addr + arena->SizeBytes() - 1) >> block_shift;
        auto guard = [&](u64 block) {
            if (block < arena_first || block > arena_last) {
                return;
            }
            bool resident = false;
            resident_ranges.ForEachInRange(block, block + 1, [&](const Backing&) { resident = true; });
            if (resident) {
                return;
            }
            binds->binds.push_back(vk::SparseMemoryBind{
                .resourceOffset = (block << block_shift) - arena->cpu_addr,
                .size = block_size,
                .memory = guard_memory,
                .memoryOffset = 0,
            });
        };
        if (first_block > 0) {
            guard(first_block - 1);
        }
        guard(last_block + 1);
    }

    staging.Flush();
    runtime.CopyBuffer(staging.buffer, bda_pagetable_buffer.get(), copies);
}

bool BufferCache::SynchronizeMemory(const Buffer* arena, VAddr device_addr, u32 size,
                                    bool is_written, bool is_texel_buffer) {
    boost::container::small_vector<vk::BufferCopy, 4> copies;
    size_t total_size_bytes{};
    const Buffer* src_buffer{};
    memory_tracker->ForEachUploadRange(
        device_addr, size, is_written,
        [&](u64 addr, u64 size) {
            copies.emplace_back(total_size_bytes, addr, size);
            total_size_bytes += size;
        },
        [&] { src_buffer = UploadCopies(arena, copies, total_size_bytes); });

    if (src_buffer) {
        runtime.CopyBuffer(src_buffer, arena, copies);
    }
    if (is_texel_buffer && !is_written) {
        return SynchronizeMemoryFromImage(arena, device_addr, size);
    }
    return false;
}

const Buffer* BufferCache::UploadCopies(const Buffer* arena, std::span<vk::BufferCopy> copies,
                                        size_t total_size_bytes) {
    if (copies.empty()) {
        return nullptr;
    }
    BbStats::buffer_upload_bytes.fetch_add(total_size_bytes, std::memory_order_relaxed);
    const auto staging = staging_pool.Request(total_size_bytes, MemoryType::HostUncached);
    // bbport: the guest memory is copied into staging on the copy threads, started now in
    // groups of about 1 MiB; submission and guest-visible fences wait for them
    // (Scheduler::WaitHostCopies).
    if (!BbToggle::Disabled(BbToggle::DeferredUploads) && total_size_bytes < 1_MB) {
        // Small uploads join the calling thread's batch.
        for (auto& copy : copies) {
            SmallGuestCopy({
                .run = &RunGuestCopy,
                .context = this,
                .source = copy.dstOffset,
                .destination = reinterpret_cast<u64>(staging.mapped + copy.srcOffset),
                .size = copy.size,
                .extra = reinterpret_cast<u64>(staging.buffer),
            });
            copy.srcOffset += staging.offset;
            copy.dstOffset -= arena->cpu_addr;
        }
        return staging.buffer;
    }
    if (!BbToggle::Disabled(BbToggle::DeferredUploads)) {
        struct HostCopy {
            VAddr source;
            u8* destination;
            u64 size;
        };
        using Group = boost::container::small_vector<HostCopy, 8>;
        auto group = std::make_shared<Group>();
        u64 group_bytes = 0;
        const auto launch = [&] {
            BbCopy::Async([group, memory = memory, staging] {
                for (const auto& copy : *group) {
                    memory->CopySparseMemory(copy.source, copy.destination, copy.size);
                }
                staging.Flush();
            });
        };
        for (auto& copy : copies) {
            group->push_back({copy.dstOffset, staging.mapped + copy.srcOffset, copy.size});
            group_bytes += copy.size;
            copy.srcOffset += staging.offset;
            copy.dstOffset -= arena->cpu_addr;
            if (group_bytes >= 1_MB) {
                launch();
                group = std::make_shared<Group>();
                group_bytes = 0;
            }
        }
        if (!group->empty()) {
            launch();
        }
        return staging.buffer;
    }
    const auto copy = [&](std::size_t i) {
        memory->CopySparseMemory(copies[i].dstOffset, staging.mapped + copies[i].srcOffset,
                                 copies[i].size);
    };
    if (total_size_bytes >= 2_MB && copies.size() > 1) {
        BbCopy::ParallelFor(copies.size(), copy);
    } else {
        for (std::size_t i = 0; i < copies.size(); ++i) {
            copy(i);
        }
    }
    for (auto& copy : copies) {
        copy.srcOffset += staging.offset;
        copy.dstOffset -= arena->cpu_addr;
    }
    staging.Flush();
    return staging.buffer;
}

bool BufferCache::SynchronizeMemoryFromImage(const Buffer* arena, VAddr device_addr, u32 size) {
    if (auto type = texture_cache.IsMeta(device_addr)) {
        if (*type == TextureCache::MetaType::HTile) {
            static constexpr u32 ZmaskUncompressed = 0xf;
            runtime.FillBuffer(arena, arena->Offset(device_addr), size, ZmaskUncompressed);
            return true;
        } else {
            LOG_WARNING(Render_Vulkan, "Unhandled metadata type {}", magic_enum::enum_name(*type));
        }
    }
    // bbport: most texel buffers alias no image; remember misses until images change.
    const u64 generation = texture_cache.RegistryGeneration();
    auto& miss = image_miss_cache[((device_addr >> 6) ^ size * 0x9E3779B1u) % image_miss_cache.size()];
    if (miss.address == device_addr && miss.size == size && miss.generation == generation &&
        !BbToggle::Disabled(BbToggle::TextureBindingMemo)) {
        return false;
    }
    const ImageId image_id = texture_cache.FindImageFromRange(device_addr, size);
    if (!image_id) {
        miss = {device_addr, size, generation};
        return false;
    }
    // bbport: the lookups above only read what the texture binding helper leaves alone; the
    // copy below changes image state, so the helper finishes first.
    runtime.BeforeImageAccess();
    Image& image = texture_cache.GetImage(image_id);
    ASSERT_MSG(device_addr == image.info.guest_address,
               "Texel buffer aliases image subresources {:x} : {:x}", device_addr,
               image.info.guest_address);
    const u64 arena_offset = arena->Offset(device_addr);
    boost::container::small_vector<vk::BufferImageCopy, 8> buffer_copies;
    for (u32 mip = 0; mip < image.info.resources.levels; mip++) {
        const auto& mip_info = image.info.mips_layout[mip];
        const u32 width = std::max(image.info.size.width >> mip, 1u);
        const u32 height = std::max(image.info.size.height >> mip, 1u);
        const u32 depth = std::max(image.info.size.depth >> mip, 1u);
        if (arena_offset + mip_info.offset + mip_info.size > arena->size_bytes) {
            break;
        }
        buffer_copies.push_back(vk::BufferImageCopy{
            .bufferOffset = mip_info.offset,
            .bufferRowLength = mip_info.pitch,
            .bufferImageHeight = mip_info.height,
            .imageSubresource{
                .aspectMask = image.aspect_mask & ~vk::ImageAspectFlagBits::eStencil,
                .mipLevel = mip,
                .baseArrayLayer = 0,
                .layerCount = image.info.resources.layers,
            },
            .imageOffset = {0, 0, 0},
            .imageExtent = {width, height, depth},
        });
    }
    if (buffer_copies.empty()) {
        return false;
    }
    auto& tile_manager = texture_cache.GetTileManager();
    tile_manager.TileImage(image, buffer_copies, arena, arena_offset);
    return true;
}

void BufferCache::SubmitPendingArenaBinds(Vulkan::SubmitInfo& info) {
    if (pending_binds.empty()) {
        return;
    }

    std::vector<vk::SparseBufferMemoryBindInfo> buffer_binds;
    buffer_binds.reserve(pending_binds.size());

    for (const auto& binds : pending_binds) {
        buffer_binds.emplace_back(vk::SparseBufferMemoryBindInfo{
            .buffer = binds.arena->Handle(),
            .bindCount = static_cast<u32>(binds.binds.size()),
            .pBinds = binds.binds.data(),
        });
    }

    const u64 signal_tick = memory_semaphore.NextTick();
    const auto signal_sema = memory_semaphore.Handle();

    const vk::TimelineSemaphoreSubmitInfo timeline_si = {
        .signalSemaphoreValueCount = 1u,
        .pSignalSemaphoreValues = &signal_tick,
    };

    const vk::BindSparseInfo sparse_info = {
        .pNext = &timeline_si,
        .bufferBindCount = static_cast<u32>(buffer_binds.size()),
        .pBufferBinds = buffer_binds.data(),
        .signalSemaphoreCount = 1u,
        .pSignalSemaphores = &signal_sema,
    };

    info.AddWait(signal_sema, signal_tick);
    auto submit_result = instance.GetGraphicsQueue().bindSparse(sparse_info);
    if (std::getenv("BB_WINDOW_BIND_DEBUG")) {
        std::printf("GPU: bindSparse of %zu arenas, result %d\n", buffer_binds.size(),
                    static_cast<int>(submit_result));
        for (const auto& binds : pending_binds) {
            for (const auto& bind : binds.binds) {
                if (bind.size >= READBACK_WINDOW_SIZE) {
                    std::printf("GPU:   %p offset %llx size %llx memory %p + %llx\n",
                                static_cast<const void*>(binds.arena->Handle()),
                                static_cast<unsigned long long>(bind.resourceOffset),
                                static_cast<unsigned long long>(bind.size),
                                static_cast<const void*>(bind.memory),
                                static_cast<unsigned long long>(bind.memoryOffset));
                }
            }
        }
        std::fflush(stdout);
    }
    if (submit_result == vk::Result::eErrorDeviceLost) {
        instance.LogDeviceLost();
    }
    ASSERT_MSG(submit_result != vk::Result::eErrorDeviceLost, "Device lost during submit");

    pending_binds.clear();
}

} // namespace VideoCore



