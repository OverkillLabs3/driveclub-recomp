// SPDX-License-Identifier: GPL-2.0-or-later
#include "bbport_frame_info.h"
#include <string>
#include <vector>
#include "bbport_overlay.h"
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#endif

#include <atomic>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <mutex>

#include <SDL3/SDL.h>
#include "bbport_settings.h"
#include "imgui.h"
#include "bbport_scene_debug.h"
#include "imgui_impl_vulkan.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"

// DejaVu Sans (Cyrillic), embedded (third_party/fonts, Bitstream Vera license).
#ifdef _WIN32
asm(".section .rdata,\"dr\"\n"
    ".balign 16\n"
    ".global bb_font_ttf\n"
    "bb_font_ttf:\n"
    ".incbin \"" BB_FONT_PATH "\"\n"
    ".global bb_font_ttf_end\n"
    "bb_font_ttf_end:\n"
    ".text\n");
#else
asm(".section .rodata\n"
    ".balign 16\n"
    ".hidden bb_font_ttf\n"
    ".global bb_font_ttf\n"
    "bb_font_ttf:\n"
    ".incbin \"" BB_FONT_PATH "\"\n"
    ".hidden bb_font_ttf_end\n"
    ".global bb_font_ttf_end\n"
    "bb_font_ttf_end:\n"
    ".previous\n");
#endif
extern "C" const unsigned char bb_font_ttf[];
extern "C" const unsigned char bb_font_ttf_end[];


static bool g_aa_used = false;
static bool g_fg_used = false;

