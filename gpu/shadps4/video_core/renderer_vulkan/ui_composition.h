// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace Vulkan::UiComposition {

enum class Background { None, Copy, Temporal };

// UI composition must not require scene depth/camera or an active FSR context.
constexpr Background Choose(bool scaled, bool ui_draw, bool scene_ready, bool fsr_active) {
    if (!scaled || !ui_draw) {
        return Background::None;
    }
    return scene_ready && fsr_active ? Background::Temporal : Background::Copy;
}

inline bool NativeViewport(float width, float height) {
    const float w = std::abs(width);
    const float h = std::abs(height);
    if (std::abs(w - 1920.0f) < 0.5f && std::abs(h - 1080.0f) < 0.5f) return true;
    if (std::abs(w - 1920.0f) < 0.5f && h >= 1000.0f && h <= 1500.0f) return true;
    if (std::abs(h - 1080.0f) < 0.5f && w >= 1400.0f && w <= 3840.0f) return true;
    return false;
}

// Scaleform draws, including the first stencil/movie pass. A native-size viewport
// alone also matches every fullscreen post pass when guest targets stay at 1080p.
constexpr bool MovieShader(uint64_t hash) {
    return hash == 0x34e8a281 || hash == 0x81d336ce || hash == 0x09957251 ||
           hash == 0x24042a9b || hash == 0xa400228b;
}

inline std::array<float, 2> Scale(uint32_t guest_width, uint32_t guest_height,
                                uint32_t output_width, uint32_t output_height,
                                bool native_coordinates) {
    if (native_coordinates) {
        const float uniform_scale = std::min(float(output_width) / 1920.0f,
                                            float(output_height) / 1080.0f);
        return {uniform_scale, uniform_scale};
    }
    return {float(output_width) / float(guest_width),
            float(output_height) / float(guest_height)};
}

} // namespace Vulkan::UiComposition
