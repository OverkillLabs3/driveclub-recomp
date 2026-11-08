// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <deque>
#include <memory>

#include <condition_variable>

#include "core/libraries/videoout/buffer.h"
#include "video_core/renderer_vulkan/host_passes/fsr_pass.h"
#include "video_core/renderer_vulkan/host_passes/hdr_pass.h"
#include "video_core/renderer_vulkan/host_passes/pp_pass.h"
#include "video_core/renderer_vulkan/host_passes/user_filter_pass.h"
#include "video_core/renderer_vulkan/vk_frame_generation.h"
#include "video_core/renderer_vulkan/vk_neural_rendering.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_runtime.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_swapchain.h"
#include "video_core/texture_cache/texture_cache.h"

namespace Frontend {
class WindowSDL;
}

namespace AmdGpu {
struct Liverpool;
}

namespace Vulkan {

struct Frame {
    u32 width;
    u32 height;
    VmaAllocation allocation;
    vk::Image image;
    vk::ImageView image_view;
    vk::Fence present_done;
    vk::Semaphore ready_semaphore;
    u64 ready_tick;
    Scheduler* source_scheduler = nullptr; ///< whose timeline ready_tick belongs to
    /// bbport: the identity NVIDIA Reflex attributes this frame's input, simulation, render and
    /// present markers to. Captured when the guest's flip is accepted, not read later on a worker.
    u64 reflex_frame_id{};
    /// The same identity for the CPU-side latency trace, kept separate so a mode change cannot
    /// make a driver report and a local sample disagree about which frame they describe.
    u64 content_frame_id{};
    /// Monotonic accepted-flip ticket the bounded backlog registers and retires by. It is not the
    /// content identity: a content id can repeat across accepted flips, which would make the
    /// backlog undercount its outstanding depth and stall the guest.
    u64 flip_ticket{};
    bool is_hdr{false};
    u8 id{};

    /// bbport: HDR output of the game's SDR picture (HdrPass): what is presented in its place.
    VmaAllocation hdr_allocation{};
    vk::Image hdr_image{};
    vk::ImageView hdr_view{};
    bool hdr_ready{false};

    /// bbport: frame generation: frames between the previous game frame and this one,
    /// presented before it (FrameGenerator), the first `generated_count` of them.
    struct Generated {
        VmaAllocation allocation{};
        vk::Image image{};
        vk::ImageView view{};
    };
    std::array<Generated, FrameGenerator::MaxGenerated> generated{};
    u32 generated_count{};
};

enum SchedulerType {
    Draw,
    Present,
    CpuFlip,
};

class Rasterizer;

class Presenter {
public:
    Presenter(Frontend::WindowSDL& window, AmdGpu::Liverpool* liverpool);
    ~Presenter();

    HostPasses::PostProcessingPass::Settings& GetPPSettingsRef() {
        return pp_settings;
    }

    HostPasses::FsrPass::Settings& GetFsrSettingsRef() {
        return fsr_settings;
    }

    Frontend::WindowSDL& GetWindow() const {
        return window;
    }

    Rasterizer& GetRasterizer() const {
        return *rasterizer.get();
    }

    bool IsHDRSupported() const {
        return swapchain.HasHDR();
    }

    void SetHDR(bool enable) {
        // bbport: with HDR output the swapchain stays HDR10 whatever the game's output mode
        // (it asks for SDR every frame).
        if (!IsHDRSupported() || hdr_output) {
            return;
        }
        swapchain.SetHDR(enable);
        pp_settings.hdr = enable ? 1 : 0;
    }

    VideoCore::Image& RegisterVideoOutSurface(
        const Libraries::VideoOut::BufferAttributeGroup& attribute, VAddr cpu_address) {
        vo_buffers_addr.emplace_back(cpu_address);
        auto desc = VideoCore::TextureCache::ImageDesc{attribute, cpu_address};
        const auto image_id = texture_cache.FindImage(desc);
        auto& image = texture_cache.GetImage(image_id);
        image.usage.vo_surface = 1u;
        return image;
    }

    bool IsVideoOutSurface(const AmdGpu::ColorBuffer& color_buffer) const;

    /// bbport: `content_frame_id` is the identity captured when the guest's flip was accepted.
    /// It becomes the frame's Reflex identity instead of the live thread-local counter.
    Frame* PrepareFrame(const Libraries::VideoOut::BufferAttributeGroup& attribute,
                        VAddr cpu_address, u64 content_frame_id);

    /// The present thread has no accepted flip and keeps the live identity; the flip path passes
    /// the accepted identity explicitly, including zero at startup.
    Frame* PrepareBlankFrame(bool present_thread, u64 content_frame_id = 0);

