// SPDX-License-Identifier: GPL-2.0-or-later

#include "video_core/renderer_vulkan/host_passes/user_filter_pass.h"

#include <array>
#include <cctype>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <vector>

#include "common/assert.h"
#include "common/logging/log.h"
#include "bbport_postfx.h"
#include "video_core/host_shaders/fs_tri_vert.h"
#include "video_core/renderer_vulkan/vk_platform.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"

extern "C" const char* runtime_file_mods_dir(int i);

namespace Vulkan::HostPasses {

namespace {

long long NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// The file of a filter in the first mods folder that has it, or an empty path.
std::filesystem::path FindFilter(const std::string& name) {
    // names are file names: no folders, no dots
    for (const char c : name) {
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-')) {
            return {};
        }
    }
    for (int i = 0; const char* mods = runtime_file_mods_dir(i); ++i) {
        std::error_code error;
        const auto path = std::filesystem::path(mods) / "postfx" / (name + ".spv");
        if (std::filesystem::is_regular_file(path, error)) {
            return path;
        }
    }
    return {};
}

long long Stamp(const std::filesystem::path& path) {
    std::error_code error;
    const auto time = std::filesystem::last_write_time(path, error);
    return error ? 0 : static_cast<long long>(time.time_since_epoch().count());
}

} // namespace

void UserFilterPass::Create(vk::Device device_, VmaAllocator allocator_, vk::Format format_) {
    device = device_;
    allocator = allocator_;
    format = format_;
    sampler = Check<"create user filter sampler">(device.createSamplerUnique({
        .magFilter = vk::Filter::eLinear,
        .minFilter = vk::Filter::eLinear,
        .mipmapMode = vk::SamplerMipmapMode::eNearest,
        .addressModeU = vk::SamplerAddressMode::eClampToEdge,
        .addressModeV = vk::SamplerAddressMode::eClampToEdge,
    }));
    const std::array bindings{
        vk::DescriptorSetLayoutBinding{
            .binding = 0,
            .descriptorType = vk::DescriptorType::eCombinedImageSampler,
            .descriptorCount = 1,
            .stageFlags = vk::ShaderStageFlagBits::eFragment,
        },
    };
    desc_set_layout = Check<"create user filter descriptor set layout">(device.createDescriptorSetLayoutUnique({
        .flags = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR,
        .bindingCount = static_cast<u32>(bindings.size()),
        .pBindings = bindings.data(),
    }));
    const vk::PushConstantRange push_constants{
        .stageFlags = vk::ShaderStageFlagBits::eFragment,
        .offset = 0,
        .size = sizeof(Constants),
    };
    pipeline_layout = Check<"create user filter pipeline layout">(device.createPipelineLayoutUnique({
        .setLayoutCount = 1U,
        .pSetLayouts = &*desc_set_layout,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &push_constants,
    }));
    vertex_module = vk::UniqueShaderModule(CompileSPV(FS_TRI_VERT, device), device);
}

