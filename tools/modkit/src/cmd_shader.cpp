// SPDX-License-Identifier: GPL-2.0-or-later
// dcmod shader ...: helpers for replacing the game's shaders. The runtime translates the console's shaders to SPIR-V
// when it first needs them; a SPIR-V file named like the translated one in a mod's shaders folder replaces it.
// Editing is done on the disassembly (spirv-dis and spirv-as from the Vulkan SDK or SPIRV-Tools, found on PATH).
#include "cli.hpp"
#include "json.hpp"

#include <algorithm>
#include <cstdlib>

namespace dcmod {

namespace {

int Usage() {
    std::puts(
        "dcmod shader: replace the game's shaders\n"
        "\n"
        "  dcmod shader list DUMPDIR [STAGE]        the dumped shaders (stage fs, vs, cs, gs, es), their hashes and sizes\n"
        "  dcmod shader dis FILE.spv [-o OUT.spvasm]  disassemble (needs spirv-dis)\n"
        "  dcmod shader as FILE.spvasm -o OUT.spv   assemble for the game's Vulkan 1.3 / SPIR-V 1.6 (needs spirv-as)\n"
        "  dcmod shader install FILE.spv MODFOLDER  put the file in MODFOLDER/files/shaders (the name must stay as dumped,\n"
        "                                           for example fs_0x0000000063b26ca4_0.spv) and make a mod.json if there is none\n"
        "\n"
        "To get the shaders: run the game once with the environment variable DC_DUMP_SHADERS=1 after deleting the cache folder\n"
        "(data\\user\\cache); the SPIR-V files are written to data\\user\\shader\\dumps. See docs/modding/shaders.md.");
    return 2;
}

std::string Quote(const std::string& s) { return "\"" + s + "\""; }

int Run(const std::string& tool, const std::string& rest) {
    // cmd.exe needs the whole command line in one more pair of quotes
    const std::string line = "\"" + Quote(tool) + " " + rest + "\"";
    const int r = std::system(line.c_str());
    if (r != 0) throw Error(tool + " failed or was not found on PATH (it comes with the Vulkan SDK and with SPIRV-Tools)");
    return r;
}

}  // namespace

int RunShader(const std::vector<std::string>& argv) {
    if (argv.empty()) return Usage();
    const std::string cmd = argv[0];
    const Args args = ParseArgs(argv, {"-o", "--output"});
    if (args.positional.size() < 2) return Usage();
    const std::string out = args.Get("-o", args.Get("--output"));
    if (cmd == "list") {
        const std::string want = args.positional.size() > 2 ? args.positional[2] : "";
        std::vector<fs::directory_entry> files;
        for (const auto& e : fs::directory_iterator(args.positional[1])) {
            if (e.path().extension() == ".spv" && (want.empty() || e.path().filename().string().compare(0, want.size() + 1, want + "_") == 0)) files.push_back(e);
        }
        std::sort(files.begin(), files.end(), [](const auto& a, const auto& b) { return a.path() < b.path(); });
        for (const auto& e : files) std::printf("%-34s %8llu bytes\n", e.path().filename().string().c_str(), (unsigned long long)e.file_size());
        std::printf("%zu shaders\n", files.size());
        return 0;
    }
    if (cmd == "dis") {
        const std::string target = out.empty() ? fs::path(args.positional[1]).replace_extension(".spvasm").string() : out;
        Run("spirv-dis", Quote(args.positional[1]) + " -o " + Quote(target));
        std::printf("wrote %s\n", target.c_str());
        return 0;
    }
    if (cmd == "as") {
        if (out.empty()) throw Error("as needs -o OUT.spv");
        Run("spirv-as", "--target-env spv1.6 " + Quote(args.positional[1]) + " -o " + Quote(out));
        std::printf("wrote %s\n", out.c_str());
        return 0;
    }
    if (cmd == "install") {
        if (args.positional.size() < 3) return Usage();
        const fs::path src = args.positional[1], mod = args.positional[2];
        const Bytes data = ReadFile(src.string());
        if (data.size() < 20 || Rd32(data.data()) != 0x07230203) throw Error(src.string() + " is not a SPIR-V file");
        WriteFile((mod / "files" / "shaders" / src.filename()).string(), data);
        if (!fs::exists(mod / "mod.json")) {
            Json m = Json::Object();
            m.Set("id", Lower(mod.filename().string().empty() ? mod.parent_path().filename().string() : mod.filename().string()));
            m.Set("name", "Shader mod");
            m.Set("version", "1.0");
            m.Set("category", "Graphics");
            WriteText(mod / "mod.json", m.Dump());
        }
        std::printf("installed %s into %s\n", src.filename().string().c_str(), (mod / "files" / "shaders").string().c_str());
        return 0;
    }
    return Usage();
}

}  // namespace dcmod
