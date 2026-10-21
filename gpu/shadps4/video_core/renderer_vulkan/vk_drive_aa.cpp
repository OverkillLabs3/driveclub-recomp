// SPDX-License-Identifier: GPL-2.0-or-later
// Changed for this port. The upstream sources of the vendored tree are listed in
// gpu/VENDOR.txt and in NOTICE.
//
#include "video_core/renderer_vulkan/vk_drive_aa.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "bbport_settings.h"
#include "ffx_vk_portable.h"
#include "video_core/host_shaders/drive_aa_copy_comp.h"
#include "video_core/renderer_vulkan/vk_dlss.h"
#include "video_core/renderer_vulkan/vk_drive_motion.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_runtime.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"
#include "video_core/texture_cache/texture_cache.h"

namespace Vulkan {

namespace {

float Halton(u32 index, u32 base) {
    float f = 1.0f, r = 0.0f;
    while (index > 0) {
        f /= float(base);
        r += f * float(index % base);
        index /= base;
    }
    return r;
}

FfxVkPortableImage Describe(vk::Image image, vk::Format format, u32 width, u32 height,
                            vk::ImageUsageFlags usage, vk::ImageAspectFlags aspect,
                            FfxVkPortableResourceState state) {
    FfxVkPortableImage out{};
    out.structSize = sizeof(out);
    out.image = image;
    out.format = static_cast<VkFormat>(format);
    out.extent = {width, height};
    out.mipCount = 1;
    out.arrayLayers = 1;
    out.usage = static_cast<VkImageUsageFlags>(usage);
    out.aspect = static_cast<VkImageAspectFlags>(aspect);
    out.state = state;
    return out;
}

void PrintIssues(const char* what, u64 issues) {
    std::printf("Anti-aliasing: %s invalid:", what);
    for (u32 bit = 0; bit < 64; ++bit) {
        if (issues & (1ull << bit)) {
            std::printf(" %s", ffxVkPortableValidationIssueName(1ull << bit));
        }
    }
    std::puts("");
}


} // namespace

DriveAA::DriveAA(const Instance& instance_, Scheduler& scheduler_,
                 VideoCore::TextureCache& texture_cache_, Runtime& runtime_, DriveMotion& motion_)
    : instance{instance_}, scheduler{scheduler_}, texture_cache{texture_cache_},
      runtime{runtime_}, motion{motion_} {}

DriveAA::~DriveAA() {
    if (context) {
        ffxVkPortableUpscaleContextDestroy(context);
    }
}

bool DriveAA::Active() const {
    return !failed && BbSettings::Get().aa_mode != BbSettings::AaOff;
}

// Upscaling is not finished: it only runs for development with BB_AA_UPSCALE_DEV=1.
static bool UpscalingAllowed() {
    static const bool allowed = std::getenv("BB_AA_UPSCALE_DEV") != nullptr;
    return allowed;
}

bool DriveAA::Upscaling() const {
    return Active() && UpscalingAllowed() && BbSettings::Get().aa_scale != BbSettings::AaScaleNative;
}

float DriveAA::MipBias() const {
    return Upscaling() ? -std::log2(BbSettings::AaScaleFactor(BbSettings::Get().aa_scale)) : 0.0f;
}

void DriveAA::NextFrame() {
    const int mode = BbSettings::Get().aa_mode;
    if (mode != applied_mode) {
        applied_mode = mode;
        reset = true;
        failed = false;
        jitter_index = 0;
    }
    // Called for every draw into the display buffer: "running" means it ran lately.
    BbSettings::Get().aa_running =
        std::chrono::steady_clock::now() - last_run < std::chrono::milliseconds(300);
    if (!Active()) {
        jitter = {};
        return;
    }
    // 8 phases per 1:1 (16 for the native anti-aliasing), more as the scale grows.
    const float factor = UpscalingAllowed() ? BbSettings::AaScaleFactor(BbSettings::Get().aa_scale) : 1.0f;
    const u32 phases = factor > 1.0f ? u32(std::ceil(8.0f * factor * factor)) : 16u;
    if (redirect_valid &&
        std::chrono::steady_clock::now() - last_run > std::chrono::milliseconds(300)) {
        redirect_valid = false;
    }
    jitter_index = jitter_index % phases + 1;
    jitter = {Halton(jitter_index, 2) - 0.5f, Halton(jitter_index, 3) - 0.5f};
}

vk::ImageView DriveAA::View(const VideoCore::Image& image, vk::Format format,
                            vk::ImageAspectFlags aspect) {
    const vk::Image handle = image.GetImage();
    CachedView* oldest = &views[0];
    for (auto& entry : views) {
        if (entry.view && entry.image == handle && entry.uid == image.image_uid &&
            entry.format == format && entry.aspect == aspect) {
            entry.last_use = ++view_uses;
            return entry.view;
        }
        if (entry.last_use < oldest->last_use) {
            oldest = &entry;
        }
    }
    const auto device = instance.GetDevice();
    if (oldest->view) {
        scheduler.DeferOperation([device, view = oldest->view] { device.destroyImageView(view); });
    }
    *oldest = {
        .image = handle,
        .uid = image.image_uid,
        .format = format,
        .aspect = aspect,
        .view = Check(device.createImageView({
            .image = handle,
            .viewType = vk::ImageViewType::e2D,
            .format = format,
            .subresourceRange = {aspect, 0, 1, 0, 1},
        })),
        .last_use = ++view_uses,
    };
    return oldest->view;
}

void DriveAA::CreatePipeline() {
    if (copy_pipeline) {
        return;
    }
    const auto device = instance.GetDevice();
    const std::array<vk::DescriptorSetLayoutBinding, 2> bindings = {{
        {.binding = 0,
         .descriptorType = vk::DescriptorType::eStorageImage,
         .descriptorCount = 1,
         .stageFlags = vk::ShaderStageFlagBits::eCompute},
        {.binding = 1,
         .descriptorType = vk::DescriptorType::eStorageImage,
         .descriptorCount = 1,
         .stageFlags = vk::ShaderStageFlagBits::eCompute},
    }};
    copy_desc_layout = Check(device.createDescriptorSetLayoutUnique({
        .flags = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR,
        .bindingCount = static_cast<u32>(bindings.size()),
        .pBindings = bindings.data(),
    }));
    const vk::PushConstantRange range{.stageFlags = vk::ShaderStageFlagBits::eCompute,
                                      .offset = 0,
                                      .size = 2 * sizeof(float)};
    copy_pipeline_layout = Check(device.createPipelineLayoutUnique({
        .setLayoutCount = 1,
        .pSetLayouts = &*copy_desc_layout,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &range,
    }));
    const auto module = CompileSPV(DRIVE_AA_COPY_COMP, device);
    copy_pipeline = Check(device.createComputePipelineUnique(
        {}, vk::ComputePipelineCreateInfo{
                .stage = {.stage = vk::ShaderStageFlagBits::eCompute,
                          .module = module,
                          .pName = "main"},
                .layout = *copy_pipeline_layout,
            }));
    device.destroyShaderModule(module);
}

bool DriveAA::EnsureResources(u32 w, u32 h, u32 ow, u32 oh, bool dlss) {
    if (ready && w == width && h == height && ow == out_width && oh == out_height &&
        (context != nullptr) == !dlss) {
        return true;
    }
    scheduler.Finish();
    ready = false;
    if (context) {
        ffxVkPortableUpscaleContextDestroy(context);
        context = nullptr;
    }
    width = w;
    height = h;
    out_width = ow;
    out_height = oh;
    if (!dlss) {
        FfxVkPortableDeviceInfo device_info{};
        device_info.structSize = sizeof(device_info);
        device_info.instance = instance.GetInstance();
        device_info.physicalDevice = instance.GetPhysicalDevice();
        device_info.device = instance.GetDevice();
        device_info.getDeviceProcAddr = VULKAN_HPP_DEFAULT_DISPATCHER.vkGetDeviceProcAddr;
        device_info.queue = instance.GetGraphicsQueue();
        device_info.queueFamilyIndex = instance.GetGraphicsQueueFamilyIndex();
        device_info.shaderFloat16Enabled = instance.IsShaderFloat16Enabled();
        device_info.subgroupSizeControlEnabled = instance.IsSubgroupSizeControlEnabled();
        device_info.synchronization2Enabled = VK_TRUE;
        device_info.shaderStorageImageWriteWithoutFormatEnabled = VK_TRUE;
        FfxVkPortableUpscaleCreateInfo create_info{};
        create_info.structSize = sizeof(create_info);
        // The game renders reversed Z with an infinite far plane.
        create_info.flags = FFX_VK_PORTABLE_CONTEXT_HDR_COLOR_INPUT |
                            FFX_VK_PORTABLE_CONTEXT_AUTO_EXPOSURE |
                            FFX_VK_PORTABLE_CONTEXT_DEPTH_INVERTED |
                            FFX_VK_PORTABLE_CONTEXT_DEPTH_INFINITE;
        create_info.maxRenderSize = {w, h};
        create_info.maxOutputSize = {ow, oh};
        if (const u64 issues = ffxVkPortableValidateUpscaleCreateInfo(&create_info)) {
            PrintIssues("create info", issues);
            return false;
        }
        if (ffxVkPortableUpscaleContextCreate(&device_info, &create_info, &context) !=
            FFX_VK_PORTABLE_OK) {
            std::puts("Anti-aliasing: FSR 3 context creation failed");
            context = nullptr;
            return false;
        }
    }
    const auto device = instance.GetDevice();
    const auto allocator = instance.GetAllocator();
    const auto make = [&](VideoCore::UniqueImage& image, vk::UniqueImageView& view,
                          vk::Format format, vk::ImageUsageFlags usage, u32 iw, u32 ih) {
        view.reset();
        image = VideoCore::UniqueImage(device, allocator);
        image.Create(vk::ImageCreateInfo{
            .imageType = vk::ImageType::e2D,
            .format = format,
            .extent = {iw, ih, 1},
            .mipLevels = 1,
            .arrayLayers = 1,
            .samples = vk::SampleCountFlagBits::e1,
            .tiling = vk::ImageTiling::eOptimal,
            .usage = usage,
            .initialLayout = vk::ImageLayout::eUndefined,
        });
        view = Check(device.createImageViewUnique({
            .image = vk::Image(image),
            .viewType = vk::ImageViewType::e2D,
            .format = format,
            .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
        }));
    };
    make(motion_image, motion_view, vk::Format::eR16G16Sfloat,
         vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled, w, h);
    make(result_image, result_view, vk::Format::eR16G16B16A16Sfloat,
         vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled |
             vk::ImageUsageFlagBits::eTransferSrc,
         ow, oh);
    CreatePipeline();
    ready = true;
    reset = true;
    if (ow == w && oh == h) {
        std::printf("Anti-aliasing: %s %ux%u", dlss ? "DLAA" : "FSR 3.1 native", w, h);
    } else {
        std::printf("Upscaling: %s %ux%u to %ux%u", dlss ? "DLSS" : "FSR 3.1", w, h, ow, oh);
    }
    std::puts("");
    return true;
}

void DriveAA::OnTaa(VideoCore::ImageId color_id, VideoCore::ImageId depth_id,
                    VideoCore::ImageId output_id) {
    static int reported = 0;
    const auto skip = [&](const char* why) {
        reset = true;
        if (reported++ < 6) {
            std::printf("Anti-aliasing: skipped, %s", why);
            std::puts("");
        }
    };
    if (!Active() || !color_id || !depth_id || !output_id) {
        return skip("no image");
    }
    if (!motion.Ready()) {
        return skip("the game camera is not known yet");
    }
    auto& color = texture_cache.GetImage(color_id);
    auto& depth = texture_cache.GetImage(depth_id);
    auto& output = texture_cache.GetImage(output_id);
    const u32 w = color.info.size.width, h = color.info.size.height;
    if (depth.info.size.width != w || depth.info.size.height != h || output.info.size.width != w ||
        output.info.size.height != h || !(output.usage_flags & vk::ImageUsageFlagBits::eStorage)) {
        std::printf("Anti-aliasing: sizes color %ux%u depth %ux%u output %ux%u storage %d", w, h,
                    depth.info.size.width, depth.info.size.height, output.info.size.width,
                    output.info.size.height, int(bool(output.usage_flags & vk::ImageUsageFlagBits::eStorage)));
        std::puts("");
        return skip("image sizes or usage differ");
    }
    const bool want_dlss = BbSettings::Get().aa_mode == BbSettings::AaDlss;
    Dlss* dlss = want_dlss ? Dlss::Get() : nullptr;
    if (want_dlss && (!dlss || !dlss->Available())) {
        std::puts("Anti-aliasing: DLSS is not available; using FSR 3.1");
        BbSettings::Get().aa_mode = BbSettings::AaFsr3;
        reset = true;
        return;
    }
    const float factor = UpscalingAllowed() ? BbSettings::AaScaleFactor(BbSettings::Get().aa_scale) : 1.0f;
    scale = factor;
    const bool upscale = factor > 1.0f;
    const u32 ow = upscale ? ((u32(std::lround(float(w) * factor)) + 1) & ~1u) : w;
    const u32 oh = upscale ? ((u32(std::lround(float(h) * factor)) + 1) & ~1u) : h;
    redirect_valid = false;
    if (!EnsureResources(w, h, ow, oh, want_dlss)) {
        failed = true;
        return;
    }
    const auto color_format = color.info.pixel_format;
    const auto depth_format = depth.info.pixel_format;
    const auto color_view = View(color, color_format, vk::ImageAspectFlagBits::eColor);
    const auto depth_view = View(depth, depth_format, vk::ImageAspectFlagBits::eDepth);
    const auto out_view = View(output, vk::Format::eB10G11R11UfloatPack32,
                               vk::ImageAspectFlagBits::eColor);

    scheduler.EndRendering();
    runtime.Transit(&depth, vk::ImageLayout::eGeneral, vk::PipelineStageFlagBits2::eComputeShader,
                    vk::AccessFlagBits2::eShaderRead);
    runtime.Transit(&color, vk::ImageLayout::eGeneral, vk::PipelineStageFlagBits2::eComputeShader,
                    vk::AccessFlagBits2::eShaderRead);
    runtime.Transit(&output, vk::ImageLayout::eGeneral, vk::PipelineStageFlagBits2::eComputeShader,
                    vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite);
    runtime.FlushBarriers();
    const auto cmdbuf = scheduler.CommandBuffer();

    const auto all = vk::PipelineStageFlagBits2::eAllCommands;
    const auto rw = vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite;
    const auto barrier = [&](vk::Image image, vk::ImageLayout old_layout,
                             vk::PipelineStageFlags2 src_stage, vk::AccessFlags2 src_access,
                             vk::PipelineStageFlags2 dst_stage, vk::AccessFlags2 dst_access) {
        const vk::ImageMemoryBarrier2 b{
            .srcStageMask = src_stage,
            .srcAccessMask = src_access,
            .dstStageMask = dst_stage,
            .dstAccessMask = dst_access,
            .oldLayout = old_layout,
            .newLayout = vk::ImageLayout::eGeneral,
            .image = image,
            .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
        };
        cmdbuf.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &b});
    };
    barrier(vk::Image(motion_image), vk::ImageLayout::eUndefined, all, vk::AccessFlagBits2::eNone,
            vk::PipelineStageFlagBits2::eComputeShader, vk::AccessFlagBits2::eShaderWrite);
    barrier(vk::Image(result_image), vk::ImageLayout::eUndefined, all, vk::AccessFlagBits2::eNone,
            all, rw);

    // Camera motion in render pixels (previous minus current).
    // No stencil here: the anti-aliasing keeps the camera's vectors for every pixel, as before.
    motion.Record(cmdbuf, depth_view, depth_view, *motion_view, w, h, w, h, false);
    barrier(vk::Image(motion_image), vk::ImageLayout::eGeneral,
            vk::PipelineStageFlagBits2::eComputeShader, vk::AccessFlagBits2::eShaderWrite, all,
            vk::AccessFlagBits2::eShaderRead);

    const auto now = std::chrono::steady_clock::now();
    float frame_ms = std::chrono::duration<float, std::milli>(now - last_frame).count();
    if (!(frame_ms > 0.0f && frame_ms < 200.0f)) {
        frame_ms = 16.6f;
    }
    last_frame = now;

    bool dispatched = false;
    const auto& settings = BbSettings::Get();
    const float sharpness = settings.sharpen ? std::min(settings.sharpness.load(), 1.0f) : 0.0f;
    if (dlss) {
        const Dlss::FeatureDesc desc{w, h, ow, oh, upscale ? Dlss::QualityForScale(factor) : 0, true,
                                     true};
        bool ok = true;
        if (!dlss->HasFeature(desc)) {
            scheduler.WaitSubmitted();
            dlss->ReleaseFeature();
            ok = dlss->CreateFeature(cmdbuf, desc);
            reset = true;
        }
        if (ok) {
            ok = dlss->Evaluate(
                cmdbuf,
                {.color = {color.GetImage(), color_view, color_format,
                           vk::ImageAspectFlagBits::eColor, w, h},
                 .depth = {depth.GetImage(), depth_view, depth_format,
                           vk::ImageAspectFlagBits::eDepth, w, h},
                 .motion = {vk::Image(motion_image), *motion_view, vk::Format::eR16G16Sfloat,
                            vk::ImageAspectFlagBits::eColor, w, h},
                 .output = {vk::Image(result_image), *result_view, vk::Format::eR16G16B16A16Sfloat,
                            vk::ImageAspectFlagBits::eColor, ow, oh},
                 .jitter_x = jitter[0],
                 .jitter_y = jitter[1],
                 .reset = reset,
                 .frame_ms = frame_ms,
                 .sharpness = sharpness});
        }
        if (!ok) {
            std::puts("Anti-aliasing: DLSS failed; using FSR 3.1");
            BbSettings::Get().aa_mode = BbSettings::AaFsr3;
            reset = true;
            return;
        }
        dispatched = true;
    } else {
        FfxVkPortableUpscaleDispatchInfo info{};
        info.structSize = sizeof(info);
        info.commandBuffer = cmdbuf;
        info.color = Describe(color.GetImage(), color_format, w, h, color.usage_flags,
                              vk::ImageAspectFlagBits::eColor,
                              FFX_VK_PORTABLE_RESOURCE_STATE_GENERIC_READ);
        info.depth = Describe(depth.GetImage(), depth_format, w, h, depth.usage_flags,
                              vk::ImageAspectFlagBits::eDepth,
                              FFX_VK_PORTABLE_RESOURCE_STATE_GENERIC_READ);
        info.motionVectors =
            Describe(vk::Image(motion_image), vk::Format::eR16G16Sfloat, w, h,
                     vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled,
                     vk::ImageAspectFlagBits::eColor, FFX_VK_PORTABLE_RESOURCE_STATE_GENERIC_READ);
        info.output = Describe(vk::Image(result_image), vk::Format::eR16G16B16A16Sfloat, ow, oh,
                               vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled |
                                   vk::ImageUsageFlagBits::eTransferSrc,
                               vk::ImageAspectFlagBits::eColor,
                               FFX_VK_PORTABLE_RESOURCE_STATE_UNORDERED_ACCESS);
        info.exposure.structSize = sizeof(info.exposure);
        info.reactiveMask.structSize = sizeof(info.reactiveMask);
        info.transparencyAndCompositionMask.structSize =
            sizeof(info.transparencyAndCompositionMask);
        info.jitterOffset = {jitter[0], jitter[1]};
        info.motionVectorScale = {1.0f, 1.0f};
        info.renderSize = {w, h};
        info.outputSize = {ow, oh};
        info.frameTimeMilliseconds = frame_ms;
        info.preExposure = 1.0f;
        info.cameraNear = motion.Near();
        info.cameraFar = 3000.0f;
        info.cameraVerticalFovRadians = motion.VerticalFov();
        info.viewSpaceToMeters = 1.0f;
        info.sharpness = sharpness;
        info.enableSharpening = settings.sharpen ? VK_TRUE : VK_FALSE;
        info.reset = reset ? VK_TRUE : VK_FALSE;
        info.frameId = frame_id++;
        FfxVkPortableUpscaleCreateInfo create_info{};
        create_info.structSize = sizeof(create_info);
        create_info.flags = FFX_VK_PORTABLE_CONTEXT_HDR_COLOR_INPUT |
                            FFX_VK_PORTABLE_CONTEXT_AUTO_EXPOSURE |
                            FFX_VK_PORTABLE_CONTEXT_DEPTH_INVERTED |
                            FFX_VK_PORTABLE_CONTEXT_DEPTH_INFINITE;
        create_info.maxRenderSize = {w, h};
        create_info.maxOutputSize = {ow, oh};
        if (const u64 issues = ffxVkPortableValidateUpscaleDispatchInfo(&create_info, &info)) {
            PrintIssues("dispatch", issues);
            failed = true;
            return;
        }
        if (ffxVkPortableUpscaleContextRecordDispatch(context, &info) != FFX_VK_PORTABLE_OK) {
            std::puts("Anti-aliasing: FSR 3 dispatch failed");
            failed = true;
            return;
        }
        dispatched = true;
    }
    if (!dispatched) {
        return;
    }
    reset = false;
    last_run = std::chrono::steady_clock::now();
    BbSettings::Get().aa_running = true;

    if (upscale) {
        // The game's own picture stays for what reads it by pixel; everything that samples it
        // by UV from here on gets the upscaled one.
        const vk::MemoryBarrier2 ready_to_read{
            .srcStageMask = all,
            .srcAccessMask = vk::AccessFlagBits2::eShaderWrite,
            .dstStageMask = all,
            .dstAccessMask = vk::AccessFlagBits2::eShaderRead,
        };
        cmdbuf.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &ready_to_read});
        redirect_id = output_id;
        redirect_valid = true;
        return;
    }
    // The reconstructed picture replaces what the game's pass wrote.
    barrier(vk::Image(result_image), vk::ImageLayout::eGeneral, all, rw,
            vk::PipelineStageFlagBits2::eComputeShader, vk::AccessFlagBits2::eShaderRead);
    const vk::DescriptorImageInfo src_info{.imageView = *result_view,
                                           .imageLayout = vk::ImageLayout::eGeneral};
    const vk::DescriptorImageInfo dst_info{.imageView = out_view,
                                           .imageLayout = vk::ImageLayout::eGeneral};
    const std::array<vk::WriteDescriptorSet, 2> writes = {{
        {.dstBinding = 0,
         .descriptorCount = 1,
         .descriptorType = vk::DescriptorType::eStorageImage,
         .pImageInfo = &src_info},
        {.dstBinding = 1,
         .descriptorCount = 1,
         .descriptorType = vk::DescriptorType::eStorageImage,
         .pImageInfo = &dst_info},
    }};
    const std::array<float, 2> size{float(w), float(h)};
    cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, *copy_pipeline);
    cmdbuf.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, *copy_pipeline_layout, 0, writes);
    cmdbuf.pushConstants(*copy_pipeline_layout, vk::ShaderStageFlagBits::eCompute, 0, sizeof(size),
                         size.data());
    cmdbuf.dispatch((w + 7) / 8, (h + 7) / 8, 1);
    const vk::MemoryBarrier2 done{
        .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
        .srcAccessMask = vk::AccessFlagBits2::eShaderWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .dstAccessMask = vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite,
    };
    cmdbuf.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &done});
}

} // namespace Vulkan
