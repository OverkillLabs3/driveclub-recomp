// SPDX-License-Identifier: GPL-2.0-or-later
// VR (dcport-vr): stereo rendering with Vulkan multiview (BB_VR_STEREO=mv).
//
// Main-view render targets carry a second array layer (the right eye; layer 0 stays the guest's image). Graphics passes
// into them run with view mask 0b11; compute passes that write them run once per eye with push data vr_eye. In this
// mode every guest shader declares its plain 2D images as 2D arrays and reads/writes layer `Layer()`: gl_ViewIndex in
// graphics stages, vr_eye in compute. One-layer images give both eyes their only layer (sampling clamps the layer;
// fetches and storage access clamp it explicitly). The hardware vertex shader adds vr_shift[view] to clip x: an eye
// moved sideways by e changes clip x by -P00 * e for every vertex (zero for draws that are not the player's camera).

#pragma once

#include "shader_recompiler/backend/spirv/spirv_emit_context.h"

namespace Shader::IR {
struct Program;
}

namespace Shader::Backend::SPIRV::Vr {

/// Multiview shaders are compiled (BB_VR_STEREO=mv; fixed for the session).
bool MultiviewShaders();

/// Whether an image of this view type is declared as a 2D array.
bool ArrayedImage(AmdGpu::ImageType view_type);

/// Per-eye buffers: guest storage buffers are declared as two-element descriptor arrays (element = the eye: the view
/// index, or vr_eye in compute). A buffer the game builds from one eye's picture (its tiled lighting data) has a copy
/// for the right eye there; every other binding gives the same buffer twice (video_core vr_multiview.h, EyeBufferInfo).
bool EyeBufferArrays();

/// At the start of main: each guest buffer alias points at the eye's element.
void EyeBuffers(EmitContext& ctx);

/// After the interfaces: the MultiView capability and the view index input (graphics stages).
void Define(EmitContext& ctx);

/// `coords` with the eye's layer appended when the image is a VR array (float for sampling, integer otherwise;
/// integer layers are clamped to the image's layers, `image` being the loaded image or, for atomics, null).
Id Coords(EmitContext& ctx, const EmitContext::TextureDefinition& texture, Id image, Id coords, bool integer);

/// End of the hardware vertex shader, in multiview pipelines (specialisation constant 200): the position's x plus
/// vr_shift[view] when w is not 1 (perspective geometry: the player's view in a main-view pass; full-screen passes draw
/// at w = 1 and stay put).
void Epilogue(EmitContext& ctx);

/// Before emission: a hardware vertex shader's outputs that hold the position's x / w (times a constant, plus one), the
/// screen position deferred light volumes pass on to find their pixel in the G-buffer. The epilogue moves them with the
/// shifted position (vr_shift[view] / w); otherwise each eye lights the G-buffer shift / w away from the pixel (the
/// cockpit's lights land in different places per eye).
///
/// Also the eye's translation for view-dependent outputs: the position is a matrix times a vector v (three IR values
/// common to x, y and w); the eye's offset e in v's own space solves M e = (-vr_shift, 0, *, 0) from the matrix's rows
/// (e along row1 x row3). The position and every output holding a division by something that depends on v (screen
/// positions, view rays p / z, projective light coordinates, normalised view vectors) are evaluated again with v - e:
/// deferred light volumes rebuild each pixel's position from the eye's own ray (headlights on the cars ahead sat
/// e * (d / z - 1) away per eye). Outputs affine in v (positions for world-space lookups) keep the centre's values.
/// Shaders where this cannot be done (other position shapes, branches, unsupported operations) keep the clip shift.
void ScanScreenParams(const IR::Program& program);

/// Geometry shaders: the same shift for each vertex they emit.
void BeforeEmitVertex(EmitContext& ctx);

} // namespace Shader::Backend::SPIRV::Vr
