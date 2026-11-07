// SPDX-License-Identifier: GPL-2.0-or-later
#include "lights.hpp"

#include "rpk.hpp"

#include <cmath>
#include <cstring>

namespace dcmod {

namespace {

float RdF(const uint8_t* p) {
    float v;
    std::memcpy(&v, p, 4);
    return v;
}

void WrF(uint8_t* p, float v) { std::memcpy(p, &v, 4); }

void Number(const Json& j, const char* what, float& out) {
    if (!j.IsNumber()) throw Error(std::string("\"") + what + "\" must be a number");
    out = float(j.AsNumber());
}

}  // namespace

LightTable ParseLights(const Bytes& payload) {
    if (payload.size() < kLightHeaderSize || std::memcmp(payload.data(), "EVOSLITL", 8) != 0) {
        throw Error("not a resource payload (no EVOSLITL header)");
    }
    const uint32_t count = Rd32(&payload[0x14]);
    if (count > 4096 || kLightHeaderSize + size_t(count) * kLightRecordSize > payload.size()) {
        throw Error("the light table does not have the expected layout (" + std::to_string(count) + " records of " +
                    std::to_string(kLightRecordSize) + " bytes do not fit in " + std::to_string(payload.size()) + " bytes)");
    }
    LightTable t;
    t.header.assign(payload.begin(), payload.begin() + std::ptrdiff_t(kLightHeaderSize));
    for (uint32_t i = 0; i < count; ++i) {
        const uint8_t* p = &payload[kLightHeaderSize + size_t(i) * kLightRecordSize];
        LightRecord r;
        for (int k = 0; k < 3; ++k) r.color[k] = RdF(p + 4 * k);
        r.intensity = RdF(p + 12);
        r.node = Rd64(p + 16);
        for (int k = 0; k < 9; ++k) r.f[k] = RdF(p + 24 + 4 * k);
        r.type = p[60];
        r.flag_a = p[61];
        r.flag_b = p[62];
        t.records.push_back(r);
    }
    t.tail.assign(payload.begin() + std::ptrdiff_t(kLightHeaderSize + size_t(count) * kLightRecordSize), payload.end());
    return t;
}

Bytes WriteLights(const LightTable& t) {
    Bytes out = t.header;
    Put32(out, 0x14, uint32_t(t.records.size()));
    for (const LightRecord& r : t.records) {
        const size_t at = out.size();
        out.resize(at + kLightRecordSize);
        uint8_t* p = &out[at];
        for (int k = 0; k < 3; ++k) WrF(p + 4 * k, r.color[k]);
        WrF(p + 12, r.intensity);
        for (int k = 0; k < 8; ++k) p[16 + k] = uint8_t(r.node >> (8 * k));
        for (int k = 0; k < 9; ++k) WrF(p + 24 + 4 * k, r.f[k]);
        p[60] = r.type;
        p[61] = r.flag_a;
        p[62] = r.flag_b;
    }
    out.insert(out.end(), t.tail.begin(), t.tail.end());
    return out;
}

Json LightsToJson(const LightTable& t) {
    Json lights = Json::Array();
    for (size_t i = 0; i < t.records.size(); ++i) {
        const LightRecord& r = t.records[i];
        Json j = Json::Object();
        j.Set("index", i);
        j.Set("type", int(r.type));
        Json c = Json::Array();
        for (float v : r.color) c.Push(double(v));
        j.Set("color", c);
        j.Set("intensity", double(r.intensity));
        j.Set("node", r.node ? HexId(r.node) : std::string());
        Json flags = Json::Array();
        flags.Push(int(r.flag_a));
        flags.Push(int(r.flag_b));
        j.Set("flags", flags);
        for (int k = 0; k < 9; ++k) j.Set("f" + std::to_string(6 + k), double(r.f[k]));
        lights.Push(j);
    }
    return lights;
}

void ApplyLightSet(LightRecord& r, const Json& set) {
    if (!set.IsObject()) throw Error("\"set\" must be an object such as {\"intensity\": 100}");
    for (const Json::Member& m : set.Members()) {
        const std::string& key = m.first;
        const Json& v = m.second;
        if (key == "color") {
            if (!v.IsArray() || v.Items().size() != 3) throw Error("\"color\" must be a list of three numbers [r, g, b]");
            for (size_t k = 0; k < 3; ++k) Number(v.Items()[k], "color", r.color[k]);
        } else if (key == "intensity") {
            Number(v, "intensity", r.intensity);
        } else if (key == "node") {
            if (!v.IsString()) throw Error("\"node\" must be a hex identifier (or \"\" for none)");
            r.node = v.AsString().empty() ? 0 : ParseHexId(v.AsString());
        } else if (key == "type") {
            if (!v.IsNumber() || v.AsInt() < 0 || v.AsInt() > 255) throw Error("\"type\" must be a number from 0 to 255");
            r.type = uint8_t(v.AsInt());
        } else if (key == "flags") {
            if (!v.IsArray() || v.Items().size() != 2) throw Error("\"flags\" must be a list of two numbers");
            r.flag_a = uint8_t(v.Items()[0].AsInt());
            r.flag_b = uint8_t(v.Items()[1].AsInt());
        } else if (key.size() >= 2 && key[0] == 'f' && key.find_first_not_of("0123456789", 1) == std::string::npos &&
                   std::stoi(key.substr(1)) >= 6 && std::stoi(key.substr(1)) <= 14) {
            Number(v, key.c_str(), r.f[std::stoi(key.substr(1)) - 6]);
        } else {
            throw Error("unknown light field \"" + key + "\" (color, intensity, node, type, flags, f6 .. f14)");
        }
    }
}

}  // namespace dcmod
