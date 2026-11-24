// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: see vk_null_resources.h.

#include <algorithm>
#include <mutex>
#include "common/assert.h"
#include "common/logging/log.h"
#include "shader_recompiler/resource.h"
#include "video_core/amdgpu/pixel_format.h"
#include "video_core/buffer_cache/buffer.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_null_resources.h"
#include "video_core/renderer_vulkan/vk_platform.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/texture_cache/image.h"

namespace Vulkan {

namespace {

constexpr vk::DeviceSize BufferBytes = 64u << 10;
// The 2D image backs 2D, 2D array and cube array views: six layers, cube compatible.
constexpr u32 Layers2D = 6;

constexpr vk::Format KindFormat(u32 kind) {
    switch (kind) {
    case 1:
        return vk::Format::eR8G8B8A8Uint;
    case 2:
        return vk::Format::eR8G8B8A8Sint;
    default:
        return vk::Format::eR8G8B8A8Unorm;
    }
}

} // namespace

NullResources::NullResources(const Instance& instance) : device{instance.GetDevice()} {
    if (instance.IsNullDescriptorSupported()) {
        return;
    }
    active = true;
    buffer_size = BufferBytes;
    buffer = std::make_unique<VideoCore::Buffer>(instance, 0, buffer_size,
                                                 VideoCore::MemoryType::DeviceLocal,
                                                 "Null descriptor placeholder");

    const bool cube_array = instance.IsCubeArraySupported();
    for (u32 kind = 0; kind < NumKinds; ++kind) {
        const vk::Format format = KindFormat(kind);
        for (u32 shape = 0; shape < NumShapes; ++shape) {
            auto& image = images[kind][shape];
            image = std::make_unique<VideoCore::UniqueImage>(device, instance.GetAllocator());
            image->Create(vk::ImageCreateInfo{
                .flags = shape == Shape2D ? vk::ImageCreateFlagBits::eCubeCompatible
                                          : vk::ImageCreateFlags{},
                .imageType = shape == Shape1D   ? vk::ImageType::e1D
                             : shape == Shape2D ? vk::ImageType::e2D
                                                : vk::ImageType::e3D,
                .format = format,
                .extent = {1, 1, 1},
                .mipLevels = 1,
                .arrayLayers = shape == Shape2D ? Layers2D : 1u,
                .samples = vk::SampleCountFlagBits::e1,
                .tiling = vk::ImageTiling::eOptimal,
                .usage = vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eStorage |
                         vk::ImageUsageFlagBits::eTransferDst,
                .initialLayout = vk::ImageLayout::eUndefined,
            });
        }
        const auto make_view = [&](Shape shape, vk::ImageViewType type, u32 layers) {
            return Check(device.createImageView({
                .image = images[kind][shape]->image,
                .viewType = type,
                .format = format,
                .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, layers},
            }));
        };
        views[kind][View1D] = make_view(Shape1D, vk::ImageViewType::e1D, 1);
        views[kind][View1DArray] = make_view(Shape1D, vk::ImageViewType::e1DArray, 1);
        views[kind][View2D] = make_view(Shape2D, vk::ImageViewType::e2D, 1);
        views[kind][View2DArray] = make_view(Shape2D, vk::ImageViewType::e2DArray, Layers2D);
        views[kind][ViewCubeArray] =
            cube_array ? make_view(Shape2D, vk::ImageViewType::eCubeArray, Layers2D)
                       : make_view(Shape2D, vk::ImageViewType::eCube, Layers2D);
        views[kind][View3D] = make_view(Shape3D, vk::ImageViewType::e3D, 1);
    }