bool UserFilterPass::LoadShader(const std::string& name) {
    loaded_name = name;
    failed = true;
    pipeline.reset();
    const auto path = FindFilter(name);
    if (path.empty()) {
        LOG_WARNING(Render_Vulkan, "Post filter '{}': no postfx/{}.spv in a mods folder", name, name);
        return false;
    }
    loaded_path = path.string();
    loaded_stamp = Stamp(path);
    std::ifstream file(path, std::ios::binary);
    const std::vector<char> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    u32 magic = 0;
    if (bytes.size() >= 20) {
        std::memcpy(&magic, bytes.data(), 4);
    }
    if (bytes.size() < 20 || bytes.size() % 4 != 0 || magic != 0x07230203u) {
        LOG_WARNING(Render_Vulkan, "Post filter '{}': {} is not a SPIR-V file", name, loaded_path);
        return false;
    }
    const auto fragment = device.createShaderModuleUnique({
        .codeSize = bytes.size(),
        .pCode = reinterpret_cast<const u32*>(bytes.data()),
    });
    if (fragment.result != vk::Result::eSuccess) {
        LOG_WARNING(Render_Vulkan, "Post filter '{}': the shader was refused ({})", name,
                    vk::to_string(fragment.result));
        return false;
    }
    const std::array stages{
        vk::PipelineShaderStageCreateInfo{
            .stage = vk::ShaderStageFlagBits::eVertex, .module = *vertex_module, .pName = "main"},
        vk::PipelineShaderStageCreateInfo{
            .stage = vk::ShaderStageFlagBits::eFragment, .module = *fragment.value, .pName = "main"},
    };
    const std::array color_formats{format};
    const vk::PipelineRenderingCreateInfo rendering{
        .colorAttachmentCount = color_formats.size(),
        .pColorAttachmentFormats = color_formats.data(),
    };
    const vk::PipelineVertexInputStateCreateInfo vertex_input{};
    const vk::PipelineInputAssemblyStateCreateInfo input_assembly{
        .topology = vk::PrimitiveTopology::eTriangleList};
    const vk::Viewport viewport{.width = 1.0f, .height = 1.0f, .maxDepth = 1.0f};
    const vk::Rect2D scissor{.extent = {1, 1}};
    const vk::PipelineViewportStateCreateInfo viewport_info{
        .viewportCount = 1, .pViewports = &viewport, .scissorCount = 1, .pScissors = &scissor};
    const vk::PipelineRasterizationStateCreateInfo raster{
        .polygonMode = vk::PolygonMode::eFill,
        .cullMode = vk::CullModeFlagBits::eBack,
        .frontFace = vk::FrontFace::eClockwise,
        .lineWidth = 1.0f,
    };
    const vk::PipelineMultisampleStateCreateInfo multisampling{
        .rasterizationSamples = vk::SampleCountFlagBits::e1};
    const std::array blend_attachments{vk::PipelineColorBlendAttachmentState{
        .blendEnable = false,
        .colorWriteMask = vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
                          vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA,
    }};
    const vk::PipelineColorBlendStateCreateInfo blending{
        .attachmentCount = blend_attachments.size(), .pAttachments = blend_attachments.data()};
    const std::array dynamic_states{vk::DynamicState::eViewport, vk::DynamicState::eScissor};
    const vk::PipelineDynamicStateCreateInfo dynamic_info{
        .dynamicStateCount = dynamic_states.size(), .pDynamicStates = dynamic_states.data()};
    auto created = device.createGraphicsPipelineUnique({}, vk::GraphicsPipelineCreateInfo{
        .pNext = &rendering,
        .stageCount = stages.size(),
        .pStages = stages.data(),
        .pVertexInputState = &vertex_input,
        .pInputAssemblyState = &input_assembly,
        .pViewportState = &viewport_info,
        .pRasterizationState = &raster,
        .pMultisampleState = &multisampling,
        .pColorBlendState = &blending,
        .pDynamicState = &dynamic_info,
        .layout = *pipeline_layout,
    });
    if (created.result != vk::Result::eSuccess) {
        LOG_WARNING(Render_Vulkan,
                    "Post filter '{}': the pipeline could not be made ({}); does the shader read binding 0 and the "
                    "push constants described in docs/modding/postfx.md?",
                    name, vk::to_string(created.result));
        return false;
    }
    pipeline = std::move(created.value);
    failed = false;
    LOG_WARNING(Render_Vulkan, "Post filter '{}' loaded from {}", name, loaded_path);
    return true;
}

