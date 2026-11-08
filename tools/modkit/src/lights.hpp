// SPDX-License-Identifier: GPL-2.0-or-later
// The `lights` resource of a vehicle pack: the car's own light sources (headlights, brake lights, indicators ...).
//
// Layout (little endian), found by reading the packs and checked in the game (headlights of the Ferrari 458 turned blue
// and much brighter after a change of record 3):
//   header   "EVOSLITL", the resource identifier (u64), u16 3, u16 3, u32 record count
//   records  63 bytes each, not aligned:
//              float color[3]      linear RGB of the light
//              float intensity
//              u64   node          identifier of the node the light sits on, 0 when it has none (the headlights have one)
//              float f6 .. f14     nine more floats; their meaning is not decoded (see docs/modding/lights.md)
//              u8    type, u8 flag_a, u8 flag_b
//   tail     16 bytes after the records, kept as they are
#pragma once

#include "common.hpp"
#include "json.hpp"

namespace dcmod {

struct LightRecord {
    float color[3] = {1, 1, 1};
    float intensity = 1;
    uint64_t node = 0;
    float f[9] = {0};  // f6 .. f14
    uint8_t type = 0, flag_a = 0, flag_b = 0;
};

struct LightTable {
    Bytes header;  // everything before the first record
    std::vector<LightRecord> records;
    Bytes tail;
};

constexpr size_t kLightRecordSize = 63;
constexpr size_t kLightHeaderSize = 24;

// Throws Error when the payload is not a light table of this layout.
LightTable ParseLights(const Bytes& payload);
Bytes WriteLights(const LightTable& table);
Json LightsToJson(const LightTable& table);
// Changes the fields named in `set`: "color" [r,g,b], "intensity", "node" (hex), "type", "flags" [a,b], "f6" .. "f14".
// Unknown names throw.
void ApplyLightSet(LightRecord& rec, const Json& set);

}  // namespace dcmod
