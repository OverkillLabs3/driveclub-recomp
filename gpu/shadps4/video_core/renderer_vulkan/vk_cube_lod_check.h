// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "video_core/renderer_vulkan/vk_common.h"

namespace Vulkan {

class Instance;

/// bbport: BB_CUBE_LOD_RANGE_SELFTEST. Samples one random image shaped like the sampled view (its format and
/// swizzle, `levels` levels down to 1x1, 6 layers, every float finite) at integer explicit LODs 0..levels+1 and 65536
/// coordinates with both samplers, on the GPU, and compares the results bit for bit. Returns true when all are
/// identical; false (and says why) otherwise or when it cannot run (levels 1-10 only). `cube`: through a cube-array
/// view (native cube sampling) instead of a 2D array.
bool CubeLodSelfTest(const Instance& instance, vk::Sampler reference, vk::Sampler candidate, vk::Format format,
                     u32 levels, const vk::ComponentMapping& mapping, const char* what, bool cube);

} // namespace Vulkan
