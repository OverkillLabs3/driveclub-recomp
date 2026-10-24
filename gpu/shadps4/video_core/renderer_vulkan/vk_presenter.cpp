// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
#endif
#include "bbport_frame_info.h"
#include "bbport_frametrace.h"
#include "common/debug.h"
#include "common/elf_info.h"
#include "common/io_file.h"
#include "common/path_util.h"
#include "common/singleton.h"
#include "core/debug_state.h"
#include "core/emulator_settings.h"
#include "sdl_window.h"
#include "video_core/buffer_cache/buffer.h"
#include "video_core/renderdoc.h"
#include "video_core/renderer_vulkan/vk_platform.h"
#include "video_core/renderer_vulkan/vk_frame_generation.h"
#include "bbport_overlay.h"
#include "bbport_neural_settings.h"
#include "bbport_vr.h"
#include "bbport_settings.h"
#include "video_core/renderer_vulkan/vk_temporal_upscaler.h"
#include "video_core/renderer_vulkan/vk_presenter.h"
#include "video_core/renderer_vulkan/vk_rasterizer.h"
#include "video_core/texture_cache/image.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <csetjmp>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <memory>
#include <span>
#include <sstream>
#include <system_error>
#include <thread>
#include <vector>
#include <vk_mem_alloc.h>
#ifdef MemoryBarrier
#undef MemoryBarrier // bbport: winnt.h macro (through fmt), clashes with vk::MemoryBarrier
#endif

extern "C" void runtime_file_miss_stats(uint64_t* slow, uint64_t* fast, uint64_t* slow_us);

