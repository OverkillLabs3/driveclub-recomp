// SPDX-License-Identifier: GPL-2.0-or-later
#include "bbport_diagnostics.h"
#include <cstdlib>
#include "video_core/renderer_vulkan/vk_drive_motion.h"

#include <cmath>
#include <cstring>

#include "video_core/host_shaders/drive_motion_comp.h"
#include "video_core/host_shaders/drive_native_motion_comp.h"
#include "video_core/host_shaders/drive_native_motion_ids_comp.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_frame_capture.h"
#include <vk_mem_alloc.h>
#include "video_core/renderer_vulkan/vk_object_motion.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"

namespace Vulkan {

namespace {

using Mat = std::array<float, 16>;

constexpr Mat Identity = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};

struct Push {
    std::array<float, 2> src_size;
    std::array<float, 2> pad; // the matrix starts on 16 bytes in the shader
    Mat clip_to_prev_clip;
    std::array<float, 2> size;
    std::array<float, 2> scale;
    std::array<float, 2> jitter;
    float use_native;
    float use_color;
    float use_stencil;
    float use_object;
    u32 stencil_mask;
    float near_depth; ///< reversed-Z depth above which geometry is screen-static (0 = off)
};
static_assert(sizeof(Push) == 128);

Mat Multiply(const Mat& a, const Mat& b) {
    Mat out{};
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) {
            float v = 0.0f;
            for (int k = 0; k < 4; ++k) {
                v += a[r * 4 + k] * b[k * 4 + c];
            }
            out[r * 4 + c] = v;
        }
    }
    return out;
}

/// General 4x4 inverse by Gauss-Jordan in doubles. Returns false for a singular matrix.
bool Invert(const Mat& m, Mat& out) {
    double a[4][8];
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) {
            a[r][c] = m[r * 4 + c];
            a[r][4 + c] = r == c ? 1.0 : 0.0;
        }
    }
    for (int col = 0; col < 4; ++col) {
        int pivot = col;
        for (int r = col + 1; r < 4; ++r) {
            if (std::fabs(a[r][col]) > std::fabs(a[pivot][col])) {
                pivot = r;
            }
        }
        if (std::fabs(a[pivot][col]) < 1e-30) {
            return false;
        }
        for (int c = 0; c < 8; ++c) {
            std::swap(a[col][c], a[pivot][c]);
        }
        const double scale = 1.0 / a[col][col];
        for (int c = 0; c < 8; ++c) {
            a[col][c] *= scale;
        }
        for (int r = 0; r < 4; ++r) {
            if (r == col) {
                continue;
            }
            const double f = a[r][col];
            for (int c = 0; c < 8; ++c) {
                a[r][c] -= f * a[col][c];
            }
        }
    }
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) {
            out[r * 4 + c] = float(a[r][4 + c]);
        }
    }
    return true;
}

} // namespace

DriveMotion::DriveMotion(const Instance& instance_, Scheduler& scheduler_)
    : instance{instance_}, scheduler{scheduler_} {
    const auto device = instance.GetDevice();
    const std::array<vk::DescriptorSetLayoutBinding, 6> bindings = {{
        {.binding = 0,
         .descriptorType = vk::DescriptorType::eSampledImage,
         .descriptorCount = 1,
         .stageFlags = vk::ShaderStageFlagBits::eCompute},
        {.binding = 1,
         .descriptorType = vk::DescriptorType::eStorageImage,
         .descriptorCount = 1,
         .stageFlags = vk::ShaderStageFlagBits::eCompute},
        {.binding = 2,
         .descriptorType = vk::DescriptorType::eSampledImage,
         .descriptorCount = 1,
         .stageFlags = vk::ShaderStageFlagBits::eCompute},
        {.binding = 3,
         .descriptorType = vk::DescriptorType::eSampledImage,
         .descriptorCount = 1,
         .stageFlags = vk::ShaderStageFlagBits::eCompute},
        {.binding = 4,
         .descriptorType = vk::DescriptorType::eSampledImage,
         .descriptorCount = 1,
         .stageFlags = vk::ShaderStageFlagBits::eCompute},        {.binding = 5,
         .descriptorType = vk::DescriptorType::eSampledImage,
         .descriptorCount = 1,
         .stageFlags = vk::ShaderStageFlagBits::eCompute},
    }};
    desc_layout = Check(device.createDescriptorSetLayoutUnique({
        .flags = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR,
        .bindingCount = static_cast<u32>(bindings.size()),
        .pBindings = bindings.data(),
    }));
    const vk::PushConstantRange range{.stageFlags = vk::ShaderStageFlagBits::eCompute,
                                      .offset = 0,
                                      .size = sizeof(Push)};
    pipeline_layout = Check(device.createPipelineLayoutUnique({
        .setLayoutCount = 1,
        .pSetLayouts = &*desc_layout,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &range,
    }));
    const auto module = CompileSPV(DRIVE_MOTION_COMP, device);
    pipeline = Check(device.createComputePipelineUnique(
        {}, vk::ComputePipelineCreateInfo{
                .stage = {.stage = vk::ShaderStageFlagBits::eCompute,
                          .module = module,
                          .pName = "main"},
                .layout = *pipeline_layout,
            }));
    device.destroyShaderModule(module);
}

