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
#include "video_core/renderer_vulkan/vr/vr_multiview.h" // VR multiview
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


void Pipeline::BindResources(DescriptorWrites& set_writes,
                             const Shader::PushData& push_data) const {
    const auto bind_point =
        IsCompute() ? vk::PipelineBindPoint::eCompute : vk::PipelineBindPoint::eGraphics;
    const auto stage_flags = IsCompute() ? vk::ShaderStageFlagBits::eCompute : AllGraphicsStageBits;
    const vk::PipelineLayout layout = *pipeline_layout;
    const Shader::PushData push = Vr::Multiview::Adjust(push_data, IsCompute()); // VR multiview: the eyes
    scheduler.Record([layout, stage_flags, push](vk::CommandBuffer cmdbuf) {
        cmdbuf.pushConstants(layout, stage_flags, 0u, sizeof(push), &push);
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
        // Writes and the infos they point to are laid out in the recording chunk, with the
        // pointers already aimed at those copies: the command only carries a span.
        size_t bytes = set_writes.size() * sizeof(vk::WriteDescriptorSet) + 64;
        for (const auto& write : set_writes) {
            bytes += write.descriptorCount *
                         (sizeof(vk::DescriptorBufferInfo) + sizeof(vk::DescriptorImageInfo) +
                          sizeof(vk::BufferView)) +
                     32;
        }
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
        const auto writes = scheduler.RecordData(std::span<const vk::WriteDescriptorSet>{set_writes});
        auto* patched = const_cast<vk::WriteDescriptorSet*>(writes.data());
        for (size_t i = 0; i < writes.size(); ++i) {
            auto& write = patched[i];
            if (write.pBufferInfo) {
                write.pBufferInfo = scheduler.RecordData(std::span{write.pBufferInfo, write.descriptorCount}).data();
            }
            if (write.pImageInfo) {
                write.pImageInfo = scheduler.RecordData(std::span{write.pImageInfo, write.descriptorCount}).data();
            }
            if (write.pTexelBufferView) {
                write.pTexelBufferView =
                    scheduler.RecordData(std::span{write.pTexelBufferView, write.descriptorCount}).data();
            }
        }
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
