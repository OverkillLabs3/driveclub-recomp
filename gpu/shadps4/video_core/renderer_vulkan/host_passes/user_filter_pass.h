// SPDX-License-Identifier: GPL-2.0-or-later
// a post-processing filter from a mod: a SPIR-V fragment shader `postfx/<name>.spv` in a mods folder, drawn over
// the game's picture (the UI is in it) before the picture settings. The shader reads the picture at binding 0 and these
// push constants, and writes location 0:
//   layout(location = 0) in vec2 uv;
//   layout(binding = 0) uniform sampler2D picture;
//   layout(push_constant) uniform P { vec2 resolution; float time; float pad; vec4 values[4]; } p;
// `values` are the 16 numbers a mod sets with dcmod.postfx.param.

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
    };

    void Create(vk::Device device, VmaAllocator allocator, vk::Format format, int slot = 0);

    /// `input` (ShaderReadOnlyOptimal) comes back when no filter is active; otherwise the filtered picture (also
    /// ShaderReadOnlyOptimal), drawn into an image of the same size.
    vk::ImageView Render(vk::CommandBuffer cmdbuf, vk::ImageView input, vk::Extent2D size);

private:
    bool LoadShader(const std::string& name);
    bool EnsureImage(vk::Extent2D size);

    int slot = 0;
    vk::Device device{};
    VmaAllocator allocator{};
    vk::Format format{};
    vk::UniqueSampler sampler{};
    vk::UniqueDescriptorSetLayout desc_set_layout{};
    vk::UniquePipelineLayout pipeline_layout{};
    vk::UniqueShaderModule vertex_module{};
    vk::UniquePipeline pipeline{};
    std::string loaded_name;
    std::string loaded_path;
    long long loaded_stamp = 0;
    long long last_check_ms = 0;
    bool failed = false;

    VideoCore::UniqueImage image;
    vk::UniqueImageView image_view;
    vk::Extent2D image_size{};
};

} // namespace Vulkan::HostPasses
