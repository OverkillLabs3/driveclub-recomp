// SPDX-License-Identifier: GPL-2.0-or-later
#include "textfile.hpp"

namespace dcmod {

bool IsValidUtf8(std::string_view s) {
    size_t i = 0;
    while (i < s.size()) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        size_t n;
        if (c < 0x80) n = 1;
        else if (c >= 0xC2 && c <= 0xDF) n = 2;
        else if (c >= 0xE0 && c <= 0xEF) n = 3;
        else if (c >= 0xF0 && c <= 0xF4) n = 4;
        else return false;
        if (i + n > s.size()) return false;
        for (size_t k = 1; k < n; ++k) {
            if ((static_cast<unsigned char>(s[i + k]) & 0xC0) != 0x80) return false;
        }
        i += n;
    }
    return true;
}

TextFile TextFile::Load(const Bytes& bytes) {
    TextFile t;
    std::string s(bytes.begin(), bytes.end());
    if (s.size() >= 3 && static_cast<unsigned char>(s[0]) == 0xEF && static_cast<unsigned char>(s[1]) == 0xBB &&
        static_cast<unsigned char>(s[2]) == 0xBF) {
        t.bom = true;
        s.erase(0, 3);
    }
    t.latin1 = !IsValidUtf8(s);
    size_t crlf = 0, lf = 0;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\n') {
            ++lf;
            if (i > 0 && s[i - 1] == '\r') ++crlf;
        }
    }
    if (crlf > 0 && crlf == lf) {
        t.crlf = true;
        std::string out;
        out.reserve(s.size());
        for (size_t i = 0; i < s.size(); ++i) {
            if (s[i] == '\r' && i + 1 < s.size() && s[i + 1] == '\n') continue;
            out.push_back(s[i]);
        }
        s = std::move(out);
    } else if (crlf > 0) {
        t.raw = true;
    }
    t.text = std::move(s);
    return t;
}

Bytes TextFile::Save() const {
    Bytes out;
    out.reserve(text.size() + text.size() / 20 + 3);
    if (bom) out.insert(out.end(), {0xEF, 0xBB, 0xBF});
    for (char c : text) {
        if (c == '\n' && crlf) out.push_back('\r');
        out.push_back(static_cast<uint8_t>(c));
    }
    return out;
}

std::string TextFile::FromPatch(const std::string& utf8) const {
    std::string s;
    s.reserve(utf8.size());
    for (size_t i = 0; i < utf8.size(); ++i) {
        if (utf8[i] == '\r' && i + 1 < utf8.size() && utf8[i + 1] == '\n' && !raw) continue;
        s.push_back(utf8[i]);
    }
    if (!latin1) return s;
    // Latin-1 files: the patch's UTF-8 letters become single bytes.
    std::string out;
    for (size_t i = 0; i < s.size();) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        if (c < 0x80) {
            out.push_back(char(c));
            ++i;
        } else if (c >= 0xC2 && c <= 0xC3 && i + 1 < s.size()) {
            out.push_back(char(((c & 0x03) << 6) | (static_cast<unsigned char>(s[i + 1]) & 0x3F)));
            i += 2;
        } else {
            throw Error("this file is Latin-1 text and cannot hold the character in \"" + utf8 + "\"");
        }
    }
    return out;
}

}  // namespace dcmod
