// SPDX-License-Identifier: GPL-2.0-or-later
// Changed for this port. The upstream sources of the vendored tree are listed in
// gpu/VENDOR.txt and in NOTICE.
//
// Reads the real size of the half resolution depth buffer in the game's depth conversion shader.
//
// That shader compares against a hard coded 960x540. Above 1080p the buffer is larger, so most of it is
// never filled. The method (replace the two constants with a read of the image descriptor's width and
// height, and make the two compares inclusive because the descriptor stores size minus one) comes from the
// "DC Res Patch Depth Fix" tool by Kravickas (MIT license), which edits the game's data files. Here the same
// change is made to a copy of the shader code just before it is translated, so no game file is touched.
#pragma once

#include <span>
#include <vector>
#include "common/types.h"

namespace Shader::Gcn {

/// Copies `code` into `patched` with the fix applied. Returns false (and leaves `patched` empty) when
/// the shader is not the depth conversion shader.
bool ApplyDepthSizeFix(std::span<const u32> code, std::vector<u32>& patched);

} // namespace Shader::Gcn

namespace Shader::Gcn {

/// The same fix applied to the translated SPIR-V module: the 960 and 540 bounds are replaced by the
/// destination image's queried size. Returns true when `spv` was the depth conversion pass and was changed.
bool ApplyDepthSizeFixSpirv(std::vector<u32>& spv);

} // namespace Shader::Gcn
