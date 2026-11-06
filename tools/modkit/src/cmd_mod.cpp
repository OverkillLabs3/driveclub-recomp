// SPDX-License-Identifier: GPL-2.0-or-later
// dcmod mod ...: make, check and build mods.
#include "cli.hpp"
#include "diff.hpp"
#include "mod.hpp"

#include <algorithm>

namespace dcmod {

namespace {

uint32_t Crc32(const uint8_t* d, size_t n) {
    static uint32_t t[256];
    if (!t[1]) {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            t[i] = c;
        }
    }
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; ++i) c = t[(c ^ d[i]) & 255] ^ (c >> 8);
    return ~c;
}

// A ZIP of the folder, files stored without compression. Returns the number of files.
size_t WriteZip(const fs::path& dir, const fs::path& out) {
    Bytes zip, central;
    auto put = [](Bytes& b, uint64_t v, int n) { for (int i = 0; i < n; ++i) b.push_back(uint8_t(v >> (8 * i))); };
    size_t count = 0;
    std::vector<fs::path> files;
    for (const auto& e : fs::recursive_directory_iterator(dir)) if (e.is_regular_file()) files.push_back(e.path());
    std::sort(files.begin(), files.end());
    for (const fs::path& f : files) {
        const std::string name = fs::relative(f, dir).generic_string();
        const Bytes data = ReadFile(f);
        const uint32_t crc = Crc32(data.data(), data.size()), at = uint32_t(zip.size());
        for (Bytes* b : {&zip, &central}) {
            if (b == &central) { put(central, 0x02014b50, 4); put(central, 20, 2); }
            else put(zip, 0x04034b50, 4);
            put(*b, 20, 2); put(*b, 0x0800, 2); put(*b, 0, 2); put(*b, 0, 2); put(*b, 0x21, 2);
            put(*b, crc, 4); put(*b, data.size(), 4); put(*b, data.size(), 4); put(*b, name.size(), 2); put(*b, 0, 2);
            if (b == &central) { put(central, 0, 2); put(central, 0, 2); put(central, 0, 2); put(central, 0, 4); put(central, at, 4); }
            b->insert(b->end(), name.begin(), name.end());
        }
        zip.insert(zip.end(), data.begin(), data.end());
        ++count;
    }
    const uint32_t cd_at = uint32_t(zip.size());
    zip.insert(zip.end(), central.begin(), central.end());
    put(zip, 0x06054b50, 4); put(zip, 0, 2); put(zip, 0, 2); put(zip, count, 2); put(zip, count, 2);
    put(zip, central.size(), 4); put(zip, cd_at, 4); put(zip, 0, 2);
    WriteFile(out, zip);
    return count;
}

int Usage() {
    std::puts(
        "dcmod mod: mods as folders with a mod.json\n"
        "\n"
        "  dcmod mod new DIR [--id ID] [--name NAME]   start a mod with a manifest and an example patch\n"
        "  dcmod mod diff EDITED -o MODFOLDER [--id ID] [--name NAME]  make a mod from files you edited (game paths under EDITED)\n"
        "  dcmod mod check DIR... [--settings FILE]    read the mods and try their patches on the game's files\n"
        "  dcmod mod pack DIR -o FILE.zip              a ZIP of the mod folder to share\n"
        "  dcmod mod build DIR... -o OUT [--settings FILE]  apply the mods, in the order given, and write the changed files to OUT\n"
        "\n"
        "A mod folder may be written ID=FOLDER to give it that id when its mod.json has none.\n"
        "OUT is a folder in the shape of the game's files; used as a mods folder it makes the game use the result.\n"
        "The game folders come from --update DIR --base DIR, DRIVECLUB_UPDATE_DIR and DRIVECLUB_BASE_DIR, or launcher.ini.");
    return 2;
}

