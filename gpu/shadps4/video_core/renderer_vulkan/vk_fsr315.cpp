// SPDX-License-Identifier: GPL-2.0-or-later
#include "video_core/renderer_vulkan/vk_fsr315.h"

#include <cstdio>
#include <string>
#include <utility>

#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"

namespace Vulkan {

namespace {

constexpr u32 kTokenStateRead = FFX_VK_FSR3_3_1_5_RESOURCE_STATE_COMPUTE_READ;
constexpr u32 kTokenStateWrite = FFX_VK_FSR3_3_1_5_RESOURCE_STATE_UNORDERED_ACCESS;

} // namespace

Fsr315::Fsr315(const Instance& instance_, Scheduler& scheduler_)
    : instance{instance_}, scheduler{scheduler_} {}

Fsr315::~Fsr315() {
    // The renderer waits for the GPU before destroying the rasterizer; only release here.
    ReleaseAll();
}

void Fsr315::Fail(const char* what, int result) {
    problem = std::string{what} + " (" + std::to_string(result) + ")";
    std::printf("FSR 3.1.5: %s\n", problem.c_str());
}

bool Fsr315::CreateShared(const FfxVkFsr3_3_1_5SharedResourceDescription& description,
                          SharedImage& out) {
    if (description.format == VK_FORMAT_UNDEFINED || description.width == 0 ||
        description.height == 0 || description.mipCount != 1) {
        problem = "a shared temporal resource has an unsupported description";
        return false;
    }
    const auto device = instance.GetDevice();
    out.image = VideoCore::UniqueImage(device, instance.GetAllocator());
    out.image.Create(vk::ImageCreateInfo{
        .imageType = vk::ImageType::e2D,
        .format = vk::Format(description.format),
        .extent = {description.width, description.height, 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = vk::SampleCountFlagBits::e1,
        .tiling = vk::ImageTiling::eOptimal,
        .usage = vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eStorage |
                 vk::ImageUsageFlagBits::eTransferDst,
        .initialLayout = vk::ImageLayout::eUndefined,
    });
    out.format = vk::Format(description.format);
    out.width = description.width;
    out.height = description.height;
    return true;
}

bool Fsr315::Create(u32 render_width, u32 render_height, u32 out_width, u32 out_height) {
    Destroy();
    problem.clear();
    FfxVkFsr3_3_1_5UpscalerCreateInfo create{};
    create.physicalDevice = VkPhysicalDevice(instance.GetPhysicalDevice());
    create.device = VkDevice(instance.GetDevice());
    create.maxRenderWidth = render_width;
    create.maxRenderHeight = render_height;
    create.maxUpscaleWidth = out_width;
    create.maxUpscaleHeight = out_height;
    create.hdrColorInput = VK_TRUE;
    create.autoExposure = VK_TRUE;
    create.depthInverted = VK_TRUE;
    create.depthInfinite = VK_FALSE;
    if (const auto result = ffxVkFsr3_3_1_5UpscalerContextCreate(&create, &context);
        result != FFX_VK_FSR3_3_1_5_OK) {
        context = nullptr;
        Fail("context creation failed", int(result));
        return false;
    }
    bridge = ffxVkFsr3_3_1_5UpscalerContextGetBridge(context);
    if (!bridge) {
        Fail("the bridge is unavailable", -1);
        Destroy();
        return false;
    }
    FfxVkFsr3_3_1_5SharedResourceDescriptions shared{};
    if (const auto result = ffxVkFsr3_3_1_5UpscalerContextGetSharedResourceDescriptions(
            context, &shared);
        result != FFX_VK_FSR3_3_1_5_OK) {
        Fail("shared resource query failed", int(result));
        Destroy();
        return false;
    }
    if (!CreateShared(shared.dilatedDepth, dilated_depth) ||
        !CreateShared(shared.dilatedMotionVectors, dilated_motion) ||
        !CreateShared(shared.reconstructedPrevNearestDepth, previous_depth)) {
        Destroy();
        return false;
    }
    std::printf("FSR 3.1.5: %ux%u to %ux%u, shared %ux%u %ux%u %ux%u\n", render_width,
                render_height, out_width, out_height, dilated_depth.width, dilated_depth.height,
                dilated_motion.width, dilated_motion.height, previous_depth.width,
                previous_depth.height);
    return true;
}

void Fsr315::Release(FfxVkFsr3_3_1_5Resource& resource) {
    if (bridge && resource.resource) {
        ffxVkFsr3_3_1_5BridgeReleaseImportedImage(bridge, resource);
    }
    resource = {};
}

void Fsr315::Retire() {
    while (!in_flight.empty() && scheduler.IsFree(in_flight.front().tick)) {
        if (context) {
            ffxVkFsr3_3_1_5UpscalerContextRetireFrame(context, in_flight.front().frame_id);
        }
        for (auto& token : in_flight.front().tokens) {
            Release(token);
        }
        in_flight.pop_front();
    }
}

void Fsr315::Destroy() {
    if (context) {
        scheduler.Finish();
    }
    ReleaseAll();
}

void Fsr315::ReleaseAll() {
    if (!context) {
        in_flight.clear();
        return;
    }
    while (!in_flight.empty()) {
        ffxVkFsr3_3_1_5UpscalerContextRetireFrame(context, in_flight.front().frame_id);
        for (auto& token : in_flight.front().tokens) {
            Release(token);
        }
        in_flight.pop_front();
    }
    for (SharedImage* shared : {&dilated_depth, &dilated_motion, &previous_depth}) {
        shared->image = VideoCore::UniqueImage{};
        shared->format = vk::Format::eUndefined;
        shared->width = 0;
        shared->height = 0;
    }
    ffxVkFsr3_3_1_5UpscalerContextDestroy(context);
    context = nullptr;
    bridge = nullptr;
    shared_transitioned = false;
    shared_initialized = false;
}

FfxVkFsr3_3_1_5Resource Fsr315::Import(vk::Image image, vk::Format format, u32 width, u32 height,
                                       u32 state, bool storage, bool undefined_contents) {
    FfxVkFsr3_3_1_5ImportedImageDescription description{};
    description.image = VkImage(image);
    description.format = VkFormat(format);
    description.width = width;
    description.height = height;
    description.mipCount = 1;
    description.arrayLayers = 1;
    description.layout = VK_IMAGE_LAYOUT_GENERAL;
    description.state = state;
    description.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
    if (storage) {
        description.usage |= VK_IMAGE_USAGE_STORAGE_BIT;
    }
    description.undefinedContents = undefined_contents ? VK_TRUE : VK_FALSE;
    return ffxVkFsr3_3_1_5BridgeImportImage(bridge, &description);
}

bool Fsr315::Record(const Frame& frame) {
    if (!context || frame.width == 0 || frame.height == 0 || frame.out_width == 0 ||
        frame.out_height == 0) {
        problem = "not ready";
        return false;
    }
    Retire();
    if (!shared_transitioned) {
        // The shared images start undefined; the SDK's first dispatch reads and writes them.
        std::array<vk::ImageMemoryBarrier2, 3> barriers{};
        const std::array<SharedImage*, 3> images{&dilated_depth, &dilated_motion, &previous_depth};
        for (size_t i = 0; i < images.size(); ++i) {
            barriers[i] = vk::ImageMemoryBarrier2{
                .srcStageMask = vk::PipelineStageFlagBits2::eNone,
                .srcAccessMask = vk::AccessFlagBits2::eNone,
                .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands,
                .dstAccessMask = vk::AccessFlagBits2::eShaderRead |
                                 vk::AccessFlagBits2::eShaderWrite,
                .oldLayout = vk::ImageLayout::eUndefined,
                .newLayout = vk::ImageLayout::eGeneral,
                .image = vk::Image(images[i]->image),
                .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
            };
        }
        frame.command.pipelineBarrier2(
            {.imageMemoryBarrierCount = u32(barriers.size()), .pImageMemoryBarriers = barriers.data()});
        shared_transitioned = true;
    }

    InFlight flight{};
    flight.frame_id = next_frame++;
    flight.tick = scheduler.CurrentTick();
    auto& tokens = flight.tokens;
    // The shared temporal images are imported per frame: their "undefined contents" flag is a
    // per-dispatch property and must become false once the first dispatch filled them.
    const bool undefined_contents = !shared_initialized;
    tokens[0] = Import(frame.color, frame.color_format, frame.width, frame.height,
                       kTokenStateRead, false, false);
    tokens[1] = Import(frame.depth, frame.depth_format, frame.width, frame.height, kTokenStateRead,
                       false, false);
    tokens[2] = Import(frame.motion, vk::Format::eR16G16Sfloat, frame.width, frame.height,
                       kTokenStateRead, false, false);
    tokens[3] = Import(frame.output, vk::Format::eR16G16B16A16Sfloat, frame.out_width,
                       frame.out_height, kTokenStateWrite, true, false);
    tokens[4] = Import(vk::Image(dilated_depth.image), dilated_depth.format, dilated_depth.width,
                       dilated_depth.height, kTokenStateWrite, true, undefined_contents);
    tokens[5] = Import(vk::Image(dilated_motion.image), dilated_motion.format,
                       dilated_motion.width, dilated_motion.height, kTokenStateWrite, true,
                       undefined_contents);
    tokens[6] = Import(vk::Image(previous_depth.image), previous_depth.format,
                       previous_depth.width, previous_depth.height, kTokenStateWrite, true,
                       undefined_contents);
    for (auto& token : tokens) {
        if (!token.resource) {
            for (auto& release : tokens) {
                Release(release);
            }
            problem = "a frame image could not be imported for FSR 3.1.5";
            return false;
        }
    }

    FfxVkFsr3_3_1_5UpscalerDispatchInfo dispatch{};
    dispatch.commandBuffer = VkCommandBuffer(frame.command);
    dispatch.color = tokens[0];
    dispatch.depth = tokens[1];
    dispatch.motionVectors = tokens[2];
    dispatch.output = tokens[3];
    dispatch.dilatedDepth = tokens[4];
    dispatch.dilatedMotionVectors = tokens[5];
    dispatch.reconstructedPrevNearestDepth = tokens[6];
    dispatch.jitterOffsetX = frame.jitter_x;
    dispatch.jitterOffsetY = frame.jitter_y;
    dispatch.motionVectorScaleX = 1.0f;
    dispatch.motionVectorScaleY = 1.0f;
    dispatch.renderWidth = frame.width;
    dispatch.renderHeight = frame.height;
    dispatch.upscaleWidth = frame.out_width;
    dispatch.upscaleHeight = frame.out_height;
    dispatch.frameTimeMilliseconds = frame.frame_ms;
    dispatch.preExposure = 1.0f;
    dispatch.enableSharpening = frame.sharpen ? VK_TRUE : VK_FALSE;
    dispatch.sharpness = frame.sharpness;
    dispatch.reset = frame.reset ? VK_TRUE : VK_FALSE;
    dispatch.cameraNear = frame.near_plane;
    dispatch.cameraFar = frame.far_plane;
    dispatch.cameraVerticalFovRadians = frame.vertical_fov;
    dispatch.viewSpaceToMeters = 1.0f;
    dispatch.flags = 0;
    dispatch.frameId = flight.frame_id;
    const auto result = ffxVkFsr3_3_1_5UpscalerContextRecordDispatch(context, &dispatch);
    if (result != FFX_VK_FSR3_3_1_5_OK) {
        for (auto& token : tokens) {
            Release(token);
        }
        Fail("dispatch failed", int(result));
        return false;
    }
    problem.clear();
    shared_initialized = true;
    in_flight.push_back(flight);
    return true;
}

} // namespace Vulkan
