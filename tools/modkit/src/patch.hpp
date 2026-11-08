// SPDX-License-Identifier: GPL-2.0-or-later
// The patch engine: a mod describes changes to the game's files ("set this cell of the vehicle table", "replace this
// text in this pack's XML") instead of shipping the files. The changes are applied to the user's own copy of the game
// data, in load order, so several mods can change the same file as long as they change different things, and the mod
// holds none of the game's data.
#pragma once

#include "archive.hpp"
#include "json.hpp"
#include "rpk.hpp"

#include <map>
#include <set>

namespace dcmod {

// Where the unmodified game files come from.
class FileSource {
public:
    virtual ~FileSource() = default;
    // The file's name as the game spells it (the archive's lower case path), or empty when there is no such file.
    virtual std::string Canonical(const std::string& path) = 0;
    virtual Bytes Read(const std::string& canonical) = 0;
    // Every file the source knows (lower case game paths), for patches that name files by a pattern.
    virtual std::vector<std::string> List() { return {}; }
    // A new index and volume that add `files` to the game's archive; false when the source has no archive.
    virtual bool ExtendArchive(const std::vector<std::pair<std::string, Bytes>>&, std::map<std::string, Bytes>&) { return false; }
};

class MemorySource : public FileSource {
public:
    std::map<std::string, Bytes> files;  // lower case game paths
    std::string Canonical(const std::string& path) override {
        const std::string k = Lower(GamePath(path));
        return files.count(k) ? k : std::string();
    }
    Bytes Read(const std::string& canonical) override { return files.at(canonical); }
    std::vector<std::string> List() override {
        std::vector<std::string> out;
        for (const auto& kv : files) out.push_back(kv.first);
        return out;
    }
};

// The game folders and archive, in the order the game itself looks: loose files in the update folder, then the base
// folder, then the archive.
class GameSource : public FileSource {
public:
    GameSource(Archive& archive, std::vector<fs::path> folders) : archive_(archive), folders_(std::move(folders)) {}
    std::string Canonical(const std::string& path) override;
    Bytes Read(const std::string& canonical) override;
    std::vector<std::string> List() override;
    bool ExtendArchive(const std::vector<std::pair<std::string, Bytes>>& files, std::map<std::string, Bytes>& out) override;

private:
    Archive& archive_;
    std::vector<fs::path> folders_;
};

struct PatchReport {
    struct Entry {
        std::string mod, action;
    };
    std::map<std::string, std::vector<Entry>> history;  // game path -> what each mod did to it, in order
    std::vector<std::string> warnings;
    std::vector<std::string> conflicts;  // two mods disagreeing about the same thing
};

// The files as they are after the mods so far. A file is read from the source the first time something needs it.
class Workspace {
public:
    Workspace(FileSource& source, PatchReport& report) : source_(source), report_(report) {}

    bool Exists(const std::string& path);
    // The files of the game (and the loose files of the folders) that a pattern with '*' and '?' matches, lower case paths.
    std::vector<std::string> Match(const std::string& pattern);
    Bytes Read(const std::string& path);
    void Write(const std::string& path, Bytes data, const std::string& mod, const std::string& action);

    // A resource pack, read once and kept parsed while mods change it. Call PackChanged after changing it.
    RpkPack& Pack(const std::string& path);
    void PackChanged(const std::string& path, const std::string& mod, const std::string& action);

    // Two mods writing different values to the same place (a table cell, a resource) is a conflict worth reporting.
    void NoteSetting(const std::string& slot, const std::string& value, const std::string& mod);

    // Files that the game's archive index is to list (see Archive::Extend).
    void Register(const std::string& path) { registered_.insert(Key(path)); }

    // Turns the changed packs back into files. Returns every file that differs from the source, by game path.
    std::map<std::string, Bytes> Finish();

    PatchReport& report() { return report_; }

private:
    struct Slot {
        std::string value, mod;
    };
    struct Item {
        std::string name;  // path as the game spells it
        Bytes data;
        bool loaded = false, changed = false;
        std::unique_ptr<RpkPack> pack;
        bool pack_changed = false;
    };
    Item& Get(const std::string& path, bool must_exist);
    static std::string Key(const std::string& path) { return Lower(GamePath(path)); }

    FileSource& source_;
    PatchReport& report_;
    std::map<std::string, Item> items_;
    std::map<std::string, Slot> settings_;
    std::set<std::string> registered_;
};

struct PatchEnv {
    Workspace& ws;
    std::string mod;   // the mod's id, for messages and the history
    fs::path dir;      // the mod's folder; `from` files are read from here
};

// Applies a list of operations. Throws an Error that names the operation that failed.
void ApplyPatches(const Json& ops, PatchEnv& env);

}  // namespace dcmod
