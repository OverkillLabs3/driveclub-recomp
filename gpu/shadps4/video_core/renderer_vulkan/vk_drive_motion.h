// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <array>

#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"

namespace Vulkan {

class Instance;

/// Camera motion vectors for frame generation. The matrices are the game's own: the constants of
/// its temporal anti-aliasing dispatch (current and previous view-projection, projection).
class DriveMotion {
public:
    explicit DriveMotion(const Instance& instance);

    /// A constant buffer of the anti-aliasing dispatch: slot 0 (104 dwords) or slot 1 (128).
    void OnTaaConstants(u32 slot, const float* data, u32 dwords);

    /// Both slots of this frame arrived since the last Consume.
    [[nodiscard]] bool Ready() const noexcept {
        return got_slot0 && got_slot1;
    }
    void Consume() noexcept {
        got_slot0 = got_slot1 = false;
    }

    /// Records the motion pass: `depth_view` (depth, General) into `motion_view` (RG16F, General).
    /// `mask_view` is the stencil of the same depth (the player's car moves with the camera), or
    /// `depth_view` itself when there is none. VR multiview: the views are one eye's layer, `eye_shift` its clip x shift.
    void Record(vk::CommandBuffer cmdbuf, vk::ImageView depth_view, vk::ImageView mask_view,
                vk::ImageView motion_view,
                u32 width, u32 height, u32 src_width, u32 src_height, bool normalized,
                vk::ImageView object_view = {}, vk::ImageView color_view = {}, float eye_shift = 0.0f);

    /// VR with alternate frames: the motion goes back two frames, to the same eye's last frame (the previous one is
    /// the other eye's): the game's moves of this frame and the last are chained.
    void SetTwoBack(bool value) noexcept {
        two_back = value;
    }
    /// The chained move is known (two frames of the game's constants).
    [[nodiscard]] bool TwoBackReady() const noexcept {
        return frames_kept >= 2;
    }
    /// The frame's motion is recorded: its move and projection become the last frame's (two_back). Once per frame:
    /// other passes write projections to slot 1 as well, only the last one before the anti-aliasing is the frame's.
    void EndFrame() noexcept;

    /// Row-vector clip space matrices for the frame generator.
    void ClipTransforms(std::array<float, 16>& view_to_clip,
                        std::array<float, 16>& clip_to_prev_clip,
                        std::array<float, 16>& prev_clip_to_clip) const;
    [[nodiscard]] float Near() const noexcept;
    [[nodiscard]] float VerticalFov() const noexcept;

private:
    /// chain: two frames back (two_back), the motion pass of DriveAA only.
    bool ClipToPrevious(std::array<float, 16>& out, bool chain) const;
    std::array<float, 16> relative{}, projection{};
    // The last frame's move and the projections of the last two frames (two_back).
    std::array<float, 16> previous_relative{}, previous_projection{}, projection_two_back{};
    int frames_kept = 0;
    bool two_back = false;
    bool got_slot0 = false, got_slot1 = false;
    vk::UniqueDescriptorSetLayout desc_layout;
    vk::UniquePipelineLayout pipeline_layout;
    vk::UniquePipeline pipeline;
};

} // namespace Vulkan
