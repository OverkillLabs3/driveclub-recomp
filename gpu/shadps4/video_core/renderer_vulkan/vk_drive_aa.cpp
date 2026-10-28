// SPDX-License-Identifier: GPL-2.0-or-later
#include "video_core/renderer_vulkan/vk_drive_aa.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <vk_mem_alloc.h>

#include "bbport_frame_info.h"
#include "bbport_diagnostics.h"
#include "bbport_settings.h"
#include "ffx_vk_portable.h"
#include "video_core/host_shaders/drive_aa_copy_comp.h"
#include "video_core/host_shaders/drive_aa_sharpen_comp.h"
#include "video_core/renderer_vulkan/vk_dlss.h"
#include "video_core/renderer_vulkan/fsr411/fsr411.h"
#include "video_core/renderer_vulkan/vk_fsr315.h"
#include "video_core/renderer_vulkan/vk_frame_capture.h"
#include "bbport_neural_settings.h"
#include "video_core/renderer_vulkan/vk_drive_motion.h"
#include "video_core/renderer_vulkan/vk_camera_motion.h"
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

struct CaptureImage {
    vk::Image image;
    u32 width, height, bytes_per_pixel;
    const char* name;
    const char* format;
    vk::ImageAspectFlagBits aspect = vk::ImageAspectFlagBits::eColor;
};

void DumpDlssCapture(const Instance& instance, Scheduler& scheduler, vk::CommandBuffer cmdbuf,
                     const std::string& directory, int frame,
                     const std::vector<CaptureImage>& images) {
    const vk::MemoryBarrier2 before{
        .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .srcAccessMask = vk::AccessFlagBits2::eMemoryWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eTransfer,
        .dstAccessMask = vk::AccessFlagBits2::eTransferRead,
    };
    cmdbuf.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &before});
    for (const auto& image : images) {
        const VkDeviceSize size = VkDeviceSize(image.width) * image.height * image.bytes_per_pixel;
        const VkBufferCreateInfo buffer_ci{
            .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .size = size,
            .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        };
        const VmaAllocationCreateInfo alloc_ci{
            .flags = VMA_ALLOCATION_CREATE_MAPPED_BIT |
                     VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT,
            .usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
        };
        VkBuffer buffer{};
        VmaAllocation allocation{};
        VmaAllocationInfo info{};
        if (vmaCreateBuffer(instance.GetAllocator(), &buffer_ci, &alloc_ci, &buffer, &allocation,
                            &info) != VK_SUCCESS) {
            std::printf("DLSS capture: no host memory for %s\n", image.name);
            continue;
        }
        const vk::BufferImageCopy region{
            .imageSubresource = {image.aspect, 0, 0, 1},
            .imageExtent = {image.width, image.height, 1},
        };
        cmdbuf.copyImageToBuffer(image.image, vk::ImageLayout::eGeneral, buffer, region);
        char path[1024];
        std::snprintf(path, sizeof(path), "%s/f%02d_%s_%ux%u_%s.raw", directory.c_str(), frame,
                      image.name, image.width, image.height, image.format);
        scheduler.DeferPriorityOperation(
            [allocator = instance.GetAllocator(), buffer, allocation, info, size,
             file = std::string{path}] {
                vmaInvalidateAllocation(allocator, allocation, 0, VK_WHOLE_SIZE);
                if (FILE* f = std::fopen(file.c_str(), "wb")) {
                    std::fwrite(info.pMappedData, 1, size, f);
                    std::fclose(f);
                }
                vmaDestroyBuffer(allocator, buffer, allocation);
            });
    }
    const vk::MemoryBarrier2 after{
        .srcStageMask = vk::PipelineStageFlagBits2::eTransfer,
        .srcAccessMask = vk::AccessFlagBits2::eNone,
        .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .dstAccessMask = vk::AccessFlagBits2::eNone,
    };
    cmdbuf.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &after});
}

int DlssCaptureFrame(bool dlss, const char* user_dir, std::string& directory) {
    static int remaining = 0;
    static int frame = 0;
    static int poll = 0;
    if (!dlss || !user_dir || !*user_dir) {
        return -1;
    }
    if (remaining == 0 && ++poll % 120 == 0) {
        const std::string trigger = std::string{user_dir} + "/dlss_capture.trigger";
        std::ifstream request(trigger);
        if (request) {
            std::getline(request, directory);
            request.close();
            std::remove(trigger.c_str());
            if (!directory.empty()) {
                std::error_code ec;
                std::filesystem::create_directories(directory, ec);
                remaining = 8;
                frame = 0;
                std::ofstream manifest(std::filesystem::path(directory) / "capture.txt");
                manifest << "Driveclub DLSS input capture\n"
                         << "8 consecutive game frames; lossless raw GPU inputs.\n"
                         << "color: B10G11R11_UFLOAT_PACK32 (4 bytes/pixel)\n"
                         << "depth: D32_SFLOAT depth aspect (4 bytes/pixel)\n"
                         << "motion: R16G16_SFLOAT, pixels, previous minus current (4 bytes/pixel)\n"
                         << "object: first frame only, RGBA32_SFLOAT; rg=object motion in pixels, "
                            "b=valid, a=depth (16 bytes/pixel)\n"
                         << "Inputs are captured before DLSS evaluation.\n";
                std::printf("DLSS capture: armed for 8 frames -> %s\n", directory.c_str());
            }
        }
    }
    if (remaining == 0) {
        return -1;
    }
    --remaining;
    return frame++;
}

} // namespace