namespace Vulkan {

namespace {
// When the last image (generated or the game's) went to the display.
std::chrono::steady_clock::time_point g_last_shown{};
} // namespace


bool CanBlitToSwapchain(const vk::PhysicalDevice physical_device, vk::Format format) {
    const vk::FormatProperties props{physical_device.getFormatProperties(format)};
    return static_cast<bool>(props.optimalTilingFeatures & vk::FormatFeatureFlagBits::eBlitDst);
}

[[nodiscard]] vk::ImageSubresourceLayers MakeImageSubresourceLayers() {
    return vk::ImageSubresourceLayers{
        .aspectMask = vk::ImageAspectFlagBits::eColor,
        .mipLevel = 0,
        .baseArrayLayer = 0,
        .layerCount = 1,
    };
}

[[nodiscard]] vk::ImageBlit MakeImageBlit(s32 frame_width, s32 frame_height, s32 dst_width,
                                          s32 dst_height, s32 offset_x, s32 offset_y) {
    return vk::ImageBlit{
        .srcSubresource = MakeImageSubresourceLayers(),
        .srcOffsets =
            std::array{
                vk::Offset3D{
                    .x = 0,
                    .y = 0,
                    .z = 0,
                },
                vk::Offset3D{
                    .x = frame_width,
                    .y = frame_height,
                    .z = 1,
                },
            },
        .dstSubresource = MakeImageSubresourceLayers(),
        .dstOffsets =
            std::array{
                vk::Offset3D{
                    .x = offset_x,
                    .y = offset_y,
                    .z = 0,
                },
                vk::Offset3D{
                    .x = offset_x + dst_width,
                    .y = offset_y + dst_height,
                    .z = 1,
                },
            },
    };
}

[[nodiscard]] vk::ImageBlit MakeImageBlitStretch(s32 frame_width, s32 frame_height,
                                                 s32 swapchain_width, s32 swapchain_height) {
    return MakeImageBlit(frame_width, frame_height, swapchain_width, swapchain_height, 0, 0);
}

static vk::Rect2D FitImage(s32 frame_width, s32 frame_height, s32 swapchain_width,
                           s32 swapchain_height) {
    float frame_aspect = static_cast<float>(frame_width) / frame_height;
    float swapchain_aspect = static_cast<float>(swapchain_width) / swapchain_height;

    u32 dst_width = swapchain_width;
    u32 dst_height = swapchain_height;

    if (frame_aspect > swapchain_aspect) {
        dst_height = static_cast<s32>(swapchain_width / frame_aspect);
    } else {
        dst_width = static_cast<s32>(swapchain_height * frame_aspect);
    }

    const s32 offset_x = (swapchain_width - dst_width) / 2;
    const s32 offset_y = (swapchain_height - dst_height) / 2;

    return vk::Rect2D{{offset_x, offset_y}, {dst_width, dst_height}};
}

[[nodiscard]] vk::ImageBlit MakeImageBlitFit(s32 frame_width, s32 frame_height, s32 swapchain_width,
                                             s32 swapchain_height) {
    const auto& dst_rect = FitImage(frame_width, frame_height, swapchain_width, swapchain_height);

    return MakeImageBlit(frame_width, frame_height, dst_rect.extent.width, dst_rect.extent.height,
                         dst_rect.offset.x, dst_rect.offset.y);
}

// bbport: screenshot capture and ImGui overlays removed; the port presents directly.

Presenter::Presenter(Frontend::WindowSDL& window_, AmdGpu::Liverpool* liverpool_)
    : window{window_}, liverpool{liverpool_},
      instance{window, EmulatorSettings.GetGpuId(), EmulatorSettings.IsVkValidationEnabled(),
               EmulatorSettings.IsVkCrashDiagnosticEnabled()},
      draw_scheduler{instance, true}, present_scheduler{instance}, flip_scheduler{instance},
      swapchain{instance, window}, runtime{instance, draw_scheduler},
      rasterizer{std::make_unique<Rasterizer>(instance, draw_scheduler, runtime, liverpool)},
      texture_cache{rasterizer->GetTextureCache()} {
    const u32 num_images = swapchain.GetImageCount();
    const vk::Device device = instance.GetDevice();

    // Create presentation frames.
    present_frames.resize(num_images);
    for (u32 i = 0; i < num_images; i++) {
        Frame& frame = present_frames[i];
        frame.id = i;
        auto fence = Check<"create present done fence">(
            device.createFence({.flags = vk::FenceCreateFlagBits::eSignaled}));
        frame.present_done = fence;
        free_queue.push(&frame);
    }

    fsr_settings.enable = EmulatorSettings.IsFsrEnabled();
    fsr_settings.use_rcas = EmulatorSettings.IsRcasEnabled();
    fsr_settings.rcas_attenuation =
        static_cast<float>(EmulatorSettings.GetRcasAttenuation() / 1000.f);

    fsr_pass.Create(device, instance.GetAllocator(), num_images);
    // bbport: frames are composed in RGBA8 (what frame generation reads and writes) and
    // blitted into the swapchain's format.
    pp_pass.Create(device, FrameGenerator::ColorFormat);
    // bbport: HDR output (BB_HDR=1, display in HDR mode): the swapchain in HDR10 from the start,
    // each frame converted by hdr_pass, and the overlay drawn into the SDR frame before that.
    if (EmulatorSettings.IsHdrAllowed()) {
        if (swapchain.HasHDR()) {
            swapchain.SetHDR(true);
            hdr_output = true;
            hdr_pass.Create(device, instance.GetAllocator(), vk::Format::eA2B10G10R10UnormPack32);
            BbSettings::Get().hdr_output = true;
            std::printf("HDR: HDR10 output, paper white %.0f nits, peak %.0f nits\n",
                        BbSettings::Get().hdr_paper.load(), BbSettings::Get().hdr_peak.load());
        } else {
            std::printf("HDR: the display offers no HDR10 output (Windows HDR off?): SDR\n");
        }
    }
    // bbport: the overlay draws over every presented image, generated ones included: its
    // vertex buffers must not be reused while one of them is still drawn.
    BbOverlay::Init(instance, hdr_output ? FrameGenerator::ColorFormat : swapchain.GetSurfaceFormat().format,
                    num_images * (FrameGenerator::MaxGenerated + 1));

    frame_generator = std::make_unique<FrameGenerator>(instance, draw_scheduler);
    neural = std::make_unique<NeuralRendering>(instance, draw_scheduler);
    swapchain_frame_gen = frame_generator->Wanted();
    rasterizer->GetUpscaler().SetFrameGenerator(frame_generator.get());
}

Presenter::~Presenter() {
    rasterizer->GetUpscaler().SetFrameGenerator(nullptr);

    // Frame generation reads the frames and their generated images: drain it before them.
    frame_generator.reset();
    draw_scheduler.Finish();
    present_scheduler.Finish();
    flip_scheduler.Finish();
    Check(draw_scheduler.CommandBuffer().reset());
    Check(present_scheduler.CommandBuffer().reset());
    Check(flip_scheduler.CommandBuffer().reset());

    const vk::Device device = instance.GetDevice();
    for (auto& frame : present_frames) {
        DestroyGenerated(&frame);
        vmaDestroyImage(instance.GetAllocator(), frame.image, frame.allocation);
        device.destroyImageView(frame.image_view);
        device.destroyFence(frame.present_done);
    }
    if (hudless_frame.image) {
        vmaDestroyImage(instance.GetAllocator(), hudless_frame.image, hudless_frame.allocation);
        device.destroyImageView(hudless_frame.image_view);
    }
}

void Presenter::DestroyGenerated(Frame* frame, u32 from) {
    const vk::Device device = instance.GetDevice();
    for (u32 i = from; i < frame->generated.size(); ++i) {
        auto& generated = frame->generated[i];
        if (generated.view) {
            device.destroyImageView(generated.view);
        }
        if (generated.image) {
            vmaDestroyImage(instance.GetAllocator(), generated.image, generated.allocation);
        }
        generated = {};
    }
    frame->generated_count = std::min(frame->generated_count, from);
}

u32 Presenter::EnsureGenerated(Frame* frame, u32 count) {
    // Only as many as the setting asks for (FSR 3.1 and DLSS 2x: one): at 4K each is 33 MB,
    // and every presentation frame has its own. The rest are freed (the frame's fence was
    // waited for in GetRenderFrame, so the GPU is done with them).
    count = std::min<u32>(count, u32(frame->generated.size()));
    DestroyGenerated(frame, count);
    const vk::Device device = instance.GetDevice();
    for (u32 i = 0; i < count; ++i) {
        auto& generated = frame->generated[i];
        if (generated.image) {
            continue;
        }
        const VkImageCreateInfo image_info = static_cast<VkImageCreateInfo>(vk::ImageCreateInfo{
            .imageType = vk::ImageType::e2D,
            .format = FrameGenerator::ColorFormat,
            .extent = {frame->width, frame->height, 1},
            .mipLevels = 1,
            .arrayLayers = 1,
            .samples = vk::SampleCountFlagBits::e1,
            .usage = vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled |
                     vk::ImageUsageFlagBits::eTransferSrc | vk::ImageUsageFlagBits::eTransferDst,
        });
        const VmaAllocationCreateInfo alloc_info = {
            .usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
        };
        VkImage image{};
        if (vmaCreateImage(instance.GetAllocator(), &image_info, &alloc_info, &image,
                           &generated.allocation, nullptr) != VK_SUCCESS) {
            LOG_ERROR(Render_Vulkan, "Frame generation: no memory for the generated frames");
            DestroyGenerated(frame, i);
            return i;
        }
        generated.image = vk::Image{image};
        SetObjectName(device, generated.image, "Generated frame #{}.{}", frame->id, i);
        generated.view = Check<"create generated frame view">(device.createImageView({
            .image = generated.image,
            .viewType = vk::ImageViewType::e2D,
            .format = FrameGenerator::ColorFormat,
            .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
        }));
    }
    return count;
}

void Presenter::ReleaseFrameGen(Frame* frame) {
    DestroyGenerated(frame);
    if (frame_generator->Wanted()) {
        return; // HDR output only: the rest is kept for when it is off again
    }
    // Switched off: the other frames free theirs when they are next used.
    if (hudless_frame.image) {
        // The previous frames' commands may still read it.
        draw_scheduler.DeferOperation([allocator = instance.GetAllocator(),
                                       device = instance.GetDevice(),
                                       image = hudless_frame.image, view = hudless_frame.image_view,
                                       allocation = hudless_frame.allocation] {
            device.destroyImageView(view);
            vmaDestroyImage(allocator, image, allocation);
        });
        hudless_frame.image = vk::Image{};
        hudless_frame.image_view = vk::ImageView{};
        hudless_frame.allocation = {};
        hudless_frame.width = hudless_frame.height = 0;
    }
    frame_generator->Release();
}

bool Presenter::IsVideoOutSurface(const AmdGpu::ColorBuffer& color_buffer) const {
    return std::ranges::find(vo_buffers_addr, color_buffer.Address()) != vo_buffers_addr.cend();
}

void Presenter::RecreateFrame(Frame* frame, u32 width, u32 height) {
    const vk::Device device = instance.GetDevice();
    if (frame->image_view) {
        device.destroyImageView(frame->image_view);
    }
    if (frame->image) {
        vmaDestroyImage(instance.GetAllocator(), frame->image, frame->allocation);
    }
    DestroyGenerated(frame); // made again at the new size when needed

    const vk::Format format = FrameGenerator::ColorFormat;
    const vk::ImageCreateInfo image_info = {
        .flags = vk::ImageCreateFlagBits::eMutableFormat,
        .imageType = vk::ImageType::e2D,
        .format = format,
        .extent = {width, height, 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = vk::SampleCountFlagBits::e1,
        .usage = vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eTransferDst |
                 vk::ImageUsageFlagBits::eTransferSrc | vk::ImageUsageFlagBits::eSampled,
    };

    const VmaAllocationCreateInfo alloc_info = {
        .flags = VMA_ALLOCATION_CREATE_WITHIN_BUDGET_BIT,
        .usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
        .requiredFlags = 0,
        .preferredFlags = 0,
        .pool = VK_NULL_HANDLE,
        .pUserData = nullptr,
    };

    VkImage unsafe_image{};
    VkImageCreateInfo unsafe_image_info = static_cast<VkImageCreateInfo>(image_info);

    VkResult result = vmaCreateImage(instance.GetAllocator(), &unsafe_image_info, &alloc_info,
                                     &unsafe_image, &frame->allocation, nullptr);
    if (result == VK_ERROR_OUT_OF_DEVICE_MEMORY) {
        // bbport: VRAM full (the texture cache fills the budget on 4-8 GB cards): past the
        // budget, in system memory if need be, as buffers and images do (buffer.cpp).
        LOG_WARNING(Render_Vulkan, "Presentation frame: out of video memory, using system memory");
        VmaAllocationCreateInfo over_budget = alloc_info;
        over_budget.flags &= ~VMA_ALLOCATION_CREATE_WITHIN_BUDGET_BIT;
        result = vmaCreateImage(instance.GetAllocator(), &unsafe_image_info, &over_budget,
                                &unsafe_image, &frame->allocation, nullptr);
    }
    if (result != VK_SUCCESS) [[unlikely]] {
        LOG_CRITICAL(Render_Vulkan, "Failed allocating texture with error {}",
                     vk::to_string(vk::Result{result}));
        UNREACHABLE();
    }
    frame->image = vk::Image{unsafe_image};
    SetObjectName(device, frame->image, "Frame image #{}", frame->id);

    const vk::ImageViewCreateInfo view_info = {
        .image = frame->image,
        .viewType = vk::ImageViewType::e2D,
        .format = format,
        .subresourceRange{
            .aspectMask = vk::ImageAspectFlagBits::eColor,
            .baseMipLevel = 0,
            .levelCount = 1,
            .baseArrayLayer = 0,
            .layerCount = 1,
        },
    };
    auto view = Check<"create frame image view">(device.createImageView(view_info));
    frame->image_view = view;
    frame->width = width;
    frame->height = height;

    frame->is_hdr = GameHdr();

    // bbport: HDR output: the HDR10 picture presented in place of the frame (same size).
    if (frame->hdr_view) {
        device.destroyImageView(frame->hdr_view);
        frame->hdr_view = vk::ImageView{};
    }
    if (frame->hdr_image) {
        vmaDestroyImage(instance.GetAllocator(), frame->hdr_image, frame->hdr_allocation);
        frame->hdr_image = vk::Image{};
    }
    frame->hdr_ready = false;
    if (hdr_output) {
        VkImageCreateInfo hdr_info = static_cast<VkImageCreateInfo>(vk::ImageCreateInfo{
            .imageType = vk::ImageType::e2D,
            .format = vk::Format::eA2B10G10R10UnormPack32,
            .extent = {width, height, 1},
            .mipLevels = 1,
            .arrayLayers = 1,
            .samples = vk::SampleCountFlagBits::e1,
            .usage = vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eTransferSrc,
        });
        VkImage hdr_image{};
        VmaAllocationCreateInfo over_budget = alloc_info;
        over_budget.flags &= ~VMA_ALLOCATION_CREATE_WITHIN_BUDGET_BIT;
        if (vmaCreateImage(instance.GetAllocator(), &hdr_info, &over_budget, &hdr_image,
                           &frame->hdr_allocation, nullptr) == VK_SUCCESS) {
            frame->hdr_image = vk::Image{hdr_image};
            frame->hdr_view = Check<"create hdr frame view">(device.createImageView({
                .image = frame->hdr_image,
                .viewType = vk::ImageViewType::e2D,
                .format = vk::Format::eA2B10G10R10UnormPack32,
                .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
            }));
        }
    }
}

Frame* Presenter::PrepareLastFrame() {
    if (last_submit_frame == nullptr) {
        return nullptr;
    }

    Frame* frame = last_submit_frame;

    while (true) {
        vk::Result result = instance.GetDevice().waitForFences(frame->present_done, false,
                                                               std::numeric_limits<u64>::max());
        if (result == vk::Result::eSuccess) {
            break;
        }
        if (result == vk::Result::eTimeout) {
            continue;
        }
        if (result == vk::Result::eErrorDeviceLost) {
            instance.LogDeviceLost();
        }
        ASSERT_MSG(result != vk::Result::eErrorDeviceLost,
                   "Device lost during waiting for a frame");
    }

    auto& scheduler = flip_scheduler;
    scheduler.EndRendering();
    const auto cmdbuf = scheduler.CommandBuffer();

    const auto frame_subresources = vk::ImageSubresourceRange{
        .aspectMask = vk::ImageAspectFlagBits::eColor,
        .baseMipLevel = 0,
        .levelCount = 1,
        .baseArrayLayer = 0,
        .layerCount = VK_REMAINING_ARRAY_LAYERS,
    };

    const auto pre_barrier =
        vk::ImageMemoryBarrier2{.srcStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
                                .srcAccessMask = vk::AccessFlagBits2::eColorAttachmentRead,
                                .dstStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
                                .dstAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite,
                                .oldLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
                                .newLayout = vk::ImageLayout::eGeneral,
                                .image = frame->image,
                                .subresourceRange{frame_subresources}};

    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .imageMemoryBarrierCount = 1,
        .pImageMemoryBarriers = &pre_barrier,
    });