bool UserFilterPass::EnsureImage(vk::Extent2D size) {
    if (image_view && image_size.width == size.width && image_size.height == size.height) {
        return true;
    }
    device.waitIdle();
    image_view.reset();
    image = VideoCore::UniqueImage(device, allocator);
    image.Create(vk::ImageCreateInfo{
        .imageType = vk::ImageType::e2D,
        .format = format,
        .extent = {size.width, size.height, 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = vk::SampleCountFlagBits::e1,
        .tiling = vk::ImageTiling::eOptimal,
        .usage = vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eSampled,
        .initialLayout = vk::ImageLayout::eUndefined,
    });
    auto view = device.createImageViewUnique({
        .image = vk::Image(image),
        .viewType = vk::ImageViewType::e2D,
        .format = format,
        .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
    });
    if (view.result != vk::Result::eSuccess) {
        return false;
    }
    image_view = std::move(view.value);
    image_size = size;
    return true;
}

vk::ImageView UserFilterPass::Render(vk::CommandBuffer cmdbuf, vk::ImageView input, vk::Extent2D size) {
    const std::string name = BbPostFx::Name();
    if (name.empty() || !desc_set_layout) {
        return input;
    }
    const long long now = NowMs();
    if (name != loaded_name) {
        LoadShader(name);
        last_check_ms = now;
    } else if (now - last_check_ms > 1000) {
        // a changed file is picked up while the game runs
        last_check_ms = now;
        if (!loaded_path.empty() && Stamp(loaded_path) != loaded_stamp) {
            device.waitIdle();
            LoadShader(name);
        }
    }
    if (failed || !pipeline || !EnsureImage(size)) {
        return input;
    }

    const vk::ImageMemoryBarrier2 to_attachment{
        .srcStageMask = vk::PipelineStageFlagBits2::eFragmentShader,
        .srcAccessMask = vk::AccessFlagBits2::eShaderRead,
        .dstStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
        .dstAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite,
        .oldLayout = vk::ImageLayout::eUndefined,
        .newLayout = vk::ImageLayout::eColorAttachmentOptimal,
        .image = vk::Image(image),
        .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
    };
    cmdbuf.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &to_attachment});

    const vk::RenderingAttachmentInfo attachment{
        .imageView = *image_view,
        .imageLayout = vk::ImageLayout::eColorAttachmentOptimal,
        .loadOp = vk::AttachmentLoadOp::eDontCare,
        .storeOp = vk::AttachmentStoreOp::eStore,
    };
    const vk::RenderingInfo rendering{
        .renderArea = {.extent = size},
        .layerCount = 1,
        .colorAttachmentCount = 1,
        .pColorAttachments = &attachment,
    };
    static const long long start_ms = NowMs();
    Constants constants{};
    constants.resolution[0] = static_cast<float>(size.width);
    constants.resolution[1] = static_cast<float>(size.height);
    constants.time = static_cast<float>(now - start_ms) / 1000.0f;
    for (int i = 0; i < 16; ++i) {
        constants.values[i] = BbPostFx::Values()[i].load(std::memory_order_relaxed);
    }
    const vk::DescriptorImageInfo image_info{
        .sampler = *sampler,
        .imageView = input,
        .imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
    };
    const std::array set_writes{vk::WriteDescriptorSet{
        .dstBinding = 0,
        .descriptorCount = 1,
        .descriptorType = vk::DescriptorType::eCombinedImageSampler,
        .pImageInfo = &image_info,
    }};
    cmdbuf.bindPipeline(vk::PipelineBindPoint::eGraphics, *pipeline);
    cmdbuf.setViewport(0, vk::Viewport{.width = static_cast<float>(size.width),
                                        .height = static_cast<float>(size.height),
                                        .maxDepth = 1.0f});
    cmdbuf.setScissor(0, vk::Rect2D{.extent = size});
    cmdbuf.pushDescriptorSetKHR(vk::PipelineBindPoint::eGraphics, *pipeline_layout, 0, set_writes);
    cmdbuf.pushConstants(*pipeline_layout, vk::ShaderStageFlagBits::eFragment, 0, sizeof(Constants), &constants);
    cmdbuf.beginRendering(rendering);
    cmdbuf.draw(3, 1, 0, 0);
    cmdbuf.endRendering();

    const vk::ImageMemoryBarrier2 to_sampled{
        .srcStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
        .srcAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eFragmentShader,
        .dstAccessMask = vk::AccessFlagBits2::eShaderRead,
        .oldLayout = vk::ImageLayout::eColorAttachmentOptimal,
        .newLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
        .image = vk::Image(image),
        .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
    };
    cmdbuf.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &to_sampled});
    return *image_view;
}

} // namespace Vulkan::HostPasses
