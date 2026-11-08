// SPDX-License-Identifier: GPL-2.0-or-later
#include "video_core/renderer_vulkan/vk_anti_lag.h"

#include <atomic>
#include <cstdio>

namespace Vulkan::AntiLag {

namespace {
VkDevice g_device = VK_NULL_HANDLE;
PFN_vkAntiLagUpdateAMD g_update = nullptr;
std::atomic<u64> g_frame{0};

void Mark(VkAntiLagStageAMD stage, u32 max_fps) {
    const VkAntiLagPresentationInfoAMD presentation{
        .sType = VK_STRUCTURE_TYPE_ANTI_LAG_PRESENTATION_INFO_AMD,
        .stage = stage,
        .frameIndex = g_frame.load(std::memory_order_relaxed),
    };
    const VkAntiLagDataAMD data{
        .sType = VK_STRUCTURE_TYPE_ANTI_LAG_DATA_AMD,
        .mode = VK_ANTI_LAG_MODE_ON_AMD,
        .maxFPS = max_fps,
        .pPresentationInfo = &presentation,
    };
    g_update(g_device, &data);
}
} // namespace

void Initialize(vk::Device device, bool enabled) {
    g_update = nullptr;
    if (!enabled) {
        return;
    }
    g_device = device;
    g_update = reinterpret_cast<PFN_vkAntiLagUpdateAMD>(
        VULKAN_HPP_DEFAULT_DISPATCHER.vkGetDeviceProcAddr(device, "vkAntiLagUpdateAMD"));
    std::printf("GPU: AMD Anti-Lag 2 %s\n", g_update ? "on" : "off (the driver has no entry point)");
}

void Input() {
    if (!g_update) {
        return;
    }
    g_frame.fetch_add(1, std::memory_order_relaxed);
    Mark(VK_ANTI_LAG_STAGE_INPUT_AMD, 0);
}

void Present(u32 max_fps) {
    if (g_update) {
        Mark(VK_ANTI_LAG_STAGE_PRESENT_AMD, max_fps);
    }
}

bool Active() {
    return g_update != nullptr;
}

} // namespace Vulkan::AntiLag