namespace BbOverlay {

namespace {

std::mutex imgui_mutex; // the ImGui context: window thread (input) and present thread
bool initialized = false;
// The launcher sets everything up; there is no in-game menu.
constexpr bool menu_open = false;
// The game's text dialog (SetTextEntry), guarded by imgui_mutex.
bool text_entry_active = false;
std::string text_entry_prompt, text_entry_text;
// A hotkey's note (Note), guarded by imgui_mutex: shown until note_until.
std::string note_text;
std::chrono::steady_clock::time_point note_until{};
std::atomic<bool> note_active{false};
float base_scale = 1.0f;

// Present rate for the FPS counter.
std::chrono::steady_clock::time_point last_present{};
float frame_ms_avg = 0.0f;

ImGuiKey KeyFromSdl(SDL_Keycode key) {
    switch (key) {
    case SDLK_TAB: return ImGuiKey_Tab;
    case SDLK_LEFT: return ImGuiKey_LeftArrow;
    case SDLK_RIGHT: return ImGuiKey_RightArrow;
    case SDLK_UP: return ImGuiKey_UpArrow;
    case SDLK_DOWN: return ImGuiKey_DownArrow;
    case SDLK_PAGEUP: return ImGuiKey_PageUp;
    case SDLK_PAGEDOWN: return ImGuiKey_PageDown;
    case SDLK_HOME: return ImGuiKey_Home;
    case SDLK_END: return ImGuiKey_End;
    case SDLK_DELETE: return ImGuiKey_Delete;
    case SDLK_BACKSPACE: return ImGuiKey_Backspace;
    case SDLK_SPACE: return ImGuiKey_Space;
    case SDLK_RETURN: return ImGuiKey_Enter;
    case SDLK_KP_ENTER: return ImGuiKey_KeypadEnter;
    case SDLK_ESCAPE: return ImGuiKey_Escape;
    case SDLK_LCTRL: return ImGuiKey_LeftCtrl;
    case SDLK_RCTRL: return ImGuiKey_RightCtrl;
    case SDLK_LSHIFT: return ImGuiKey_LeftShift;
    case SDLK_RSHIFT: return ImGuiKey_RightShift;
    case SDLK_LALT: return ImGuiKey_LeftAlt;
    case SDLK_RALT: return ImGuiKey_RightAlt;
    default: return ImGuiKey_None;
    }
}

ImGuiKey KeyFromGamepad(u8 button) {
    switch (button) {
    case SDL_GAMEPAD_BUTTON_SOUTH: return ImGuiKey_GamepadFaceDown;
    case SDL_GAMEPAD_BUTTON_EAST: return ImGuiKey_GamepadFaceRight;
    case SDL_GAMEPAD_BUTTON_WEST: return ImGuiKey_GamepadFaceLeft;
    case SDL_GAMEPAD_BUTTON_NORTH: return ImGuiKey_GamepadFaceUp;
    case SDL_GAMEPAD_BUTTON_DPAD_UP: return ImGuiKey_GamepadDpadUp;
    case SDL_GAMEPAD_BUTTON_DPAD_DOWN: return ImGuiKey_GamepadDpadDown;
    case SDL_GAMEPAD_BUTTON_DPAD_LEFT: return ImGuiKey_GamepadDpadLeft;
    case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: return ImGuiKey_GamepadDpadRight;
    case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER: return ImGuiKey_GamepadL1;
    case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: return ImGuiKey_GamepadR1;
    case SDL_GAMEPAD_BUTTON_START: return ImGuiKey_GamepadStart;
    case SDL_GAMEPAD_BUTTON_BACK: return ImGuiKey_GamepadBack;
    default: return ImGuiKey_None;
    }
}

float PixelDensity(SDL_WindowID id) {
    SDL_Window* window = SDL_GetWindowFromID(id);
    const float density = window ? SDL_GetWindowPixelDensity(window) : 1.0f;
    return density > 0.0f ? density : 1.0f;
}

// The game's text dialog: what is typed, and how to finish (keyboard or controller).
void TextEntryBox() {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x * 0.5f,
                                   viewport->WorkPos.y + viewport->WorkSize.y * 0.42f),
                            ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSizeConstraints(ImVec2(viewport->WorkSize.x * 0.32f, 0.0f),
                                        ImVec2(viewport->WorkSize.x * 0.8f, FLT_MAX));
    ImGui::SetNextWindowBgAlpha(0.92f);
    ImGui::Begin("##text_entry", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                     ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav |
                     ImGuiWindowFlags_NoFocusOnAppearing);
    ImGui::TextColored(ImVec4(0.85f, 0.72f, 0.45f, 1.0f), "%s", text_entry_prompt.c_str());
    ImGui::Separator();
    ImGui::SetWindowFontScale(1.4f);
    ImGui::Text("%s_", text_entry_text.c_str());
    ImGui::SetWindowFontScale(1.0f);
    ImGui::Separator();
    ImGui::TextDisabled("Keyboard: type, Backspace to erase, Enter = OK, Esc = cancel");
    ImGui::TextDisabled("Controller: Cross (A) = OK, Circle (B) = cancel");
    ImGui::End();
}

/// The in-game keys. Called from Render() before the visibility check: F12 hides the overlay,
/// and everything here has to keep working while nothing of ours is drawn.
void PollHotkeys() {
#ifdef _WIN32
    auto& s = BbSettings::Get();
    const auto edge = [](const int key, bool& down) {
        const bool now = (GetAsyncKeyState(key) & 0x8000) != 0;
        const bool pressed = now && !down;
        down = now;
        return pressed;
    };
    static bool f3_down = false;
    if (edge(VK_F3, f3_down) && SDL_GetKeyboardFocus()) {
        const int reference = s.motion_blur_reference == 30 ? 60 : 30;
        s.motion_blur_reference = reference;
        Note(reference == 30 ? "Motion blur: Slow (30 FPS exposure)  F3: Fast"
                             : "Motion blur: Fast (60 FPS exposure)  F3: Slow");
        std::printf("MotionBlur: F3 %s\n", reference == 30 ? "Slow" : "Fast");
        std::fflush(stdout);
    }
    static bool f12_down = false;
    if (edge(VK_F12, f12_down)) {
        if (!SceneDebug::Enabled()) {
            s.show_fps = !s.show_fps;
        } else if (!s.show_fps) {
            SceneDebug::expanded = false;
            s.show_fps = true;
        } else if (!SceneDebug::expanded.load()) {
            SceneDebug::expanded = true;
        } else {
            SceneDebug::expanded = false;
            s.show_fps = false;
        }
    }
    // F9: anti-aliasing on and off, F10: FSR 3.1 and DLAA (for comparing).
    static bool f9_down = false, f10_down = false;
    static int remembered = BbSettings::AaFsr3;
    const bool f9 = (GetAsyncKeyState(VK_F9) & 0x8000) != 0;
    const bool f10 = (GetAsyncKeyState(VK_F10) & 0x8000) != 0;
    if (s.aa_mode != BbSettings::AaOff) {
        remembered = s.aa_mode;
        g_aa_used = true;
    }
    if (f9 && !f9_down) {
        if (s.aa_scale != BbSettings::AaScaleNative && s.aa_mode != BbSettings::AaOff) {
            // Upscaling: the game draws at the smaller size from the start; without FSR or DLSS
            // there would only be that picture stretched, not the full resolution.
            Note(s.aa_mode == BbSettings::AaDlss
                     ? "Upscaling is on: DLSS stays on (change Upscaling in the launcher)"
                     : "Upscaling is on: FSR stays on (change Upscaling in the launcher)");
        } else {
            s.aa_mode = s.aa_mode != BbSettings::AaOff ? int(BbSettings::AaOff) : remembered;
        }
    }
    if (f10 && !f10_down && s.dlss_supported) { // DLSS only where this card can run it
        remembered = remembered == BbSettings::AaDlss ? int(BbSettings::AaFsr3)
                                                      : int(BbSettings::AaDlss);
        if (s.aa_mode != BbSettings::AaOff) {
            s.aa_mode = remembered;
        }
    }
    f9_down = f9;
    f10_down = f10;
    static bool f11_down = false;
    const bool f11 = (GetAsyncKeyState(VK_F11) & 0x8000) != 0;
    if (f11 && !f11_down && s.aa_mode == BbSettings::AaDlss) {
        const int preset = s.dlss_model.load();
        s.dlss_model = preset < 10 ? 10 : preset < 13 ? preset + 1 : 0;
    }
    f11_down = f11;
    // Page Up / Page Down: HDR brightness (SDR white) in steps of 25 nits, kept for the next start.
    static bool page_up_down = false, page_down_down = false;
    const bool brighter = edge(VK_PRIOR, page_up_down), darker = edge(VK_NEXT, page_down_down);
    if (s.hdr_output && (brighter || darker)) {
        const int now = int(std::lround(s.hdr_paper.load() / 25.0f)) * 25;
        const int next = std::clamp(now + (brighter ? 25 : -25), 100, 600);
        s.hdr_paper = float(next);
        BbSettings::SaveHdrBrightness(next);
        Note("HDR brightness " + std::to_string(next) + " nits   (Page Up / Page Down)");
    }
    // F8: frame generation paused and resumed (everything stays set up).
    static bool f8_down = false;
    const bool f8 = (GetAsyncKeyState(VK_F8) & 0x8000) != 0;
    if (s.frame_gen != BbSettings::FrameGenOff) {
        g_fg_used = true;
        if (f8 && !f8_down) {
            s.frame_gen_paused = !s.frame_gen_paused;
        }
    }
    f8_down = f8;
#endif
}

void NoteBox() {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x * 0.5f, viewport->WorkPos.y + 40.0f * base_scale),
                            ImGuiCond_Always, ImVec2(0.5f, 0.0f));
    ImGui::SetNextWindowBgAlpha(0.75f);
    ImGui::Begin("##note", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs |
                     ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoFocusOnAppearing);
    ImGui::SetWindowFontScale(1.3f);
    ImGui::Text("%s", note_text.c_str());
    ImGui::SetWindowFontScale(1.0f);
    ImGui::End();
}