int Run(const Args& args, bool write) {
    std::vector<Mod> mods;
    for (size_t i = 1; i < args.positional.size(); ++i) {
        // ID=FOLDER gives a mod without an id in its manifest that id
        const std::string& a = args.positional[i];
        const size_t eq = a.find('=');
        const bool named = eq != std::string::npos && eq > 0 && a.find_first_of("/\\:", 0) > eq;
        mods.push_back(named ? LoadMod(a.substr(eq + 1), a.substr(0, eq)) : LoadMod(a));
    }
    if (mods.empty()) throw Error("no mod folders given");
    if (args.Has("--settings")) {
        // {"mod.id": {"setting": value}}: what the user chose for the settings that mods offer
        const Json all = Json::ParseFile(args.Get("--settings"));
        for (Mod& m : mods) {
            if (const Json* c = all.Find(m.id)) ApplySettings(m, *c);
        }
    }
    std::vector<std::string> problems;
    std::vector<Mod> ordered;
    for (size_t i : OrderMods(mods, problems)) ordered.push_back(mods[i]);
    for (const std::string& p : problems) std::printf("problem: %s\n", p.c_str());
    auto archive = OpenGame(args);
    GameSource source(*archive, GameFolders(args));
    PatchReport report;
    const std::map<std::string, Bytes> files = BuildMods(ordered, source, report);
    for (const std::string& w : report.warnings) std::printf("warning: %s\n", w.c_str());
    for (const std::string& c : report.conflicts) std::printf("conflict: %s\n", c.c_str());
    if (write) {
        const std::string out = args.Get("-o", args.Get("--output"));
        if (out.empty()) throw Error("build needs -o OUT");
        Json history = Json::Object();
        for (const auto& [path, bytes] : files) {
            WriteFile(fs::path(out) / path, bytes);
            Json list = Json::Array();
            for (const PatchReport::Entry& e : report.history[path]) {
                Json j = Json::Object();
                j.Set("mod", e.mod);
                j.Set("action", e.action);
                list.Push(j);
            }
            history.Set(path, list);
        }
        WriteText(fs::path(out) / "dcmod-report.json", history.Dump());
    }
    std::printf("%zu mods, %zu files %s%s\n", ordered.size(), files.size(), write ? "written" : "would change",
                problems.empty() && report.conflicts.empty() ? "" : " (see the problems above)");
    return problems.empty() ? 0 : 1;
}

}  // namespace

int RunMod(const std::vector<std::string>& argv) {
    if (argv.empty()) return Usage();
    const std::string cmd = argv[0];
    const Args args = ParseArgs(argv, {"--update", "--base", "--game", "-o", "--output", "--id", "--name", "--settings"});
    if (args.positional.size() < 2) return Usage();
    if (cmd == "new") {
        const fs::path dir = args.positional[1];
        const std::string id = args.Get("--id", Lower(dir.filename().string()));
        NewMod(dir, id, args.Get("--name", id));
        std::printf("created %s\n", (dir / "mod.json").string().c_str());
        return 0;
    }
    if (cmd == "diff") {
        const std::string out = args.Get("-o", args.Get("--output"));
        if (out.empty()) throw Error("diff needs -o MODFOLDER");
        const fs::path dir = args.positional[1];
        const std::string id = args.Get("--id", Lower(fs::path(out).filename().string()));
        auto archive = OpenGame(args);
        GameSource source(*archive, GameFolders(args));
        const DiffResult r = MakeModFromEdits(source, dir, out, id, args.Get("--name", id));
        for (const std::string& n : r.notes) std::printf("%s\n", n.c_str());
        std::printf("%zu patches, %zu files included, %zu files unchanged; mod written to %s\n", r.patches, r.loose_files, r.unchanged, out.c_str());
        for (const std::string& n : r.notes) {
            if (StartsWith(n, "CHECK FAILED")) return 1;
        }
        return 0;
    }
    if (cmd == "pack") {
        const std::string out = args.Get("-o", args.Get("--output"));
        if (out.empty()) throw Error("pack needs -o FILE.zip");
        const Mod m = LoadMod(args.positional[1]);
        const size_t n = WriteZip(args.positional[1], out);
        std::printf("packed %s (%zu files) into %s\n", m.id.c_str(), n, out.c_str());
        return 0;
    }
    if (cmd == "check") return Run(args, false);
    if (cmd == "build") return Run(args, true);
    return Usage();
}

}  // namespace dcmod
