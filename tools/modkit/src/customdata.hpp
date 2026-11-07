// SPDX-License-Identifier: GPL-2.0-or-later
// The `vehicle_custom_data` resource of a vehicle pack: named lamp and display glow settings of the car (HeadLight,
// TailLight, BrakeLight, SideLight, DashDisplayBacklight1 ...).
//
// Layout (little endian), found by reading the packs and checked in the game (the 458's rear lights glowed green
// after a change of TailLight and BrakeLight):
//   header   "EVOSLITL", the resource identifier (u64), u32 0x30000, u32 0, u32 0, u32 record count
//   records  96 bytes each: char name[64] (zero padded), float f0 .. f7
//              f0 f1 f2   colour (linear RGB), f3 intensity (the 458's HeadLight is 184, its BrakeLight 100),
//              f4 f5 f6   1, 1, 1 on every record seen, f7 100 on the lamps and 0.14 on the dash backlights
//   tail     more data after the records (tables of numbers), kept as it is
#pragma once

#include "common.hpp"
#include "json.hpp"

namespace dcmod {

struct CustomRecord {
    std::string name;
    float f[8] = {0};
};

struct CustomTable {
    Bytes header;
    std::vector<CustomRecord> records;
    Bytes tail;
};

constexpr size_t kCustomRecordSize = 96;
constexpr size_t kCustomHeaderSize = 32;

CustomTable ParseCustom(const Bytes& payload);  // throws Error when the layout does not fit
Bytes WriteCustom(const CustomTable& table);
Json CustomToJson(const CustomTable& table);
// "color" [r,g,b], "intensity", "f4" .. "f7"
void ApplyCustomSet(CustomRecord& rec, const Json& set);

}  // namespace dcmod