void FpsCounter() {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const float pad = 12.0f * base_scale;
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x - pad,
                                   viewport->WorkPos.y + pad),
                            ImGuiCond_Always, ImVec2(1.0f, 0.0f));
    // Keep the right-anchored panel from moving when a counter gains a digit.
    const bool expanded_view = SceneDebug::Enabled() && SceneDebug::expanded.load();
    const bool showing_game_fps = BbSettings::Get().frame_gen != BbSettings::FrameGenOff;
    const float panel_width = expanded_view && showing_game_fps ? 620.0f : expanded_view ? 520.0f : 480.0f;
    ImGui::SetNextWindowSize(ImVec2(panel_width * base_scale, 0.0f),
                             ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.5f);
    ImGui::Begin("##fps", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                     ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav |
                     ImGuiWindowFlags_NoFocusOnAppearing);
    const char* fps_hint = nullptr;
    float fps_hint_y = 0.0f;
    {
        auto& s = BbSettings::Get();
        const auto text_now = std::chrono::steady_clock::now();
        const bool expanded = SceneDebug::Enabled() && SceneDebug::expanded.load();
        static float text_frame_ms = 0, text_game_fps = 0;
        static FrameTimingStats::Lows lows;
        static auto last_text_update = std::chrono::steady_clock::time_point{};
        static bool last_expanded = false;
        if (text_now - last_text_update >= std::chrono::milliseconds(100) ||
            expanded != last_expanded) {
            last_text_update = text_now;
            last_expanded = expanded;
            text_frame_ms = frame_ms_avg;
            text_game_fps = s.game_fps.load();
            if (expanded) lows = SceneDebug::frame_stats.Get(text_now);
        }
        const float shown = text_frame_ms > 0.0f ? 1000.0f / text_frame_ms : 0.0f;
        // Fixed cells prevent changing numeric widths from shifting neighboring counters.
        const auto cell = [](const char* text, float width, bool right_align = false) {
            const ImVec2 pos = ImGui::GetCursorScreenPos();
            const ImVec2 text_size = ImGui::CalcTextSize(text);
            const float pixels = width * base_scale;
            ImGui::Dummy(ImVec2(pixels, text_size.y));
            ImGui::GetWindowDrawList()->AddText(
                ImVec2(pos.x + (right_align ? std::max(0.0f, pixels - text_size.x) : 0.0f), pos.y),
                ImGui::GetColorU32(ImGuiCol_Text), text);
        };
        char value[64];
        std::snprintf(value, sizeof(value), "%.0f FPS", shown);
        cell(value, 68.0f, true);
        if (!expanded) {
            ImGui::SameLine(0, 12.0f * base_scale);
            std::snprintf(value, sizeof(value), "%.1f ms", text_frame_ms);
            cell(value, 70.0f, true);
        }
        if (s.frame_gen != BbSettings::FrameGenOff && s.frame_gen_now.load() > 0 && text_game_fps > 0) {
            ImGui::SameLine(0, 8.0f * base_scale);
            std::snprintf(value, sizeof(value), "(game %.0f)", text_game_fps);
            cell(value, 90.0f, true);
        }
        if (expanded) {
            const auto divider = [] {
                ImGui::SameLine(0, 10.0f * base_scale);
                ImGui::TextDisabled("|");
                ImGui::SameLine(0, 10.0f * base_scale);
            };
            divider();
            if (lows.ready) std::snprintf(value, sizeof(value), "1%% low %.1f", lows.one);
            else std::snprintf(value, sizeof(value), "1%% low --");
            cell(value, 112.0f);
            divider();
            if (lows.ready) std::snprintf(value, sizeof(value), "0.1%% low %.1f", lows.point_one);
            else std::snprintf(value, sizeof(value), "0.1%% low --");
            cell(value, 125.0f);
        }
        const char* hint = expanded ? "F12: hide" : SceneDebug::Enabled() ? "F12: expand" : "F12: hide";
        ImGui::SameLine(0, 16.0f * base_scale);
        fps_hint = hint;
        fps_hint_y = ImGui::GetCursorScreenPos().y;
        // Reserve space without feeding the previous width into auto-sizing.
        ImGui::Dummy(ImGui::CalcTextSize(hint));
        if (expanded) SceneDebug::DrawFrameTimes(base_scale);
        if (s.frame_gen != BbSettings::FrameGenOff) {
            if (s.frame_gen_paused) {
                ImGui::Text("Frame generation: paused  (F8 on)");
            } else {
                ImGui::Text("Frame generation: %s %dx  (F8 pause)",
                            s.frame_gen == BbSettings::FrameGenDlss ? "DLSS" : "FSR 3.1",
                            s.frame_gen == BbSettings::FrameGenDlss ? s.frame_gen_multiplier.load() : 2);
            }
        }
        if (s.aa_mode != BbSettings::AaOff) {
            const bool up = s.aa_scale != BbSettings::AaScaleNative;
            // With upscaling F9 does not turn it off (see PollHotkeys).
            const char* keys = up ? (s.dlss_supported ? "F10 FSR/DLSS" : "")
                                  : (s.dlss_supported ? "F9 off, F10 FSR/DLSS" : "F9 off");
            ImGui::Text("%s: %s%s%s%s%s%s%s", up ? "Upscaling" : "AA",
                        s.aa_effective == BbSettings::AaDlss ? (up ? "DLSS" : "DLAA")
                            : s.aa_effective == BbSettings::AaFsr411 ? "FSR 4.1.1" : "FSR 3.1.5",
                        up ? " " : "", up ? BbSettings::AaScaleName(s.aa_scale) : "",
                        s.aa_running ? " on" : " waiting", *keys ? "  (" : "", keys, *keys ? ")" : "");
            if (s.aa_mode == BbSettings::AaDlss) {
                const int preset = s.dlss_model.load();
                const char* name = preset == 10 ? "J" : preset == 11 ? "K" : preset == 12 ? "L" : preset == 13 ? "M" : "Auto";
                ImGui::Text("DLSS SR preset: %s  (F11)", name);
            }
        } else if (g_aa_used) {
            ImGui::Text("AA: game  (F9 on)");
        }
    }
    static unsigned shown_w, shown_h, frames;
    if (++frames % 60 == 0) {
        shown_w = BbFrame::scene_w.exchange(0);
        shown_h = BbFrame::scene_h.exchange(0);
        if (shown_w == 0) {  // no scene of another size than the output: it is drawn at the output size
            shown_w = BbFrame::output_w.load();
            shown_h = BbFrame::output_h.load();
        }
    }
    ImGui::Text("Render %ux%u  Output %ux%u  Screen %.0fx%.0f", shown_w, shown_h, BbFrame::output_w.load(), BbFrame::output_h.load(), viewport->Size.x, viewport->Size.y);
    if (SceneDebug::Enabled() && SceneDebug::expanded.load()) SceneDebug::Draw();
    if (fps_hint) {
        const float x = ImGui::GetWindowPos().x + ImGui::GetWindowWidth() -
                        ImGui::GetStyle().WindowPadding.x - ImGui::CalcTextSize(fps_hint).x;
        ImGui::GetWindowDrawList()->AddText(ImVec2(x, fps_hint_y),
                                          ImGui::GetColorU32(ImGuiCol_TextDisabled), fps_hint);
    }
    ImGui::End();
}

} // namespace

