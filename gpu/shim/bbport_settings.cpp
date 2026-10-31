// SPDX-License-Identifier: GPL-2.0-or-later
#include "bbport_settings.h"
#include "bbport_neural_settings.h"

#include <cmath>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>

namespace BbSettings {

namespace {

const char* Path() {
    const char* env = std::getenv("BB_CONFIG");
    return env && env[0] ? env : "bbport.ini";
}

// A file in the settings file's folder.
std::string NextToConfig(const char* name) {
    const std::string config = Path();
    const size_t slash = config.find_last_of("/\\");
    return (slash == std::string::npos ? std::string() : config.substr(0, slash + 1)) + name;
}
// hdr.ini: the HDR brightness set while playing; picture.ini: the SDR picture (launcher sliders, F4).
std::string HdrIniPath() { return NextToConfig("hdr.ini"); }
std::string PictureIniPath() { return NextToConfig("picture.ini"); }

float Clamp(float v, float lo, float hi) {
    return std::clamp(v, lo, hi);
}

void Set(Values& v, const std::string& key, const std::string& value) {
    const float f = float(std::atof(value.c_str()));
    const int i = std::atoi(value.c_str());
    if (key == "upscaler") {
        for (int u = 0; u < UpscalerCount; ++u) {
            if (value == UpscalerName(u)) {
                v.upscaler = u;
            }
        }
    } else if (key == "preset") {
        v.preset = std::clamp(i, 0, PresetCount - 1);
    } else if (key == "sharpen") {
        v.sharpen = i != 0;
    } else if (key == "sharpness") {
        v.sharpness = Clamp(f, 0.0f, 2.0f);
    } else if (key == "jitter") {
        v.jitter = i != 0;
    } else if (key == "reactive") {
        v.reactive = i != 0;
    } else if (key == "object_motion") {
        v.object_motion = i != 0;
    } else if (key == "reactive_scale") {
        v.reactive_scale = Clamp(f, 0.0f, 16.0f);
    } else if (key == "reactive_threshold") {
        v.reactive_threshold = Clamp(f, 0.0f, 1.0f);
    } else if (key == "reactive_max") {
        v.reactive_max = Clamp(f, 0.0f, 1.0f);
    } else if (key == "debug_view") {
        v.debug_view = std::clamp(i, 0, DebugViewCount - 1);
    } else if (key == "frame_gen") {
        for (int g = 0; g < FrameGenCount; ++g) {
            if (value == FrameGenName(g)) {
                v.frame_gen = g;
            }
        }
    } else if (key == "aa") {
        for (int a = 0; a < AaCount; ++a) {
            if (value == AaName(a)) {
                v.aa_mode = a;
            }
        }
    } else if (key == "aa_scale") {
        for (int a = 0; a < AaScaleCount; ++a) {
            if (value == AaScaleName(a)) {
                v.aa_scale = a;
            }
        }
    } else if (key == "hdr_paper") {
        v.hdr_paper = Clamp(f, 80.0f, 600.0f);
    } else if (key == "hdr_peak") {
        v.hdr_peak = Clamp(f, 400.0f, 10000.0f);
    } else if (key == "brightness") {
        v.picture_brightness = std::clamp(i, -50, 50);
    } else if (key == "contrast") {
        v.picture_contrast = std::clamp(i, 50, 150);
    } else if (key == "gamma") {
        // hundredths, or a plain 2.4
        v.picture_gamma = std::clamp(f < 10.0f ? int(std::lround(f * 100.0f)) : i, 180, 280);
    } else if (key == "dlss_model") {
        // auto, or the preset's letter
        v.dlss_model = value == "k" || value == "K" ? 11 : value == "l" || value == "L" ? 12
                     : value == "m" || value == "M" ? 13 : 0;
    } else if (key == "frame_gen_multiplier") {
        v.frame_gen_multiplier = std::clamp(i, 2, 4);
    } else if (key == "frame_gen_fit_display") {
        v.frame_gen_fit_display = i != 0;
    } else if (key == "motion_blur_reference") {
        v.motion_blur_reference = value == "long" || value == "30" ? 30
                                : value == "short" || value == "60" ? 60 : 0;
    } else if (key == "button_prompts") {
        v.button_prompts = value == "playstation" ? PromptsPlayStation
                         : value == "keyboard"    ? PromptsKeyboard
                         : value == "xbox"        ? PromptsXbox
                         : value == "switch"      ? PromptsSwitch
                                                  : PromptsAuto;
    } else if (key == "xbox_buttons") {
        v.xbox_buttons = value == "one" ? 1 : 0;
    } else if (key == "playstation_buttons") {
        v.playstation_buttons = value == "ps5" ? 1 : 0;
    } else if (key == "show_fps") {
        v.show_fps = i != 0;
    } else if (key == "show_pedals") {
        v.show_pedals = i != 0;
    } else if (key == "fsr4_auto_exposure") {
        v.fsr4_auto_exposure = i != 0;
    } else if (key == "fsr4_invert_jitter") {
        v.fsr4_invert_jitter = i != 0;
    } else if (key == "model_lod") {
        v.model_lod = std::clamp(i, -2, 2);
    } else if (key == "live_resolution") {
        v.live_resolution = value == "auto" ? -1 : std::clamp(i, 0, 1);
    } else if (key == "output_res") {
        for (int r = 0; r < OutputCount; ++r) {
            if (value == std::to_string(OutputWidths[r]) + "x" + std::to_string(OutputHeights[r])) {
                v.output_res = r;
            }
        }
    } else {
        for (int e = 0; e < EffectCount; ++e) {
            if (key == Effects[e].key) {
                v.effects[e] = i != 0;
            }
        }
    }
}

} // namespace

Values& Get() {
    static Values values;
    return values;
}

void Load() {
    auto& v = Get();
    for (int e = 0; e < EffectCount; ++e) {
        v.effects[e] = Effects[e].default_on;
    }
    if (const char* bp = std::getenv("BB_BUTTON_PROMPTS")) {
        Set(v, "button_prompts", bp);
    }
    if (const char* xb = std::getenv("BB_XBOX_BUTTONS")) {
        Set(v, "xbox_buttons", xb);
    }
    if (const char* ps = std::getenv("BB_PLAYSTATION_BUTTONS")) {
        Set(v, "playstation_buttons", ps);
    }
    if (FILE* file = std::fopen(Path(), "r")) {
        char line[256];
        while (std::fgets(line, sizeof(line), file)) {
            std::string text{line};
            text.erase(text.find_last_not_of(" \t\r\n") + 1);
            const auto eq = text.find('=');
            if (text.empty() || text[0] == '#' || eq == std::string::npos) {
                continue;
            }
            Set(v, text.substr(0, eq), text.substr(eq + 1));
        }
        std::fclose(file);
        std::printf("Settings: %s\n", Path());
    }
    BbNeural::Load(Path());
    // The HDR brightness last set while playing (the launcher passes its slider, which shows it).
    if (FILE* file = std::fopen(HdrIniPath().c_str(), "r")) {
        char line[64];
        while (std::fgets(line, sizeof(line), file)) {
            if (std::strncmp(line, "paper=", 6) == 0) {
                Set(v, "hdr_paper", std::string(line + 6));
            }
        }
        std::fclose(file);
    }
    if (FILE* file = std::fopen(PictureIniPath().c_str(), "r")) {
        char line[64];
        while (std::fgets(line, sizeof(line), file)) {
            std::string text{line};
            text.erase(text.find_last_not_of(" \t\r\n") + 1);
            const auto eq = text.find('=');
            if (eq != std::string::npos) {
                Set(v, text.substr(0, eq), text.substr(eq + 1));
            }
        }
        std::fclose(file);
    }
    // BB_TV_GAMMA=0 (before picture.ini): the picture as on a PC monitor.
    if (const char* tv = std::getenv("BB_TV_GAMMA"); tv && tv[0] == '0') {
        v.picture_gamma = 220;
    }
    // Environment overrides (scripts, A/B tests).
    if (const char* env = std::getenv("BB_UPSCALER")) {
        v.upscaler = UpscalerOff;
        for (int u = 0; u < UpscalerCount; ++u) {
            if (std::strcmp(env, UpscalerName(u)) == 0) v.upscaler = u;
        }
    }
    const std::pair<const char*, const char*> env_keys[] = {
        {"BB_FSR_SHARPNESS", "sharpness"},        {"BB_JITTER", "jitter"},
        {"BB_REACTIVE", "reactive"},              {"BB_REACTIVE_SCALE", "reactive_scale"},
        {"BB_REACTIVE_THRESHOLD", "reactive_threshold"}, {"BB_REACTIVE_MAX", "reactive_max"},
        {"BB_UPSCALE_PRESET", "preset"},            {"BB_OBJECT_MOTION", "object_motion"},
        {"BB_AA", "aa"},                            {"BB_AA_SCALE", "aa_scale"},                            {"BB_FRAME_GEN", "frame_gen"},              {"BB_FRAME_GEN_MULTIPLIER", "frame_gen_multiplier"},
        {"BB_FRAME_GEN_FIT_DISPLAY", "frame_gen_fit_display"}, {"BB_DLSS_MODEL", "dlss_model"},
        {"BB_MOTION_BLUR_REFERENCE", "motion_blur_reference"},
        {"BB_HDR_PAPER", "hdr_paper"},                {"BB_HDR_PEAK", "hdr_peak"},
        {"BB_BRIGHTNESS", "brightness"},              {"BB_CONTRAST", "contrast"},
        {"BB_GAMMA", "gamma"},
    };
    for (const auto& [env, key] : env_keys) {
        if (const char* value = std::getenv(env)) {
            Set(v, key, value);
        }
    }
    v.startup_preset = v.preset;
    v.startup_upscaler = v.upscaler;
    v.startup_object_motion = v.object_motion;
    for (int e = 0; e < EffectCount; ++e) {
        v.startup_effects[e] = v.effects[e];
    }
    v.startup_model_lod = v.model_lod;
    v.startup_output_res = v.output_res;
    v.startup_live_resolution = v.live_resolution;
}

void ConfigureUpscalerSupport(bool fsr4, bool fsr411) {
    auto& v = Get();
    v.fsr4_supported = fsr4;
    v.fsr411_supported = fsr4 && fsr411;
    const int requested = v.upscaler;
    if ((requested == UpscalerFsr4 && !v.fsr4_supported) ||
        (requested == UpscalerFsr411 && !v.fsr411_supported)) {
        v.fsr4_problem = "GPU does not support the selected FSR 4 shaders; using FSR 3.1";
        std::printf("Upscaler: %s unsupported on this GPU; falling back to FSR 3.1 before the first frame\n",
                    UpscalerName(requested));
        v.upscaler = UpscalerFsr3;
    }
}

void ConfigureDlssSupport(bool available, const char* problem) {
    auto& v = Get();
    v.dlss_supported = available;
    static std::string kept;
    kept = problem ? problem : "";
    v.dlss_problem = available || kept.empty() ? nullptr : kept.c_str();
    if (v.upscaler == UpscalerDlss && !available) {
        std::printf("Upscaler: DLSS unavailable (%s); falling back to FSR 3.1\n",
                    kept.empty() ? "bbport_dlss.dll or nvngx_dlss.dll missing" : kept.c_str());
        v.upscaler = UpscalerFsr3;
    }
}

bool FixedRenderSession() {
    const char* size = std::getenv("BB_RENDER_RES");
    return size && size[0];
}

int RenderPreset() {
    const auto& v = Get();
    return FixedRenderSession() ? v.startup_preset :
        v.upscaler == UpscalerTaa ? NativeAA : v.preset.load();
}

bool ResolutionNeedsRestart() {
    const auto& v = Get();
    // TAA needs the live path (native guest targets): run.sh selects it on restart.
    return FixedRenderSession() &&
        (v.preset != v.startup_preset || v.output_res != v.startup_output_res ||
         (v.upscaler == UpscalerOff) != (v.startup_upscaler == UpscalerOff) ||
         (v.upscaler == UpscalerTaa) != (v.startup_upscaler == UpscalerTaa));
}

void Save() {
    const auto& v = Get();
    FILE* file = std::fopen(Path(), "w");
    if (!file) {
        std::printf("Settings: cannot write %s\n", Path());
        return;
    }
    std::fprintf(file,
                 "# bbport settings (in-game menu: Insert / L3+R3)\n"
                 "upscaler=%s\npreset=%d\nsharpen=%d\nsharpness=%.2f\njitter=%d\n"
                 "reactive=%d\nobject_motion=%d\nreactive_scale=%.2f\nreactive_threshold=%.2f\nreactive_max=%.2f\n"
                 "debug_view=%d\nshow_fps=%d\nfsr4_auto_exposure=%d\nfsr4_invert_jitter=%d\n",
                 UpscalerName(v.upscaler), v.preset.load(), int(v.sharpen.load()),
                 v.sharpness.load(), int(v.jitter.load()), int(v.reactive.load()),
                 int(v.object_motion.load()),
                 v.reactive_scale.load(), v.reactive_threshold.load(), v.reactive_max.load(),
                 v.debug_view.load(), int(v.show_fps.load()),
                 int(v.fsr4_auto_exposure.load()), int(v.fsr4_invert_jitter.load()));
    static constexpr const char* prompts[PromptsCount] = {"auto", "playstation", "keyboard", "xbox", "switch"};
    std::fprintf(file, "button_prompts=%s\n", prompts[std::clamp(v.button_prompts.load(), 0, PromptsCount - 1)]);
    std::fprintf(file, "xbox_buttons=%s\nplaystation_buttons=%s\n", v.xbox_buttons ? "one" : "360",
                 v.playstation_buttons ? "ps5" : "ps4");
    std::fprintf(file, "motion_blur_reference=%d\n", v.motion_blur_reference.load());
    std::fprintf(file, "show_pedals=%d\n", int(v.show_pedals.load()));
    std::fprintf(file, "aa_scale=%s", AaScaleName(v.aa_scale));
    std::fputc(10, file);
    std::fprintf(file, "aa=%s", AaName(v.aa_mode));
    std::fputc(10, file);
    std::fprintf(file, "frame_gen=%s\nframe_gen_multiplier=%d\nframe_gen_fit_display=%d\n",
                 FrameGenName(v.frame_gen), v.frame_gen_multiplier.load(),
                 int(v.frame_gen_fit_display.load()));
    // Read by patches.py at start.
    for (int e = 0; e < EffectCount; ++e) {
        std::fprintf(file, "%s=%d\n", Effects[e].key, int(v.effects[e].load()));
    }
    std::fprintf(file, "model_lod=%d\noutput_res=%dx%d\n", v.model_lod.load(),
                 OutputWidths[v.output_res], OutputHeights[v.output_res]);
    // Read by run.sh at start.
    std::fprintf(file, "live_resolution=%s\n", v.live_resolution < 0 ? "auto"
                                                  : v.live_resolution ? "1" : "0");
    std::fclose(file);
}

float PresetScale(int preset) {
    static constexpr float scales[PresetCount] = {1.0f, 1.5f, 1.7f, 2.0f, 3.0f};
    return scales[std::clamp(preset, 0, PresetCount - 1)];
}

const char* PresetName(int preset) {
    static constexpr const char* names[PresetCount] = {"Native AA", "Quality", "Balanced",
                                                       "Performance", "Ultra Performance"};
    return names[std::clamp(preset, 0, PresetCount - 1)];
}

const char* FrameGenName(int frame_gen) {
    static constexpr const char* names[FrameGenCount] = {"off", "fsr3", "dlss"};
    return names[std::clamp(frame_gen, 0, FrameGenCount - 1)];
}

const char* AaName(int aa) {
    static constexpr const char* names[AaCount] = {"off", "fsr3", "dlss"};
    return names[std::clamp(aa, 0, AaCount - 1)];
}

void SaveHdrBrightness(int nits) {
    if (FILE* file = std::fopen(HdrIniPath().c_str(), "w")) {
        std::fprintf(file, "paper=%d\n", nits);
        std::fclose(file);
    }
}

void SavePicture() {
    const auto& v = Get();
    if (FILE* file = std::fopen(PictureIniPath().c_str(), "w")) {
        std::fprintf(file, "brightness=%d\ncontrast=%d\ngamma=%d\n", v.picture_brightness.load(),
                     v.picture_contrast.load(), v.picture_gamma.load());
        std::fclose(file);
    }
}

const char* AaScaleName(int scale) {
    static constexpr const char* names[AaScaleCount] = {"native", "quality", "balanced", "performance",
                                                        "ultra_performance"};
    return names[std::clamp(scale, 0, AaScaleCount - 1)];
}

float AaScaleFactor(int scale) {
    static constexpr float factors[AaScaleCount] = {1.0f, 1.5f, 1.7f, 2.0f, 3.0f};
    return factors[std::clamp(scale, 0, AaScaleCount - 1)];
}

const char* UpscalerName(int upscaler) {
    static constexpr const char* names[UpscalerCount] = {"off", "fsr3", "fsr4", "fsr411", "taa", "dlss"};
    return names[std::clamp(upscaler, 0, UpscalerCount - 1)];
}

} // namespace BbSettings
