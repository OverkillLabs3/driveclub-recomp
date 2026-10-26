#include "../../../shim/bbport_diagnostics.h"
// SPDX-FileCopyrightText: Copyright 2025 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <vector>
#include <set>
#include <span>
#include <ctime>
#include <boost/container/small_vector.hpp>
#include "bbport_toggles.h"
#include "video_core/buffer_cache/buffer.h"
#include "video_core/renderer_vulkan/liverpool_to_vk.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_runtime.h"
#include "video_core/renderer_vulkan/vk_gpu_profiler.h"
#include "video_core/renderer_vulkan/vk_scene_resolution.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/texture_cache/blit_helper.h"
#include "video_core/texture_cache/image.h"

#include <vulkan/vulkan_format_traits.hpp>
#include <vk_mem_alloc.h>

namespace Vulkan {

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
        UNREACHABLE_MSG("Unexpected image type {}", u32(type));
    }
}

static std::pair<u32, u32> SanitizeCopyLayers(const VideoCore::ImageInfo& src_info,
                                              const VideoCore::ImageInfo& dst_info,
                                              const u32 depth) {
    const auto vk_src_type = ConvertImageType(src_info.type);
    const auto vk_dst_type = ConvertImageType(dst_info.type);

    u32 src_layers = src_info.resources.layers;
    u32 dst_layers = dst_info.resources.layers;

    // 3D images can only use 1 layer.
    if (vk_src_type == vk::ImageType::e3D && src_layers != 1) {
        LOG_WARNING(Render_Vulkan, "Coercing copy 3D source layers {} to 1.", src_layers);
        src_layers = 1;
    }
    if (vk_dst_type == vk::ImageType::e3D && dst_layers != 1) {
        LOG_WARNING(Render_Vulkan, "Coercing copy 3D destination layers {} to 1.", dst_layers);
        dst_layers = 1;
    }

    // If the image type is equal, layer count must match. Take the minimum of both.
    if (vk_src_type == vk_dst_type) {
        if (src_layers != dst_layers) {
            LOG_WARNING(Render_Vulkan,
                        "Coercing copy source layers {} and destination layers {} to minimum.",
                        src_layers, dst_layers);
            src_layers = dst_layers = std::min(src_layers, dst_layers);
        }
    } else {
        // For 2D <-> 3D copies, 2D layer count must equal 3D depth.
        if (vk_src_type == vk::ImageType::e2D && vk_dst_type == vk::ImageType::e3D &&
            src_layers != depth) {
            LOG_WARNING(Render_Vulkan,
                        "Coercing copy 2D source layers {} to 3D destination depth {}", src_layers,
                        depth);
            src_layers = depth;
        }
        if (vk_src_type == vk::ImageType::e3D && vk_dst_type == vk::ImageType::e2D &&
            dst_layers != depth) {
            LOG_WARNING(Render_Vulkan,
                        "Coercing copy 2D destination layers {} to 3D source depth {}", dst_layers,
                        depth);
            dst_layers = depth;
        }
    }

    return std::make_pair(src_layers, dst_layers);
}

static u32 BufferImageCopySize(const vk::BufferImageCopy& copy, const vk::Format pixel_format) {
    const u32 row_length = copy.bufferRowLength ? copy.bufferRowLength : copy.imageExtent.width;
    const u32 height = copy.bufferImageHeight ? copy.bufferImageHeight : copy.imageExtent.height;

    const auto block = vk::blockExtent(pixel_format);
    const u32 block_size = vk::blockSize(pixel_format);
    const u32 row_pitch = (row_length / block[0]) * block_size;
    const u32 slice_pitch = (height / block[1]) * row_pitch;

    const u32 width_in_blocks = (copy.imageExtent.width + block[0] - 1) / block[0];
    const u32 height_in_blocks = (copy.imageExtent.height + block[1] - 1) / block[1];
    const u32 num_slices = copy.imageExtent.depth * copy.imageSubresource.layerCount;

    return (num_slices - 1) * slice_pitch + (height_in_blocks - 1) * row_pitch +
           width_in_blocks * block_size;
}

Runtime::Runtime(const Instance& instance_, Scheduler& scheduler_)
    : instance{instance_}, scheduler{scheduler_}, staging_pool{instance_, scheduler_} {
    blit_helper = std::make_unique<VideoCore::BlitHelper>(instance, scheduler);

    memory_barrier.dstStageMask = vk::PipelineStageFlagBits2::eAllCommands;
    memory_barrier.dstAccessMask =
        vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite;
}

void Runtime::TickFrame() {
    staging_pool.TickFrame();
}

void Runtime::CopyBuffer(const VideoCore::Buffer* src, const VideoCore::Buffer* dst,
                         std::span<const vk::BufferCopy> copies) {
    scheduler.EndRendering();

    // bbport: many regions (HLE copy shaders) are tracked as one bounding range per buffer:
    // conservative for barriers, and two tree lookups instead of two per region.
    const bool bounded = copies.size() > 4;
    u64 src_min = ~0ULL, src_max = 0, dst_min = ~0ULL, dst_max = 0;
    if (bounded) {
        for (const auto& copy : copies) {
            src_min = std::min<u64>(src_min, copy.srcOffset);
            src_max = std::max<u64>(src_max, copy.srcOffset + copy.size);
            dst_min = std::min<u64>(dst_min, copy.dstOffset);
            dst_max = std::max<u64>(dst_max, copy.dstOffset + copy.size);
        }
    }

    bool needs_flush{};
    if (bounded) {
        needs_flush = IsBufferAccessed(src, src_min, src_max - src_min) ||
                      IsBufferAccessed(dst, dst_min, dst_max - dst_min, true);
    } else {
        for (const auto& copy : copies) {
            needs_flush |= IsBufferAccessed(src, copy.srcOffset, copy.size);
            needs_flush |= IsBufferAccessed(dst, copy.dstOffset, copy.size, true);
        }
    }
    if (needs_flush) {
        FlushBarriers();
    }

    scheduler.Record([src_handle = src->Handle(), dst_handle = dst->Handle(),
                      regions = scheduler.RecordData(copies)](vk::CommandBuffer cmdbuf) {
        cmdbuf.copyBuffer(src_handle, dst_handle, regions.size(), regions.data());
    });

    if (bounded) {
        AccessBuffer(src, src_min, src_max - src_min, vk::PipelineStageFlagBits2::eCopy,
                     vk::AccessFlagBits2::eTransferRead);
        AccessBuffer(dst, dst_min, dst_max - dst_min, vk::PipelineStageFlagBits2::eCopy,
                     vk::AccessFlagBits2::eTransferWrite);
        return;
    }
    for (const auto& copy : copies) {
        AccessBuffer(src, copy.srcOffset, copy.size, vk::PipelineStageFlagBits2::eCopy,
                     vk::AccessFlagBits2::eTransferRead);
        AccessBuffer(dst, copy.dstOffset, copy.size, vk::PipelineStageFlagBits2::eCopy,
                     vk::AccessFlagBits2::eTransferWrite);
    }
}

void Runtime::FillBuffer(const VideoCore::Buffer* dst, u64 offset, u64 size, u32 value) {
    scheduler.EndRendering();

    if (IsBufferAccessed(dst, offset, size, true)) {
        FlushBarriers();
    }

    scheduler.Record([handle = dst->Handle(), offset, size, value](vk::CommandBuffer cmdbuf) {
        cmdbuf.fillBuffer(handle, offset, size, value);
    });

    AccessBuffer(dst, offset, size, vk::PipelineStageFlagBits2::eClear,
                 vk::AccessFlagBits2::eTransferWrite);
}

void Runtime::InlineData(VideoCore::Buffer* dst, u64 offset, u32 value) {
    scheduler.EndRendering();

    if (IsBufferAccessed(dst, offset, sizeof(value), true)) {
        FlushBarriers();
    }

    scheduler.Record([handle = dst->Handle(), offset, value](vk::CommandBuffer cmdbuf) {
        cmdbuf.updateBuffer(handle, offset, sizeof(value), &value);
    });

    AccessBuffer(dst, offset, sizeof(value), vk::PipelineStageFlagBits2::eCopy,
                 vk::AccessFlagBits2::eTransferWrite);
}

void Runtime::InlineBytes(VideoCore::Buffer* dst, u64 offset, const void* data, u32 size) {
    scheduler.EndRendering();

    if (IsBufferAccessed(dst, offset, size, true)) {
        FlushBarriers();
    }

    std::vector<u8> bytes(static_cast<const u8*>(data), static_cast<const u8*>(data) + size);
    scheduler.Record([handle = dst->Handle(), offset, bytes = std::move(bytes)](vk::CommandBuffer cmdbuf) {
        cmdbuf.updateBuffer(handle, offset, bytes.size(), bytes.data());
    });

    AccessBuffer(dst, offset, size, vk::PipelineStageFlagBits2::eCopy, vk::AccessFlagBits2::eTransferWrite);
}

