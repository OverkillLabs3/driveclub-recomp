// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: BB_CUBE_LOD_RANGE_SELFTEST, see vk_cube_lod_check.h and BB_CUBE_LOD_RANGE in vk_rasterizer.cpp.

#include <array>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <random>
#include <vector>
#include "video_core/buffer_cache/buffer.h"
#include "video_core/host_shaders/cube_lod_check_comp.h"
#include "video_core/host_shaders/cube_lod_check_cube_comp.h"
#include "video_core/renderer_vulkan/vk_cube_lod_check.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_platform.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"
#include "video_core/texture_cache/image.h"

namespace Vulkan {

namespace {

/// One random texel of `format` with every float component finite. Returns its size, 0 for other formats.
u32 RandomTexel(vk::Format format, std::mt19937& rng, u8* out) {
    switch (format) {
    case vk::Format::eR16G16B16A16Sfloat:
        for (u32 c = 0; c < 4; ++c) {
            u16 h = u16(rng());
            if (((h >> 10) & 31) == 31) {
                h &= ~u16(1u << 10); // exponent 31 (Inf/NaN) -> 30
            }
            std::memcpy(out + c * 2, &h, 2);
        }
        return 8;
    case vk::Format::eB10G11R11UfloatPack32: {
        const auto finite = [&](u32 bits, u32 mantissa) {
            u32 v = rng() & ((1u << bits) - 1);
            if (((v >> mantissa) & 31) == 31) {
                v &= ~(1u << mantissa);
            }
            return v;
        };
        const u32 v = finite(11, 6) | (finite(11, 6) << 11) | (finite(10, 5) << 22);
        std::memcpy(out, &v, 4);
        return 4;
    }
    case vk::Format::eR32G32B32A32Sfloat:
        for (u32 c = 0; c < 4; ++c) {
            u32 f = rng();
            if (((f >> 23) & 255) == 255) {
                f &= ~(1u << 23);
            }
            std::memcpy(out + c * 4, &f, 4);
        }
        return 16;
    case vk::Format::eR8G8B8A8Unorm:
    case vk::Format::eR8G8B8A8Srgb:
    case vk::Format::eB8G8R8A8Unorm:
    case vk::Format::eB8G8R8A8Srgb:
    case vk::Format::eA2B10G10R10UnormPack32: {
        const u32 v = rng();
        std::memcpy(out, &v, 4);
        return 4;
    }
    default:
        return 0;
    }
}

} // namespace

bool CubeLodSelfTest(const Instance& instance, vk::Sampler reference, vk::Sampler candidate, vk::Format format,
                     u32 levels, const vk::ComponentMapping& mapping, const char* what, bool cube) {
    constexpr u32 layers = 6, count = 1u << 16;
    if (levels < 1 || levels > 10) {
        std::printf("BB_CUBE_LOD_RANGE_SELFTEST: %u levels not covered: FAIL (not run)\n", levels);
        return false;
    }
    const u32 size = 1u << (levels - 1), lods = levels + 2;
    struct Constants {
        u32 count, layers, lods, size;
    };
    std::mt19937 rng{20261023};
    std::array<u8, 16> texel{};
    const u32 texel_bytes = RandomTexel(format, rng, texel.data());
    if (!texel_bytes) {
        std::printf("BB_CUBE_LOD_RANGE_SELFTEST: format %s not covered: FAIL (not run)\n",
                    vk::to_string(format).c_str());
        return false;
    }
    const auto device = instance.GetDevice();

    // Random levels: one upload buffer, level after level, each with all layers.
    std::vector<u64> level_offset(levels);
    u64 upload_size = 0;
    for (u32 level = 0; level < levels; ++level) {
        level_offset[level] = upload_size;
        upload_size += u64(size >> level) * (size >> level) * layers * texel_bytes;
    }
    VideoCore::Buffer upload{instance, 0, upload_size, VideoCore::MemoryType::HostUncached, "cube lod check upload"};
    VideoCore::Buffer results[2] = {
        VideoCore::Buffer{instance, 0, count * 16ull, VideoCore::MemoryType::HostCached, "cube lod check A"},
        VideoCore::Buffer{instance, 0, count * 16ull, VideoCore::MemoryType::HostCached, "cube lod check B"},
    };
    if (upload.mapped_data.empty() || results[0].mapped_data.empty() || results[1].mapped_data.empty()) {
        std::printf("BB_CUBE_LOD_RANGE_SELFTEST: no host mapping: FAIL (not run)\n");
        return false;
    }
    for (u64 offset = 0; offset < upload_size; offset += texel_bytes) {
        RandomTexel(format, rng, upload.mapped_data.data() + offset);
    }
    upload.Flush(0, upload_size);
    std::memset(results[0].mapped_data.data(), 0xcd, count * 16);
    std::memset(results[1].mapped_data.data(), 0xab, count * 16);
    results[0].Flush(0, count * 16);
    results[1].Flush(0, count * 16);

    VideoCore::UniqueImage image{device, instance.GetAllocator()};
    image.Create(vk::ImageCreateInfo{
        .flags = cube ? vk::ImageCreateFlagBits::eCubeCompatible : vk::ImageCreateFlags{},
        .imageType = vk::ImageType::e2D,
        .format = format,
        .extent = {size, size, 1},
        .mipLevels = levels,
        .arrayLayers = layers,
        .samples = vk::SampleCountFlagBits::e1,
        .tiling = vk::ImageTiling::eOptimal,
        .usage = vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferDst,
        .sharingMode = vk::SharingMode::eExclusive,
        .initialLayout = vk::ImageLayout::eUndefined,
    });
    const vk::ImageSubresourceRange all{vk::ImageAspectFlagBits::eColor, 0, levels, 0, layers};
    const auto view = Check(device.createImageViewUnique({
        .image = image.image,
        .viewType = cube ? vk::ImageViewType::eCubeArray : vk::ImageViewType::e2DArray,
        .format = format,
        .components = mapping,
        .subresourceRange = all,
    }));

    const std::array<vk::DescriptorSetLayoutBinding, 3> bindings = {{
        {.binding = 0,
         .descriptorType = vk::DescriptorType::eSampledImage,
         .descriptorCount = 1,
         .stageFlags = vk::ShaderStageFlagBits::eCompute},
        {.binding = 1,
         .descriptorType = vk::DescriptorType::eSampler,
         .descriptorCount = 1,
         .stageFlags = vk::ShaderStageFlagBits::eCompute},
        {.binding = 2,
         .descriptorType = vk::DescriptorType::eStorageBuffer,
         .descriptorCount = 1,
         .stageFlags = vk::ShaderStageFlagBits::eCompute},
    }};
    const auto set_layout = Check(device.createDescriptorSetLayoutUnique({
        .flags = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR,
        .bindingCount = static_cast<u32>(bindings.size()),
        .pBindings = bindings.data(),
    }));
    const vk::PushConstantRange push_range{
        .stageFlags = vk::ShaderStageFlagBits::eCompute, .offset = 0, .size = sizeof(Constants)};
    const auto layout = Check(device.createPipelineLayoutUnique({
        .setLayoutCount = 1,
        .pSetLayouts = &*set_layout,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &push_range,
    }));
    const auto module =
        cube ? CompileSPV(CUBE_LOD_CHECK_CUBE_COMP, device) : CompileSPV(CUBE_LOD_CHECK_COMP, device);
    const auto pipeline = Check(device.createComputePipelineUnique(
        {}, vk::ComputePipelineCreateInfo{
                .stage = {.stage = vk::ShaderStageFlagBits::eCompute, .module = module, .pName = "main"},
                .layout = *layout,
            }));
    device.destroyShaderModule(module);

    const auto pool = Check(device.createCommandPoolUnique({
        .flags = vk::CommandPoolCreateFlagBits::eTransient,
        .queueFamilyIndex = instance.GetGraphicsQueueFamilyIndex(),
    }));
    auto cmdbufs = Check(device.allocateCommandBuffersUnique({
        .commandPool = *pool,
        .level = vk::CommandBufferLevel::ePrimary,
        .commandBufferCount = 1,
    }));
    const vk::CommandBuffer cmdbuf = *cmdbufs[0];
    Check(cmdbuf.begin({.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit}));

    const auto image_barrier = [&](vk::ImageLayout from, vk::ImageLayout to, vk::PipelineStageFlags2 src_stage,
                                   vk::AccessFlags2 src_access, vk::PipelineStageFlags2 dst_stage,
                                   vk::AccessFlags2 dst_access) {
        const vk::ImageMemoryBarrier2 barrier{
            .srcStageMask = src_stage,
            .srcAccessMask = src_access,
            .dstStageMask = dst_stage,
            .dstAccessMask = dst_access,
            .oldLayout = from,
            .newLayout = to,
            .image = image.image,
            .subresourceRange = all,
        };
        cmdbuf.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &barrier});
    };
    image_barrier(vk::ImageLayout::eUndefined, vk::ImageLayout::eTransferDstOptimal,
                  vk::PipelineStageFlagBits2::eNone, vk::AccessFlagBits2::eNone,
                  vk::PipelineStageFlagBits2::eCopy, vk::AccessFlagBits2::eTransferWrite);
    std::vector<vk::BufferImageCopy> copies(levels);
    for (u32 level = 0; level < levels; ++level) {
        copies[level] = vk::BufferImageCopy{
            .bufferOffset = level_offset[level],
            .imageSubresource = {vk::ImageAspectFlagBits::eColor, level, 0, layers},
            .imageExtent = {size >> level, size >> level, 1},
        };
    }
    cmdbuf.copyBufferToImage(upload.Handle(), image.image, vk::ImageLayout::eTransferDstOptimal, copies);
    image_barrier(vk::ImageLayout::eTransferDstOptimal, vk::ImageLayout::eShaderReadOnlyOptimal,
                  vk::PipelineStageFlagBits2::eCopy, vk::AccessFlagBits2::eTransferWrite,
                  vk::PipelineStageFlagBits2::eComputeShader, vk::AccessFlagBits2::eShaderSampledRead);

    cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, *pipeline);
    const Constants constants{count, layers, lods, size};
    cmdbuf.pushConstants(*layout, vk::ShaderStageFlagBits::eCompute, 0, sizeof(constants), &constants);
    const vk::Sampler samplers[2] = {reference, candidate};
    for (u32 i = 0; i < 2; ++i) {
        const vk::DescriptorImageInfo image_info{.imageView = *view,
                                                 .imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal};
        const vk::DescriptorImageInfo sampler_info{.sampler = samplers[i]};
        const vk::DescriptorBufferInfo buffer_info{.buffer = results[i].Handle(), .offset = 0, .range = count * 16ull};
        const std::array<vk::WriteDescriptorSet, 3> writes = {{
            {.dstBinding = 0,
             .descriptorCount = 1,
             .descriptorType = vk::DescriptorType::eSampledImage,
             .pImageInfo = &image_info},
            {.dstBinding = 1,
             .descriptorCount = 1,
             .descriptorType = vk::DescriptorType::eSampler,
             .pImageInfo = &sampler_info},
            {.dstBinding = 2,
             .descriptorCount = 1,
             .descriptorType = vk::DescriptorType::eStorageBuffer,
             .pBufferInfo = &buffer_info},
        }};
        cmdbuf.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, *layout, 0, writes);
        cmdbuf.dispatch(count / 64, 1, 1);
    }
    const vk::MemoryBarrier2 to_host{
        .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
        .srcAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eHost,
        .dstAccessMask = vk::AccessFlagBits2::eHostRead,
    };
    cmdbuf.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &to_host});
    Check(cmdbuf.end());

    const auto fence = Check(device.createFenceUnique({}));
    {
        std::scoped_lock lk{Scheduler::submit_mutex};
        const vk::SubmitInfo submit{.commandBufferCount = 1, .pCommandBuffers = &cmdbuf};
        if (instance.GetGraphicsQueue().submit(submit, *fence) != vk::Result::eSuccess) {
            std::printf("BB_CUBE_LOD_RANGE_SELFTEST: submit failed: FAIL (not run)\n");
            return false;
        }
    }
    if (device.waitForFences(*fence, VK_TRUE, 5'000'000'000ull) != vk::Result::eSuccess) {
        std::printf("BB_CUBE_LOD_RANGE_SELFTEST: GPU did not finish in 5 s: FAIL\n");
        // The command buffer may still run: keep its resources alive.
        std::exit(1);
    }
    results[0].Invalidate(0, count * 16);
    results[1].Invalidate(0, count * 16);

    u32 same = 0, shown = 0;
    for (u32 i = 0; i < count; ++i) {
        const u8* a = results[0].mapped_data.data() + i * 16ull;
        const u8* b = results[1].mapped_data.data() + i * 16ull;
        if (std::memcmp(a, b, 16) == 0) {
            ++same;
        } else if (shown++ < 4) {
            u32 va[4], vb[4];
            std::memcpy(va, a, 16);
            std::memcpy(vb, b, 16);
            std::printf("BB_CUBE_LOD_RANGE_SELFTEST: sample %u (lod %u) %08x %08x %08x %08x vs %08x %08x %08x %08x\n",
                        i, i % lods, va[0], va[1], va[2], va[3], vb[0], vb[1], vb[2], vb[3]);
        }
    }
    // A sampler that wrote nothing leaves the fill pattern: identical results must also differ from it.
    u32 written = 0;
    for (u32 i = 0; i < count; ++i) {
        u32 v;
        std::memcpy(&v, results[0].mapped_data.data() + i * 16ull, 4);
        written += v != 0xcdcdcdcdu;
    }
    const bool pass = same == count && written == count;
    std::printf("BB_CUBE_LOD_RANGE_SELFTEST: %s, format %s, %u of %u samples identical (%ux%u, 6 layers%s, %u "
                "levels, LODs 0-%u), %u written: %s\n",
                what, vk::to_string(format).c_str(), same, count, size, size, cube ? " as a cube" : "", levels,
                lods - 1, written,
                pass ? "PASS" : "FAIL");
    return pass;
}

} // namespace Vulkan
