// SPDX-License-Identifier: GPL-2.0-or-later
// dcmod track ...: shows a track: its row of newdata/tracks.csv and the files named after its RPKName (the level data, the
// world pack and its patch packs), and makes a mod that copies it under a new name.
#include "cli.hpp"
#include "csv.hpp"
#include "mod.hpp"
#include "rpk.hpp"

#include <algorithm>

namespace dcmod {

namespace {

int Usage() {
    std::puts(
        "dcmod track: the files and table row of a track\n"
        "\n"
        "  dcmod track list           every track: Track Name and the name shown in menus\n"
        "  dcmod track files NAME     the table row of a track (NAME is its Track Name) and every file named after it\n"
        "  dcmod track clone NAME NEWNAME -o MODFOLDER [--title TEXT] [--layers OLD:NEW]\n"
        "                             a mod that copies the track under a new name (NEWNAME as long as NAME and the same\n"
        "                             except for the circuit number: canada_road_circuit_02_n to canada_road_circuit_09_n)\n"
        "\n"
        "The game folders come from --update DIR --base DIR, DRIVECLUB_UPDATE_DIR and DRIVECLUB_BASE_DIR, or launcher.ini.");
    return 2;
}

// True when a path segment starts with `asset` followed by nothing, '.', '_' or '/'.
bool Named(const std::string& path, const std::string& asset) {
    size_t pos = 0;
    while (pos <= path.size()) {
        size_t slash = path.find('/', pos);
        if (slash == std::string::npos) slash = path.size();
        const std::string seg = Lower(path.substr(pos, slash - pos));
        if (seg.compare(0, asset.size(), Lower(asset)) == 0) {
            const char next = seg.size() > asset.size() ? seg[asset.size()] : '\0';
            if (next == '\0' || next == '.' || next == '_') return true;
        }
        pos = slash + 1;
    }
    return false;
}

std::string ReplaceFirstName(const std::string& path, const std::string& asset, const std::string& fresh) {
    std::string out;
    size_t pos = 0;
    bool hit = false;
    while (pos <= path.size()) {
        size_t slash = path.find('/', pos);
        if (slash == std::string::npos) slash = path.size();
        std::string seg = path.substr(pos, slash - pos);
        if (Lower(seg).compare(0, asset.size(), Lower(asset)) == 0) {
            const char next = seg.size() > asset.size() ? seg[asset.size()] : '\0';
            if (next == '\0' || next == '.' || next == '_') seg = fresh + seg.substr(asset.size()), hit = true;
        }
        out += seg;
        if (slash < path.size()) out.push_back('/');
        pos = slash + 1;
    }
    return hit ? out : std::string();
}

uint32_t NameCrc(const std::string& t) {
    uint32_t c = 0xFFFFFFFFu;
    for (const char ch : t) {
        c ^= uint8_t(ch);
        for (int k = 0; k < 8; ++k) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
    }
    return ~c;
}

std::string ReplaceAll(std::string text, const std::string& find, const std::string& repl) {
    for (size_t at = 0; (at = text.find(find, at)) != std::string::npos; at += repl.size()) text.replace(at, find.size(), repl);
    return text;
}

bool Contains(const Bytes& d, const std::string& t) {
    return !t.empty() && std::search(d.begin(), d.end(), t.begin(), t.end()) != d.end();
}

std::vector<std::string> Lines(const std::string& text) {
    std::vector<std::string> out;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t nl = text.find('\n', pos);
        if (nl == std::string::npos) nl = text.size();
        std::string l = text.substr(pos, nl - pos);
        if (!l.empty() && l.back() == '\r') l.pop_back();
        out.push_back(l);
        pos = nl + 1;
    }
    return out;
}

// The mod that copies a track under a new name. What the game keys by the track's name was found by running a copy:
// the world and the level data are found by the names of their resources, the world holds the CRC32 of the names of its
// scene objects, the layers are named after the circuit, the interface has pictures and text per track and one Lua
// script has a table by track name. Each of those is a patch below.
int CloneTrack(const Args& args, GameSource& source, Archive& archive, const CsvDoc& doc, size_t row, int name_col, int rpk_col, const std::string& table_path) {
    const std::string out = args.Get("-o", args.Get("--output"));
    if (out.empty() || args.positional.size() != 3) throw Error("usage: dcmod track clone NAME NEWNAME -o MODFOLDER [--title TEXT] [--layers OLD:NEW]");
    const std::string name = args.positional[1], fresh = args.positional[2];
    const std::string asset = doc.Get(row, size_t(rpk_col)), fresh_asset = fresh;
    if (name.size() != fresh.size() || asset != name) throw Error("NEWNAME must be as long as the name of the track (the name is stored inside packs that keep their size) and the track's RPKName must be its Track Name");
    for (size_t r = 1; r < doc.rows.size(); ++r) {
        if (doc.Get(r, size_t(name_col)) == fresh) throw Error("a track named \"" + fresh + "\" already exists");
    }
    // the circuit part of the name (up to the number), which the world's resources are named after
    size_t digits = 0;
    for (size_t i = 0; i + 3 < asset.size(); ++i) {
        if (asset[i] == '_' && isdigit(uint8_t(asset[i + 1])) && isdigit(uint8_t(asset[i + 2]))) {
            digits = i + 1;
            break;
        }
    }
    if (!digits) throw Error("the track name has no circuit number (_NN) to tell it from the copy");
    const std::string base = asset.substr(0, digits + 2), fresh_base = fresh.substr(0, digits + 2);
    if (base == fresh_base) throw Error("NEWNAME must differ from the track name in the circuit number");
    const std::string old_num = asset.substr(digits, 2), new_num = fresh.substr(digits, 2);
    const std::string title = args.Get("--title", fresh);
    long next_id = 0;
    const int id_col = doc.Column("UniqueID");
    for (size_t r = 1; r < doc.rows.size() && id_col >= 0; ++r) {
        const std::string& v = doc.Get(r, size_t(id_col));
        if (!v.empty() && v.find_first_not_of("0123456789") == std::string::npos) next_id = std::max(next_id, std::stol(v));
    }
    Json patches = Json::Array();
    const auto op = [&](const char* op_name) {
        Json j = Json::Object();
        j.Set("op", op_name);
        return j;
    };
    // 1. the row
    {
        Json cells = Json::Object();
        cells.Set("Track Name", fresh);
        cells.Set("RPKName", fresh_asset);
        if (id_col >= 0) cells.Set("UniqueID", std::to_string(next_id + 1));
        if (doc.Column("PRODUCT_NAME_1") >= 0) cells.Set("PRODUCT_NAME_1", title);
        Json add = op("csv.add");
        add.Set("file", table_path);
        add.Set("key", "Track Name");
        add.Set("like", name);
        add.Set("after", name);
        add.Set("row", cells);
        patches.Push(add);
    }
    // 2. the files named after the track, and the packs' own names and requirements
    std::vector<std::string> files;
    for (const IndexEntry& e : archive.Entries()) {
        if (Named(e.name, asset) && (StartsWith(e.name, "worlds/") || StartsWith(e.name, "data/leveldata/"))) files.push_back(e.name);
    }
    std::sort(files.begin(), files.end());
    if (files.empty()) throw Error("no files are named after the track");
    std::vector<std::string> copies;
    for (const std::string& f : files) {
        const std::string to = ReplaceFirstName(f, asset, fresh_asset);
        copies.push_back(to);
        Json c = op("file.copy");
        c.Set("from", f);
        c.Set("file", to);
        patches.Push(c);
    }
    if (source.Canonical("scripts/filemanifest.cfg").size()) {
        Json listed = op("manifest.add");
        listed.Set("file", "scripts/filemanifest.cfg");
        Json paths = Json::Array();
        for (const std::string& f : copies) paths.Push(f);
        listed.Set("paths", paths);
        patches.Push(listed);
    }
    // layers of the world: the text before "Tier1" (Cana_C02_) holds the circuit number
    std::string layer_old, layer_new;
    if (args.Has("--layers")) {
        const std::string v = args.Get("--layers");
        const size_t colon = v.find(':');
        if (colon == std::string::npos) throw Error("--layers is OLD:NEW");
        layer_old = v.substr(0, colon), layer_new = v.substr(colon + 1);
    }
    // the names of the scene objects of all the track's packs: any of them may hold the hash of a name from another
    std::vector<std::string> all_hashed{asset};
    for (const std::string& f : files) {
        if (!EndsWith(f, ".rpk")) continue;
        for (const RpkResource& r : RpkPack::Parse(source.Read(source.Canonical(f))).resources) {
            for (const std::string& n : r.names) {
                if (r.Type() == 0x14 && n.find(base) != std::string::npos && std::find(all_hashed.begin(), all_hashed.end(), n) == all_hashed.end()) all_hashed.push_back(n);
            }
        }
    }
    for (size_t fi = 0; fi < files.size(); ++fi) {
        if (!EndsWith(files[fi], ".rpk")) continue;
        const RpkPack pack = RpkPack::Parse(source.Read(source.Canonical(files[fi])));
        const std::string& to = copies[fi];
        const bool needs_required = std::any_of(pack.required.begin(), pack.required.end(), [&](const std::string& r) { return r.find(asset) != std::string::npos; });
        if (needs_required) {
            Json ren = op("rpk.rename");
            ren.Set("file", to);
            ren.Set("find", asset);
            ren.Set("replace", fresh_asset);
            ren.Set("scope", "required");
            patches.Push(ren);
        }
        // resources are found by name: the names that hold the circuit
        bool any_name = false;
        std::vector<std::string> hashed;
        for (const RpkResource& r : pack.resources) {
            for (const std::string& n : r.names) {
                if (n.find(base) != std::string::npos) {
                    any_name = true;
                    if (r.Type() == 0x14 && std::find(hashed.begin(), hashed.end(), n) == hashed.end()) hashed.push_back(n);
                }
            }
        }
        for (const RpkAlias& a : pack.aliases) any_name = any_name || a.name.find(base) != std::string::npos;
        if (any_name) {
            Json ren = op("rpk.rename");
            ren.Set("file", to);
            ren.Set("find", base);
            ren.Set("replace", fresh_base);
            ren.Set("scope", "names");
            patches.Push(ren);
        }
        hashed = all_hashed;
        for (const RpkResource& r : pack.resources) {
            const std::string ref = HexId(r.id);
            if (r.Type() == 13) {  // world: the layer names and the CRC32 of the names of what it holds
                if (layer_old.empty()) {
                    const std::string text(r.data.begin(), r.data.end());
                    const size_t at = text.find("Tier1");
                    if (at != std::string::npos) {
                        size_t b = at;
                        while (b > 0 && (isalnum(uint8_t(text[b - 1])) || text[b - 1] == '_')) --b;
                        const std::string prefix = text.substr(b, at - b);
                        const size_t np = prefix.find(old_num);
                        if (np != std::string::npos) layer_old = prefix, layer_new = prefix.substr(0, np) + new_num + prefix.substr(np + 2);
                    }
                }
                if (!layer_old.empty() && Contains(r.data, layer_old)) {
                    Json rr = op("rpk.replace");
                    rr.Set("file", to), rr.Set("resource", ref), rr.Set("find", layer_old), rr.Set("replace", layer_new);
                    patches.Push(rr);
                }
                for (const std::string& n : hashed) {
                    const uint32_t c = NameCrc(n);
                    Bytes key(4);
                    for (int k = 0; k < 4; ++k) key[size_t(k)] = uint8_t(c >> (8 * k));
                    if (std::search(r.data.begin(), r.data.end(), key.begin(), key.end()) == r.data.end()) continue;
                    Json rr = op("rpk.replace");
                    rr.Set("file", to), rr.Set("resource", ref), rr.Set("hash", "crc32"), rr.Set("find", n), rr.Set("replace", ReplaceAll(n, base, fresh_base));
                    patches.Push(rr);
                }
            }
            if ((r.Type() == 13 || r.Type() == 0x14) && Contains(r.data, base)) {
                Json rr = op("rpk.replace");
                rr.Set("file", to), rr.Set("resource", ref), rr.Set("find", base), rr.Set("replace", fresh_base);
                patches.Push(rr);
            }
        }
    }
    // 3. the pictures of the interface that carry the track's name
    for (const IndexEntry& e : archive.Entries()) {
        if (!StartsWith(e.name, "newui/art/") || !EndsWith(e.name, ".rpk")) continue;
        const Bytes raw = source.Read(source.Canonical(e.name));
        if (!Contains(raw, asset)) continue;
        const RpkPack pack = RpkPack::Parse(raw);
        for (const RpkResource& r : pack.resources) {
            for (const std::string& n : r.names) {
                if (n.find(asset) == std::string::npos) continue;
                Json c = op("rpk.copy");
                c.Set("file", e.name), c.Set("resource", n), c.Set("name", ReplaceAll(n, asset, fresh_asset)), c.Set("optional", true);
                patches.Push(c);
            }
        }
    }
    // 4. text and tables with a line per track
    const auto insert_after = [&](const std::string& file, const std::string& after_line, const std::string& text) {
        Json t = op("text.insert");
        t.Set("file", file), t.Set("after", after_line), t.Set("text", text), t.Set("optional", true);
        patches.Push(t);
    };
    for (const IndexEntry& e : archive.Entries()) {
        if (!StartsWith(e.name, "gui/lams/") || !EndsWith(e.name, ".csv")) continue;
        const Bytes b = source.Read(source.Canonical(e.name));
        const std::vector<std::string> lines = Lines(std::string(b.begin(), b.end()));
        std::vector<std::string> group;
        const auto flush = [&]() {
            if (group.empty()) return;
            std::string text;
            for (const std::string& g : group) text += "\n" + ReplaceAll(g, asset, fresh_asset);
            insert_after(e.name, group.back(), text);
            group.clear();
        };
        for (const std::string& l : lines) {
            const size_t k = l.find(asset);
            const bool mine = k != std::string::npos && k <= 1 && (l.size() == asset.size() + k || l[asset.size() + k] == ',' || l[asset.size() + k] == '_' || l[asset.size() + k] == '\x03');
            if (mine) group.push_back(l);
            else flush();
        }
        flush();
    }
    for (const char* f : {"newui/control/pre_loaded_textures.ctl", "newdata/rank/elite.csv"}) {
        if (!source.Canonical(f).size()) continue;
        const Bytes b = source.Read(source.Canonical(f));
        for (const std::string& l : Lines(std::string(b.begin(), b.end()))) {
            if (l.find(asset) == std::string::npos) continue;
            std::string copy = ReplaceAll(l, asset, fresh_asset);
            if (EndsWith(f, "elite.csv") && !copy.empty() && isdigit(uint8_t(copy[0]))) {
                const size_t comma = copy.find(',');
                copy = std::to_string(std::stoll(copy.substr(0, comma)) + 1) + copy.substr(comma);
            }
            insert_after(f, l, "\n" + copy);
        }
    }
    // 5. Lua scripts with a table by track name
    for (const IndexEntry& e : archive.Entries()) {
        if (!StartsWith(e.name, "scripts/lua/") || !EndsWith(e.name, ".lua.bin")) continue;
        if (!Contains(source.Read(source.Canonical(e.name)), asset)) continue;
        Json a = op("lua.alias");
        a.Set("file", e.name), a.Set("find", asset), a.Set("replace", fresh_asset), a.Set("optional", true);
        patches.Push(a);
    }
    Json manifest = Json::Object();
    manifest.Set("id", Lower(fresh));
    manifest.Set("name", fresh + " (copy of " + name + ")");
    manifest.Set("version", "1.0");
    manifest.Set("category", "Tracks");
    manifest.Set("patches", patches);
    WriteText(fs::path(out) / "mod.json", manifest.Dump());
    std::printf("%zu patches, %zu files will be copied; mod written to %s\n", patches.Items().size(), files.size(), out.c_str());
    return 0;
}

}  // namespace

