// SPDX-License-Identifier: GPL-2.0-or-later
#include "video_core/renderer_vulkan/vk_object_motion.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <vk_mem_alloc.h>
#include "bbport_settings.h"
#include "bbport_toggles.h"
#include "shader_recompiler/runtime_info.h"
#include "video_core/amdgpu/regs.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"

namespace Vulkan {

namespace {

bool CreateBuffer(const Instance& instance, vk::DeviceSize size, bool host, vk::Buffer& buffer,
                  VmaAllocation& allocation, void** mapped, u64& address) {
    const VkBufferCreateInfo buffer_ci{
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = size,
        .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
    };
    VmaAllocationCreateInfo alloc_ci{
        .usage = VMA_MEMORY_USAGE_AUTO,
    };
    if (host) {
        alloc_ci.requiredFlags = VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        alloc_ci.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT |
                         VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT;
    } else {
        alloc_ci.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    }
    VkBuffer raw{};
    VmaAllocationInfo info{};
    if (vmaCreateBuffer(instance.GetAllocator(), &buffer_ci, &alloc_ci, &raw, &allocation, &info) !=
        VK_SUCCESS) {
        return false;
    }
    buffer = raw;
    if (mapped) {
        *mapped = info.pMappedData;
    }
    address = instance.GetDevice().getBufferAddress({.buffer = buffer});
    return address != 0;
}

} // namespace

ObjectMotion::ObjectMotion(const Instance& instance_, Scheduler& scheduler_)
    : instance{instance_}, scheduler{scheduler_} {
    // Pipeline selection limits the extra attachment and vertex stores to likely
    // animated draws. A setting or environment override can still disable the path.
    const auto& settings = BbSettings::Get();
    if (!settings.object_motion) {
        std::puts("Object motion: off (enable in menu or BB_OBJECT_MOTION=1)");
        return;
    }
    const auto& features = instance.GetPhysicalDevice().getFeatures();
    if (!features.vertexPipelineStoresAndAtomics) {
        std::printf("Object motion: vertexPipelineStoresAndAtomics unsupported, off\n");
        return;
    }
    void* mapped = nullptr;
    u64 params_address = 0, positions_address = 0;
    if (!CreateBuffer(instance, vk::DeviceSize(1 + FrameSlots * ParamsPerFrame) * 32, true,
                      params_buffer, params_allocation, &mapped, params_address) ||
        !CreateBuffer(instance, vk::DeviceSize(1 + 2 * PositionsPerFrame) * 16, false,
                      positions_buffer, positions_allocation, nullptr, positions_address)) {
        std::printf("Object motion: buffer creation failed, off\n");
        return;
    }
    params_mapped = static_cast<u32*>(mapped);
    std::memset(params_mapped, 0, 32);
    Shader::MotionVectors::params_address = params_address;
    Shader::MotionVectors::positions_address = positions_address;
    enabled = true;
    std::printf("Object motion: on (%u vertices per frame)\n", PositionsPerFrame);
}

ObjectMotion::~ObjectMotion() {
    scheduler.Finish();
    const auto allocator = instance.GetAllocator();
    if (params_buffer) {
        vmaDestroyBuffer(allocator, params_buffer, params_allocation);
    }
    if (positions_buffer) {
        vmaDestroyBuffer(allocator, positions_buffer, positions_allocation);
    }
}

void ObjectMotion::OnFrameStart() {
    if (!enabled) {
        return;
    }
    if (diagnostic_frames) {
        --diagnostic_frames;
    }
    if (BbStats::enabled && (frame % 600) == 0 && history.stats.draws) {
        const auto& s = history.stats;
        std::printf("Object motion: %llu draws, %llu stored, %llu with history, %llu unmatched, "
                    "%llu capacity skips, %llu invalid, %llu still (600 frames); last frame %u/%u vertices; "
                    "index ranges %llu reused, %llu scanned (%llu found changed), %zu cached\n",
                    (unsigned long long)s.draws, (unsigned long long)s.stored,
                    (unsigned long long)s.loaded, (unsigned long long)s.unmatched,
                    (unsigned long long)s.exhausted, (unsigned long long)s.invalid,
                    (unsigned long long)s.still,
                    history.Used(), PositionsPerFrame, (unsigned long long)index_ranges.stats.hits,
                    (unsigned long long)index_ranges.stats.scans,
                    (unsigned long long)index_ranges.stats.stale, index_ranges.Size());
        index_ranges.stats = {};
        history.stats = {};
    }
    history.NextFrame();
    ++frame;
    if (frame % Motion::IndexRangeCache::Unused == 0) {
        index_ranges.Trim(frame);
    }
    const u32 slot = u32(frame % FrameSlots);
    // Do not overwrite host parameters still referenced by queued GPU work.
    if (params_ticks[slot]) {
        scheduler.Wait(params_ticks[slot]);
        params_ticks[slot] = 0;
    }
    params_used = 0;
    for (auto& target : images) {
        target.written = false;
    }
    scheduler.EndRendering();
    // Last frame's stores must be visible to this frame's loads. Also finish old reads
    // before the ping-pong half they reference is reused for writes.
    scheduler.Record([](vk::CommandBuffer cmd) {
        const vk::MemoryBarrier2 barrier{
            .srcStageMask = vk::PipelineStageFlagBits2::eVertexShader,
            .srcAccessMask = vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eVertexShader,
            .dstAccessMask = vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite,
        };
        cmd.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &barrier});
    });
}

