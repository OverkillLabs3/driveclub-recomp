// SPDX-License-Identifier: GPL-2.0-or-later
#include "customdata.hpp"

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

CustomTable ParseCustom(const Bytes& payload) {
    if (payload.size() < kCustomHeaderSize || std::memcmp(payload.data(), "EVOSLITL", 8) != 0) {
        throw Error("not a resource payload (no EVOSLITL header)");
    }
    const uint32_t count = Rd32(&payload[0x1c]);
    if (count > 256 || kCustomHeaderSize + size_t(count) * kCustomRecordSize > payload.size()) {
        throw Error("the custom data does not have the expected layout (" + std::to_string(count) + " records of " +
                    std::to_string(kCustomRecordSize) + " bytes do not fit in " + std::to_string(payload.size()) + " bytes)");
    }
    CustomTable t;
    t.header.assign(payload.begin(), payload.begin() + std::ptrdiff_t(kCustomHeaderSize));
    for (uint32_t i = 0; i < count; ++i) {
        const uint8_t* p = &payload[kCustomHeaderSize + size_t(i) * kCustomRecordSize];
        CustomRecord r;
        size_t n = 0;
        while (n < 64 && p[n]) ++n;
        r.name.assign(reinterpret_cast<const char*>(p), n);
        for (int k = 0; k < 8; ++k) r.f[k] = RdF(p + 64 + 4 * k);
        t.records.push_back(r);
    }
    t.tail.assign(payload.begin() + std::ptrdiff_t(kCustomHeaderSize + size_t(count) * kCustomRecordSize), payload.end());
    return t;
}

Bytes WriteCustom(const CustomTable& t) {
    Bytes out = t.header;
    Put32(out, 0x1c, uint32_t(t.records.size()));
    for (const CustomRecord& r : t.records) {
        const size_t at = out.size();
        out.resize(at + kCustomRecordSize, 0);
        uint8_t* p = &out[at];
        std::memcpy(p, r.name.data(), std::min<size_t>(r.name.size(), 63));
        for (int k = 0; k < 8; ++k) WrF(p + 64 + 4 * k, r.f[k]);
    }
    out.insert(out.end(), t.tail.begin(), t.tail.end());
    return out;
}

Json CustomToJson(const CustomTable& t) {
    Json list = Json::Array();
    for (const CustomRecord& r : t.records) {
        Json j = Json::Object();
        j.Set("name", r.name);
        Json c = Json::Array();
        for (int k = 0; k < 3; ++k) c.Push(double(r.f[k]));
        j.Set("color", c);
        j.Set("intensity", double(r.f[3]));
        for (int k = 4; k < 8; ++k) j.Set("f" + std::to_string(k), double(r.f[k]));
        list.Push(j);
    }
    return list;
}

void ApplyCustomSet(CustomRecord& r, const Json& set) {
    if (!set.IsObject()) throw Error("\"set\" must be an object such as {\"intensity\": 100}");
    for (const Json::Member& m : set.Members()) {
        const std::string& key = m.first;
        const Json& v = m.second;
        if (key == "color") {
            if (!v.IsArray() || v.Items().size() != 3) throw Error("\"color\" must be a list of three numbers [r, g, b]");
            for (size_t k = 0; k < 3; ++k) Number(v.Items()[k], "color", r.f[k]);
        } else if (key == "intensity") {
            Number(v, "intensity", r.f[3]);
        } else if (key.size() == 2 && key[0] == 'f' && key[1] >= '4' && key[1] <= '7') {
            Number(v, key.c_str(), r.f[key[1] - '0']);
        } else {
            throw Error("unknown field \"" + key + "\" (color, intensity, f4 .. f7)");
        }
    }
}

}  // namespace dcmod
