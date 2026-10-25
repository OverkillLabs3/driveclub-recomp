// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include "video_core/renderer_vulkan/liverpool_to_vk.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/texture_cache/sampler.h"

namespace VideoCore {

// bbport: BB_ANISO=0|2|4|8|16 (the launcher's Anisotropic filtering): colour samplers that
// filter linearly across mip levels get at least that much anisotropy even where the game
// asked for none. Roads and terrain at a shallow angle stay sharp. Depth compares and point
// samplers are left alone. 0 keeps the game's own. Default: 16x, 4x on integrated GPUs.
static int OverrideAnisotropy(const Vulkan::Instance& instance) {
    static const int override_af = [&] {
        const char* env = std::getenv("BB_ANISO");
        const int value = env ? std::atoi(env) : (instance.IsIntegrated() ? 4 : 16);
        return value == 2 || value == 4 || value == 8 || value == 16 ? value : 0;
    }();
    return override_af;
}

static bool ForceAnisotropy(const Vulkan::Instance& instance, const AmdGpu::Sampler& sampler, bool is_depth) {
    using namespace Vulkan;
    return OverrideAnisotropy(instance) && !is_depth && sampler.MaxLod() > sampler.MinLod() &&
           LiverpoolToVK::Filter(sampler.xy_min_filter) == vk::Filter::eLinear &&
           LiverpoolToVK::Filter(sampler.xy_mag_filter) == vk::Filter::eLinear;
}

bool UsesAnisotropy(const Vulkan::Instance& instance, const AmdGpu::Sampler& sampler, bool is_depth) {
    return instance.IsAnisotropicFilteringSupported() &&
           (ForceAnisotropy(instance, sampler, is_depth) || AmdGpu::IsAnisoFilter(sampler.xy_mag_filter) ||
            AmdGpu::IsAnisoFilter(sampler.xy_min_filter));
}

// isotropic: bbport, no anisotropy at all (BB_CUBE_LOD_RANGE=2: a sample at an explicit LOD reads one level).
Sampler::Sampler(const Vulkan::Instance& instance, const AmdGpu::Sampler& sampler,
                 const AmdGpu::BorderColorBuffer border_color_base, const bool is_depth, float extra_lod_bias,
                 bool isotropic) {
    using namespace Vulkan;
    const int override_af = OverrideAnisotropy(instance);
    const bool force_af = ForceAnisotropy(instance, sampler, is_depth);
    {
        // Proof in the log that the override does something: how many samplers it raised.
        static std::atomic<unsigned> seen{0}, forced{0}, already{0};
        const bool game_aniso = AmdGpu::IsAnisoFilter(sampler.xy_mag_filter) ||
                                AmdGpu::IsAnisoFilter(sampler.xy_min_filter);
        const unsigned n = ++seen;
        forced += force_af && !game_aniso;
        already += game_aniso;
        if (n == 1 || n == 50 || n == 500 || n == 5000) {
            std::printf("Samplers: %u created, anisotropy %dx forced on %u, the game asked for it on %u%s\n",
                        n, override_af, forced.load(), already.load(),
                        instance.IsAnisotropicFilteringSupported() ? "" : " (the GPU does not support it)");
        }
    }
    const bool anisotropy_enable = !isotropic && UsesAnisotropy(instance, sampler, is_depth);
    const float max_anisotropy =
        anisotropy_enable ? std::clamp(force_af ? std::max(float(override_af), sampler.MaxAniso())
                                                : sampler.MaxAniso(),
                                       1.0f, instance.MaxSamplerAnisotropy())
                          : 1.0f;
    auto border_color = LiverpoolToVK::BorderColor(sampler.border_color_type);
    if (border_color == vk::BorderColor::eFloatCustomEXT &&
        !instance.IsCustomBorderColorSupported()) {
        LOG_WARNING(Render_Vulkan, "Custom border color is not supported, falling back to black");
        border_color = vk::BorderColor::eFloatOpaqueBlack;
    }

    const auto custom_color = [&]() -> std::optional<vk::SamplerCustomBorderColorCreateInfoEXT> {
        if (border_color == vk::BorderColor::eFloatCustomEXT) {
            const auto border_color_index = sampler.border_color_ptr.Value();
            const auto border_color_buffer = border_color_base.Address<std::array<float, 4>*>();
            const auto custom_border_color_array = border_color_buffer[border_color_index];

            const vk::SamplerCustomBorderColorCreateInfoEXT ret{
                .customBorderColor =
                    vk::ClearColorValue{
                        .float32 = custom_border_color_array,
                    },
                .format = vk::Format::eR32G32B32A32Sfloat,
            };
            return ret;
        } else {
            return std::nullopt;
        }
    }();

    const vk::SamplerCreateInfo sampler_ci = {
        .pNext = custom_color ? &*custom_color : nullptr,
        .magFilter = LiverpoolToVK::Filter(sampler.xy_mag_filter),
        .minFilter = LiverpoolToVK::Filter(sampler.xy_min_filter),
        .mipmapMode = LiverpoolToVK::MipFilter(sampler.mip_filter),
        .addressModeU = LiverpoolToVK::ClampMode(sampler.clamp_x),
        .addressModeV = LiverpoolToVK::ClampMode(sampler.clamp_y),
        .addressModeW = LiverpoolToVK::ClampMode(sampler.clamp_z),
        .mipLodBias = std::clamp(sampler.LodBias() + extra_lod_bias, -instance.MaxSamplerLodBias(),
                                 instance.MaxSamplerLodBias()),
        .anisotropyEnable = anisotropy_enable,
        .maxAnisotropy = max_anisotropy,
        // GCN compares per instruction; a plain read of a compare sampler is undefined in Vulkan.
        .compareEnable = is_depth,
        .compareOp = LiverpoolToVK::DepthCompare(sampler.depth_compare_func),
        .minLod = sampler.MinLod(),
        .maxLod = sampler.MaxLod(),
        .borderColor = border_color,
        .unnormalizedCoordinates = false, // Handled in shader due to Vulkan limitations.
    };
    auto [sampler_result, smplr] = instance.GetDevice().createSamplerUnique(sampler_ci);
    ASSERT_MSG(sampler_result == vk::Result::eSuccess, "Failed to create sampler: {}",
               vk::to_string(sampler_result));
    handle = std::move(smplr);
}

Sampler::~Sampler() = default;

} // namespace VideoCore
