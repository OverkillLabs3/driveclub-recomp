// SPDX-License-Identifier: GPL-2.0-or-later
// dcmod studio: a local web page for browsing the game's files, editing them and turning the edits into a mod.
// The server only listens on 127.0.0.1 and every request needs the random token that is in the address it opens.
#include "cli.hpp"
#include "diff.hpp"
#include "lua.hpp"
#include "mod.hpp"
#include "studio_ui.hpp"
#include "textfile.hpp"

#include <algorithm>
#include <random>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
using Sock = SOCKET;
#define CLOSE_SOCKET closesocket
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
using Sock = int;
#define CLOSE_SOCKET close
constexpr Sock INVALID_SOCKET = -1;
#endif

namespace dcmod {

namespace {

struct Request {
    std::string method, path, query, body;
    std::map<std::string, std::string> headers;  // lower case names
};

struct Response {
    int status = 200;
    std::string type = "application/json";
    std::string body;
};

std::string UrlDecode(const std::string& s) {
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size() + 0 && std::isxdigit(static_cast<unsigned char>(s[i + 1])) && std::isxdigit(static_cast<unsigned char>(s[i + 2]))) {
            out.push_back(char(std::stoi(s.substr(i + 1, 2), nullptr, 16)));
            i += 2;
        } else if (s[i] == '+') {
            out.push_back(' ');
        } else {
            out.push_back(s[i]);
        }
    }
    return out;
}

std::string Param(const Request& r, const std::string& name) {
    size_t p = 0;
    while (p < r.query.size()) {
        size_t e = r.query.find('&', p);
        if (e == std::string::npos) e = r.query.size();
        const std::string pair = r.query.substr(p, e - p);
        const size_t eq = pair.find('=');
        if (eq != std::string::npos && pair.substr(0, eq) == name) return UrlDecode(pair.substr(eq + 1));
        p = e + 1;
    }
    return {};
}

Response Error_(int status, const std::string& message) {
    Json j = Json::Object();
    j.Set("error", message);
    return {status, "application/json", j.Dump(0)};
}

Response Ok(const Json& j) { return {200, "application/json", j.Dump(0)}; }

bool SafeName(const std::string& n) {
    return !n.empty() && n.find_first_of("/\\:") == std::string::npos && n != "." && n != ".." && n[0] != '_';
}

class Studio {
public:
    Studio(Archive& archive, std::vector<fs::path> folders, fs::path workspace, std::string token)
        : archive_(archive), source_(archive, std::move(folders)), workspace_(std::move(workspace)), token_(std::move(token)) {
        fs::create_directories(workspace_);
    }

    Response Handle(const Request& r) {
        try {
            if (r.path == "/") {
                if (Param(r, "t") != token_) return Error_(403, "open the address that dcmod printed");
                return {200, "text/html; charset=utf-8", kStudioHtml};
            }
            const auto tok = r.headers.find("x-token");
            if (tok == r.headers.end() || tok->second != token_) return Error_(403, "bad token");
            if (r.path == "/api/state") return State();
            if (r.path == "/api/files") return Files();
            if (r.path == "/api/open") return Open(Param(r, "path"), Param(r, "res"));
            if (r.path == "/api/raw") return Raw(Param(r, "path"));
            if (r.path == "/api/mod") return ModText(Param(r, "name"));
            if (r.method != "POST") return Error_(404, "unknown request");
            const Json body = r.body.empty() ? Json::Object() : Json::Parse(r.body);
            if (r.path == "/api/mod/new") return NewModDir(body);
            if (r.path == "/api/mod/save") return SaveMod(body);
            if (r.path == "/api/mod/addedit") return AddEdit(body);
            if (r.path == "/api/mod/run") return Run(body);
            return Error_(404, "unknown request");
        } catch (const std::exception& e) {
            return Error_(400, e.what());
        }
    }

private:
    Response State() {
        Json j = Json::Object();
        j.Set("entries", archive_.Entries().size());
        j.Set("workspace", workspace_.string());
        Json mods = Json::Array();
        for (const auto& it : fs::directory_iterator(workspace_)) {
            if (!it.is_directory() || !SafeName(it.path().filename().string())) continue;
            Json m = Json::Object();
            m.Set("dir", it.path().filename().string());
            try {
                const Mod mod = LoadMod(it.path());
                m.Set("id", mod.id);
                m.Set("name", mod.name);
                m.Set("version", mod.version);
                m.Set("patches", mod.patches.Items().size());
            } catch (const Error& e) {
                m.Set("error", e.what());
            }
            mods.Push(m);
        }
        j.Set("mods", mods);
        return Ok(j);
    }