bool ObjectMotion::IsCarDraw(const AmdGpu::Regs& regs) {
    const auto marks_car = [](AmdGpu::StencilFunc op, const AmdGpu::StencilRefMask& ref) {
        const u32 value = op == AmdGpu::StencilFunc::ReplaceTest ? ref.stencil_test_val :
                          op == AmdGpu::StencilFunc::ReplaceOp ? ref.stencil_op_val : 0;
        return (value & ref.stencil_write_mask & 130u) != 0;
    };
    return regs.depth_control.stencil_enable &&
           (marks_car(regs.stencil_control.stencil_zpass_front, regs.stencil_ref_front) ||
            (regs.depth_control.backface_enable &&
             marks_car(regs.stencil_control.stencil_zpass_back, regs.stencil_ref_back)));
}

void ObjectMotion::CaptureHistory() {
    const char* value = std::getenv("BB_MOTION_SELECT_LOG");
    if (value && value[0] == '1') {
        diagnostic_frames = 8;
    }
}

u32 ObjectMotion::PrepareDraw(const DrawInfo& draw, std::span<const u64> instances) {
    const u32 count = instances.empty() ? 1u : u32(instances.size());
    if (!enabled || BbToggle::Disabled(1u << 29) || count > ParamsPerFrame - params_used) {
        return 0;
    }
    const u32 slot = u32(frame % FrameSlots);
    const u32 index = 1 + slot * ParamsPerFrame + params_used;
    bool active = false;
    for (u32 i = 0; i < count; ++i) {
        const auto allocation = instances.empty() ? history.Prepare(draw)
                                                 : history.PrepareInstance(draw, instances[i]);
        if (diagnostic_frames) {
            std::printf("Motion history: frame %llu vs %016llx geometry %016llx indices %llx topology %016llx count %u instances %u first_instance %u first_vertex %u vertices %u store %u load %u\n",
                        (unsigned long long)frame, (unsigned long long)draw.shader,
                        (unsigned long long)(instances.empty() ? draw.geometry : instances[i]),
                        (unsigned long long)draw.indices, (unsigned long long)draw.topology,
                        draw.index_count, instances.empty() ? draw.instances : 1u,
                        instances.empty() ? draw.first_instance : 0u, draw.vertices.first,
                        draw.vertices.count, allocation.store, allocation.load);
        }
        u32 flags = (allocation.store ? Shader::MotionVectors::FlagStore : 0) |
                    (allocation.load ? Shader::MotionVectors::FlagLoad : 0);
        active |= flags != 0;
        u32* param = params_mapped + (index + i) * 8;
        param[0] = allocation.store;
        param[1] = allocation.load;
        param[2] = allocation.vertices;
        param[3] = flags;
        param[4] = allocation.first_vertex;
        param[5] = instances.empty() ? allocation.first_instance : draw.first_instance + i;
        param[6] = allocation.instances;
        param[7] = instances.empty() ? 0u : 1u;
    }
    params_used += count;
    if (!active) {
        return 0;
    }
    params_ticks[slot] = scheduler.CurrentTick();
    return index;
}

