// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: placeholder resources for devices without VK_EXT_robustness2 nullDescriptor (MoltenVK).
//
// The rasterizer writes VK_NULL_HANDLE for a binding the game leaves empty: a null T# or V#,
// clip planes no permutation reads, an unbound vertex stream. That needs nullDescriptor. Where
// the device lacks it, these zeroed resources are bound instead: one buffer, and 1x1 images
// in every view shape and component type a shader can declare. Reads return zero as a null
// descriptor's would; writes land in the placeholder and are never read back by the game.
#pragma once

#include <array>
#include <memory>
#include "video_core/amdgpu/resource.h"
#include "video_core/renderer_vulkan/vk_common.h"
#include "video_core/texture_cache/image.h"

namespace VideoCore {
class Buffer;
} // namespace VideoCore

namespace Shader {
struct ImageResource;
}

namespace Vulkan {

class Instance;

class NullResources {
public:
    /// Creates the placeholders only when the device has no nullDescriptor; otherwise every
    /// getter hands back the null handles the rasterizer used before.
    explicit NullResources(const Instance& instance);
    ~NullResources();

    NullResources(const NullResources&) = delete;
    NullResources& operator=(const NullResources&) = delete;

    bool Active() const {
        return active;
    }

    /// A buffer descriptor for an empty binding.
    vk::DescriptorBufferInfo BufferInfo() const;

    /// A vertex buffer for an empty stream, with the size to bind it with.
    vk::Buffer VertexBuffer() const;
    vk::DeviceSize VertexSize(vk::DeviceSize requested) const;

    /// An image descriptor for an empty binding of `desc`, whose T# is `sharp` (null or
    /// rejected): the view shape and component type the shader was specialized on, as
    /// spirv_emit_context.cpp ImageType() declares them.
    vk::DescriptorImageInfo ImageInfo(const Shader::ImageResource& desc,
                                      const AmdGpu::Image& sharp) const;

private:
    enum Kind : u32 { Float, Uint, Sint, NumKinds };
    enum Shape : u32 { Shape1D, Shape2D, Shape3D, NumShapes };
    enum View : u32 { View1D, View1DArray, View2D, View2DArray, ViewCubeArray, View3D, NumViews };

    bool active{};
    vk::Device device;
    std::unique_ptr<VideoCore::Buffer> buffer;
    vk::DeviceSize buffer_size{};
    std::array<std::array<std::unique_ptr<VideoCore::UniqueImage>, NumShapes>, NumKinds> images{};
    std::array<std::array<vk::ImageView, NumViews>, NumKinds> views{};
};

} // namespace Vulkan
