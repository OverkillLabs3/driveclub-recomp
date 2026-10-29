// SPDX-License-Identifier: GPL-2.0-or-later
// VR: true stereo rendering with Vulkan multiview (BB_VR_STEREO=mv). Ported from hookmanuk's dcport-vr fork
// (0.2.5-vr1.3). See also
// shader_recompiler/backend/spirv/emit_spirv_vr.h (the shader side).
//
// Main-view images (2D, one layer, one sample, at the game's render size or a power-of-two fraction of it; mip chains too)
// get a second array layer: the right eye. Layer 0 stays the guest's image (uploads, downloads, the presenter). A
// render pass whose attachments all have two layers draws with view mask 0b11 (each graphics pipeline has a multiview
// twin, bound for such passes); guest views of plain 2D images are 2D arrays (shaders read layer = view). In the twins,
// perspective vertices get a clip-space x shift per view (the eyes either side of the player's camera); compute
// dispatches that write a two-layer storage image run again for the right eye (push data vr_eye = 1).

#pragma once

#include <array>
#include <vector>
#include <optional>
#include <span>

#include "common/types.h"
#include "shader_recompiler/resource.h"
#include "video_core/amdgpu/resource.h"
#include "video_core/renderer_vulkan/vk_common.h"
#include "video_core/texture_cache/image.h"

namespace VideoCore {
struct Image;
struct ImageInfo;
struct ImageViewInfo;
} // namespace VideoCore

namespace Vulkan {
class Instance;
class Runtime;
class Scheduler;
}