    Response Files() {
        Json list = Json::Array();
        for (const IndexEntry& e : archive_.Entries()) {
            Json row = Json::Array();
            row.Push(e.name);
            row.Push(int64_t(e.size));
            list.Push(row);
        }
        return {200, "application/json", list.Dump(0)};
    }

    static std::string Show(const Bytes& b) {
        const TextFile tf = TextFile::Load(b);
        return tf.latin1 ? Latin1ToUtf8(tf.text) : tf.text;
    }

    static bool IsText(const Bytes& b) {
        for (size_t i = 0; i < b.size() && i < 8192; ++i) {
            if (b[i] == 0) return false;
        }
        return true;
    }

    Response Open(const std::string& path, const std::string& res) {
        const std::string canon = source_.Canonical(path);
        if (canon.empty()) return Error_(404, "no such game file");
        const Bytes data = source_.Read(canon);
        Json j = Json::Object();
        j.Set("path", canon);
        j.Set("size", data.size());
        if (EndsWith(Lower(canon), ".rpk")) {
            const RpkPack pack = RpkPack::Parse(data);
            if (!res.empty()) {
                const RpkResource* r = pack.Find(ParseHexId(res));
                if (!r) return Error_(404, "no such resource");
                j.Set("kind", "resource");
                j.Set("name", r->DisplayName());
                if (r->HasBlob() && IsText(r->Blob())) {
                    j.Set("kind", "text");
                    j.Set("text", Show(r->Blob()));
                } else {
                    j.Set("info", RpkTypeName(r->Type()) + ", " + std::to_string(r->data.size()) + " bytes (not text)");
                }
                return Ok(j);
            }
            j.Set("kind", "pack");
            Json list = Json::Array();
            for (const RpkResource& r : pack.resources) {
                Json row = Json::Object();
                row.Set("id", HexId(r.id));
                row.Set("type", RpkTypeName(r.Type()));
                row.Set("name", r.DisplayName());
                row.Set("size", r.data.size());
                row.Set("text", r.HasBlob() && r.data.size() < 4000000);
                list.Push(row);
            }
            j.Set("resources", list);
            return Ok(j);
        }
        if (IsLuaBytecode(data)) {
            j.Set("kind", "lua");
            j.Set("text", DisassembleLua(ParseLua(data)));
            return Ok(j);
        }
        if (IsText(data) && data.size() < 8'000'000) {
            j.Set("kind", EndsWith(Lower(canon), ".csv") ? "csv" : "text");
            j.Set("text", Show(data));
            return Ok(j);
        }
        j.Set("kind", "binary");
        return Ok(j);
    }

    Response Raw(const std::string& path) {
        const std::string canon = source_.Canonical(path);
        if (canon.empty()) return Error_(404, "no such game file");
        const Bytes d = source_.Read(canon);
        return {200, "application/octet-stream", std::string(d.begin(), d.end())};
    }

    fs::path ModDir(const std::string& name) const {
        if (!SafeName(name)) throw Error("bad mod name");
        return workspace_ / name;
    }

    Response ModText(const std::string& name) {
        const fs::path f = ModDir(name) / "mod.json";
        Json j = Json::Object();
        j.Set("text", fs::exists(f) ? ReadText(f) : std::string());
        return Ok(j);
    }

    Response NewModDir(const Json& b) {
        const std::string name = b.GetString("name");
        NewMod(ModDir(name), Lower(name), name);
        return Ok(Json::Object());
    }

    Response SaveMod(const Json& b) {
        const std::string text = b.GetString("text");
        Json::Parse(text);  // refuse text that is not JSON
        WriteText(ModDir(b.GetString("name")) / "mod.json", text);
        return Ok(Json::Object());
    }

