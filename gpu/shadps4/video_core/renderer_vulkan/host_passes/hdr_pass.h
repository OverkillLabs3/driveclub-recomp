// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: HDR10 output for the game's SDR picture (hdr_output.frag): the frame at paper white, the
// 3D scene's highlights towards the display's peak from the game's linear scene, matched to the
// SDR picture's brightness (hdr_exposure.comp). BB_HDR, BB_HDR_PAPER, BB_HDR_PEAK.

#pragma once

#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"
#include "video_core/texture_cache/image.h"

namespace Vulkan::HostPasses {

class HdrPass {
public:
    struct Params {
        float paper_nits = 350.0f;
        float peak_nits = 1000.0f;
        float has_hudless = 0.0f;
        float knee = 0.75f;
        float has_scene = 0.0f;
    };

    void Create(vk::Device device, VmaAllocator allocator, vk::Format output_format);

    /// `frame`, `hudless` (the picture before the UI) and `scene` (the linear scene) are sampled
    /// (ShaderReadOnlyOptimal); hudless and scene may be null. `output` is drawn whole
    /// (ColorAttachmentOptimal).
    void Render(vk::CommandBuffer cmdbuf, vk::ImageView frame, vk::ImageView hudless,
                vk::ImageView scene, vk::ImageView output, vk::Extent2D size, Params params);

private:
    vk::UniquePipeline pipeline{};
    vk::UniquePipelineLayout pipeline_layout{};
    vk::UniqueDescriptorSetLayout desc_set_layout{};
    vk::UniquePipeline exposure_pipeline{};
    vk::UniquePipelineLayout exposure_layout{};
    vk::UniqueDescriptorSetLayout exposure_set_layout{};
    vk::UniqueSampler sampler{};
    /// 1x1 R32F, General: SDR white per scene unit, smoothed (0 until the first measure).
    VideoCore::UniqueImage state;
    vk::UniqueImageView state_view;
    bool state_cleared = false;
};

} // namespace Vulkan::HostPasses
