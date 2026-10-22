// SPDX-License-Identifier: GPL-2.0-or-later
// VR (experimental, off by default): the game in an OpenXR headset. The headset moves the driver's head camera (a
// hook in the game's camera update, DriveClub 1.28), stereo comes from alternate frames (one frame drawn from the left
// eye, the next from the right), and each frame goes to its eye with the exact head pose it was drawn from. Menus and
// other cameras are a still screen in front of the player. Setup and settings: packaging/vr/README.md.
//
// On only with BB_VR=1 (the launcher's VR switch) or a vr.ini next to DriveClubGame.exe, and only once an OpenXR
// headset answers; otherwise every entry point returns at once and nothing else changes.
#pragma once

#include <cstdint>
#include <mutex>
#include <vector>

#include "video_core/renderer_vulkan/vk_common.h"

namespace BbVr {

/// Every queue submission of the renderer is made under this mutex; set by the presenter.
extern std::mutex* g_queue_mutex;
/// VR asked for (BB_VR or vr.ini), read once.
bool Wanted();
/// A headset session exists: the per-frame entry points have work to do.
bool Active();

/// Before vkCreateInstance: starts OpenXR and adds the instance extensions its runtime needs.
void AppendInstanceExtensions(std::vector<const char*>& extensions);
/// Before vkCreateDevice: the device extensions the runtime needs (`out` gets them once).
void AppendDeviceExtensions(VkInstance instance, VkPhysicalDevice physical_device, std::vector<const char*>& out);
/// After vkCreateDevice: the OpenXR session on the game's device and queue, and the head camera hook.
void OnDevice(VkInstance instance, VkPhysicalDevice physical_device, VkDevice device, uint32_t queue_family,
              uint32_t queue_index);

/// The game's projection (anti-aliasing constants, slot 1): tells the driver's head camera apart.
void NoteProjection(float x_scale, float y_scale);
/// A 512-byte constant buffer of a G-buffer draw (three colour targets): floats 108..110 are the position of the
/// camera the frame is drawn from, which names the head pose and eye of the frame.
void NoteCameraConstants(const float* data);
/// A presenter frame was made from the game's display buffer (`display_view`: SHADER_READ_ONLY_OPTIMAL, in
/// `display_format`, at the output resolution; null for HDR): the headset gets that picture, with the frame's camera.
void NoteFrame(VkCommandBuffer cmd, VkImage frame, VkImageView display_view, VkFormat display_format,
               uint32_t display_width, uint32_t display_height);
/// Present: `image` (the window's picture, TRANSFER_SRC_OPTIMAL) is the fallback when the frame has no picture.
void RecordFrame(VkCommandBuffer cmd, VkImage image, uint32_t width, uint32_t height);
/// After the present submission (under the queue's submit lock): the headset's layer goes out.
void AfterSubmit();

} // namespace BbVr