namespace Vulkan::Vr::Multiview {

/// BB_VR_STEREO=mv was asked for (the device then enables multiview, the shaders are compiled for it).
bool Requested();

/// BB_VR_STEREO=mv on a device with multiview (fixed once the device is made).
bool Enabled();

/// The pipeline cache directory's suffix: multiview shaders are cached apart ("_mv").
const char* CacheSuffix();

/// Device creation: whether the device has (and now enables) multiview.
void OnDevice(VkDevice device, bool supported);

/// Image creation: the backing's array layers (2 for main-view images).
u32 BackingLayers(const VideoCore::ImageInfo& info, u32 layers);

/// The image has the right eye's layer.
bool Stereo(const VideoCore::Image& image);

/// Guest views of this type are 2D arrays (the shaders declare them so).
bool ArrayView(AmdGpu::ImageType type);

/// View creation: a guest 2D view becomes a 2D array, over both layers of a two-layer image.
void AdjustView(const VideoCore::Image& image, const VideoCore::ImageViewInfo& info, vk::ImageViewType& type,
                u32& layer_count);

/// A render pass's view mask: 0b11 when every attachment has two layers (`images`, null entries skipped).
u16 ViewMask(const VideoCore::Image* const* images, u32 count);

/// ViewMask of a pass's colour targets (texture cache ids in `colors[i].first`) and depth target.
template <typename Cache, typename Colors, typename Depth>
u16 PassViewMask(Cache& cache, const Colors& colors, u32 num_colors, const Depth& depth) {
    if (!Enabled()) {
        return 0;
    }
    const VideoCore::Image* images[16]{};
    u32 count = 0;
    for (u32 i = 0; i < num_colors && count < 15; ++i) {
        if (colors[i].first) {
            images[count++] = &cache.GetImage(colors[i].first);
        }
    }
    if (depth.first) {
        images[count++] = &cache.GetImage(depth.first);
    }
    return ViewMask(images, count);
}

/// Graphics pipeline creation: builds the multiview twin of `pipeline`.
void OnPipelineCreated(vk::Device device, vk::PipelineCache cache, const vk::GraphicsPipelineCreateInfo& info,
                       const vk::PipelineRenderingCreateInfo& rendering, vk::Pipeline pipeline);

/// The pipeline to bind for a pass with `view_mask`.
vk::Pipeline Variant(vk::Pipeline pipeline, u16 view_mask);

/// Each frame: the eyes' clip x shift (left eye +shift, right eye -shift; P00 x half the eye distance).
void SetShift(float shift);
/// Deferred spot lights (vs 0xf719615a with pixel shaders 0xd80f041b, 0x7b2b16ed): the pixel shader rebuilds the lit
/// point as light constants plus the view ray times the depth (light-relative: dwords 40-42 minus the ray; light
/// space: dwords 44-46 plus the light-space ray, the light matrix being dwords 24-34). The vertex shader gives each eye
/// its own ray (the eye translation in the shader recompiler), the constants stay the centre camera's: each eye's point
/// was off by its eye offset, enough to flip the headlights' shadow test on a car side the beam grazes (lit in one eye,
/// dark in the other). The light buffer binding (pixel shader buffer 1) gets a copy per eye with the offset added.
bool EyeLightBinding(u64 fs_hash, u32 buffer_index);
/// Development (EYELIGHTS trigger or BB_VR_EYE_LIGHTS at start: 1 on, 0 off, 2 the opposite correction).
void SetEyeLights(int mode);
/// The two copies (left, right) of a light's constants (`light`, `bytes` long, at most 256), given the player camera's
/// constants (`camera`: the view-projection at dwords 0-15). False when they cannot be made.
bool EyeLightConstants(const u32* light, u32 bytes, const float* camera, std::array<std::array<u32, 64>, 2>& out);

/// The eye's offset from the centre camera in camera-relative units (`camera`: the view-projection at dwords 0-15).
bool EyeOffset(const float* camera, int eye, float e[3]);
/// The scene constants (1936 bytes: fog, sun, the sun's shadow matrix) per eye. The matrix maps camera-relative points
/// to shadow space; with each eye's own ray a pixel's point is relative to that eye, so its translation moves by the
/// matrix times the eye's offset. Without it each eye's sun shadow lookups were off by the eye offset: shadows came
/// and went in the two eyes in tight places (a brake caliper lit in the right eye on the car select screen, dark in
/// the left and in the flat game). BB_VR_EYE_SHADOW=0 turns it off.
bool EyeSceneConstants(const u32* scene, u32 bytes, const float* camera, std::array<std::vector<u32>, 2>& out);

/// The left eye's clip x shift (the right eye's is its negative).
float Shift();

/// Binding: an image bound to the next draw or dispatch (storage: written).
void NoteImage(const VideoCore::Image& image, bool storage);

/// Development (BB_VR_HASHES): the dispatch about to bind (its hash, for the second-eye log).
void SetDispatchHash(u64 hash);

/// Push data as recorded: the draw's shift, or eye 0 for a dispatch (which then knows whether it writes two layers).
Shader::PushData Adjust(const Shader::PushData& push_data, bool compute);

/// Image copies (`regions`, vk::ImageCopy) from `src` to `dst`: a two-layer destination gets its right eye too, from
/// the source's right eye or, for a one-layer source, the same picture.
template <typename Regions>
void StereoCopies(const VideoCore::Image& src, const VideoCore::Image& dst, Regions& regions) {
    if (!Enabled() || !Stereo(dst)) {
        return;
    }
    const bool stereo_src = Stereo(src);
    const size_t count = regions.size();
    for (size_t i = 0; i < count; ++i) {
        auto& region = regions[i];
        if (region.dstSubresource.baseArrayLayer != 0 || region.dstSubresource.layerCount != 1 ||
            region.srcSubresource.layerCount != 1) {
            continue;
        }
        if (stereo_src && region.srcSubresource.baseArrayLayer == 0) {
            region.srcSubresource.layerCount = region.dstSubresource.layerCount = 2;
        } else {
            auto right = region;
            right.dstSubresource.baseArrayLayer = 1;
            regions.push_back(right);
        }
    }
}

/// Runtime::UploadImage: the copies with the right eye's layer added for a two-layer image (the same data in both
/// eyes: a game texture that matched a main-view size would otherwise show unfilled memory in the right eye).
std::span<const vk::BufferImageCopy> StereoUploads(const VideoCore::Image& dst,
                                                   std::span<const vk::BufferImageCopy> copies);

/// A guest buffer the bindings use (`adjust`: the byte offset the shader adds), written or read: the audit, and the
/// binding the next EyeBufferWrite completes.
void NoteBuffer(u64 address, u64 size, bool written, u64 adjust);

/// Per-eye buffers. In VR every guest storage buffer is a two-element descriptor array in the shaders (the element is
/// the eye: shader_recompiler emit_spirv_vr.h). The computes that build Driveclub's lighting data from the scene's
/// depth and normals (its tiled lights) ran once, for the left eye, and the right eye was lit with the left eye's
/// tiles (a sign beside the road dark in the right eye only, lighting on the cars different in each eye): they run per
/// eye now, the right eye's dispatch writing a copy of the buffer that the right eye's reads get.
/// The descriptors a storage buffer binding takes (two for a guest buffer in VR).
u32 BufferDescriptors(bool special);

/// The descriptors past one per binding among storage buffer bindings (for the push descriptor limit).
u32 ExtraDescriptors(std::span<const vk::DescriptorSetLayoutBinding> bindings);

/// The second descriptor of a guest buffer binding (`first` its left eye's): the right eye's copy of a per-eye buffer,
/// else the same buffer. Nothing for a special buffer or outside VR.
std::optional<vk::DescriptorBufferInfo> EyeBufferInfo(const Instance& instance, Runtime& runtime, bool special,
                                                      const vk::DescriptorBufferInfo& first);

/// Rasterizer::BindBuffers, after a buffer binding's write: the second descriptor appended to `infos`, the write made
/// two long.
template <typename Infos>
void EyeBufferWrite(const Instance& instance, Runtime& runtime, bool special, vk::WriteDescriptorSet& write,
                    Infos& infos) {
    if (const auto second = EyeBufferInfo(instance, runtime, special, infos.back())) {
        infos.push_back(*second);
        write.descriptorCount = 2;
        write.pBufferInfo = &infos[infos.size() - 2];
    }
}

/// Development audit (BB_VR_AUDIT=1): a read-only buffer bound from the constant ring (the player camera's
/// constants are found among them).
void NoteRingBuffer(u64 address, u64 size);

/// Development audit (BB_VR_AUDIT=1): an image copy (`what`) touching a two-layer image, once per kind.
void AuditCopy(const char* what, const VideoCore::Image& src, const VideoCore::Image& dst);

/// Development log (BB_VR_MONO_COMPUTE): the draw's vertex shader.
void SetDrawHash(u64 hash);

/// Runtime::ResolveImage: the layers to resolve or copy (`layers`, the source's and the destination's): both eyes when
/// both images have them.
template <typename Layers>
Layers ResolveLayers(const VideoCore::Image& src, const VideoCore::Image& dst, Layers layers) {
    if (Enabled() && Stereo(src) && Stereo(dst) && layers.first == 1 && layers.second == 1) {
        layers.first = layers.second = 2;
    }
    return layers;
}

/// After a dispatch is recorded: the right eye's dispatch when it writes a two-layer image (`indirect`: null for a
/// direct dispatch of x, y, z groups).
void SecondEye(Scheduler& scheduler, vk::PipelineLayout layout, const Shader::PushData& push_data, vk::Pipeline handle,
               u32 x, u32 y, u32 z, vk::Buffer indirect = {}, u64 indirect_offset = 0);

} // namespace Vulkan::Vr::Multiview
