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
    std::vector<std::string> scripts;  // Lua files (game paths) that run at start-up, in this order
    Json patches = Json::Array();
    Json settings_def = Json::Object();  // "settings" of the manifest: name -> {label, type, default, min, max, choices}
    Json settings = Json::Object();      // the values in use: the defaults, changed by what the user chose
    fs::path files_dir;  // loose files; empty when the mod has none
};

// Reads and checks a mod folder. Throws an Error that says what is wrong with mod.json.
// `fallback_id` names a mod that has no id in its manifest (default: the folder name).
Mod LoadMod(const fs::path& dir, const std::string& fallback_id = {});

// Takes the user's choices for this mod (a JSON object name -> value) over the defaults. Throws an Error for a name the
// mod does not have or a value of the wrong type or outside its range.
void ApplySettings(Mod& mod, const Json& choices);

// Puts the mods in load order: the order given, moved only as far as requirements and after/before need. Problems
// (missing requirements, conflicts, cycles) are added to `problems`; a cycle also throws.
std::vector<size_t> OrderMods(const std::vector<Mod>& mods, std::vector<std::string>& problems);

// Applies the mods (already in load order) to the game files and returns the files that changed.
std::map<std::string, Bytes> BuildMods(const std::vector<Mod>& ordered, FileSource& source, PatchReport& report);

// Adds the boot script wrapper when mods list scripts (see scripts.cpp).
void AddScriptLoader(Workspace& ws, const std::vector<Mod>& ordered);
// The Lua text that gives the scripts their mods' settings (dcmod.settings and dcmod.setting).
std::string SettingsLua(const std::vector<Mod>& mods);
std::string BootWrapper(const std::string& original, const std::vector<std::string>& scripts, const std::string& settings_lua = {});

// A new mod folder with a manifest and one example patch.
void NewMod(const fs::path& dir, const std::string& id, const std::string& name);

}  // namespace dcmod
