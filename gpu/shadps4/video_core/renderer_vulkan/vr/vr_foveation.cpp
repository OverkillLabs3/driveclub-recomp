// SPDX-License-Identifier: GPL-2.0-or-later
// VR fixed foveated rendering: see vr_foveation.h.

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <vector>

#include "bbport_vr.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vr/vr_foveation.h"
#include "video_core/renderer_vulkan/vr/vr_multiview.h"
#include "video_core/texture_cache/image.h"

#include <vk_mem_alloc.h>

namespace Vulkan::Vr::Foveation {

namespace {

bool g_supported = false;
vk::Extent2D g_texel{16, 16};

// One map per pass size (the render size; DLSS's other sizes keep their own).
struct Map {
    u32 width = 0, height = 0; // the pass
    u64 params = 0;            // zones and tangents it was made for
    VideoCore::UniqueImage image;
    vk::UniqueImageView view;
};
std::mutex g_mutex;
std::vector<Map> g_maps;

// Rate codes of the attachment: log2(width) << 2 | log2(height).
constexpr u8 Rate1x1 = 0, Rate2x2 = (1 << 2) | 1, Rate4x4 = (2 << 2) | 2;

u64 Params(float right, float up, float down, float inner, float outer, float lower) {
    const auto q = [](float v) { return u64(std::lround(v * 1000.0f)) & 0xffff; };
    return q(right) | q(up) << 16 | q(down) << 32 | (q(inner) ^ q(outer) << 5 ^ q(lower) << 10) << 48;
}

} // namespace

bool Wanted() {
    return Multiview::Requested() && BbVr::FfrWanted();
}

void OnDevice(bool supported, u32 texel_width, u32 texel_height) {
    g_supported = supported;
    g_texel = {std::max(texel_width, 1u), std::max(texel_height, 1u)};
    if (Wanted()) {
        std::printf("VR: foveated rendering %s (one rate per %ux%u pixels)\n",
                    supported ? "on" : "asked for, but the graphics card has no attachment shading rate", g_texel.width,
                    g_texel.height);
        std::fflush(stdout);
    }
}

bool Supported() {
    return g_supported;
}

vk::Extent2D TexelSize() {
    return g_texel;
}

vk::ImageView Prepare(const Instance& instance, Scheduler& scheduler, u32 width, u32 height) {
    if (!g_supported || !BbVr::FfrOn() || !width || !height) {
        return {};
    }
    float right = 1.0f, up = 1.0f, down = 1.0f; // 45 degrees each way until the runtime's are known
    BbVr::DrawnTangents(right, up, down);
    float inner = 35.0f, outer = 42.0f, lower = 0.8f;
    BbVr::FfrZones(inner, outer, lower);
    const u64 params = Params(right, up, down, inner, outer, lower);

    std::scoped_lock lock{g_mutex};
    auto it = std::find_if(g_maps.begin(), g_maps.end(), [&](const Map& m) { return m.width == width && m.height == height; });
    if (it != g_maps.end() && it->params == params) {
        return *it->view;
    }
    if (it == g_maps.end()) {
        g_maps.emplace_back();
        it = std::prev(g_maps.end());
        it->width = width, it->height = height;
    }
    Map& map = *it;
    const u32 mw = (width + g_texel.width - 1) / g_texel.width, mh = (height + g_texel.height - 1) / g_texel.height;

    // The rates: by the angle from the middle of the eye (the view straight ahead), the lower part brought nearer.
    std::vector<u8> rates(size_t(mw) * mh);
    const float tan_inner = std::tan(inner * 3.14159265f / 180.0f), tan_outer = std::tan(outer * 3.14159265f / 180.0f);
    u32 counts[3]{};
    for (u32 y = 0; y < mh; ++y) {
        const float fy = (float(y) + 0.5f) * float(g_texel.height) / float(height); // 0 top, 1 bottom
        float ty = up - fy * (up + down);
        if (ty < 0.0f) {
            ty *= lower;
        }
        for (u32 x = 0; x < mw; ++x) {
            const float fx = (float(x) + 0.5f) * float(g_texel.width) / float(width);
            const float tx = (fx * 2.0f - 1.0f) * right;
            const float t = std::sqrt(tx * tx + ty * ty);
            const u8 rate = t < tan_inner ? Rate1x1 : t < tan_outer ? Rate2x2 : Rate4x4;
            rates[size_t(y) * mw + x] = rate;
            ++counts[rate == Rate1x1 ? 0 : rate == Rate2x2 ? 1 : 2];
        }
    }

    // A new image each time (the old one may be in recorded work): two layers, one per eye, the same map.
    const vk::Device device = instance.GetDevice();
    VideoCore::UniqueImage image{device, instance.GetAllocator()};
    image.Create(vk::ImageCreateInfo{
        .imageType = vk::ImageType::e2D,
        .format = vk::Format::eR8Uint,
        .extent = {mw, mh, 1},
        .mipLevels = 1,
        .arrayLayers = 2,
        .samples = vk::SampleCountFlagBits::e1,
        .tiling = vk::ImageTiling::eOptimal,
        .usage = vk::ImageUsageFlagBits::eFragmentShadingRateAttachmentKHR | vk::ImageUsageFlagBits::eTransferDst,
        .initialLayout = vk::ImageLayout::eUndefined,
    });
    auto [view_result, view] = device.createImageViewUnique(vk::ImageViewCreateInfo{
        .image = vk::Image(image),
        .viewType = vk::ImageViewType::e2DArray,
        .format = vk::Format::eR8Uint,
        .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 2},
    });
    if (view_result != vk::Result::eSuccess) {
        return {};
    }

