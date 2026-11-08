// SPDX-License-Identifier: GPL-2.0-or-later
// dcmod lua ...: read the game's compiled Lua scripts.
#include "cli.hpp"
#include "lua.hpp"

#include <algorithm>
#include <functional>

namespace dcmod {

namespace {

int Usage() {
    std::puts(
        "dcmod lua: the game's compiled Lua 5.1 scripts (scripts/**/*.lua.bin)\n"
        "\n"
        "  dcmod lua info SCRIPT...                 size, functions and instructions of each script\n"
        "  dcmod lua dis SCRIPT [-o FILE] [--no-lines]  assembly listing of one script\n"
        "  dcmod lua survey SCRIPT... [-o FILE]     the globals, calls and table keys used by the scripts, as Markdown\n"
        "  dcmod lua strings SCRIPT...              every string constant\n"
        "  dcmod lua compile SOURCE.lua [-o OUT]    compile Lua source to the bytecode the game runs\n"
        "  dcmod lua verify SCRIPT...               read each script and write it again; the bytes must be the same\n"
        "\n"
        "SCRIPT is a file, a folder (searched for .lua.bin and .lua files) or @PATTERN for scripts inside the game's\n"
        "archive, for example \"@scripts/lua/*\". The game folders come from --update DIR --base DIR,\n"
        "DRIVECLUB_UPDATE_DIR and DRIVECLUB_BASE_DIR, or launcher.ini.\n"
        "Scripts that are plain Lua text can be edited directly. The compiled ones can be read with 'dis'; to get Lua\n"
        "source back from them use a decompiler such as unluac (not part of this kit).");
    return 2;
}

struct Script {
    std::string label;
    Bytes data;
};

void ForEachScript(const Args& args, const std::vector<std::string>& specs, const std::function<void(Script&)>& each) {
    std::unique_ptr<Archive> archive;
    size_t count = 0;
    for (const std::string& spec : specs) {
        if (!spec.empty() && spec[0] == '@') {
            if (!archive) archive = OpenGame(args);
            const std::string pattern = GamePath(spec.substr(1));
            for (const IndexEntry& e : archive->Entries()) {
                if (WildMatch(pattern, e.name)) {
                    Script s{"@" + e.name, archive->Read(e)};
                    ++count;
                    each(s);
                }
            }
        } else if (fs::is_directory(spec)) {
            std::vector<fs::path> files;
            for (const auto& it : fs::recursive_directory_iterator(spec)) {
                const std::string name = Lower(it.path().filename().string());
                if (it.is_regular_file() && (EndsWith(name, ".lua.bin") || EndsWith(name, ".lua"))) files.push_back(it.path());
            }
            std::sort(files.begin(), files.end());
            for (const fs::path& p : files) {
                Script s{p.generic_string(), ReadFile(p)};
                ++count;
                each(s);
            }
        } else {
            Script s{spec, ReadFile(spec)};
            ++count;
            each(s);
        }
    }
    if (count == 0) throw Error("no scripts found");
}

struct Tally {
    int count = 0;
    std::set<std::string> scripts;
};

void Add(std::map<std::string, Tally>& m, const std::map<std::string, int>& from, const std::string& script) {
    for (const auto& [name, n] : from) {
        m[name].count += n;
        m[name].scripts.insert(script);
    }
}

std::string Short(const std::string& label) {
    const size_t slash = label.find_last_of('/');
    std::string name = slash == std::string::npos ? label : label.substr(slash + 1);
    for (const char* ext : {".lua.bin", ".lua"}) {
        if (EndsWith(name, ext)) name.resize(name.size() - std::strlen(ext));
    }
    return name;
}

void Table(std::string& out, const char* title, const char* column, const std::map<std::string, Tally>& m, size_t min_count) {
    std::vector<std::pair<std::string, const Tally*>> rows;
    for (const auto& [name, t] : m) {
        if (size_t(t.count) >= min_count) rows.emplace_back(name, &t);
    }
    std::stable_sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) { return a.second->count > b.second->count; });
    out += std::string("\n## ") + title + "\n\n| " + column + " | uses | scripts |\n|---|---:|---|\n";
    for (const auto& [name, t] : rows) {
        std::string scripts;
        size_t shown = 0;
        for (const std::string& s : t->scripts) {
            if (shown++ == 4) {
                scripts += ", and " + std::to_string(t->scripts.size() - 4) + " more";
                break;
            }
            if (!scripts.empty()) scripts += ", ";
            scripts += s;
        }
        out += "| `" + name + "` | " + std::to_string(t->count) + " | " + scripts + " |\n";
    }
}

}  // namespace

