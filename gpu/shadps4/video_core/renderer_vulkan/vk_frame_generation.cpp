#include "../../../shim/bbport_diagnostics.h"
// SPDX-License-Identifier: GPL-2.0-or-later
#include "video_core/renderer_vulkan/vk_frame_generation.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>

#include <vk_mem_alloc.h>

#include "bbport_settings.h"
#include "ffx_vk_fsr3_3_1_5_bridge.h"
#include "video_core/host_shaders/frame_gen_inputs_comp.h"
#include "video_core/host_shaders/frame_gen_flow_down_comp.h"
#include "video_core/host_shaders/frame_gen_flow_final_comp.h"
#include "video_core/host_shaders/frame_gen_obj_cost_comp.h"
#include "video_core/host_shaders/frame_gen_obj_pick_comp.h"
#include "video_core/host_shaders/frame_gen_ui_comp.h"
#include "video_core/renderer_vulkan/vk_dlss.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"

namespace Vulkan {

namespace {

/// The hudless copy viewed like the presenter views the display buffer (`requested`): the
/// copy's channel order, the requested sRGB decoding.
vk::Format HudlessViewFormat(vk::Format /*storage*/, vk::Format requested) {
    // The same view format the real display image is read with (A8R8G8B8 reads as B8G8R8A8): the
    // copy is a mutable-format image of the same 32-bit class, so the channel order then matches.
    return requested;
}

constexpr vk::ImageUsageFlags HudlessUsage = vk::ImageUsageFlagBits::eSampled |
                                              vk::ImageUsageFlagBits::eTransferSrc |
                                              vk::ImageUsageFlagBits::eTransferDst;

constexpr vk::ImageUsageFlags InputUsage =
    vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled |
    vk::ImageUsageFlagBits::eTransferSrc | vk::ImageUsageFlagBits::eTransferDst;

// Search radius of the player's car vector per pyramid level (1/8, 1/4, 1/2 of the picture): up to
// about 100 pixels of the picture in total.
constexpr std::array<int, 3> kCarRadius{12, 2, 2};

int DlssMaxCount() {
    const Dlss* dlss = Dlss::Get();
    return dlss ? dlss->FrameGenMaxCount() : 0;
}

void DumpFrame(const Instance& instance, Scheduler& scheduler, vk::CommandBuffer cmdbuf,
               vk::Image image, u32 w, u32 h, const char* name, u32 frame) {
    const VkBufferCreateInfo buffer_ci{.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                                       .size = VkDeviceSize(w) * h * 4,
                                       .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    const VmaAllocationCreateInfo alloc_ci{
        .flags = VMA_ALLOCATION_CREATE_MAPPED_BIT | VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT,
        .usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST};
    VkBuffer buffer{};
    VmaAllocation allocation{};
    VmaAllocationInfo info{};
    if (vmaCreateBuffer(instance.GetAllocator(), &buffer_ci, &alloc_ci, &buffer, &allocation,
                        &info) != VK_SUCCESS) {
        return;
    }
    const vk::MemoryBarrier2 written{.srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
                                     .srcAccessMask = vk::AccessFlagBits2::eMemoryWrite,
                                     .dstStageMask = vk::PipelineStageFlagBits2::eTransfer,
                                     .dstAccessMask = vk::AccessFlagBits2::eTransferRead};
    cmdbuf.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &written});
    cmdbuf.copyImageToBuffer(image, vk::ImageLayout::eGeneral, buffer,
                             vk::BufferImageCopy{.imageSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
                                                 .imageExtent = {w, h, 1}});
    const vk::MemoryBarrier2 host_read{.srcStageMask = vk::PipelineStageFlagBits2::eTransfer,
        .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eHost,
        .dstAccessMask = vk::AccessFlagBits2::eHostRead};
    cmdbuf.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &host_read});
    const char* dir = BB_DIAGNOSTIC_ENV("BB_DUMP_DIR");
    char path[512];
    std::snprintf(path, sizeof(path), "%s/fg_%s_f%u_%ux%u.raw", dir && *dir ? dir : "out/dump", name,
                  frame, w, h);
    scheduler.DeferPriorityOperation([allocator = instance.GetAllocator(), buffer, allocation, info,
                                      size = buffer_ci.size, file = std::string{path}] {
        vmaInvalidateAllocation(allocator, allocation, 0, VK_WHOLE_SIZE);
        if (FILE* f = std::fopen(file.c_str(), "wb")) {
            std::fwrite(info.pMappedData, 1, size, f);
            std::fclose(f);
        }
        vmaDestroyBuffer(allocator, buffer, allocation);
    });
}

extern "C" short __stdcall GetAsyncKeyState(int key);

bool DumpWindow() {
    static const char* env = BB_DIAGNOSTIC_ENV("BB_FG_DUMP");
    static u32 left = 0;
    static bool down = false;
    if (!env) {
        return false;
    }
    const bool now = (GetAsyncKeyState(0x7A) & 0x8000) != 0;
    if (now && !down) {
        left = 3;
    }
    down = now;
    if (left == 0) {
        return false;
    }
    --left;
    return true;
}

} // namespace

void FgProfiler::Init(const Instance& instance) {
    const char* env = BB_DIAGNOSTIC_ENV("BB_FG_PROFILE");
    if (!env || env[0] != '1' || instance_ptr) {
        return;
    }
    instance_ptr = new FgProfiler(instance, "capture queue");
    fg_ptr = new FgProfiler(instance, "generation queue");
}

FgProfiler::FgProfiler(const Instance& instance, const char* name_)
    : name{name_}, device{instance.GetDevice()} {
    const vk::QueryPoolCreateInfo info{.queryType = vk::QueryType::eTimestamp,
                                         .queryCount = NumSlices * SliceQueries};
    pool = Check<"create FG timestamp pool">(device.createQueryPoolUnique(info));
    device.resetQueryPool(*pool, 0, NumSlices * SliceQueries);
    period_ns = instance.GetPhysicalDevice().getProperties().limits.timestampPeriod;
    for (auto& k : keys) {
        k.reserve(SliceQueries);
    }
    std::printf("FG profile: on, %s (timestamp period %.2f ns)\n", name, period_ns);
}

void FgProfiler::Write(vk::CommandBuffer cmdbuf, u64 key) {
    if (used[slice] >= SliceQueries) {
        return; // this frame's slice is full: the rest goes to the last label
    }
    if (key) {
        keys[slice].push_back(key);
    }
    const u32 query = slice * SliceQueries + used[slice]++;
    cmdbuf.writeTimestamp2(vk::PipelineStageFlagBits2::eAllCommands, *pool, query);
}

void FgProfiler::Mark(vk::CommandBuffer cmdbuf, u64 key, const char* label) {
    labels.emplace(key, label);
    Write(cmdbuf, key);
}

void FgProfiler::BeginFrame(vk::CommandBuffer cmdbuf) {
    if (used[slice]) {
        Write(cmdbuf, 0); // close the frame's last segment
        pending[slice] = true;
    }
    slice = (slice + 1) % NumSlices;
    if (!pending[slice]) {
        if (used[slice]) {
            device.resetQueryPool(*pool, slice * SliceQueries, used[slice]);
        }
        used[slice] = 0;
        keys[slice].clear();
    }
    Collect(slice); // retries while the GPU has not finished the slice
    Print();
}