DriveMotion::~DriveMotion() {
    scheduler.Finish();
    if (native_buffer) {
        vmaDestroyBuffer(instance.GetAllocator(), native_buffer, native_allocation);
    }
}

void DriveMotion::EnsureNative(u32 width, u32 height) {
    const auto device = instance.GetDevice();
    if (!native_buffer) {
        const VkBufferCreateInfo info{.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .size = sizeof(native_transforms),
            .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT};
        const VmaAllocationCreateInfo allocation_info{.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
        VkBuffer buffer{};
        if (vmaCreateBuffer(instance.GetAllocator(), &info, &allocation_info, &buffer,
                            &native_allocation, nullptr) != VK_SUCCESS) {
            return;
        }
        native_buffer = buffer;
        const std::array<vk::DescriptorSetLayoutBinding, 5> bindings = {{
            {0, vk::DescriptorType::eSampledImage, 1, vk::ShaderStageFlagBits::eCompute},
            {1, vk::DescriptorType::eSampledImage, 1, vk::ShaderStageFlagBits::eCompute},
            {2, vk::DescriptorType::eStorageBuffer, 1, vk::ShaderStageFlagBits::eCompute},
            {3, vk::DescriptorType::eStorageImage, 1, vk::ShaderStageFlagBits::eCompute},
            {4, vk::DescriptorType::eSampledImage, 1, vk::ShaderStageFlagBits::eCompute},
        }};
        native_desc_layout = Check(device.createDescriptorSetLayoutUnique({
            .flags = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR,
            .bindingCount = u32(bindings.size()), .pBindings = bindings.data()}));
        const vk::PushConstantRange push{vk::ShaderStageFlagBits::eCompute, 0, 28};
        native_pipeline_layout = Check(device.createPipelineLayoutUnique({
            .setLayoutCount = 1, .pSetLayouts = &*native_desc_layout,
            .pushConstantRangeCount = 1, .pPushConstantRanges = &push}));
        for (u32 i = 0; i < 2; ++i) {
            const auto module = i == 0 ? CompileSPV(DRIVE_NATIVE_MOTION_COMP, device)
                                      : CompileSPV(DRIVE_NATIVE_MOTION_IDS_COMP, device);
            native_pipelines[i] = Check(device.createComputePipelineUnique({},
                vk::ComputePipelineCreateInfo{.stage = {.stage = vk::ShaderStageFlagBits::eCompute,
                    .module = module, .pName = "main"}, .layout = *native_pipeline_layout}));
            device.destroyShaderModule(module);
        }
    }
    if (native_image && width == native_width && height == native_height) {
        return;
    }
    if (native_image) {
        scheduler.Finish();
    }
    native_view.reset();
    native_image = VideoCore::UniqueImage(device, instance.GetAllocator());
    native_image.Create({.imageType = vk::ImageType::e2D,
        .format = vk::Format::eR32G32B32A32Sfloat, .extent = {width, height, 1},
        .mipLevels = 1, .arrayLayers = 1, .samples = vk::SampleCountFlagBits::e1,
        .tiling = vk::ImageTiling::eOptimal,
        .usage = vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled |
                 vk::ImageUsageFlagBits::eTransferSrc,
        .initialLayout = vk::ImageLayout::eUndefined});
    native_view = Check(device.createImageViewUnique({.image = vk::Image(native_image),
        .viewType = vk::ImageViewType::e2D, .format = vk::Format::eR32G32B32A32Sfloat,
        .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1}}));
    native_width = width;
    native_height = height;
    native_layout = vk::ImageLayout::eUndefined;
    native_valid = false;
}