int RunLua(const std::vector<std::string>& argv) {
    if (argv.empty()) return Usage();
    const std::string cmd = argv[0];
    const Args args = ParseArgs({argv.begin() + 1, argv.end()}, {"--update", "--base", "--game", "-o", "--output", "--min"});
    if (args.positional.empty()) return Usage();
    const std::string out_path = args.Get("-o", args.Get("--output"));

    if (cmd == "dis") {
        if (args.positional.size() != 1) throw Error("dis takes one script");
        std::string text;
        ForEachScript(args, args.positional, [&](Script& s) {
            if (!IsLuaBytecode(s.data)) throw Error(s.label + " is Lua text, not compiled code");
            text = "; " + s.label + "\n" + DisassembleLua(ParseLua(s.data), !args.Has("--no-lines"));
        });
        if (out_path.empty()) std::fputs(text.c_str(), stdout);
        else WriteText(out_path, text);
        return 0;
    }
    if (cmd == "info") {
        ForEachScript(args, args.positional, [&](Script& s) {
            if (!IsLuaBytecode(s.data)) {
                std::printf("%-60s %9zu bytes  Lua text\n", s.label.c_str(), s.data.size());
                return;
            }
            LuaUsage u;
            CollectLuaUsage(ParseLua(s.data), u);
            std::printf("%-60s %9zu bytes  %4zu functions %6zu instructions\n", s.label.c_str(), s.data.size(), u.functions, u.instructions);
        });
        return 0;
    }
    if (cmd == "compile") {
        // dcmod lua compile SOURCE.lua [-o OUT]: the bytecode the game can run (the game cannot compile text itself)
        if (args.positional.size() != 1) throw Error("compile takes one Lua source file");
        const std::string src = ReadText(args.positional[0]);
        const Bytes out = CompileLua(src, fs::path(args.positional[0]).filename().string());
        const std::string target = out_path.empty() ? args.positional[0] + ".bin" : out_path;
        WriteFile(target, out);
        std::printf("%zu bytes written to %s\n", out.size(), target.c_str());
        return 0;
    }
    if (cmd == "verify") {
        size_t checked = 0, bad = 0;
        ForEachScript(args, args.positional, [&](Script& s) {
            if (!IsLuaBytecode(s.data)) return;
            ++checked;
            try {
                if (WriteLua(ParseLua(s.data)) != s.data) throw Error("writing it again gives different bytes");
            } catch (const Error& e) {
                ++bad;
                std::printf("FAIL %s: %s\n", s.label.c_str(), e.what());
            }
        });
        std::printf("%zu scripts checked, %zu failed\n", checked, bad);
        return bad ? 1 : 0;
    }
    if (cmd == "strings") {
        ForEachScript(args, args.positional, [&](Script& s) {
            if (!IsLuaBytecode(s.data)) return;
            LuaUsage u;
            CollectLuaUsage(ParseLua(s.data), u);
            for (const std::string& str : u.strings) std::printf("%s\t%s\n", Short(s.label).c_str(), str.c_str());
        });
        return 0;
    }
    if (cmd == "survey") {
        std::map<std::string, Tally> reads, writes, calls, fields;
        size_t scripts = 0, functions = 0, instructions = 0, text_scripts = 0;
        ForEachScript(args, args.positional, [&](Script& s) {
            if (!IsLuaBytecode(s.data)) {
                ++text_scripts;
                return;
            }
            LuaUsage u;
            CollectLuaUsage(ParseLua(s.data), u);
            const std::string name = Short(s.label);
            ++scripts;
            functions += u.functions;
            instructions += u.instructions;
            Add(reads, u.globals_read, name);
            Add(writes, u.globals_written, name);
            Add(calls, u.calls, name);
            Add(fields, u.fields, name);
        });
        const size_t min_count = args.Has("--min") ? size_t(std::stoul(args.Get("--min"))) : 1;
        std::string out = "# Names used by the game's Lua scripts\n\n";
        out += "Read from " + std::to_string(scripts) + " compiled scripts (" + std::to_string(functions) + " functions, " +
               std::to_string(instructions) + " instructions)";
        if (text_scripts) out += "; " + std::to_string(text_scripts) + " plain text scripts were skipped";
        out += ".\nA call is listed under the name the code used to reach it; where the code gets a function from a local variable or\n"
               "a parameter, the owner shows as `?`. Counts are the number of places in the code, not how often they run.\n";
        Table(out, "Global variables read", "name", reads, min_count);
        Table(out, "Global variables written", "name", writes, min_count);
        Table(out, "Calls", "call", calls, min_count);
        Table(out, "Table keys and methods", "key", fields, min_count);
        if (out_path.empty()) std::fputs(out.c_str(), stdout);
        else {
            WriteText(out_path, out);
            std::printf("written to %s\n", out_path.c_str());
        }
        return 0;
    }
    return Usage();
}

}  // namespace dcmod
