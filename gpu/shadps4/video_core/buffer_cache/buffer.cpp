#include "../../../shim/bbport_diagnostics.h"
// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <atomic>
#include <cstdlib>
#include <string_view>
#include <numeric>

#include "bbport_toggles.h"

#include "common/alignment.h"
#include "common/assert.h"
#include "video_core/buffer_cache/buffer.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_platform.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"

#include <vk_mem_alloc.h>

namespace VideoCore {

std::string_view BufferTypeName(MemoryType type) {
    switch (type) {
    case MemoryType::HostUncached:
        return "HostUncached";
    case MemoryType::HostCached:
        return "HostCached";
    case MemoryType::Stream:
        return "Stream";
    case MemoryType::StreamRing:
        return "StreamRing";
    case MemoryType::Gds:
        return "Gds";
    case MemoryType::DeviceLocal:
        return "DeviceLocal";
    case MemoryType::Sparse:
        return "Sparse";
    default:
        return "Invalid";
    }
}

/// bbport: where the stream buffer lives (BB_STREAM_MEMORY=device|host|cached). Every draw
/// copies its small read-only buffers from guest memory into it on the Vulkan recording thread.
/// "device" (shadPS4's choice) is VRAM written through the PCIe BAR: cheap with AMD Smart Access
/// Memory on a desktop, but on a laptop NVIDIA GPU these writes took half of that thread's time
/// on laptops measured so. "cached" is cached system memory the GPU reads over PCIe
/// instead; "host" is uncached system memory. The default is device on AMD, and on NVIDIA when
/// the game renders at 1080p or more; cached otherwise (SetStreamPlacementDefault).
enum class StreamPlacement { Device, Host, Cached };
static std::atomic<int> stream_default{int(StreamPlacement::Device)};
static std::atomic<int> planned_render_height{0};
static StreamPlacement StreamMemory() {
    static const StreamPlacement placement = [] {
        const char* env = std::getenv("BB_STREAM_MEMORY");
        if (!env) {
            return StreamPlacement(stream_default.load());
        }
        const std::string_view value = env;
        if (value == "device") {
            return StreamPlacement::Device;
        }
        return value == "host" ? StreamPlacement::Host : StreamPlacement::Cached;
    }();
    static const bool reported = [] {
        std::printf("GPU: stream buffer in %s memory\n",
                    placement == StreamPlacement::Device ? "device" : placement == StreamPlacement::Host ? "host" : "cached host");
        return true;
    }();
    (void)reported;
    return placement;
}

// bbport: BB_RING_MEMORY=device|host|cached: the constant ring (stage A's copies of small read-only guest buffers,
// read by every draw's shaders) placed apart from the stream buffer. Default: as the stream buffer (cached system
// memory on NVIDIA). On an RTX 3070 Laptop (720p render, 4K output, uncapped) the ring in VRAM gives much
// steadier frames (1% low 55 -> 65, p99 16 -> 13 ms, jitter 2.0 -> 0.65) but ~7% fewer FPS: stage A, the limit,
// writes 7-9 MB a frame through the BAR (+0.4 ms), and the GPU saves only ~0.6 ms a frame of constant reads.
static StreamPlacement RingMemory() {
    static const StreamPlacement placement = [] {
        const char* env = std::getenv("BB_RING_MEMORY");
        if (!env) {
            return StreamMemory();
        }
        const std::string_view value = env;
        if (value == "device") {
            return StreamPlacement::Device;
        }
        return value == "host" ? StreamPlacement::Host : StreamPlacement::Cached;
    }();
    static const bool reported = [] {
        std::printf("GPU: constant ring in %s memory\n",
                    placement == StreamPlacement::Device ? "device" : placement == StreamPlacement::Host ? "host" : "cached host");
        return true;
    }();
    (void)reported;
    return placement;
}
static bool IsStream(MemoryType type) {
    return type == MemoryType::Stream || type == MemoryType::StreamRing;
}
static StreamPlacement PlacementOf(MemoryType type) {
    return type == MemoryType::StreamRing ? RingMemory() : StreamMemory();
}

void SetStreamPlacementRenderHeight(s32 height) {
    planned_render_height = height;
}

s32 PlannedRenderHeight() {
    return planned_render_height.load();
}

void SetStreamPlacementDefault(u32 vendor_id) {
    // AMD keeps VRAM (Smart Access Memory). NVIDIA 0x10DE: VRAM from 1080p render up, where the GPU is the limit:
    // 1440p uncapped 66.7 -> 79.1 FPS, sd 6.2 -> 2.7 ms, the 13/22 ms alternation nearly gone; capped 60 1% low
    // 48.2 -> 56.3; 1080p render / 4K output 93.9 -> 99.0, 1% low 51.1 -> 57.1. At 720p
    // render the draw recording thread is the limit and its BAR writes cost ~7%: cached system
    // memory there. Intel 0x8086: cached.
    const bool device = vendor_id == 0x1002 || (vendor_id == 0x10DE && planned_render_height.load() >= 1080);
    stream_default = int(device ? StreamPlacement::Device : StreamPlacement::Cached);
}

// bbport: GDS gets GPU atomics and occasional CPU reads (ReadDataFromGds reads the mapping without an
// invalidate), so it stays in mapped coherent memory, cached as with the stream buffer's cached placement,
// whatever the stream buffer's placement (VRAM from 1080p render on NVIDIA).
[[nodiscard]] VkMemoryPropertyFlags MemoryUsagePreferredVmaFlags(MemoryType type) {
    if (type == MemoryType::Gds || (IsStream(type) && PlacementOf(type) == StreamPlacement::Cached)) {
        return VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
    }
    return type != MemoryType::DeviceLocal ? VK_MEMORY_PROPERTY_HOST_COHERENT_BIT
                                           : VkMemoryPropertyFlagBits{};
}

[[nodiscard]] VmaAllocationCreateFlags MemoryUsageVmaFlags(MemoryType type) {
    if (IsStream(type) && PlacementOf(type) == StreamPlacement::Cached) {
        return VMA_ALLOCATION_CREATE_MAPPED_BIT | VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT;
    }
    switch (type) {
    case MemoryType::HostUncached:
    case MemoryType::Stream:
    case MemoryType::StreamRing:
        return VMA_ALLOCATION_CREATE_MAPPED_BIT |
               VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT;
    case MemoryType::HostCached:
    case MemoryType::Gds:
        return VMA_ALLOCATION_CREATE_MAPPED_BIT | VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT;
    case MemoryType::DeviceLocal:
    default:
        return {};
    }
}

[[nodiscard]] VmaMemoryUsage MemoryUsageVma(MemoryType type) {
    if (IsStream(type) && PlacementOf(type) != StreamPlacement::Device) {
        return VMA_MEMORY_USAGE_AUTO_PREFER_HOST;
    }
    switch (type) {
    case MemoryType::DeviceLocal:
    case MemoryType::Stream:
    case MemoryType::StreamRing:
        return VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    case MemoryType::HostUncached:
    case MemoryType::HostCached:
    case MemoryType::Gds:
        return VMA_MEMORY_USAGE_AUTO_PREFER_HOST;
    default:
        return VMA_MEMORY_USAGE_UNKNOWN;
    }
}

UniqueBuffer::UniqueBuffer(vk::Device device_, VmaAllocator allocator_)
    : device{device_}, allocator{allocator_} {}

UniqueBuffer::~UniqueBuffer() {
    Destroy();
}

void UniqueBuffer::Destroy() {
    if (allocation) {
        vmaDestroyBuffer(allocator, buffer, allocation);
    } else if (buffer) {
        device.destroyBuffer(buffer);
    }
    buffer = VK_NULL_HANDLE;
    allocation = VK_NULL_HANDLE;
    bda_addr = 0;
}

void UniqueBuffer::Create(vk::BufferCreateInfo& buffer_ci, MemoryType mem_type,
                          VmaAllocationInfo* out_alloc_info) {
    const bool with_bda = bool(buffer_ci.usage & vk::BufferUsageFlagBits::eShaderDeviceAddress);
    if (mem_type != MemoryType::Sparse) {
        const VmaAllocationCreateFlags bda_flag =
            with_bda ? VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT : 0;
        const VmaAllocationCreateInfo alloc_ci = {
            .flags =
                VMA_ALLOCATION_CREATE_WITHIN_BUDGET_BIT | bda_flag | MemoryUsageVmaFlags(mem_type),
            .usage = MemoryUsageVma(mem_type),
            .requiredFlags = mem_type == MemoryType::Gds
                                 ? VkMemoryPropertyFlags(VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                         VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)
                                 : VkMemoryPropertyFlags(0),
            .preferredFlags = MemoryUsagePreferredVmaFlags(mem_type),
            .pool = VK_NULL_HANDLE,
            .pUserData = nullptr,
        };

        const VkBufferCreateInfo buffer_ci_unsafe = static_cast<VkBufferCreateInfo>(buffer_ci);
        VkBuffer unsafe_buffer{};
        VkResult result = vmaCreateBuffer(allocator, &buffer_ci_unsafe, &alloc_ci, &unsafe_buffer,
                                          &allocation, out_alloc_info);
        if (result == VK_ERROR_OUT_OF_DEVICE_MEMORY) {
            // The budget shrinks as other programs take video memory; past it the driver moves
            // memory out to system memory instead of the game ending.
            LOG_WARNING(Render_Vulkan, "Allocating a buffer of {} bytes went over the memory budget, allocating it past the budget",
                        buffer_ci.size);
            VmaAllocationCreateInfo over_budget_ci = alloc_ci;
            over_budget_ci.flags &= ~VMA_ALLOCATION_CREATE_WITHIN_BUDGET_BIT;
            result = vmaCreateBuffer(allocator, &buffer_ci_unsafe, &over_budget_ci, &unsafe_buffer,
                                     &allocation, out_alloc_info);
            if (result == VK_ERROR_OUT_OF_DEVICE_MEMORY) {
                // A card that is full (a 4 GB card with the texture cache at its limit): the buffer goes to
                // system memory, where the card reaches it over the bus, instead of the game ending.
                LOG_WARNING(Render_Vulkan, "The card's memory is full: a buffer of {} bytes is placed in system memory",
                            buffer_ci.size);
                VmaAllocationCreateInfo host_ci = over_budget_ci;
                host_ci.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST;
                host_ci.preferredFlags = 0;
                result = vmaCreateBuffer(allocator, &buffer_ci_unsafe, &host_ci, &unsafe_buffer,
                                         &allocation, out_alloc_info);
            }
        }
        if constexpr (BB_DIAGNOSTICS) BbStats::alloc_count[0].fetch_add(1, std::memory_order_relaxed);
        if constexpr (BB_DIAGNOSTICS) BbStats::alloc_bytes[0].fetch_add(buffer_ci_unsafe.size, std::memory_order_relaxed);
        ASSERT_MSG(result == VK_SUCCESS, "Failed allocating buffer with error {}",
                   vk::to_string(vk::Result{result}));
        buffer = vk::Buffer{unsafe_buffer};
    } else {
        buffer_ci.flags |=
            vk::BufferCreateFlagBits::eSparseBinding | vk::BufferCreateFlagBits::eSparseResidency;
        if (sparse_aliased_enabled) {
            buffer_ci.flags |= vk::BufferCreateFlagBits::eSparseAliased;
        }
        buffer = Vulkan::Check(device.createBuffer(buffer_ci));
    }

    if (with_bda) {
        const vk::BufferDeviceAddressInfo bda_info = {
            .buffer = buffer,
        };
        auto bda_result = device.getBufferAddress(bda_info);
        ASSERT_MSG(bda_result != 0, "Failed to get buffer device address");
        bda_addr = bda_result;
    }
}

Buffer::Buffer(const Vulkan::Instance& instance, VAddr cpu_addr_, u64 size_bytes_,
               MemoryType mem_type_, std::string_view debug_name)
    : cpu_addr{cpu_addr_}, size_bytes{size_bytes_}, mem_type{mem_type_},
      buffer{instance.GetDevice(), instance.GetAllocator()} {

    vk::BufferCreateInfo buffer_ci = {
        .size = size_bytes,
        .usage = AllFlags,
        .sharingMode = vk::SharingMode::eExclusive,
    };
    VmaAllocationInfo alloc_info{};
    buffer.Create(buffer_ci, mem_type, &alloc_info);

    const auto device = instance.GetDevice();
    if (!debug_name.empty()) {
        Vulkan::SetObjectName(device, Handle(), debug_name);
    } else {
        Vulkan::SetObjectName(device, Handle(), "Buffer {:#x}:{:#x}", cpu_addr, size_bytes);
    }

    if (mem_type != MemoryType::Sparse) {
        VkMemoryPropertyFlags property_flags{};
        vmaGetAllocationMemoryProperties(instance.GetAllocator(), buffer.allocation,
                                         &property_flags);
        if (alloc_info.pMappedData) {
            mapped_data = std::span<u8>{std::bit_cast<u8*>(alloc_info.pMappedData), size_bytes};
        }
        is_coherent = property_flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        is_host_cached = property_flags & VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
        ASSERT_MSG(mem_type != MemoryType::Gds || (!mapped_data.empty() && is_coherent),
                   "GDS requires a coherent CPU mapping");
    }
}

void Buffer::Flush(u64 offset, u64 size) {
    if (mapped_data.empty() || is_coherent) {
        return;
    }
    vmaFlushAllocation(buffer.allocator, buffer.allocation, offset, size);
}

void Buffer::Invalidate(u64 offset, u64 size) {
    if (mapped_data.empty() || is_coherent) {
        return;
    }
    vmaInvalidateAllocation(buffer.allocator, buffer.allocation, offset, size);
}

constexpr u64 WATCHES_INITIAL_RESERVE = 0x100;
constexpr u64 WATCHES_RESERVE_CHUNK = 0x100;

StreamBuffer::StreamBuffer(const Vulkan::Instance& instance, Vulkan::Scheduler& scheduler_,
                           MemoryType mem_type, u64 size_bytes)
    : Buffer{instance, 0, size_bytes, mem_type}, scheduler{scheduler_},
      non_coherent_atom_size{instance.NonCoherentAtomSize()} {
    ReserveWatches(current_watches, WATCHES_INITIAL_RESERVE);
    ReserveWatches(previous_watches, WATCHES_INITIAL_RESERVE);
    Vulkan::SetObjectName(instance.GetDevice(), Handle(), "StreamBuffer({}):{:#x}",
                          BufferTypeName(mem_type), size_bytes);
}

bool StreamBuffer::PrepareMap(u64 size, u64 alignment, bool allow_wait) {
    if (!mapped_data.empty() && !is_coherent) {
        size = Common::AlignUp(size, non_coherent_atom_size);
        alignment =
            alignment > 0 ? std::lcm(alignment, non_coherent_atom_size) : non_coherent_atom_size;
    }

    if (size > this->size_bytes) {
        return false;
    }

    if (alignment > 0) {
        offset = Common::AlignUp(offset, alignment);
    }

    if (offset + size > this->size_bytes) {
        // The buffer would overflow, save the amount of used watches and reset the state.
        invalidation_mark = current_watch_cursor;
        current_watch_cursor = 0;
        offset = 0;

        // Swap watches and reset waiting cursors.
        std::swap(previous_watches, current_watches);
        wait_cursor = 0;
        wait_bound = 0;
    }

    if (!WaitPendingOperations(offset + size, allow_wait)) {
        return false;
    }

    mapped_size = size;
    return true;
}

std::pair<u8*, u64> StreamBuffer::Map(u64 size, u64 alignment, bool allow_wait) {
    if (!PrepareMap(size, alignment, allow_wait)) {
        return {nullptr, 0};
    }
    u8* const data = mapped_data.empty() ? nullptr : mapped_data.data() + offset;
    return {data, offset};
}

void StreamBuffer::Commit() {
    if (mem_type == MemoryType::HostCached) {
        Invalidate(offset, mapped_size);
    } else {
        Flush(offset, mapped_size);
    }
    AdvanceAndWatch();
}

std::optional<u64> StreamBuffer::Reserve(u64 size, u64 alignment, bool allow_wait) {
    if (!PrepareMap(size, alignment, allow_wait)) {
        return std::nullopt;
    }
    const u64 reserved_offset = offset;
    AdvanceAndWatch();
    return reserved_offset;
}

void StreamBuffer::AdvanceAndWatch() {
    offset += mapped_size;
    const u64 tick = scheduler.CurrentTick();
    last_tick = tick;

    // Extend the last watch if it belongs to the same tick.
    if (current_watch_cursor != 0 && current_watches[current_watch_cursor - 1].tick == tick) {
        current_watches[current_watch_cursor - 1].upper_bound = offset;
        return;
    }

    if (current_watch_cursor + 1 >= current_watches.size()) {
        // Ensure that there are enough watches.
        ReserveWatches(current_watches, WATCHES_RESERVE_CHUNK);
    }

    auto& watch = current_watches[current_watch_cursor++];
    watch.upper_bound = offset;
    watch.tick = tick;
}

void StreamBuffer::ReserveWatches(std::vector<Watch>& watches, std::size_t grow_size) {
    watches.resize(watches.size() + grow_size);
}

bool StreamBuffer::WaitPendingOperations(u64 requested_upper_bound, bool allow_wait) {
    if (!invalidation_mark) {
        return true;
    }
    while (requested_upper_bound > wait_bound && wait_cursor < *invalidation_mark) {
        auto& watch = previous_watches[wait_cursor];
        if (!scheduler.IsFree(watch.tick) && !allow_wait) {
            return false;
        }
        scheduler.Wait(watch.tick, 1);
        wait_bound = watch.upper_bound;
        ++wait_cursor;
    }
    return true;
}

} // namespace VideoCore