void DriveMotion::OnTaaTransforms(const float* data, u32 dwords) {
    native_count = std::min(u32(native_transforms.size()), dwords / 48);
    for (u32 i = 0; i < native_count; ++i) {
        std::memcpy(native_transforms[i].data(), data + i * 48 + 16,
                    sizeof(native_transforms[i]));
    }
}

void DriveMotion::CaptureNative(vk::ImageView gbuffer, vk::ImageView depth,
                                vk::ImageView previous_ids, u32 width, u32 height) {
    if (!Ready() || !native_count) {
        return;
    }
    Mat inverse{};
    if (!Invert(projection, inverse)) {
        return;
    }
    auto transforms = native_transforms;
    for (u32 i = 0; i < native_count; ++i) {
        transforms[i] = Multiply(inverse, transforms[i]);
    }
    EnsureNative(width, height);
    if (!native_buffer) {
        return;
    }
    const vk::MemoryBarrier2 before{.srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
        .srcAccessMask = vk::AccessFlagBits2::eShaderRead,
        .dstStageMask = vk::PipelineStageFlagBits2::eTransfer,
        .dstAccessMask = vk::AccessFlagBits2::eTransferWrite};
    const vk::MemoryBarrier2 after{.srcStageMask = vk::PipelineStageFlagBits2::eTransfer,
        .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
        .dstAccessMask = vk::AccessFlagBits2::eShaderRead};
    const vk::ImageMemoryBarrier2 to_write{
        .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .srcAccessMask = vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
        .dstAccessMask = vk::AccessFlagBits2::eShaderWrite,
        .oldLayout = native_layout, .newLayout = vk::ImageLayout::eGeneral,
        .image = vk::Image(native_image),
        .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1}};
    struct NativePush {
        std::array<float, 2> size, jitter, previous_jitter;
        u32 count;
    } push{{float(width), float(height)}, jitter, previous_jitter, native_count};
    static_assert(sizeof(NativePush) == 28);
    const vk::ImageMemoryBarrier2 to_read{
        .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
        .srcAccessMask = vk::AccessFlagBits2::eShaderWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
        .dstAccessMask = vk::AccessFlagBits2::eShaderRead,
        .oldLayout = vk::ImageLayout::eGeneral, .newLayout = vk::ImageLayout::eGeneral,
        .image = vk::Image(native_image),
        .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1}};
    scheduler.Record([buffer = native_buffer, transforms, before, after, to_write, to_read,
                      gbuffer, depth, previous_ids, output = *native_view, push, width, height,
                      pipeline = *native_pipelines[previous_ids ? 1 : 0],
                      layout = *native_pipeline_layout](vk::CommandBuffer cmd) {
        cmd.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &before});
        cmd.updateBuffer(buffer, 0, sizeof(transforms), transforms.data());
        cmd.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &after});
        cmd.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &to_write});
        const vk::DescriptorImageInfo gbuffer_info{.imageView = gbuffer,
            .imageLayout = vk::ImageLayout::eGeneral};
        const vk::DescriptorImageInfo depth_info{.imageView = depth,
            .imageLayout = vk::ImageLayout::eGeneral};
        const vk::DescriptorBufferInfo transforms_info{buffer, 0, sizeof(transforms)};
        const vk::DescriptorImageInfo output_info{.imageView = output,
            .imageLayout = vk::ImageLayout::eGeneral};
        const vk::DescriptorImageInfo ids_info{.imageView = previous_ids,
            .imageLayout = vk::ImageLayout::eGeneral};
        const std::array<vk::WriteDescriptorSet, 5> writes = {{
            {.dstBinding = 0, .descriptorCount = 1, .descriptorType = vk::DescriptorType::eSampledImage,
             .pImageInfo = &gbuffer_info},
            {.dstBinding = 1, .descriptorCount = 1, .descriptorType = vk::DescriptorType::eSampledImage,
             .pImageInfo = &depth_info},
            {.dstBinding = 2, .descriptorCount = 1, .descriptorType = vk::DescriptorType::eStorageBuffer,
             .pBufferInfo = &transforms_info},
            {.dstBinding = 3, .descriptorCount = 1, .descriptorType = vk::DescriptorType::eStorageImage,
             .pImageInfo = &output_info},
            {.dstBinding = 4, .descriptorCount = 1, .descriptorType = vk::DescriptorType::eSampledImage,
             .pImageInfo = &ids_info},
        }};
        cmd.bindPipeline(vk::PipelineBindPoint::eCompute, pipeline);
        cmd.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, layout, 0,
            vk::ArrayProxy<const vk::WriteDescriptorSet>{previous_ids ? 5u : 4u, writes.data()});
        cmd.pushConstants(layout, vk::ShaderStageFlagBits::eCompute, 0, sizeof(push), &push);
        cmd.dispatch((width + 7) / 8, (height + 7) / 8, 1);
        cmd.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &to_read});
    });
    native_layout = vk::ImageLayout::eGeneral;
    native_valid = true;
}

