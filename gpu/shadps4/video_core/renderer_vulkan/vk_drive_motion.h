// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <array>

#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"
#include "video_core/texture_cache/image.h"

VK_DEFINE_HANDLE(VmaAllocation)

namespace Vulkan {

class Instance;
class ObjectMotion;
class Scheduler;

/// Camera motion vectors for frame generation. The matrices are the game's own: the constants of
/// its temporal anti-aliasing dispatch (current and previous view-projection, projection).
class DriveMotion {
public:
    DriveMotion(const Instance& instance, Scheduler& scheduler);
    ~DriveMotion();

    /// Object motion from the G-buffer, where the game's draws matched a previous frame.
    void SetObjectMotion(ObjectMotion* motion) noexcept {
        object_motion = motion;
    }
    /// Current and previous viewport jitter in render pixels (the geometry is shifted by it).
    void SetJitter(std::array<float, 2> current, std::array<float, 2> previous) noexcept {
        jitter = current;
        previous_jitter = previous;
    }

    [[nodiscard]] std::array<float, 2> Jitter() const noexcept {
        return jitter;
    }

    /// A constant buffer of the anti-aliasing dispatch: slot 0 (104 dwords) or slot 1 (128).
    void OnTaaConstants(u32 slot, const float* data, u32 dwords);
    void BeginNativeTaa() noexcept {
        got_slot0 = got_slot1 = false;
        native_count = 0;
    }
    void OnTaaTransforms(const float* data, u32 dwords);
    void CaptureNative(vk::ImageView gbuffer, vk::ImageView depth, vk::ImageView previous_ids,
                       u32 width, u32 height);
    void EndFrame() noexcept {
        got_slot0 = got_slot1 = false;
        native_count = 0;
        native_valid = false;
    }

    /// Both slots of this frame arrived since the last Consume.
    [[nodiscard]] bool Ready() const noexcept {
        return got_slot0 && got_slot1;
    }
    void Consume() noexcept {
        got_slot0 = got_slot1 = false;
    }

    /// Records the motion pass: `depth_view` (depth, General) into `motion_view` (RG16F, General).
    /// `mask_view` is the stencil of the same depth (the player's car moves with the camera), or
    /// `depth_view` itself when there is none.
    void Record(vk::CommandBuffer cmdbuf, vk::ImageView depth_view, vk::ImageView mask_view,
                vk::ImageView motion_view,
                u32 width, u32 height, u32 src_width, u32 src_height, bool normalized,
                vk::ImageView object_view = {}, vk::ImageView color_view = {});
    void DumpInputs(vk::CommandBuffer cmdbuf, int frame, u32 width, u32 height);

    /// Row-vector clip space matrices for the frame generator.
    void ClipTransforms(std::array<float, 16>& view_to_clip,
                        std::array<float, 16>& clip_to_prev_clip,
                        std::array<float, 16>& prev_clip_to_clip) const;
    [[nodiscard]] float Near() const noexcept;
    [[nodiscard]] float Far() const noexcept;
    [[nodiscard]] float VerticalFov() const noexcept;

private:
    bool ClipToPrevious(std::array<float, 16>& out) const;
    void EnsureNative(u32 width, u32 height);
    const Instance& instance;
    Scheduler& scheduler;
    std::array<float, 16> relative{}, projection{};
    bool got_slot0 = false, got_slot1 = false;
    ObjectMotion* object_motion = nullptr;
    std::array<float, 2> jitter{}, previous_jitter{};
    vk::UniqueDescriptorSetLayout desc_layout;
    vk::UniquePipelineLayout pipeline_layout;
    vk::UniquePipeline pipeline;
    std::array<std::array<float, 16>, 64> native_transforms{};
    u32 native_count = 0;
    vk::Buffer native_buffer{};
    VmaAllocation native_allocation{};
    VideoCore::UniqueImage native_image;
    vk::UniqueImageView native_view;
    u32 native_width = 0, native_height = 0;
    bool native_valid = false;
    vk::ImageLayout native_layout = vk::ImageLayout::eUndefined;
    vk::UniqueDescriptorSetLayout native_desc_layout;
    vk::UniquePipelineLayout native_pipeline_layout;
    std::array<vk::UniquePipeline, 2> native_pipelines;
};

} // namespace Vulkan