    // Flush frame creation commands.
    frame->ready_semaphore = scheduler.GetWorkSemaphore()->Handle();
    frame->ready_tick = scheduler.CurrentTick();
    SubmitInfo info{};
    scheduler.Flush(info);
    return frame;
}

static vk::Format GetFrameViewFormat(const Libraries::VideoOut::PixelFormat format) {
    switch (format) {
    case Libraries::VideoOut::PixelFormat::A8B8G8R8Srgb:
        return vk::Format::eR8G8B8A8Srgb;
    case Libraries::VideoOut::PixelFormat::A8R8G8B8Srgb:
        return vk::Format::eB8G8R8A8Srgb;
    case Libraries::VideoOut::PixelFormat::A2R10G10B10:
    case Libraries::VideoOut::PixelFormat::A2R10G10B10Srgb:
    case Libraries::VideoOut::PixelFormat::A2R10G10B10Bt2020Pq:
        return vk::Format::eA2R10G10B10UnormPack32;
    default:
        break;
    }
    UNREACHABLE_MSG("Unknown format={}", static_cast<u32>(format));
    return {};
}

// bbport: raw copy of an image for BB_FRAME_DUMP_TRIGGER, written once the GPU is done.
static void DumpRaw(const Instance& instance, Scheduler& scheduler, vk::CommandBuffer cmdbuf,
                    vk::Image image, vk::ImageLayout layout, u32 w, u32 h, const char* name,
                    int index) {
    const VkBufferCreateInfo buffer_ci{.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                                       .size = VkDeviceSize(w) * h * 4,
                                       .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    const VmaAllocationCreateInfo alloc_ci{
        .flags = VMA_ALLOCATION_CREATE_MAPPED_BIT | VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT,
        .usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST};
    VkBuffer buffer{};
    VmaAllocation allocation{};
    VmaAllocationInfo info{};
    if (vmaCreateBuffer(instance.GetAllocator(), &buffer_ci, &alloc_ci, &buffer, &allocation,
                        &info) != VK_SUCCESS) {
        return;
    }
    cmdbuf.copyImageToBuffer(image, layout, buffer,
                             vk::BufferImageCopy{.imageSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
                                                 .imageExtent = {w, h, 1}});
    const char* dir = std::getenv("BB_DUMP_DIR");
    char path[512];
    std::snprintf(path, sizeof(path), "%s/%s_%03d_%ux%u.raw", dir && *dir ? dir : "out/dump", name,
                  index, w, h);
    scheduler.DeferPriorityOperation([allocator = instance.GetAllocator(), buffer, allocation, info,
                                      size = buffer_ci.size, file = std::string{path}] {
        vmaInvalidateAllocation(allocator, allocation, 0, VK_WHOLE_SIZE);
        if (FILE* f = std::fopen(file.c_str(), "wb")) {
            std::fwrite(info.pMappedData, 1, size, f);
            std::fclose(f);
        }
        vmaDestroyBuffer(allocator, buffer, allocation);
    });
}

Frame* Presenter::PrepareFrame(const Libraries::VideoOut::BufferAttributeGroup& attribute,
                               VAddr cpu_address) {
    const s64 trace_start = BbTrace::enabled ? BbTrace::NowNs() : 0;
#ifdef _WIN32
    {
        // Neural rendering (off unless nr_enabled is set): F7 switches it off and on, F6 steps through the styles.
        static bool f7_down = false, f6_down = false;
        static const bool requested = BbNeural::Snapshot().enabled;
        if (requested) {
            const bool f7 = (GetAsyncKeyState(VK_F7) & 0x8000) != 0;
            const bool f6 = (GetAsyncKeyState(VK_F6) & 0x8000) != 0;
            if (f7 && !f7_down) {
                BbNeural::Toggle();
            }
            if (f6 && !f6_down) {
                auto config = BbNeural::Snapshot();
                config.passes[0].style = (config.passes[0].style + 1) % BbNeural::StyleCount;
                BbNeural::Set(config);
            }
            f7_down = f7;
            f6_down = f6;
        }
    }
#endif
    // Before any command buffer of this frame is taken: a settings change finishes the scheduler.
    neural->BeginFrame();
    // bbport: BB_FRAME_DUMP_TRIGGER=<file>: when the file exists it is removed and this frame's
    // guest display buffer and presented image are written to BB_DUMP_DIR (default out/dump)
    // as raw 32-bit pixels (menus and movies included; the upscaler dumps scene frames only).
    static const char* frame_dump_trigger = std::getenv("BB_FRAME_DUMP_TRIGGER");
    static int frame_dump_index = 0;
    const bool frame_dump = frame_dump_trigger && std::remove(frame_dump_trigger) == 0;
    // bbport: scaled upscaler presets: the output-size display buffer drawn by the port.
    TemporalUpscaler::Display display{};
    const bool upscaled = rasterizer->GetUpscaler().DisplayOverride(cpu_address, display);
    VideoCore::ImageId image_id{};
    if (!upscaled) {
        auto desc = VideoCore::TextureCache::ImageDesc{attribute, cpu_address};
        image_id = texture_cache.FindImage(desc);
        texture_cache.UpdateImage(image_id);
    }

    Frame* frame = GetRenderFrame();

    const auto frame_subresources = vk::ImageSubresourceRange{
        .aspectMask = vk::ImageAspectFlagBits::eColor,
        .baseMipLevel = 0,
        .levelCount = 1,
        .baseArrayLayer = 0,
        .layerCount = VK_REMAINING_ARRAY_LAYERS,
    };

    const auto pre_barrier = vk::ImageMemoryBarrier2{
        .srcStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
        .srcAccessMask = vk::AccessFlagBits2::eColorAttachmentRead,
        .dstStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
        .dstAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite,
        .oldLayout = vk::ImageLayout::eUndefined,
        .newLayout = vk::ImageLayout::eColorAttachmentOptimal,
        .image = frame->image,
        .subresourceRange{frame_subresources},
    };

    draw_scheduler.EndRendering();
    const auto cmdbuf = draw_scheduler.CommandBuffer();
    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .imageMemoryBarrierCount = 1,
        .pImageMemoryBarriers = &pre_barrier,
    });

    VideoCore::ImageViewInfo view_info{};
    view_info.format = GetFrameViewFormat(attribute.attrib.pixel_format);
    // Exclude alpha from output frame to avoid blending with UI.
    view_info.mapping.a = vk::ComponentSwizzle::eOne;

    vk::ImageView image_view{};
    vk::ImageView alpha_view{}; // bbport: VR: the display buffer with its alpha (the HUD's coverage)
    vk::Extent2D image_size{};
    if (upscaled) {
        const auto device = instance.GetDevice();
        image_view = Check(device.createImageView({
            .image = display.image,
            .viewType = vk::ImageViewType::e2D,
            .format = view_info.format,
            .components = {vk::ComponentSwizzle::eIdentity, vk::ComponentSwizzle::eIdentity,
                           vk::ComponentSwizzle::eIdentity, vk::ComponentSwizzle::eOne},
            .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
        }));
        draw_scheduler.DeferOperation([device, image_view] { device.destroyImageView(image_view); });
        image_size = vk::Extent2D{display.width, display.height};
        const vk::ImageMemoryBarrier2 to_read{
            .srcStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
            .srcAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands,
            .dstAccessMask = vk::AccessFlagBits2::eShaderRead,
            .oldLayout = vk::ImageLayout::eGeneral,
            .newLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
            .image = display.image,
            .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
        };
        cmdbuf.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &to_read});
    } else {
        auto& image = texture_cache.GetImage(image_id);
        if (frame_dump) {
            runtime.Transit(&image, vk::ImageLayout::eTransferSrcOptimal,
                            vk::PipelineStageFlagBits2::eTransfer, vk::AccessFlagBits2::eTransferRead);
            runtime.FlushBarriers();
            DumpRaw(instance, draw_scheduler, cmdbuf, image.GetImage(),
                    vk::ImageLayout::eTransferSrcOptimal, image.info.size.width,
                    image.info.size.height, "display", frame_dump_index);
        }
        image_view = *image.FindView(view_info).image_view;
        if (BbVr::Active()) {
            VideoCore::ImageViewInfo alpha_info = view_info;
            alpha_info.mapping.a = vk::ComponentSwizzle::eIdentity;
            alpha_view = *image.FindView(alpha_info).image_view;
        }
        image_size = vk::Extent2D{image.info.size.width, image.info.size.height};
        runtime.Transit(&image, vk::ImageLayout::eShaderReadOnlyOptimal,
                        vk::PipelineStageFlagBits2::eFragmentShader,
                        vk::AccessFlagBits2::eShaderRead);
        runtime.FlushBarriers();
    }
    // bbport: VR: the headset takes the display buffer at its own size (the output resolution).
    // With the HUD drawn apart the window shows the scene and the HUD put together again.
    if (const VkImageView whole = BbVr::NoteFrame(cmdbuf, frame->image, frame->is_hdr ? vk::ImageView{} : image_view,
                                                  alpha_view, VkFormat(view_info.format), image_size.width,
                                                  image_size.height)) {
        image_view = vk::ImageView(whole);
    }
    BbFrame::NoteOutput(image_size.width, image_size.height);
    expected_ratio = static_cast<float>(image_size.width) / static_cast<float>(image_size.height);

    image_view = fsr_pass.Render(cmdbuf, image_view, image_size, {frame->width, frame->height},
                                 fsr_settings, frame->is_hdr);

    // Vulkan has no sRGB variant of the 10-bit format, so an A2R10G10B10Srgb buffer reaches
    // the post process pass still sRGB encoded and has to be decoded there instead.
    pp_settings.srgb_input =
        attribute.attrib.pixel_format == Libraries::VideoOut::PixelFormat::A2R10G10B10Srgb;
    // bbport: TV gamma: the game was made for TVs that decode with 2.4, PC displays decode sRGB
    // (about 2.2) and showed it washed out. The encoded values to the power 2.4 / 2.2 look on a PC
    // display as the picture did on a TV. SDR only (HDR maps the picture itself). BB_TV_GAMMA=0: off.
    static const bool tv_gamma = [] {
        const char* env = std::getenv("BB_TV_GAMMA");
        return !env || env[0] != '0';
    }();
    pp_settings.power = tv_gamma && !hdr_output ? 2.4f / 2.2f : 1.0f;
    pp_pass.Render(cmdbuf, image_view, image_size, *frame, pp_settings);

    // Neural rendering on the finished, display-referred frame (the UI is in it), before the HDR conversion.
    if (!frame->is_hdr) {
        neural->Record(cmdbuf, BbNeural::Present,
                       {frame->image, frame->image_view, FrameGenerator::ColorFormat, frame->width,
                        frame->height, false, false},
                       {}, false);
    }

    // bbport: HDR output: the overlay over the SDR frame, then the frame and the game's picture
    // before its UI into the HDR10 image presented in its place.
    frame->hdr_ready = false;
    if (hdr_output && frame->hdr_image) {
        const auto frame_barrier = [&](vk::Image image, vk::ImageLayout from, vk::ImageLayout to,
                                       vk::PipelineStageFlags2 src, vk::AccessFlags2 src_access,
                                       vk::PipelineStageFlags2 dst, vk::AccessFlags2 dst_access) {
            const vk::ImageMemoryBarrier2 b{
                .srcStageMask = src,
                .srcAccessMask = src_access,
                .dstStageMask = dst,
                .dstAccessMask = dst_access,
                .oldLayout = from,
                .newLayout = to,
                .image = image,
                .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
            };
            cmdbuf.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &b});
        };
        const auto attachment = vk::PipelineStageFlagBits2::eColorAttachmentOutput;
        const auto write = vk::AccessFlagBits2::eColorAttachmentWrite;
        const auto read_access = vk::AccessFlagBits2::eColorAttachmentRead | write;
        if (BbOverlay::Visible()) {
            frame_barrier(frame->image, vk::ImageLayout::eGeneral,
                          vk::ImageLayout::eColorAttachmentOptimal, attachment, write, attachment,
                          read_access);
            BbOverlay::Render(cmdbuf, frame->image_view, {frame->width, frame->height});
            frame_barrier(frame->image, vk::ImageLayout::eColorAttachmentOptimal,
                          vk::ImageLayout::eShaderReadOnlyOptimal, attachment, write,
                          vk::PipelineStageFlagBits2::eFragmentShader,
                          vk::AccessFlagBits2::eShaderRead);
        } else {
            frame_barrier(frame->image, vk::ImageLayout::eGeneral,
                          vk::ImageLayout::eShaderReadOnlyOptimal, attachment, write,
                          vk::PipelineStageFlagBits2::eFragmentShader,
                          vk::AccessFlagBits2::eShaderRead);
        }
        frame_barrier(frame->hdr_image, vk::ImageLayout::eUndefined,
                      vk::ImageLayout::eColorAttachmentOptimal, vk::PipelineStageFlagBits2::eAllCommands,
                      vk::AccessFlagBits2::eNone, attachment, write);
        const vk::ImageView hudless = rasterizer->HdrHudless(cpu_address);
        const vk::ImageView scene = rasterizer->HdrScene(cpu_address);
        const auto& s = BbSettings::Get();
        const HostPasses::HdrPass::Params params{
            .paper_nits = s.hdr_paper.load(),
            .peak_nits = s.hdr_peak.load(),
            .has_hudless = hudless ? 1.0f : 0.0f,
        };
        hdr_pass.Render(cmdbuf, frame->image_view, hudless, scene, frame->hdr_view,
                        {frame->width, frame->height}, params);
        frame_barrier(frame->hdr_image, vk::ImageLayout::eColorAttachmentOptimal,
                      vk::ImageLayout::eGeneral, attachment, write,
                      vk::PipelineStageFlagBits2::eAllCommands, vk::AccessFlagBits2::eMemoryRead);
        frame_barrier(frame->image, vk::ImageLayout::eShaderReadOnlyOptimal, vk::ImageLayout::eGeneral,
                      vk::PipelineStageFlagBits2::eFragmentShader, vk::AccessFlagBits2::eShaderRead,
                      vk::PipelineStageFlagBits2::eAllCommands,
                      vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite);
        frame->hdr_ready = true;
    }

    // bbport: frame generation: the frames between the previous frame and this one.
    frame->generated_count = 0;
    frame_generator->SetOutputSize(frame->width, frame->height);
    constexpr auto fg_color_usage =
        vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eTransferDst |
        vk::ImageUsageFlagBits::eTransferSrc | vk::ImageUsageFlagBits::eSampled;
    std::array<FrameGenerator::Output, FrameGenerator::MaxGenerated> fg_outputs{};
    // bbport: also for the upscaled display (outputs other than 1080p): composed the same way.
    if (frame_generator->Wanted() && frame_generator->Paused()) {
        frame_generator->Skip(); // paused with F8: everything stays set up
    } else if (!frame_generator->Wanted() || frame->is_hdr || hdr_output) {
        ReleaseFrameGen(frame);
        frame_generator->Skip(); // HDR: this frame's inputs are not used
    } else {
        // At most the frames the display can show between two of the game's (present thread):
        // more are dropped by Mailbox, and with FIFO they would slow the game to the display.
        const u32 count = std::min(EnsureGenerated(frame, frame_generator->WantedCount()),
                                   frame_gen_limit.load(std::memory_order_relaxed));
        // The picture before the UI, composed like the frame (equal outside the UI).
        vk::Image hudless{};
        vk::ImageView hudless_view{};
        vk::Extent2D hudless_size{};
        if (const auto source = frame_generator->Hudless(view_info.format, hudless_size, cpu_address);
            count && source && hudless_size == image_size) {
            if (hudless_frame.width != frame->width || hudless_frame.height != frame->height ||
                !hudless_frame.image) {
                // A single image: the previous frames' commands may still read the old one.
                draw_scheduler.WaitSubmitted();
                RecreateFrame(&hudless_frame, frame->width, frame->height);
            }
            const vk::ImageMemoryBarrier2 to_target{
                .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
                .srcAccessMask = vk::AccessFlagBits2::eShaderRead,
                .dstStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
                .dstAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite,
                .oldLayout = vk::ImageLayout::eUndefined,
                .newLayout = vk::ImageLayout::eColorAttachmentOptimal,
                .image = hudless_frame.image,
                .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
            };
            cmdbuf.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &to_target});
            if (auto* prof = FgProfiler::Get()) {
                prof->Mark(cmdbuf, FgProfileHudless, "fg hudless upscale+pp");
            }
            const auto scaled = fsr_pass.Render(cmdbuf, source, hudless_size,
                                                {frame->width, frame->height}, fsr_settings,
                                                frame->is_hdr);
            pp_pass.Render(cmdbuf, scaled, hudless_size, hudless_frame, pp_settings);
            if (auto* prof = FgProfiler::Get()) {
                prof->Mark(cmdbuf, FgProfileFrame, "rest of the game's frame (draw queue)");
            }
            hudless = hudless_frame.image;
            hudless_view = hudless_frame.image_view;
        }
        // Also with none: a frame without generation starts the sequence over.
        for (u32 i = 0; i < count; ++i) {
            fg_outputs[i] = {frame->generated[i].image, frame->generated[i].view};
        }
        frame->generated_count = frame_generator->Generate(
            cmdbuf, frame->image, frame->image_view, fg_color_usage, hudless, hudless_view,
            frame->width, frame->height,
            std::span<const FrameGenerator::Output>{fg_outputs.data(), count}, cpu_address);
    }
    if (frame_dump) {
        // The presented image as the swapchain blit reads it (General after the pass), and
        // the generated frames shown before it.
        const vk::MemoryBarrier2 done{.srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
                                      .srcAccessMask = vk::AccessFlagBits2::eMemoryWrite,
                                      .dstStageMask = vk::PipelineStageFlagBits2::eTransfer,
                                      .dstAccessMask = vk::AccessFlagBits2::eTransferRead};
        cmdbuf.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &done});
        for (u32 i = 0; i < frame->generated_count; ++i) {
            char name[16];
            std::snprintf(name, sizeof(name), "generated%u", i);
            DumpRaw(instance, draw_scheduler, cmdbuf, frame->generated[i].image,
                    vk::ImageLayout::eGeneral, frame->width, frame->height, name,
                    frame_dump_index);
        }
        DumpRaw(instance, draw_scheduler, cmdbuf, frame->image, vk::ImageLayout::eGeneral,
                frame->width, frame->height, "output", frame_dump_index);
    }

    // Flush frame creation commands.
    frame->ready_semaphore = draw_scheduler.GetWorkSemaphore()->Handle();
    frame->ready_tick = draw_scheduler.CurrentTick();
    SubmitInfo info{};
    draw_scheduler.Flush(info);

    if (frame_dump) {
        ++frame_dump_index;
    }

    // bbport: the GPU command thread runs at most BB_FRAMES_AHEAD (default 1) guest frames
    // ahead of the GPU: it waits here for the frame that many flips back. When the GPU is the
    // bottleneck it finishes frames at an even rate; without this bound the command thread ran
    // ahead and then blocked wherever a resource ran out, so flips (and the guest's frame
    // timing) came in bursts: 12.5/25 ms alternation at 80 FPS. 0 turns it off.
    static const u32 frames_ahead = [] {
        const char* env = std::getenv("BB_FRAMES_AHEAD");
        return env ? u32(std::max(0, std::atoi(env))) : 1u;
    }();
    s64 ahead_start = 0, ahead_end = 0;
    if (frames_ahead) {
        recent_frame_ticks.push_back(frame->ready_tick);
        while (recent_frame_ticks.size() > frames_ahead) {
            const u64 tick = recent_frame_ticks.front();
            recent_frame_ticks.pop_front();
            if (recent_frame_ticks.size() == frames_ahead) {
                ahead_start = BbTrace::enabled ? BbTrace::NowNs() : 0;
                draw_scheduler.Wait(tick, 3);
                ahead_end = BbTrace::enabled ? BbTrace::NowNs() : 0;
            }
        }
    }
    if (BbTrace::enabled) {
        BbTrace::prep_ns.store(u64(BbTrace::NowNs() - trace_start), std::memory_order_relaxed);
        BbTrace::ahead_wait_ns.store(u64(ahead_end - ahead_start), std::memory_order_relaxed);
    }
    return frame;
}