void FgProfiler::Collect(u32 which) {
    if (!pending[which] || !used[which]) {
        return;
    }
    const u32 count = used[which];
    std::vector<u64> stamps(count);
    // Never wait: an unfinished slice is read on a later frame.
    if (device.getQueryPoolResults(*pool, which * SliceQueries, count, count * sizeof(u64),
                                   stamps.data(), sizeof(u64),
                                   vk::QueryResultFlagBits::e64) != vk::Result::eSuccess) {
        return;
    }
    for (u32 i = 0; i + 1 < count && i < keys[which].size(); ++i) {
        auto& total = totals[keys[which][i]];
        total.ms += double(stamps[i + 1] - stamps[i]) * period_ns / 1e6;
        ++total.segments;
    }
    ++frames;
    device.resetQueryPool(*pool, which * SliceQueries, count);
    used[which] = 0;
    keys[which].clear();
    pending[which] = false;
}

void FgProfiler::Print() {
    const auto now = std::chrono::steady_clock::now();
    if (now - window < std::chrono::seconds(5) || frames == 0) {
        return;
    }
    window = now;
    std::printf("FG GPU profile, %s: %llu frames\n", name,
                static_cast<unsigned long long>(frames));
    for (u64 key = 1; key <= 7; ++key) {
        if (const auto it = totals.find(key); it != totals.end()) {
            std::printf("  %7.3f ms/frame %6.1f/frame  %s\n", it->second.ms / frames,
                        double(it->second.segments) / frames, labels[key].c_str());
        }
    }
    totals.clear();
    frames = 0;
}

FrameGenerator::FrameGenerator(const Instance& instance_, Scheduler& scheduler_)
    : instance{instance_}, scheduler{scheduler_} {
    FgProfiler::Init(instance_);
    auto& settings = BbSettings::Get();
    const int dlss_max = DlssMaxCount();
    settings.dlssg_max_multiplier = dlss_max + 1;
    // BB_DEPTH_INVERTED=1: the game's depth is 1 at the near plane (reversed Z). For DLSS;
    // FSR-Vulkan's frame generation interface has no such option (0 at the near plane).
    if (const char* env = std::getenv("BB_DEPTH_INVERTED")) {
        depth_inverted = env[0] == '1';
    }
    if (settings.frame_gen == BbSettings::FrameGenDlss && dlss_max < 1) {
        const Dlss* dlss = Dlss::Get();
        std::printf("Frame generation: DLSS Frame Generation unavailable (%s); using FSR 3.1\n",
                    !dlss ? "bbport_dlss.dll and nvngx_dlss.dll are not installed"
                    : !dlss->Available() ? "DLSS is unavailable on this system"
                                         : "GeForce RTX 40 or newer, nvngx_dlssg.dll and a "
                                           "current driver are needed");
        settings.frame_gen_problem = "DLSS Frame Generation needs a GeForce RTX 40 or newer";
        settings.frame_gen = BbSettings::FrameGenFsr3;
    }
    if (settings.frame_gen != BbSettings::FrameGenOff) {
        const int max = settings.frame_gen == BbSettings::FrameGenDlss ? dlss_max + 1 : 2;
        std::printf("Frame generation: %s, %dx\n",
                    settings.frame_gen == BbSettings::FrameGenDlss ? "DLSS" : "FSR 3.1",
                    std::min(settings.frame_gen_multiplier.load(), max));
    }
}

FrameGenerator::~FrameGenerator() {
    DestroyFsr3();
    if (Dlss* dlss = Dlss::Get()) {
        dlss->ReleaseFrameGen();
    }
}

bool FrameGenerator::Wanted() const {
    return BbSettings::Get().frame_gen != BbSettings::FrameGenOff;
}

bool FrameGenerator::Paused() const {
    return BbSettings::Get().frame_gen_paused;
}

u32 FrameGenerator::WantedCount() const {
    const auto& settings = BbSettings::Get();
    switch (settings.frame_gen) {
    case BbSettings::FrameGenDlss:
        return u32(std::clamp(settings.frame_gen_multiplier.load() - 1, 1,
                              std::clamp(DlssMaxCount(), 1, int(MaxGenerated))));
    case BbSettings::FrameGenFsr3:
        return 1;
    default:
        return 0;
    }
}

void FrameGenerator::Skip() {
    for (auto& capture : captures) {
        capture.captured = false;
        capture.hudless_captured = false;
    }
    reset = true;
}

FrameGenerator::Capture* FrameGenerator::FindCapture(u64 address) {
    for (auto& capture : captures) {
        if (capture.captured && capture.address == address) {
            return &capture;
        }
    }
    return nullptr;
}

void FrameGenerator::Release() {
    Skip();
    bool held = fsr3 || last_provider != 0;
    for (const auto& capture : captures) {
        held |= bool(capture.depth_image) || bool(capture.hudless_image);
    }
    if (!held) {
        return; // nothing held
    }
    // Submitted frames may still use them; the open command buffer does not.
    scheduler.WaitSubmitted();
    DestroyFsr3();
    if (Dlss* dlss = Dlss::Get()) {
        dlss->ReleaseFrameGen();
    }
    for (auto& capture : captures) {
        capture.depth_view.reset();
        capture.motion_view.reset();
        capture.depth_image = VideoCore::UniqueImage{};
        capture.motion_image = VideoCore::UniqueImage{};
        capture.input_width = capture.input_height = 0;
        capture.hudless_views.clear();
        capture.hudless_image = VideoCore::UniqueImage{};
        capture.hudless_width = capture.hudless_height = 0;
        capture.address = 0;
    }
    for (auto& set : pyramid) {
        for (auto& level : set) {
            level.view.reset();
            level.image = VideoCore::UniqueImage{};
        }
    }
    for (auto& level : flow) {
        level.view.reset();
        level.image = VideoCore::UniqueImage{};
    }
    cost.view.reset();
    cost.image = VideoCore::UniqueImage{};
    flow_width = flow_height = 0;
    prev_valid = false;
    active = nullptr;
    last_provider = 0;
    last_hudless = false;
    std::puts("Frame generation: off, its memory freed");
}