DriveAA::DriveAA(const Instance& instance_, Scheduler& scheduler_,
                 VideoCore::TextureCache& texture_cache_, Runtime& runtime_, DriveMotion& motion_,
                 CameraMotion& camera_motion_)
    : instance{instance_}, scheduler{scheduler_}, texture_cache{texture_cache_},
      runtime{runtime_}, motion{motion_}, camera_motion{camera_motion_},
      neural{std::make_unique<NeuralRendering>(instance_, scheduler_)} {}

DriveAA::~DriveAA() = default;

int DriveAA::EffectiveMode() const {
    const int requested = BbSettings::Get().aa_mode;
    if (requested == BbSettings::AaFsr411 && !instance.IsFsr411PortableSupported()) {
        return BbSettings::AaFsr3;
    }
    if (requested != BbSettings::AaDlss) {
        return requested;
    }
    // The user's choice stays DLSS; this session runs FSR 3.1 when NGX cannot start.
    const Dlss* dlss = Dlss::Get();
    return !dlss_failed && dlss && dlss->Available() ? requested : BbSettings::AaFsr3;
}

bool DriveAA::Active() const {
    return !failed && EffectiveMode() != BbSettings::AaOff;
}

// The render size is the output size over the scale (src/probe.c sets it at start).
bool DriveAA::Upscaling() const {
    return Active() && BbSettings::Get().aa_scale != BbSettings::AaScaleNative;
}

// The scale measured on the last run (render to output), else the chosen one.
float DriveAA::MipBias() const {
    if (!Upscaling()) {
        return 0.0f;
    }
    return -std::log2(scale > 1.0f ? scale : BbSettings::AaScaleFactor(BbSettings::Get().aa_scale));
}