bool Runtime::Transit(VideoCore::Image* image, vk::ImageLayout dst_layout,
                      vk::PipelineStageFlags2 dst_stage, vk::AccessFlags2 dst_access,
                      std::optional<VideoCore::SubresourceRange> subres_range) {
    BeforeImageAccess();
    if (scene_targets) scene_targets->NativeAccess(*image, dst_access);
    const size_t prev_num_barriers = static_cast<size_t>(image_barriers.size());
    image->GetBarriers(image_barriers, dst_layout, dst_access, dst_stage, subres_range);
    return image_barriers.size() != prev_num_barriers;
}

Runtime::TransferMark::TransferMark(Runtime& runtime, const char* what,
                                    const VideoCore::Image& image) {
    profiler = GpuProfiler::Get();
    if (!profiler || !profiler->Records(&runtime.scheduler)) {
        profiler = nullptr;
        return;
    }
    resume = profiler->Current();
    const auto& info = image.info;
    const u64 key = (u64(reinterpret_cast<uintptr_t>(what)) << 20) ^
                    (u64(info.pixel_format) << 40) ^ (u64(info.size.width) << 16) ^
                    info.size.height ^ 0x7A5Full;
    profiler->Mark(key, [&] {
        return fmt::format("{} {} {}x{}", what, vk::to_string(info.pixel_format),
                           info.size.width, info.size.height);
    });
}

Runtime::TransferMark::~TransferMark() {
    if (profiler && resume) {
        profiler->Resume(resume);
    }
}

void Runtime::UploadImage(VideoCore::Image* dst, const VideoCore::Buffer* src,
                          std::span<const vk::BufferImageCopy> upload_copies) {
    SetBackingSamples(dst, dst->info.num_samples, false);
    scheduler.EndRendering();
    const TransferMark mark{*this, "image upload", *dst};

    bool needs_flush =
        Transit(dst, vk::ImageLayout::eTransferDstOptimal, vk::PipelineStageFlagBits2::eCopy,
                vk::AccessFlagBits2::eTransferWrite);
    for (const auto& copy : upload_copies) {
        const auto copy_size = BufferImageCopySize(copy, dst->info.pixel_format);
        needs_flush |= IsBufferAccessed(src, copy.bufferOffset, copy_size);
    }
    if (needs_flush) {
        FlushBarriers();
    }

    scheduler.Record([src_handle = src->Handle(), image = dst->GetImage(),
                      regions = scheduler.RecordData(upload_copies)](vk::CommandBuffer cmdbuf) {
        cmdbuf.copyBufferToImage(src_handle, image, vk::ImageLayout::eTransferDstOptimal,
                                 regions.size(), regions.data());
    });

    for (const auto& copy : upload_copies) {
        const auto copy_size = BufferImageCopySize(copy, dst->info.pixel_format);
        AccessBuffer(src, copy.bufferOffset, copy_size, vk::PipelineStageFlagBits2::eCopy,
                     vk::AccessFlagBits2::eTransferRead);
    }

    dst->flags &= ~VideoCore::ImageFlagBits::Dirty;
}

void Runtime::DownloadImage(VideoCore::Image* src, const VideoCore::Buffer* dst,
                            std::span<const vk::BufferImageCopy> download_copies) {
    SetBackingSamples(src, src->info.num_samples);
    scheduler.EndRendering();
    const TransferMark mark{*this, "image download", *src};

    bool needs_flush =
        Transit(src, vk::ImageLayout::eTransferSrcOptimal, vk::PipelineStageFlagBits2::eCopy,
                vk::AccessFlagBits2::eTransferRead);
    for (const auto& copy : download_copies) {
        const auto copy_size = BufferImageCopySize(copy, src->info.pixel_format);
        needs_flush |= IsBufferAccessed(dst, copy.bufferOffset, copy_size, true);
    }
    if (needs_flush) {
        FlushBarriers();
    }

    scheduler.Record([image = src->GetImage(), buffer = dst->Handle(),
                      regions = scheduler.RecordData(download_copies)](vk::CommandBuffer cmdbuf) {
        cmdbuf.copyImageToBuffer(image, vk::ImageLayout::eTransferSrcOptimal, buffer,
                                 regions.size(), regions.data());
    });

    for (const auto& copy : download_copies) {
        const auto copy_size = BufferImageCopySize(copy, src->info.pixel_format);
        AccessBuffer(dst, copy.bufferOffset, copy_size, vk::PipelineStageFlagBits2::eCopy,
                     vk::AccessFlagBits2::eTransferWrite);
    }
}

void Runtime::CopyImage(VideoCore::Image* src, VideoCore::Image* dst) {
    const TransferMark mark{*this, "image copy", *dst};
    const u32 num_mips = std::min(src->info.resources.levels, dst->info.resources.levels);

    // Format mismatch warning (safe but useful)
    if (src->info.pixel_format != dst->info.pixel_format) {
        LOG_DEBUG(Render_Vulkan,
                  "Copy between different formats: src={}, dst={}. "
                  "Result may be undefined.",
                  vk::to_string(src->info.pixel_format), vk::to_string(dst->info.pixel_format));
    }

    const u32 base_width = src->info.size.width;
    const u32 base_height = src->info.size.height;
    const u32 base_depth =
        dst->info.type == AmdGpu::ImageType::Color3D ? dst->info.size.depth : src->info.size.depth;

    // Match sample count before copying
    SetBackingSamples(dst, dst->info.num_samples, false);
    SetBackingSamples(src, src->info.num_samples);

    boost::container::small_vector<vk::ImageCopy, 8> regions;

    const vk::ImageAspectFlags src_aspect = src->aspect_mask & ~vk::ImageAspectFlagBits::eStencil;
    const vk::ImageAspectFlags dst_aspect = dst->aspect_mask & ~vk::ImageAspectFlagBits::eStencil;

    const bool src_is_2d = ConvertImageType(src->info.type) == vk::ImageType::e2D;
    const bool src_is_3d = ConvertImageType(src->info.type) == vk::ImageType::e3D;

    const bool dst_is_2d = ConvertImageType(dst->info.type) == vk::ImageType::e2D;
    const bool dst_is_3d = ConvertImageType(dst->info.type) == vk::ImageType::e3D;

    const bool is_2d_to_3d = src_is_2d && dst_is_3d;
    const bool is_3d_to_2d = src_is_3d && dst_is_2d;
    const bool is_same_type = !is_2d_to_3d && !is_3d_to_2d;

    for (u32 mip = 0; mip < num_mips; ++mip) {
        const u32 mip_w = std::max(base_width >> mip, 1u);
        const u32 mip_h = std::max(base_height >> mip, 1u);
        const u32 mip_d = std::max(base_depth >> mip, 1u);

        const auto [src_layers, dst_layers] = SanitizeCopyLayers(src->info, dst->info, mip_d);

        vk::ImageCopy region{};
        region.srcSubresource.aspectMask = src_aspect;
        region.srcSubresource.mipLevel = mip;
        region.srcSubresource.baseArrayLayer = 0;
        region.dstSubresource.aspectMask = dst_aspect;
        region.dstSubresource.mipLevel = mip;
        region.dstSubresource.baseArrayLayer = 0;

        if (is_same_type) {
            // 2D->2D OR 3D->3D
            if (src_is_3d) {
                // 3D images must use layerCount=1
                region.srcSubresource.layerCount = 1;
                region.dstSubresource.layerCount = 1;
                region.extent = vk::Extent3D(mip_w, mip_h, mip_d);
            } else {
                // Array images
                const u32 copy_layers = std::min(src_layers, dst_layers);
                region.srcSubresource.layerCount = copy_layers;
                region.dstSubresource.layerCount = copy_layers;
                region.extent = vk::Extent3D(mip_w, mip_h, 1);
            }
        } else if (is_2d_to_3d) {
            // 2D array -> 3D volume
            region.srcSubresource.layerCount = src_layers;
            region.dstSubresource.layerCount = 1;
            region.extent = vk::Extent3D(mip_w, mip_h, src_layers);
        } else if (is_3d_to_2d) {
            // 3D volume -> 2D array
            region.srcSubresource.layerCount = 1;
            region.dstSubresource.layerCount = dst_layers;
            region.extent = vk::Extent3D(mip_w, mip_h, dst_layers);
        }

        regions.push_back(region);
    }

    scheduler.EndRendering();

    bool needs_flush =
        Transit(src, vk::ImageLayout::eTransferSrcOptimal, vk::PipelineStageFlagBits2::eCopy,
                vk::AccessFlagBits2::eTransferRead);
    needs_flush |= Transit(dst, vk::ImageLayout::eTransferDstOptimal,
                           vk::PipelineStageFlagBits2::eCopy, vk::AccessFlagBits2::eTransferWrite);
    if (needs_flush) {
        FlushBarriers();
    }

    scheduler.Record([src_image = src->GetImage(), dst_image = dst->GetImage(),
                      regions](vk::CommandBuffer cmdbuf) {
        cmdbuf.copyImage(src_image, vk::ImageLayout::eTransferSrcOptimal, dst_image,
                         vk::ImageLayout::eTransferDstOptimal, regions);
    });

    dst->flags |= (src->flags & VideoCore::ImageFlagBits::GpuModified);
    dst->flags &= ~VideoCore::ImageFlagBits::Dirty;
}

