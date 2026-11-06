// SPDX-License-Identifier: GPL-2.0-or-later
// dcmod: tools for making DriveClub mods.
#include "cli.hpp"

#include <iostream>

namespace {

int Usage() {
    std::puts(
        "dcmod, the DriveClub mod kit\n"
        "\n"
        "  dcmod archive ...   look into the game's own archive (list, extract, verify)\n"
        "  dcmod rpk ...       resource packs: list, extract, build, replace, verify\n"
        "  dcmod lua ...       the game's compiled Lua scripts: disassemble, survey the names they use\n"
        "  dcmod studio        a web page to browse the game files, edit them and make mods from the edits\n"
        "  dcmod dash ...      in-car dashboards: list, show, check\n"
        "  dcmod tex ...       textures: extract to DDS, import from DDS\n"
        "  dcmod mod ...     mods: new, check, build (patches applied to the game files)\n"
        "\n"
        "Run a command without arguments for its help.");
    return 2;
}

}  // namespace

int main(int argc, char** argv) {
    std::vector<std::string> args(argv + 1, argv + argc);
    try {
        if (args.empty()) return Usage();
        const std::string cmd = args[0];
        const std::vector<std::string> rest(args.begin() + 1, args.end());
        if (cmd == "archive") return dcmod::RunArchive(rest);
        if (cmd == "rpk") return dcmod::RunRpk(rest);
        if (cmd == "lua") return dcmod::RunLua(rest);
        if (cmd == "mod") return dcmod::RunMod(rest);
        if (cmd == "tex") return dcmod::RunTex(rest);
        if (cmd == "studio") return dcmod::RunStudio(rest);
        if (cmd == "dash") return dcmod::RunDash(rest);
        std::fprintf(stderr, "dcmod: unknown command \"%s\"\n", cmd.c_str());
        return Usage();
    } catch (const std::exception& e) {
        std::fprintf(stderr, "dcmod: %s\n", e.what());
        return 1;
    }
}