void DriveAA::NextFrame() {
    neural->BeginFrame();
    const int mode = EffectiveMode();
    BbSettings::Get().aa_effective = mode;
    if (mode != applied_mode) {
        if (BbSettings::Get().aa_mode == BbSettings::AaFsr411 && mode == BbSettings::AaFsr3) {
            std::puts("Anti-aliasing: FSR 4.1.1 is unsupported on this GPU; using FSR 3.1.5");
        }
        applied_mode = mode;
        reset = true;
        failed = false;
        BbSettings::Get().aa_failed = false;
        jitter_index = 0;
    }
    // Called for every draw into the display buffer: "running" means it ran lately.
    BbSettings::Get().aa_running =
        std::chrono::steady_clock::now() - last_run < std::chrono::milliseconds(300);
    if (!Active()) {
        jitter = {};
        motion.SetJitter({}, {});
        return;
    }
    // 8 phases per 1:1 (16 for the native anti-aliasing), more as the scale grows.
    const float factor = Upscaling() && scale > 1.0f ? scale : 1.0f;
    const u32 phases = factor > 1.0f ? u32(std::ceil(8.0f * factor * factor)) : 16u;
    // The upscaled picture stands in for the game's during the one draw that scales it to the
    // output (OnComposite runs after this, for that draw).
    redirect_valid = false;
    jitter_index = jitter_index % phases + 1;
    jitter = {Halton(jitter_index, 2) - 0.5f, Halton(jitter_index, 3) - 0.5f};
    // The game's lighting and shadows rebuild positions from depth with the unshifted camera (it
    // does not jitter its own), so a shift of the geometry moves every lookup by as much: shadows
    // and the road mirage flickered when upscaling, where a render pixel covers several output
    // pixels. A shift of half an output pixel (render / output of a render pixel) errs no more than
    // at the render size, and still reaches the output pixels' centres. BB_AA_JITTER=<scale>: this
    // scale instead (0: none).
    static const float forced_scale = [] {
        const char* env = std::getenv("BB_AA_JITTER");
        return env && *env ? float(std::atof(env)) : -1.0f;
    }();
    const float jitter_scale = forced_scale >= 0.0f ? forced_scale : 1.0f / factor;
    jitter = {jitter[0] * jitter_scale, jitter[1] * jitter_scale};
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

void DriveAA::Redirect(const vk::ComponentMapping& mapping, vk::ImageView& view,
                       vk::ImageLayout& layout) {
    layout = vk::ImageLayout::eGeneral;
    if (mapping == vk::ComponentMapping{}) {
        view = *result_view;
        return;
    }
    for (const auto& [key, cached] : swizzled_views) {
        if (cached && key == mapping) {
            view = *cached;
            return;
        }
    }
    // Few swizzles read it (the game's passes agree on one); the oldest slot is reused.
    static u32 next = 0;
    auto& slot = swizzled_views[next++ % swizzled_views.size()];
    if (slot.second) {
        scheduler.DeferOperation([old = std::make_shared<vk::UniqueImageView>(std::move(slot.second))] {});
    }
    slot.first = mapping;
    slot.second = Check(instance.GetDevice().createImageViewUnique({
        .image = vk::Image(result_image),
        .viewType = vk::ImageViewType::e2D,
        .format = vk::Format::eR16G16B16A16Sfloat,
        .components = mapping,
        .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
    }));
    view = *slot.second;
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
    // Both take two storage images and up to two floats: the copy its size, the sharpening its
    // strength.
    const auto compute = [&](const auto& code) {
        const auto module = CompileSPV(code, device);
        auto pipeline = Check(device.createComputePipelineUnique(
            {}, vk::ComputePipelineCreateInfo{
                    .stage = {.stage = vk::ShaderStageFlagBits::eCompute,
                              .module = module,
                              .pName = "main"},
                    .layout = *copy_pipeline_layout,
                }));
        device.destroyShaderModule(module);
        return pipeline;
    };
    copy_pipeline = compute(DRIVE_AA_COPY_COMP);
    sharpen_pipeline = compute(DRIVE_AA_SHARPEN_COMP);

}

void DriveAA::EnsureMotionResources(u32 w, u32 h) {
    if (motion_image && motion_width == w && motion_height == h) return;
    scheduler.Finish();
    motion_view.reset();
    motion_image = VideoCore::UniqueImage(instance.GetDevice(), instance.GetAllocator());
    motion_image.Create(vk::ImageCreateInfo{
        .imageType = vk::ImageType::e2D,
        .format = vk::Format::eR16G16Sfloat,
        .extent = {w, h, 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = vk::SampleCountFlagBits::e1,
        .tiling = vk::ImageTiling::eOptimal,
        .usage = vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled |
                 vk::ImageUsageFlagBits::eTransferSrc,
        .initialLayout = vk::ImageLayout::eUndefined,
    });
    motion_view = Check(instance.GetDevice().createImageViewUnique({
        .image = vk::Image(motion_image),
        .viewType = vk::ImageViewType::e2D,
        .format = vk::Format::eR16G16Sfloat,
        .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
    }));
    motion_width = w;
    motion_height = h;
}

void DriveAA::RecordGuides(VideoCore::ImageId depth_id, std::array<float, 2> frame_jitter,
                           vk::ImageView color_view) {
    frame_guides = {};
    frame_depth = {};
    frame_depth_uid = 0;
    if (!motion.Ready()) return;
    auto& depth = texture_cache.GetImage(depth_id);
    const u32 w = depth.info.size.width, h = depth.info.size.height;
    EnsureMotionResources(w, h);
    scheduler.EndRendering();
    runtime.Transit(&depth, vk::ImageLayout::eGeneral, vk::PipelineStageFlagBits2::eComputeShader,
                    vk::AccessFlagBits2::eShaderRead);
    runtime.FlushBarriers();
    const auto cmdbuf = scheduler.CommandBuffer();
    const auto depth_format = depth.info.pixel_format;
    const auto depth_view = View(depth, depth_format, vk::ImageAspectFlagBits::eDepth);
    const auto stencil_view = depth_format == vk::Format::eD32SfloatS8Uint
        ? View(depth, depth_format, vk::ImageAspectFlagBits::eStencil) : depth_view;
    vk::ImageMemoryBarrier2 barrier{
        .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .srcAccessMask = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
        .dstAccessMask = vk::AccessFlagBits2::eShaderWrite,
        .oldLayout = vk::ImageLayout::eUndefined,
        .newLayout = vk::ImageLayout::eGeneral,
        .image = vk::Image(motion_image),
        .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
    };
    cmdbuf.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &barrier});
    motion.SetJitter(frame_jitter, previous_jitter);
    motion.Record(cmdbuf, depth_view, stencil_view, *motion_view, w, h, w, h, false, {}, color_view);
    barrier.srcStageMask = vk::PipelineStageFlagBits2::eComputeShader;
    barrier.srcAccessMask = vk::AccessFlagBits2::eShaderWrite;
    barrier.dstStageMask = vk::PipelineStageFlagBits2::eComputeShader;
    barrier.dstAccessMask = vk::AccessFlagBits2::eShaderRead;
    barrier.oldLayout = vk::ImageLayout::eGeneral;
    cmdbuf.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &barrier});
    frame_guides = {*motion_view, depth_view, w, h};
    frame_depth = depth_id;
    frame_depth_uid = depth.image_uid;
}

NeuralRendering::Guides DriveAA::FrameGuides() {
    if (frame_guides.depth && BbNeural::Snapshot().enabled) {
        auto* depth = texture_cache.TryGetImage(frame_depth, frame_depth_uid);
        if (!depth) return {};
        // Guest passes can change the scene depth layout after motion capture.
        runtime.Transit(depth, vk::ImageLayout::eGeneral,
                        vk::PipelineStageFlagBits2::eComputeShader, vk::AccessFlagBits2::eShaderRead);
        runtime.FlushBarriers();
        frame_guides.depth = View(*depth, depth->info.pixel_format, vk::ImageAspectFlagBits::eDepth);
    }
    return frame_guides;
}