void Runtime::CopyImageWithBuffer(VideoCore::Image* src, VideoCore::Image* dst,
                                  const VideoCore::Buffer* buffer, u64 offset) {
    const TransferMark mark{*this, "image copy through buffer", *dst};
    const u32 num_mips = std::min(src->info.resources.levels, dst->info.resources.levels);
    const u32 num_layers = std::min(src->info.resources.layers, dst->info.resources.layers);
    ASSERT(src->info.resources.layers == dst->info.resources.layers && num_mips == 1);

    SetBackingSamples(dst, dst->info.num_samples, false);
    SetBackingSamples(src, src->info.num_samples);

    vk::BufferImageCopy buffer_copy = {
        .bufferOffset = offset,
        .bufferRowLength = 0,
        .bufferImageHeight = 0,
        .imageSubresource{
            .aspectMask = src->aspect_mask & ~vk::ImageAspectFlagBits::eStencil,
            .mipLevel = 0u,
            .baseArrayLayer = 0,
            .layerCount = num_layers,
        },
        .imageOffset = {0, 0, 0},
        .imageExtent = {src->info.size.width, src->info.size.height, src->info.size.depth},
    };
    const auto copy_size = BufferImageCopySize(buffer_copy, src->info.pixel_format);

    scheduler.EndRendering();

    bool needs_flush =
        Transit(src, vk::ImageLayout::eTransferSrcOptimal, vk::PipelineStageFlagBits2::eCopy,
                vk::AccessFlagBits2::eTransferRead);
    needs_flush |= Transit(dst, vk::ImageLayout::eTransferDstOptimal,
                           vk::PipelineStageFlagBits2::eCopy, vk::AccessFlagBits2::eTransferWrite);
    needs_flush |= IsBufferAccessed(buffer, offset, copy_size);
    if (needs_flush) {
        FlushBarriers();
    }

    const auto download_copy = buffer_copy;
    buffer_copy.imageSubresource.aspectMask = dst->aspect_mask & ~vk::ImageAspectFlagBits::eStencil;
    scheduler.Record([src_image = src->GetImage(), dst_image = dst->GetImage(),
                      handle = buffer->Handle(), download_copy,
                      upload_copy = buffer_copy](vk::CommandBuffer cmdbuf) {
        cmdbuf.copyImageToBuffer(src_image, vk::ImageLayout::eTransferSrcOptimal, handle,
                                 download_copy);
        const vk::MemoryBarrier2 post_copy_barrier = {
            .srcStageMask = vk::PipelineStageFlagBits2::eCopy,
            .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eCopy,
            .dstAccessMask = vk::AccessFlagBits2::eTransferRead,
        };
        cmdbuf.pipelineBarrier2(vk::DependencyInfo{
            .dependencyFlags = vk::DependencyFlagBits::eByRegion,
            .memoryBarrierCount = 1u,
            .pMemoryBarriers = &post_copy_barrier,
        });
        cmdbuf.copyBufferToImage(handle, dst_image, vk::ImageLayout::eTransferDstOptimal,
                                 upload_copy);
    });

    dst->flags |= (src->flags & VideoCore::ImageFlagBits::GpuModified);
    dst->flags &= ~VideoCore::ImageFlagBits::Dirty;
}

void Runtime::CopyMip(VideoCore::Image* src, VideoCore::Image* dst, u32 mip, u32 slice) {
    const auto dst_dim = dst->info.props.is_block ? 2 : 0;
    const auto mip_block_w = std::max(dst->info.size.width >> (mip + dst_dim), 1u);
    const auto mip_block_h = std::max(dst->info.size.height >> (mip + dst_dim), 1u);
    const auto mip_block_p = std::max(dst->info.mips_layout[mip].pitch >> dst_dim, 1u);

    const auto src_dim = src->info.props.is_block ? 2 : 0;
    ASSERT(mip_block_w == (src->info.size.width >> src_dim));
    ASSERT(mip_block_h == (src->info.size.height >> src_dim));
    ASSERT(mip_block_p == (src->info.pitch >> src_dim));

    const auto [src_layers, dst_layers] =
        SanitizeCopyLayers(src->info, dst->info, src->info.size.depth);

    const vk::ImageCopy image_copy{
        .srcSubresource{
            .aspectMask = src->aspect_mask,
            .mipLevel = 0,
            .baseArrayLayer = 0,
            .layerCount = src_layers,
        },
        .dstSubresource{
            .aspectMask = src->aspect_mask,
            .mipLevel = mip,
            .baseArrayLayer = slice,
            .layerCount = dst_layers,
        },
        .extent = {src->info.size.width, src->info.size.height, src->info.size.depth},
    };

    SetBackingSamples(dst, dst->info.num_samples);
    SetBackingSamples(src, src->info.num_samples);

    scheduler.EndRendering();

    bool needs_flush =
        Transit(src, vk::ImageLayout::eTransferSrcOptimal, vk::PipelineStageFlagBits2::eCopy,
                vk::AccessFlagBits2::eTransferRead);
    needs_flush |= Transit(dst, vk::ImageLayout::eTransferDstOptimal,
                           vk::PipelineStageFlagBits2::eCopy, vk::AccessFlagBits2::eTransferWrite);
    if (needs_flush) {
        FlushBarriers();
    }

    scheduler.Record([src_image = src->GetImage(), dst_image = dst->GetImage(),
                      image_copy](vk::CommandBuffer cmdbuf) {
        cmdbuf.copyImage(src_image, vk::ImageLayout::eTransferSrcOptimal, dst_image,
                         vk::ImageLayout::eTransferDstOptimal, image_copy);
    });

    dst->flags |= (src->flags & VideoCore::ImageFlagBits::GpuModified);
    dst->flags &= ~VideoCore::ImageFlagBits::Dirty;
}

namespace {
const char* DepthName(vk::Format f) {
    return f == vk::Format::eD32SfloatS8Uint ? "D32S8" : f == vk::Format::eD32Sfloat ? "D32" : "?";
}
vk::ImageMemoryBarrier2 LayoutBarrier(vk::Image image, vk::ImageAspectFlags aspect, vk::ImageLayout from,
                                      vk::ImageLayout to, vk::PipelineStageFlags2 src_stage,
                                      vk::AccessFlags2 src_access, vk::PipelineStageFlags2 dst_stage,
                                      vk::AccessFlags2 dst_access) {
    return {.srcStageMask = src_stage,
            .srcAccessMask = src_access,
            .dstStageMask = dst_stage,
            .dstAccessMask = dst_access,
            .oldLayout = from,
            .newLayout = to,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = image,
            .subresourceRange{aspect, 0U, 1U, 0U, 1U}};
}
} // namespace