void Init(const Vulkan::Instance& instance, vk::Format format, u32 image_count) {
    std::scoped_lock lock{imgui_mutex};
    if (initialized) {
        return;
    }
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr; // window positions are not kept
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
    io.BackendPlatformName = "bbport";

    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 6.0f;
    style.FrameRounding = 4.0f;
    style.GrabRounding = 4.0f;
    style.Colors[ImGuiCol_WindowBg].w = 0.92f;

    ImFontConfig font_config;
    font_config.FontDataOwnedByAtlas = false;
    io.Fonts->AddFontFromMemoryTTF(const_cast<unsigned char*>(bb_font_ttf),
                                   int(bb_font_ttf_end - bb_font_ttf), 18.0f, &font_config);

    const vk::Instance vk_instance = instance.GetInstance();
    ImGui_ImplVulkan_LoadFunctions(
        instance.ApiVersion(),
        [](const char* name, void* user) {
            return VULKAN_HPP_DEFAULT_DISPATCHER.vkGetInstanceProcAddr(
                *static_cast<const vk::Instance*>(user), name);
        },
        const_cast<vk::Instance*>(&vk_instance));

    const VkFormat color_format = static_cast<VkFormat>(format);
    ImGui_ImplVulkan_InitInfo info{};
    info.ApiVersion = instance.ApiVersion();
    info.Instance = vk_instance;
    info.PhysicalDevice = instance.GetPhysicalDevice();
    info.Device = instance.GetDevice();
    info.QueueFamily = instance.GetGraphicsQueueFamilyIndex();
    info.Queue = instance.GetGraphicsQueue();
    info.DescriptorPoolSize = 16;
    info.MinImageCount = std::max(image_count, 2u);
    info.ImageCount = std::max(image_count, 2u);
    info.UseDynamicRendering = true;
    info.PipelineInfoMain.PipelineRenderingCreateInfo = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR,
        .colorAttachmentCount = 1,
        .pColorAttachmentFormats = &color_format,
    };
    if (!ImGui_ImplVulkan_Init(&info)) {
        std::printf("Overlay: ImGui Vulkan backend init failed\n");
        ImGui::DestroyContext();
        return;
    }
    initialized = true;
    std::printf("Overlay: menu ready (Insert or L3+R3)\n");
}

