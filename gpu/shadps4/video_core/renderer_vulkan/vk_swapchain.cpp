// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "bbport_settings.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <limits>
#include "common/assert.h"
#include "common/logging/log.h"
#include "core/emulator_settings.h"
#include "imgui/renderer/imgui_core.h"
#include "sdl_window.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_swapchain.h"
#include "bbport_diagnostics.h"

namespace Vulkan {

static constexpr vk::SurfaceFormatKHR SURFACE_FORMAT_HDR = {
    .format = vk::Format::eA2B10G10R10UnormPack32,
    .colorSpace = vk::ColorSpaceKHR::eHdr10St2084EXT,
};

Swapchain::Swapchain(const Instance& instance_, const Frontend::WindowSDL& window_)
    : instance{instance_}, window{window_}, surface{CreateSurface(instance.GetInstance(), window)} {
    FindPresentFormat();
    FindPresentMode();

    Create(window.GetWidth(), window.GetHeight());
    ImGui::Core::Initialize(instance, window, image_count, surface_format.format);
}

Swapchain::~Swapchain() {
    Destroy();
    instance.GetInstance().destroySurfaceKHR(surface);
}

void Swapchain::Create(u32 width_, u32 height_) {
    width = width_;
    height = height_;
    needs_recreation = false;

    Destroy();

    SetSurfaceProperties();

    const std::array queue_family_indices = {
        instance.GetGraphicsQueueFamilyIndex(),
        instance.GetPresentQueueFamilyIndex(),
    };

    const bool exclusive = queue_family_indices[0] == queue_family_indices[1];
    const u32 queue_family_indices_count = exclusive ? 1u : 2u;
    const vk::SharingMode sharing_mode =
        exclusive ? vk::SharingMode::eExclusive : vk::SharingMode::eConcurrent;
    const auto format = needs_hdr ? SURFACE_FORMAT_HDR : surface_format;
    // A swapchain can only use Reflex's low-latency mode when it is created for it.
    VkSwapchainLatencyCreateInfoNV latency{};
    latency.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_LATENCY_CREATE_INFO_NV;
    latency.latencyModeEnable = VK_TRUE;
    vk::SwapchainCreateInfoKHR swapchain_info = {
        .surface = surface,
        .minImageCount = image_count,
        .imageFormat = format.format,
        .imageColorSpace = format.colorSpace,
        .imageExtent = extent,
        .imageArrayLayers = 1,
        .imageUsage = vk::ImageUsageFlagBits::eColorAttachment |
                      vk::ImageUsageFlagBits::eTransferSrc | vk::ImageUsageFlagBits::eTransferDst,
        .imageSharingMode = sharing_mode,
        .queueFamilyIndexCount = queue_family_indices_count,
        .pQueueFamilyIndices = queue_family_indices.data(),
        .preTransform = transform,
        .compositeAlpha = composite_alpha,
        .presentMode = present_mode,
        .clipped = true,
        .oldSwapchain = nullptr,
    };
    if (instance.HasLowLatency2()) {
        swapchain_info.pNext = &latency;
    }

    auto [swapchain_result, chain] = instance.GetDevice().createSwapchainKHR(swapchain_info);
    ASSERT_MSG(swapchain_result == vk::Result::eSuccess, "Failed to create swapchain: {}",
               vk::to_string(swapchain_result));
    {
        std::scoped_lock lock{latency_mutex};
        swapchain = chain;
    }
    SetReflex(BbSettings::Get().reflex);
    {
        static int reported = 0;
        if (reported++ < 4) {
            std::printf("Swapchain: %ux%u, %s, present mode %s, %u images\n", extent.width, extent.height,
                        vk::to_string(format.format).c_str(), vk::to_string(present_mode).c_str(), image_count);
        }
    }

    SetupImages();
    RefreshSemaphores();
}

void Swapchain::Recreate(u32 width_, u32 height_) {
    LOG_DEBUG(Render_Vulkan, "Recreate the swapchain: width={} height={} HDR={}", width_, height_,
              needs_hdr);
    Create(width_, height_);
}

void Swapchain::SetHDR(bool hdr) {
    if (needs_hdr == hdr) {
        return;
    }

    auto result = Scheduler::WaitDeviceIdle(instance);
    if (result != vk::Result::eSuccess) {
        LOG_WARNING(ImGui, "Failed to wait for Vulkan device idle on mode change: {}",
                    vk::to_string(result));
    }

    needs_hdr = hdr;
    Recreate(width, height);
    ImGui::Core::OnSurfaceFormatChange(needs_hdr ? SURFACE_FORMAT_HDR.format
                                                 : surface_format.format);
}

void Swapchain::SetReflex(int setting) {
    std::scoped_lock lock{latency_mutex};
    const int requested = std::clamp(setting, 0, BbSettings::ReflexCount - 1);
    if (requested == reflex_mode) {
        return;
    }
    const auto set_mode = instance.SetLatencySleepModeFn();
    if (!instance.HasLowLatency2() || !set_mode) {
        if (requested != BbSettings::ReflexOff && reflex_mode != requested) {
            std::puts("Reflex: VK_NV_low_latency2 is not available on this system");
        }
        reflex_mode = requested;
        return;
    }
    if (!swapchain) {
        return;
    }
    if (!latency_semaphore) {
        const vk::SemaphoreTypeCreateInfo type{
            .semaphoreType = vk::SemaphoreType::eTimeline,
        };
        auto [result, semaphore] = instance.GetDevice().createSemaphoreUnique({.pNext = &type});
        ASSERT_MSG(result == vk::Result::eSuccess, "Failed creating Reflex sleep semaphore");
        latency_semaphore = std::move(semaphore);
    }
    VkLatencySleepModeInfoNV mode{};
    mode.sType = VK_STRUCTURE_TYPE_LATENCY_SLEEP_MODE_INFO_NV;
    mode.lowLatencyMode = requested != BbSettings::ReflexOff;
    mode.lowLatencyBoost = requested == BbSettings::ReflexBoost;
    mode.minimumIntervalUs = 0;
    const VkResult result =
        set_mode(VkDevice(instance.GetDevice()), VkSwapchainKHR(swapchain), &mode);
    ASSERT_MSG(result == VK_SUCCESS, "Failed setting Reflex mode: {}", int(result));
    reflex_mode = requested;
    std::printf("Reflex: %s (VK_NV_low_latency2, lowLatencyMode=%u lowLatencyBoost=%u)\n",
                requested == BbSettings::ReflexBoost ? "on + boost"
                : requested == BbSettings::ReflexOn ? "on" : "off",
                mode.lowLatencyMode, mode.lowLatencyBoost);
}

void Swapchain::MarkLatencyLocked(u64 id, VkLatencyMarkerNV marker) {
    if (!instance.HasLowLatency2() || !swapchain || !id) {
        return;
    }
    const VkSetLatencyMarkerInfoNV info{
        .sType = VK_STRUCTURE_TYPE_SET_LATENCY_MARKER_INFO_NV,
        .presentID = id,
        .marker = marker,
    };
    instance.SetLatencyMarkerFn()(VkDevice(instance.GetDevice()), VkSwapchainKHR(swapchain), &info);
}

void Swapchain::MarkLatency(u64 id, VkLatencyMarkerNV marker) {
    if (!id) {
        return;
    }
    std::scoped_lock lock{latency_mutex};
    MarkLatencyLocked(id, marker);
}

void Swapchain::BeforeInput() {
    std::scoped_lock input_lock{input_mutex};
    SetReflex(BbSettings::Get().reflex);
    const auto sleep_start = std::chrono::steady_clock::now();
    vk::Semaphore semaphore;
    u64 tick;
    u64 generation;
    {
        std::scoped_lock lock{latency_mutex};
        if (!instance.HasLowLatency2() || !swapchain || latency_input_id) {
            return;
        }
        semaphore = *latency_semaphore;
        tick = ++latency_tick;
        generation = latency_generation;
        const VkLatencySleepInfoNV sleep{
            .sType = VK_STRUCTURE_TYPE_LATENCY_SLEEP_INFO_NV,
            .signalSemaphore = VkSemaphore(semaphore),
            .value = tick,
        };
        const auto result = instance.LatencySleepFn()(VkDevice(instance.GetDevice()),
                                                     VkSwapchainKHR(swapchain), &sleep);
        ASSERT_MSG(result == VK_SUCCESS, "Reflex sleep failed: {}", int(result));
    }
    // Presentation and swapchain recreation must be able to progress during the wait.
    const vk::SemaphoreWaitInfo wait{
        .semaphoreCount = 1,
        .pSemaphores = &semaphore,
        .pValues = &tick,
    };
    const auto result = instance.GetDevice().waitSemaphores(wait, std::numeric_limits<u64>::max());
    ASSERT_MSG(result == vk::Result::eSuccess, "Reflex sleep wait failed: {}", vk::to_string(result));
    if (tick % 60 == 0 && BB_DIAGNOSTIC_ENV("BB_FPS_LOG")) {
        std::printf("Reflex: sleep frame %llu %.3f ms\n", tick,
                    std::chrono::duration<double, std::milli>(
                        std::chrono::steady_clock::now() - sleep_start).count());
    }
    std::scoped_lock lock{latency_mutex};
    if (swapchain && generation == latency_generation) {
        MarkLatencyLocked(tick, VK_LATENCY_MARKER_SIMULATION_START_NV);
        MarkLatencyLocked(tick, VK_LATENCY_MARKER_INPUT_SAMPLE_NV);
        latency_input_id = tick;
    }
}

u64 Swapchain::BeginLatencyRender() {
    std::scoped_lock input_lock{input_mutex};
    std::scoped_lock lock{latency_mutex};
    if (!instance.HasLowLatency2() || !swapchain) {
        return 0;
    }
    // Polling can repeat within one guest submission epoch. Only its first poll sleeps.
    const u64 id = latency_input_id ? latency_input_id : ++latency_tick;
    if (!latency_input_id) {
        MarkLatencyLocked(id, VK_LATENCY_MARKER_SIMULATION_START_NV);
    }
    latency_input_id = 0;
    MarkLatencyLocked(id, VK_LATENCY_MARKER_SIMULATION_END_NV);
    return id;
}

void Swapchain::EndLatencyPresent(u64 id) {
    if (!id) {
        return;
    }
    std::scoped_lock lock{latency_mutex};
    MarkLatencyLocked(id, VK_LATENCY_MARKER_PRESENT_END_NV);
    if (!instance.HasLowLatency2() || !swapchain || ++latency_presented % 60 ||
        !BB_DIAGNOSTIC_ENV("BB_FPS_LOG")) {
        return;
    }
    std::array<VkLatencyTimingsFrameReportNV, 64> reports{};
    for (auto& report : reports) {
        report.sType = VK_STRUCTURE_TYPE_LATENCY_TIMINGS_FRAME_REPORT_NV;
    }
    VkGetLatencyMarkerInfoNV info{
        .sType = VK_STRUCTURE_TYPE_GET_LATENCY_MARKER_INFO_NV,
        .timingCount = u32(reports.size()),
        .pTimings = reports.data(),
    };
    instance.GetLatencyTimingsFn()(VkDevice(instance.GetDevice()), VkSwapchainKHR(swapchain), &info);
    for (u32 i = 0; i < info.timingCount; ++i) {
        const auto& report = reports[i];
        if (report.presentID > latency_reported && report.inputSampleTimeUs &&
            report.gpuRenderEndTimeUs && report.presentEndTimeUs) {
            std::printf("Reflex: frame %llu input %llu simulation %llu-%llu render %llu-%llu "
                        "GPU %llu-%llu present %llu-%llu us\n", report.presentID,
                        report.inputSampleTimeUs, report.simStartTimeUs, report.simEndTimeUs,
                        report.renderSubmitStartTimeUs, report.renderSubmitEndTimeUs,
                        report.gpuRenderStartTimeUs, report.gpuRenderEndTimeUs,
                        report.presentStartTimeUs, report.presentEndTimeUs);
            latency_reported = report.presentID;
        }
    }
}

bool Swapchain::AcquireNextImage(Scheduler& scheduler) {
    scheduler.Wait(image_acquired_ticks[frame_index]);
    vk::Device device = instance.GetDevice();
    vk::Result result =
        device.acquireNextImageKHR(swapchain, std::numeric_limits<u64>::max(),
                                   image_acquired[frame_index], VK_NULL_HANDLE, &image_index);

    switch (result) {
    case vk::Result::eSuccess:
        break;
    case vk::Result::eSuboptimalKHR:
    case vk::Result::eErrorSurfaceLostKHR:
    case vk::Result::eErrorOutOfDateKHR:
    case vk::Result::eErrorUnknown:
        needs_recreation = true;
        break;
    default:
        LOG_CRITICAL(Render_Vulkan, "Swapchain acquire returned unknown result {}",
                     vk::to_string(result));
        UNREACHABLE();
        break;
    }

    return !needs_recreation;
}

bool Swapchain::Present(u64 latency_id) {
    vk::PresentInfoKHR present_info = {
        .waitSemaphoreCount = 1,
        .pWaitSemaphores = &present_ready[image_index],
        .swapchainCount = 1,
        .pSwapchains = &swapchain,
        .pImageIndices = &image_index,
    };
    // Reflex marker IDs identify game frames, including their generated presents. They are
    // independent of VkPresentIdKHR's per-presentation IDs.
    MarkLatency(latency_id, VK_LATENCY_MARKER_OUT_OF_BAND_PRESENT_START_NV);
    auto result = instance.GetPresentQueue().presentKHR(present_info);
    MarkLatency(latency_id, VK_LATENCY_MARKER_OUT_OF_BAND_PRESENT_END_NV);
    if (result == vk::Result::eErrorOutOfDateKHR || result == vk::Result::eSuboptimalKHR) {
        needs_recreation = true;
    } else {
        ASSERT_MSG(result == vk::Result::eSuccess, "Swapchain presentation failed: {}",
                   vk::to_string(result));
    }

    frame_index = (frame_index + 1) % image_count;

    return !needs_recreation;
}

void Swapchain::FindPresentFormat() {
    const auto [formats_result, formats] =
        instance.GetPhysicalDevice().getSurfaceFormatsKHR(surface);
    ASSERT_MSG(formats_result == vk::Result::eSuccess, "Failed to query surface formats: {}",
               vk::to_string(formats_result));

    // Check if the device supports HDR formats. Here we care of Rec.2020 PQ only as it is expected
    // game output. Other variants as e.g. linear Rec.2020 will require additional color space
    // rotation
    supports_hdr =
        std::find_if(formats.begin(), formats.end(), [](const vk::SurfaceFormatKHR& format) {
            return format == SURFACE_FORMAT_HDR;
        }) != formats.end();
    // Also make sure that user allowed us to use HDR
    supports_hdr &= EmulatorSettings.IsHdrAllowed();

    // If there is a single undefined surface format, the device doesn't care, so we'll just use
    // RGBA sRGB.
    if (formats[0].format == vk::Format::eUndefined) {
        surface_format.format = vk::Format::eR8G8B8A8Unorm;
        surface_format.colorSpace = vk::ColorSpaceKHR::eSrgbNonlinear;
        return;
    }

    // Try to find a suitable format.
    for (const vk::SurfaceFormatKHR& sformat : formats) {
        vk::Format format = sformat.format;
        if (format != vk::Format::eR8G8B8A8Unorm && format != vk::Format::eB8G8R8A8Unorm) {
            continue;
        }

        surface_format.format = format;
        surface_format.colorSpace = sformat.colorSpace;
        return;
    }

    UNREACHABLE_MSG("Unable to find required swapchain format!");
}

void Swapchain::FindPresentMode() {
    const auto [modes_result, modes] =
        instance.GetPhysicalDevice().getSurfacePresentModesKHR(surface);
    if (modes_result != vk::Result::eSuccess) {
        LOG_ERROR(Render, "Failed to query available present modes, falling back to Fifo as "
                          "guaranteed supported option.");
        present_mode = vk::PresentModeKHR::eFifo;
        return;
    }

    const auto requested_mode = EmulatorSettings.GetPresentMode();
    if (requested_mode == "Mailbox") {
        present_mode = vk::PresentModeKHR::eMailbox;
    } else if (requested_mode == "Fifo") {
        present_mode = vk::PresentModeKHR::eFifo;
    } else if (requested_mode == "Immediate") {
        present_mode = vk::PresentModeKHR::eImmediate;
    } else {
        LOG_ERROR(Render_Vulkan, "Unknown present mode {}, defaulting to Mailbox.",
                  EmulatorSettings.GetPresentMode());
        present_mode = vk::PresentModeKHR::eMailbox;
    }

    if (std::ranges::find(modes, present_mode) == modes.cend()) {
        // FIFO is guaranteed to be supported by the Vulkan spec.
        constexpr auto fallback = vk::PresentModeKHR::eFifo;
        LOG_WARNING(Render, "Requested present mode {} is not supported, falling back to {}.",
                    vk::to_string(present_mode), vk::to_string(fallback));
        present_mode = fallback;
    }
}

void Swapchain::SetSurfaceProperties() {
    const auto [capabilities_result, capabilities] =
        instance.GetPhysicalDevice().getSurfaceCapabilitiesKHR(surface);
    ASSERT_MSG(capabilities_result == vk::Result::eSuccess,
               "Failed to query surface capabilities: {}", vk::to_string(capabilities_result));

    extent = capabilities.currentExtent;
    if (capabilities.currentExtent.width == std::numeric_limits<u32>::max()) {
        extent.width = std::max(capabilities.minImageExtent.width,
                                std::min(capabilities.maxImageExtent.width, width));
        extent.height = std::max(capabilities.minImageExtent.height,
                                 std::min(capabilities.maxImageExtent.height, height));
    }

    // Select number of images in swap chain, we prefer one buffer in the background to work on
    // (bbport: two with frame generation, which presents several images per game frame).
    image_count = capabilities.minImageCount +
                  (BbSettings::Get().frame_gen != BbSettings::FrameGenOff ? 2 : 1);
    if (capabilities.maxImageCount > 0) {
        image_count = std::min(image_count, capabilities.maxImageCount);
    }

    // Prefer identity transform if possible
    transform = vk::SurfaceTransformFlagBitsKHR::eIdentity;
    if (!(capabilities.supportedTransforms & transform)) {
        transform = capabilities.currentTransform;
    }

    // Opaque is not supported everywhere.
    composite_alpha = vk::CompositeAlphaFlagBitsKHR::eOpaque;
    if (!(capabilities.supportedCompositeAlpha & vk::CompositeAlphaFlagBitsKHR::eOpaque)) {
        composite_alpha = vk::CompositeAlphaFlagBitsKHR::eInherit;
    }
}

void Swapchain::Destroy() {
    vk::Device device = instance.GetDevice();
    const auto wait_result = Scheduler::WaitDeviceIdle(instance);
    if (wait_result != vk::Result::eSuccess) {
        LOG_WARNING(Render_Vulkan, "Failed to wait for device to become idle: {}",
                    vk::to_string(wait_result));
    }

    for (auto& image_view : images_view) {
        device.destroyImageView(image_view);
    }
    images_view.clear();

    if (swapchain) {
        std::scoped_lock lock{latency_mutex};
        device.destroySwapchainKHR(swapchain);
        swapchain = nullptr;
        reflex_mode = -1;
        ++latency_generation;
        latency_input_id = 0;
    }

    for (const auto& sem : image_acquired) {
        device.destroySemaphore(sem);
    }
    for (const auto& sem : present_ready) {
        device.destroySemaphore(sem);
    }

    image_acquired.clear();
    present_ready.clear();
}

void Swapchain::RefreshSemaphores() {
    const vk::Device device = instance.GetDevice();
    image_acquired.resize(image_count);
    image_acquired_ticks.assign(image_count, 0);
    frame_index = 0;
    present_ready.resize(image_count);

    for (vk::Semaphore& semaphore : image_acquired) {
        auto [semaphore_result, sem] = device.createSemaphore({});
        ASSERT_MSG(semaphore_result == vk::Result::eSuccess,
                   "Failed to create image acquired semaphore: {}",
                   vk::to_string(semaphore_result));
        semaphore = sem;
    }
    for (vk::Semaphore& semaphore : present_ready) {
        auto [semaphore_result, sem] = device.createSemaphore({});
        ASSERT_MSG(semaphore_result == vk::Result::eSuccess,
                   "Failed to create present ready semaphore: {}", vk::to_string(semaphore_result));
        semaphore = sem;
    }

    for (u32 i = 0; i < image_count; ++i) {
        SetObjectName(device, image_acquired[i], "Swapchain Semaphore: image_acquired {}", i);
        SetObjectName(device, present_ready[i], "Swapchain Semaphore: present_ready {}", i);
    }
}

void Swapchain::SetupImages() {
    vk::Device device = instance.GetDevice();
    auto [images_result, imgs] = device.getSwapchainImagesKHR(swapchain);
    ASSERT_MSG(images_result == vk::Result::eSuccess, "Failed to create swapchain images: {}",
               vk::to_string(images_result));
    images = std::move(imgs);
    image_count = static_cast<u32>(images.size());
    images_view.resize(image_count);
    for (u32 i = 0; i < image_count; ++i) {
        if (images_view[i]) {
            device.destroyImageView(images_view[i]);
        }
        auto [im_view_result, im_view] = device.createImageView(vk::ImageViewCreateInfo{
            .image = images[i],
            .viewType = vk::ImageViewType::e2D,
            .format = needs_hdr ? SURFACE_FORMAT_HDR.format : surface_format.format,
            .subresourceRange =
                {
                    .aspectMask = vk::ImageAspectFlagBits::eColor,
                    .levelCount = 1,
                    .layerCount = 1,
                },
        });
        ASSERT_MSG(im_view_result == vk::Result::eSuccess, "Failed to create image view: {}",
                   vk::to_string(im_view_result));
        images_view[i] = im_view;
    }

    for (u32 i = 0; i < image_count; ++i) {
        SetObjectName(device, images[i], "Swapchain Image {}", i);
        SetObjectName(device, images_view[i], "Swapchain ImageView {}", i);
    }
}

} // namespace Vulkan