    // The upload: a host buffer with both layers, copied in, the image then left as the rate attachment.
    const VkDeviceSize bytes = VkDeviceSize(rates.size()) * 2;
    const VkBufferCreateInfo buffer_ci{
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = bytes,
        .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
    };
    const VmaAllocationCreateInfo alloc_ci{
        .flags = VMA_ALLOCATION_CREATE_MAPPED_BIT | VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT,
        .usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
    };
    VkBuffer buffer{};
    VmaAllocation allocation{};
    VmaAllocationInfo info{};
    if (vmaCreateBuffer(instance.GetAllocator(), &buffer_ci, &alloc_ci, &buffer, &allocation, &info) != VK_SUCCESS) {
        return {};
    }
    std::memcpy(info.pMappedData, rates.data(), rates.size());
    std::memcpy(static_cast<u8*>(info.pMappedData) + rates.size(), rates.data(), rates.size());
    vmaFlushAllocation(instance.GetAllocator(), allocation, 0, VK_WHOLE_SIZE);

    scheduler.EndRendering();
    scheduler.Record([image = vk::Image(image), buffer, mw, mh, layer_bytes = VkDeviceSize(rates.size())](
                         vk::CommandBuffer cmdbuf) {
        const vk::ImageMemoryBarrier2 to_copy{
            .srcStageMask = vk::PipelineStageFlagBits2::eNone,
            .dstStageMask = vk::PipelineStageFlagBits2::eCopy,
            .dstAccessMask = vk::AccessFlagBits2::eTransferWrite,
            .oldLayout = vk::ImageLayout::eUndefined,
            .newLayout = vk::ImageLayout::eTransferDstOptimal,
            .image = image,
            .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 2},
        };
        cmdbuf.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &to_copy});
        const std::array<vk::BufferImageCopy, 2> regions{{
            {.bufferOffset = 0, .imageSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1}, .imageExtent = {mw, mh, 1}},
            {.bufferOffset = layer_bytes,
             .imageSubresource = {vk::ImageAspectFlagBits::eColor, 0, 1, 1},
             .imageExtent = {mw, mh, 1}},
        }};
        cmdbuf.copyBufferToImage(vk::Buffer(buffer), image, vk::ImageLayout::eTransferDstOptimal, regions);
        const vk::ImageMemoryBarrier2 to_rate{
            .srcStageMask = vk::PipelineStageFlagBits2::eCopy,
            .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eFragmentShadingRateAttachmentKHR,
            .dstAccessMask = vk::AccessFlagBits2::eFragmentShadingRateAttachmentReadKHR,
            .oldLayout = vk::ImageLayout::eTransferDstOptimal,
            .newLayout = vk::ImageLayout::eFragmentShadingRateAttachmentOptimalKHR,
            .image = image,
            .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 2},
        };
        cmdbuf.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &to_rate});
    });
    // The buffer, and the old map, once the GPU is past this work.
    scheduler.DeferPriorityOperation([allocator = instance.GetAllocator(), buffer, allocation,
                                      old_image = std::make_shared<VideoCore::UniqueImage>(std::move(map.image)),
                                      old_view = std::make_shared<vk::UniqueImageView>(std::move(map.view))] {
        vmaDestroyBuffer(allocator, buffer, allocation);
    });
    map.image = std::move(image);
    map.view = std::move(view);
    map.params = params;

    const float all = float(rates.size());
    std::printf("VR: foveated rendering map for %ux%u: %.0f%% full rate, %.0f%% 2x2, %.0f%% 4x4 (inside %.0f, %.0f "
                "degrees; lower part x%.2f)\n",
                width, height, 100.0f * counts[0] / all, 100.0f * counts[1] / all, 100.0f * counts[2] / all, inner,
                outer, lower);
    std::fflush(stdout);
    return *map.view;
}

} // namespace Vulkan::Vr::Foveation