void UpdateTextInput(SDL_Window* window) {
    bool want = false;
    {
        std::scoped_lock lock{imgui_mutex};
        want = initialized && menu_open && ImGui::GetIO().WantTextInput;
    }
    if (want != SDL_TextInputActive(window)) {
        if (want) {
            SDL_StartTextInput(window);
        } else {
            SDL_StopTextInput(window);
        }
    }
}

bool HandleEvent(const SDL_Event& event) {
    std::scoped_lock lock{imgui_mutex};
    if (!initialized) {
        return false;
    }
    ImGuiIO& io = ImGui::GetIO();
    const bool is_open = menu_open;
    switch (event.type) {
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP: {
        const bool down = event.type == SDL_EVENT_KEY_DOWN;
        if (!is_open) {
            return false;
        }
        io.AddKeyEvent(ImGuiMod_Ctrl, (event.key.mod & SDL_KMOD_CTRL) != 0);
        io.AddKeyEvent(ImGuiMod_Shift, (event.key.mod & SDL_KMOD_SHIFT) != 0);
        io.AddKeyEvent(ImGuiMod_Alt, (event.key.mod & SDL_KMOD_ALT) != 0);
        if (const ImGuiKey key = KeyFromSdl(event.key.key); key != ImGuiKey_None) {
            io.AddKeyEvent(key, down);
        }
        return true;
    }
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
    case SDL_EVENT_GAMEPAD_BUTTON_UP: {
        const bool down = event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN;
        const u8 button = event.gbutton.button;
        if (!is_open) {
            return false;
        }
        if (const ImGuiKey key = KeyFromGamepad(button); key != ImGuiKey_None) {
            io.AddKeyEvent(key, down);
        }
        return true;
    }
    case SDL_EVENT_TEXT_INPUT: {
        // Typed characters (Ctrl+click on a slider, a text field): key events alone erase but
        // do not type. SDL sends them while text input is on (UpdateTextInput).
        if (!is_open) {
            return false;
        }
        io.AddInputCharactersUTF8(event.text.text);
        return true;
    }
    case SDL_EVENT_MOUSE_MOTION: {
        if (!is_open) {
            return false;
        }
        const float density = PixelDensity(event.motion.windowID);
        io.AddMousePosEvent(event.motion.x * density, event.motion.y * density);
        return true;
    }
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP: {
        if (!is_open) {
            return false;
        }
        const int button = event.button.button == SDL_BUTTON_LEFT    ? 0
                           : event.button.button == SDL_BUTTON_RIGHT  ? 1
                           : event.button.button == SDL_BUTTON_MIDDLE ? 2
                                                                      : -1;
        if (button >= 0) {
            io.AddMouseButtonEvent(button, event.type == SDL_EVENT_MOUSE_BUTTON_DOWN);
        }
        return true;
    }
    case SDL_EVENT_MOUSE_WHEEL:
        if (!is_open) {
            return false;
        }
        io.AddMouseWheelEvent(event.wheel.x, event.wheel.y);
        return true;
    default:
        return false;
    }
}

