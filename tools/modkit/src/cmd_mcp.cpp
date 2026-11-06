// SPDX-License-Identifier: GPL-2.0-or-later
// dcmod mcp: a Model Context Protocol server (JSON-RPC over standard input and output) that lets an AI agent work with
// the game's files and with mods through the same commands a person uses. It runs inside the dcmod program: a tool call
// runs the matching command and returns what the command printed. Mods are kept in one workspace folder, and the agent
// can write only there.
#include "cli.hpp"
#include "json.hpp"
#include "mod.hpp"

#include <iostream>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#define DUP _dup
#define DUP2 _dup2
#define FILENO _fileno
#define CLOSEFD _close
#else
#include <unistd.h>
#define DUP dup
#define DUP2 dup2
#define FILENO fileno
#define CLOSEFD close
#endif

namespace dcmod {

namespace {

// Runs `fn` with the standard output (and error) collected into a string.
std::string Capture(const std::function<int()>& fn, int* code) {
    std::fflush(stdout);
    std::fflush(stderr);
    FILE* tmp = std::tmpfile();
    if (!tmp) throw Error("cannot create a temporary file");
    const int out_saved = DUP(FILENO(stdout)), err_saved = DUP(FILENO(stderr));
    DUP2(FILENO(tmp), FILENO(stdout));
    DUP2(FILENO(tmp), FILENO(stderr));
    std::string failure;
    int rc = 1;
    try {
        rc = fn();
    } catch (const std::exception& e) {
        failure = std::string("dcmod: ") + e.what() + "\n";
    }
    std::fflush(stdout);
    std::fflush(stderr);
    DUP2(out_saved, FILENO(stdout));
    DUP2(err_saved, FILENO(stderr));
    CLOSEFD(out_saved);
    CLOSEFD(err_saved);
    std::string text;
    std::rewind(tmp);
    char buf[8192];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, tmp)) > 0) text.append(buf, n);
    std::fclose(tmp);
    text += failure;
    if (code) *code = failure.empty() ? rc : 1;
    return text;
}

Json Schema(std::initializer_list<std::pair<const char*, const char*>> props, std::initializer_list<const char*> required) {
    Json s = Json::Object();
    s.Set("type", "object");
    Json p = Json::Object();
    for (const auto& [name, kind] : props) {
        Json one = Json::Object();
        if (std::string(kind) == "strings") {
            one.Set("type", "array");
            Json items = Json::Object();
            items.Set("type", "string");
            one.Set("items", items);
        } else {
            one.Set("type", kind);
        }
        p.Set(name, one);
    }
    s.Set("properties", p);
    Json r = Json::Array();
    for (const char* q : required) r.Push(q);
    s.Set("required", r);
    return s;
}

Json Tool(const char* name, const char* description, Json schema) {
    Json t = Json::Object();
    t.Set("name", name);
    t.Set("description", description);
    t.Set("inputSchema", std::move(schema));
    return t;
}

class Server {
public:
    explicit Server(const Args& args, fs::path workspace) : args_(args), workspace_(std::move(workspace)) { fs::create_directories(workspace_); }

    void Run() {
        std::string line;
        while (std::getline(std::cin, line)) {
            if (line.empty() || line == "\r") continue;
            Json request;
            try {
                request = Json::Parse(line);
            } catch (const Error&) {
                continue;
            }
            const Json* id = request.Find("id");
            const std::string method = request.GetString("method");
            if (!id) continue;  // a notification: no answer
            Json reply = Json::Object();
            reply.Set("jsonrpc", "2.0");
            reply.Set("id", *id);
            try {
                reply.Set("result", Handle(method, request));
            } catch (const std::exception& e) {
                Json err = Json::Object();
                err.Set("code", -32603);
                err.Set("message", e.what());
                reply.Set("error", err);
            }
            std::cout << reply.Dump(0) << std::flush;
        }
    }

private:
    Json Handle(const std::string& method, const Json& request) {
        if (method == "initialize") {
            Json r = Json::Object();
            r.Set("protocolVersion", "2024-11-05");
            Json caps = Json::Object();
            caps.Set("tools", Json::Object());
            r.Set("capabilities", caps);
            Json info = Json::Object();
            info.Set("name", "dcmod");
            info.Set("version", "1.0");
            r.Set("serverInfo", info);
            r.Set("instructions",
                  "Tools for modding DriveClub. Start with `game` (archive, rpk, lua, tex, audio, mesh, dash, car, mod commands). Mods live in the workspace: "
                  "use `mod_list`, `mod_read`, `mod_write`, `mod_add_patch`, `mod_check` and `mod_build`. Patches change the game's files without copying them; "
                  "see the patch operations in docs/modding/patches.md.");
            return r;
        }
        if (method == "ping") return Json::Object();
        if (method == "tools/list") {
            Json r = Json::Object();
            r.Set("tools", Tools());
            return r;
        }
        if (method == "tools/call") {
            const Json* params = request.Find("params");
            if (!params) throw Error("missing params");
            return Call(params->GetString("name"), params->Find("arguments") ? *params->Find("arguments") : Json::Object());
        }
        throw Error("unknown method " + method);
    }

