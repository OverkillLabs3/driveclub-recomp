// SPDX-License-Identifier: GPL-2.0-or-later
// VR: fixed foveated rendering (vr.ini ffr = 1). The lenses blur the edges of each eye's picture, and a flat picture
// spends most of its pixels there: the scene passes at the render size shade the middle at full rate and the edges
// at 2x2 and 4x4 (VK_KHR_fragment_shading_rate, an attachment of one rate per 16x16 pixels, each eye's layer).
// Geometry edges and depth keep the full resolution; only the pixel shaders run less often.

#pragma once

#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"

namespace Vulkan {
class Instance;
class Scheduler;
}

namespace Vulkan::Vr::Foveation {

/// Device creation: VR (true stereo or AFR) with ffr in vr.ini, before the device is made (the extension is then enabled).
bool Wanted();
/// After the device: whether the attachment rate works (its feature, the texel size the map uses).
void OnDevice(bool supported, u32 texel_width, u32 texel_height);
/// The device has it on: the multiview pipelines are made able to take the map.
bool Supported();

/// Before a two-eye scene pass of `width` x `height`: the map's view (made and filled on first use, again when the
/// zones change), or null with foveation off (vr.ini ffr = 0 or its key). Records outside the render pass.
vk::ImageView Prepare(const Instance& instance, Scheduler& scheduler, u32 width, u32 height);
/// The map's texel size (pixels per rate).
vk::Extent2D TexelSize();

} // namespace Vulkan::Vr::Foveation
