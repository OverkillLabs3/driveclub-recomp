// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: AMD Anti-Lag 2 (VK_AMD_anti_lag). The driver delays the start of a frame's CPU work so the input it reads is
// as fresh as the GPU allows: Input() marks where the game samples its controls, Present() marks the real game frame
// reaching the display. Off with BB_ANTI_LAG=0; on its own on Radeon cards that offer the extension.
#pragma once

#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"

namespace Vulkan::AntiLag {

/// `enabled`: the device was created with the extension. Looks up the driver's entry point.
void Initialize(vk::Device device, bool enabled);
/// The game samples its controls for a new frame (the guest thread).
void Input();
/// The game's own frame is presented (the present thread); `max_fps` is the frame cap, 0 for none.
void Present(u32 max_fps);
[[nodiscard]] bool Active();

} // namespace Vulkan::AntiLag
