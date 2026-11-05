// SPDX-License-Identifier: GPL-2.0-or-later
// dcmod mod ...: make, check and build mods.
#include "cli.hpp"
#include "diff.hpp"
#include "mod.hpp"

namespace dcmod {

namespace {

int Usage() {
    std::puts(
        "dcmod mod: mods as folders with a mod.json\n"
        "\n"
        "  dcmod mod new DIR [--id ID] [--name NAME]   start a mod with a manifest and an example patch\n"
        "  dcmod mod diff EDITED -o MODFOLDER [--id ID] [--name NAME]  make a mod from files you edited (game paths under EDITED)\n"
        "  dcmod mod check DIR...                      read the mods and try their patches on the game's files\n"
        "  dcmod mod build DIR... -o OUT               apply the mods, in the order given, and write the changed files to OUT\n"
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
    const Args args = ParseArgs(argv, {"--update", "--base", "--game", "-o", "--output", "--id", "--name"});
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
    if (cmd == "check") return Run(args, false);
    if (cmd == "build") return Run(args, true);
    return Usage();
}

}  // namespace dcmod