Frame* Presenter::PrepareBlankFrame(bool present_thread) {
    // Request a free presentation frame.
    Frame* frame = GetRenderFrame();
    frame->generated_count = 0;
    frame->hdr_ready = false; // black: the frame itself is presented

    auto& scheduler = present_thread ? present_scheduler : draw_scheduler;
    scheduler.EndRendering();

    const auto cmdbuf = scheduler.CommandBuffer();

    constexpr vk::ImageSubresourceRange simple_subresource = {
        .aspectMask = vk::ImageAspectFlagBits::eColor,
        .levelCount = 1,
        .layerCount = 1,
    };
    const auto pre_barrier = vk::ImageMemoryBarrier2{
        .srcStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
        .srcAccessMask = vk::AccessFlagBits2::eColorAttachmentRead,
        .dstStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
        .dstAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite,
        .oldLayout = vk::ImageLayout::eUndefined,
        .newLayout = vk::ImageLayout::eColorAttachmentOptimal,
        .image = frame->image,
        .subresourceRange = simple_subresource,
    };

    const auto post_barrier = vk::ImageMemoryBarrier2{
        .srcStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
        .srcAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eFragmentShader,
        .dstAccessMask = vk::AccessFlagBits2::eShaderRead,
        .oldLayout = vk::ImageLayout::eColorAttachmentOptimal,
        .newLayout = vk::ImageLayout::eGeneral,
        .image = frame->image,
        .subresourceRange = simple_subresource,
    };

    const vk::RenderingAttachmentInfo attachment = {
        .imageView = frame->image_view,
        .imageLayout = vk::ImageLayout::eColorAttachmentOptimal,
        .loadOp = vk::AttachmentLoadOp::eClear,
        .storeOp = vk::AttachmentStoreOp::eStore,
    };
    const vk::RenderingInfo rendering_info = {
        .renderArea =
            {
                .extent = {frame->width, frame->height},
            },
        .layerCount = 1,
        .colorAttachmentCount = 1u,
        .pColorAttachments = &attachment,
    };

    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .imageMemoryBarrierCount = 1,
        .pImageMemoryBarriers = &pre_barrier,
    });

    cmdbuf.beginRendering(rendering_info);
    cmdbuf.endRendering();

    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .imageMemoryBarrierCount = 1,
        .pImageMemoryBarriers = &post_barrier,
    });

    // Flush frame creation commands.
    frame->ready_semaphore = scheduler.GetWorkSemaphore()->Handle();
    frame->ready_tick = scheduler.CurrentTick();
    SubmitInfo info{};
    scheduler.Flush(info);
    return frame;
}

