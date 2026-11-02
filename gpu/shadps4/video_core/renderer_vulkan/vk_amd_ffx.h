// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: AMD's own FSR 4.1 upscaler through the FidelityFX API of FSR SDK 2.4 (BB_UPSCALER=fsr_amd).
//
// AMD's signed Vulkan DLLs (amd_fidelityfx_loader_vk.dll and amd_fidelityfx_upscaler_vk.dll) are loaded at run
// time from BB_FFX_DIR, <runtime>/fsr or <user dir>/fsr; none is shipped. The model comes from the installed
// Radeon driver: the full INT8 model on RX 6000 and RX 7000, FP8 on RX 9000, a lighter INT8 model on the Ryzen AI
// laptop and handheld parts. On any other graphics card, or without the DLLs, creating the context fails and the
// caller falls back to the FSR 4 (INT8) models or FSR 3.1.
#pragma once

#include <array>
#include <memory>

#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"

namespace Vulkan {

class Instance;
class Scheduler;

class AmdFfxUpscaler {
public:
    AmdFfxUpscaler(const Instance& instance, Scheduler& scheduler);
    ~AmdFfxUpscaler();

    /// An image in layout General, written/read by compute shaders.
    struct Image {
        vk::Image image;
        vk::Format format;
        vk::ImageUsageFlags usage;
        u32 width, height;
    };
    struct Frame {
        vk::CommandBuffer cmdbuf;
        Image color, depth, motion, output;
        u32 render_width, render_height;
        std::array<float, 2> jitter;
        float frame_ms, near_plane, far_plane, vertical_fov;
        float sharpness;
        bool sharpen, reset, auto_exposure;
    };

    /// Records the upscale into `frame.cmdbuf`; the output stays in General. False when it cannot run (Problem()
    /// says why; missing DLLs, a missing model for this card and a context failure are permanent).
    bool Record(const Frame& frame);

    /// Why the last Record failed, or null. Fatal(): it will not run in this session.
    [[nodiscard]] const char* Problem() const noexcept;
    [[nodiscard]] bool Fatal() const noexcept;
    /// The version the driver's provider reports once the context exists ("4.1.2"), or empty.
    [[nodiscard]] const char* Version() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

} // namespace Vulkan
