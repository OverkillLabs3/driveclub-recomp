// SPDX-License-Identifier: GPL-2.0-or-later
// Anti-aliasing and upscaling for DriveClub through FSR 3.1 or DLSS. At the render resolution
// (DLAA): the game's own temporal anti-aliasing pass runs as before; right after it the lit HDR
// picture it read is reconstructed by FSR or DLSS (jittered geometry, depth, camera motion) and
// the result replaces the pass's output. Upscaling (the runtime lowers the render size, see
// src/probe.c): the game's anti-aliasing is skipped, its later passes (transparents, bloom,
// motion blur) draw at the render size as before, and where the game scales its finished picture
// into the display buffer, FSR or DLSS upscales it to the output size instead.
#pragma once

#include <array>
#include <chrono>
#include <deque>
#include <string>
#include <memory>

#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"
#include "video_core/renderer_vulkan/vk_neural_rendering.h"
#include "video_core/texture_cache/image.h"

namespace Fsr411 { class Upscaler; }

namespace VideoCore {
class TextureCache;
}

namespace Vulkan {

class Instance;
class Scheduler;
class Runtime;
class DriveMotion;
class Fsr315;
class CameraMotion;

class DriveAA {
public:
    DriveAA(const Instance& instance, Scheduler& scheduler, VideoCore::TextureCache& texture_cache,
            Runtime& runtime, DriveMotion& motion, CameraMotion& camera_motion);
    ~DriveAA();

    /// FSR or DLSS anti-aliasing is switched on and has not failed.
    [[nodiscard]] bool Active() const;
    [[nodiscard]] int EffectiveMode() const;

    /// Sub-pixel shift of this frame's scene geometry in render pixels (x right, y down).
    [[nodiscard]] std::array<float, 2> Jitter() const {
        return Active() ? jitter : std::array<float, 2>{};
    }
    [[nodiscard]] std::array<float, 2> JitterForExtent(u32 w, u32 h) const {
        return Active() && jitter_width == w && jitter_height == h ? geometry_jitter : std::array<float, 2>{};
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
    /// `mapping`: the component swizzle of the view the game asked for (its T#'s destination
    /// select), which the upscaled picture is read with too.
    void Redirect(const vk::ComponentMapping& mapping, vk::ImageView& view, vk::ImageLayout& layout);

    [[nodiscard]] NeuralRendering::Guides FrameGuides();
    void EndFrame() {
        frame_guides = {};
        frame_depth = {};
        frame_depth_uid = 0;
        previous_jitter = applied_jitter;
        applied_jitter = {};
        jitter_width = width;
        jitter_height = height;
        geometry_jitter = jitter;
    }

    /// The display pass: the next frame's jitter.
    void NextFrame();

    /// After the game's anti-aliasing dispatch: `color` is the lit HDR picture it read, `depth` the
    /// scene depth and `output` the image it wrote.
    void OnTaa(VideoCore::ImageId color, VideoCore::ImageId depth, VideoCore::ImageId output, VideoCore::ImageId gbuffer = {},
               VideoCore::ImageId native_depth = {}, VideoCore::ImageId previous_ids = {});

    /// Upscaling: OnTaa left the picture for the pass that scales it into the display buffer, and
    /// `image` (sampled by the first draw into the display buffer) is that picture.
    [[nodiscard]] bool LatePending() const {
        return late_pending;
    }
    [[nodiscard]] bool WantsComposite(const VideoCore::Image& image) const;
    /// Upscales `color` to the output size; the draw then samples the result (Redirect).
    void OnComposite(VideoCore::ImageId color);

    /// The last upscaled picture (RGBA16F, General layout; the caller knows it was made this
    /// frame): HDR output reads the scene from it.
    [[nodiscard]] bool UpscaledResult(vk::Image& image, u32& w, u32& h) const {
        if (!ready || !result_image) {
            return false;
        }
        image = vk::Image(result_image);
        w = out_width;
        h = out_height;
        return true;
    }

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
    void EnsureMotionResources(u32 width, u32 height);
    void RecordGuides(VideoCore::ImageId depth, std::array<float, 2> frame_jitter,
                      vk::ImageView color_view = {});
    bool EnsureResources(u32 width, u32 height, u32 out_width, u32 out_height, bool dlss, bool fsr411_mode);
    /// FSR or DLSS on `color` (jittered by `frame_jitter`): at the render size into `output`,
    /// or upscaled to the output size and sampled in place of `color` (Redirect).
    void Reconstruct(VideoCore::ImageId color, VideoCore::ImageId depth, VideoCore::ImageId output,
                     std::array<float, 2> frame_jitter, bool upscale);
    void CreatePipeline();

    const Instance& instance;
    Scheduler& scheduler;
    VideoCore::TextureCache& texture_cache;
    Runtime& runtime;
    DriveMotion& motion;
    CameraMotion& camera_motion;
    std::unique_ptr<NeuralRendering> neural;

    std::array<CachedView, 8> views{};
    u64 view_uses = 0;

    u32 width = 0, height = 0, out_width = 0, out_height = 0;
    bool redirect_valid = false;
    VideoCore::ImageId redirect_id{};
    /// Upscaling: the scene of this frame waits for its composite (OnTaa to OnComposite).
    bool late_pending = false;
    VideoCore::ImageId late_depth{};
    std::array<float, 2> late_jitter{};
    u32 late_width = 0, late_height = 0;
    float scale = 1.0f;
    bool ready = false;
    bool failed = false;
    bool dlss_failed = false;
    std::chrono::steady_clock::time_point last_run{};
    bool reset = true;
    int applied_mode = -1;
    u32 frame_id = 0;
    u32 jitter_index = 0;
    u32 jitter_width = 0, jitter_height = 0;
    std::array<float, 2> jitter{}, previous_jitter{}, applied_jitter{}, geometry_jitter{};
    std::chrono::steady_clock::time_point last_frame{};

    std::unique_ptr<Fsr315> fsr315;
    std::unique_ptr<Fsr411::Upscaler> fsr411;
    std::deque<u64> fsr411_ticks;
    std::string fsr411_described;
    NeuralRendering::Guides frame_guides{};
    VideoCore::ImageId frame_depth{};
    u64 frame_depth_uid = 0;
    u32 motion_width = 0, motion_height = 0;
    VideoCore::UniqueImage motion_image, result_image;
    vk::UniqueImageView motion_view, result_view;
    /// DLSS writes here when the result is sharpened into result_image afterwards.
    VideoCore::UniqueImage raw_image;
    vk::UniqueImageView raw_view;
    vk::UniquePipeline sharpen_pipeline;
    /// Views of the result with the swizzles the game samples it with (Redirect).
    std::array<std::pair<vk::ComponentMapping, vk::UniqueImageView>, 4> swizzled_views{};
    vk::UniqueDescriptorSetLayout copy_desc_layout;
    vk::UniquePipelineLayout copy_pipeline_layout;
    vk::UniquePipeline copy_pipeline;
};

} // namespace Vulkan