void FrameGenerator::CreatePipeline() {
    if (pipeline) {
        return;
    }
    const auto device = instance.GetDevice();
    const std::array<vk::DescriptorSetLayoutBinding, 4> bindings{{
        {0, vk::DescriptorType::eSampledImage, 1, vk::ShaderStageFlagBits::eCompute},
        {1, vk::DescriptorType::eStorageImage, 1, vk::ShaderStageFlagBits::eCompute},
        {2, vk::DescriptorType::eSampledImage, 1, vk::ShaderStageFlagBits::eCompute},
        {3, vk::DescriptorType::eStorageImage, 1, vk::ShaderStageFlagBits::eCompute},
    }};
    desc_layout = Check(device.createDescriptorSetLayoutUnique({
        .flags = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR,
        .bindingCount = u32(bindings.size()),
        .pBindings = bindings.data(),
    }));
    const vk::PushConstantRange push{vk::ShaderStageFlagBits::eCompute, 0, 6 * sizeof(float)};
    pipeline_layout = Check(device.createPipelineLayoutUnique({
        .setLayoutCount = 1,
        .pSetLayouts = &*desc_layout,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &push,
    }));
    const auto compute = [&](std::span<const u32> code, vk::PipelineLayout layout) {
        const auto module = CompileSPV(code, device);
        auto made = Check(device.createComputePipelineUnique(
            {}, vk::ComputePipelineCreateInfo{
                    .stage = {.stage = vk::ShaderStageFlagBits::eCompute,
                              .module = module,
                              .pName = "main"},
                    .layout = layout,
                }));
        device.destroyShaderModule(module);
        return made;
    };
    pipeline = compute(FRAME_GEN_INPUTS_COMP, *pipeline_layout);
    // The UI over generated frames: the generated frame (storage), the frame and the frame
    // without UI (sampled).
    const std::array<vk::DescriptorSetLayoutBinding, 3> ui_bindings{{
        {0, vk::DescriptorType::eStorageImage, 1, vk::ShaderStageFlagBits::eCompute},
        {1, vk::DescriptorType::eSampledImage, 1, vk::ShaderStageFlagBits::eCompute},
        {2, vk::DescriptorType::eSampledImage, 1, vk::ShaderStageFlagBits::eCompute},
    }};
    ui_desc_layout = Check(device.createDescriptorSetLayoutUnique({
        .flags = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR,
        .bindingCount = u32(ui_bindings.size()),
        .pBindings = ui_bindings.data(),
    }));
    const vk::PushConstantRange ui_push{vk::ShaderStageFlagBits::eCompute, 0, 2 * sizeof(s32)};
    ui_pipeline_layout = Check(device.createPipelineLayoutUnique({
        .setLayoutCount = 1,
        .pSetLayouts = &*ui_desc_layout,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &ui_push,
    }));
    ui_pipeline = compute(FRAME_GEN_UI_COMP, *ui_pipeline_layout);
    // Optical flow: luma pyramid, block matching per level, the vectors for the generator.
    const auto flow_stage = [&](FlowStage& stage,
                                std::span<const vk::DescriptorSetLayoutBinding> bindings,
                                u32 push_bytes, std::span<const u32> code) {
        stage.desc = Check(device.createDescriptorSetLayoutUnique({
            .flags = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR,
            .bindingCount = u32(bindings.size()),
            .pBindings = bindings.data(),
        }));
        const vk::PushConstantRange range{vk::ShaderStageFlagBits::eCompute, 0, push_bytes};
        stage.layout = Check(device.createPipelineLayoutUnique({
            .setLayoutCount = 1,
            .pSetLayouts = &*stage.desc,
            .pushConstantRangeCount = 1,
            .pPushConstantRanges = &range,
        }));
        stage.pipeline = compute(code, *stage.layout);
    };
    constexpr auto Cs = vk::ShaderStageFlagBits::eCompute;
    constexpr auto Sampled = vk::DescriptorType::eSampledImage;
    constexpr auto Storage = vk::DescriptorType::eStorageImage;
    const std::array<vk::DescriptorSetLayoutBinding, 2> down_bindings{{
        {0, Sampled, 1, Cs}, {1, Storage, 1, Cs}}};
    const std::array<vk::DescriptorSetLayoutBinding, 5> cost_bindings{{
        {0, Sampled, 1, Cs}, {1, Sampled, 1, Cs}, {2, Sampled, 1, Cs}, {3, Sampled, 1, Cs},
        {4, Storage, 1, Cs}}};
    const std::array<vk::DescriptorSetLayoutBinding, 3> pick_bindings{{
        {0, Sampled, 1, Cs}, {1, Sampled, 1, Cs}, {2, Storage, 1, Cs}}};
    const std::array<vk::DescriptorSetLayoutBinding, 3> final_bindings{{
        {0, Sampled, 1, Cs}, {1, Storage, 1, Cs}, {2, Sampled, 1, Cs}}};
    flow_stage(flow_down, down_bindings, 20, FRAME_GEN_FLOW_DOWN_COMP);
    flow_stage(flow_cost, cost_bindings, 28, FRAME_GEN_OBJ_COST_COMP);
    flow_stage(flow_pick, pick_bindings, 16, FRAME_GEN_OBJ_PICK_COMP);
    flow_stage(flow_final, final_bindings, 32, FRAME_GEN_FLOW_FINAL_COMP);
}

void FrameGenerator::EnsureInputs(Capture& capture, u32 w, u32 h) {
    if (capture.depth_image && capture.input_width == w && capture.input_height == h) {
        return;
    }
    // The previous frame's generation may still read the old images.
    scheduler.WaitSubmitted();
    const auto device = instance.GetDevice();
    const auto make = [&](VideoCore::UniqueImage& image, vk::UniqueImageView& view,
                          vk::Format format) {
        view.reset();
        image = VideoCore::UniqueImage(device, instance.GetAllocator());
        image.Create(vk::ImageCreateInfo{
            .imageType = vk::ImageType::e2D,
            .format = format,
            .extent = {w, h, 1},
            .mipLevels = 1,
            .arrayLayers = 1,
            .samples = vk::SampleCountFlagBits::e1,
            .tiling = vk::ImageTiling::eOptimal,
            .usage = InputUsage,
            .initialLayout = vk::ImageLayout::eUndefined,
        });
        view = Check(device.createImageViewUnique({
            .image = vk::Image(image),
            .viewType = vk::ImageViewType::e2D,
            .format = format,
            .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
        }));
    };
    make(capture.depth_image, capture.depth_view, vk::Format::eR32Sfloat);
    make(capture.motion_image, capture.motion_view, vk::Format::eR16G16Sfloat);
    make(capture.mask_image, capture.mask_view, vk::Format::eR8Unorm);
    capture.input_width = w;
    capture.input_height = h;
    reset = true;
}

vk::ImageView FrameGenerator::BeginCapture(vk::CommandBuffer cmdbuf, vk::ImageView depth,
                                           vk::ImageView stencil, u32 w, u32 h, u64 address) {
    active = nullptr;
    if (!Wanted() || !depth || !w || !h) {
        return {};
    }
    CreatePipeline();
    // The depth and motion are brought to the size of the frame: a feature given inputs of a
    // size other than the frame's ignored them (the generated frames smeared).
    static const bool native_size = std::getenv("BB_FG_NATIVE_SIZE") != nullptr;
    const u32 src_w = w, src_h = h;
    if (!native_size && out_width && out_height) {
        w = out_width;
        h = out_height;
    }
    if (auto* prof = FgProfiler::Get()) {
        prof->BeginFrame(cmdbuf);
    }
    // This frame's capture lives with its display buffer: the flip of a frame is processed after
    // the game's thread has begun the next one.
    Capture* slot = nullptr;
    for (auto& capture : captures) {
        if (capture.address == address) {
            slot = &capture;
        }
    }
    if (!slot) {
        slot = &captures[0];
        for (auto& capture : captures) {
            if (capture.serial < slot->serial) {
                slot = &capture;
            }
        }
        slot->address = address;
    }
    slot->serial = ++capture_serial;
    slot->captured = false;
    slot->hudless_captured = false;
    active = slot;
    EnsureInputs(*slot, w, h);
    // Last frame's contents are not needed.
    std::array<vk::ImageMemoryBarrier2, 3> to_write{};
    for (u32 i = 0; i < 3; ++i) {
        to_write[i] = {
            .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
            .srcAccessMask = vk::AccessFlagBits2::eShaderRead,
            .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
            .dstAccessMask = vk::AccessFlagBits2::eShaderWrite,
            .oldLayout = vk::ImageLayout::eUndefined,
            .newLayout = vk::ImageLayout::eGeneral,
            .image = i == 0   ? vk::Image(active->depth_image)
                     : i == 1 ? vk::Image(active->motion_image)
                              : vk::Image(active->mask_image),
            .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
        };
    }
    cmdbuf.pipelineBarrier2({.imageMemoryBarrierCount = 3, .pImageMemoryBarriers = to_write.data()});
    const vk::DescriptorImageInfo depth_info{.imageView = depth,
                                             .imageLayout = vk::ImageLayout::eGeneral};
    const vk::DescriptorImageInfo depth_out{.imageView = *active->depth_view,
                                            .imageLayout = vk::ImageLayout::eGeneral};
    const vk::DescriptorImageInfo stencil_info{.imageView = stencil,
                                               .imageLayout = vk::ImageLayout::eGeneral};
    const vk::DescriptorImageInfo mask_out{.imageView = *active->mask_view,
                                           .imageLayout = vk::ImageLayout::eGeneral};
    const std::array<vk::WriteDescriptorSet, 4> writes{{
        {.dstBinding = 0, .descriptorCount = 1, .descriptorType = vk::DescriptorType::eSampledImage,
         .pImageInfo = &depth_info},
        {.dstBinding = 1, .descriptorCount = 1, .descriptorType = vk::DescriptorType::eStorageImage,
         .pImageInfo = &depth_out},
        {.dstBinding = 2, .descriptorCount = 1, .descriptorType = vk::DescriptorType::eSampledImage,
         .pImageInfo = &stencil_info},
        {.dstBinding = 3, .descriptorCount = 1, .descriptorType = vk::DescriptorType::eStorageImage,
         .pImageInfo = &mask_out},
    }};
    depth_inverted = BbSettings::Get().frame_gen == BbSettings::FrameGenDlss;
    const std::array<float, 6> size{float(src_w), float(src_h), float(w), float(h),
                                    depth_inverted ? 0.0f : 1.0f, stencil != depth ? 1.0f : 0.0f};
    if (auto* prof = FgProfiler::Get()) {
        prof->Mark(cmdbuf, FgProfileCapture, "fg capture setup + depth dispatch");
    }
    cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, *pipeline);
    cmdbuf.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, *pipeline_layout, 0, writes);
    cmdbuf.pushConstants(*pipeline_layout, vk::ShaderStageFlagBits::eCompute, 0, sizeof(size),
                         size.data());
    cmdbuf.dispatch((w + 7) / 8, (h + 7) / 8, 1);
    return *active->motion_view;
}