void DriveMotion::OnTaaConstants(u32 slot, const float* data, u32 dwords) {
    // Slot 0, dwords 16..31: the relative camera move current view -> previous view followed by
    // the projection (the anti-aliasing pass reprojects with it). Slot 1, dwords 48..63: the
    // projection itself (row vectors, reversed Z).
    if (slot == 0 && dwords >= 32) {
        std::memcpy(relative.data(), data + 16, sizeof(relative));
        got_slot0 = true;
    } else if (slot == 1 && dwords >= 64) {
        std::memcpy(projection.data(), data + 48, sizeof(projection));
        got_slot1 = true;
    }
}

bool DriveMotion::ClipToPrevious(std::array<float, 16>& out) const {
    Mat inverse{};
    if (!Invert(projection, inverse)) {
        out = Identity;
        return false;
    }
    out = Multiply(inverse, relative);
    return true;
}

void DriveMotion::Record(vk::CommandBuffer cmdbuf, vk::ImageView depth_view,
                         vk::ImageView mask_view, vk::ImageView motion_view, u32 width, u32 height, u32 src_width,
                         u32 src_height, bool normalized, vk::ImageView object_view,
                         vk::ImageView color_view) {
    Push push{};
    ClipToPrevious(push.clip_to_prev_clip);
    push.size = {float(width), float(height)};
    push.src_size = {float(src_width), float(src_height)};
    // DLSS Frame Generation takes screen fractions (its scale is 1); FSR takes pixels.
    push.scale = normalized ? std::array<float, 2>{1.0f / float(width), 1.0f / float(height)}
                            : std::array<float, 2>{1.0f, 1.0f};
    push.jitter = jitter;
    push.use_native = native_valid && native_width == src_width && native_height == src_height
                          ? 1.0f : 0.0f;

    bool object_valid = bool(object_view);
    if (!object_view && object_motion && object_motion->Enabled()) {
        object_view = object_motion->PrepareRead(cmdbuf, src_width, src_height, object_valid);
    }
    push.use_object = object_valid ? 1.0f : 0.0f;
    push.use_color = color_view ? 1.0f : 0.0f;
    if (!object_view) object_view = depth_view;
    const vk::DescriptorImageInfo depth_info{.imageView = depth_view,
                                             .imageLayout = vk::ImageLayout::eGeneral};
    const vk::DescriptorImageInfo motion_info{.imageView = motion_view,
                                              .imageLayout = vk::ImageLayout::eGeneral};
    push.use_stencil = mask_view != depth_view ? 1.0f : 0.0f;
    static const u32 stencil_mask = [] {
        const char* value = std::getenv("BB_AA_STENCIL_MASK");
        return value ? u32(std::strtoul(value, nullptr, 0)) : 130u;
    }();
    push.stencil_mask = stencil_mask;
    static const float near_depth = [] {
        const char* value = std::getenv("BB_AA_NEAR_DEPTH");
        return value ? std::strtof(value, nullptr) : 0.01f;
    }();
    push.near_depth = near_depth;
    const vk::DescriptorImageInfo mask_info{.imageView = mask_view,
                                            .imageLayout = vk::ImageLayout::eGeneral};
    const vk::DescriptorImageInfo object_info{.imageView = object_view,
                                              .imageLayout = vk::ImageLayout::eGeneral};
    const vk::DescriptorImageInfo native_info{
        .imageView = native_view ? *native_view : depth_view,
        .imageLayout = vk::ImageLayout::eGeneral};
    const vk::DescriptorImageInfo color_info{
        .imageView = color_view ? color_view : depth_view,
        .imageLayout = vk::ImageLayout::eGeneral};
    const std::array<vk::WriteDescriptorSet, 6> writes = {{
        {.dstBinding = 2,
         .descriptorCount = 1,
         .descriptorType = vk::DescriptorType::eSampledImage,
         .pImageInfo = &mask_info},
        {.dstBinding = 0,
         .descriptorCount = 1,
         .descriptorType = vk::DescriptorType::eSampledImage,
         .pImageInfo = &depth_info},
        {.dstBinding = 3,
         .descriptorCount = 1,
         .descriptorType = vk::DescriptorType::eSampledImage,
         .pImageInfo = &object_info},
        {.dstBinding = 4,
         .descriptorCount = 1,
         .descriptorType = vk::DescriptorType::eSampledImage,
         .pImageInfo = &color_info},
        {.dstBinding = 1,
         .descriptorCount = 1,
         .descriptorType = vk::DescriptorType::eStorageImage,
         .pImageInfo = &motion_info},

        {.dstBinding = 5,
         .descriptorCount = 1,
         .descriptorType = vk::DescriptorType::eSampledImage,
         .pImageInfo = &native_info},
    }};
    cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, *pipeline);
    cmdbuf.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, *pipeline_layout, 0, writes);
    cmdbuf.pushConstants(*pipeline_layout, vk::ShaderStageFlagBits::eCompute, 0, sizeof(push),
                         &push);
    cmdbuf.dispatch((width + 7) / 8, (height + 7) / 8, 1);
}

