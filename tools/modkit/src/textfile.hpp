// SPDX-License-Identifier: GPL-2.0-or-later
// Text files as the patch operations see them: line breaks normalised to '\n', and the file's own conventions (CRLF,
// Latin-1) put back when it is saved, so that a patch written with "\n" and UTF-8 text works on any game file.
#pragma once

#include "common.hpp"

namespace dcmod {

struct TextFile {
    std::string text;      // with '\n' line breaks
    bool crlf = false;     // every line break was "\r\n"; they are written that way again
    bool latin1 = false;   // the bytes are not valid UTF-8, so they are taken as Latin-1
    bool bom = false;      // starts with a UTF-8 byte order mark (kept out of `text`)
    bool raw = false;      // line breaks are mixed; `text` is the bytes as they are and nothing is converted

    static TextFile Load(const Bytes& bytes);
    Bytes Save() const;

    // A string from a patch (UTF-8, any line breaks) in the form that matches `text`.
    std::string FromPatch(const std::string& utf8) const;
};

bool IsValidUtf8(std::string_view s);

}  // namespace dcmod
