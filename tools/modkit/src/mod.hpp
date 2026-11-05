// SPDX-License-Identifier: GPL-2.0-or-later
// Mods as folders: a manifest (mod.json), loose files that replace game files, and patches that change them.
#pragma once

#include "patch.hpp"

namespace dcmod {

struct ModDep {
    std::string id, version;  // version is a requirement such as ">=1.2", or empty
    bool optional = false;
};

struct Mod {
    fs::path dir;
    std::string id, name, version, author, description, game;
    std::vector<ModDep> requires_;
    std::vector<std::string> conflicts, after, before;
    Json patches = Json::Array();
    fs::path files_dir;  // loose files; empty when the mod has none
};

// Reads and checks a mod folder. Throws an Error that says what is wrong with mod.json.
// `fallback_id` names a mod that has no id in its manifest (default: the folder name).
Mod LoadMod(const fs::path& dir, const std::string& fallback_id = {});

// Puts the mods in load order: the order given, moved only as far as requirements and after/before need. Problems
// (missing requirements, conflicts, cycles) are added to `problems`; a cycle also throws.
std::vector<size_t> OrderMods(const std::vector<Mod>& mods, std::vector<std::string>& problems);

// Applies the mods (already in load order) to the game files and returns the files that changed.
std::map<std::string, Bytes> BuildMods(const std::vector<Mod>& ordered, FileSource& source, PatchReport& report);

// A new mod folder with a manifest and one example patch.
void NewMod(const fs::path& dir, const std::string& id, const std::string& name);

}  // namespace dcmod