bool DriveAA::EnsureResources(u32 w, u32 h, u32 ow, u32 oh, bool dlss, bool fsr411_mode) {
    EnsureMotionResources(w, h);
    const bool provider_ok =
        dlss ? (!fsr315 && !fsr411)
             : (fsr411_mode ? fsr411 != nullptr : (fsr315 && fsr315->Ready()));
    if (ready && w == width && h == height && ow == out_width && oh == out_height && provider_ok) {
        return true;
    }
    scheduler.Finish();
    ready = false;
    redirect_valid = false;
    for (auto& slot : swizzled_views) {
        slot.second.reset();
    }
    fsr315.reset();
    fsr411.reset();
    fsr411_ticks.clear();
    fsr411_described.clear();
    width = w;
    height = h;
    out_width = ow;
    out_height = oh;
    if (!dlss) {
        if (fsr411_mode) {
            if (!instance.IsFsr411PortableSupported()) {
                std::puts("Anti-aliasing: FSR 4.1.1 needs shaderInt8, shaderInt16 and "
                          "shaderIntegerDotProduct; not available on this GPU");
                return false;
            }
            // The game runs with the runtime folder as its working directory; the asset set may
            // also sit in the install root next to it (or wherever BB_FSR411_DIR points).
            const auto has_assets = [](const std::filesystem::path& dir) {
                return std::filesystem::exists(dir / "t1080_m0/initializer.bin") ||
                       std::filesystem::exists(dir / "t2160_m0/initializer.bin");
            };
            std::string dir;
            if (const char* env = std::getenv("BB_FSR411_DIR"); env && env[0]) {
                dir = env;
            } else {
                for (const char* candidate : {"fsr4_411", "../fsr4_411"}) {
                    if (has_assets(candidate)) {
                        dir = candidate;
                        break;
                    }
                }
                if (dir.empty()) {
                    dir = "fsr4_411";
                }
            }
            if (!has_assets(dir)) {
                std::printf("Anti-aliasing: FSR 4.1.1 assets missing in %s (build fsr4_411 from "
                            "a FSR 4.1.1 DLL with tools/fsr4cap)\n",
                            dir.c_str());
                return false;
            }
            fsr411 = std::make_unique<Fsr411::Upscaler>(
                VkPhysicalDevice(instance.GetPhysicalDevice()), VkDevice(instance.GetDevice()), dir);
            std::printf("FSR 4.1.1: replay from %s\n", dir.c_str());
        } else {
            fsr315 = std::make_unique<Fsr315>(instance, scheduler);
            if (!fsr315->Create(w, h, ow, oh)) {
                const std::string problem =
                    fsr315->Problem() ? fsr315->Problem() : "unknown reason";
                std::printf("Anti-aliasing: FSR 3.1.5 creation failed (%s)\n", problem.c_str());
                fsr315.reset();
                return false;
            }
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
    make(result_image, result_view, vk::Format::eR16G16B16A16Sfloat,
         vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled |
             vk::ImageUsageFlagBits::eTransferSrc | vk::ImageUsageFlagBits::eTransferDst,
         ow, oh);
    if (dlss) {
        make(raw_image, raw_view, vk::Format::eR16G16B16A16Sfloat,
             vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled |
                 vk::ImageUsageFlagBits::eTransferSrc | vk::ImageUsageFlagBits::eTransferDst, ow, oh);
    }
    CreatePipeline();
    ready = true;
    reset = true;
    if (ow == w && oh == h) {
        std::printf("Anti-aliasing: %s %ux%u", dlss ? "DLAA" : fsr411_mode ? "FSR 4.1.1 native" : "FSR 3.1.5 native", w, h);
    } else {
        std::printf("Upscaling: %s %ux%u to %ux%u", dlss ? "DLSS" : fsr411_mode ? "FSR 4.1.1" : "FSR 3.1.5", w, h, ow, oh);
    }
    std::puts("");
    return true;
}

void DriveAA::OnTaa(VideoCore::ImageId color_id, VideoCore::ImageId depth_id,
                    VideoCore::ImageId output_id, VideoCore::ImageId gbuffer_id,
                    VideoCore::ImageId native_depth_id, VideoCore::ImageId previous_ids_id) {
    applied_jitter = {};
    if (color_id) {
        const auto& size = texture_cache.GetImage(color_id).info.size;
        applied_jitter = JitterForExtent(size.width, size.height);
    }
    motion.SetJitter(applied_jitter, previous_jitter);
    if ((Active() || BbSettings::Get().frame_gen != BbSettings::FrameGenOff ||
         BbNeural::Snapshot().enabled) &&
        gbuffer_id && native_depth_id && color_id) {
        auto& gbuffer = texture_cache.GetImage(gbuffer_id);
        auto& native_depth = texture_cache.GetImage(native_depth_id);
        const auto& color = texture_cache.GetImage(color_id).info;
        const u32 w = color.size.width, h = color.size.height;
        if (gbuffer.info.size.width == w && gbuffer.info.size.height == h &&
            native_depth.info.size.width == w && native_depth.info.size.height == h &&
            (!previous_ids_id || (texture_cache.GetImage(previous_ids_id).info.size.width == w &&
                                  texture_cache.GetImage(previous_ids_id).info.size.height == h))) {
            scheduler.EndRendering();
            runtime.Transit(&gbuffer, vk::ImageLayout::eGeneral,
                            vk::PipelineStageFlagBits2::eComputeShader, vk::AccessFlagBits2::eShaderRead);
            runtime.Transit(&native_depth, vk::ImageLayout::eGeneral,
                            vk::PipelineStageFlagBits2::eComputeShader, vk::AccessFlagBits2::eShaderRead);
            vk::ImageView previous_ids{};
            if (previous_ids_id) {
                auto& ids = texture_cache.GetImage(previous_ids_id);
                runtime.Transit(&ids, vk::ImageLayout::eGeneral,
                                vk::PipelineStageFlagBits2::eComputeShader, vk::AccessFlagBits2::eShaderRead);
                previous_ids = View(ids, ids.info.pixel_format, vk::ImageAspectFlagBits::eColor);
            }
            runtime.FlushBarriers();
            motion.CaptureNative(View(gbuffer, gbuffer.info.pixel_format, vk::ImageAspectFlagBits::eColor),
                                 View(native_depth, native_depth.info.pixel_format, vk::ImageAspectFlagBits::eColor),
                                 previous_ids, w, h);
        }
    }
    static int reported = 0;
    const auto skip = [&](const char* why) {
        reset = true;
        if (reported++ < 6) {
            std::printf("Anti-aliasing: skipped, %s", why);
            std::puts("");
        }
    };
    late_pending = false;
    redirect_valid = false;
    if (!Active() && BbNeural::Snapshot().enabled && depth_id && output_id) {
        RecordGuides(depth_id, applied_jitter);
        return;
    }
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
    const u32 display_w = BbFrame::output_w.load(std::memory_order_relaxed);
    const u32 display_h = BbFrame::output_h.load(std::memory_order_relaxed);
    if (!Upscaling() || display_w <= w || display_h <= h) {
        // At the render size: the reconstructed picture replaces the pass's result.
        Reconstruct(color_id, depth_id, output_id, applied_jitter, false);
        return;
    }
    // Upscaling: the passes after this one (transparents, bloom, motion blur and depth of field)
    // draw at the render size, and the picture only reaches the output size when the game scales
    // it into its display buffer. So the upscale waits for that pass (OnComposite), and the
    // game's own anti-aliasing is skipped here: its result is the lit picture as it is, still
    // aliased and jittered, as FSR and DLSS want their input.
    scheduler.EndRendering();
    runtime.Transit(&color, vk::ImageLayout::eGeneral, vk::PipelineStageFlagBits2::eTransfer,
                    vk::AccessFlagBits2::eTransferRead);
    runtime.Transit(&output, vk::ImageLayout::eGeneral, vk::PipelineStageFlagBits2::eTransfer,
                    vk::AccessFlagBits2::eTransferWrite);
    runtime.FlushBarriers();
    const auto cmdbuf = scheduler.CommandBuffer();
    const vk::ImageCopy region{
        .srcSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
        .dstSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
        .extent = {w, h, 1},
    };
    cmdbuf.copyImage(color.GetImage(), vk::ImageLayout::eGeneral, output.GetImage(),
                     vk::ImageLayout::eGeneral, region);
    const vk::MemoryBarrier2 copied{
        .srcStageMask = vk::PipelineStageFlagBits2::eTransfer,
        .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .dstAccessMask = vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite,
    };
    cmdbuf.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &copied});
    late_pending = true;
    late_depth = depth_id;
    late_jitter = applied_jitter;  // the jitter the scene was drawn with (NextFrame moves on before the composite)
    late_width = w;
    late_height = h;
}

