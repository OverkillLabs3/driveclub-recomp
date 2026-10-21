// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Changed for this port. The upstream sources of the vendored tree are listed in
// gpu/VENDOR.txt and in NOTICE.
//
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <mutex>
#include <vector>
#include "bbport_toggles.h"
#include <ranges>
#include "common/assert.h"
#include "common/perf_stats.h"
#include "common/scope_exit.h"
#include "video_core/renderer_vulkan/liverpool_to_vk.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_runtime.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/texture_cache/blit_helper.h"
#include "video_core/texture_cache/image.h"

#include <vk_mem_alloc.h>

namespace VideoCore {

using namespace Vulkan;

Common::IncrementalIdProvider<u64> Image::global_image_uid{};

static vk::ImageUsageFlags ImageUsageFlags(const Vulkan::Instance& instance,
                                           const ImageInfo& info) {
    vk::ImageUsageFlags usage = vk::ImageUsageFlagBits::eTransferSrc |
                                vk::ImageUsageFlagBits::eTransferDst |
                                vk::ImageUsageFlagBits::eSampled;
    if (!info.props.is_block) {
        if (info.props.is_depth) {
            usage |= vk::ImageUsageFlagBits::eDepthStencilAttachment;
        } else {
            usage |= vk::ImageUsageFlagBits::eColorAttachment;
            if (instance.IsAttachmentFeedbackLoopLayoutSupported()) {
                usage |= vk::ImageUsageFlagBits::eAttachmentFeedbackLoopEXT;
            }
            // Always create images with storage flag to avoid needing re-creation in case of e.g
            // compute clears This sacrifices a bit of performance but is less work. ExtendedUsage
            // flag is also used.
            usage |= vk::ImageUsageFlagBits::eStorage;
        }
    } else {
        // Similarly to above, we specify storage usage. This is typically not supported by
        // compressed formats, but may be used for uncompressed views. In order to satisfy this,
        // we will also specify the extended usage bit.
        usage |= vk::ImageUsageFlagBits::eStorage;
    }

    return usage;
}

static vk::ImageType ConvertImageType(AmdGpu::ImageType type) noexcept {
    switch (type) {
    case AmdGpu::ImageType::Color1D:
    case AmdGpu::ImageType::Color1DArray:
        return vk::ImageType::e1D;
    case AmdGpu::ImageType::Color2D:
    case AmdGpu::ImageType::Color2DMsaa:
    case AmdGpu::ImageType::Color2DArray:
        return vk::ImageType::e2D;
    case AmdGpu::ImageType::Color3D:
        return vk::ImageType::e3D;
    default:
        UNREACHABLE();
    }
}

static vk::FormatFeatureFlags2 FormatFeatureFlags(const vk::ImageUsageFlags usage_flags) {
    vk::FormatFeatureFlags2 feature_flags{};
    if (usage_flags & vk::ImageUsageFlagBits::eTransferSrc) {
        feature_flags |= vk::FormatFeatureFlagBits2::eTransferSrc;
    }
    if (usage_flags & vk::ImageUsageFlagBits::eTransferDst) {
        feature_flags |= vk::FormatFeatureFlagBits2::eTransferDst;
    }
    if (usage_flags & vk::ImageUsageFlagBits::eSampled) {
        feature_flags |= vk::FormatFeatureFlagBits2::eSampledImage;
    }
    if (usage_flags & vk::ImageUsageFlagBits::eColorAttachment) {
        feature_flags |= vk::FormatFeatureFlagBits2::eColorAttachment;
    }
    if (usage_flags & vk::ImageUsageFlagBits::eDepthStencilAttachment) {
        feature_flags |= vk::FormatFeatureFlagBits2::eDepthStencilAttachment;
    }
    // Note: StorageImage is intentionally ignored for now since it is always set, and can mess up
    // compatibility checks.
    return feature_flags;
}

namespace {
// Device images of destroyed cache images, kept for the next image of the same shape. The game
// makes and drops hundreds of full-size render targets a second; allocating and freeing device
// memory for each was a large share of the GPU thread's time. Images reach Destroy() only after
// the GPU is done with them, and a new image starts in the undefined layout, so contents and
// layout of a recycled one do not matter. BB_IMAGE_POOL_MB sets the cap (0: off).
struct PooledImage {
    VmaAllocator allocator;
    VkImage image;
    VmaAllocation allocation;
    vk::DeviceSize size_bytes;
    vk::ImageCreateInfo ci;
    std::chrono::steady_clock::time_point since;
};

std::mutex pool_mutex;
std::vector<PooledImage> pool;
u64 pool_bytes = 0;

u64 PoolCapBytes(VmaAllocator allocator) {
    static const u64 cap = [allocator] {
        if (const char* env = std::getenv("BB_IMAGE_POOL_MB")) {
            return static_cast<u64>(std::max(0, std::atoi(env))) << 20;
        }
        // A sixteenth of the device-local memory: 1 GiB at 16 GiB, 256 MiB at 4 GiB.
        VmaBudget budgets[VK_MAX_MEMORY_HEAPS]{};
        vmaGetHeapBudgets(allocator, budgets);
        const VkPhysicalDeviceMemoryProperties* props{};
        vmaGetMemoryProperties(allocator, &props);
        u64 local = 0;
        for (u32 i = 0; i < props->memoryHeapCount; ++i) {
            if (props->memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) {
                local += props->memoryHeaps[i].size;
            }
        }
        return std::clamp<u64>(local / 16, 128ull << 20, 1024ull << 20);
    }();
    return cap;
}

bool SameShape(const vk::ImageCreateInfo& a, const vk::ImageCreateInfo& b) {
    return a.flags == b.flags && a.imageType == b.imageType && a.format == b.format &&
           a.extent == b.extent && a.mipLevels == b.mipLevels &&
           a.arrayLayers == b.arrayLayers && a.samples == b.samples && a.tiling == b.tiling &&
           a.usage == b.usage && a.sharingMode == b.sharingMode &&
           a.initialLayout == b.initialLayout;
}

void PurgePool(VmaAllocator allocator, bool all) {
    const auto now = std::chrono::steady_clock::now();
    for (size_t i = 0; i < pool.size();) {
        if (all || now - pool[i].since > std::chrono::seconds(4)) {
            vmaDestroyImage(pool[i].allocator, pool[i].image, pool[i].allocation);
            pool_bytes -= pool[i].size_bytes;
            pool[i] = pool.back();
            pool.pop_back();
        } else {
            ++i;
        }
    }
}
} // namespace

UniqueImage::~UniqueImage() {
    Destroy();
}

void UniqueImage::Destroy() {
    if (!image) {
        return;
    }
    // Small images are cheap to allocate (the allocator suballocates them): only the big ones pool.
    if (size_bytes >= (512u << 10) && image_ci.pNext == nullptr) {
        std::scoped_lock lk{pool_mutex};
        PurgePool(allocator, false);
        if (const u64 cap = PoolCapBytes(allocator); cap && pool_bytes + size_bytes <= cap) {
            pool.push_back({allocator, static_cast<VkImage>(image), allocation, size_bytes, image_ci,
                            std::chrono::steady_clock::now()});
            pool_bytes += size_bytes;
            image = vk::Image{};
            allocation = {};
            return;
        }
    }
    vmaDestroyImage(allocator, image, allocation);
    image = vk::Image{};
    allocation = {};
}

void UniqueImage::Create(const vk::ImageCreateInfo& image_ci) {
    this->image_ci = image_ci;
    ASSERT(!image);
    if (image_ci.pNext == nullptr) {
        std::scoped_lock lk{pool_mutex};
        for (size_t i = 0; i < pool.size(); ++i) {
            if (pool[i].allocator == allocator && SameShape(pool[i].ci, image_ci)) {
                image = vk::Image{pool[i].image};
                allocation = pool[i].allocation;
                size_bytes = pool[i].size_bytes;
                pool_bytes -= pool[i].size_bytes;
                pool[i] = pool.back();
                pool.pop_back();
                BbStats::alloc_count[3].fetch_add(1, std::memory_order_relaxed);
                return;
            }
        }
    }
    const VmaAllocationCreateInfo alloc_ci = {
        .flags = VMA_ALLOCATION_CREATE_WITHIN_BUDGET_BIT,
        .usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
        .requiredFlags = 0,
        .preferredFlags = 0,
        .pool = VK_NULL_HANDLE,
        .pUserData = nullptr,
    };

    const VkImageCreateInfo image_ci_unsafe = static_cast<VkImageCreateInfo>(image_ci);
    VkImage unsafe_image{};
    VmaAllocationInfo alloc_info{};
    VkResult result = vmaCreateImage(allocator, &image_ci_unsafe, &alloc_ci, &unsafe_image,
                                     &allocation, &alloc_info);
    if (result != VK_SUCCESS) {
        // Out of device memory: give back what the pool holds and try again.
        {
            std::scoped_lock lk{pool_mutex};
            PurgePool(allocator, true);
        }
        result = vmaCreateImage(allocator, &image_ci_unsafe, &alloc_ci, &unsafe_image, &allocation,
                                &alloc_info);
    }
    if (result == VK_ERROR_OUT_OF_DEVICE_MEMORY) {
        // The budget shrinks as other programs take video memory, and going over it ended the
        // game. Past it the driver moves memory out to system memory: slower, but it goes on.
        LOG_WARNING(Render_Vulkan, "Allocating an image went over the memory budget, allocating it past the budget");
        VmaAllocationCreateInfo over_budget_ci = alloc_ci;
        over_budget_ci.flags &= ~VMA_ALLOCATION_CREATE_WITHIN_BUDGET_BIT;
        result = vmaCreateImage(allocator, &image_ci_unsafe, &over_budget_ci, &unsafe_image,
                                &allocation, &alloc_info);
    }
    BbStats::alloc_count[1].fetch_add(1, std::memory_order_relaxed);
    BbStats::alloc_bytes[1].fetch_add(alloc_info.size, std::memory_order_relaxed);
    ASSERT_MSG(result == VK_SUCCESS, "Failed allocating image with error {}",
               vk::to_string(vk::Result{result}));
    image = vk::Image{unsafe_image};
    size_bytes = alloc_info.size;
}

Image::Image(const Vulkan::Instance& instance, Vulkan::Runtime& runtime_,
             Common::SlotVector<ImageView>& slot_image_views_, const ImageInfo& info_)
    : guest_begin{info_.guest_address}, guest_end{info_.guest_address + info_.guest_size},
      info{info_}, runtime{&runtime_}, slot_image_views{&slot_image_views_} {
    BbStats::Timer timer{BbStats::t_image_create};
    if (info.pixel_format == vk::Format::eUndefined) {
        return;
    }
    Common::Perf::Add(Common::Perf::ImageCreates);

    image_uid = global_image_uid.Next();
    mip_hashes.resize(info.resources.levels);
    vk::ImageCreateFlags flags{vk::ImageCreateFlagBits::eMutableFormat |
                               vk::ImageCreateFlagBits::eExtendedUsage};
    if (info.props.is_volume) {
        flags |= vk::ImageCreateFlagBits::e2DArrayCompatible;
        if (instance.Is2dViewOf3dSupported()) {
            flags |= vk::ImageCreateFlagBits::e2DViewCompatibleEXT;
        }
    }
    if (info.props.is_block && instance.IsBlockTexelViewSupported()) {
        flags |= vk::ImageCreateFlagBits::eBlockTexelViewCompatible;
    }

    usage_flags = ImageUsageFlags(instance, info);
    format_features = FormatFeatureFlags(usage_flags);
    if (info.props.is_depth) {
        aspect_mask = vk::ImageAspectFlagBits::eDepth;
        if (info.props.has_stencil) {
            aspect_mask |= vk::ImageAspectFlagBits::eStencil;
        }
    }

    constexpr auto tiling = vk::ImageTiling::eOptimal;
    // ssport: block-compressed formats are not available as 1D images on this class of driver
    // (a 1D BC image created anyway takes the device down a few frames later). The same texels
    // as a one-row 2D image are equivalent; views of it are 2D too (see ImageView).
    auto vk_image_type = ConvertImageType(info.type);
    if (info.props.is_block && vk_image_type == vk::ImageType::e1D) {
        vk_image_type = vk::ImageType::e2D;
    }
    const auto supported_format = instance.GetSupportedFormat(info.pixel_format, format_features);
    vk::PhysicalDeviceImageFormatInfo2 format_info{
        .format = supported_format,
        .type = vk_image_type,
        .tiling = tiling,
        .usage = usage_flags,
        .flags = flags,
    };
    auto image_format_properties =
        instance.GetPhysicalDevice().getImageFormatProperties2(format_info);
    // bbport: storage usage is speculative (see ImageUsageFlags). Drivers that refuse it for a
    // format (AMD's Windows driver for BC6H) get the image without it: creating the unsupported
    // combination anyway loses the device a few frames later.
    if (image_format_properties.result == vk::Result::eErrorFormatNotSupported &&
        (usage_flags & vk::ImageUsageFlagBits::eStorage)) {
        format_info.usage = usage_flags & ~vk::ImageUsageFlagBits::eStorage;
        image_format_properties = instance.GetPhysicalDevice().getImageFormatProperties2(format_info);
        if (image_format_properties.result == vk::Result::eSuccess) {
            usage_flags = format_info.usage;
            format_features = FormatFeatureFlags(usage_flags);
        } else {
            format_info.usage = usage_flags;
        }
    }
    if (image_format_properties.result == vk::Result::eErrorFormatNotSupported) {
        LOG_ERROR(Render_Vulkan, "image format {} type {} is not supported (flags {}, usage {})",
                  vk::to_string(supported_format), vk::to_string(format_info.type),
                  vk::to_string(format_info.flags), vk::to_string(format_info.usage));
    }
    supported_samples = image_format_properties.result == vk::Result::eSuccess
                            ? image_format_properties.value.imageFormatProperties.sampleCounts
                            : vk::SampleCountFlagBits::e1;

    const vk::ImageCreateInfo image_ci = {
        .flags = flags,
        .imageType = vk_image_type,
        .format = supported_format,
        .extent{
            .width = info.size.width,
            .height = info.size.height,
            .depth = info.size.depth,
        },
        .mipLevels = static_cast<u32>(info.resources.levels),
        .arrayLayers = static_cast<u32>(info.resources.layers),
        .samples = LiverpoolToVK::NumSamples(info.num_samples, supported_samples),
        .tiling = tiling,
        .usage = usage_flags,
        .initialLayout = vk::ImageLayout::eUndefined,
    };

    backing = &backing_images.emplace_back();
    backing->num_samples = info.num_samples;
    backing->image = UniqueImage{instance.GetDevice(), instance.GetAllocator()};
    backing->image.Create(image_ci);

    Vulkan::SetObjectName(instance.GetDevice(), GetImage(),
                          "Image {}x{}x{} {} {} {:#x}:{:#x} L:{} M:{} S:{}", info.size.width,
                          info.size.height, info.size.depth, AmdGpu::NameOf(info.tile_mode),
                          vk::to_string(info.pixel_format), info.guest_address, info.guest_size,
                          info.resources.layers, info.resources.levels, info.num_samples);
}

Image::~Image() = default;

ImageView& Image::FindView(const ImageViewInfo& view_info, bool ensure_guest_samples) {
    if (ensure_guest_samples && backing->num_samples > 1 != info.num_samples > 1) {
        runtime->SetBackingSamples(this, info.num_samples);
    }
    const auto& view_infos = backing->image_view_infos;
    if (backing->last_view < view_infos.size() && view_infos[backing->last_view] == view_info) {
        return (*slot_image_views)[backing->image_view_ids[backing->last_view]];
    }
    const auto it = std::ranges::find(view_infos, view_info);
    if (it != view_infos.end()) {
        backing->last_view = static_cast<u32>(std::distance(view_infos.begin(), it));
        const auto view_id = backing->image_view_ids[backing->last_view];
        return (*slot_image_views)[view_id];
    }
    const auto view_id = slot_image_views->insert(runtime->GetInstance(), view_info, *this);
    backing->image_view_infos.emplace_back(view_info);
    backing->image_view_ids.emplace_back(view_id);
    return (*slot_image_views)[view_id];
}

void Image::GetBarriers(Barriers& barriers, vk::ImageLayout dst_layout, vk::AccessFlags2 dst_mask,
                        vk::PipelineStageFlags2 dst_stage,
                        std::optional<SubresourceRange> subres_range) {
    auto& last_state = backing->state;
    auto& subresource_states = backing->subresource_states;
    auto& dirty = backing->dirty_subresources;
    Common::Perf::Add(Common::Perf::BarrierCalls);
    const size_t barriers_before = barriers.size();
    SCOPE_EXIT {
        if (barriers.size() != barriers_before) {
            Common::Perf::Add(Common::Perf::BarriersEmitted, barriers.size() - barriers_before);
        }
    };

    constexpr auto write_flags = vk::AccessFlagBits2::eTransferWrite |
                                 vk::AccessFlagBits2::eShaderWrite |
                                 vk::AccessFlagBits2::eMemoryWrite;
    const auto needs_barrier = [&](const State& state) {
        const bool is_write = static_cast<bool>(state.access_mask & write_flags);
        return state.layout != dst_layout || state.access_mask != dst_mask || is_write;
    };
    const auto transition = [&](State& state, u32 mip, u32 layer) {
        // NOTE: this may produce a lot of small barriers.
        // If this becomes a problem, we can optimize it by merging adjacent barriers.
        barriers.emplace_back(vk::ImageMemoryBarrier2{
            .srcStageMask = state.pl_stage,
            .srcAccessMask = state.access_mask,
            .dstStageMask = dst_stage,
            .dstAccessMask = dst_mask,
            .oldLayout = state.layout,
            .newLayout = dst_layout,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = GetImage(),
            .subresourceRange{
                .aspectMask = aspect_mask,
                .baseMipLevel = mip,
                .levelCount = 1,
                .baseArrayLayer = layer,
                .layerCount = 1,
            },
        });
        state.layout = dst_layout;
        state.access_mask = dst_mask;
        state.pl_stage = dst_stage;
        ++backing->state_version;
    };

    const bool needs_partial_transition =
        subres_range &&
        (subres_range->base != SubresourceBase{} || subres_range->extent != info.resources);
    const bool partially_transited = !subresource_states.empty();

    if (needs_partial_transition && !partially_transited && !needs_barrier(last_state)) {
        // The whole image is in a single state. A sub range that is already in the requested
        // state needs no barrier, and splitting the state per subresource here would make every
        // later full transition walk all subresources (common when sampling one layer or one mip
        // of an image every draw).
        Common::Perf::Add(Common::Perf::BarrierFastPath);
        last_state.pl_stage |= dst_stage;
        return;
    }

    if (needs_partial_transition && partially_transited) {
        // A sub range that is already in the requested state is common when the same view of a
        // partially transited image is bound by every draw: skip walking it again.
        const auto& memo = backing->last_partial;
        if (memo.valid && memo.version == backing->state_version && memo.range == *subres_range &&
            memo.layout == dst_layout && memo.access == dst_mask && memo.stage == dst_stage &&
            !(dst_mask & write_flags)) {
            Common::Perf::Add(Common::Perf::BarrierMemoHit);
            last_state.layout = dst_layout;
            last_state.access_mask = dst_mask;
            last_state.pl_stage = dst_stage;
            return;
        }
    }

    if (needs_partial_transition || partially_transited) {
        if (!partially_transited) {
            subresource_states.assign(info.resources.levels * info.resources.layers, last_state);
            backing->partial_base_state = last_state;
            dirty.clear();
            backing->dirty_overflow = false;
            ++backing->state_version;
        }

        if (!needs_partial_transition && !backing->dirty_overflow &&
            !needs_barrier(backing->partial_base_state)) {
            // Full transition of an image whose untouched subresources are already in the
            // requested state: only the subresources changed since the image stopped being
            // uniform can need a barrier.
            Common::Perf::Add(Common::Perf::BarrierDirtyPath);
            for (const u32 subres_idx : dirty) {
                auto& state = subresource_states[subres_idx];
                if (needs_barrier(state)) {
                    transition(state, subres_idx / info.resources.layers,
                               subres_idx % info.resources.layers);
                }
            }
        } else {
            // In case of partial transition, we need to change the specified subresources only.
            // Otherwise all subresources need to be set to the same state so we can use a full
            // resource transition for the next time.
            const auto mips = needs_partial_transition
                                  ? std::ranges::views::iota(subres_range->base.level,
                                                             subres_range->base.level +
                                                                 subres_range->extent.levels)
                                  : std::views::iota(0u, info.resources.levels);
            const auto layers = needs_partial_transition
                                    ? std::ranges::views::iota(subres_range->base.layer,
                                                               subres_range->base.layer +
                                                                   subres_range->extent.layers)
                                    : std::views::iota(0u, info.resources.layers);

            Common::Perf::Add(Common::Perf::BarrierSubresources,
                              static_cast<u64>(std::ranges::distance(mips)) *
                                  static_cast<u64>(std::ranges::distance(layers)));
            for (u32 mip : mips) {
                for (u32 layer : layers) {
                    const u32 subres_idx = mip * info.resources.layers + layer;
                    ASSERT(subres_idx < subresource_states.size());
                    auto& state = subresource_states[subres_idx];
                    if (!needs_barrier(state)) {
                        continue;
                    }
                    transition(state, mip, layer);
                    if (needs_partial_transition && !backing->dirty_overflow &&
                        std::ranges::find(dirty, subres_idx) == dirty.end()) {
                        if (dirty.size() < 64) {
                            dirty.push_back(subres_idx);
                        } else {
                            backing->dirty_overflow = true;
                        }
                    }
                }
            }
        }

        if (!needs_partial_transition) {
            subresource_states.clear();
            dirty.clear();
            backing->dirty_overflow = false;
            backing->last_partial.valid = false;
        } else {
            backing->last_partial = {*subres_range,          dst_layout, dst_mask, dst_stage,
                                     backing->state_version, true};
        }
    } else { // Full resource transition
        if (!needs_barrier(last_state)) {
            return;
        }

        barriers.emplace_back(vk::ImageMemoryBarrier2{
            .srcStageMask = last_state.pl_stage,
            .srcAccessMask = last_state.access_mask,
            .dstStageMask = dst_stage,
            .dstAccessMask = dst_mask,
            .oldLayout = last_state.layout,
            .newLayout = dst_layout,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = GetImage(),
            .subresourceRange{
                .aspectMask = aspect_mask,
                .baseMipLevel = 0,
                .levelCount = VK_REMAINING_MIP_LEVELS,
                .baseArrayLayer = 0,
                .layerCount = VK_REMAINING_ARRAY_LAYERS,
            },
        });
    }

    last_state.layout = dst_layout;
    last_state.access_mask = dst_mask;
    last_state.pl_stage = dst_stage;
}

} // namespace VideoCore
