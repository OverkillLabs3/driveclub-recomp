// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <chrono>
#include <thread>
#include <memory>
#include <span>
#include <vector>

#include <boost/container/small_vector.hpp>
#include <boost/container/static_vector.hpp>

#include "shader_recompiler/resource.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_pipeline_cache.h"
#include "video_core/renderer_vulkan/vk_pipeline_common.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"

namespace Vulkan {

Pipeline::Pipeline(const Instance& instance_, Scheduler& scheduler_, DescriptorHeap& desc_heap_,
                   const Shader::Profile& profile_, vk::PipelineCache pipeline_cache,
                   bool is_compute_ /*= false*/)
    : instance{instance_}, scheduler{scheduler_}, desc_heap{desc_heap_}, profile{profile_},
      is_compute{is_compute_} {}

Pipeline::~Pipeline() = default;

bool RetryPipelineCreation(vk::Result result, u32 attempt, const char* kind, const char* name) {
    constexpr u32 MaxAttempts = 5;
    const bool may_recover = result == vk::Result::eErrorUnknown ||
                             result == vk::Result::eErrorOutOfHostMemory ||
                             result == vk::Result::eErrorOutOfDeviceMemory ||
                             result == vk::Result::eErrorInitializationFailed;
    if (!may_recover || attempt >= MaxAttempts) {
        return false;
    }
    LOG_WARNING(Render_Vulkan, "Creating {} pipeline {} failed with {} (attempt {}), trying again",
                kind, name, vk::to_string(result), attempt);
    std::this_thread::sleep_for(std::chrono::milliseconds{100} * attempt);
    return true;
}


void Pipeline::BindResources(std::span<vk::WriteDescriptorSet> set_writes,
                             const Shader::PushData& push_data) const {
    const auto bind_point =
        IsCompute() ? vk::PipelineBindPoint::eCompute : vk::PipelineBindPoint::eGraphics;
    const auto stage_flags = IsCompute() ? vk::ShaderStageFlagBits::eCompute : AllGraphicsStageBits;
    const vk::PipelineLayout layout = *pipeline_layout;
    scheduler.Record([layout, stage_flags, push_data](vk::CommandBuffer cmdbuf) {
        cmdbuf.pushConstants(layout, stage_flags, 0u, sizeof(push_data), &push_data);
    });

    // Bind descriptor set.
    if (set_writes.empty()) {
        return;
    }

    if (uses_push_descriptors) {
        if (!scheduler.IsRecordingDeferred()) {
            scheduler.Record([&](vk::CommandBuffer cmdbuf) {
                cmdbuf.pushDescriptorSetKHR(bind_point, layout, 0, set_writes);
            });
            return;
        }
        // Writes and the infos they point to are laid out in the recording chunk in contiguous
        // arrays (one allocation per kind instead of one per descriptor write).
        size_t num_buffers = 0, num_images = 0, num_views = 0;
        for (const auto& write : set_writes) {
            if (write.pBufferInfo) {
                num_buffers += write.descriptorCount;
            }
            if (write.pImageInfo) {
                num_images += write.descriptorCount;
            }
            if (write.pTexelBufferView) {
                num_views += write.descriptorCount;
            }
        }
        const size_t bytes = set_writes.size() * sizeof(vk::WriteDescriptorSet) +
                             num_buffers * sizeof(vk::DescriptorBufferInfo) +
                             num_images * sizeof(vk::DescriptorImageInfo) +
                             num_views * sizeof(vk::BufferView) + 128;
        // ssport: a shader can bind descriptor arrays far larger than one recording chunk
        // (128 KiB). Those writes and their infos are owned by the command on the heap.
        if (bytes + 1024 > RecordChunk::Capacity) {
            struct HeapWrites {
                std::vector<vk::WriteDescriptorSet> writes;
                std::vector<std::vector<vk::DescriptorBufferInfo>> buffers;
                std::vector<std::vector<vk::DescriptorImageInfo>> images;
                std::vector<std::vector<vk::BufferView>> texel_views;
            };
            auto heap = std::make_shared<HeapWrites>();
            heap->writes.assign(set_writes.begin(), set_writes.end());
            heap->buffers.resize(heap->writes.size());
            heap->images.resize(heap->writes.size());
            heap->texel_views.resize(heap->writes.size());
            for (size_t i = 0; i < heap->writes.size(); ++i) {
                auto& write = heap->writes[i];
                if (write.pBufferInfo) {
                    heap->buffers[i].assign(write.pBufferInfo, write.pBufferInfo + write.descriptorCount);
                    write.pBufferInfo = heap->buffers[i].data();
                }
                if (write.pImageInfo) {
                    heap->images[i].assign(write.pImageInfo, write.pImageInfo + write.descriptorCount);
                    write.pImageInfo = heap->images[i].data();
                }
                if (write.pTexelBufferView) {
                    heap->texel_views[i].assign(write.pTexelBufferView,
                                                write.pTexelBufferView + write.descriptorCount);
                    write.pTexelBufferView = heap->texel_views[i].data();
                }
            }
            scheduler.Record([bind_point, layout, heap](vk::CommandBuffer cmdbuf) {
                cmdbuf.pushDescriptorSetKHR(bind_point, layout, 0, heap->writes);
            });
            return;
        }
        scheduler.ReserveRecordData(bytes);
        auto* patched = scheduler.AllocateRecordArray<vk::WriteDescriptorSet>(set_writes.size());
        std::memcpy(patched, set_writes.data(), set_writes.size() * sizeof(vk::WriteDescriptorSet));
        auto* buf_dst = scheduler.AllocateRecordArray<vk::DescriptorBufferInfo>(num_buffers);
        auto* img_dst = scheduler.AllocateRecordArray<vk::DescriptorImageInfo>(num_images);
        auto* view_dst = scheduler.AllocateRecordArray<vk::BufferView>(num_views);
        for (size_t i = 0; i < set_writes.size(); ++i) {
            auto& write = patched[i];
            const u32 count = write.descriptorCount;
            if (write.pBufferInfo) {
                if (count == 1) {
                    *buf_dst = *write.pBufferInfo;
                } else {
                    std::memcpy(buf_dst, write.pBufferInfo, count * sizeof(vk::DescriptorBufferInfo));
                }
                write.pBufferInfo = buf_dst;
                buf_dst += count;
            }
            if (write.pImageInfo) {
                if (count == 1) {
                    *img_dst = *write.pImageInfo;
                } else {
                    std::memcpy(img_dst, write.pImageInfo, count * sizeof(vk::DescriptorImageInfo));
                }
                write.pImageInfo = img_dst;
                img_dst += count;
            }
            if (write.pTexelBufferView) {
                if (count == 1) {
                    *view_dst = *write.pTexelBufferView;
                } else {
                    std::memcpy(view_dst, write.pTexelBufferView, count * sizeof(vk::BufferView));
                }
                write.pTexelBufferView = view_dst;
                view_dst += count;
            }
        }
        const std::span<const vk::WriteDescriptorSet> writes{patched, set_writes.size()};
        scheduler.Record([bind_point, layout, writes](vk::CommandBuffer cmdbuf) {
            cmdbuf.pushDescriptorSetKHR(bind_point, layout, 0, writes);
        });
        return;
    }

    // Descriptor set updates are device calls: they stay on this thread.
    const auto desc_set = desc_heap.Commit(*desc_layout);
    for (auto& set_write : set_writes) {
        set_write.dstSet = desc_set;
    }
    instance.GetDevice().updateDescriptorSets(set_writes, {});
    scheduler.Record([bind_point, layout, desc_set](vk::CommandBuffer cmdbuf) {
        cmdbuf.bindDescriptorSets(bind_point, layout, 0, desc_set, {});
    });
}

std::string Pipeline::GetDebugString() const {
    std::string stage_desc;
    for (const auto& stage : stages) {
        if (stage) {
            const auto shader_name = PipelineCache::GetShaderName(stage->hw_stage, stage->pgm_hash);
            if (stage_desc.empty()) {
                stage_desc = shader_name;
            } else {
                stage_desc = fmt::format("{},{}", stage_desc, shader_name);
            }
        }
    }
    return stage_desc;
}

} // namespace Vulkan
