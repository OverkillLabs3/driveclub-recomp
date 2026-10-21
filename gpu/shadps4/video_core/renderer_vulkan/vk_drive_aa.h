// SPDX-License-Identifier: GPL-2.0-or-later
// Changed for this port. The upstream sources of the vendored tree are listed in
// gpu/VENDOR.txt and in NOTICE.
//
// Anti-aliasing for DriveClub through FSR 3.1 or DLSS (DLAA) at the render resolution. The game's
// own temporal anti-aliasing pass runs as before; right after it the lit HDR picture it read is
// reconstructed by FSR or DLSS (jittered geometry, depth, camera motion) and the result replaces
// the pass's output.
#pragma once

#include <array>
#include <chrono>
#include <memory>

#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"
#include "video_core/texture_cache/image.h"

struct FfxVkPortableUpscaleContext;

namespace VideoCore {
class TextureCache;
}

namespace Vulkan {

class Instance;
class Scheduler;
class Runtime;
class DriveMotion;

class DriveAA {
public:
    DriveAA(const Instance& instance, Scheduler& scheduler, VideoCore::TextureCache& texture_cache,
            Runtime& runtime, DriveMotion& motion);
    ~DriveAA();

    /// FSR or DLSS anti-aliasing is switched on and has not failed.
    [[nodiscard]] bool Active() const;

    /// Sub-pixel shift of this frame's scene geometry in render pixels (x right, y down).
    [[nodiscard]] std::array<float, 2> Jitter() const {
        return Active() ? jitter : std::array<float, 2>{};
    }

    /// Upscaling (render size below the output size) is on.
    [[nodiscard]] bool Upscaling() const;
    /// Texture LOD bias for the scene's material textures while upscaling: log2(render / output).
    [[nodiscard]] float MipBias() const;

    /// While upscaled: later reads of the game's anti-aliasing result by normalized sampling see
    /// the upscaled picture instead (its passes sample by UV, so the size does not matter).
    [[nodiscard]] bool Redirects(VideoCore::ImageId id) const {
        return redirect_valid && id == redirect_id;
    }
    void Redirect(vk::ImageView& view, vk::ImageLayout& layout) const {
        view = *result_view;
        layout = vk::ImageLayout::eGeneral;
    }

    /// The display pass: the next frame's jitter.
    void NextFrame();

    /// After the game's anti-aliasing dispatch: `color` is the lit HDR picture it read, `depth` the
    /// scene depth and `output` the image it wrote.
    void OnTaa(VideoCore::ImageId color, VideoCore::ImageId depth, VideoCore::ImageId output);

private:
    struct CachedView {
        vk::Image image;
        u64 uid = 0;
        vk::Format format{};
        vk::ImageAspectFlags aspect{};
        vk::ImageView view;
        u64 last_use = 0;
    };
    vk::ImageView View(const VideoCore::Image& image, vk::Format format,
                       vk::ImageAspectFlags aspect);
    bool EnsureResources(u32 width, u32 height, u32 out_width, u32 out_height, bool dlss);
    void CreatePipeline();

    const Instance& instance;
    Scheduler& scheduler;
    VideoCore::TextureCache& texture_cache;
    Runtime& runtime;
    DriveMotion& motion;

    std::array<CachedView, 8> views{};
    u64 view_uses = 0;

    u32 width = 0, height = 0, out_width = 0, out_height = 0;
    bool redirect_valid = false;
    VideoCore::ImageId redirect_id{};
    float scale = 1.0f;
    bool ready = false;
    bool failed = false;
    std::chrono::steady_clock::time_point last_run{};
    bool reset = true;
    int applied_mode = -1;
    u32 frame_id = 0;
    u32 jitter_index = 0;
    std::array<float, 2> jitter{};
    std::chrono::steady_clock::time_point last_frame{};

    FfxVkPortableUpscaleContext* context = nullptr;
    VideoCore::UniqueImage motion_image, result_image;
    vk::UniqueImageView motion_view, result_view;
    vk::UniqueDescriptorSetLayout copy_desc_layout;
    vk::UniquePipelineLayout copy_pipeline_layout;
    vk::UniquePipeline copy_pipeline;
};

} // namespace Vulkan