void Presenter::Present(Frame* frame, bool is_reusing_frame, bool is_game_frame) {
    // Free the frame for reuse
    const auto free_frame = [&] {
        if (!is_reusing_frame) {
            last_submit_frame = frame;
            std::scoped_lock fl{free_mutex};
            free_queue.push(frame);
            free_cv.notify_one();
        }
    };

    // bbport: a minimized window has no pixels on Windows (0x0); keep the swapchain and skip
    // presenting until the window is restored.
    if (window.GetWidth() == 0 || window.GetHeight() == 0) {
        free_frame();
        return;
    }

    // Recreate the swapchain if the window was resized.
    // bbport: also when frame generation was switched in the menu: it presents several images
    // per game frame and the swapchain gets one more for them (Swapchain::SetSurfaceProperties).
    const bool frame_gen = frame_generator->Wanted();
    if (window.GetWidth() != swapchain.GetWidth() || window.GetHeight() != swapchain.GetHeight() ||
        frame_gen != swapchain_frame_gen) {
        swapchain_frame_gen = frame_gen;
        swapchain.Recreate(window.GetWidth(), window.GetHeight());
    }

    // bbport: frame generation: the generated frames first, then the game's own, evenly
    // spaced over the interval between the game's frames (the generated ones belong to the
    // time before this frame). A frame shown again has none.
    u32 generated = is_reusing_frame ? 0 : frame->generated_count;
    auto start = std::chrono::steady_clock::now();
    if (!is_reusing_frame && is_game_frame) {
        const double ms =
            std::chrono::duration<double, std::milli>(start - last_game_present).count();
        if (ms > 1.0 && ms < 100.0) {
            game_frame_ms = game_frame_ms + (ms - game_frame_ms) * 0.1;
        }
        last_game_present = start;
    }
    const auto step = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
        std::chrono::duration<double, std::milli>(game_frame_ms / (generated + 1)));
    // The game must never wait for generated frames: when every frame is in use (the GPU
    // thread would block on the next one, and the game on its GPU work: Uncharted 3 asserts
    // in FrameBegin) they are dropped and the game's frame goes out at once.
    const auto behind = [this] {
        std::scoped_lock lock{free_mutex};
        return free_queue.empty();
    };
    if (generated && behind()) {
        generated = 0;
    }
    const auto wait_until = [](std::chrono::steady_clock::time_point at) {
        // The system sleep is coarse: sleep most of the way, then spin.
        constexpr auto spin = std::chrono::microseconds(1500);
        const auto now = std::chrono::steady_clock::now();
        if (at - now > spin) {
            std::this_thread::sleep_for(at - now - spin);
        }
        while (std::chrono::steady_clock::now() < at) {
            std::this_thread::yield();
        }
    };
    // A game frame that arrives early must not bunch its frames up behind the last one shown:
    // the first of them waits one step after it.
    if (generated && g_last_shown.time_since_epoch().count() != 0) {
        const auto earliest = g_last_shown + step;
        if (start < earliest && earliest - start < std::chrono::milliseconds(50)) {
            wait_until(earliest);
            start = earliest;
        }
    }
    u32 shown = 0;
    for (; shown < generated; ++shown) {
        if (shown > 0) {
            if (behind()) {
                break;
            }
            wait_until(start + step * shown);
        }
        PresentImage(frame, frame->generated[shown].image, false);
    }
    if (shown && !behind()) {
        wait_until(start + step * shown);
    }
    const auto presented_generated = std::chrono::steady_clock::now();
    if (!PresentImage(frame, frame->image, true)) {
        if (shown) {
            // The frame's fence was not reset and so does not cover the generated frames'
            // blits: they must be done before the frame is drawn again.
            present_scheduler.Finish();
        }
        free_frame();
        return;
    }
    if (!is_reusing_frame && is_game_frame) {
        UpdateFrameGenLimit(frame_gen, std::chrono::duration<double, std::milli>(
                                           std::chrono::steady_clock::now() - start)
                                           .count());
    }
    // The log shows whether frame generation really made frames: every 10 seconds.
    if (frame_gen && !is_reusing_frame && is_game_frame) {
        static auto window_start = std::chrono::steady_clock::now();
        static u32 game_frames = 0, with_generated = 0;
        ++game_frames;
        with_generated += frame->generated_count ? 1 : 0;
        const auto now_time = std::chrono::steady_clock::now();
        if (now_time - window_start >= std::chrono::seconds(10)) {
            std::printf("Frame generation: %u of %u game frames got a generated frame in the last 10 s\n",
                        with_generated, game_frames);
            window_start = now_time;
            game_frames = with_generated = 0;
        }
    }
    // bbport: BB_FPS_LOG=1: frame generation's pacing every 5 seconds.
    static const bool fg_trace = [] {
        const char* env = std::getenv("BB_FPS_LOG");
        return env && env[0] == '1';
    }();
    if (fg_trace && is_game_frame && !is_reusing_frame) {
        static auto window = start;
        static u32 frames = 0, made = 0, shown_total = 0;
        static double busy_ms = 0.0, present_ms = 0.0;
        ++frames;
        made += frame->generated_count;
        shown_total += shown;
        const auto end = std::chrono::steady_clock::now();
        busy_ms += std::chrono::duration<double, std::milli>(end - start).count();
        present_ms += std::chrono::duration<double, std::milli>(end - presented_generated).count();
        const double elapsed = std::chrono::duration<double>(end - window).count();
        if (elapsed >= 5.0) {
            std::printf("Frame generation pacing: %.1f game frames/s, %u generated, %u shown, "
                        "Present %.2f ms/frame (last image %.2f ms), interval %.2f ms\n",
                        frames / elapsed, made, shown_total, busy_ms / frames, present_ms / frames,
                        game_frame_ms.load());
            window = end;
            frames = made = shown_total = 0;
            busy_ms = present_ms = 0.0;
        }
    }

    // A line every 30 s for support: how the game was running when something went wrong.
    if (is_game_frame && !is_reusing_frame) {
        static auto status_window = std::chrono::steady_clock::now();
        static u32 status_games = 0, status_images = 0, status_hitches = 0;
        static double status_worst_ms = 0.0;
        static auto status_prev = status_window;
        ++status_games;
        status_images += 1 + shown;
        const auto status_now = std::chrono::steady_clock::now();
        {
            const double frame_ms = std::chrono::duration<double, std::milli>(status_now - status_prev).count();
            status_prev = status_now;
            status_worst_ms = std::max(status_worst_ms, frame_ms);
            status_hitches += frame_ms > 50.0 ? 1 : 0;
        }
        const double status_s = std::chrono::duration<double>(status_now - status_window).count();
        if (status_s >= 30.0) {
            const auto& cfg = BbSettings::Get();
            std::printf("Status: %.1f game fps, %.1f shown fps over %.0f s; frame generation %s, anti-aliasing %s",
                        status_games / status_s, status_images / status_s, status_s,
                        cfg.frame_gen == BbSettings::FrameGenOff ? "off"
                        : cfg.frame_gen_paused              ? "paused"
                                                            : BbSettings::FrameGenName(cfg.frame_gen),
                        BbSettings::AaName(cfg.aa_mode));
            std::printf("; worst frame %.0f ms, %u over 50 ms", status_worst_ms, status_hitches);
            {
                // Game files looked for and not found (runtime_file.c): on disk, and answered from memory.
                static u64 last_slow = 0, last_fast = 0, last_us = 0;
                u64 slow = 0, fast = 0, us = 0;
                runtime_file_miss_stats(&slow, &fast, &us);
                if (slow != last_slow || fast != last_fast) {
                    std::printf("; missing game files %llu on disk (%.0f ms), %llu from memory",
                                static_cast<unsigned long long>(slow - last_slow), (us - last_us) / 1000.0,
                                static_cast<unsigned long long>(fast - last_fast));
                }
                last_slow = slow, last_fast = fast, last_us = us;
            }
#ifdef _WIN32
            {
                PROCESS_MEMORY_COUNTERS counters{};
                if (K32GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters))) {
                    std::printf("; process memory %zu MB (peak %zu MB, commit %zu MB)",
                                size_t(counters.WorkingSetSize >> 20), size_t(counters.PeakWorkingSetSize >> 20),
                                size_t(counters.PagefileUsage >> 20));
                }
            }
