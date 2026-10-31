// SPDX-License-Identifier: GPL-2.0-or-later

#include "video_core/renderer_vulkan/host_passes/hdr_pass.h"

#include "common/assert.h"
#include "video_core/host_shaders/fs_tri_vert.h"
#include "video_core/host_shaders/hdr_exposure_comp.h"
#include "video_core/host_shaders/hdr_output_frag.h"
#include "video_core/renderer_vulkan/vk_platform.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"

namespace Vulkan::HostPasses {

namespace {
vk::DescriptorSetLayoutBinding Binding(u32 index, vk::DescriptorType type, vk::ShaderStageFlags stage) {
    return {.binding = index, .descriptorType = type, .descriptorCount = 1, .stageFlags = stage};
}
} // namespace

void HdrPass::Create(vk::Device device, VmaAllocator allocator, vk::Format output_format) {
    constexpr auto sampled = vk::DescriptorType::eCombinedImageSampler;
    constexpr auto storage = vk::DescriptorType::eStorageImage;
    {
        constexpr auto fragment = vk::ShaderStageFlagBits::eFragment;
        const std::array bindings{Binding(0, sampled, fragment), Binding(1, sampled, fragment),
                                  Binding(2, sampled, fragment), Binding(3, storage, fragment)};
        desc_set_layout = Check<"create hdr descriptor set layout">(device.createDescriptorSetLayoutUnique({
            .flags = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR,
            .bindingCount = static_cast<u32>(bindings.size()),
            .pBindings = bindings.data(),
        }));
        const vk::PushConstantRange push_constants{
            .stageFlags = fragment, .offset = 0, .size = sizeof(Params)};
        pipeline_layout = Check<"create hdr pipeline layout">(device.createPipelineLayoutUnique({
            .setLayoutCount = 1U,
            .pSetLayouts = &*desc_set_layout,
            .pushConstantRangeCount = 1,
            .pPushConstantRanges = &push_constants,
        }));
    }
    {
        constexpr auto compute = vk::ShaderStageFlagBits::eCompute;
        const std::array bindings{Binding(0, sampled, compute), Binding(1, sampled, compute),
                                  Binding(2, storage, compute)};
        exposure_set_layout = Check<"create hdr exposure set layout">(device.createDescriptorSetLayoutUnique({
            .flags = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR,
            .bindingCount = static_cast<u32>(bindings.size()),
            .pBindings = bindings.data(),
        }));
        exposure_layout = Check<"create hdr exposure layout">(device.createPipelineLayoutUnique({
            .setLayoutCount = 1U,
            .pSetLayouts = &*exposure_set_layout,
        }));
        const auto module = CompileSPV(HDR_EXPOSURE_COMP, device);
        ASSERT(module);
        exposure_pipeline = Check<"create hdr exposure pipeline">(device.createComputePipelineUnique(
            {}, vk::ComputePipelineCreateInfo{
                    .stage = {.stage = compute, .module = module, .pName = "main"},
                    .layout = *exposure_layout,
                }));
        device.destroyShaderModule(module);
    }

    const auto vs_module = CompileSPV(FS_TRI_VERT, device);
    const auto fs_module = CompileSPV(HDR_OUTPUT_FRAG, device);
    ASSERT(vs_module && fs_module);
    const std::array shaders_ci{
        vk::PipelineShaderStageCreateInfo{
            .stage = vk::ShaderStageFlagBits::eVertex, .module = vs_module, .pName = "main"},
        vk::PipelineShaderStageCreateInfo{
            .stage = vk::ShaderStageFlagBits::eFragment, .module = fs_module, .pName = "main"},
    };
    const vk::PipelineRenderingCreateInfo rendering_ci{
        .colorAttachmentCount = 1,
        .pColorAttachmentFormats = &output_format,
    };
    const vk::PipelineVertexInputStateCreateInfo vertex_input{};
    const vk::PipelineInputAssemblyStateCreateInfo input_assembly{
        .topology = vk::PrimitiveTopology::eTriangleList,
    };
    const vk::PipelineViewportStateCreateInfo viewport_info{.viewportCount = 1, .scissorCount = 1};
    const vk::PipelineRasterizationStateCreateInfo raster_state{
        .polygonMode = vk::PolygonMode::eFill,
        .cullMode = vk::CullModeFlagBits::eBack,
        .frontFace = vk::FrontFace::eClockwise,
        .lineWidth = 1.0f,
    };
    const vk::PipelineMultisampleStateCreateInfo multisampling{
        .rasterizationSamples = vk::SampleCountFlagBits::e1,
    };
    const vk::PipelineColorBlendAttachmentState attachment{
        .blendEnable = false,
        .colorWriteMask = vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
                          vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA,
    };
    const vk::PipelineColorBlendStateCreateInfo color_blending{
        .attachmentCount = 1,
        .pAttachments = &attachment,
    };
    const std::array dynamic_states{vk::DynamicState::eViewport, vk::DynamicState::eScissor};
    const vk::PipelineDynamicStateCreateInfo dynamic_info{
        .dynamicStateCount = dynamic_states.size(),
        .pDynamicStates = dynamic_states.data(),
    };
    pipeline = Check<"create hdr pipeline">(device.createGraphicsPipelineUnique(
        {}, vk::GraphicsPipelineCreateInfo{
                .pNext = &rendering_ci,
                .stageCount = shaders_ci.size(),
                .pStages = shaders_ci.data(),
                .pVertexInputState = &vertex_input,
                .pInputAssemblyState = &input_assembly,
                .pViewportState = &viewport_info,
                .pRasterizationState = &raster_state,
                .pMultisampleState = &multisampling,
                .pColorBlendState = &color_blending,
                .pDynamicState = &dynamic_info,
                .layout = *pipeline_layout,
            }));
    device.destroyShaderModule(vs_module);
    device.destroyShaderModule(fs_module);

    sampler = Check<"create hdr sampler">(device.createSamplerUnique({
        .magFilter = vk::Filter::eLinear,
        .minFilter = vk::Filter::eLinear,
        .mipmapMode = vk::SamplerMipmapMode::eNearest,
        .addressModeU = vk::SamplerAddressMode::eClampToEdge,
        .addressModeV = vk::SamplerAddressMode::eClampToEdge,
    }));

    state = VideoCore::UniqueImage(device, allocator);
    state.Create(vk::ImageCreateInfo{
        .imageType = vk::ImageType::e2D,
        .format = vk::Format::eR32Sfloat,
        .extent = {1, 1, 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = vk::SampleCountFlagBits::e1,
        .tiling = vk::ImageTiling::eOptimal,
        .usage = vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eTransferDst,
        .initialLayout = vk::ImageLayout::eUndefined,
    });
    state_view = Check<"create hdr state view">(device.createImageViewUnique({
        .image = vk::Image(state),
        .viewType = vk::ImageViewType::e2D,
        .format = vk::Format::eR32Sfloat,
        .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
    }));
}

void HdrPass::Render(vk::CommandBuffer cmdbuf, vk::ImageView frame, vk::ImageView hudless,
                     vk::ImageView scene, vk::ImageView output, vk::Extent2D size, Params params) {
    const vk::ImageSubresourceRange color_range{vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};
    if (!state_cleared) {
        state_cleared = true;
        const vk::ImageMemoryBarrier2 to_clear{
            .dstStageMask = vk::PipelineStageFlagBits2::eTransfer,
            .dstAccessMask = vk::AccessFlagBits2::eTransferWrite,
            .oldLayout = vk::ImageLayout::eUndefined,
            .newLayout = vk::ImageLayout::eGeneral,
            .image = vk::Image(state),
            .subresourceRange = color_range,
        };
        cmdbuf.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &to_clear});
        cmdbuf.clearColorImage(vk::Image(state), vk::ImageLayout::eGeneral,
                               vk::ClearColorValue{std::array<float, 4>{0.0f, 0.0f, 0.0f, 0.0f}},
                               color_range);
    }
    const auto state_ready = [&](vk::PipelineStageFlags2 src, vk::AccessFlags2 src_access,
                                 vk::PipelineStageFlags2 dst, vk::AccessFlags2 dst_access) {
        const vk::MemoryBarrier2 b{
            .srcStageMask = src, .srcAccessMask = src_access, .dstStageMask = dst, .dstAccessMask = dst_access};
        cmdbuf.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &b});
    };
    const vk::DescriptorImageInfo state_info{.imageView = *state_view,
                                             .imageLayout = vk::ImageLayout::eGeneral};
    const auto sampled_info = [&](vk::ImageView view) {
        return vk::DescriptorImageInfo{.sampler = *sampler,
                                       .imageView = view,
                                       .imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal};
    };
    params.has_scene = 0.0f;
    if (params.has_scene > 0.5f) {
        // How bright the scene is against the SDR picture, from the picture before the UI.
        state_ready(vk::PipelineStageFlagBits2::eAllCommands, vk::AccessFlagBits2::eMemoryWrite,
                    vk::PipelineStageFlagBits2::eComputeShader,
                    vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite);
        const auto hudless_info = sampled_info(hudless), scene_info = sampled_info(scene);
        const std::array writes{
            vk::WriteDescriptorSet{.dstBinding = 0, .descriptorCount = 1,
                                   .descriptorType = vk::DescriptorType::eCombinedImageSampler,
                                   .pImageInfo = &hudless_info},
            vk::WriteDescriptorSet{.dstBinding = 1, .descriptorCount = 1,
                                   .descriptorType = vk::DescriptorType::eCombinedImageSampler,
                                   .pImageInfo = &scene_info},
            vk::WriteDescriptorSet{.dstBinding = 2, .descriptorCount = 1,
                                   .descriptorType = vk::DescriptorType::eStorageImage,
                                   .pImageInfo = &state_info},
        };
        cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, *exposure_pipeline);
        cmdbuf.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, *exposure_layout, 0, writes);
        cmdbuf.dispatch(1, 1, 1);
    }
    state_ready(vk::PipelineStageFlagBits2::eComputeShader | vk::PipelineStageFlagBits2::eTransfer,
                vk::AccessFlagBits2::eShaderWrite | vk::AccessFlagBits2::eTransferWrite,
                vk::PipelineStageFlagBits2::eFragmentShader, vk::AccessFlagBits2::eShaderRead);

    const auto frame_info = sampled_info(frame);
    const auto hudless_info = sampled_info(hudless ? hudless : frame);
    const auto scene_info = sampled_info(scene ? scene : frame);
    const std::array set_writes{
        vk::WriteDescriptorSet{.dstBinding = 0, .descriptorCount = 1,
                               .descriptorType = vk::DescriptorType::eCombinedImageSampler,
                               .pImageInfo = &frame_info},
        vk::WriteDescriptorSet{.dstBinding = 1, .descriptorCount = 1,
                               .descriptorType = vk::DescriptorType::eCombinedImageSampler,
                               .pImageInfo = &hudless_info},
        vk::WriteDescriptorSet{.dstBinding = 2, .descriptorCount = 1,
                               .descriptorType = vk::DescriptorType::eCombinedImageSampler,
                               .pImageInfo = &scene_info},
        vk::WriteDescriptorSet{.dstBinding = 3, .descriptorCount = 1,
                               .descriptorType = vk::DescriptorType::eStorageImage,
                               .pImageInfo = &state_info},
    };
    const vk::RenderingAttachmentInfo attachment{
        .imageView = output,
        .imageLayout = vk::ImageLayout::eColorAttachmentOptimal,
        .loadOp = vk::AttachmentLoadOp::eDontCare,
        .storeOp = vk::AttachmentStoreOp::eStore,
    };
    cmdbuf.bindPipeline(vk::PipelineBindPoint::eGraphics, *pipeline);
    cmdbuf.setViewport(0, vk::Viewport{.width = float(size.width),
                                       .height = float(size.height),
                                       .minDepth = 0.0f,
                                       .maxDepth = 1.0f});
    cmdbuf.setScissor(0, vk::Rect2D{.extent = size});
    cmdbuf.pushDescriptorSetKHR(vk::PipelineBindPoint::eGraphics, *pipeline_layout, 0, set_writes);
    cmdbuf.pushConstants(*pipeline_layout, vk::ShaderStageFlagBits::eFragment, 0, sizeof(Params),
                         &params);
    cmdbuf.beginRendering(vk::RenderingInfo{
        .renderArea = {.extent = size},
        .layerCount = 1,
        .colorAttachmentCount = 1,
        .pColorAttachments = &attachment,
    });
    cmdbuf.draw(3, 1, 0, 0);
    cmdbuf.endRendering();
}

} // namespace Vulkan::HostPasses