bool DriveAA::WantsComposite(const VideoCore::Image& image) const {
    return late_pending && image.info.size.width == late_width &&
           image.info.size.height == late_height &&
           image.info.pixel_format == vk::Format::eB10G11R11UfloatPack32;
}

void DriveAA::OnComposite(VideoCore::ImageId color_id) {
    late_pending = false;
    static int reported = 0;
    if (reported++ < 2) {
        const auto& color = texture_cache.GetImage(color_id);
        std::printf("Upscaling: the game's picture %#llx (%ux%u) upscaled where it is scaled to the output\n",
                    static_cast<unsigned long long>(color.info.guest_address), color.info.size.width,
                    color.info.size.height);
    }
    Reconstruct(color_id, late_depth, {}, late_jitter, true);
}

void DriveAA::Reconstruct(VideoCore::ImageId color_id, VideoCore::ImageId depth_id,
                          VideoCore::ImageId output_id, std::array<float, 2> frame_jitter,
                          bool upscale) {
    // The draw attached this frame's object vectors before the display composite. Preserve
    // them through capture and temporal evaluation, then advance history for the next scene.
    auto& color = texture_cache.GetImage(color_id);
    auto& depth = texture_cache.GetImage(depth_id);
    const u32 w = color.info.size.width, h = color.info.size.height;
    if (depth.info.size.width != w || depth.info.size.height != h) {
        reset = true;
        return;
    }
    BbFrame::NoteScene(w, h);  // the overlay's Render size, also when it equals the output size
    const int mode = EffectiveMode();
    const bool want_dlss = mode == BbSettings::AaDlss;
    const bool want_fsr411 = mode == BbSettings::AaFsr411;
    Dlss* dlss = want_dlss ? Dlss::Get() : nullptr;
    if (want_dlss && (!dlss || !dlss->Available())) {
        std::puts("Anti-aliasing: DLSS is not available; using FSR 3.1");
        dlss_failed = true;
        reset = true;
        return;
    }
    // Upscaled to the game's output size (its display buffer).
    const u32 ow = upscale ? BbFrame::output_w.load(std::memory_order_relaxed) : w;
    const u32 oh = upscale ? BbFrame::output_h.load(std::memory_order_relaxed) : h;
    const float factor = float(ow) / float(w);
    scale = factor;
    if (!EnsureResources(w, h, ow, oh, want_dlss, want_fsr411)) {
        failed = true;
        BbSettings::Get().aa_failed = true;
        return;
    }
    if (want_fsr411) {
        while (fsr411_ticks.size() >= Fsr411::kFramesInFlight) {
            scheduler.Wait(fsr411_ticks.front());
            fsr411_ticks.pop_front();
        }
    }
    const auto color_format = color.info.pixel_format;
    const auto depth_format = depth.info.pixel_format;
    const auto color_view = View(color, color_format, vk::ImageAspectFlagBits::eColor);
    const auto depth_view = View(depth, depth_format, vk::ImageAspectFlagBits::eDepth);

    // A new size or model makes a new DLSS feature. The old one's images and buffers may still be
    // used by recorded or submitted work (this command buffer can already hold an evaluation), so
    // all of it finishes on the GPU before they are released; then this command buffer starts
    // afresh. WaitSubmitted released them while the GPU still wrote to them: a device loss
    // (WriteInvalid) when leaving a race, where the sizes change.
    const Dlss::FeatureDesc dlss_desc{w, h, ow, oh, upscale ? Dlss::QualityForScale(factor) : 0, true, true,
                                      u32(BbSettings::Get().dlss_model.load())};
    const bool new_feature = dlss && !dlss->HasFeature(dlss_desc);
    if (new_feature) {
        scheduler.Finish();
        dlss->ReleaseFeature();
    }

    scheduler.EndRendering();
    runtime.Transit(&depth, vk::ImageLayout::eGeneral, vk::PipelineStageFlagBits2::eComputeShader,
                    vk::AccessFlagBits2::eShaderRead);
    runtime.Transit(&color, vk::ImageLayout::eGeneral, vk::PipelineStageFlagBits2::eComputeShader,
                    vk::AccessFlagBits2::eShaderRead);
    VideoCore::Image* output = output_id ? &texture_cache.GetImage(output_id) : nullptr;
    if (output) {
        runtime.Transit(output, vk::ImageLayout::eGeneral, vk::PipelineStageFlagBits2::eComputeShader,
                        vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite);
    }
    runtime.FlushBarriers();
    const auto cmdbuf = scheduler.CommandBuffer();

    const auto all = vk::PipelineStageFlagBits2::eAllCommands;
    const auto rw = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite;
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
    barrier(vk::Image(result_image), vk::ImageLayout::eUndefined, all, vk::AccessFlagBits2::eNone,
            all, rw);

    RecordGuides(depth_id, frame_jitter, color_view);
    const int dump = FrameCapture::DumpFrame();
    if (dump >= 0) {
        FrameCapture::DumpImages(instance, scheduler, cmdbuf, dump,
            {{color.GetImage(), w, h, 4, "sr_color", "r11g11b10f"},
             {depth.GetImage(), w, h, 4, "sr_depth", "f32", vk::ImageAspectFlagBits::eDepth},
             {vk::Image(motion_image), w, h, 4, "sr_motion", "rg16f"}});
        motion.DumpInputs(cmdbuf, dump, w, h);
        std::printf("AA dump: %d mode %d %ux%u -> %ux%u jitter %.9g %.9g reset %d\n",
                    dump, mode, w, h, ow, oh, frame_jitter[0], frame_jitter[1], int(reset));
    }

    static std::string capture_directory;
    static bool object_map_dumped = false;
    const int capture_frame =
        DlssCaptureFrame(dlss != nullptr, BB_DIAGNOSTIC_ENV("BB_GPU_USER_DIR"), capture_directory);
    if (capture_frame == 0) {
        object_map_dumped = false;
    }
    if (capture_frame >= 0) {
        const bool camera_ready = motion.Ready();
        const vk::Image object_image = camera_motion.ObjectMotionImage(w, h);
        bool object_map_valid = false;
        if (object_image && !(dlss && camera_ready)) {
            camera_motion.PrepareObjectMotion(cmdbuf, w, h, object_map_valid);
        } else {
            object_map_valid = object_image && camera_ready;
        }
        const bool dump_object = object_image && !object_map_dumped;
        std::vector<CaptureImage> images{
            {color.GetImage(), w, h, 4, "color", "b10g11r11"},
            {depth.GetImage(), w, h, 4, "depth", "d32", vk::ImageAspectFlagBits::eDepth},
            {vk::Image(motion_image), w, h, 4, "motion", "rg16f"},
        };
        if (dump_object) {
            images.push_back({object_image, w, h, 16, "object_motion", "rgba32f"});
            object_map_dumped = true;
        }
        DumpDlssCapture(instance, scheduler, cmdbuf, capture_directory, capture_frame, images);
        std::ofstream frame_log(std::filesystem::path(capture_directory) /
                                ("f" + std::to_string(capture_frame) + "_meta.txt"));
        frame_log << "capture_frame=" << capture_frame << "\nrender=" << w << 'x' << h << "\noutput=" << ow
                  << 'x' << oh << "\njitter_x=" << frame_jitter[0]
                  << "\njitter_y=" << frame_jitter[1]
                  << "\nmotion_source=" << (object_map_valid ? "game_taa_native_object_fused" : "game_taa_native_camera")
                  << "\ncamera_ready=" << camera_ready
                  << "\nobject_map_available=" << bool(object_image)
                  << "\nobject_map_valid=" << object_map_valid
                  << "\nobject_map_in_this_file=" << dump_object
                  << "\nobject_motion_setting=" << bool(BbSettings::Get().object_motion)
                  << "\nupscale=" << upscale << "\n";
        std::printf("DLSS capture: frame %d saved (motion=%s, object-map=%s)\n", capture_frame,
                    dlss && camera_ready ? "scene/object" : "game-camera+object-fusion",
                    dump_object ? "yes" : "no");
    }

    // Optional neural rendering (off by default) before the upscale: the pass runs on the game's render pixels
    // and DLSS or FSR then carries its result to the output size.
    neural->Record(cmdbuf, BbNeural::PreUpscale,
                   {color.GetImage(), color_view, color_format, w, h, true, true},
                   {*motion_view, depth_view, w, h}, reset);

    const auto now = std::chrono::steady_clock::now();
    float frame_ms = std::chrono::duration<float, std::milli>(now - last_frame).count();
    if (!(frame_ms > 0.0f && frame_ms < 200.0f)) {
        frame_ms = 16.6f;
    }
    last_frame = now;

    bool dispatched = false;
    const u32 upscale_frame_id = frame_id++;
    const auto& settings = BbSettings::Get();
    const float sharpness = settings.sharpen ? std::min(settings.sharpness.load(), 1.0f) : 0.0f;
    // DLSS ignores its sharpness since 2.5.1: RCAS sharpens its result afterwards (FSR has its own).
    const bool sharpen_after = dlss && sharpness > 0.0f && raw_image;
    if (sharpen_after) {
        barrier(vk::Image(raw_image), vk::ImageLayout::eUndefined, all, vk::AccessFlagBits2::eNone,
                all, rw);
    }
    if (dlss) {
        bool ok = true;
        if (new_feature) {
            ok = dlss->CreateFeature(cmdbuf, dlss_desc);
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
                 .output = {sharpen_after ? vk::Image(raw_image) : vk::Image(result_image),
                            sharpen_after ? *raw_view : *result_view, vk::Format::eR16G16B16A16Sfloat,
                            vk::ImageAspectFlagBits::eColor, ow, oh},
                 .jitter_x = frame_jitter[0],
                 .jitter_y = frame_jitter[1],
                 .reset = reset,
                 .frame_ms = frame_ms,
                 .sharpness = sharpness});
        }
        if (!ok) {
            std::puts("Anti-aliasing: DLSS failed; using FSR 3.1");
            dlss_failed = true;
            reset = true;
            return;
        }
        dispatched = true;
    } else if (want_fsr411 && fsr411) {
        Fsr411::Frame frame{};
        frame.cmdbuf = VkCommandBuffer(cmdbuf);
        frame.color = {VkImage(color.GetImage()), VkImageView(color_view), w, h};
        frame.depth = {VkImage(depth.GetImage()), VkImageView(depth_view), w, h};
        frame.motion = {static_cast<VkImage>(vk::Image(motion_image)),
                        VkImageView(*motion_view), w, h};
        frame.output = {static_cast<VkImage>(vk::Image(result_image)),
                        VkImageView(*result_view), ow, oh};
        frame.render_width = w;
        frame.render_height = h;
        frame.ultra_performance = BbSettings::Get().aa_scale == BbSettings::AaScaleUltraPerformance;
        frame.jitter[0] = frame_jitter[0];
        frame.jitter[1] = frame_jitter[1];
        frame.motion_scale[0] = 1.0f;
        frame.motion_scale[1] = 1.0f;
        frame.pre_exposure = 1.0f;
        frame.sharpness = sharpness;
        frame.sharpen = settings.sharpen && sharpness > 0.0f;
        frame.reset = reset;
        frame.auto_exposure = true;
        if (!fsr411->Record(frame)) {
            std::printf("Anti-aliasing: FSR 4.1.1 failed (%s)\n", fsr411->Error().c_str());
            failed = true;
            BbSettings::Get().aa_failed = true;
            reset = true;
            return;
        }
        fsr411_ticks.push_back(scheduler.CurrentTick());
        if (const std::string description = fsr411->Describe(); description != fsr411_described) {
            fsr411_described = description;
            std::printf("FSR 4.1.1: %s\n", description.c_str());
        }
        dispatched = true;
    } else if (fsr315) {
        Fsr315::Frame frame{};
        frame.command = cmdbuf;
        frame.color = color.GetImage();
        frame.color_format = color_format;
        frame.depth = depth.GetImage();
        frame.depth_format = depth_format;
        frame.motion = vk::Image(motion_image);
        frame.output = vk::Image(result_image);
        frame.width = w;
        frame.height = h;
        frame.out_width = ow;
        frame.out_height = oh;
        frame.jitter_x = frame_jitter[0];
        frame.jitter_y = frame_jitter[1];
        frame.frame_ms = frame_ms;
        frame.sharpness = sharpness;
        frame.sharpen = settings.sharpen;
        frame.reset = reset;
        frame.near_plane = motion.Near();
        frame.far_plane = motion.Far();
        frame.vertical_fov = motion.VerticalFov();
        if (!fsr315->Record(frame)) {
            const std::string problem = fsr315->Problem() ? fsr315->Problem() : "unknown reason";
            std::printf("Anti-aliasing: FSR 3.1.5 failed (%s)\n", problem.c_str());
            failed = true;
            BbSettings::Get().aa_failed = true;
            reset = true;
            return;
        }
        dispatched = true;
    }
    if (!dispatched) {
        return;
    }
    const bool was_reset = reset;
    reset = false;
    last_run = std::chrono::steady_clock::now();
    BbSettings::Get().aa_running = true;

    if (sharpen_after) {
        barrier(vk::Image(raw_image), vk::ImageLayout::eGeneral, all, rw,
                vk::PipelineStageFlagBits2::eComputeShader, vk::AccessFlagBits2::eShaderRead);
        const vk::DescriptorImageInfo raw_info{.imageView = *raw_view,
                                               .imageLayout = vk::ImageLayout::eGeneral};
        const vk::DescriptorImageInfo result_info{.imageView = *result_view,
                                                  .imageLayout = vk::ImageLayout::eGeneral};
        const std::array<vk::WriteDescriptorSet, 2> writes = {{
            {.dstBinding = 0,
             .descriptorCount = 1,
             .descriptorType = vk::DescriptorType::eStorageImage,
             .pImageInfo = &raw_info},
            {.dstBinding = 1,
             .descriptorCount = 1,
             .descriptorType = vk::DescriptorType::eStorageImage,
             .pImageInfo = &result_info},
        }};
        const std::array<float, 2> strength{sharpness, 0.0f};
        cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, *sharpen_pipeline);
        cmdbuf.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, *copy_pipeline_layout, 0, writes);
        cmdbuf.pushConstants(*copy_pipeline_layout, vk::ShaderStageFlagBits::eCompute, 0,
                             sizeof(strength), strength.data());
        cmdbuf.dispatch((ow + 7) / 8, (oh + 7) / 8, 1);
        barrier(vk::Image(result_image), vk::ImageLayout::eGeneral,
                vk::PipelineStageFlagBits2::eComputeShader, vk::AccessFlagBits2::eShaderWrite, all, rw);
        if (dump >= 0) {
            FrameCapture::DumpImages(instance, scheduler, cmdbuf, dump,
                {{vk::Image(raw_image), ow, oh, 8, "sr_unsharpened", "rgba16f"},
                 {vk::Image(result_image), ow, oh, 8, "sr_sharpened", "rgba16f"}});
        }
    }

    // Optional neural rendering (off by default) on the upscaled picture. BB_NR_ENCODING: 0 sRGB, 1 linear SDR, 2 linear HDR.
    static const int nr_encoding = [] {
        const char* e = std::getenv("BB_NR_ENCODING");
        return e ? std::atoi(e) : 2;
    }();
    neural->Record(cmdbuf, BbNeural::PostUpscale,
                   {vk::Image(result_image), *result_view, vk::Format::eR16G16B16A16Sfloat, ow, oh,
                    nr_encoding >= 1, nr_encoding == 2},
                   {*motion_view, depth_view, w, h}, was_reset);

    if (upscale) {
        // The pass scaling the game's picture into its display buffer samples the upscaled one
        // instead (by UV, so the size does not matter); what reads the picture by pixel keeps
        // the game's.
        const vk::MemoryBarrier2 ready_to_read{
            .srcStageMask = all,
            .srcAccessMask = vk::AccessFlagBits2::eMemoryWrite,
            .dstStageMask = all,
            .dstAccessMask = vk::AccessFlagBits2::eShaderRead,
        };
        cmdbuf.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &ready_to_read});
        redirect_id = color_id;
        redirect_valid = true;
        return;
    }
    if (!output) {
        return;
    }
    // The reconstructed picture replaces what the game's pass wrote.
    const auto out_view = View(*output, vk::Format::eB10G11R11UfloatPack32,
                               vk::ImageAspectFlagBits::eColor);
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