    // Replaces what the mod does to one file or resource with the patches for the new text.
    Response AddEdit(const Json& b) {
        const fs::path dir = ModDir(b.GetString("mod"));
        const std::string canon = source_.Canonical(b.GetString("path"));
        if (canon.empty()) throw Error("no such game file");
        const std::string res = b.GetString("res");
        const std::string text = b.GetString("text");
        Bytes before = source_.Read(canon);
        if (!res.empty()) {
            RpkPack pack = RpkPack::Parse(before);
            const RpkResource* r = pack.Find(ParseHexId(res));
            if (!r || !r->HasBlob()) throw Error("that resource has no text content");
            before = r->Blob();
        }
        const TextFile tf = TextFile::Load(before);
        TextFile edited = tf;
        edited.text = tf.FromPatch(text);
        const Bytes now = edited.Save();
        Json manifest = fs::exists(dir / "mod.json") ? Json::ParseFile(dir / "mod.json") : Json::Object();
        if (!manifest.Has("id")) manifest.Set("id", Lower(dir.filename().string()));
        Json patches = Json::Array();
        if (const Json* old = manifest.Find("patches")) {
            for (const Json& p : old->Items()) {
                const bool same = p.GetString("file") == canon && (res.empty() ? p.GetString("op") != "rpk.patch" && p.GetString("op") != "rpk.set" : p.GetString("resource") == res);
                if (!same) patches.Push(p);
            }
        }
        size_t added = 0;
        if (now != before) {
            auto ops = DiffText(canon, before, now);
            if (!res.empty()) {
                Json wrap = Json::Object();
                wrap.Set("op", "rpk.patch");
                wrap.Set("file", canon);
                wrap.Set("resource", res);
                Json sub = Json::Array();
                if (ops) {
                    for (Json op : *ops) {
                        op.Remove("file");
                        sub.Push(op);
                    }
                    wrap.Set("ops", sub);
                } else {
                    const std::string rel = "resources/" + canon + "/" + res + ".txt";
                    WriteFile(dir / rel, now);
                    wrap = Json::Object();
                    wrap.Set("op", "rpk.set");
                    wrap.Set("file", canon);
                    wrap.Set("resource", res);
                    wrap.Set("from", rel);
                }
                patches.Push(wrap);
                added = 1;
            } else if (ops) {
                for (const Json& op : *ops) patches.Push(op);
                added = ops->size();
            } else {
                WriteFile(dir / "files" / canon, now);
                added = 1;
            }
        }
        manifest.Set("patches", patches);
        WriteText(dir / "mod.json", manifest.Dump());
        Json out = Json::Object();
        out.Set("added", added);
        return Ok(out);
    }

    Response Run(const Json& b) {
        std::vector<Mod> mods;
        for (const Json& n : b.Find("mods")->Items()) mods.push_back(LoadMod(ModDir(n.AsString())));
        if (mods.empty()) throw Error("no mods selected");
        std::vector<std::string> problems;
        std::vector<Mod> ordered;
        for (size_t i : OrderMods(mods, problems)) ordered.push_back(mods[i]);
        std::string log;
        for (const std::string& p : problems) log += "problem: " + p + "\n";
        PatchReport report;
        const std::map<std::string, Bytes> files = BuildMods(ordered, source_, report);
        for (const std::string& w : report.warnings) log += "warning: " + w + "\n";
        for (const std::string& c : report.conflicts) log += "conflict: " + c + "\n";
        const bool build = b.GetBool("build");
        const fs::path out = workspace_ / "_built";
        if (build) {
            fs::remove_all(out);
            for (const auto& [path, data] : files) WriteFile(out / path, data);
        }
        log += std::to_string(ordered.size()) + " mods, " + std::to_string(files.size()) + " files " + (build ? "written to " + out.string() : "would change") + "\n";
        for (const auto& [path, data] : files) log += "  " + path + " (" + HumanSize(data.size()) + ")\n";
        Json j = Json::Object();
        j.Set("log", log);
        return Ok(j);
    }

    Archive& archive_;
    GameSource source_;
    fs::path workspace_;
    std::string token_;
};