void FrameGenerator::ZeroMotion(vk::CommandBuffer cmdbuf) {
    const vk::MemoryBarrier2 before{.srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
                                    .srcAccessMask = vk::AccessFlagBits2::eMemoryWrite,
                                    .dstStageMask = vk::PipelineStageFlagBits2::eTransfer,
                                    .dstAccessMask = vk::AccessFlagBits2::eTransferWrite};
    cmdbuf.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &before});
    const vk::ClearColorValue zero{std::array<float, 4>{0.0f, 0.0f, 0.0f, 0.0f}};
    const vk::ImageSubresourceRange range{vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};
    cmdbuf.clearColorImage(vk::Image(active->motion_image), vk::ImageLayout::eGeneral, zero, range);
    const vk::MemoryBarrier2 after{.srcStageMask = vk::PipelineStageFlagBits2::eTransfer,
                                   .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
                                   .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands,
                                   .dstAccessMask = vk::AccessFlagBits2::eMemoryRead |
                                                    vk::AccessFlagBits2::eMemoryWrite};
    cmdbuf.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &after});
}

void FrameGenerator::EndCapture(vk::CommandBuffer cmdbuf, std::array<float, 2> jitter,
                                bool motion_valid, Camera camera) {
    std::array<vk::ImageMemoryBarrier2, 3> to_read{};
    for (u32 i = 0; i < 3; ++i) {
        to_read[i] = {
            .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
            .srcAccessMask = vk::AccessFlagBits2::eShaderWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands,
            .dstAccessMask = vk::AccessFlagBits2::eShaderRead,
            .oldLayout = vk::ImageLayout::eGeneral,
            .newLayout = vk::ImageLayout::eGeneral,
            .image = i == 0   ? vk::Image(active->depth_image)
                     : i == 1 ? vk::Image(active->motion_image)
                              : vk::Image(active->mask_image),
            .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
        };
    }
    cmdbuf.pipelineBarrier2({.imageMemoryBarrierCount = 3, .pImageMemoryBarriers = to_read.data()});
    if (auto* prof = FgProfiler::Get()) {
        prof->Mark(cmdbuf, FgProfileMotion, "fg motion clear/record + capture end");
    }
    if (!motion_valid) {
        reset = true; // no camera this frame: the frames are not related
    }
    active->captured = true;
    active->camera_valid = motion_valid;
    active->capture_jitter = jitter;
    if (camera.near_plane > 0.0f && camera.far_plane > camera.near_plane &&
        camera.vertical_fov > 0.0f) {
        active->capture_camera = camera;
    }
    active->capture_camera.has_matrices = motion_valid && camera.has_matrices;
}

void FrameGenerator::CaptureHudless(vk::CommandBuffer cmdbuf, vk::Image source, vk::Format format,
                                    u32 w, u32 h, u64 address) {
    active = FindCapture(address);
    if (!Wanted() || !active || active->hudless_captured || !w || !h) {
        return; // only for a frame with the game's scene, and before its UI
    }
    if (!active->hudless_image || active->hudless_format != format || active->hudless_width != w || active->hudless_height != h) {
        // The presenter of the previous frame may still read the old copy.
        scheduler.WaitSubmitted();
        active->hudless_views.clear();
        active->hudless_image = VideoCore::UniqueImage(instance.GetDevice(), instance.GetAllocator());
        active->hudless_image.Create(vk::ImageCreateInfo{
            .flags = vk::ImageCreateFlagBits::eMutableFormat,
            .imageType = vk::ImageType::e2D,
            .format = format,
            .extent = {w, h, 1},
            .mipLevels = 1,
            .arrayLayers = 1,
            .samples = vk::SampleCountFlagBits::e1,
            .tiling = vk::ImageTiling::eOptimal,
            .usage = vk::ImageUsageFlagBits::eTransferDst | vk::ImageUsageFlagBits::eTransferSrc |
                     vk::ImageUsageFlagBits::eSampled,
            .initialLayout = vk::ImageLayout::eUndefined,
        });
        active->hudless_format = format;
        active->hudless_width = w;
        active->hudless_height = h;
    }
    const auto barrier = [&](vk::ImageLayout from, vk::ImageLayout to,
                             vk::PipelineStageFlags2 src_stage, vk::AccessFlags2 src_access,
                             vk::PipelineStageFlags2 dst_stage, vk::AccessFlags2 dst_access) {
        const vk::ImageMemoryBarrier2 b{
            .srcStageMask = src_stage,
            .srcAccessMask = src_access,
            .dstStageMask = dst_stage,
            .dstAccessMask = dst_access,
            .oldLayout = from,
            .newLayout = to,
            .image = vk::Image(active->hudless_image),
            .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
        };
        cmdbuf.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &b});
    };
    barrier(vk::ImageLayout::eUndefined, vk::ImageLayout::eTransferDstOptimal,
            vk::PipelineStageFlagBits2::eAllCommands, vk::AccessFlagBits2::eShaderRead,
            vk::PipelineStageFlagBits2::eTransfer, vk::AccessFlagBits2::eTransferWrite);
    cmdbuf.copyImage(source, vk::ImageLayout::eTransferSrcOptimal, vk::Image(active->hudless_image),
                     vk::ImageLayout::eTransferDstOptimal,
                     vk::ImageCopy{.srcSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
                                   .dstSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
                                   .extent = {w, h, 1}});
    barrier(vk::ImageLayout::eTransferDstOptimal, vk::ImageLayout::eShaderReadOnlyOptimal,
            vk::PipelineStageFlagBits2::eTransfer, vk::AccessFlagBits2::eTransferWrite,
            vk::PipelineStageFlagBits2::eAllCommands, vk::AccessFlagBits2::eShaderRead);
    active->hudless_captured = true;
    static bool reported = false;
    if (!reported) {
        reported = true;
        std::printf("Frame generation: the picture before the UI, %ux%u %s\n", w, h,
                    vk::to_string(format).c_str());
    }
}