#endif
            std::puts("");
            status_window = status_now;
            status_games = status_images = status_hitches = 0;
            status_worst_ms = 0.0;
        }
    }
    free_frame();
    if (!is_reusing_frame && is_game_frame) {
        DebugState.IncFlipFrameNum();
    }
}

void Presenter::UpdateFrameGenLimit(bool wanted, double present_ms) {
    auto& settings = BbSettings::Get();
    settings.game_fps = float(1000.0 / game_frame_ms.load(std::memory_order_relaxed));
    if (!wanted) {
        frame_gen_limit = 0;
        settings.frame_gen_now = 0;
        settings.frame_gen_flip_limit = 0;
        present_busy = 0.0;
        return;
    }
    const double interval = game_frame_ms.load(std::memory_order_relaxed);
    const u32 hz = BbDisplayRefreshHz();
    const auto now = std::chrono::steady_clock::now();
    const u32 before = frame_gen_limit.load(std::memory_order_relaxed);
    u32 limit = before;
    // The images the display shows per game frame: 60 FPS on 144 Hz has room for one generated
    // frame, on 240 Hz for three. Decided once a second from that second's game frame rate
    // (single slow frames while an area loads do not count): down at once when 10% short, up
    // after two seconds with 5% to spare.
    ++limit_frames;
    const double window_s = std::chrono::duration<double>(now - limit_window).count();
    if (window_s >= 1.0) {
        const double ratio = hz * window_s / limit_frames;
        limit_window = now;
        limit_frames = 0;
        if (window_s < 2.0) { // not the first, nor after a pause of the game
            if (limit > 0 && ratio < (limit + 1) * 0.9) {
                while (limit > 0 && ratio < (limit + 1) * 0.9) {
                    --limit;
                }
                limit_up_seconds = 0;
            } else if (limit < FrameGenerator::MaxGenerated && ratio >= (limit + 2) * 0.95) {
                if (++limit_up_seconds >= 2) {
                    ++limit;
                    limit_up_seconds = 0;
                }
            } else {
                limit_up_seconds = 0;
            }
        }
    }
    const bool fifo = swapchain.WaitsForDisplay();
    if (!settings.frame_gen_fit_display || fifo) {
        // The multiplier as chosen, past the display's refresh rate with an uncapped game (the
        // display shows the newest image at each refresh); with FIFO the game is capped below
        // instead. The pause below still applies.
        limit = FrameGenerator::MaxGenerated;
    }
    // The share of the game's frame interval spent presenting its images: paced, about
    // count / (count + 1). Near one the presentation holds the game back (FIFO on a display
    // slower than measured, a GPU too slow for generation): off for a while, longer each time.
    present_busy += (present_ms / interval - present_busy) * 0.1;
    if (before && present_busy > 0.97) {
        frame_gen_hold_seconds =
            now - frame_gen_hold_until > std::chrono::seconds(60)
                ? 5
                : std::min(frame_gen_hold_seconds * 2, 120);
        frame_gen_hold_until = now + std::chrono::seconds(frame_gen_hold_seconds);
        std::printf("Frame generation: paused for %d s, presenting does not keep up (%.1f of "
                    "%.1f ms per game frame; FIFO present mode or a slower display?)\n",
                    frame_gen_hold_seconds, present_ms, interval);
        present_busy = 0.0;
    }
    if (now < frame_gen_hold_until) {
        limit = 0;
    }
    if (limit != before && settings.frame_gen_fit_display) {
        std::printf("Frame generation: %u generated per game frame (display %u Hz, game %.0f "
                    "FPS)\n",
                    limit, hz, 1000.0 / interval);
    }
    // FIFO (V-Sync; AMD's Windows driver has no Mailbox) shows at most `hz` images a second
    // and makes each one more wait for the display: the game is capped so that its frames and
    // the generated ones fit, e.g. 72 FPS for 2x on 144 Hz (VideoOut applies it).
    int flip_limit = 0;
    if (fifo && limit) {
        const u32 count = std::min(frame_generator->WantedCount(), limit);
        flip_limit = count ? int(hz / (count + 1)) : 0;
    }
    if (flip_limit != settings.frame_gen_flip_limit.exchange(flip_limit) && flip_limit) {
        std::printf("Frame generation: V-Sync (FIFO) at %u Hz: the game is capped at %d FPS so "
                    "its frames and the generated ones fit\n",
                    hz, flip_limit);
    }
    frame_gen_limit = limit;
    settings.frame_gen_now = int(limit);
    settings.frame_gen_display_hz = int(hz);
}

