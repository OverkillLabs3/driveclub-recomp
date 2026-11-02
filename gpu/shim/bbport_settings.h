// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: user settings changed at run time from the in-game menu (bbport_overlay.h) and kept
// in bbport.ini (BB_CONFIG overrides the path). Environment variables override the file at
// start. Readers load the atomics every frame; writers are the menu and Load().

#pragma once

#include <atomic>
#include <cstdlib>
#include <cstring>

namespace BbSettings {

enum Upscaler : int { UpscalerOff = 0, UpscalerFsr3 = 1, UpscalerFsr4 = 2, UpscalerFsr411 = 3,
                      UpscalerTaa = 4, UpscalerDlss = 5, UpscalerCount };
/// FSR 4 v07 or FSR 4.1.1: the same inputs, settings and placement in the frame.
inline bool IsFsr4(int upscaler) {
    return upscaler == UpscalerFsr4 || upscaler == UpscalerFsr411;
}
enum Preset : int { NativeAA = 0, Quality, Balanced, Performance, UltraPerformance, PresetCount };
enum DebugView : int { DebugNone = 0, DebugReactive = 1, DebugMotion = 2, DebugViewCount };
/// Frame generation (vk_frame_generation.h): frames generated between two of the game's.
enum FrameGen : int { FrameGenOff = 0, FrameGenFsr3 = 1, FrameGenDlss = 2, FrameGenCount };
/// Anti-aliasing at the render resolution: the game's own, FSR 3.1, or DLSS (DLAA).
enum Aa : int { AaOff = 0, AaFsr3 = 1, AaDlss = 2, AaCount };
/// Upscaling with the anti-aliasing: render size to output size (Native, Quality 1.5x, Balanced 1.7x,
/// Performance 2x, Ultra Performance 3x). The runtime sets the game's render size (src/probe.c).
enum AaScale : int { AaScaleNative = 0, AaScaleQuality = 1, AaScaleBalanced = 2, AaScalePerformance = 3,
                     AaScaleUltraPerformance = 4, AaScaleCount };

/// Game effects switched by the community patches at start (patches.py EFFECTS): ini key,
/// menu label, default (the game's own behaviour).
struct Effect {
    const char* key;
    const char* label;
    bool default_on;
};
inline constexpr Effect Effects[] = {
    {"effect_chromatic_aberration", "Хроматическая аберрация", true},
    {"effect_dof", "Глубина резкости (DoF)", true},
    {"effect_motion_blur", "Размытие в движении", true},
    {"effect_ssao", "Затенение SSAO", true},
    {"effect_game_aa", "Собственное сглаживание игры", true},
    {"effect_dynamic_shadows", "Тени от динамических источников", true},
    {"effect_ssr", "Отражения SSR (не было в игре)", false},
    {"skip_intro", "Пропуск заставок при запуске", false},
    {"debug_camera", "Свободная камера (Cross + L3)", false},
    {"debug_menu", "Debug menu (нужны файлы шрифтов)", false},
    {"cheat_no_death", "Чит: бессмертие (не ниже 1 HP)", false},
    {"cheat_stealth", "Чит: враги не замечают", false},
    {"cheat_silent", "Чит: враги не слышат", false},
    {"cheat_rally_no_decay", "Чит: Rally не угасает", false},
    {"cheat_enemy_control", "Чит: управление врагом (R3 / L3)", false},
    {"tweak_no_rally", "Без Rally (возврата HP)", false},
    {"tweak_camera_distance", "Камера дальше", false},
    {"tweak_no_camera_rotation", "Без автоповорота камеры", false},
    {"tweak_easy_run", "Бег с меньшим наклоном стика", false},
    {"tweak_ragdoll", "Физика тел как в Dark Souls", false},
};
inline constexpr int EffectCount = int(sizeof(Effects) / sizeof(Effects[0]));
/// Live output resolutions: the upscaler's output and the UI host targets.
inline constexpr int OutputWidths[] = {1280, 1920, 2560, 3840};
inline constexpr int OutputHeights[] = {720, 1080, 1440, 2160};
inline constexpr int OutputCount = 4;
inline constexpr int OutputDefault = 1; ///< 1920x1080, the game's own size

/// The Bloodborne temporal upscaler, camera and object motion tracking run only when BB_UPSCALER names one:
/// they add work to every draw and nothing in this game uses them.
inline bool UpscalerOptIn() {
    static const bool on = [] {
        const char* env = std::getenv("BB_UPSCALER");
        return env && env[0] && std::strcmp(env, "none") != 0;
    }();
    return on;
}

/// Camera and object vectors are also useful to DriveAA's DLSS path, which is selected with
/// BB_AA instead of the standalone temporal upscaler's BB_UPSCALER setting.
inline bool MotionVectorsOptIn() {
    if (UpscalerOptIn()) {
        return true;
    }
    const char* aa = std::getenv("BB_AA");
    return aa && std::strcmp(aa, "dlss") == 0;
}

/// Button prompts (bbport_prompts.cpp): the game's icons switch with the device in use, or stay.
enum Prompts : int {
    PromptsAuto = 0, PromptsPlayStation = 1, PromptsKeyboard = 2, PromptsXbox = 3, PromptsSwitch = 4, PromptsCount
};

struct Values {
    /// HDR output (BB_HDR=1 and a display in HDR mode): set by the presenter; the rasterizer then
    /// keeps the picture before the UI. Brightness of SDR white (UI, menus) and of the brightest
    /// highlight, in nits (BB_HDR_PAPER, BB_HDR_PEAK).
    std::atomic<bool> hdr_output{false};
    std::atomic<float> hdr_paper{350.0f};
    std::atomic<float> hdr_peak{1000.0f};
    /// The SDR picture (picture.ini, F4 in the game): brightness -50..50 (0 as made), contrast 50..150 %, and the
    /// display gamma it is made for in hundredths, 180..280 (240 the TVs the game was made for, 220 a PC monitor).
    std::atomic<int> picture_brightness{0};
    std::atomic<int> picture_contrast{100};
    std::atomic<int> picture_gamma{240};
    std::atomic<int> upscaler{UpscalerFsr3};
    std::atomic<int> preset{NativeAA};
    std::atomic<bool> sharpen{true};
    std::atomic<float> sharpness{0.3f};
    std::atomic<bool> jitter{true};
    std::atomic<bool> reactive{false};
    std::atomic<bool> object_motion{true};
    std::atomic<float> reactive_scale{1.0f};
    std::atomic<float> reactive_threshold{0.2f};
    std::atomic<float> reactive_max{0.9f};
    std::atomic<int> debug_view{DebugNone};
    // 0: unmodified control; 30: Slow; 60: Fast. F3 switches the two exposure presets.
    std::atomic<int> motion_blur_reference{0};
    /// Keyboard and mouse button prompts: with the device in use (auto), or always one kind.
    std::atomic<int> button_prompts{PromptsAuto};
    /// DCport: the look of Driveclub's controller prompts: Xbox 360 (0) or Xbox One (1) buttons
    /// for an Xbox controller; the game's PS4 symbols (0) or DualSense-style ones (1) for a
    /// PlayStation controller.
    std::atomic<int> xbox_buttons{0};
    std::atomic<int> playstation_buttons{0};
    std::atomic<bool> show_fps{true}; // ssport: on by default
    std::atomic<bool> show_pedals{false}; // the pedals' travel, bottom left (F2)
    // FSR 4 checks (menu): the provider's auto exposure, the jitter sign it is given.
    std::atomic<bool> fsr4_auto_exposure{true};
    std::atomic<bool> fsr4_invert_jitter{false};
    std::atomic<int> active_render_width{1920}, active_render_height{1080};
    /// Applied at start (patches.py); the menu shows when a restart is needed.
    std::atomic<bool> effects[EffectCount]{};
    std::atomic<int> model_lod{0}; ///< -2 highest .. 2 lowest, 0 the game's
    std::atomic<int> output_res{OutputDefault}; ///< index into OutputWidths
    /// Live resolution and preset changes (run.sh): 0 off by default (startup patch, fastest
    /// on the Steam Deck and older GPUs), -1 auto (strong discrete GPUs), 1 on. On restart.
    std::atomic<int> live_resolution{0};
    /// Why FSR 4 cannot run (assets, device features), or null. Set by the renderer.
    std::atomic<const char*> fsr4_problem{nullptr};
    std::atomic<bool> fsr4_supported{false}, fsr411_supported{false};
    /// DLSS (bbport_dlss.dll, NVIDIA RTX) is ready, or why not (null before the device exists).
    std::atomic<bool> dlss_supported{false};
    std::atomic<const char*> dlss_problem{nullptr};