vk::ImageView FrameGenerator::Hudless(vk::Format format, vk::Extent2D& size, u64 address) {
    active = FindCapture(address);
    if (!active || !active->hudless_captured || !active->hudless_image) {
        return {};
    }
    size = {active->hudless_width, active->hudless_height};
    format = HudlessViewFormat(active->hudless_format, format);
    for (const auto& view : active->hudless_views) {
        if (view.format == format) {
            return *view.view;
        }
    }
    active->hudless_views.push_back({
        format,
        Check(instance.GetDevice().createImageViewUnique({
            .image = vk::Image(active->hudless_image),
            .viewType = vk::ImageViewType::e2D,
            .format = format,
            .components = {vk::ComponentSwizzle::eIdentity, vk::ComponentSwizzle::eIdentity,
                           vk::ComponentSwizzle::eIdentity, vk::ComponentSwizzle::eOne},
            .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
        })),
    });
    return *active->hudless_views.back().view;
}

void FrameGenerator::ComputeFlow(vk::CommandBuffer cmdbuf, vk::ImageView picture, u32 width,
                                 u32 height, bool normalized) {
    const auto device = instance.GetDevice();
    const std::array<std::array<u32, 2>, 3> size{{
        {(width + 1) / 2, (height + 1) / 2},
        {(width + 3) / 4, (height + 3) / 4},
        {(width + 7) / 8, (height + 7) / 8},
    }};
    constexpr vk::ImageSubresourceRange range{vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};
    if (!flow[0].image || flow_width != width || flow_height != height) {
        scheduler.WaitSubmitted();
        std::array<vk::ImageMemoryBarrier2, 12> to_general{};
        u32 count = 0;
        const auto make = [&](FlowImage& out, vk::Format format, u32 w, u32 h) {
            out.view.reset();
            out.image = VideoCore::UniqueImage(device, instance.GetAllocator());
            out.image.Create(vk::ImageCreateInfo{
                .imageType = vk::ImageType::e2D,
                .format = format,
                .extent = {w, h, 1},
                .mipLevels = 1,
                .arrayLayers = 1,
                .samples = vk::SampleCountFlagBits::e1,
                .tiling = vk::ImageTiling::eOptimal,
                .usage = vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled,
                .initialLayout = vk::ImageLayout::eUndefined,
            });
            out.view = Check(device.createImageViewUnique({
                .image = vk::Image(out.image),
                .viewType = vk::ImageViewType::e2D,
                .format = format,
                .subresourceRange = range,
            }));
            to_general[count++] = {
                .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
                .srcAccessMask = vk::AccessFlagBits2::eNone,
                .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands,
                .dstAccessMask = vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite,
                .oldLayout = vk::ImageLayout::eUndefined,
                .newLayout = vk::ImageLayout::eGeneral,
                .image = vk::Image(out.image),
                .subresourceRange = range,
            };
        };
        for (u32 set = 0; set < 2; ++set) {
            for (u32 level = 0; level < 3; ++level) {
                make(pyramid[set][level], vk::Format::eR16Sfloat, size[level][0], size[level][1]);
            }
        }
        for (u32 level = 0; level < 3; ++level) {
            make(flow[level], vk::Format::eR16G16Sfloat, 1, 1);
        }
        make(cost, vk::Format::eR32G32Sfloat, 2 * kCarRadius[0] + 1, 2 * kCarRadius[0] + 1);
        cmdbuf.pipelineBarrier2({.imageMemoryBarrierCount = count, .pImageMemoryBarriers = to_general.data()});
        flow_width = width;
        flow_height = height;
        prev_valid = false;
        cur_pyramid = 0;
    }
    const auto barrier = [&](vk::PipelineStageFlags2 from_stage, vk::AccessFlags2 from_access,
                             vk::PipelineStageFlags2 to_stage, vk::AccessFlags2 to_access) {
        const vk::MemoryBarrier2 b{.srcStageMask = from_stage,
                                   .srcAccessMask = from_access,
                                   .dstStageMask = to_stage,
                                   .dstAccessMask = to_access};
        cmdbuf.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &b});
    };
    constexpr auto Compute = vk::PipelineStageFlagBits2::eComputeShader;
    const auto between = [&] {
        barrier(Compute, vk::AccessFlagBits2::eShaderWrite, Compute,
                vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite);
    };
    // The picture (a copy) and the camera's vectors were written before.
    barrier(vk::PipelineStageFlagBits2::eAllCommands, vk::AccessFlagBits2::eMemoryWrite, Compute,
            vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite);
    const auto image_info = [](vk::ImageView view) {
        return vk::DescriptorImageInfo{.imageView = view, .imageLayout = vk::ImageLayout::eGeneral};
    };
    const auto write = [](u32 binding, vk::DescriptorType type, const vk::DescriptorImageInfo* info) {
        return vk::WriteDescriptorSet{.dstBinding = binding,
                                      .descriptorCount = 1,
                                      .descriptorType = type,
                                      .pImageInfo = info};
    };
    const auto run = [&](const FlowStage& stage, std::span<const vk::WriteDescriptorSet> writes,
                         const void* push, u32 bytes, u32 w, u32 h) {
        cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, *stage.pipeline);
        cmdbuf.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, *stage.layout, 0, writes);
        cmdbuf.pushConstants(*stage.layout, vk::ShaderStageFlagBits::eCompute, 0, bytes, push);
        cmdbuf.dispatch((w + 7) / 8, (h + 7) / 8, 1);
        between();
    };
    auto& now = pyramid[cur_pyramid];
    auto& before = pyramid[cur_pyramid ^ 1];
    // The luma pyramid of this picture.
    for (u32 level = 0; level < 3; ++level) {
        const auto src = image_info(level == 0 ? picture : *now[level - 1].view);
        const auto dst = image_info(*now[level].view);
        const std::array<vk::WriteDescriptorSet, 2> writes{
            write(0, vk::DescriptorType::eSampledImage, &src),
            write(1, vk::DescriptorType::eStorageImage, &dst)};
        struct {
            s32 dst[2];
            s32 src[2];
            float color;
        } push{{s32(size[level][0]), s32(size[level][1])},
               {level == 0 ? s32(width) : s32(size[level - 1][0]),
                level == 0 ? s32(height) : s32(size[level - 1][1])},
               level == 0 ? 1.0f : 0.0f};
        run(flow_down, writes, &push, sizeof(push), size[level][0], size[level][1]);
    }
    const u32 mw = active->input_width, mh = active->input_height;
    const auto mask = image_info(*active->mask_view);
    if (prev_valid) {
        // Coarse to fine: the vector of a level starts from the one above it.
        constexpr std::array<float, 3> lambda{0.0004f, 0.0004f, 0.0004f};
        for (int level = 2; level >= 0; --level) {
            const auto cur = image_info(*now[level].view);
            const auto prev = image_info(*before[level].view);
            const auto center = image_info(level == 2 ? *flow[2].view : *flow[level + 1].view);
            const auto scores = image_info(*cost.view);
            const std::array<vk::WriteDescriptorSet, 5> writes{
                write(0, vk::DescriptorType::eSampledImage, &cur),
                write(1, vk::DescriptorType::eSampledImage, &prev),
                write(2, vk::DescriptorType::eSampledImage, &mask),
                write(3, vk::DescriptorType::eSampledImage, &center),
                write(4, vk::DescriptorType::eStorageImage, &scores)};
            const int radius = kCarRadius[level];
            struct {
                s32 size[2];
                s32 mask_size[2];
                s32 radius;
                s32 scale;
                s32 has_center;
            } push{{s32(size[level][0]), s32(size[level][1])},
                   {s32(mw), s32(mh)},
                   radius,
                   2 << level,
                   level == 2 ? 0 : 1};
            const u32 side = u32(2 * radius + 1);
            cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, *flow_cost.pipeline);
            cmdbuf.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, *flow_cost.layout, 0, writes);
            cmdbuf.pushConstants(*flow_cost.layout, vk::ShaderStageFlagBits::eCompute, 0, sizeof(push), &push);
            cmdbuf.dispatch(side, side, 1);
            between();
            const auto best = image_info(*flow[level].view);
            const std::array<vk::WriteDescriptorSet, 3> pick_writes{
                write(0, vk::DescriptorType::eSampledImage, &scores),
                write(1, vk::DescriptorType::eSampledImage, &center),
                write(2, vk::DescriptorType::eStorageImage, &best)};
            struct {
                s32 radius;
                s32 has_center;
                float lambda;
                float min_count;
            } pick{radius, level == 2 ? 0 : 1, lambda[level], 32.0f};
            cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, *flow_pick.pipeline);
            cmdbuf.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, *flow_pick.layout, 0, pick_writes);
            cmdbuf.pushConstants(*flow_pick.layout, vk::ShaderStageFlagBits::eCompute, 0, sizeof(pick), &pick);
            cmdbuf.dispatch(1, 1, 1);
            between();
        }
    }
    {
        const auto source = image_info(*flow[0].view);
        const auto out = image_info(*active->motion_view);
        const std::array<vk::WriteDescriptorSet, 3> writes{
            write(0, vk::DescriptorType::eSampledImage, &source),
            write(1, vk::DescriptorType::eStorageImage, &out),
            write(2, vk::DescriptorType::eSampledImage, &mask)};
        struct {
            s32 mot[2];
            s32 flow[2];
            float pic[2];
            float zero;
            float normalized;
        } push{{s32(mw), s32(mh)},
               {s32(size[0][0]), s32(size[0][1])},
               {float(width), float(height)},
               prev_valid ? 0.0f : 1.0f,
               normalized ? 1.0f : 0.0f};
        run(flow_final, writes, &push, sizeof(push), mw, mh);
    }
    barrier(Compute, vk::AccessFlagBits2::eShaderWrite, vk::PipelineStageFlagBits2::eAllCommands,
            vk::AccessFlagBits2::eShaderRead);
    cur_pyramid ^= 1;
    prev_valid = true;
}

