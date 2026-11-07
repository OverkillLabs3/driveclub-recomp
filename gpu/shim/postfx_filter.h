// SPDX-License-Identifier: GPL-2.0-or-later
// Post-processing filter chosen by mods (dcmod.postfx in the mod kit) or DC_POSTFX: the name of a SPIR-V fragment shader
// `postfx/<name>.spv` in a mods folder, and 16 numbers it reads from its push constants.
#pragma once

#include <array>
#include <atomic>
#include <mutex>
#include <string>

namespace ModPostFx {

// Filters run one after the other, slot 0 first; each slot has its own shader name and its own 16 numbers.
// Slots 3 to 5 run earlier, on the upscaled HDR image (DriveAA), before the game's bloom and tone mapping.
constexpr int kSlots = 6;   // 0 to 2 on the finished picture, 3 to 5 on the HDR image before the game's own post

inline std::mutex& NameMutex() {
    static std::mutex m;
    return m;
}
inline std::array<std::string, kSlots>& NameStore() {
    static std::array<std::string, kSlots> names = [] {
        std::array<std::string, kSlots> n;
        const char* env = std::getenv("DC_POSTFX");
        n[0] = env ? env : "";
        return n;
    }();
    return names;
}
inline std::string Name(int slot = 0) {
    std::scoped_lock lock{NameMutex()};
    return NameStore()[slot];
}
inline void SetName(const std::string& name, int slot = 0) {
    std::scoped_lock lock{NameMutex()};
    NameStore()[slot] = name;
}
inline std::array<std::atomic<float>, 16>& Values(int slot = 0) {
    static std::array<std::array<std::atomic<float>, 16>, kSlots> values{};
    return values[slot];
}

} // namespace ModPostFx
