// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: a start that crashes inside a risky feature (frame generation, FSR 4, neural rendering: each loads a driver
// or vendor library and can take the process down in its first frames with nothing logged) must not crash the next
// start the same way. Begin() writes a marker naming the risky features this run uses; Frame() removes it after 30
// seconds of presented frames and so does a normal exit. A marker found at the next start means that start did not get
// that far: this run starts with those features off, and the overlay says so. The settings in the launcher are
// untouched, so the next start that gets through tries them again.
#pragma once

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "bbport_neural_settings.h"
#include "bbport_settings.h"

namespace BbSafeStart {

inline std::string& MarkerPath() {
    static std::string path = [] {
        const char* dir = std::getenv("BB_GPU_USER_DIR");
        return dir && *dir ? std::string{dir} + "/feature_start.txt" : std::string{};
    }();
    return path;
}

inline std::string& NoteText() {
    static std::string note;
    return note;
}

/// Shown in the overlay while non-empty: which features this run left off, and why.
inline const char* Note() {
    return NoteText().empty() ? nullptr : NoteText().c_str();
}

inline void Clear() {
    const std::string& path = MarkerPath();
    if (!path.empty()) {
        std::remove(path.c_str());
    }
}

inline void Begin() {
    const std::string& path = MarkerPath();
    if (path.empty()) {
        return;
    }
    auto& s = BbSettings::Get();
    if (FILE* previous = std::fopen(path.c_str(), "rb")) {
        char text[256] = {};
        const size_t n = std::fread(text, 1, sizeof(text) - 1, previous);
        text[n] = 0;
        std::fclose(previous);
        std::string left_off;
        const auto add = [&](const char* what) { left_off += (left_off.empty() ? "" : ", ") + std::string{what}; };
        if (s.frame_gen != BbSettings::FrameGenOff) {
            s.frame_gen = BbSettings::FrameGenOff;
            add("frame generation");
        }
        if (BbSettings::IsFsr4(s.upscaler) || s.upscaler == BbSettings::UpscalerAmd) {
            s.upscaler = BbSettings::UpscalerFsr3;
            add("FSR 4");
        }
        auto neural = BbNeural::Snapshot();
        if (neural.enabled) {
            neural.enabled = false;
            BbNeural::Set(neural);
            add("neural rendering");
        }
        std::printf("Safe start: the last start did not get through its first 30 seconds (it used: %s). "
                    "Left off this time: %s\n",
                    text[0] ? text : "unknown", left_off.empty() ? "nothing was on" : left_off.c_str());
        if (!left_off.empty()) {
            NoteText() = "The last start did not finish: " + left_off + " off this time";
        }
    }
    // What this run uses.
    std::string uses;
    const auto add_use = [&](const char* what) { uses += (uses.empty() ? "" : ", ") + std::string{what}; };
    if (s.frame_gen != BbSettings::FrameGenOff) {
        add_use("frame generation");
    }
    if (BbSettings::IsFsr4(s.upscaler) || s.upscaler == BbSettings::UpscalerAmd) {
        add_use("FSR 4");
    }
    if (BbNeural::Snapshot().enabled) {
        add_use("neural rendering");
    }
    if (uses.empty()) {
        Clear();   // nothing risky: no marker
        return;
    }
    if (FILE* marker = std::fopen(path.c_str(), "wb")) {
        std::fputs(uses.c_str(), marker);
        std::fclose(marker);
        std::atexit(Clear);
    }
}

/// Once per presented frame, `ms` long: after 30 seconds of frames the start counts as having got through.
inline void Frame(float ms) {
    static double seconds = 0.0;
    static bool done = false;
    if (done) {
        return;
    }
    seconds += double(ms) / 1000.0;
    if (seconds >= 30.0) {
        done = true;
        Clear();
    }
}

} // namespace BbSafeStart