void FrameGenerator::ComposeUi(vk::CommandBuffer cmdbuf, vk::ImageView color,
                               vk::ImageView hudless, const Output& output, u32 width,
                               u32 height) {
    const vk::ImageMemoryBarrier2 written{
        .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .srcAccessMask = vk::AccessFlagBits2::eShaderWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
        .dstAccessMask = vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite,
        .oldLayout = vk::ImageLayout::eGeneral,
        .newLayout = vk::ImageLayout::eGeneral,
        .image = output.image,
        .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
    };
    cmdbuf.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &written});
    const vk::DescriptorImageInfo output_info{.imageView = output.view,
                                              .imageLayout = vk::ImageLayout::eGeneral};
    const vk::DescriptorImageInfo color_info{.imageView = color,
                                             .imageLayout = vk::ImageLayout::eGeneral};
    const vk::DescriptorImageInfo hudless_info{.imageView = hudless,
                                               .imageLayout = vk::ImageLayout::eGeneral};
    const std::array<vk::WriteDescriptorSet, 3> writes{{
        {.dstBinding = 0, .descriptorCount = 1, .descriptorType = vk::DescriptorType::eStorageImage,
         .pImageInfo = &output_info},
        {.dstBinding = 1, .descriptorCount = 1, .descriptorType = vk::DescriptorType::eSampledImage,
         .pImageInfo = &color_info},
        {.dstBinding = 2, .descriptorCount = 1, .descriptorType = vk::DescriptorType::eSampledImage,
         .pImageInfo = &hudless_info},
    }};
    const std::array<s32, 2> size{s32(width), s32(height)};
    cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, *ui_pipeline);
    cmdbuf.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, *ui_pipeline_layout, 0, writes);
    cmdbuf.pushConstants(*ui_pipeline_layout, vk::ShaderStageFlagBits::eCompute, 0, sizeof(size),
                         size.data());
    cmdbuf.dispatch((width + 7) / 8, (height + 7) / 8, 1);
}