    /// Startup settings for the explicit BB_RENDER_RES compatibility patch only.
    /// Frame generation and the frames per game frame (2x with FSR 3.1, up to 4x with DLSS on
    /// RTX 50). dlssg_max_multiplier: 1 without DLSS Frame Generation.
    std::atomic<int> frame_gen{FrameGenOff};
    std::atomic<int> aa_mode{AaOff};
    /// F8: frame generation stays set up but stops capturing and generating (nothing is freed).
    std::atomic<bool> frame_gen_paused{false};
    std::atomic<int> aa_scale{AaScaleNative};
    /// The DLSS Super Resolution model (NVIDIA's render preset): 0 NVIDIA's choice for the quality
    /// mode, 11 K (DLSS 4), 12 L, 13 M (DLSS 4.5); BB_DLSS_MODEL = k, l or m for tests.
    std::atomic<int> dlss_model{0};
    /// The anti-aliasing replaced the game's result in the last frame (for the overlay).
    std::atomic<bool> aa_running{false};
    /// FSR or DLSS could not start or run and stays off until the choice changes (the log says why).
    std::atomic<bool> aa_failed{false};
    std::atomic<int> frame_gen_multiplier{2};
    /// At most the frames the display shows between two of the game's (an uncapped game then
    /// gets fewer generated frames); off: the multiplier as chosen.
    std::atomic<bool> frame_gen_fit_display{false};
    std::atomic<int> dlssg_max_multiplier{1};
    std::atomic<const char*> frame_gen_problem{nullptr};
    /// Generated frames per game frame the display has room for now (0: paused), and its
    /// refresh rate (the presenter's measure).
    std::atomic<int> frame_gen_now{0};
    std::atomic<int> frame_gen_display_hz{0};
    /// The game's own frame rate (presenter, game frames only).
    std::atomic<float> game_fps{0.0f};
    /// The game's frame rate cap frame generation needs (VideoOut applies it), 0 for none: with
    /// FIFO presentation (V-Sync) at most refresh / (generated + 1).
    std::atomic<int> frame_gen_flip_limit{0};
    /// Startup settings for the explicit BB_RENDER_RES compatibility patch only.
    int startup_preset = NativeAA;
    int startup_upscaler = UpscalerFsr3;
    bool startup_object_motion = true;
    bool startup_effects[EffectCount]{};
    int startup_model_lod = 0;
    int startup_output_res = OutputDefault;
    int startup_live_resolution = 0;
};

Values& Get();

/// Reads the file, then the environment overrides. Called once at start.
void Load();
/// Checks the loaded choice before the first frame; unsupported FSR 4 uses FSR 3.1.
void ConfigureUpscalerSupport(bool fsr4, bool fsr411);
/// After device creation: DLSS availability; a DLSS setting falls back to FSR 3.1 without it.
void ConfigureDlssSupport(bool available, const char* problem);
/// Startup-patched scene dimensions cannot change until run.sh prepares a new image.
bool FixedRenderSession();
int RenderPreset();
bool ResolutionNeedsRestart();
/// Writes the file (menu changes).
void Save();

/// Render resolution divisor of a preset (1.0 native, 1.5 quality, ...).
float PresetScale(int preset);
const char* PresetName(int preset);
const char* UpscalerName(int upscaler);
const char* FrameGenName(int frame_gen);
const char* AaName(int aa);
const char* AaScaleName(int scale);
/// HDR brightness changed while playing (Page Up / Page Down): hdr.ini next to the settings file
/// (the launcher shows it on its slider; read at start when BB_HDR_PAPER is not set).
void SaveHdrBrightness(int nits);
/// The picture changed while playing (F4): picture.ini next to the settings file, which the launcher's sliders show.
void SavePicture();
float AaScaleFactor(int scale);
} // namespace BbSettings