bool ReadRequest(Sock s, Request& req) {
    std::string buf;
    char chunk[8192];
    size_t head_end = std::string::npos;
    while (head_end == std::string::npos) {
        const int n = recv(s, chunk, sizeof chunk, 0);
        if (n <= 0) return false;
        buf.append(chunk, size_t(n));
        head_end = buf.find("\r\n\r\n");
        if (buf.size() > 1 << 20 && head_end == std::string::npos) return false;
    }
    const std::string head = buf.substr(0, head_end);
    std::string body = buf.substr(head_end + 4);
    size_t p = head.find("\r\n");
    const std::string line = head.substr(0, p);
    const size_t sp1 = line.find(' '), sp2 = line.find(' ', sp1 + 1);
    if (sp1 == std::string::npos || sp2 == std::string::npos) return false;
    req.method = line.substr(0, sp1);
    const std::string target = line.substr(sp1 + 1, sp2 - sp1 - 1);
    const size_t q = target.find('?');
    req.path = target.substr(0, q);
    if (q != std::string::npos) req.query = target.substr(q + 1);
    while (p != std::string::npos) {
        const size_t next = head.find("\r\n", p + 2);
        const std::string h = head.substr(p + 2, next == std::string::npos ? std::string::npos : next - p - 2);
        const size_t colon = h.find(':');
        if (colon != std::string::npos) {
            std::string v = h.substr(colon + 1);
            while (!v.empty() && v.front() == ' ') v.erase(v.begin());
            req.headers[Lower(h.substr(0, colon))] = v;
        }
        p = next;
    }
    size_t want = 0;
    if (req.headers.count("content-length")) want = size_t(std::stoull(req.headers["content-length"]));
    if (want > 64u << 20) return false;
    while (body.size() < want) {
        const int n = recv(s, chunk, sizeof chunk, 0);
        if (n <= 0) return false;
        body.append(chunk, size_t(n));
    }
    req.body = body.substr(0, want);
    return true;
}

void Send(Sock s, const Response& r) {
    const char* text = r.status == 200 ? "OK" : r.status == 403 ? "Forbidden" : r.status == 404 ? "Not Found" : "Bad Request";
    std::string out = "HTTP/1.1 " + std::to_string(r.status) + " " + text + "\r\nContent-Type: " + r.type + "\r\nContent-Length: " +
                      std::to_string(r.body.size()) + "\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n" + r.body;
    size_t sent = 0;
    while (sent < out.size()) {
        const int n = send(s, out.data() + sent, int(out.size() - sent), 0);
        if (n <= 0) return;
        sent += size_t(n);
    }
}

}  // namespace

int RunStudio(const std::vector<std::string>& argv) {
    const Args args = ParseArgs(argv, {"--update", "--base", "--game", "--port", "--mods"});
    auto archive = OpenGame(args);
    const fs::path workspace = args.Get("--mods", "dcmod-mods");
    std::random_device rd;
    char tok[40];
    std::snprintf(tok, sizeof tok, "%08x%08x%08x", rd(), rd(), rd());
    Studio studio(*archive, GameFolders(args), workspace, tok);
#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
    Sock listener = socket(AF_INET, SOCK_STREAM, 0);
    if (listener == INVALID_SOCKET) throw Error("cannot open a network socket");
    int yes = 1;
    setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&yes), sizeof yes);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(uint16_t(std::stoi(args.Get("--port", "0"))));
    if (bind(listener, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0 || listen(listener, 8) != 0) throw Error("cannot listen on that port");
    socklen_t len = sizeof addr;
    getsockname(listener, reinterpret_cast<sockaddr*>(&addr), &len);
    const unsigned port = ntohs(addr.sin_port);
    std::printf("dcmod studio is running. Open this address in a browser:\n\n  http://127.0.0.1:%u/?t=%s\n\nMods are kept in %s. Press Ctrl+C to stop.\n", port, tok, fs::absolute(workspace).string().c_str());
    std::fflush(stdout);
    for (;;) {
        Sock c = accept(listener, nullptr, nullptr);
        if (c == INVALID_SOCKET) continue;
        Request req;
        if (ReadRequest(c, req)) {
            const std::string host = req.headers.count("host") ? req.headers["host"] : "";
            // A page on another site must not be able to use the studio through the browser.
            if (host != "127.0.0.1:" + std::to_string(port) && host != "localhost:" + std::to_string(port)) Send(c, Error_(403, "bad host"));
            else Send(c, studio.Handle(req));
        }
        CLOSE_SOCKET(c);
    }
}

}  // namespace dcmod
