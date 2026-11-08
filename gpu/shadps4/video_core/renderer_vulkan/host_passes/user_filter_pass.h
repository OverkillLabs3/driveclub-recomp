// SPDX-License-Identifier: GPL-2.0-or-later
// a post-processing filter from a mod: a SPIR-V fragment shader `postfx/<name>.spv` in a mods folder, drawn over
// the game's picture (the UI is in it) before the picture settings. The shader reads the picture at binding 0 and these
// push constants, and writes location 0:
//   layout(location = 0) in vec2 uv;
//   layout(binding = 0) uniform sampler2D picture;
//   layout(push_constant) uniform P { vec2 resolution; float time; float pad; vec4 values[4]; } p;
// `values` are the 16 numbers a mod sets with dcmod.postfx.param.
// A PNG with the name of the shader (postfx/<name>.png) is bound at binding 7 for lookup tables, noise and lens dirt.

#pragma once

#include <string>

#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"
#include "video_core/texture_cache/image.h"

namespace Vulkan::HostPasses {

class UserFilterPass {
public:
    struct Constants {
        float resolution[2];
        float time;
        float pad;
        float values[16];
        float has_history;   // 1 when binding 6 holds this slot's picture of the frame before
        float has_texture;   // 1 when binding 7 holds the filter's own PNG (postfx/<name>.png next to the shader)
        float texture_size[2];
    };

    /// What the HDR stage can also read (all in General layout): the view-space distance of every pixel, the motion
    /// vectors (pixels, previous minus current), the g-buffer and the view ray parameters. Without them (the stage
    /// on the finished picture) the picture itself stands in at each binding and `pad` is 0.
    struct Extras {
        vk::ImageView depth{}, motion{}, gbuffer{}, params{}, scene{};   // scene: the HDR picture before the first HDR slot
        bool valid = false;
    };

    void Create(vk::Device device, VmaAllocator allocator, vk::Format format, int slot = 0);

    /// `input` (ShaderReadOnlyOptimal) comes back when no filter is active; otherwise the filtered picture (also
    /// ShaderReadOnlyOptimal), drawn into an image of the same size.
    vk::ImageView Render(vk::CommandBuffer cmdbuf, vk::ImageView input, vk::Extent2D size,
                         vk::ImageLayout input_layout, const Extras& extras);
    vk::ImageView Render(vk::CommandBuffer cmdbuf, vk::ImageView input, vk::Extent2D size) {
        return Render(cmdbuf, input, size, vk::ImageLayout::eShaderReadOnlyOptimal, Extras{});
    }

    /// The image the last Render drew into (valid while Render returned its view).
    [[nodiscard]] vk::Image OutputImage() const {
        return vk::Image(images[last]);
    }
    /// True when a filter is chosen for this slot (whether or not it loaded).
    [[nodiscard]] bool WantsToRun() const;

private:
    bool LoadShader(const std::string& name);
    bool EnsureImage(vk::Extent2D size);
    void LoadTexture(const std::string& shader_path);
    void FreeTexture();

    int slot = 0;
    vk::Device device{};
    VmaAllocator allocator{};
    vk::Format format{};
    vk::UniqueSampler sampler{};
    vk::UniqueSampler nearest{};
    vk::UniqueDescriptorSetLayout desc_set_layout{};
    vk::UniquePipelineLayout pipeline_layout{};
    vk::UniqueShaderModule vertex_module{};
    vk::UniquePipeline pipeline{};
    std::string loaded_name;
    std::string loaded_path;
    long long loaded_stamp = 0;
    long long last_check_ms = 0;
    bool failed = false;

    // two pictures taking turns: one is drawn into while the other, the frame before, is read at binding 6
    VideoCore::UniqueImage images[2];
    vk::UniqueImageView image_views[2];
    vk::Extent2D image_size{};
    int last = 0;
    bool history_valid = false;

    // the filter's own picture (binding 7): a PNG next to the shader, uploaded on the first frame it is drawn
    vk::UniqueSampler repeat{};
    VideoCore::UniqueImage texture;
    vk::UniqueImageView texture_view;
    VkBuffer texture_staging{};
    VmaAllocation texture_allocation{};
    u32 texture_width = 0, texture_height = 0;
    bool texture_pending = false;
};

} // namespace Vulkan::HostPasses
