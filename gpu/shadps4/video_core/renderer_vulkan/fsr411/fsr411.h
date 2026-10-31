// SPDX-License-Identifier: GPL-2.0-or-later
// FSR 4.1.2 on Vulkan: the native AMD runtime when supported, otherwise the captured INT8
// shaders and weights in gpu/third_party/fsr412. The replay retains its Fsr411 namespace.
//
// Plain Vulkan (no renderer types) so that the game (vk_fsr4.cpp) and the benchmark
// (tools/fsr4_bench.cpp) share it. Frames record into the caller's command buffer; the caller
// keeps the images in layout General and makes sure that the frame recorded kFramesInFlight
// frames earlier has completed before recording a new one (constant buffer ring).

#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include <vulkan/vulkan.h>

namespace Fsr411 {

constexpr uint32_t kFramesInFlight = 8;

struct Image {
    VkImage image = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE; ///< depth: a view of the depth aspect
    uint32_t width = 0, height = 0;
};

struct Frame {
    VkCommandBuffer cmdbuf = VK_NULL_HANDLE;
    Image color, depth, motion, output; ///< color RGBA16F, depth D32, motion RG16F (render pixels)
    uint32_t render_width = 0, render_height = 0;
    bool ultra_performance = false; ///< the second model (quality ratio 3)
    float jitter[2] = {};           ///< render pixels, as for FSR 3/4
    float motion_scale[2] = {1.0f, 1.0f};
    float pre_exposure = 1.0f;
    float sharpness = 0.0f; ///< 0..1
    bool sharpen = false;
    bool reset = false;
    bool auto_exposure = true;
    VkFormat color_format = VK_FORMAT_R16G16B16A16_SFLOAT;
    VkFormat depth_format = VK_FORMAT_D32_SFLOAT;
    float frame_ms = 16.6f;
    float near_plane = 0.1f, far_plane = 3000.0f, vertical_fov = 1.0f;
};

class Upscaler {
public:
    /// `dir`: the asset sets (t1080_m0 ... t2160_m1).
    Upscaler(VkPhysicalDevice physical, VkDevice device, std::string dir,
             bool mixed_float_dot = false);
    ~Upscaler();

    /// Records one upscale; false with Error() set when it cannot (assets, device, sizes).
    bool Record(const Frame& frame);
    [[nodiscard]] const std::string& Error() const noexcept;
    /// The asset set and sizes in use, for logs ("t2160_m0 2260x1272 -> 3840x2160").
    [[nodiscard]] std::string Describe() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

} // namespace Fsr411