// bbport: run once by BB_FORM_COPY_VERIFY: D32 and D32S8 depth planes loaded through a buffer copy with chosen bits
// (+0, -0, subnormals, the smallest normals, values next to 0.5 and 1, then a sweep of [0, 1] with varied low bits),
// then the depth->R32F draw; its result must equal the depth plane read back by a transfer.
// Depth images hold [0, 1] only without VK_EXT_depth_range_unrestricted (vkCmdCopyBufferToImage), so no NaN or Inf:
// the game cannot have them in a depth buffer either.
void Runtime::SelfTestFormCopy() {
    constexpr u32 W = 64, H = 64, N = W * H;
    static constexpr u32 special[] = {0x00000000u, 0x80000000u, 0x00000001u, 0x00000002u, 0x003fffffu,
                                      0x007fffffu, 0x00800000u, 0x00800001u, 0x33800000u, 0x3dcccccdu,
                                      0x3effffffu, 0x3f000000u, 0x3f000001u, 0x3f7ffffeu, 0x3f7fffffu,
                                      0x3f800000u};
    std::vector<u32> pattern(N);
    for (u32 i = 0; i < N; ++i) {
        pattern[i] = i < std::size(special) ? special[i]
                                            : u32(u64(i) * 0x3f800000ull / N) ^ ((i * 2654435761u) >> 22);
    }
    const auto device = instance.GetDevice();
    for (const vk::Format format : {vk::Format::eD32Sfloat, vk::Format::eD32SfloatS8Uint}) {
        const vk::ImageAspectFlags all = format == vk::Format::eD32SfloatS8Uint
                                             ? vk::ImageAspectFlagBits::eDepth | vk::ImageAspectFlagBits::eStencil
                                             : vk::ImageAspectFlags{vk::ImageAspectFlagBits::eDepth};
        VideoCore::UniqueImage depth{device, instance.GetAllocator()};
        VideoCore::UniqueImage color{device, instance.GetAllocator()};
        depth.Create(vk::ImageCreateInfo{
            .imageType = vk::ImageType::e2D,
            .format = format,
            .extent = {W, H, 1},
            .mipLevels = 1,
            .arrayLayers = 1,
            .samples = vk::SampleCountFlagBits::e1,
            .tiling = vk::ImageTiling::eOptimal,
            .usage = vk::ImageUsageFlagBits::eTransferDst | vk::ImageUsageFlagBits::eTransferSrc |
                     vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eDepthStencilAttachment,
            .initialLayout = vk::ImageLayout::eUndefined});
        color.Create(vk::ImageCreateInfo{
            .imageType = vk::ImageType::e2D,
            .format = vk::Format::eR32Sfloat,
            .extent = {W, H, 1},
            .mipLevels = 1,
            .arrayLayers = 1,
            .samples = vk::SampleCountFlagBits::e1,
            .tiling = vk::ImageTiling::eOptimal,
            .usage = vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eTransferSrc,
            .initialLayout = vk::ImageLayout::eUndefined});
        const auto up = staging_pool.Request(N * 4, VideoCore::MemoryType::HostCached, 0, true);
        const auto down_depth = staging_pool.Request(N * 4, VideoCore::MemoryType::HostCached, 0, true);
        const auto down_color = staging_pool.Request(N * 4, VideoCore::MemoryType::HostCached, 0, true);
        std::memcpy(up.mapped, pattern.data(), N * 4);
        up.Flush();
        const auto region = [](vk::ImageAspectFlags aspect, u64 offset) {
            return vk::BufferImageCopy{.bufferOffset = offset,
                                       .imageSubresource{aspect, 0U, 0U, 1U},
                                       .imageExtent = {W, H, 1}};
        };
        const vk::Image d_image = depth;
        const vk::Image c_image = color;
        scheduler.EndRendering();
        scheduler.Record([d = d_image, c = c_image, all, up_buf = up.buffer->Handle(),
                          ur = region(vk::ImageAspectFlagBits::eDepth, up.offset)](vk::CommandBuffer cmdbuf) {
            const auto to_dst =
                LayoutBarrier(d, all, vk::ImageLayout::eUndefined, vk::ImageLayout::eTransferDstOptimal,
                              vk::PipelineStageFlagBits2::eNone, vk::AccessFlagBits2::eNone,
                              vk::PipelineStageFlagBits2::eCopy, vk::AccessFlagBits2::eTransferWrite);
            cmdbuf.pipelineBarrier2(vk::DependencyInfo{.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &to_dst});
            cmdbuf.copyBufferToImage(up_buf, d, vk::ImageLayout::eTransferDstOptimal, ur);
            const std::array barriers{
                LayoutBarrier(d, all, vk::ImageLayout::eTransferDstOptimal,
                              vk::ImageLayout::eDepthStencilReadOnlyOptimal, vk::PipelineStageFlagBits2::eCopy,
                              vk::AccessFlagBits2::eTransferWrite, vk::PipelineStageFlagBits2::eFragmentShader,
                              vk::AccessFlagBits2::eShaderRead),
                LayoutBarrier(c, vk::ImageAspectFlagBits::eColor, vk::ImageLayout::eUndefined,
                              vk::ImageLayout::eColorAttachmentOptimal, vk::PipelineStageFlagBits2::eNone,
                              vk::AccessFlagBits2::eNone, vk::PipelineStageFlagBits2::eColorAttachmentOutput,
                              vk::AccessFlagBits2::eColorAttachmentWrite)};
            cmdbuf.pipelineBarrier2(vk::DependencyInfo{.imageMemoryBarrierCount = u32(barriers.size()),
                                                       .pImageMemoryBarriers = barriers.data()});
        });
        blit_helper->CopyDepthToColor(W, H, format, vk::Format::eR32Sfloat, d_image, c_image);
        scheduler.Record([d = d_image, c = c_image, all, dd = down_depth.buffer->Handle(),
                          dc = down_color.buffer->Handle(),
                          dr = region(vk::ImageAspectFlagBits::eDepth, down_depth.offset),
                          cr = region(vk::ImageAspectFlagBits::eColor, down_color.offset)](vk::CommandBuffer cmdbuf) {
            const std::array barriers{
                LayoutBarrier(d, all, vk::ImageLayout::eDepthStencilReadOnlyOptimal,
                              vk::ImageLayout::eTransferSrcOptimal, vk::PipelineStageFlagBits2::eFragmentShader,
                              vk::AccessFlagBits2::eShaderRead, vk::PipelineStageFlagBits2::eCopy,
                              vk::AccessFlagBits2::eTransferRead),
                LayoutBarrier(c, vk::ImageAspectFlagBits::eColor, vk::ImageLayout::eColorAttachmentOptimal,
                              vk::ImageLayout::eTransferSrcOptimal, vk::PipelineStageFlagBits2::eColorAttachmentOutput,
                              vk::AccessFlagBits2::eColorAttachmentWrite, vk::PipelineStageFlagBits2::eCopy,
                              vk::AccessFlagBits2::eTransferRead)};
            cmdbuf.pipelineBarrier2(vk::DependencyInfo{.imageMemoryBarrierCount = u32(barriers.size()),
                                                       .pImageMemoryBarriers = barriers.data()});
            cmdbuf.copyImageToBuffer(d, vk::ImageLayout::eTransferSrcOptimal, dd, dr);
            cmdbuf.copyImageToBuffer(c, vk::ImageLayout::eTransferSrcOptimal, dc, cr);
            const vk::MemoryBarrier2 to_host{.srcStageMask = vk::PipelineStageFlagBits2::eCopy,
                                             .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
                                             .dstStageMask = vk::PipelineStageFlagBits2::eHost,
                                             .dstAccessMask = vk::AccessFlagBits2::eHostRead};
            cmdbuf.pipelineBarrier2(vk::DependencyInfo{.memoryBarrierCount = 1, .pMemoryBarriers = &to_host});
        });
        scheduler.Finish();
        down_depth.Invalidate();
        down_color.Invalidate();
        const auto* stored = reinterpret_cast<const u32*>(down_depth.mapped);
        const auto* drawn = reinterpret_cast<const u32*>(down_color.mapped);
        u32 changed_on_upload = 0, draw_differs = 0;
        for (u32 i = 0; i < N; ++i) {
            if (stored[i] != pattern[i] && changed_on_upload++ < 4) {
                std::printf("BB_FORM_COPY_VERIFY self-test %s: texel %u loaded %08x, depth plane holds %08x\n",
                            DepthName(format), i, pattern[i], stored[i]);
            }
            if (drawn[i] != stored[i] && draw_differs++ < 8) {
                std::printf("BB_FORM_COPY_VERIFY self-test %s: texel %u depth %08x, draw wrote %08x\n",
                            DepthName(format), i, stored[i], drawn[i]);
            }
        }
        std::printf("BB_FORM_COPY_VERIFY self-test %s: %u texels, %u changed by the upload, %u differ after the draw\n",
                    DepthName(format), N, changed_on_upload, draw_differs);
        std::fflush(stdout);
        staging_pool.FreeDeferred(up);
        staging_pool.FreeDeferred(down_depth);
        staging_pool.FreeDeferred(down_color);
    }
}