bool Visible() {
    return initialized && (menu_open || text_entry_active || note_active || BbSettings::Get().show_fps);
}

void Note(const std::string& text) {
    std::scoped_lock lock{imgui_mutex};
    note_text = text;
    note_until = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    note_active = true;
}

bool CapturesInput() {
    return menu_open || text_entry_active;
}

void SetTextEntry(bool active, const std::string& prompt, const std::string& text) {
    std::scoped_lock lock{imgui_mutex};
    text_entry_active = active;
    text_entry_prompt = prompt;
    text_entry_text = text;
}

void NotePresent() {
    // Called every present whether or not anything of ours is drawn, so this is the only place
    // the keys can be polled from: Render() is gated on Visible(), which is false once F12 has
    // hidden the readout, and the poll would stop with it.
    PollHotkeys();
    std::scoped_lock lock{imgui_mutex};
    const auto now = std::chrono::steady_clock::now();
    const float ms = std::chrono::duration<float, std::milli>(now - last_present).count();
    if (last_present.time_since_epoch().count() && SceneDebug::Enabled()) SceneDebug::NoteFrame(ms,now);
    last_present = now;
    if (ms > 0.0f && ms < 1000.0f) {
        frame_ms_avg = frame_ms_avg == 0.0f ? ms : frame_ms_avg * 0.95f + ms * 0.05f;
    }
}

