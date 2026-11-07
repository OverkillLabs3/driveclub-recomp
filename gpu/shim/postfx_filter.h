// SPDX-License-Identifier: GPL-2.0-or-later
// Post-processing filter chosen by mods (dcmod.postfx in the mod kit) or DC_POSTFX: the name of a SPIR-V fragment shader
// `postfx/<name>.spv` in a mods folder, and 16 numbers it reads from its push constants.
#pragma once

#include <array>
#include <atomic>
#include <mutex>
#include <string>

namespace ModPostFx {

inline std::mutex& NameMutex() {
    static std::mutex m;
    return m;
}
inline std::string& NameStore() {
    static std::string name = [] {
        const char* env = std::getenv("DC_POSTFX");
        return std::string(env ? env : "");
    }();
    return name;
}
inline std::string Name() {
    std::scoped_lock lock{NameMutex()};
    return NameStore();
}
inline void SetName(const std::string& name) {
    std::scoped_lock lock{NameMutex()};
    NameStore() = name;
}
inline std::array<std::atomic<float>, 16>& Values() {
    static std::array<std::atomic<float>, 16> values{};
    return values;
}

} // namespace ModPostFx