bool Presenter::PresentImage(Frame* frame, vk::Image image, bool last) {
    if (!swapchain.AcquireNextImage()) {
        swapchain.Recreate(window.GetWidth(), window.GetHeight());
        if (!swapchain.AcquireNextImage()) {
            // User resizes the window too fast and GPU can't keep up. Skip this frame.
            LOG_WARNING(Render_Vulkan, "Skipping frame!");
            return false;
        }
    }

    // Reset fence for queue submission. Do it here instead of GetRenderFrame() because we may
    // skip frame because of slow swapchain recreation. If a frame skip occurs, we skip signal
    // the frame's present fence and future GetRenderFrame() call will hang waiting for this frame.
    if (last) {
        const auto reset_result = instance.GetDevice().resetFences(frame->present_done);
        ASSERT_MSG(reset_result == vk::Result::eSuccess,
                   "Unexpected error resetting present done fence: {}",
                   vk::to_string(reset_result));
    }

    // bbport: the frame is blitted (letterboxed) straight into the swapchain image; with HDR
    // output its HDR10 picture, which already has the overlay.
    const bool hdr_frame = hdr_output && frame->hdr_ready && image == frame->image;
    if (hdr_frame) {
        image = frame->hdr_image;
    }
    const vk::Image swapchain_image = swapchain.Image();
    auto& scheduler = present_scheduler;
    const auto cmdbuf = scheduler.CommandBuffer();

    if (EmulatorSettings.IsVkHostMarkersEnabled()) {
        cmdbuf.beginDebugUtilsLabelEXT(vk::DebugUtilsLabelEXT{
            .pLabelName = "Present",
        });
    }

    {
        const vk::Extent2D extent = swapchain.GetExtent();
        SetExpectedGameSize(s32(extent.width), s32(extent.height));
        const vk::ImageSubresourceRange color_range{
            .aspectMask = vk::ImageAspectFlagBits::eColor,
            .baseMipLevel = 0,
            .levelCount = 1,
            .baseArrayLayer = 0,
            .layerCount = VK_REMAINING_ARRAY_LAYERS,
        };
        const std::array pre_barriers{
            vk::ImageMemoryBarrier{
                .srcAccessMask = vk::AccessFlagBits::eNone,
                .dstAccessMask = vk::AccessFlagBits::eTransferWrite,
                .oldLayout = vk::ImageLayout::eUndefined,
                .newLayout = vk::ImageLayout::eTransferDstOptimal,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = swapchain_image,
                .subresourceRange = color_range,
            },
            vk::ImageMemoryBarrier{
                .srcAccessMask =
                    vk::AccessFlagBits::eColorAttachmentWrite | vk::AccessFlagBits::eShaderWrite,
                .dstAccessMask = vk::AccessFlagBits::eTransferRead,
                .oldLayout = vk::ImageLayout::eGeneral,
                .newLayout = vk::ImageLayout::eTransferSrcOptimal,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = image,
                .subresourceRange = color_range,
            },
        };
        cmdbuf.pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands,
                               vk::PipelineStageFlagBits::eTransfer, vk::DependencyFlagBits::eByRegion,
                               {}, {}, pre_barriers);
        const vk::ClearColorValue black{std::array<float, 4>{0.0f, 0.0f, 0.0f, 1.0f}};
        cmdbuf.clearColorImage(swapchain_image, vk::ImageLayout::eTransferDstOptimal, black, color_range);
        const vk::MemoryBarrier clear_done{
            .srcAccessMask = vk::AccessFlagBits::eTransferWrite,
            .dstAccessMask = vk::AccessFlagBits::eTransferWrite,
        };
        cmdbuf.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer, vk::PipelineStageFlagBits::eTransfer,
                               vk::DependencyFlagBits::eByRegion, clear_done, {}, {});
        cmdbuf.blitImage(image, vk::ImageLayout::eTransferSrcOptimal, swapchain_image,
                         vk::ImageLayout::eTransferDstOptimal,
                         MakeImageBlitFit(frame->width, frame->height, extent.width, extent.height),
                         vk::Filter::eLinear);
        // bbport: VR: the headset gets this frame too.
        if (last) {
            BbVr::g_queue_mutex = &Scheduler::submit_mutex;
            BbVr::RecordFrame(cmdbuf, image, frame->width, frame->height);
        }
        // bbport: the settings menu / FPS counter over the frame, at display resolution.
        BbOverlay::NotePresent();
        const bool overlay = BbOverlay::Visible() && !hdr_output;
        const std::array post_barriers{
            vk::ImageMemoryBarrier{
                .srcAccessMask = vk::AccessFlagBits::eTransferWrite,
                .dstAccessMask = overlay ? vk::AccessFlagBits::eColorAttachmentRead |
                                               vk::AccessFlagBits::eColorAttachmentWrite
                                         : vk::AccessFlagBits::eNone,
                .oldLayout = vk::ImageLayout::eTransferDstOptimal,
                .newLayout = overlay ? vk::ImageLayout::eColorAttachmentOptimal
                                     : vk::ImageLayout::ePresentSrcKHR,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = swapchain_image,
                .subresourceRange = color_range,
            },
            vk::ImageMemoryBarrier{
                .srcAccessMask = vk::AccessFlagBits::eTransferRead,
                .dstAccessMask = vk::AccessFlagBits::eColorAttachmentWrite,
                .oldLayout = vk::ImageLayout::eTransferSrcOptimal,
                .newLayout = vk::ImageLayout::eGeneral,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = image,
                .subresourceRange = color_range,
            },
        };
        cmdbuf.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
                               vk::PipelineStageFlagBits::eAllCommands,
                               vk::DependencyFlagBits::eByRegion, {}, {}, post_barriers);
        if (overlay) {
            BbOverlay::Render(cmdbuf, swapchain.ImageView(), extent);
            const vk::ImageMemoryBarrier to_present{
                .srcAccessMask = vk::AccessFlagBits::eColorAttachmentWrite,
                .dstAccessMask = vk::AccessFlagBits::eNone,
                .oldLayout = vk::ImageLayout::eColorAttachmentOptimal,
                .newLayout = vk::ImageLayout::ePresentSrcKHR,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = swapchain_image,
                .subresourceRange = color_range,
            };
            cmdbuf.pipelineBarrier(vk::PipelineStageFlagBits::eColorAttachmentOutput,
                                   vk::PipelineStageFlagBits::eBottomOfPipe,
                                   vk::DependencyFlagBits::eByRegion, {}, {}, to_present);
        }
    }
    if (EmulatorSettings.IsVkHostMarkersEnabled()) {
        cmdbuf.endDebugUtilsLabelEXT();
    }

    // Flush vulkan commands.

    SubmitInfo info{};
    info.AddWait(swapchain.GetImageAcquiredSemaphore());
    info.AddWait(frame->ready_semaphore, frame->ready_tick);
    info.AddSignal(swapchain.GetPresentReadySemaphore());
    if (last) {
        // Also covers the generated frames' submissions before it on this queue.
        info.AddSignal(frame->present_done);
    }
    scheduler.Flush(info);

    // Present to swapchain.
    {
        std::scoped_lock submit_lock{Scheduler::submit_mutex};
        if (last) {
            BbVr::AfterSubmit();
        }
        if (!swapchain.Present() && window.GetWidth() != 0 && window.GetHeight() != 0) {
            swapchain.Recreate(window.GetWidth(), window.GetHeight());
        }
    }
    g_last_shown = std::chrono::steady_clock::now();
    return true;
}

