// SPDX-License-Identifier: GPL-2.0-or-later
#include "bbport_frame_info.h"
#include <deque>
#include <cstring>
#include <zlib.h>
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
#include "../../src/runtime_exit.h"
#include "../../src/exit_dialog.h"
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
extern "C" int runtime_file_translate(const char* guest, char* out, size_t size);
extern "C" const unsigned char bb_font_ttf[];
extern "C" const unsigned char bb_font_ttf_end[];

#ifdef _WIN32
#define BB_EXIT_FONT_SECTION ".section .rdata,\"dr\"\n"
#define BB_EXIT_FONT_END ".text\n"
#else
#define BB_EXIT_FONT_SECTION ".section .rodata\n"
#define BB_EXIT_FONT_END ".previous\n"
#endif
asm(BB_EXIT_FONT_SECTION
    ".balign 16\n.global bb_exit_body\nbb_exit_body:\n.incbin \"" BB_EXIT_BODY_FONT_PATH "\"\n"
    ".global bb_exit_body_end\nbb_exit_body_end:\n"
    ".balign 16\n.global bb_exit_heading\nbb_exit_heading:\n.incbin \"" BB_EXIT_HEADING_FONT_PATH "\"\n"
    ".global bb_exit_heading_end\nbb_exit_heading_end:\n" BB_EXIT_FONT_END);
extern "C" const unsigned char bb_exit_body[],bb_exit_body_end[],bb_exit_heading[],bb_exit_heading_end[];


static bool g_aa_used = false;
static bool g_fg_used = false;
// src/runtime_pad.c: the pedals the game read last, 0..1; 3 a wheel with a clutch, 2 without, 1 a gamepad, 0 none yet.
extern "C" int runtime_pad_pedals(float* throttle, float* brake, float* clutch);

