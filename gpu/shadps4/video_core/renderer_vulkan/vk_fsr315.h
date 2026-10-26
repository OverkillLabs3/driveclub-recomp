// SPDX-License-Identifier: GPL-2.0-or-later
// FSR 3.1.5 (FidelityFX SDK 2.3) upscaling through the vendored Vulkan bridge: the SDK's
// scheduler owns the pipelines and its own internal images, the caller supplies the frame
// images and the three shared temporal resources. The bridge creates temporary views on import
// and restores each image's layout after a dispatch, so every image stays in General here.
#pragma once

#include <array>
#include <deque>
#include <memory>
#include <string>

#include "common/types.h"
// vk_common enables the beta Vulkan extensions that the C++ headers expect; the C bridge header
// pulls vulkan.h and must come after it.
#include "video_core/renderer_vulkan/vk_common.h"
#include "ffx_vk_fsr3_3_1_5_bridge.h"
#include "video_core/texture_cache/image.h"

namespace Vulkan {

class Instance;
class Scheduler;

class Fsr315 {
public:
    struct Frame {
        vk::CommandBuffer command;
        vk::Image color;
        vk::Format color_format;
        vk::Image depth;
        vk::Format depth_format;
        vk::Image motion;
        vk::Image output;
        u32 width = 0, height = 0, out_width = 0, out_height = 0;
        float jitter_x = 0.0f, jitter_y = 0.0f;
        float frame_ms = 16.6f;
        float sharpness = 0.0f;
        bool sharpen = false;
        bool reset = false;
        float near_plane = 0.1f;
        float far_plane = 3000.0f;
        float vertical_fov = 1.0f;
    };

    Fsr315(const Instance& instance, Scheduler& scheduler);
    ~Fsr315();
    Fsr315(const Fsr315&) = delete;
    Fsr315& operator=(const Fsr315&) = delete;

    /// Creates the context, pipelines and the three shared temporal images. The GPU must be idle.
    bool Create(u32 render_width, u32 render_height, u32 out_width, u32 out_height);
    /// Waits for the GPU and releases everything. Safe to call repeatedly.
    void Destroy();
    /// Records one upscale. The images must be in General layout; the caller submits.
    bool Record(const Frame& frame);
    [[nodiscard]] bool Ready() const {
        return context != nullptr;
    }
    [[nodiscard]] const char* Problem() const {
        return problem.empty() ? nullptr : problem.c_str();
    }

private:
    struct SharedImage {
        VideoCore::UniqueImage image;
        vk::Format format = vk::Format::eUndefined;
        u32 width = 0, height = 0;
    };
    struct InFlight {
        u64 frame_id = 0;
        u64 tick = 0;
        /// color, depth, motion, output, dilated depth, dilated motion, previous depth.
        std::array<FfxVkFsr3_3_1_5Resource, 7> tokens{};
    };

    bool CreateShared(const FfxVkFsr3_3_1_5SharedResourceDescription& description,
                      SharedImage& out);
    FfxVkFsr3_3_1_5Resource Import(vk::Image image, vk::Format format, u32 width, u32 height,
                                   u32 state, bool storage, bool undefined_contents);
    void Release(FfxVkFsr3_3_1_5Resource& resource);
    void Retire();
    void ReleaseAll();
    void Fail(const char* what, int result);

    const Instance& instance;
    Scheduler& scheduler;
    FfxVkFsr3_3_1_5UpscalerContext* context = nullptr;
    FfxVkFsr3_3_1_5Bridge* bridge = nullptr;
    SharedImage dilated_depth, dilated_motion, previous_depth;
    bool shared_transitioned = false;
    bool shared_initialized = false;
    u64 next_frame = 1;
    std::deque<InFlight> in_flight;
    std::string problem;
};

} // namespace Vulkan