    static Json Tools() {
        Json t = Json::Array();
        t.Push(Tool("game",
                    "Run a dcmod command on the game's data. `command` is archive, rpk, lua, tex, audio, mesh, dash, car or mod; `args` are its arguments as one string each, "
                    "for example command=archive args=[list,-l,newdata/*], command=archive args=[cat,newdata/vehicles.csv], command=rpk args=[list,@global.rpk,type:xml,-l], "
                    "command=lua args=[dis,@scripts/lua/libraries/rewards.lua.bin], command=car args=[files,fe_458]. Run with no args for each command's help. "
                    "Output files (-o) can only go in the workspace.",
                    Schema({{"command", "string"}, {"args", "strings"}}, {"command"})));
        t.Push(Tool("mod_list", "List the mods in the workspace with their id, name and number of patches.", Schema({}, {})));
        t.Push(Tool("mod_read", "Read a mod's mod.json.", Schema({{"mod", "string"}}, {"mod"})));
        t.Push(Tool("mod_write", "Write a mod's mod.json (text must be valid JSON). Creates the mod folder if needed.", Schema({{"mod", "string"}, {"text", "string"}}, {"mod", "text"})));
        t.Push(Tool("mod_add_patch", "Append one patch (a JSON object such as {\"op\":\"csv.set\",...}) to a mod's patches, then check that the mod still applies to the game's files.",
                    Schema({{"mod", "string"}, {"patch", "object"}}, {"mod", "patch"})));
        t.Push(Tool("mod_write_file", "Write a file inside a mod folder (for example files/scripts/mods/x/main.lua, or a file a patch refers to with \"from\").",
                    Schema({{"mod", "string"}, {"path", "string"}, {"text", "string"}}, {"mod", "path", "text"})));
        t.Push(Tool("mod_check", "Apply the named mods (in order) to the game's files without writing anything; reports problems, conflicts and the files that would change.",
                    Schema({{"mods", "strings"}}, {"mods"})));
        t.Push(Tool("mod_build", "Apply the named mods and write the changed files to the workspace's _built folder.", Schema({{"mods", "strings"}}, {"mods"})));
        return t;
    }

    fs::path ModDir(const std::string& name) const {
        if (name.empty() || name.find_first_of("/\\:") != std::string::npos || name == "." || name == ".." || name[0] == '_') throw Error("bad mod name");
        return workspace_ / name;
    }

    bool InWorkspace(const fs::path& p) const {
        const fs::path root = fs::weakly_canonical(workspace_), q = fs::weakly_canonical(p);
        const fs::path r = q.lexically_relative(root);
        return !r.empty() && *r.begin() != "..";
    }

    static Json TextResult(const std::string& text, bool error) {
        Json item = Json::Object();
        item.Set("type", "text");
        item.Set("text", text.size() > 60000 ? text.substr(0, 60000) + "\n... (cut at 60000 characters; narrow the request)" : text);
        Json content = Json::Array();
        content.Push(item);
        Json r = Json::Object();
        r.Set("content", content);
        r.Set("isError", error);
        return r;
    }

    Json RunCommand(const std::string& command, std::vector<std::string> a) {
        for (size_t i = 0; i < a.size(); ++i) {
            if ((a[i] == "-o" || a[i] == "--output") && i + 1 < a.size() && !InWorkspace(a[i + 1])) throw Error("output files can only be written inside the workspace " + workspace_.string());
            if (a[i].rfind("--output=", 0) == 0 && !InWorkspace(a[i].substr(9))) throw Error("output files can only be written inside the workspace");
        }
        int rc = 0;
        const std::string text = Capture([&] {
            if (command == "archive") return RunArchive(a);
            if (command == "rpk") return RunRpk(a);
            if (command == "lua") return RunLua(a);
            if (command == "tex") return RunTex(a);
            if (command == "audio") return RunAudio(a);
            if (command == "mesh") return RunMesh(a);
            if (command == "dash") return RunDash(a);
            if (command == "car") return RunCar(a);
            if (command == "mod") return RunMod(a);
            throw Error("unknown command " + command + " (use archive, rpk, lua, tex, audio, mesh, dash, car or mod)");
        }, &rc);
        return TextResult(text.empty() ? "(no output)" : text, rc != 0 && rc != 2);
    }