// bbport: BB_FORM_COPY_VERIFY=<n> (diagnostic, off by default): the depth->R32F draws are also read back, the
// source's depth plane and the draw's result, and compared bit for bit once the GPU has done them (the draw must
// equal a raw vkCmdCopyImage): the first copy of each target and format, then every n-th, up
// to 256 checks; then the queue drains and a final line says so. The self-test above runs first.
void Runtime::VerifyFormCopy(VideoCore::Image* src, VideoCore::Image* dst) {
    static const u32 every = [] {
        const char* env = std::getenv("BB_FORM_COPY_VERIFY");
        return env ? u32(std::max(0, std::atoi(env))) : 0u;
    }();
    struct Pending {
        StagingBufferRef depth, color;
        u64 texels, tick;
        u32 width, height;
        const char* format;
        VAddr target;
    };
    static constexpr u64 max_checks = 256;
    static std::mutex mutex;
    static std::vector<Pending> pending;
    static std::vector<u64> seen; // target address ^ format: the first copy of each is checked
    static u64 copies = 0, queued = 0, skipped = 0, checked = 0, differing = 0, texels_differing = 0;
    static bool self_tested = false, reported = false;
    if (!every) {
        return;
    }
    std::scoped_lock lk{mutex};
    if (!self_tested) {
        self_tested = true;
        SelfTestFormCopy();
    }
    // Results of earlier checks whose submission the GPU has finished.
    for (auto it = pending.begin(); it != pending.end();) {
        if (!scheduler.IsFree(it->tick)) {
            ++it;
            continue;
        }
        it->depth.Invalidate();
        it->color.Invalidate();
        const auto* d = reinterpret_cast<const u32*>(it->depth.mapped);
        const auto* c = reinterpret_cast<const u32*>(it->color.mapped);
        u64 bad = 0;
        for (u64 i = 0; i < it->texels; ++i) {
            if (d[i] != c[i]) {
                if (bad < 8) {
                    std::printf("BB_FORM_COPY_VERIFY: %s %ux%u texel %llu: depth bits %08x, draw wrote %08x\n",
                                it->format, it->width, it->height, static_cast<unsigned long long>(i), d[i], c[i]);
                }
                ++bad;
            }
        }
        ++checked;
        differing += bad != 0;
        texels_differing += bad;
        std::printf("BB_FORM_COPY_VERIFY: %llu checked, %llu differing (%llu texels); this %s %ux%u at %llx: %llu\n",
                    static_cast<unsigned long long>(checked), static_cast<unsigned long long>(differing),
                    static_cast<unsigned long long>(texels_differing), it->format, it->width, it->height,
                    static_cast<unsigned long long>(it->target), static_cast<unsigned long long>(bad));
        staging_pool.FreeDeferred(it->depth);
        staging_pool.FreeDeferred(it->color);
        it = pending.erase(it);
    }
    if (queued >= max_checks) {
        if (pending.empty() && !reported) {
            reported = true;
            std::printf("BB_FORM_COPY_VERIFY: complete: %llu copies seen, %llu queued, %llu checked, %llu skipped "
                        "(queue full), 0 pending; %llu differing (%llu texels)\n",
                        static_cast<unsigned long long>(copies), static_cast<unsigned long long>(queued),
                        static_cast<unsigned long long>(checked), static_cast<unsigned long long>(skipped),
                        static_cast<unsigned long long>(differing), static_cast<unsigned long long>(texels_differing));
        }
        std::fflush(stdout);
        return;
    }
    std::fflush(stdout);
    const vk::Format src_format = src->backing->image.image_ci.format;
    const u64 key = dst->info.guest_address ^ (u64(src_format) << 48);
    const bool first = std::ranges::find(seen, key) == seen.end();
    if (copies++ % every != 0 && !first) {
        return;
    }
    if (pending.size() >= 4) {
        ++skipped; // a first copy stays unseen and is tried again next time
        return;
    }
    if (first) {
        seen.push_back(key);
    }
    ++queued;
    const u32 w = dst->info.size.width, h = dst->info.size.height;
    const u64 bytes = u64(w) * h * 4;
    bool needs_flush = Transit(src, vk::ImageLayout::eTransferSrcOptimal, vk::PipelineStageFlagBits2::eCopy,
                               vk::AccessFlagBits2::eTransferRead);
    needs_flush |= Transit(dst, vk::ImageLayout::eTransferSrcOptimal, vk::PipelineStageFlagBits2::eCopy,
                           vk::AccessFlagBits2::eTransferRead);
    if (needs_flush) {
        FlushBarriers();
    }
    Pending p{staging_pool.Request(bytes, VideoCore::MemoryType::HostCached, 0, true),
              staging_pool.Request(bytes, VideoCore::MemoryType::HostCached, 0, true),
              u64(w) * h,
              0,
              w,
              h,
              DepthName(src_format),
              dst->info.guest_address};
    const auto region = [w, h](vk::ImageAspectFlags aspect, u64 offset) {
        return vk::BufferImageCopy{.bufferOffset = offset,
                                   .imageSubresource{.aspectMask = aspect, .mipLevel = 0, .baseArrayLayer = 0, .layerCount = 1},
                                   .imageExtent = {w, h, 1}};
    };
    scheduler.Record([src_image = src->GetImage(), dst_image = dst->GetImage(), depth_buf = p.depth.buffer->Handle(),
                      color_buf = p.color.buffer->Handle(), dr = region(vk::ImageAspectFlagBits::eDepth, p.depth.offset),
                      cr = region(vk::ImageAspectFlagBits::eColor, p.color.offset)](vk::CommandBuffer cmdbuf) {
        cmdbuf.copyImageToBuffer(src_image, vk::ImageLayout::eTransferSrcOptimal, depth_buf, dr);
        cmdbuf.copyImageToBuffer(dst_image, vk::ImageLayout::eTransferSrcOptimal, color_buf, cr);
        const vk::MemoryBarrier2 to_host{.srcStageMask = vk::PipelineStageFlagBits2::eCopy,
                                         .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
                                         .dstStageMask = vk::PipelineStageFlagBits2::eHost,
                                         .dstAccessMask = vk::AccessFlagBits2::eHostRead};
        cmdbuf.pipelineBarrier2(vk::DependencyInfo{.memoryBarrierCount = 1, .pMemoryBarriers = &to_host});
    });
    p.tick = scheduler.CurrentTick();
    pending.push_back(p);
}

