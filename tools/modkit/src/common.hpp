// SPDX-License-Identifier: GPL-2.0-or-later
// Small helpers shared by the mod kit: files, little-endian numbers, strings.
#pragma once

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace dcmod {

namespace fs = std::filesystem;
using Bytes = std::vector<uint8_t>;

// Every failure the kit reports is an Error with a sentence a modder can act on.
struct Error : std::runtime_error {
    using std::runtime_error::runtime_error;
};

inline uint16_t Rd16(const uint8_t* p) { return uint16_t(p[0] | (p[1] << 8)); }
inline uint32_t Rd32(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (uint32_t(p[3]) << 24); }
inline uint64_t Rd64(const uint8_t* p) { return uint64_t(Rd32(p)) | (uint64_t(Rd32(p + 4)) << 32); }

inline void Wr16(Bytes& b, uint16_t v) {
    b.push_back(uint8_t(v));
    b.push_back(uint8_t(v >> 8));
}
inline void Wr32(Bytes& b, uint32_t v) {
    for (int i = 0; i < 4; ++i) b.push_back(uint8_t(v >> (8 * i)));
}
inline void Wr64(Bytes& b, uint64_t v) {
    for (int i = 0; i < 8; ++i) b.push_back(uint8_t(v >> (8 * i)));
}
inline void Put32(Bytes& b, size_t at, uint32_t v) {
    for (int i = 0; i < 4; ++i) b[at + size_t(i)] = uint8_t(v >> (8 * i));
}

inline Bytes ReadFile(const fs::path& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) throw Error("cannot open " + path.string());
    f.seekg(0, std::ios::end);
    const std::streamoff n = f.tellg();
    f.seekg(0, std::ios::beg);
    Bytes data(static_cast<size_t>(n));
    if (n > 0 && !f.read(reinterpret_cast<char*>(data.data()), n)) throw Error("cannot read " + path.string());
    return data;
}

inline std::string ReadText(const fs::path& path) {
    const Bytes b = ReadFile(path);
    return std::string(b.begin(), b.end());
}

inline void WriteFile(const fs::path& path, const uint8_t* data, size_t size) {
    if (path.has_parent_path()) {
        std::error_code ec;
        fs::create_directories(path.parent_path(), ec);
    }
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) throw Error("cannot write " + path.string());
    if (size && !f.write(reinterpret_cast<const char*>(data), std::streamsize(size))) throw Error("cannot write " + path.string());
}
inline void WriteFile(const fs::path& path, const Bytes& data) { WriteFile(path, data.data(), data.size()); }
inline void WriteText(const fs::path& path, std::string_view text) {
    WriteFile(path, reinterpret_cast<const uint8_t*>(text.data()), text.size());
}

inline std::string Lower(std::string s) {
    for (char& c : s) {
        if (c >= 'A' && c <= 'Z') c = char(c + 32);
    }
    return s;
}

// A path as the game spells it: forward slashes, no leading slash.
inline std::string GamePath(std::string s) {
    for (char& c : s) {
        if (c == '\\') c = '/';
    }
    while (!s.empty() && s.front() == '/') s.erase(s.begin());
    return s;
}

inline bool StartsWith(std::string_view s, std::string_view prefix) {
    return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
}
inline bool EndsWith(std::string_view s, std::string_view suffix) {
    return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

}  // namespace dcmod