void Render(vk::CommandBuffer cmdbuf, vk::ImageView view, vk::Extent2D extent) {
    // ImGui animation interval; present statistics are collected in NotePresent().
    const auto now = std::chrono::steady_clock::now();
    if (!Visible()) {
        return;
    }
    std::scoped_lock lock{imgui_mutex};
    static auto last_render = now;
    const float ms = std::chrono::duration<float, std::milli>(now - last_render).count();
    last_render = now;
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(float(extent.width), float(extent.height));
    io.DeltaTime = ms > 0.0f && ms < 1000.0f ? ms / 1000.0f : 1.0f / 60.0f;
    // UI scale follows the display height (1080p = 1).
    const float scale = std::max(float(extent.height) / 1080.0f, 0.75f);
    if (std::abs(scale - base_scale) > 0.01f) {
        ImGuiStyle& style = ImGui::GetStyle();
        style.ScaleAllSizes(scale / base_scale);
        style.FontScaleMain = scale;
        base_scale = scale;
    }

    ImGui_ImplVulkan_NewFrame();
    ImGui::NewFrame();
    if (BbSettings::Get().show_fps && !menu_open) {
        FpsCounter();
    }
    if (text_entry_active) {
        TextEntryBox();
    }
    if (note_active) {
        if (now < note_until) {
            NoteBox();
        } else {
            note_active = false;
        }
    }
    ImGui::Render();

    const vk::RenderingAttachmentInfo attachment{
        .imageView = view,
        .imageLayout = vk::ImageLayout::eColorAttachmentOptimal,
        .loadOp = vk::AttachmentLoadOp::eLoad,
        .storeOp = vk::AttachmentStoreOp::eStore,
    };
    cmdbuf.beginRendering(vk::RenderingInfo{
        .renderArea = {{0, 0}, extent},
        .layerCount = 1,
        .colorAttachmentCount = 1,
        .pColorAttachments = &attachment,
    });
    {
        // Font atlas uploads submit to the graphics queue themselves.
        std::scoped_lock submit_lock{Vulkan::Scheduler::submit_mutex};
        ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cmdbuf);
    }
    cmdbuf.endRendering();
}

} // namespace BbOverlay
