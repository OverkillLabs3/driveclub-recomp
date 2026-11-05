// SPDX-License-Identifier: GPL-2.0-or-later
// Makes a mod from edited files: edit the game's files in any editor, and this finds what changed and writes the
// patches (table cells, text hunks, resources inside packs) so that the mod holds only the difference.
#pragma once

#include "mod.hpp"

namespace dcmod {

struct DiffResult {
    size_t patches = 0, loose_files = 0, unchanged = 0;
    std::vector<std::string> notes;
};

// `edited` holds files at their game paths. A folder named like a pack (global.rpk) that holds a pack.json from
// `dcmod rpk extract` is compared resource by resource. Writes `out`/mod.json and the files it needs, then builds the
// mod again on top of the game files and checks that the result is the edited files.
DiffResult MakeModFromEdits(FileSource& source, const fs::path& edited, const fs::path& out, const std::string& id, const std::string& name);

}  // namespace dcmod