namespace BbOverlay {

namespace {

std::mutex imgui_mutex; // the ImGui context: window thread (input) and present thread
bool initialized = false;
// The launcher sets everything up; there is no in-game menu.
constexpr bool menu_open = false;
// The game's text dialog (SetTextEntry), guarded by imgui_mutex.
std::atomic<bool> text_entry_active{false};
std::string text_entry_prompt, text_entry_text;
// A hotkey's note (Note), guarded by imgui_mutex: shown until note_until.
std::string note_text;
std::chrono::steady_clock::time_point note_until{};
std::atomic<bool> note_active{false};
// Trophy banners (Trophy), guarded by imgui_mutex: shown one after another, a few seconds each.
struct TrophyToast { int grade; std::string title, description; std::vector<ImU32> icon; };
std::deque<TrophyToast> trophy_queue;
std::chrono::steady_clock::time_point trophy_shown{};
bool trophy_showing = false;
std::atomic<bool> trophy_active{false};
float fps_panel_bottom = 0.0f; // bottom edge of the FPS panel this frame, 0 when it is not drawn
// The picture row F4 chose (0 brightness, 1 contrast, 2 gamma), -1 closed; closes by itself at picture_until.
std::atomic<int> picture_row{-1};
std::chrono::steady_clock::time_point picture_until{};
float base_scale = 1.0f;
ImFont *exit_body=nullptr,*exit_heading=nullptr;
std::atomic<bool> mouse_capture_requested{false};

void ExitConfirmation(float scale) {
    const auto view=runtime_exit_view();
    static bool was_open=false;
    if (!view.open) { was_open=false; return; }
    const bool just_opened=!was_open;
    was_open=true;
    ImGuiIO& io=ImGui::GetIO();
    auto *draw=ImGui::GetForegroundDrawList();
    const ImU32 red=IM_COL32(237,43,56,255), white=IM_COL32(242,242,244,255);
    draw->AddRectFilled(ImVec2(0,0),io.DisplaySize,IM_COL32(0,0,0,175));
    const float w=540*scale,h=202*scale;
    const ImVec2 pos((io.DisplaySize.x-w)*0.5f,(io.DisplaySize.y-h)*0.5f);
    draw->AddRectFilled(pos,ImVec2(pos.x+w,pos.y+h),IM_COL32(16,16,16,255));
    draw->AddRectFilled(pos,ImVec2(pos.x+3*scale,pos.y+h),red);
    const char *question="Exit DRIVECLUB?";
    const auto question_size=exit_heading->CalcTextSizeA(28*scale,FLT_MAX,0,question);
    draw->AddText(exit_heading,28*scale,ImVec2(pos.x+(w-question_size.x)*0.5f,pos.y+30*scale),white,question);
    const float bw=225*scale,bh=44*scale,by=pos.y+88*scale;
    for (int i=0;i<2;++i) {
        ImVec2 a(pos.x+(36+i*243)*scale,by),b(a.x+bw,a.y+bh);
        const bool hovered=io.MousePos.x>=a.x && io.MousePos.x<b.x && io.MousePos.y>=a.y && io.MousePos.y<b.y;
        if (!just_opened && hovered && (io.MouseDelta.x || io.MouseDelta.y)) runtime_exit_hover(i);
        const bool selected=(view.selected_exit!=0)==(i!=0);
        draw->AddRectFilled(a,b,selected ? red : hovered ? IM_COL32(49,49,53,255) : IM_COL32(32,32,36,255));
        const char *label=i ? "Exit" : "Cancel";
        const ImVec2 size=exit_body->CalcTextSizeA(20*scale,FLT_MAX,0,label);
        draw->AddText(exit_body,20*scale,ImVec2(a.x+(bw-size.x)*0.5f,a.y+(bh-size.y)*0.5f),white,label);
        if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !view.exiting) runtime_exit_click(i);
    }
    char hints[160];
    std::snprintf(hints,sizeof(hints),"%s • Select       %s • Back",view.confirm,view.back);
    const char *footer=view.exiting ? "Exiting DRIVECLUB..." : hints;
    const ImVec2 footer_size=exit_body->CalcTextSizeA(15*scale,FLT_MAX,0,footer);
    draw->AddText(exit_body,15*scale,ImVec2(pos.x+(w-footer_size.x)*0.5f,pos.y+154*scale),
                  IM_COL32(155,155,160,255),footer);
}

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
    // F2: the pedal inputs (PedalPanel) on and off.
    static bool f2_down = false;
    if (edge(VK_F2, f2_down) && SDL_GetKeyboardFocus()) {
        s.show_pedals = !s.show_pedals;
        Note(s.show_pedals ? "Pedal inputs on  (F2: off)" : "Pedal inputs off  (F2: on)");
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
    // F4: the picture (PicturePanel): brightness, contrast, gamma in turn, then closed. SDR only.
    static bool f4_down = false;
    if (edge(VK_F4, f4_down) && SDL_GetKeyboardFocus() && !(GetAsyncKeyState(VK_MENU) & 0x8000)) {
        if (s.hdr_output) {
            Note("Brightness, contrast and gamma are for SDR   (HDR: Page Up / Page Down)");
        } else {
            picture_row = picture_row >= 2 ? -1 : picture_row + 1;
        }
        picture_until = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    }
    if (picture_row >= 0 && std::chrono::steady_clock::now() > picture_until) {
        picture_row = -1;
    }
    // Page Up / Page Down: the picture row F4 chose; else HDR brightness (SDR white) in steps of 25 nits with HDR on, the
    // picture's brightness without it. All kept for the next start.
    static bool page_up_down = false, page_down_down = false;
    const bool brighter = edge(VK_PRIOR, page_up_down), darker = edge(VK_NEXT, page_down_down);
    if (picture_row >= 0 && (brighter || darker)) {
        const int step = brighter ? 1 : -1;
        if (picture_row == 0) {
            s.picture_brightness = std::clamp(s.picture_brightness.load() + 2 * step, -50, 50);
        } else if (picture_row == 1) {
            s.picture_contrast = std::clamp(s.picture_contrast.load() + 5 * step, 50, 150);
        } else {
            s.picture_gamma = std::clamp(s.picture_gamma.load() + 5 * step, 180, 280);
        }
        BbSettings::SavePicture();
        picture_until = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    } else if (s.hdr_output && (brighter || darker)) {
        const int now = int(std::lround(s.hdr_paper.load() / 25.0f)) * 25;
        const int next = std::clamp(now + (brighter ? 25 : -25), 100, 600);
        s.hdr_paper = float(next);
        BbSettings::SaveHdrBrightness(next);
        Note("HDR brightness " + std::to_string(next) + " nits   (Page Up / Page Down)");
    } else if (brighter || darker) {
        const int next = std::clamp(s.picture_brightness.load() + (brighter ? 2 : -2), -50, 50);
        s.picture_brightness = next;
        BbSettings::SavePicture();
        Note("Brightness " + std::string(next > 0 ? "+" : "") + std::to_string(next) +
             "   (Page Up / Page Down;  F4: contrast and gamma)");
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

// The artwork of a trophy sits unencrypted as a PNG in the game's own trophy file (sce_sys/trophy/trophy00.trp), so
// the banner shows the real picture without any key. It is decoded here (zlib) and shrunk to IconSize squares.
constexpr int IconSize = 64;

bool DecodePng(const std::vector<unsigned char>& png, int& width, int& height, std::vector<unsigned char>& rgba) {
    static const unsigned char signature[8] = {0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a};
    if (png.size() < 33 || std::memcmp(png.data(), signature, 8) != 0) {
        return false;
    }
    const auto be32 = [&](size_t at) {
        return (uint32_t(png[at]) << 24) | (uint32_t(png[at + 1]) << 16) | (uint32_t(png[at + 2]) << 8) | png[at + 3];
    };
    std::vector<unsigned char> packed;
    int colour_type = 0;
    size_t at = 8;
    width = height = 0;
    while (at + 12 <= png.size()) {
        const uint32_t length = be32(at);
        const char* type = reinterpret_cast<const char*>(&png[at + 4]);
        if (at + 12 + length > png.size()) {
            return false;
        }
        if (!std::memcmp(type, "IHDR", 4) && length >= 13) {
            width = int(be32(at + 8));
            height = int(be32(at + 12));
            colour_type = png[at + 17];
            if (png[at + 16] != 8 || png[at + 20] != 0) {
                return false; // 8 bits per channel, not interlaced
            }
        } else if (!std::memcmp(type, "IDAT", 4)) {
            packed.insert(packed.end(), png.begin() + at + 8, png.begin() + at + 8 + length);
        } else if (!std::memcmp(type, "IEND", 4)) {
            break;
        }
        at += 12 + length;
    }
    const int bytes = colour_type == 6 ? 4 : colour_type == 2 ? 3 : 0;
    if (!bytes || width <= 0 || height <= 0 || width > 2048 || height > 2048) {
        return false;
    }
    const size_t stride = size_t(width) * bytes;
    std::vector<unsigned char> raw((stride + 1) * size_t(height));
    uLongf raw_size = uLongf(raw.size());
    if (uncompress(raw.data(), &raw_size, packed.data(), uLong(packed.size())) != Z_OK || raw_size != raw.size()) {
        return false;
    }
    std::vector<unsigned char> pixels(stride * size_t(height));
    for (int y = 0; y < height; ++y) {
        const unsigned char filter = raw[size_t(y) * (stride + 1)];
        const unsigned char* in = &raw[size_t(y) * (stride + 1) + 1];
        unsigned char* out = &pixels[size_t(y) * stride];
        const unsigned char* above = y ? out - stride : nullptr;
        for (size_t x = 0; x < stride; ++x) {
            const int a = x >= size_t(bytes) ? out[x - bytes] : 0;
            const int b = above ? above[x] : 0;
            const int c = above && x >= size_t(bytes) ? above[x - bytes] : 0;
            int predictor = 0;
            switch (filter) {
            case 1: predictor = a; break;
            case 2: predictor = b; break;
            case 3: predictor = (a + b) / 2; break;
            case 4: {
                const int p = a + b - c, pa = std::abs(p - a), pb = std::abs(p - b), pc = std::abs(p - c);
                predictor = pa <= pb && pa <= pc ? a : (pb <= pc ? b : c);
                break;
            }
            default: break;
            }
            out[x] = static_cast<unsigned char>(in[x] + predictor);
        }
    }
    rgba.assign(size_t(width) * size_t(height) * 4, 255);
    for (size_t i = 0; i < size_t(width) * size_t(height); ++i) {
        for (int k = 0; k < bytes; ++k) {
            rgba[i * 4 + k] = pixels[i * bytes + k];
        }
    }
    return true;
}

// Reads trophy `id`'s picture out of the player's own trophy file; empty when it is not there.
std::vector<ImU32> LoadTrophyIcon(int id) {
    std::vector<ImU32> icon;
    char host[1024];
    if (runtime_file_translate("/app0/sce_sys/trophy/trophy00.trp", host, sizeof(host)) != 0) {
        return icon;
    }
    std::FILE* file = std::fopen(host, "rb");
    if (!file) {
        return icon;
    }
    unsigned char header[0x60];
    uint32_t count = 0;
    std::vector<unsigned char> png;
    char wanted[16];
    std::snprintf(wanted, sizeof(wanted), "TROP%03d.PNG", id);
    if (std::fread(header, 1, sizeof(header), file) == sizeof(header)) {
        count = (uint32_t(header[0x10]) << 24) | (uint32_t(header[0x11]) << 16) | (uint32_t(header[0x12]) << 8) | header[0x13];
        for (uint32_t i = 0; i < count && i < 512 && png.empty(); ++i) {
            unsigned char entry[0x40];
            if (std::fseek(file, long(0x60 + i * 0x40), SEEK_SET) != 0 || std::fread(entry, 1, sizeof(entry), file) != sizeof(entry)) {
                break;
            }
            if (std::strncmp(reinterpret_cast<const char*>(entry), wanted, 0x20) != 0) {
                continue;
            }
            uint64_t offset = 0, size = 0;
            for (int k = 0; k < 8; ++k) {
                offset = (offset << 8) | entry[0x20 + k];
                size = (size << 8) | entry[0x28 + k];
            }
            if (size > 0 && size < (8u << 20) && std::fseek(file, long(offset), SEEK_SET) == 0) {
                png.resize(size_t(size));
                if (std::fread(png.data(), 1, png.size(), file) != png.size()) {
                    png.clear();
                }
            }
            break;
        }
    }
    std::fclose(file);
    int width = 0, height = 0;
    std::vector<unsigned char> rgba;
    if (png.empty() || !DecodePng(png, width, height, rgba)) {
        return icon;
    }
    icon.resize(size_t(IconSize) * IconSize);
    for (int y = 0; y < IconSize; ++y) {
        for (int x = 0; x < IconSize; ++x) {
            const int x0 = x * width / IconSize, x1 = std::max(x0 + 1, (x + 1) * width / IconSize);
            const int y0 = y * height / IconSize, y1 = std::max(y0 + 1, (y + 1) * height / IconSize);
            unsigned sum[3] = {0, 0, 0}, n = 0;
            for (int yy = y0; yy < y1 && yy < height; ++yy) {
                for (int xx = x0; xx < x1 && xx < width; ++xx) {
                    const unsigned char* p = &rgba[(size_t(yy) * width + xx) * 4];
                    sum[0] += p[0]; sum[1] += p[1]; sum[2] += p[2]; ++n;
                }
            }
            n = n ? n : 1;
            icon[size_t(y) * IconSize + x] = IM_COL32(sum[0] / n, sum[1] / n, sum[2] / n, 255);
        }
    }
    return icon;
}

void TrophyBanner(const TrophyToast& toast, float age) {
    // The console's banner: top right, a dark pill with the trophy's own picture on the left, the name over the
    // description, and the grade's cup at the end. It slides in from the edge and out again.
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const float s = base_scale;
    const float slide = age < 0.35f ? age / 0.35f : (age > 4.65f ? std::max(0.0f, (5.0f - age) / 0.35f) : 1.0f);
    const float w = 560.0f * s, h = 92.0f * s, margin = 40.0f * s;
    const float banner_top = fps_panel_bottom > 0.0f ? fps_panel_bottom + 10.0f * s : viewport->WorkPos.y + margin;
    const ImVec2 pos(viewport->WorkPos.x + viewport->WorkSize.x - w - margin + (1.0f - slide) * (w + margin), banner_top);
    ImGui::SetNextWindowPos(pos);
    ImGui::SetNextWindowSize(ImVec2(w, h));
    ImGui::SetNextWindowBgAlpha(0.9f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 14.0f * s);
    ImGui::Begin("##trophy", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav |
                     ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoSavedSettings);
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const float icon_size = 68.0f * s;
    const ImVec2 icon_pos(pos.x + 12.0f * s, pos.y + (h - icon_size) * 0.5f);
    if (!toast.icon.empty()) {
        const ImDrawListFlags saved = draw->Flags;
        draw->Flags &= ~ImDrawListFlags_AntiAliasedFill; // keep the picture's pixels edge to edge
        const float cell = icon_size / IconSize;
        for (int y = 0; y < IconSize; ++y) {
            for (int x = 0; x < IconSize; ++x) {
                const ImVec2 a(icon_pos.x + std::floor(x * cell), icon_pos.y + std::floor(y * cell));
                const ImVec2 b(icon_pos.x + std::ceil((x + 1) * cell), icon_pos.y + std::ceil((y + 1) * cell));
                draw->AddRectFilled(a, b, toast.icon[size_t(y) * IconSize + x]);
            }
        }
        draw->Flags = saved;
        draw->AddRect(icon_pos, ImVec2(icon_pos.x + icon_size, icon_pos.y + icon_size), IM_COL32(255, 255, 255, 70), 0.0f, 0, 1.0f);
    }
    static const ImU32 cup[4] = {IM_COL32(190, 220, 245, 255), IM_COL32(240, 190, 60, 255), IM_COL32(200, 205, 215, 255),
                                 IM_COL32(205, 130, 70, 255)};
    const ImU32 colour = cup[toast.grade >= 0 && toast.grade < 4 ? toast.grade : 3];
    const float cx = pos.x + w - 34.0f * s, top = pos.y + h * 0.5f - 18.0f * s;
    draw->AddRectFilled(ImVec2(cx - 12.0f * s, top), ImVec2(cx + 12.0f * s, top + 17.0f * s), colour, 4.0f * s, ImDrawFlags_RoundCornersBottom);
    draw->AddRect(ImVec2(cx - 17.0f * s, top + 2.0f * s), ImVec2(cx - 10.0f * s, top + 11.0f * s), colour, 3.0f * s, 0, 2.0f * s);
    draw->AddRect(ImVec2(cx + 10.0f * s, top + 2.0f * s), ImVec2(cx + 17.0f * s, top + 11.0f * s), colour, 3.0f * s, 0, 2.0f * s);
    draw->AddRectFilled(ImVec2(cx - 2.5f * s, top + 17.0f * s), ImVec2(cx + 2.5f * s, top + 25.0f * s), colour);
    draw->AddRectFilled(ImVec2(cx - 9.0f * s, top + 25.0f * s), ImVec2(cx + 9.0f * s, top + 29.0f * s), colour, 2.0f * s);
    const float text_x = pos.x + 12.0f * s + (toast.icon.empty() ? 0.0f : icon_size + 14.0f * s);
    ImGui::SetCursorScreenPos(ImVec2(text_x, pos.y + 12.0f * s));
    ImGui::TextDisabled("Trophy earned");
    ImGui::SetCursorScreenPos(ImVec2(text_x, pos.y + 31.0f * s));
    ImGui::SetWindowFontScale(1.2f);
    ImGui::TextUnformatted(toast.title.c_str());
    ImGui::SetWindowFontScale(1.0f);
    ImGui::SetCursorScreenPos(ImVec2(text_x, pos.y + 58.0f * s));
    ImGui::PushTextWrapPos(pos.x + w - 60.0f * s);
    ImGui::TextDisabled("%s", toast.description.c_str());
    ImGui::PopTextWrapPos();
    ImGui::End();
    ImGui::PopStyleVar();
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

/// The pedals the game read last (F2), bottom left: clutch, brake and throttle as bars with their percentage. A pedal
/// at the end of its travel is drawn bright and outlined, so a press that stops short of full shows.
void PedalPanel() {
    float throttle = 0.0f, brake = 0.0f, clutch = 0.0f;
    const int kind = runtime_pad_pedals(&throttle, &brake, &clutch);
    if (!kind) return;
    struct Bar { const char* name; float value; ImU32 color; };
    Bar bars[3];
    int count = 0;
    if (kind == 3) bars[count++] = {"Clutch", clutch, IM_COL32(70, 140, 255, 255)};
    bars[count++] = {"Brake", brake, IM_COL32(235, 60, 55, 255)};
    bars[count++] = {"Throttle", throttle, IM_COL32(60, 200, 90, 255)};
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const float pad = 12.0f * base_scale, gap = 12.0f * base_scale, bar_h = 150.0f * base_scale;
    float bar_w = 40.0f * base_scale;   // as wide as the widest name
    for (int i = 0; i < count; ++i) bar_w = std::max(bar_w, ImGui::CalcTextSize(bars[i].name).x + 6.0f * base_scale);
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + pad, viewport->WorkPos.y + viewport->WorkSize.y - pad),
                            ImGuiCond_Always, ImVec2(0.0f, 1.0f));
    ImGui::SetNextWindowBgAlpha(0.5f);
    ImGui::Begin("##pedals", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs |
                     ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoFocusOnAppearing);
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const float line = ImGui::GetTextLineHeight(), space = 4.0f * base_scale;
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    for (int i = 0; i < count; ++i) {
        const float x = origin.x + float(i) * (bar_w + gap);
        const float v = std::clamp(bars[i].value, 0.0f, 1.0f);
        const int percent = int(v * 100.0f + 0.5f);
        const bool full = percent >= 100;
        char text[16];
        std::snprintf(text, sizeof(text), "%d%%", percent);
        const float text_w = ImGui::CalcTextSize(text).x, name_w = ImGui::CalcTextSize(bars[i].name).x;
        draw->AddText(ImVec2(x + (bar_w - text_w) * 0.5f, origin.y), full ? IM_COL32_WHITE : ImGui::GetColorU32(ImGuiCol_Text), text);
        const float top = origin.y + line + space, bottom = top + bar_h;
        draw->AddRectFilled(ImVec2(x, top), ImVec2(x + bar_w, bottom), IM_COL32(255, 255, 255, 40));
        // below full the colour is dimmed, at full it is bright with an outline
        const ImU32 fill = full ? bars[i].color : (bars[i].color & 0x00ffffffu) | (150u << 24);
        draw->AddRectFilled(ImVec2(x, bottom - bar_h * v), ImVec2(x + bar_w, bottom), fill);
        if (full) draw->AddRect(ImVec2(x, top), ImVec2(x + bar_w, bottom), IM_COL32_WHITE, 0.0f, 0, 2.0f * base_scale);
        draw->AddText(ImVec2(x + (bar_w - name_w) * 0.5f, bottom + space), ImGui::GetColorU32(ImGuiCol_TextDisabled),
                      bars[i].name);
    }
    ImGui::Dummy(ImVec2(float(count) * bar_w + float(count - 1) * gap, line + space + bar_h + space + line));
    ImGui::End();
}

/// The picture (F4), bottom centre: brightness, contrast and gamma, the one Page Up / Page Down changes marked.
void PicturePanel() {
    const auto& s = BbSettings::Get();
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x * 0.5f,
                                   viewport->WorkPos.y + viewport->WorkSize.y - 60.0f * base_scale),
                            ImGuiCond_Always, ImVec2(0.5f, 1.0f));
    ImGui::SetNextWindowBgAlpha(0.75f);
    ImGui::Begin("##picture", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs |
                     ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoFocusOnAppearing);
    ImGui::SetWindowFontScale(1.3f);
    char rows[3][32];
    std::snprintf(rows[0], sizeof(rows[0]), "Brightness  %+d", s.picture_brightness.load());
    std::snprintf(rows[1], sizeof(rows[1]), "Contrast  %d%%", s.picture_contrast.load());
    std::snprintf(rows[2], sizeof(rows[2]), "Gamma  %.2f", float(s.picture_gamma.load()) / 100.0f);
    for (int i = 0; i < 3; ++i) {
        if (i) ImGui::SameLine(0.0f, 28.0f * base_scale);
        if (i == picture_row) {
            ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.35f, 1.0f), "> %s", rows[i]);
        } else {
            ImGui::TextDisabled("  %s", rows[i]);
        }
    }
    ImGui::SetWindowFontScale(1.0f);
    ImGui::TextDisabled("Page Up / Page Down: change    F4: next    defaults: brightness 0, contrast 100%%, gamma 2.40 (TV)");
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
                        s.aa_mode == BbSettings::AaDlss ? (up ? "DLSS" : "DLAA") : "FSR 3.1",
                        up ? " " : "", up ? BbSettings::AaScaleName(s.aa_scale) : "",
                        s.aa_running ? " on" : s.aa_failed ? " failed (see the log)" : " waiting", *keys ? "  (" : "",
                        keys, *keys ? ")" : "");
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
    fps_panel_bottom = ImGui::GetWindowPos().y + ImGui::GetWindowSize().y;
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
    static const ImWchar exit_body_ranges[]={0x0020,0x00ff,0x2022,0x2022,0};
    exit_body=io.Fonts->AddFontFromMemoryTTF(const_cast<unsigned char*>(bb_exit_body),
        int(bb_exit_body_end-bb_exit_body),20.0f,&font_config,exit_body_ranges);
    exit_heading=io.Fonts->AddFontFromMemoryTTF(const_cast<unsigned char*>(bb_exit_heading),
        int(bb_exit_heading_end-bb_exit_heading),28.0f,&font_config);

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

