// SPDX-License-Identifier: GPL-2.0-or-later
#include <cstdlib>
#include "video_core/renderer_vulkan/vk_drive_motion.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "video_core/host_shaders/drive_motion_comp.h"
#include "video_core/renderer_vulkan/vk_instance.h"
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
    float use_stencil;
    float use_object;
};

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

DriveMotion::DriveMotion(const Instance& instance) {
    const auto device = instance.GetDevice();
    const std::array<vk::DescriptorSetLayoutBinding, 5> bindings = {{
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

void DriveMotion::OnTaaConstants(u32 slot, const float* data, u32 dwords) {
    // Slot 0, dwords 16..31: the relative camera move current view -> previous view followed by
    // the projection (the anti-aliasing pass reprojects with it). Slot 1, dwords 48..63: the
    // projection itself (row vectors, reversed Z, infinite far plane).
    if (slot == 0 && dwords >= 32) {
        std::memcpy(relative.data(), data + 16, sizeof(relative));
        got_slot0 = true;
    } else if (slot == 1 && dwords >= 64) {
        std::memcpy(projection.data(), data + 48, sizeof(projection));
        got_slot1 = true;
    }
}

void DriveMotion::EndFrame() noexcept {
    // the same values again (no new frame of the game's): nothing moves back
    if (frames_kept > 0 && relative == previous_relative && projection == previous_projection) {
        return;
    }
    projection_two_back = previous_projection;
    previous_projection = projection;
    previous_relative = relative;
    frames_kept = std::min(frames_kept + 1, 3);
}

namespace {
// How far a row-vector 4x4 is from a rigid move (orthonormal 3x3, last column 0 0 0 1).
float RigidError(const Mat& m) {
    float e = std::abs(m[3]) + std::abs(m[7]) + std::abs(m[11]) + std::abs(m[15] - 1.0f);
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) {
            float d = 0.0f;
            for (int k = 0; k < 3; ++k) d += m[i * 4 + k] * m[j * 4 + k];
            e += std::abs(d - (i == j ? 1.0f : 0.0f));
        }
    return e;
}
} // namespace

bool DriveMotion::ClipToPrevious(std::array<float, 16>& out, bool chain) const {
    Mat inverse{};
    if (!Invert(projection, inverse)) {
        out = Identity;
        return false;
    }
    out = Multiply(inverse, relative);
    // the last frame and the one before (EndFrame of this frame not run yet)
    if (!chain || frames_kept < 2) {
        return true;
    }
    // The move is view -> previous view, followed by a projection: the previous frame's or this one's (they differ
    // in VR, each eye its own). The one that leaves a rigid move is it. Then clip -> clip two frames back:
    // P^-1 * M(this frame) * M(last frame) * P(two frames back).
    Mat inv_prev{}, inv_prev2{};
    if (!Invert(previous_projection, inv_prev) || !Invert(projection_two_back, inv_prev2)) {
        return true;
    }
    const Mat move_a = Multiply(relative, inv_prev), move_b = Multiply(relative, inverse);
    const bool a = RigidError(move_a) <= RigidError(move_b);
    const Mat move = a ? move_a : move_b;
    const Mat last_move = Multiply(previous_relative, a ? inv_prev2 : inv_prev);
    // into the clip space of the frame two back: the same eye's, with its own projection (each eye's differs)
    out = Multiply(Multiply(Multiply(inverse, move), last_move), projection_two_back);
    static int told = 0;
    if (told++ < 3) {
        std::printf("Camera motion: two frames back (VR), the move's projection is %s (rigid error %.4f against "
                    "%.4f)\n",
                    a ? "the previous frame's" : "this frame's", a ? RigidError(move_a) : RigidError(move_b),
                    a ? RigidError(move_b) : RigidError(move_a));
    }
    return true;
}

void DriveMotion::Record(vk::CommandBuffer cmdbuf, vk::ImageView depth_view,
                         vk::ImageView mask_view, vk::ImageView motion_view, u32 width, u32 height, u32 src_width,
                         u32 src_height, bool normalized, vk::ImageView object_view,
                         vk::ImageView color_view) {
    Push push{};
    ClipToPrevious(push.clip_to_prev_clip, two_back);
    push.size = {float(width), float(height)};
    push.src_size = {float(src_width), float(src_height)};
    // DLSS Frame Generation takes screen fractions (its scale is 1); FSR takes pixels.
    push.scale = normalized ? std::array<float, 2>{1.0f / float(width), 1.0f / float(height)}
                            : std::array<float, 2>{1.0f, 1.0f};
    const vk::DescriptorImageInfo depth_info{.imageView = depth_view,
                                             .imageLayout = vk::ImageLayout::eGeneral};
    const vk::DescriptorImageInfo motion_info{.imageView = motion_view,
                                              .imageLayout = vk::ImageLayout::eGeneral};
    push.use_stencil = mask_view != depth_view ? 1.0f : 0.0f;
    push.use_object = object_view && color_view ? 1.0f : 0.0f;
    const vk::DescriptorImageInfo mask_info{.imageView = mask_view,
                                            .imageLayout = vk::ImageLayout::eGeneral};
    const vk::DescriptorImageInfo object_info{
        .imageView = object_view ? object_view : depth_view,
        .imageLayout = vk::ImageLayout::eGeneral};
    const vk::DescriptorImageInfo color_info{
        .imageView = color_view ? color_view : depth_view,
        .imageLayout = vk::ImageLayout::eGeneral};
    const std::array<vk::WriteDescriptorSet, 5> writes = {{
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
    }};
    cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, *pipeline);
    cmdbuf.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, *pipeline_layout, 0, writes);
    cmdbuf.pushConstants(*pipeline_layout, vk::ShaderStageFlagBits::eCompute, 0, sizeof(push),
                         &push);
    cmdbuf.dispatch((width + 7) / 8, (height + 7) / 8, 1);
}

void DriveMotion::ClipTransforms(std::array<float, 16>& view_to_clip,
                                 std::array<float, 16>& clip_to_prev_clip,
                                 std::array<float, 16>& prev_clip_to_clip) const {
    view_to_clip = projection;
    ClipToPrevious(clip_to_prev_clip, false); // the frame generator: one frame back
    Mat inverse{};
    prev_clip_to_clip = Invert(clip_to_prev_clip, inverse) ? inverse : Identity;
}

float DriveMotion::Near() const noexcept {
    return projection[14] > 0.0f ? projection[14] : 0.02f;
}

float DriveMotion::VerticalFov() const noexcept {
    return projection[5] > 0.0f ? 2.0f * std::atan(1.0f / projection[5]) : 0.9f;
}

} // namespace Vulkan