    Json Call(const std::string& name, const Json& a) {
        try {
            if (name == "game") {
                std::vector<std::string> args;
                if (const Json* list = a.Find("args")) {
                    for (const Json& s : list->Items()) args.push_back(s.AsString());
                }
                const std::string command = a.GetString("command");
                // the command line wants the game folders as options; they come from the server's own options
                for (const char* opt : {"--update", "--base", "--game"}) {
                    for (const std::string& v : args_.All(opt)) {
                        args.push_back(opt);
                        args.push_back(v);
                    }
                }
                return RunCommand(command, args);
            }
            if (name == "mod_list") {
                std::string out;
                for (const auto& it : fs::directory_iterator(workspace_)) {
                    if (!it.is_directory() || it.path().filename().string()[0] == '_') continue;
                    try {
                        const Mod m = LoadMod(it.path());
                        out += it.path().filename().string() + "  id=" + m.id + "  name=" + m.name + "  patches=" + std::to_string(m.patches.Items().size()) + "\n";
                    } catch (const Error& e) {
                        out += it.path().filename().string() + "  ERROR " + e.what() + "\n";
                    }
                }
                return TextResult(out.empty() ? "(no mods yet)" : out, false);
            }
            if (name == "mod_read") {
                const fs::path f = ModDir(a.GetString("mod")) / "mod.json";
                return TextResult(fs::exists(f) ? ReadText(f) : "(no mod.json yet)", false);
            }
            if (name == "mod_write") {
                Json::Parse(a.GetString("text"));
                WriteText(ModDir(a.GetString("mod")) / "mod.json", a.GetString("text"));
                return TextResult("written", false);
            }
            if (name == "mod_write_file") {
                const fs::path dir = ModDir(a.GetString("mod"));
                const fs::path f = dir / fs::path(a.GetString("path"));
                if (!InWorkspace(f) || fs::path(a.GetString("path")).is_absolute() || !InWorkspace(dir) ) throw Error("the path must stay inside the mod folder");
                const fs::path rel = fs::weakly_canonical(f).lexically_relative(fs::weakly_canonical(dir));
                if (rel.empty() || *rel.begin() == "..") throw Error("the path must stay inside the mod folder");
                WriteText(f, a.GetString("text"));
                return TextResult("written " + f.string(), false);
            }
            if (name == "mod_add_patch") {
                const fs::path dir = ModDir(a.GetString("mod"));
                const Json* patch = a.Find("patch");
                if (!patch || !patch->IsObject()) throw Error("patch must be an object");
                Json manifest = fs::exists(dir / "mod.json") ? Json::ParseFile(dir / "mod.json") : Json::Object();
                if (!manifest.Has("id")) manifest.Set("id", Lower(dir.filename().string()));
                Json patches = Json::Array();
                if (const Json* old = manifest.Find("patches")) patches = *old;
                patches.Push(*patch);
                manifest.Set("patches", patches);
                const std::string before = fs::exists(dir / "mod.json") ? ReadText(dir / "mod.json") : std::string();
                WriteText(dir / "mod.json", manifest.Dump());
                int rc = 0;
                const std::string check = Capture([&] {
                    std::vector<std::string> v{"check", dir.string()};
                    for (const char* opt : {"--update", "--base", "--game"}) {
                        for (const std::string& s : args_.All(opt)) {
                            v.push_back(opt);
                            v.push_back(s);
                        }
                    }
                    return RunMod(v);
                }, &rc);
                if (rc != 0) {
                    if (before.empty()) fs::remove(dir / "mod.json");
                    else WriteText(dir / "mod.json", before);
                    return TextResult("the patch was NOT added because the mod no longer applies:\n" + check, true);
                }
                return TextResult("patch added.\n" + check, false);
            }
            if (name == "mod_check" || name == "mod_build") {
                std::vector<std::string> v{name == "mod_check" ? "check" : "build"};
                for (const Json& m : a.Find("mods")->Items()) v.push_back(ModDir(m.AsString()).string());
                if (name == "mod_build") {
                    v.push_back("-o");
                    v.push_back((workspace_ / "_built").string());
                }
                for (const char* opt : {"--update", "--base", "--game"}) {
                    for (const std::string& s : args_.All(opt)) {
                        v.push_back(opt);
                        v.push_back(s);
                    }
                }
                return RunCommand("mod", v);
            }
            throw Error("unknown tool " + name);
        } catch (const std::exception& e) {
            return TextResult(std::string("error: ") + e.what(), true);
        }
    }

    const Args& args_;
    fs::path workspace_;
};

}  // namespace

int RunMcp(const std::vector<std::string>& argv) {
    const Args args = ParseArgs(argv, {"--update", "--base", "--game", "--mods"});
    Server server(args, args.Get("--mods", "dcmod-mods"));
    server.Run();
    return 0;
}

}  // namespace dcmod