void DriveMotion::DumpInputs(vk::CommandBuffer cmdbuf, int frame, u32 width, u32 height) {
    const u64 object_frame = object_motion ? object_motion->CaptureFrame() : 0;
    if (frame == 0 && object_motion) {
        object_motion->CaptureHistory();
    }
    if (native_valid && native_width == width && native_height == height) {
        FrameCapture::DumpImages(instance, scheduler, cmdbuf, frame,
            {{vk::Image(native_image), width, height, 16, "native", "rgba32f"}});
    }
    const auto object_image = object_motion ? object_motion->Image(width, height) : vk::Image{};
    if (object_image) {
        FrameCapture::DumpImages(instance, scheduler, cmdbuf, frame,
            {{object_image, width, height, 16, "object", "rgba32f"}});
    }
    const char* dir = BB_DIAGNOSTIC_ENV("BB_DUMP_DIR");
    char path[512];
    std::snprintf(path, sizeof(path), "%s/f%03d_guides.txt", dir && *dir ? dir : "out/dump", frame);
    if (FILE* file = std::fopen(path, "wb")) {
        std::fprintf(file, "jitter %.9g %.9g\nprevious_jitter %.9g %.9g\nnative_count %u\nobject_valid %d\n",
                     jitter[0], jitter[1], previous_jitter[0], previous_jitter[1], native_count,
                     int(bool(object_image)));
        std::fprintf(file, "object_frame %llu\n",
                     static_cast<unsigned long long>(object_frame));
        for (const auto& [name, matrix] : {std::pair{"relative", relative},
                                          std::pair{"projection", projection}}) {
            std::fprintf(file, "%s", name);
            for (float value : matrix) std::fprintf(file, " %.9g", value);
            std::fputc('\n', file);
        }
        std::fclose(file);
    }
}

void DriveMotion::ClipTransforms(std::array<float, 16>& view_to_clip,
                                 std::array<float, 16>& clip_to_prev_clip,
                                 std::array<float, 16>& prev_clip_to_clip) const {
    view_to_clip = projection;
    ClipToPrevious(clip_to_prev_clip);
    Mat inverse{};
    prev_clip_to_clip = Invert(clip_to_prev_clip, inverse) ? inverse : Identity;
}

float DriveMotion::Near() const noexcept {
    return projection[14] > 0.0f ? projection[14] / (1.0f + projection[10]) : 0.02f;
}

float DriveMotion::Far() const noexcept {
    return projection[14] / projection[10];
}

float DriveMotion::VerticalFov() const noexcept {
    return projection[5] > 0.0f ? 2.0f * std::atan(1.0f / projection[5]) : 0.9f;
}

} // namespace Vulkan