void Shutdown() {
    std::scoped_lock lock{imgui_mutex};
    if(!initialized) return;
    initialized=false;
    ImGui_ImplVulkan_Shutdown();
    ImGui::DestroyContext();
    exit_body=exit_heading=nullptr;
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

void RequestMouseCapture(bool capture) { mouse_capture_requested=capture; }

void UpdateMouseCapture(SDL_Window* window) {
    // SDL cursor and relative-mode changes belong to the window thread. The
    // input sampler requests its usual mouse-stick mode; a modal overrides it.
    const bool modal=CapturesInput();
    const bool relative=mouse_capture_requested && !modal && SDL_GetKeyboardFocus()==window;
    if (SDL_GetWindowRelativeMouseMode(window)!=relative) {
        SDL_SetWindowRelativeMouseMode(window,relative);
        SDL_GetRelativeMouseState(nullptr,nullptr); // discard capture/release warp deltas
    }
    static bool was_modal=false,previous_cursor_visible=true;
    if (modal && !was_modal) { previous_cursor_visible=SDL_CursorVisible(); SDL_ShowCursor(); }
    if (!modal && was_modal && !previous_cursor_visible) SDL_HideCursor();
    was_modal=modal;
}

bool HandleEvent(const SDL_Event& event) {
    std::scoped_lock lock{imgui_mutex};
    if (!initialized) {
        return false;
    }
    ImGuiIO& io = ImGui::GetIO();
    const bool exit_capture=runtime_exit_captures()!=0;
    if (event.type==SDL_EVENT_KEY_DOWN || event.type==SDL_EVENT_KEY_UP) {
        if (event.type==SDL_EVENT_KEY_DOWN && !event.key.repeat) runtime_exit_mouse_activity();
        unsigned action=0;
        switch(event.key.key) {
        case SDLK_ESCAPE: action=EXIT_BACK; break;
        case SDLK_RETURN: case SDLK_KP_ENTER: action=EXIT_CONFIRM; break;
        case SDLK_LEFT: case SDLK_UP: action=EXIT_PREVIOUS; break;
        case SDLK_RIGHT: case SDLK_DOWN: action=EXIT_NEXT; break;
        default: break;
        }
        if (action) runtime_exit_key(action,event.type==SDL_EVENT_KEY_DOWN,event.key.repeat);
    }
    if (exit_capture && (event.type==SDL_EVENT_MOUSE_BUTTON_DOWN ||
        (event.type==SDL_EVENT_MOUSE_MOTION && (event.motion.xrel!=0 || event.motion.yrel!=0))))
        runtime_exit_mouse_activity();
    const bool is_open = menu_open || exit_capture;
    switch (event.type) {
    case SDL_EVENT_WINDOW_FOCUS_LOST:
        runtime_exit_release_keys();
        io.AddFocusEvent(false);
        return false;
    case SDL_EVENT_WINDOW_FOCUS_GAINED:
        io.AddFocusEvent(true);
        return false;
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP: {
        const bool down = event.type == SDL_EVENT_KEY_DOWN;
        if (!is_open) {
            return false;
        }
        if (exit_capture) return true; // Logical navigation is sampled once by runtime_pad.
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
        if (exit_capture) return true; // Respect pad_remap; don't also navigate via physical SDL buttons.
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
    case SDL_EVENT_GAMEPAD_AXIS_MOTION:
    case SDL_EVENT_GAMEPAD_TOUCHPAD_DOWN:
    case SDL_EVENT_GAMEPAD_TOUCHPAD_MOTION:
    case SDL_EVENT_GAMEPAD_TOUCHPAD_UP:
        return exit_capture;
    default:
        return false;
    }
}

bool Visible() {
    return initialized && (runtime_exit_captures() || menu_open || text_entry_active || note_active || trophy_active || BbSettings::Get().show_fps ||
                           BbSettings::Get().show_pedals || picture_row >= 0);
}

void Trophy(int id, int grade, const std::string& title, const std::string& description) {
    std::vector<ImU32> icon = LoadTrophyIcon(id);
    std::scoped_lock lock{imgui_mutex};
    trophy_queue.push_back({grade, title, description, std::move(icon)});
    trophy_active = true;
}

void Note(const std::string& text) {
    std::scoped_lock lock{imgui_mutex};
    note_text = text;
    note_until = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    note_active = true;
}

bool CapturesInput() {
    return runtime_exit_captures() || menu_open || text_entry_active;
}
bool ExitDialogAvailable() {
    std::scoped_lock lock{imgui_mutex};
    return initialized && !text_entry_active && !menu_open;
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
    fps_panel_bottom = 0.0f;
    if (BbSettings::Get().show_fps && !menu_open) {
        FpsCounter();
    }
    if (BbSettings::Get().show_pedals && !menu_open) {
        PedalPanel();
    }
    if (picture_row >= 0 && !menu_open) {
        PicturePanel();
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
    if (trophy_showing || !trophy_queue.empty()) {
        if (!trophy_showing) {
            trophy_showing = true;
            trophy_shown = now;
        }
        const float age = std::chrono::duration<float>(now - trophy_shown).count();
        if (age < 5.0f) {
            TrophyBanner(trophy_queue.front(), age);
        } else {
            trophy_queue.pop_front();
            trophy_showing = false;
            trophy_active = !trophy_queue.empty();
        }
    }
    ExitConfirmation(scale);
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