bool Runtime::CopyColorAndDepth(VideoCore::Image* src, VideoCore::Image* dst) {
    const TransferMark mark{*this, "depth/color form copy", *dst};
    // bbport: two depth images of different formats (DriveClub reuses a D32 with stencil's memory for a
    // D32 when it leaves a race): a direct copy needs the exact same depth format
    // (VUID-vkCmdCopyImage-srcImage-01548), and NVIDIA wrote outside the destination (device lost,
    // WriteInvalid). Both 32-bit float depths go through the copy buffer, depth only; other pairs are
    // not copied (the caller then keeps to the guest's own copy or an upload from memory).
    if (src->info.props.is_depth && dst->info.props.is_depth &&
        src->info.pixel_format != dst->info.pixel_format) {
        const auto float32 = [](vk::Format f) {
            return f == vk::Format::eD32Sfloat || f == vk::Format::eD32SfloatS8Uint;
        };
        const bool same_shape = src->info.size.width == dst->info.size.width &&
                                src->info.size.height == dst->info.size.height &&
                                src->info.size.depth == dst->info.size.depth &&
                                src->info.resources.levels == 1 && dst->info.resources.levels == 1 &&
                                src->info.resources.layers == dst->info.resources.layers;
        static constexpr size_t COPY_BUFFER_SIZE = 128_MB;
        const u64 depth_bytes = u64(src->info.size.width) * src->info.size.height * src->info.size.depth *
                                src->info.resources.layers * 4;
        if (src->info.num_samples == 1 && dst->info.num_samples == 1 && same_shape &&
            float32(src->info.pixel_format) && float32(dst->info.pixel_format) &&
            depth_bytes <= COPY_BUFFER_SIZE) {
            const auto copy_ref = staging_pool.Request(COPY_BUFFER_SIZE, VideoCore::MemoryType::DeviceLocal);
            CopyImageWithBuffer(src, dst, copy_ref.buffer, copy_ref.offset);
            return true;
        }
        return false;
    }
    // bbport: BB_FORM_COPY_SHADER (default on, =0 off): a 32-bit float depth into an R32F image of the same size
    // with a full-screen draw instead of vkCmdCopyImage. DriveClub turns its main depth into R32F twice a frame;
    // the copy took 0.36 ms each at 1440p on NVIDIA (~0.1 ns per pixel and byte).
    static const bool shader_copy = [] {
        const char* env = std::getenv("BB_FORM_COPY_SHADER");
        return !env || env[0] != '0';
    }();
    if (shader_copy && src->info.num_samples == 1 && dst->info.num_samples == 1 && src->info.props.is_depth &&
        !dst->info.props.is_depth && dst->info.pixel_format == vk::Format::eR32Sfloat &&
        (src->info.pixel_format == vk::Format::eD32Sfloat || src->info.pixel_format == vk::Format::eD32SfloatS8Uint) &&
        src->info.size.width == dst->info.size.width && src->info.size.height == dst->info.size.height &&
        src->info.resources.levels == 1 && dst->info.resources.levels == 1 && src->info.resources.layers == 1 &&
        dst->info.resources.layers == 1 && src->info.type == dst->info.type &&
        ConvertImageType(src->info.type) == vk::ImageType::e2D) {
        scheduler.EndRendering();
        bool needs_flush = Transit(src, vk::ImageLayout::eDepthStencilReadOnlyOptimal,
                                   vk::PipelineStageFlagBits2::eFragmentShader, vk::AccessFlagBits2::eShaderRead);
        needs_flush |= Transit(dst, vk::ImageLayout::eColorAttachmentOptimal,
                               vk::PipelineStageFlagBits2::eColorAttachmentOutput,
                               vk::AccessFlagBits2::eColorAttachmentWrite);
        if (needs_flush) {
            FlushBarriers();
        }
        blit_helper->CopyDepthToColor(dst->info.size.width, dst->info.size.height, src->backing->image.image_ci.format,
                                      dst->backing->image.image_ci.format, src->GetImage(), dst->GetImage());
        VerifyFormCopy(src, dst);
        dst->flags |= (src->flags & VideoCore::ImageFlagBits::GpuModified);
        dst->flags &= ~VideoCore::ImageFlagBits::Dirty;
        return true;
    }
    if (src->info.num_samples == 1 && dst->info.num_samples == 1) {
        if (instance.IsMaintenance8Supported() ||
            src->info.props.is_depth == dst->info.props.is_depth) {
            CopyImage(src, dst);
            return true;
        } else {
            // Perform depth from/to color copy using the intermediate copy buffer.
            static constexpr size_t COPY_BUFFER_SIZE = 128_MB;
            const auto copy_ref =
                staging_pool.Request(COPY_BUFFER_SIZE, VideoCore::MemoryType::DeviceLocal);
            CopyImageWithBuffer(src, dst, copy_ref.buffer, copy_ref.offset);
            return true;
        }
    } else if (src->info.num_samples == 1 && dst->info.num_samples > 1 &&
               dst->info.props.is_depth) {
        // Perform a rendering pass to transfer the channels of source as samples in dest.
        bool needs_flush =
            Transit(src, vk::ImageLayout::eShaderReadOnlyOptimal,
                    vk::PipelineStageFlagBits2::eFragmentShader, vk::AccessFlagBits2::eShaderRead);
        needs_flush |= Transit(dst, vk::ImageLayout::eDepthStencilAttachmentOptimal,
                               vk::PipelineStageFlagBits2::eEarlyFragmentTests |
                                   vk::PipelineStageFlagBits2::eLateFragmentTests,
                               vk::AccessFlagBits2::eDepthStencilAttachmentWrite);
        if (needs_flush) {
            FlushBarriers();
        }

        blit_helper->ReinterpretColorAsMsDepth(
            dst->info.size.width, dst->info.size.height, dst->info.num_samples,
            src->info.pixel_format, dst->info.pixel_format, src->GetImage(), dst->GetImage());
        return true;
    } else if (src->info.props.is_depth && src->info.num_samples > 1 && !dst->info.props.is_depth &&
               dst->info.num_samples == 1 && src->info.size.width == dst->info.size.width &&
               src->info.size.height == dst->info.size.height) {
        // The branch above the other way round: the game reads its multisampled depth through a plain color
        // image (screen-space lighting, SSAO, soft particles). Sample i of a pixel goes to channel i; without
        // this copy that image keeps whatever it held before.
        bool needs_flush =
            Transit(src, vk::ImageLayout::eShaderReadOnlyOptimal,
                    vk::PipelineStageFlagBits2::eFragmentShader, vk::AccessFlagBits2::eShaderRead);
        needs_flush |= Transit(dst, vk::ImageLayout::eColorAttachmentOptimal,
                               vk::PipelineStageFlagBits2::eColorAttachmentOutput,
                               vk::AccessFlagBits2::eColorAttachmentWrite);
        if (needs_flush) {
            FlushBarriers();
        }

        blit_helper->ReinterpretMsDepthAsColor(dst->info.size.width, dst->info.size.height,
                                               src->info.pixel_format, dst->info.pixel_format,
                                               src->GetImage(), dst->GetImage());
        dst->flags |= VideoCore::ImageFlagBits::GpuModified;
        dst->flags &= ~VideoCore::ImageFlagBits::Dirty;
        return true;
    } else {
        static std::atomic<u32> warned{0};
        if (warned.fetch_add(1) < 3) {  // it repeats many times a second: only the first few are logged
            LOG_WARNING(Render_Vulkan, "Unimplemented depth overlap copy (later ones are not logged)");
        }
    }
    // A multisampled source, or a multisampled colour destination: the pixels stay where they are
    // and the caller decides what to do instead (the guest's own copy, or an upload from memory).
    return false;
}

void Runtime::CopyDepthStencil(VideoCore::Image* src, VideoCore::Image* dst, vk::ImageAspectFlags aspects,
                               const VideoCore::SubresourceRange& sub_range) {
    scheduler.EndRendering();
    const TransferMark mark{*this, "depth-stencil copy", *dst};

    bool needs_flush =
        Transit(src, vk::ImageLayout::eTransferSrcOptimal, vk::PipelineStageFlagBits2::eCopy,
                vk::AccessFlagBits2::eTransferRead);
    needs_flush |= Transit(dst, vk::ImageLayout::eTransferDstOptimal,
                           vk::PipelineStageFlagBits2::eCopy, vk::AccessFlagBits2::eTransferWrite);
    if (needs_flush) {
        FlushBarriers();
    }

    const auto aspect_mask = src->aspect_mask & dst->aspect_mask & aspects;
    if (!aspect_mask) {
        return;
    }

    const vk::ImageCopy region = {
        .srcSubresource{
            .aspectMask = aspect_mask,
            .mipLevel = 0,
            .baseArrayLayer = sub_range.base.layer,
            .layerCount = sub_range.extent.layers,
        },
        .srcOffset = {0, 0, 0},
        .dstSubresource{
            .aspectMask = aspect_mask,
            .mipLevel = 0,
            .baseArrayLayer = sub_range.base.layer,
            .layerCount = sub_range.extent.layers,
        },
        .dstOffset = {0, 0, 0},
        .extent = {dst->info.size.width, dst->info.size.height, 1},
    };

    scheduler.Record([src_image = src->GetImage(), dst_image = dst->GetImage(),
                      region](vk::CommandBuffer cmdbuf) {
        cmdbuf.copyImage(src_image, vk::ImageLayout::eTransferSrcOptimal, dst_image,
                         vk::ImageLayout::eTransferDstOptimal, region);
    });

    dst->flags |= VideoCore::ImageFlagBits::GpuModified;
    dst->flags &= ~VideoCore::ImageFlagBits::Dirty;
}