u32 FrameGenerator::Generate(vk::CommandBuffer cmdbuf, vk::Image color, vk::ImageView color_view,
                             vk::ImageUsageFlags color_usage, vk::Image hudless,
                             vk::ImageView hudless_view, u32 width, u32 height,
                             std::span<const Output> outputs, u64 address) {
    const auto now = std::chrono::steady_clock::now();
    float frame_ms = std::chrono::duration<float, std::milli>(now - last_generate).count();
    last_generate = now;
    if (!(frame_ms > 0.0f && frame_ms < 200.0f)) {
        frame_ms = 16.6f;
    }
    // Views the FSR 3.1 context took for frames the GPU has finished.
    while (fsr3 && !fsr3_pending.empty() && scheduler.IsFree(fsr3_pending.front().tick)) {
        ffxVkFsr3_3_1_6FrameGenerationContextRetireFrame(fsr3, fsr3_pending.front().frame_id);
        fsr3_pending.pop_front();
    }
    const auto& settings = BbSettings::Get();
    const int provider = settings.frame_gen;
    active = FindCapture(address);
    const bool had_capture = active != nullptr;
    if (active) {
        active->captured = false;
        active->hudless_captured = false;
    }
    if (provider != last_provider) {
        last_provider = provider;
        reset = true;
    }
    if (provider == BbSettings::FrameGenOff || !had_capture || outputs.empty() || !width ||
        !height) {
        // A frame without the game's scene (menu, movie, loading), or none wanted (paused for
        // the display): start over afterwards.
        reset = true;
        prev_valid = false;
        return 0;
    }
    // FSR 3.1 keeps the previous input: it cannot switch between frames with and without UI.
    const bool with_hudless = hudless && hudless_view;
    if (with_hudless != last_hudless) {
        last_hudless = with_hudless;
        reset = true;
        if (++hudless_changes <= 4) {
            std::printf("Frame generation: %s\n",
                        with_hudless ? "UI kept out of the generated frames"
                                     : "frames without a picture before the UI");
        }
    }
    // The composed frame is read by compute (FSR 3.1) or NGX after the post pass wrote it.
    const vk::ImageMemoryBarrier2 color_ready{
        .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .srcAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite |
                         vk::AccessFlagBits2::eShaderWrite | vk::AccessFlagBits2::eTransferWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .dstAccessMask = vk::AccessFlagBits2::eShaderRead,
        .oldLayout = vk::ImageLayout::eGeneral,
        .newLayout = vk::ImageLayout::eGeneral,
        .image = color,
        .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
    };
    cmdbuf.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &color_ready});
    if (with_hudless) {
        auto hudless_ready = color_ready;
        hudless_ready.image = hudless;
        cmdbuf.pipelineBarrier2(
            {.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &hudless_ready});
    }
    // The outputs' previous contents (presented already) are not needed.
    std::array<vk::ImageMemoryBarrier2, MaxGenerated> to_write{};
    const u32 used = std::min<u32>(u32(outputs.size()), MaxGenerated);
    for (u32 i = 0; i < used; ++i) {
        to_write[i] = {
            .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
            .srcAccessMask = vk::AccessFlagBits2::eNone,
            .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands,
            .dstAccessMask = vk::AccessFlagBits2::eShaderWrite | vk::AccessFlagBits2::eShaderRead,
            .oldLayout = vk::ImageLayout::eUndefined,
            .newLayout = vk::ImageLayout::eGeneral,
            .image = outputs[i].image,
            .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
        };
    }
    cmdbuf.pipelineBarrier2({.imageMemoryBarrierCount = used, .pImageMemoryBarriers = to_write.data()});
    if (auto* prof = FgProfiler::GetFg()) {
        prof->BeginFrame(cmdbuf);
        prof->Mark(cmdbuf, FgProfileEvaluate, "fg evaluate (FSR/DLSS)");
    }
    static const bool flow_enabled = std::getenv("BB_FG_FLOW") != nullptr;
    if (with_hudless && flow_enabled) {
        ComputeFlow(cmdbuf, hudless_view, width, height, provider == BbSettings::FrameGenDlss);
    } else {
        prev_valid = false;
    }
    static u32 dump_calls = 0;
    const bool dump_now = DumpWindow();
    if (dump_now) {
        ++dump_calls;
        DumpFrame(instance, scheduler, cmdbuf, vk::Image(active->motion_image), active->input_width,
                  active->input_height, "mv", dump_calls);
    }
    u32 generated = 0;
    if (provider == BbSettings::FrameGenDlss) {
        const u32 count = std::min(WantedCount(), used);
        // DLSS reads the real frame (a different image every game frame: it needs the previous one
        // to interpolate) and the picture before the UI beside it; this frame's UI is then drawn
        // over each generated frame (DLSS's own UI separation left the UI smeared).
        generated = GenerateDlss(cmdbuf, color, color_view, with_hudless ? hudless : vk::Image{},
                                 with_hudless ? hudless_view : vk::ImageView{}, width, height,
                                 outputs, count, frame_ms);
        if (generated && with_hudless) {
            if (auto* prof = FgProfiler::GetFg()) {
                prof->Mark(cmdbuf, FgProfileCompose, "fg compose UI over generated");
            }
            for (u32 i = 0; i < generated; ++i) {
                ComposeUi(cmdbuf, color_view, hudless_view, outputs[i], width, height);
            }
        }
    } else if (with_hudless) {
        // FSR interpolates the real frame (a different image every game frame, with its UI); this
        // frame's UI is then drawn over the result where it differs from the picture before it.
        // FSR gets the picture before the UI, so no UI is carried along with the motion; this
        // frame's UI then goes over the result (BB_FG_COLOR_INPUT=1: the frame with its UI).
        // Menus (no camera): the frame as it is, its panels dim and blur the scene behind them.
        static const bool env_ui_input = std::getenv("BB_FG_COLOR_INPUT") != nullptr;
        const bool with_ui_input = env_ui_input || !active->camera_valid;
        static bool last_ui_input = false;
        if (with_ui_input != last_ui_input) {
            last_ui_input = with_ui_input;
            reset = true; // FSR 3.1 keeps the previous input: the kind of picture must not change
        }
        generated = with_ui_input
                        ? GenerateFsr3(cmdbuf, color, color_usage, width, height, outputs[0], frame_ms)
                        : GenerateFsr3(cmdbuf, hudless, HudlessUsage, width, height, outputs[0], frame_ms);
        if (generated) {
            if (auto* prof = FgProfiler::GetFg()) {
                prof->Mark(cmdbuf, FgProfileCompose, "fg compose UI over generated");
            }
            ComposeUi(cmdbuf, color_view, hudless_view, outputs[0], width, height);
        }
    } else {
        generated = GenerateFsr3(cmdbuf, color, color_usage, width, height, outputs[0], frame_ms);
    }
    if (generated) {
        reset = false;
        // Presented after this submission: the writes go to the transfer that copies them.
        std::array<vk::ImageMemoryBarrier2, MaxGenerated> written{};
        for (u32 i = 0; i < generated; ++i) {
            written[i] = to_write[i];
            written[i].srcStageMask = vk::PipelineStageFlagBits2::eAllCommands;
            written[i].srcAccessMask = vk::AccessFlagBits2::eShaderWrite;
            written[i].dstStageMask = vk::PipelineStageFlagBits2::eAllCommands;
            written[i].dstAccessMask = vk::AccessFlagBits2::eTransferRead;
            written[i].oldLayout = vk::ImageLayout::eGeneral;
        }
        cmdbuf.pipelineBarrier2(
            {.imageMemoryBarrierCount = generated, .pImageMemoryBarriers = written.data()});
        if (dump_now) {
            DumpFrame(instance, scheduler, cmdbuf, color, width, height, "real", dump_calls);
            if (with_hudless) {
                DumpFrame(instance, scheduler, cmdbuf, hudless, width, height, "clean", dump_calls);
            }
            for (u32 i = 0; i < generated && i < 3; ++i) {
                DumpFrame(instance, scheduler, cmdbuf, outputs[i].image, width, height,
                          i == 0 ? "gen1" : i == 1 ? "gen2" : "gen3", dump_calls);
            }
        }
    } else {
        reset = true;
    }
    if (auto* prof = FgProfiler::GetFg()) {
        prof->Mark(cmdbuf, FgProfileTail, "generation queue idle between frames");
    }
    return generated;
}

void FrameGenerator::DestroyFsr3() {
    if (!fsr3) {
        return;
    }
    scheduler.WaitSubmitted();
    ffxVkFsr3_3_1_6FrameGenerationContextRetireFrame(fsr3, fsr3_frame_id);
    ffxVkFsr3_3_1_6FrameGenerationContextDestroy(fsr3);
    fsr3 = nullptr;
    fsr3_pending.clear();
}

