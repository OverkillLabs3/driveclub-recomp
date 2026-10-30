// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: in-game settings menu (Dear ImGui), drawn by the presenter into the swapchain image
// after the game frame, at display resolution. Insert (keyboard) or L3+R3 (gamepad) opens it;
// while it is open the game gets no pad/keyboard input. Settings live in bbport_settings.h.

#pragma once

#include <string>
#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"

union SDL_Event;
struct SDL_Window;

namespace Vulkan {
class Instance;
}

namespace BbOverlay {

/// Present thread, once: the ImGui context and its Vulkan backend.
void Init(const Vulkan::Instance& instance, vk::Format format, u32 image_count);
/// After presentation has stopped and the device is idle, before device destruction.
void Shutdown();

/// Window thread, for every SDL event: true when the menu consumed it.
bool HandleEvent(const SDL_Event& event);
/// Turns SDL text input on while the menu edits a value (window thread, once per poll).
void UpdateTextInput(SDL_Window* window);

/// Whether anything is drawn this frame (menu open or FPS counter on).
bool Visible();

/// Present thread: draws into `view` (layout ColorAttachmentOptimal).
void Render(vk::CommandBuffer cmdbuf, vk::ImageView view, vk::Extent2D extent);

/// Every presented image, generated frames included (the Render call measures the interval).
void NotePresent();

/// The menu is open or a text entry is shown: the game's input is held neutral.
bool CapturesInput();
bool ExitDialogAvailable();
void RequestMouseCapture(bool capture);
void UpdateMouseCapture(SDL_Window* window);

/// Window thread: the game's text dialog (sceImeDialog). Shown as a box on screen until
/// `active` is false; typing goes to the window (sdl_window), the box only displays it.
void SetTextEntry(bool active, const std::string& prompt, const std::string& text);

/// Any thread: a short line shown at the top of the screen for a few seconds (a hotkey's new value).
void Note(const std::string& text);
/// A trophy banner: grade 0 platinum, 1 gold, 2 silver, 3 bronze.
void Trophy(int id, int grade, const std::string& title, const std::string& description);

} // namespace BbOverlay