Frame* Presenter::GetRenderFrame() {
    // Wait for free presentation frames
    Frame* frame;
    {
        std::unique_lock lock{free_mutex};
        free_cv.wait(lock, [this] { return !free_queue.empty(); });
        LOG_DEBUG(Render_Vulkan, "Got render frame, remaining {}", free_queue.size() - 1);

        // Take the frame from the queue
        frame = free_queue.front();
        free_queue.pop();
    }

    const vk::Device device = instance.GetDevice();
    vk::Result result{};

    const auto wait = [&]() {
        result = device.waitForFences(frame->present_done, false, std::numeric_limits<u64>::max());
        return result;
    };

    // Wait for the presentation to be finished so all frame resources are free
    while (wait() != vk::Result::eSuccess) {
        if (result == vk::Result::eErrorDeviceLost) {
            instance.LogDeviceLost();
        }
        ASSERT_MSG(result != vk::Result::eErrorDeviceLost,
                   "Device lost during waiting for a frame");
        // Retry if the waiting times out
        if (result == vk::Result::eTimeout) {
            continue;
        }
    }

    if (frame->width != expected_frame_width || frame->height != expected_frame_height ||
        frame->is_hdr != GameHdr() || (hdr_output && !frame->hdr_image)) {
        RecreateFrame(frame, expected_frame_width, expected_frame_height);
    }

    return frame;
}

void Presenter::SetExpectedGameSize(s32 width, s32 height) {
    if (width <= 0 || height <= 0) {
        return; // no surface (minimized window): keep the last frame size
    }
    const float ratio = (float)width / (float)height;

    expected_frame_height = height;
    expected_frame_width = width;
    if (ratio > expected_ratio) {
        expected_frame_width = static_cast<s32>(height * expected_ratio);
    } else {
        expected_frame_height = static_cast<s32>(width / expected_ratio);
    }
}

} // namespace Vulkan