void ObjectMotion::EnsureImage(u32 width, u32 height) {
    if (active_image < images.size()) {
        auto& active = images[active_image];
        if (width == active.width && height == active.height) {
            active.last_use = ++image_use_serial;
            return;
        }
    }

    for (u32 i = 0; i < images.size(); ++i) {
        if (images[i].width == width && images[i].height == height) {
            active_image = i;
            images[i].last_use = ++image_use_serial;
            return;
        }
    }

    BbStats::motion_image_recreates.fetch_add(1, std::memory_order_relaxed);
    const auto device = instance.GetDevice();
    if (images.size() >= MotionImageCacheSize) {
        u32 victim = ~u32{0};
        u64 oldest_use = ~u64{0};
        for (u32 i = 0; i < images.size(); ++i) {
            if (i != active_image && images[i].last_use < oldest_use) {
                victim = i;
                oldest_use = images[i].last_use;
            }
        }
        if (victim != ~u32{0}) {
            // Keep resources alive until commands that referenced them have completed. A global
            // Finish here stalls the recording thread every time the guest changes target size.
            scheduler.DeferOperation([retired = std::move(images[victim])] {});
            images.erase(images.begin() + victim);
            if (active_image > victim) {
                --active_image;
            } else if (active_image == victim) {
                active_image = ~u32{0};
            }
        }
    }

    images.emplace_back();
    active_image = static_cast<u32>(images.size() - 1);
    auto& target = images.back();
    target.width = width;
    target.height = height;
    target.last_use = ++image_use_serial;
    target.written = false;
    target.layout = vk::ImageLayout::eUndefined;
    target.image = VideoCore::UniqueImage(device, instance.GetAllocator());
    BbStats::MotionProfileTimer image_timer{
        BbStats::motion_profile_ns[BbStats::MotionEnsureImageCreate]};
    target.image.Create(vk::ImageCreateInfo{
        .imageType = vk::ImageType::e2D,
        .format = vk::Format::eR16G16B16A16Sfloat,
        .extent = {width, height, 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = vk::SampleCountFlagBits::e1,
        .tiling = vk::ImageTiling::eOptimal,
        .usage = vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eSampled |
                 vk::ImageUsageFlagBits::eTransferSrc,
        .initialLayout = vk::ImageLayout::eUndefined,
    });
    image_timer.Stop();
    BbStats::MotionProfileTimer view_timer{
        BbStats::motion_profile_ns[BbStats::MotionEnsureViewCreate]};
    target.view = Check(device.createImageViewUnique({
        .image = vk::Image(target.image),
        .viewType = vk::ImageViewType::e2D,
        .format = vk::Format::eR16G16B16A16Sfloat,
        .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
    }));
    view_timer.Stop();
}

void ObjectMotion::Attach(RenderState& state, u32 width, u32 height) {
    BbStats::motion_attach_calls.fetch_add(1, std::memory_order_relaxed);
    BbStats::MotionProfileTimer profile_timer{
        BbStats::motion_profile_ns[BbStats::MotionAttach]};
    constexpr u32 slot = Shader::MotionVectors::Output;
    BbStats::MotionProfileTimer image_timer{
        BbStats::motion_profile_ns[BbStats::MotionAttachEnsureImage]};
    EnsureImage(width, height);
    image_timer.Stop();
    auto& target = images[active_image];
    auto& attachment = state.color_attachments[slot];
    attachment = {};
    attachment.image_view = *target.view;
    attachment.image_layout = vk::ImageLayout::eColorAttachmentOptimal;
    if (!target.written || target.layout != vk::ImageLayout::eColorAttachmentOptimal) {
        BbStats::motion_attach_transitions.fetch_add(1, std::memory_order_relaxed);
        // Clear once per frame; later reads must not erase the validity of this history.
        BbStats::MotionProfileTimer end_render_timer{
            BbStats::motion_profile_ns[BbStats::MotionAttachEndRendering]};
        scheduler.EndRendering();
        end_render_timer.Stop();
        const vk::ImageMemoryBarrier2 barrier{
            .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
            .srcAccessMask = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
            .dstAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite |
                             vk::AccessFlagBits2::eColorAttachmentRead,
            .oldLayout = target.layout,
            .newLayout = vk::ImageLayout::eColorAttachmentOptimal,
            .image = vk::Image(target.image),
            .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
        };
        // Recorded in order on the recording thread: CommandBuffer() here switched the rest of
        // the submission (most of the G-buffer) to direct recording on the GPU thread.
        BbStats::MotionProfileTimer record_timer{
            BbStats::motion_profile_ns[BbStats::MotionAttachRecord]};
        scheduler.Record([barrier](vk::CommandBuffer cmd) {
            cmd.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &barrier});
        });
        record_timer.Stop();
        target.layout = vk::ImageLayout::eColorAttachmentOptimal;
        attachment.is_clear = !target.written;
        target.written = true;
    }
    state.num_color_attachments = std::max<u16>(state.num_color_attachments, slot + 1);
}

vk::ImageView ObjectMotion::PrepareRead(vk::CommandBuffer cmdbuf, u32 width, u32 height,
                                        bool& valid) {
    Shader::MotionVectors::render_size.store(u64(width) | (u64(height) << 32),
                                             std::memory_order_relaxed);
    if (active_image >= images.size()) {
        EnsureImage(1, 1);
    }
    auto& target = images[active_image];
    target.last_use = ++image_use_serial;
    valid = target.written && width == target.width && height == target.height;
    const vk::ImageMemoryBarrier2 barrier{
        .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .srcAccessMask = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
        .dstAccessMask = vk::AccessFlagBits2::eShaderRead,
        .oldLayout = target.layout,
        .newLayout = vk::ImageLayout::eGeneral,
        .image = vk::Image(target.image),
        .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
    };
    cmdbuf.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &barrier});
    target.layout = vk::ImageLayout::eGeneral;
    return *target.view;
}

} // namespace Vulkan
