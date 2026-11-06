// SPDX-License-Identifier: GPL-2.0-or-later
// dcmod car ...: new cars made from existing ones. A car is a row of newdata/vehicles.csv plus about thirty files named
// after its Asset (the model pack, its top mips, cameras, wipers, state machines). A clone is a mod that copies all
// of them under a new name with `file.copy` patches and adds the row, so it holds none of the game's data.
#include "cli.hpp"
#include "csv.hpp"
#include "mod.hpp"

#include <algorithm>

namespace dcmod {

namespace {

int Usage() {
    std::puts(
        "dcmod car: new cars from existing ones\n"
        "\n"
        "  dcmod car list                  every car: Vehicle Name, Asset (the file name stem) and class\n"
        "  dcmod car files NAME            the table row of a car (NAME is its Vehicle Name) and every file named after it\n"
        "  dcmod car swap DONOR VICTIM -o MODFOLDER\n"
        "                                  a mod that gives the car VICTIM the model and textures of the car DONOR\n"
        "  dcmod car variant NAME NEWNAME -o MODFOLDER\n"
        "                                  a mod that adds NEWNAME as a car that shares the model, sounds and cameras of\n"
        "                                  NAME and has its own copy of the vehicle definition (physics) to change\n"
        "  dcmod car clone NAME NEWNAME -o MODFOLDER [--asset ASSET]\n"
        "                                  a mod that adds a copy of the car as NEWNAME (Vehicle Name) with the files\n"
        "                                  copied under the new Asset (default: NEWNAME), so the copy has its own model files\n"
        "                                  that can be changed (dcmod mesh, dcmod tex), see docs/modding/cars.md\n"
        "\n"
        "The game folders come from --update DIR --base DIR, DRIVECLUB_UPDATE_DIR and DRIVECLUB_BASE_DIR, or launcher.ini.");
    return 2;
}

// The path with `asset` replaced by `fresh` in each segment that starts with it (followed by nothing, '.', '_' or '/').
// Returns an empty string when no segment does.
std::string Rename(const std::string& path, const std::string& asset, const std::string& fresh) {
    std::string out;
    bool hit = false;
    size_t pos = 0;
    while (pos <= path.size()) {
        size_t slash = path.find('/', pos);
        if (slash == std::string::npos) slash = path.size();
        std::string seg = path.substr(pos, slash - pos);
        if (Lower(seg).compare(0, asset.size(), Lower(asset)) == 0) {
            const char next = seg.size() > asset.size() ? seg[asset.size()] : '\0';
            if (next == '\0' || next == '.' || next == '_') {
                seg = fresh + seg.substr(asset.size());
                hit = true;
            }
        }
        out += seg;
        if (slash < path.size()) out.push_back('/');
        pos = slash + 1;
    }
    return hit ? out : std::string();
}

}  // namespace

int RunCar(const std::vector<std::string>& argv) {
    if (argv.empty()) return Usage();
    const std::string cmd = argv[0];
    const Args args = ParseArgs(argv, {"--update", "--base", "--game", "-o", "--output", "--asset"});
    if (args.positional.size() < 2 && cmd != "list") return Usage();
    auto archive = OpenGame(args);
    GameSource source(*archive, GameFolders(args));
    const std::string table_path = source.Canonical("newdata/vehicles.csv");
    if (table_path.empty()) throw Error("newdata/vehicles.csv is not in the game's files");
    const Bytes table_bytes = source.Read(table_path);
    const TextFile tf = TextFile::Load(table_bytes);
    const CsvDoc doc = CsvDoc::Parse(tf.latin1 ? Latin1ToUtf8(tf.text) : tf.text);
    const int name_col = doc.Column("Vehicle Name"), asset_col = doc.Column("Asset"), id_col = doc.Column("UniqueID");
    if (name_col < 0 || asset_col < 0 || id_col < 0) throw Error("the vehicle table does not have the expected columns");
    if (cmd == "list") {
        const int class_col = doc.Column("Class");
        for (size_t r = 1; r < doc.rows.size(); ++r) {
            std::printf("%-30s %-36s %s%s\n", doc.Get(r, size_t(name_col)).c_str(), doc.Get(r, size_t(asset_col)).c_str(), class_col >= 0 ? "class " : "", class_col >= 0 ? doc.Get(r, size_t(class_col)).c_str() : "");
        }
        std::printf("%zu cars\n", doc.rows.size() - 1);
        return 0;
    }
    const std::string& name = args.positional[1];
    size_t row = 0;
    for (size_t r = 1; r < doc.rows.size(); ++r) {
        if (doc.Get(r, size_t(name_col)) == name) row = r;
    }
    if (!row) throw Error("no car has the Vehicle Name \"" + name + "\" in newdata/vehicles.csv");
    const std::string asset = doc.Get(row, size_t(asset_col));
    if (asset.empty()) throw Error("that car has no Asset");

    std::vector<std::string> files;
    for (const IndexEntry& e : archive->Entries()) {
        if (!Rename(e.name, asset, "x").empty()) files.push_back(e.name);
    }
    std::sort(files.begin(), files.end());

    if (cmd == "files") {
        for (size_t c = 0; c < doc.rows[0].fields.size(); ++c) {
            if (!doc.Get(row, c).empty()) std::printf("%-26s %s\n", doc.Get(0, c).c_str(), doc.Get(row, c).c_str());
        }
        std::printf("\n%zu files are named after the asset \"%s\":\n", files.size(), asset.c_str());
        for (const std::string& f : files) std::printf("  %s\n", f.c_str());
        return 0;
    }
    if (cmd == "swap") {
        // dcmod car swap DONOR VICTIM -o MODFOLDER: the victim car gets the donor's model and textures.
        const std::string out = args.Get("-o", args.Get("--output"));
        if (out.empty() || args.positional.size() != 3) throw Error("usage: dcmod car swap DONOR VICTIM -o MODFOLDER");
        size_t victim_row = 0;
        for (size_t r = 1; r < doc.rows.size(); ++r) {
            if (doc.Get(r, size_t(name_col)) == args.positional[2]) victim_row = r;
        }
        if (!victim_row) throw Error("no car has the Vehicle Name \"" + args.positional[2] + "\"");
        const std::string victim = doc.Get(victim_row, size_t(asset_col));
        Json patches = Json::Array();
        const auto add = [&](const char* op, const std::vector<std::pair<const char*, std::string>>& kv) {
            Json j = Json::Object();
            j.Set("op", op);
            for (const auto& [k, v] : kv) j.Set(k, v);
            patches.Push(j);
        };
        for (const char* suffix : {".rpk", "_top_mips.rpk"}) {
            const std::string from = "vehicles/" + asset + suffix;
            if (source.Canonical(from).empty()) continue;
            add("file.copy", {{"from", from}, {"file", "vehicles/" + victim + suffix}});
        }
        for (const char* scope : {"required", "names"}) {
            add("rpk.rename", {{"file", "vehicles/" + victim + ".rpk"}, {"find", asset}, {"replace", victim}, {"scope", scope}});
        }
        Json manifest = Json::Object();
        manifest.Set("id", Lower(victim) + "-as-" + Lower(asset));
        manifest.Set("name", name + " model on " + args.positional[2]);
        manifest.Set("version", "1.0");
        manifest.Set("patches", patches);
        WriteText(fs::path(out) / "mod.json", manifest.Dump());
        std::printf("%s will show the model of %s; mod written to %s\n", args.positional[2].c_str(), name.c_str(), out.c_str());
        return 0;
    }
    if (cmd == "clone" || cmd == "variant") {
        const bool variant = cmd == "variant";
        const std::string out = args.Get("-o", args.Get("--output"));
        if (out.empty() || args.positional.size() != 3) throw Error("usage: dcmod car clone NAME NEWNAME -o MODFOLDER [--asset ASSET]");
        const std::string fresh_name = args.positional[2];
        const std::string fresh_asset = variant ? asset : args.Get("--asset", fresh_name);
        for (size_t r = 1; r < doc.rows.size(); ++r) {
            if (doc.Get(r, size_t(name_col)) == fresh_name) throw Error("a car named \"" + fresh_name + "\" already exists");
            if (!variant && doc.Get(r, size_t(asset_col)) == fresh_asset) throw Error("a car with the asset \"" + fresh_asset + "\" already exists");
        }
        long next_id = 0;
        for (size_t r = 1; r < doc.rows.size(); ++r) {
            const std::string& v = doc.Get(r, size_t(id_col));
            if (!v.empty() && v.find_first_not_of("0123456789") == std::string::npos) next_id = std::max(next_id, std::stol(v));
        }
        Json patches = Json::Array();
        const int def_col = doc.Column("Vehicle Def");
        const std::string old_def = def_col >= 0 ? doc.Get(row, size_t(def_col)) : std::string();
        const std::string fresh_def = variant ? fresh_name : fresh_asset;
        if (!old_def.empty() && source.Canonical("vehicledefs.vdef").size()) {
            Json def = Json::Object();
            def.Set("op", "vdef.clone");
            def.Set("file", "vehicledefs.vdef");
            def.Set("from", old_def);
            def.Set("name", fresh_def);
            patches.Push(def);
        }
        Json set = Json::Object();
        if (!old_def.empty() && source.Canonical("vehicledefs.vdef").size()) set.Set("Vehicle Def", fresh_def);
        set.Set("Vehicle Name", fresh_name);
        set.Set("UniqueID", std::to_string(next_id + 1));
        if (!variant) set.Set("Asset", fresh_asset);
        if (!variant && doc.Column("Animation") >= 0 && doc.Get(row, size_t(doc.Column("Animation"))) == asset) set.Set("Animation", fresh_asset);
        Json add = Json::Object();
        add.Set("op", "csv.add");
        add.Set("file", table_path);
        add.Set("key", "Vehicle Name");
        add.Set("like", name);
        add.Set("after", name);
        add.Set("row", set);
        patches.Push(add);
        for (const std::string& f : variant ? std::vector<std::string>() : files) {
            Json op = Json::Object();
            op.Set("op", "file.copy");
            op.Set("from", f);
            op.Set("file", Rename(f, asset, fresh_asset));
            patches.Push(op);
            // The car's own pack lists the packs it requires by asset name.
            if (StartsWith(f, "vehicles/") && !EndsWith(f, "_top_mips.rpk") && EndsWith(f, ".rpk")) {
                Json ren = Json::Object();
                ren.Set("op", "rpk.rename");
                ren.Set("file", Rename(f, asset, fresh_asset));
                ren.Set("find", asset);
                ren.Set("replace", fresh_asset);
                ren.Set("scope", "required");
                patches.Push(ren);
            } else if (EndsWith(f, ".rpk")) {
                // The game finds what it loads from these packs by name (the state machine as stateMachine\vehicles\<asset>),
                // so the copies must carry the new name; without it the loading screen never ends.
                Json ren = Json::Object();
                ren.Set("op", "rpk.rename");
                ren.Set("file", Rename(f, asset, fresh_asset));
                ren.Set("find", asset);
                ren.Set("replace", fresh_asset);
                ren.Set("scope", "names");
                patches.Push(ren);
            }
        }
        if (!variant) {
            // The dashboard definition: the game builds <asset>.def.rpk from the resource named <asset>.def in global.rpk.
            Json dash = Json::Object();
            dash.Set("op", "rpk.copy");
            dash.Set("file", "global.rpk");
            dash.Set("resource", asset + ".def");
            dash.Set("name", fresh_asset + ".def");
            dash.Set("optional", true);
            patches.Push(dash);
            Json names = Json::Object();
            names.Set("op", "rpk.rename");
            names.Set("file", "vehicles/" + fresh_asset + ".rpk");
            names.Set("find", asset);
            names.Set("replace", fresh_asset);
            names.Set("scope", "names");
            patches.Push(names);
        }
        if (!variant && source.Canonical("scripts/filemanifest.cfg").size()) {
            Json listed = Json::Object();
            listed.Set("op", "manifest.add");
            listed.Set("file", "scripts/filemanifest.cfg");
            Json paths = Json::Array();
            for (const std::string& f : files) paths.Push(Rename(f, asset, fresh_asset));
            listed.Set("paths", paths);
            patches.Push(listed);
        }
        Json manifest = Json::Object();
        manifest.Set("id", Lower(fresh_name));
        manifest.Set("name", fresh_name + " (copy of " + name + ")");
        manifest.Set("version", "1.0");
        manifest.Set("patches", patches);
        WriteText(fs::path(out) / "mod.json", manifest.Dump());
        if (variant) std::printf("a new row and a copy of the vehicle definition; mod written to %s\n", out.c_str());
        else std::printf("%zu files will be copied; mod written to %s\n", files.size(), out.c_str());
        return 0;
    }
    return Usage();
}

}  // namespace dcmod