    // Zero everything and leave the images in the General layout every placeholder binds with.
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
    cmdbuf.fillBuffer(buffer->Handle(), 0, buffer_size, 0);
    std::vector<vk::ImageMemoryBarrier2> to_clear, to_general;
    for (const auto& kind : images) {
        for (const auto& image : kind) {
            const vk::ImageSubresourceRange all{vk::ImageAspectFlagBits::eColor, 0, 1, 0,
                                                image->image_ci.arrayLayers};
            to_clear.push_back({
                .srcStageMask = vk::PipelineStageFlagBits2::eNone,
                .srcAccessMask = vk::AccessFlagBits2::eNone,
                .dstStageMask = vk::PipelineStageFlagBits2::eClear,
                .dstAccessMask = vk::AccessFlagBits2::eTransferWrite,
                .oldLayout = vk::ImageLayout::eUndefined,
                .newLayout = vk::ImageLayout::eGeneral,
                .image = image->image,
                .subresourceRange = all,
            });
            to_general.push_back({
                .srcStageMask = vk::PipelineStageFlagBits2::eClear,
                .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
                .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands,
                .dstAccessMask = vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite,
                .oldLayout = vk::ImageLayout::eGeneral,
                .newLayout = vk::ImageLayout::eGeneral,
                .image = image->image,
                .subresourceRange = all,
            });
        }
    }
    cmdbuf.pipelineBarrier2({.imageMemoryBarrierCount = static_cast<u32>(to_clear.size()),
                             .pImageMemoryBarriers = to_clear.data()});
    for (const auto& barrier : to_clear) {
        cmdbuf.clearColorImage(barrier.image, vk::ImageLayout::eGeneral, vk::ClearColorValue{},
                               barrier.subresourceRange);
    }
    const vk::MemoryBarrier2 buffer_ready{
        .srcStageMask = vk::PipelineStageFlagBits2::eClear,
        .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .dstAccessMask = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite,
    };
    cmdbuf.pipelineBarrier2({.memoryBarrierCount = 1,
                             .pMemoryBarriers = &buffer_ready,
                             .imageMemoryBarrierCount = static_cast<u32>(to_general.size()),
                             .pImageMemoryBarriers = to_general.data()});
    Check(cmdbuf.end());

    const auto fence = Check(device.createFenceUnique({}));
    {
        std::scoped_lock lk{Scheduler::submit_mutex};
        const vk::SubmitInfo submit{.commandBufferCount = 1, .pCommandBuffers = &cmdbuf};
        ASSERT_MSG(instance.GetGraphicsQueue().submit(submit, *fence) == vk::Result::eSuccess,
                   "Null descriptor placeholders: submit failed");
    }
    ASSERT(device.waitForFences(*fence, true, UINT64_MAX) == vk::Result::eSuccess);
    LOG_INFO(Render_Vulkan,
             "No nullDescriptor: binding a {} KiB zero buffer and 1x1 zero images for empty "
             "bindings{}",
             buffer_size >> 10, cube_array ? "" : " (no imageCubeArray: cube views)");
}

NullResources::~NullResources() {
    for (const auto& kind : views) {
        for (const vk::ImageView view : kind) {
            if (view) {
                device.destroyImageView(view);
            }
        }
    }
}

vk::DescriptorBufferInfo NullResources::BufferInfo() const {
    if (!active) {
        return {VK_NULL_HANDLE, 0, VK_WHOLE_SIZE};
    }
    return {buffer->Handle(), 0, buffer_size};
}

vk::Buffer NullResources::VertexBuffer() const {
    return active ? buffer->Handle() : vk::Buffer{};
}

vk::DeviceSize NullResources::VertexSize(vk::DeviceSize requested) const {
    return active ? std::min(requested, buffer_size) : requested;
}

vk::DescriptorImageInfo NullResources::ImageInfo(const Shader::ImageResource& desc,
                                                 const AmdGpu::Image& sharp) const {
    if (!active) {
        return {VK_NULL_HANDLE, VK_NULL_HANDLE, vk::ImageLayout::eGeneral};
    }
    const auto nfmt = sharp.GetNumberFmt();
    const u32 kind = nfmt == AmdGpu::NumberFormat::Uint   ? Uint
                     : nfmt == AmdGpu::NumberFormat::Sint ? Sint
                                                          : Float;
    View view = View2D;
    if (desc.native_cube) {
        view = ViewCubeArray;
    } else {
        switch (sharp.GetViewType(desc.is_array)) {
        case AmdGpu::ImageType::Color1D:
            view = View1D;
            break;
        case AmdGpu::ImageType::Color1DArray:
            view = View1DArray;
            break;
        case AmdGpu::ImageType::Color2DArray:
        case AmdGpu::ImageType::Color2DMsaaArray:
            view = View2DArray;
            break;
        case AmdGpu::ImageType::Color3D:
            view = View3D;
            break;
        default:
            // Color2D; a multisampled shader binding gets a single-sampled view.
            view = View2D;
            break;
        }
    }
    return {VK_NULL_HANDLE, views[kind][view], vk::ImageLayout::eGeneral};
}

} // namespace Vulkan