int RunTrack(const std::vector<std::string>& argv) {
    if (argv.empty() || (argv[0] != "files" && argv[0] != "list" && argv[0] != "clone") || (argv[0] != "list" && argv.size() < 2)) return Usage();
    const Args args = ParseArgs(argv, {"--update", "--base", "--game", "-o", "--output", "--title", "--layers"});
    if (args.positional.size() < 2 && argv[0] != "list") return Usage();
    auto archive = OpenGame(args);
    GameSource source(*archive, GameFolders(args));
    const std::string table_path = source.Canonical("newdata/tracks.csv");
    if (table_path.empty()) throw Error("newdata/tracks.csv is not in the game files");
    const TextFile tf = TextFile::Load(source.Read(table_path));
    const CsvDoc doc = CsvDoc::Parse(tf.latin1 ? Latin1ToUtf8(tf.text) : tf.text);
    const int name_col = doc.Column("Track Name"), rpk_col = doc.Column("RPKName");
    if (name_col < 0 || rpk_col < 0) throw Error("the track table does not have the expected columns");
    if (argv[0] == "list") {
        const int title_col = doc.Column("PRODUCT_NAME_1");
        for (size_t r = 1; r < doc.rows.size(); ++r) std::printf("%-38s %s%s\n", doc.Get(r, size_t(name_col)).c_str(), title_col >= 0 ? doc.Get(r, size_t(title_col)).c_str() : "", "");
        std::printf("%zu tracks\n", doc.rows.size() - 1);
        return 0;
    }
    size_t row = 0;
    for (size_t r = 1; r < doc.rows.size(); ++r) {
        if (doc.Get(r, size_t(name_col)) == args.positional[1]) row = r;
    }
    if (!row) throw Error("no track has the Track Name \"" + args.positional[1] + "\" in newdata/tracks.csv");
    if (argv[0] == "clone") return CloneTrack(args, source, *archive, doc, row, name_col, rpk_col, table_path);
    const std::string asset = doc.Get(row, size_t(rpk_col));
    if (asset.empty()) throw Error("that track has no RPKName");
    std::vector<std::string> files;
    for (const IndexEntry& e : archive->Entries()) {
        if (Named(e.name, asset) && (StartsWith(e.name, "worlds/") || StartsWith(e.name, "data/leveldata/"))) files.push_back(e.name);
    }
    std::sort(files.begin(), files.end());
    for (size_t c = 0; c < doc.rows[0].fields.size(); ++c) {
        if (!doc.Get(row, c).empty()) std::printf("%-26s %s\n", doc.Get(0, c).c_str(), doc.Get(row, c).c_str());
    }
    std::printf("\n%zu files are named after \"%s\":\n", files.size(), asset.c_str());
    for (const std::string& f : files) std::printf("  %s\n", f.c_str());
    return 0;
}

}  // namespace dcmod