void Runtime::ResolveImage(VideoCore::Image* src, VideoCore::Image* dst,
                           const VideoCore::SubresourceRange& src_range,
                           const VideoCore::SubresourceRange& dst_range) {
    const TransferMark mark{*this, "image resolve", *dst};
    SetBackingSamples(dst, 1, false);
    scheduler.EndRendering();

    const bool needs_resolve = src->backing->num_samples != 1;
    const auto dst_stage =
        needs_resolve ? vk::PipelineStageFlagBits2::eResolve : vk::PipelineStageFlagBits2::eCopy;

    bool needs_flush = Transit(src, vk::ImageLayout::eTransferSrcOptimal, dst_stage,
                               vk::AccessFlagBits2::eTransferRead);
    needs_flush |= Transit(dst, vk::ImageLayout::eTransferDstOptimal, dst_stage,
                           vk::AccessFlagBits2::eTransferWrite);
    if (needs_flush) {
        FlushBarriers();
    }

    const auto [src_layers, dst_layers] = SanitizeCopyLayers(src->info, dst->info, 1);
    if (!needs_resolve) {
        const vk::ImageCopy region = {
            .srcSubresource{
                .aspectMask = vk::ImageAspectFlagBits::eColor,
                .mipLevel = 0,
                .baseArrayLayer = src_range.base.layer,
                .layerCount = src_layers,
            },
            .srcOffset = {0, 0, 0},
            .dstSubresource{
                .aspectMask = vk::ImageAspectFlagBits::eColor,
                .mipLevel = 0,
                .baseArrayLayer = dst_range.base.layer,
                .layerCount = dst_layers,
            },
            .dstOffset = {0, 0, 0},
            .extent = {src->info.size.width, src->info.size.height, 1},
        };
        scheduler.Record([src_image = src->GetImage(), dst_image = dst->GetImage(),
                          region](vk::CommandBuffer cmdbuf) {
            cmdbuf.copyImage(src_image, vk::ImageLayout::eTransferSrcOptimal, dst_image,
                             vk::ImageLayout::eTransferDstOptimal, region);
        });
    } else {
        const vk::ImageResolve region = {
            .srcSubresource{
                .aspectMask = vk::ImageAspectFlagBits::eColor,
                .mipLevel = 0,
                .baseArrayLayer = src_range.base.layer,
                .layerCount = src_layers,
            },
            .srcOffset = {0, 0, 0},
            .dstSubresource{
                .aspectMask = vk::ImageAspectFlagBits::eColor,
                .mipLevel = 0,
                .baseArrayLayer = dst_range.base.layer,
                .layerCount = dst_layers,
            },
            .dstOffset = {0, 0, 0},
            .extent = {src->info.size.width, src->info.size.height, 1},
        };
        scheduler.Record([src_image = src->GetImage(), dst_image = dst->GetImage(),
                          region](vk::CommandBuffer cmdbuf) {
            cmdbuf.resolveImage(src_image, vk::ImageLayout::eTransferSrcOptimal, dst_image,
                                vk::ImageLayout::eTransferDstOptimal, region);
        });
    }

    dst->flags |= VideoCore::ImageFlagBits::GpuModified;
    dst->flags &= ~VideoCore::ImageFlagBits::Dirty;
}

void Runtime::ClearDepth(VideoCore::Image* dst, const VideoCore::SubresourceRange& range,
                         float depth) {
    const TransferMark mark{*this, "depth clear", *dst};
    scheduler.EndRendering();
    if (Transit(dst, vk::ImageLayout::eTransferDstOptimal,
                vk::PipelineStageFlagBits2::eClear, vk::AccessFlagBits2::eTransferWrite, range)) {
        FlushBarriers();
    }
    const vk::ImageSubresourceRange vk_range{
        .aspectMask = vk::ImageAspectFlagBits::eDepth,
        .baseMipLevel = range.base.level,
        .levelCount = range.extent.levels,
        .baseArrayLayer = range.base.layer,
        .layerCount = range.extent.layers,
    };
    scheduler.Record([image = dst->GetImage(), depth, vk_range](vk::CommandBuffer cmd) {
        cmd.clearDepthStencilImage(image, vk::ImageLayout::eTransferDstOptimal,
                                   vk::ClearDepthStencilValue{depth, 0}, vk_range);
    });
    dst->flags |= VideoCore::ImageFlagBits::GpuModified;
    dst->flags &= ~VideoCore::ImageFlagBits::Dirty;
}

void Runtime::ClearImage(VideoCore::Image* dst, const VideoCore::SubresourceRange& range,
                         const vk::ClearValue& clear_value) {
    const TransferMark mark{*this, "image clear", *dst};
    scheduler.EndRendering();

    const bool needs_flush =
        Transit(dst, vk::ImageLayout::eTransferDstOptimal, vk::PipelineStageFlagBits2::eClear,
                vk::AccessFlagBits2::eTransferWrite, range);
    if (needs_flush) {
        FlushBarriers();
    }

    const vk::ImageSubresourceRange vk_range = {
        .aspectMask = vk::ImageAspectFlagBits::eColor,
        .baseMipLevel = range.base.level,
        .levelCount = range.extent.levels,
        .baseArrayLayer = range.base.layer,
        .layerCount = range.extent.layers,
    };
    scheduler.Record([image = dst->GetImage(), color = clear_value.color,
                      vk_range](vk::CommandBuffer cmdbuf) {
        cmdbuf.clearColorImage(image, vk::ImageLayout::eTransferDstOptimal, color, vk_range);
    });

    dst->flags |= VideoCore::ImageFlagBits::GpuModified;
    dst->flags &= ~VideoCore::ImageFlagBits::Dirty;
}

void Runtime::SetBackingSamples(VideoCore::Image* image, u32 num_samples, bool copy_backing) {
    BeforeImageAccess();
    auto& backing = image->backing;
    auto& backing_images = image->backing_images;
    const auto& info = image->info;
    if (!backing || backing->num_samples == num_samples) {
        return;
    }
    ASSERT_MSG(!image->info.props.is_depth, "Swapping samples is only valid for color images");
    VideoCore::Image::BackingImage* new_backing;
    auto it = std::ranges::find(backing_images, num_samples,
                                &VideoCore::Image::BackingImage::num_samples);
    if (it == backing_images.end()) {
        auto new_image_ci = backing->image.image_ci;
        new_image_ci.samples = LiverpoolToVK::NumSamples(num_samples, image->supported_samples);

        new_backing = &backing_images.emplace_back();
        new_backing->num_samples = num_samples;
        new_backing->image = VideoCore::UniqueImage{instance.GetDevice(), instance.GetAllocator()};
        new_backing->image.Create(new_image_ci);

        Vulkan::SetObjectName(instance.GetDevice(), new_backing->image.image,
                              "Image {}x{}x{} {} {} {:#x}:{:#x} L:{} M:{} S:{} (backing)",
                              info.size.width, info.size.height, info.size.depth,
                              AmdGpu::NameOf(info.tile_mode), vk::to_string(info.pixel_format),
                              info.guest_address, info.guest_size, info.resources.layers,
                              info.resources.levels, num_samples);
    } else {
        new_backing = std::addressof(*it);
    }

    if (copy_backing) {
        scheduler.EndRendering();
        ASSERT(image->info.resources.levels == 1 && image->info.resources.layers == 1);

        // Transition current backing to shader read layout
        Transit(image, vk::ImageLayout::eShaderReadOnlyOptimal,
                vk::PipelineStageFlagBits2::eFragmentShader, vk::AccessFlagBits2::eShaderRead);

        // Transition dest backing to color attachment layout, not caring of previous contents
        constexpr auto dst_stage = vk::PipelineStageFlagBits2::eColorAttachmentOutput;
        constexpr auto dst_access =
            vk::AccessFlagBits2::eColorAttachmentRead | vk::AccessFlagBits2::eColorAttachmentWrite;
        constexpr auto dst_layout = vk::ImageLayout::eColorAttachmentOptimal;
        image_barriers.push_back(vk::ImageMemoryBarrier2{
            .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
            .srcAccessMask = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite,
            .dstStageMask = dst_stage,
            .dstAccessMask = dst_access,
            .oldLayout = vk::ImageLayout::eUndefined,
            .newLayout = dst_layout,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = new_backing->image,
            .subresourceRange{
                .aspectMask = image->aspect_mask,
                .baseMipLevel = 0,
                .levelCount = 1,
                .baseArrayLayer = 0,
                .layerCount = image->info.resources.layers,
            },
        });
        FlushBarriers();

        // Copy between ms and non ms backing images
        blit_helper->CopyBetweenMsImages(
            info.size.width, info.size.height, new_backing->num_samples, info.pixel_format,
            backing->num_samples > 1, backing->image, new_backing->image);

        // Update current layout in tracker to new backings layout
        new_backing->state.layout = dst_layout;
        new_backing->state.access_mask = dst_access;
        new_backing->state.pl_stage = dst_stage;
    }

    backing = new_backing;
}

bool Runtime::IsBufferAccessed(const VideoCore::Buffer* handle, u64 offset, u64 size,
                               bool check_read_access) {
    const AddressRange range = {
        .resource = reinterpret_cast<u64>(handle),
        .range_start = offset,
        .range_end = offset + size - 1,
    };
    bool has_access = barrier_tracker.FindRange(range, Access::Write);
    if (check_read_access && !has_access) {
        has_access |= barrier_tracker.FindRange(range, Access::Read);
    }
    return has_access;
}