u32 FrameGenerator::GenerateFsr3(vk::CommandBuffer cmdbuf, vk::Image color,
                                 vk::ImageUsageFlags color_usage, u32 width, u32 height,
                                 const Output& output, float frame_ms) {
    if (fsr3 && (fsr3_width != width || fsr3_height != height ||
                 fsr3_render_width != active->input_width || fsr3_render_height != active->input_height)) {
        DestroyFsr3();
    }
    if (!fsr3) {
        const FfxVkFsr3_3_1_6FrameGenerationCreateInfo create{
            .physicalDevice = instance.GetPhysicalDevice(),
            .device = instance.GetDevice(),
            .maxRenderWidth = active->input_width,
            .maxRenderHeight = active->input_height,
            .displayWidth = width,
            .displayHeight = height,
            .colorFormat = static_cast<VkFormat>(ColorFormat),
        };
        if (ffxVkFsr3_3_1_6FrameGenerationContextCreate(&create, &fsr3) !=
            FFX_VK_FSR3_3_1_6_FRAMEGEN_OK) {
            fsr3 = nullptr;
            Fail(BbSettings::FrameGenFsr3, "FSR 3.1 frame generation context creation failed");
            return 0;
        }
        fsr3_width = width;
        fsr3_height = height;
        fsr3_render_width = active->input_width;
        fsr3_render_height = active->input_height;
        reset = true;
        FfxVkFsr3_3_1_6FrameGenerationMemoryUsage usage{};
        ffxVkFsr3_3_1_6FrameGenerationContextGetMemoryUsage(fsr3, &usage);
        std::printf("Frame generation: FSR 3.1 %ux%u (depth and motion %ux%u), %.1f MB\n", width,
                    height, active->input_width, active->input_height, usage.totalUsageInBytes / 1e6);
    }
    const auto image = [](vk::Image handle, vk::Format format, u32 w, u32 h,
                          vk::ImageUsageFlags usage) {
        return FfxVkFsr3_3_1_6FrameGenerationImage{
            .image = handle,
            .format = static_cast<VkFormat>(format),
            .width = w,
            .height = h,
            .layout = VK_IMAGE_LAYOUT_GENERAL,
            .usage = static_cast<VkImageUsageFlags>(usage),
        };
    };
    const u64 id = ++fsr3_frame_id;
    FfxVkFsr3_3_1_6FrameGenerationPrepareInfo prepare{
        .commandBuffer = cmdbuf,
        .color = image(color, ColorFormat, width, height, color_usage),
        .depth = image(active->depth_image, vk::Format::eR32Sfloat, active->input_width, active->input_height, InputUsage),
        .motionVectors =
            image(active->motion_image, vk::Format::eR16G16Sfloat, active->input_width, active->input_height, InputUsage),
        .renderWidth = active->input_width,
        .renderHeight = active->input_height,
        .jitterOffsetX = active->capture_jitter[0],
        .jitterOffsetY = active->capture_jitter[1],
        .motionVectorScaleX = 1.0f,
        .motionVectorScaleY = 1.0f,
        .frameTimeMilliseconds = frame_ms,
        .minLuminance = 0.0f,
        .maxLuminance = 1.0f,
        .transferFunction = FFX_VK_FSR3_3_1_6_FRAMEGEN_TRANSFER_SRGB,
        .cameraNear = active->capture_camera.near_plane,
        .cameraFar = active->capture_camera.far_plane,
        .viewSpaceToMeters = 1.0f,
        .cameraVerticalFovRadians = active->capture_camera.vertical_fov,
        .cameraPosition = {0.0f, 0.0f, 0.0f},
        .cameraUp = {0.0f, 1.0f, 0.0f},
        .cameraRight = {1.0f, 0.0f, 0.0f},
        .cameraForward = {0.0f, 0.0f, 1.0f},
        .frameId = id,
        .reset = reset ? VK_TRUE : VK_FALSE,
    };
    if (ffxVkFsr3_3_1_6FrameGenerationContextRecordPrepare(fsr3, &prepare) !=
        FFX_VK_FSR3_3_1_6_FRAMEGEN_OK) {
        fsr3_pending.push_back({id, scheduler.CurrentTick()});
        Fail(BbSettings::FrameGenFsr3, "FSR 3.1 frame generation (prepare) failed");
        return 0;
    }
    const FfxVkFsr3_3_1_6FrameGenerationDispatchInfo dispatch{
        .commandBuffer = cmdbuf,
        .color = prepare.color,
        .output = image(output.image, ColorFormat, width, height,
                        InputUsage | vk::ImageUsageFlagBits::eTransferSrc),
        .distortionField = {},
        .displayWidth = width,
        .displayHeight = height,
        .frameTimeMilliseconds = frame_ms,
        .cameraNear = prepare.cameraNear,
        .cameraFar = prepare.cameraFar,
        .viewSpaceToMeters = 1.0f,
        .cameraVerticalFovRadians = active->capture_camera.vertical_fov,
        .minLuminance = 0.0f,
        .maxLuminance = 1.0f,
        .transferFunction = FFX_VK_FSR3_3_1_6_FRAMEGEN_TRANSFER_SRGB,
        .frameId = id,
        .reset = prepare.reset,
    };
    const auto result = ffxVkFsr3_3_1_6FrameGenerationContextRecordDispatch(fsr3, &dispatch);
    fsr3_pending.push_back({id, scheduler.CurrentTick()});
    if (result != FFX_VK_FSR3_3_1_6_FRAMEGEN_OK) {
        Fail(BbSettings::FrameGenFsr3, "FSR 3.1 frame generation (dispatch) failed");
        return 0;
    }
    return 1;
}

u32 FrameGenerator::GenerateDlss(vk::CommandBuffer cmdbuf, vk::Image color,
                                 vk::ImageView color_view, vk::Image hudless,
                                 vk::ImageView hudless_view, u32 width, u32 height,
                                 std::span<const Output> outputs, u32 count, float frame_ms) {
    (void)frame_ms; // DLSS Frame Generation measures the time itself
    Dlss* dlss = Dlss::Get();
    if (!dlss || dlss->FrameGenMaxCount() < 1) {
        Fail(BbSettings::FrameGenDlss, "DLSS Frame Generation is unavailable");
        return 0;
    }
    const Dlss::FrameGenDesc desc{width, height, ColorFormat, active->input_width, active->input_height};
    if (!dlss->HasFrameGen(desc)) {
        // The previous feature may still be used by submitted work; `cmdbuf` stays open.
        scheduler.WaitSubmitted();
        dlss->ReleaseFrameGen();
        if (!dlss->CreateFrameGen(cmdbuf, desc)) {
            Fail(BbSettings::FrameGenDlss, "DLSS Frame Generation feature creation failed");
            return 0;
        }
        reset = true;
    }
    const auto resource = [](vk::Image image, vk::ImageView view, vk::Format format, u32 w,
                             u32 h) {
        return Dlss::Resource{image, view, format, vk::ImageAspectFlagBits::eColor, w, h};
    };
    for (u32 index = 1; index <= count; ++index) {
        const bool ok = dlss->EvaluateFrameGen(
            cmdbuf, {
                        .backbuffer = resource(color, color_view, ColorFormat, width, height),
                        .depth = resource(active->depth_image, *active->depth_view, vk::Format::eR32Sfloat,
                                          active->input_width, active->input_height),
                        .motion = resource(active->motion_image, *active->motion_view, vk::Format::eR16G16Sfloat,
                                           active->input_width, active->input_height),
                        .output = resource(outputs[index - 1].image, outputs[index - 1].view,
                                           ColorFormat, width, height),
                        .hudless = hudless ? resource(hudless, hudless_view, ColorFormat, width,
                                                      height)
                                           : Dlss::Resource{},
                        .jitter_x = active->capture_jitter[0],
                        .jitter_y = active->capture_jitter[1],
                        .camera_near = active->capture_camera.near_plane,
                        .camera_far = active->capture_camera.far_plane,
                        .camera_fov = active->capture_camera.vertical_fov,
                        .depth_inverted = depth_inverted,
                        .has_matrices = active->capture_camera.has_matrices,
                        .view_to_clip = active->capture_camera.view_to_clip,
                        .clip_to_prev_clip = active->capture_camera.clip_to_prev_clip,
                        .prev_clip_to_clip = active->capture_camera.prev_clip_to_clip,
                        .reset = reset,
                        .count = count,
                        .index = index,
                    });
        if (!ok) {
            Fail(BbSettings::FrameGenDlss, "DLSS Frame Generation evaluation failed");
            return 0;
        }
    }
    return count;
}

void FrameGenerator::Fail(int provider, const char* reason) {
    auto& settings = BbSettings::Get();
    std::printf("Frame generation: %s; %s\n", reason,
                provider == BbSettings::FrameGenDlss ? "using FSR 3.1" : "off");
    settings.frame_gen_problem = reason;
    settings.frame_gen =
        provider == BbSettings::FrameGenDlss ? BbSettings::FrameGenFsr3 : BbSettings::FrameGenOff;
    reset = true;
}

} // namespace Vulkan
