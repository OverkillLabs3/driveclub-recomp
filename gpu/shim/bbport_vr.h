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
/// The headset's refresh rate (from its frame timing), 0 before its first frame or without a headset.
uint32_t HeadsetHz();

/// Before vkCreateInstance: starts OpenXR and adds the instance extensions its runtime needs.
void AppendInstanceExtensions(std::vector<const char*>& extensions);
/// Before vkCreateDevice: the device extensions the runtime needs (`out` gets them once).
void AppendDeviceExtensions(VkInstance instance, VkPhysicalDevice physical_device, std::vector<const char*>& out);
/// After vkCreateDevice: the OpenXR session on the game's device and queue, and the head camera hook.
void OnDevice(VkInstance instance, VkPhysicalDevice physical_device, VkDevice device, uint32_t queue_family,
              uint32_t queue_index);

/// The game's projection (anti-aliasing constants, slot 1): tells the driver's head camera apart.
/// Also the depth terms (row vectors: z22 = P[2][2], z32 = P[3][2], the near plane of the reversed depth).
void NoteProjection(float x_scale, float y_scale, float z22, float z32);
/// A 512-byte constant buffer of a G-buffer draw (three colour targets): floats 108..110 are the position of the
/// camera the frame is drawn from, which names the head pose and eye of the frame.
void NoteCameraConstants(const float* data);
/// The HUD drawn apart: the rasterizer, at a frame's first HUD draw into the display buffer (the second draw there,
/// not a SceneOverlay draw) with a 3D scene drawn (SceneDrawn), calls TakeScene with the display buffer in
/// TRANSFER_SRC_OPTIMAL: it is copied out and cleared to transparent black, and stays in TRANSFER_SRC_OPTIMAL. The
/// pipeline cache makes the alpha of blended draws into the display buffer their coverage while HudLayer().
bool HudLayer();
/// The eye the frame being drawn is for (alternate frames, its camera found in the G-buffer pass), or -1: for DLSS
/// per eye.
int DrawnEye();
/// DLSS capture: the picture the headset gets for the frame being drawn is saved to `path` (+ "_eyeN_WxH_rgba8.raw").
void CapturePicture(const char* path);
/// The eye of the head hook's call that put the camera at `position` (world, within 1 cm), or -1; `distance` gets the
/// nearest call's distance. For DLSS per eye: the camera of the very frame being upscaled.
int EyeAt(const float* position, float& distance);
bool SceneOverlay(uint64_t vs_hash);
/// Diagnostics (vr.ini, without a headset too): draw_trace = 1 logs one frame's draws into the display buffer every
/// 10 s and the shaders drawn in only one eye's frames (without VR: only every other frame).
bool DrawTrace();
/// vr.ini hide_shader = <hex> ...: draws with one of these vertex or pixel shaders are skipped.
bool HiddenShader(uint64_t vs_hash, uint64_t ps_hash);
bool SceneDrawn();
/// `depth` (TRANSFER_SRC_OPTIMAL, stays so; may be null): the frame's scene depth, for reproject.
/// `stereo`: the display buffer has the right eye in layer 1 (multiview): it is copied out too.
void TakeScene(VkCommandBuffer cmd, VkImage display, VkFormat format, uint32_t width, uint32_t height, VkImage depth,
               VkFormat depth_format, uint32_t depth_width, uint32_t depth_height, bool stereo = false);
/// A presenter frame was made from the game's display buffer (`display_view`: SHADER_READ_ONLY_OPTIMAL, in
/// `display_format`, at the output resolution; null for HDR): the headset gets that picture, with the frame's camera.
/// `alpha_view`: the same with its alpha (the window's view makes it one), for the HUD's coverage.
/// Returns the view the window shows instead (the scene and the HUD put together again, or vr.ini window = 1: one
/// eye's frames at 16:9), or null; `window_width` / `window_height` get its size when it is not the display buffer's.
VkImageView NoteFrame(VkCommandBuffer cmd, VkImage frame, VkImageView display_view, VkImageView alpha_view,
                      VkFormat display_format, uint32_t display_width, uint32_t display_height,
                      uint32_t* window_width = nullptr, uint32_t* window_height = nullptr);
/// Present: `image` (the window's picture, TRANSFER_SRC_OPTIMAL) is the fallback when the frame has no picture.
void RecordFrame(VkCommandBuffer cmd, VkImage image, uint32_t width, uint32_t height);
/// After the present submission (under the queue's submit lock): the headset's layer goes out.
void AfterSubmit();

} // namespace BbVr
