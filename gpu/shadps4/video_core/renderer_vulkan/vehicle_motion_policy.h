// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <algorithm>
#include <cstdint>
#include <iterator>

namespace Vulkan::VehicleMotionScope {
inline bool VehicleProgram(std::uint64_t hash) {
    static constexpr std::uint64_t programs[] = {
#include "vehicle_motion_programs.inc"
    };
    return std::binary_search(std::begin(programs), std::end(programs), hash);
}

// An explicit developer override can broaden admission; the normal/default
// policy never infers vehicle ownership from a constant-buffer size.
inline bool ExplicitAll(const char* value) {
    return value && value[0] == '1';
}

inline bool Broad(bool vehicle, bool explicit_all) {
    return vehicle || explicit_all;
}

inline unsigned MinimumTargets(bool vehicle, bool explicit_all) {
    return Broad(vehicle, explicit_all) ? 3u : 5u;
}

inline bool Admission(unsigned targets, bool vehicle, bool explicit_all, bool existing_guards) {
    return existing_guards && targets >= MinimumTargets(vehicle, explicit_all);
}

inline bool HashModelState(bool vehicle, unsigned size) {
    return vehicle && size == 416;
}
} // namespace Vulkan::VehicleMotionScope