    void Present(Frame* frame, bool is_reusing_frame = false, bool is_game_frame = true);
    Frame* PrepareLastFrame();

private:
    Frame* GetRenderFrame();

    void RecreateFrame(Frame* frame, u32 width, u32 height);
    /// bbport: the first `count` images frame generation writes for `frame` (its size), made
    /// when missing; the others are freed. Returns how many there are.
    u32 EnsureGenerated(Frame* frame, u32 count);
    void DestroyGenerated(Frame* frame, u32 from = 0);
    /// bbport: frame generation is off (or the output HDR): its memory is freed.
    void ReleaseFrameGen(Frame* frame);
    /// bbport: present thread, after a game frame: how many generated frames the display can
    /// show per game frame (frame_gen_limit). `work_ms`: what presentation itself cost, with the
    /// pacing sleeps and the wait for the source on the GPU left out.
    void UpdateFrameGenLimit(bool wanted, double present_ms);
    /// bbport: blits `image` (General layout) into a swapchain image with the overlay and
    /// presents it. `last`: the game frame itself, which also signals the frame's fence.
    /// False when the swapchain could not give an image.
    bool PresentImage(Frame* frame, vk::Image image, bool last, bool notify_launcher = false);

    void SetExpectedGameSize(s32 width, s32 height);

private:
    float expected_ratio{1920.0 / 1080.0f};
    u32 expected_frame_width{1920};
    u32 expected_frame_height{1080};

    Frontend::WindowSDL& window;
    Instance instance;
    HostPasses::FsrPass fsr_pass;
    HostPasses::FsrPass::Settings fsr_settings{};
    HostPasses::PostProcessingPass::Settings pp_settings{};
    HostPasses::PostProcessingPass pp_pass;
    HostPasses::UserFilterPass user_filter_pass[3]; // the post-processing filters of a mod (postfx/<name>.spv), in order
    /// bbport: BB_HDR=1 with the display in HDR mode: the swapchain is HDR10 and each frame goes
    /// through hdr_pass (the game itself makes an SDR picture and never asks for HDR).
    bool hdr_output{false};
    HostPasses::HdrPass hdr_pass;
    /// The game's own HDR output (the swapchain in HDR for a game that asked for it).
    bool GameHdr() const {
        return swapchain.GetHDR() && !hdr_output;
    }
    AmdGpu::Liverpool* liverpool;
    Scheduler draw_scheduler;
    std::deque<u64> recent_frame_ticks; ///< bbport: BB_FRAMES_AHEAD bound (PrepareFrame)
    Scheduler present_scheduler;
    Scheduler flip_scheduler;
    Swapchain swapchain;
    Runtime runtime;
    std::unique_ptr<Rasterizer> rasterizer;
    VideoCore::TextureCache& texture_cache;
    vk::UniqueCommandPool command_pool;
    std::vector<Frame> present_frames;
    std::queue<Frame*> free_queue;
    Frame* last_submit_frame;
    std::mutex free_mutex;
    std::condition_variable free_cv;
    std::condition_variable_any frame_cv;
    std::vector<VAddr> vo_buffers_addr;
    /// bbport: frame generation, and the interval of the game's frames its frames are paced
    /// in (present thread).
    std::unique_ptr<FrameGenerator> frame_generator;
    std::unique_ptr<NeuralRendering> neural; // optional DLSS neural rendering on the final frame, off by default
    /// The game's picture before its UI, through the same passes as the frame.
    Frame hudless_frame{};
    /// When the last game frame reached the presenter: the interval the pacer works from.
    std::chrono::steady_clock::time_point last_game_present{};
    std::atomic<double> game_frame_ms{16.7}; ///< written by the present thread
    /// Generated frames per game frame the display can show (UpdateFrameGenLimit).
    std::atomic<u32> frame_gen_limit{0};
    // Present thread: the share of the frame interval spent presenting, the pause after
    // presenting fell behind, and whether the swapchain was made for frame generation.
    double present_busy{};
    int frame_gen_hold_seconds{5};
    std::chrono::steady_clock::time_point frame_gen_hold_until{};
    bool swapchain_frame_gen{};
    /// Present thread: an interpolation group is between its ordinary PRESENT markers, so the
    /// images inside it (the real one included) are attributed out of band.
    bool reflex_grouped{};
    // Present thread: the game frames of the current second, and the seconds with room for
    // one more generated frame.
    std::chrono::steady_clock::time_point limit_window{};
    u32 limit_frames{};
    u32 limit_up_seconds{};
};

} // namespace Vulkan
