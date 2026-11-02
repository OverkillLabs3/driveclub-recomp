// Sizes the on-screen counter shows: the largest 16:9 render target drawn into over the last second that is not the
// output picture itself (with a 4K output that picture is the largest target whatever the render resolution).
#pragma once
#include <atomic>
#include <cstdint>

namespace BbFrame {
inline std::atomic<uint32_t> scene_w{0}, scene_h{0};
inline std::atomic<uint32_t> output_w{0}, output_h{0};  // the game's display buffer (what is presented)

inline void NoteOutput(uint32_t w, uint32_t h) {
    output_w.store(w, std::memory_order_relaxed);
    output_h.store(h, std::memory_order_relaxed);
}

// The size DriveAA reconstructs from: known to be the scene, so it counts even when it equals the output size.
inline void NoteScene(uint32_t w, uint32_t h) {
    if (uint64_t(w) * h > uint64_t(scene_w.load(std::memory_order_relaxed)) * scene_h.load(std::memory_order_relaxed)) {
        scene_w.store(w, std::memory_order_relaxed);
        scene_h.store(h, std::memory_order_relaxed);
    }
}

inline void NoteRender(uint32_t w, uint32_t h) {
    if (w == 0 || h == 0 || w > 8192 || h > 8192) {
        return;  // passes without a real extent report huge sizes
    }
    const uint32_t ow = output_w.load(std::memory_order_relaxed);
    const uint32_t oh = output_h.load(std::memory_order_relaxed);
    if (ow > 0 && oh > 0) {
        if (uint64_t(w) * oh * 100 < uint64_t(h) * ow * 95 ||
            uint64_t(w) * oh * 100 > uint64_t(h) * ow * 105) {
            return;  // atlases and cube faces are not the scene
        }
    } else if (w * 9 * 100 < h * 16 * 95 || w * 9 * 100 > h * 16 * 105) {
        return;  // atlases and cube faces are not the scene
    }
    if (w == ow && h == oh) {
        return;  // the finished picture (menus, HUD), not the scene; with nothing else drawn the overlay shows the output size
    }
    const uint64_t area = uint64_t(w) * h;
    if (area > uint64_t(scene_w.load(std::memory_order_relaxed)) * scene_h.load(std::memory_order_relaxed)) {
        scene_w.store(w, std::memory_order_relaxed);
        scene_h.store(h, std::memory_order_relaxed);
    }
}
} // namespace BbFrame