void Runtime::AccessBuffer(const VideoCore::Buffer* handle, u64 offset, u64 size,
                           vk::PipelineStageFlags2 src_stage, vk::AccessFlags2 src_access) {
    AddressRange range = {
        .resource = reinterpret_cast<u64>(handle),
        .range_start = offset,
        .range_end = offset + size - 1,
    };

    constexpr static vk::AccessFlags2 READ_MASK =
        vk::AccessFlagBits2::eIndexRead | vk::AccessFlagBits2::eVertexAttributeRead |
        vk::AccessFlagBits2::eUniformRead | vk::AccessFlagBits2::eShaderRead |
        vk::AccessFlagBits2::eColorAttachmentRead |
        vk::AccessFlagBits2::eDepthStencilAttachmentRead | vk::AccessFlagBits2::eTransferRead |
        vk::AccessFlagBits2::eMemoryRead;

    constexpr static vk::AccessFlags2 WRITE_MASK =
        vk::AccessFlagBits2::eShaderWrite | vk::AccessFlagBits2::eColorAttachmentWrite |
        vk::AccessFlagBits2::eDepthStencilAttachmentWrite | vk::AccessFlagBits2::eTransferWrite |
        vk::AccessFlagBits2::eMemoryWrite | vk::AccessFlagBits2::eTransformFeedbackWriteEXT;

    // bbport: reads are tracked at 4 KiB granularity. Constant data comes from ring allocations
    // at a new offset every draw; rounded, they land in ranges already present and the
    // insert returns early instead of growing the tree until the next barrier flush.
    // Wider read ranges only add barriers, never drop one.
    if (!(src_access & WRITE_MASK) && !BbToggle::Disabled(BbToggle::CoarseReadTracking)) {
        range.range_start &= ~u64{0xFFF};
        range.range_end |= 0xFFF;
    }
    const auto insert = [&](Access access) {
        if (BbToggle::Disabled(BbToggle::AccessMemo)) {
            barrier_tracker.InsertRange(range, access);
            return;
        }
        u64 hash = range.resource ^ range.range_start * 0x9E3779B97F4A7C15ull ^
                   range.range_end << 13 ^ u64(access) << 61;
        hash = (hash ^ hash >> 31) * 0xFF51AFD7ED558CCDull;
        auto& memo = access_memo[(hash ^ hash >> 29) % access_memo.size()];
        if (memo.epoch == access_epoch && memo.resource == range.resource &&
            memo.start == range.range_start && memo.end == range.range_end &&
            memo.access == u32(access)) {
            return;
        }
        barrier_tracker.InsertRange(range, access);
        memo = {range.resource, range.range_start, range.range_end, access_epoch, u32(access)};
    };
    if (src_access & WRITE_MASK) {
        insert(Access::Write);
    }
    if (src_access & READ_MASK) {
        insert(Access::Read);
    }

    memory_barrier.srcStageMask |= src_stage;
    memory_barrier.srcAccessMask |= src_access & WRITE_MASK;
}

void Runtime::FlushBarriers() {
    BeforeImageAccess();
    vk::DependencyInfo dep_info{};

    if (memory_barrier.srcStageMask) {
        dep_info.pMemoryBarriers = &memory_barrier;
        dep_info.memoryBarrierCount = 1U;
    }
    if (!image_barriers.empty()) {
        dep_info.pImageMemoryBarriers = image_barriers.data();
        dep_info.imageMemoryBarrierCount = static_cast<u32>(image_barriers.size());
    }

    if (!dep_info.memoryBarrierCount && !dep_info.imageMemoryBarrierCount) {
        return;
    }

    scheduler.EndRendering();
    // bbport: with the GPU profiler on, a barrier that changes the layout of a depth/stencil image gets its own
    // label (a depth decompression on some GPUs), keyed by the layouts and size.
    GpuProfiler* profiler = GpuProfiler::Get();
    u64 resume = 0;
    if (profiler && profiler->Records(&scheduler)) {
        for (const auto& b : image_barriers) {
            if ((b.subresourceRange.aspectMask & vk::ImageAspectFlagBits::eDepth) && b.oldLayout != b.newLayout) {
                resume = profiler->Current();
                const u64 key = 0xDE7B000000000000ull ^ (u64(b.oldLayout) << 32) ^ (u64(b.newLayout) << 16) ^
                                u64(image_barriers.size());
                profiler->Mark(key, [&] {
                    return fmt::format("depth layout barrier {} -> {} ({} image barriers)", vk::to_string(b.oldLayout),
                                       vk::to_string(b.newLayout), image_barriers.size());
                });
                break;
            }
        }
    }
    scheduler.Record([memory = memory_barrier, has_memory = dep_info.memoryBarrierCount != 0,
                      images = scheduler.RecordData(std::span<const vk::ImageMemoryBarrier2>(
                          image_barriers.data(), image_barriers.size()))](vk::CommandBuffer cmdbuf) {
        const vk::DependencyInfo info = {
            .memoryBarrierCount = has_memory ? 1U : 0U,
            .pMemoryBarriers = has_memory ? &memory : nullptr,
            .imageMemoryBarrierCount = static_cast<u32>(images.size()),
            .pImageMemoryBarriers = images.data(),
        };
        cmdbuf.pipelineBarrier2(info);
    });

    memory_barrier.srcStageMask = vk::PipelineStageFlagBits2::eNone;
    memory_barrier.srcAccessMask = vk::AccessFlagBits2::eNone;

    image_barriers.clear();
    barrier_tracker.Clear();
    ++access_epoch;
    if (resume) {
        profiler->Resume(resume);
    }
}

// Capture aid: the contents of a texture sampled by a vertex shader (height and wind maps), copied out
// as raw blocks into BB_DUMP_DIR once per capture.
void DumpVertexTexture(const Instance& instance, Scheduler& scheduler, Runtime& runtime,
                              VideoCore::Image& image) {
#if BB_DIAGNOSTICS

    static std::set<u64> done;
    static u64 bucket = 0;
    const u64 now_bucket = u64(std::time(nullptr)) / 3;
    if (now_bucket != bucket) {
        bucket = now_bucket;
        done.clear();
    }
    const auto& info = image.info;
    const u64 key = info.guest_address ^ (u64(info.size.width) << 40) ^ (u64(info.size.height) << 52);
    if (done.size() >= 200 || !done.insert(key).second) {
        return;
    }
    const auto fmt = info.pixel_format;
    const u32 w = info.size.width, h = info.size.height;
    const auto block = vk::blockExtent(fmt);
    const u32 bs = vk::blockSize(fmt);
    const u64 size = u64((w + block[0] - 1) / block[0]) * ((h + block[1] - 1) / block[1]) * bs;
    if (!size || size > (64u << 20)) {
        return;
    }
    const VkBufferCreateInfo buffer_ci{.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                                       .size = size,
                                       .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    const VmaAllocationCreateInfo alloc_ci{
        .flags = VMA_ALLOCATION_CREATE_MAPPED_BIT | VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT,
        .usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST};
    VkBuffer buffer{};
    VmaAllocation allocation{};
    VmaAllocationInfo ainfo{};
    BbStats::alloc_count[2].fetch_add(1, std::memory_order_relaxed);
    BbStats::alloc_bytes[2].fetch_add(buffer_ci.size, std::memory_order_relaxed);
    if (vmaCreateBuffer(instance.GetAllocator(), &buffer_ci, &alloc_ci, &buffer, &allocation,
                        &ainfo) != VK_SUCCESS) {
        return;
    }
    scheduler.EndRendering();
    runtime.Transit(&image, vk::ImageLayout::eTransferSrcOptimal, vk::PipelineStageFlagBits2::eCopy,
                    vk::AccessFlagBits2::eTransferRead);
    runtime.FlushBarriers();
    scheduler.Record([img = image.GetImage(), buffer, w, h](vk::CommandBuffer cmdbuf) {
        const vk::BufferImageCopy region{
            .imageSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
            .imageExtent = {w, h, 1}};
        cmdbuf.copyImageToBuffer(img, vk::ImageLayout::eTransferSrcOptimal, buffer, region);
    });
    const char* dir = BB_DIAGNOSTIC_ENV("BB_DUMP_DIR");
    char path[600];
    std::snprintf(path, sizeof(path), "%s/vtex_%llx_%s_%ux%u.raw", dir && *dir ? dir : "out/dump",
                  static_cast<unsigned long long>(info.guest_address), vk::to_string(fmt).c_str(), w, h);
    scheduler.DeferPriorityOperation([allocator = instance.GetAllocator(), buffer, allocation, ainfo,
                                      size, file = std::string{path}] {
        vmaInvalidateAllocation(allocator, allocation, 0, VK_WHOLE_SIZE);
        if (FILE* f = std::fopen(file.c_str(), "wb")) {
            std::fwrite(ainfo.pMappedData, 1, size, f);
            std::fclose(f);
        }
        vmaDestroyBuffer(allocator, buffer, allocation);
    });

#endif
}


} // namespace Vulkan
