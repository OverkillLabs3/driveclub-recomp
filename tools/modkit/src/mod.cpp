// SPDX-License-Identifier: GPL-2.0-or-later
#include "mod.hpp"

#include <algorithm>

namespace dcmod {

namespace {

bool ValidId(const std::string& id) {
    if (id.empty() || id.size() > 64) return false;
    for (char c : id) {
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '-' || c == '_')) return false;
    }
    return true;
}

std::vector<long> Parts(const std::string& v) {
    std::vector<long> out;
    size_t i = 0;
    while (i < v.size()) {
        size_t used = 0;
        try {
            out.push_back(std::stol(v.substr(i), &used));
        } catch (const std::exception&) {
            break;
        }
        i += used;
        if (i < v.size() && v[i] == '.') ++i;
        else break;
    }
    return out;
}

int Compare(const std::string& a, const std::string& b) {
    std::vector<long> x = Parts(a), y = Parts(b);
    const size_t n = std::max(x.size(), y.size());
    x.resize(n);
    y.resize(n);
    return x < y ? -1 : (x > y ? 1 : 0);
}

bool Satisfies(const std::string& have, const std::string& req) {
    if (req.empty()) return true;
    for (const char* op : {">=", "<=", ">", "<", "="}) {
        if (StartsWith(req, op)) {
            const int c = Compare(have, req.substr(std::strlen(op)));
            const std::string o = op;
            return o == ">=" ? c >= 0 : o == "<=" ? c <= 0 : o == ">" ? c > 0 : o == "<" ? c < 0 : c == 0;
        }
    }
    return Compare(have, req) == 0;
}

std::vector<std::string> Strings(const Json& j, const char* key) {
    std::vector<std::string> out;
    if (const Json* a = j.Find(key)) {
        for (const Json& s : a->Items()) out.push_back(s.AsString());
    }
    return out;
}

bool IsMetadata(const std::string& name) {
    const std::string n = Lower(name);
    return n == "mod.json" || StartsWith(n, "readme") || StartsWith(n, "license") || StartsWith(n, "changelog") || StartsWith(n, "credits");
}

}  // namespace

void ApplySettings(Mod& m, const Json& choices) {
    for (const auto& [key, value] : choices.Members()) {
        if (!m.settings_def.Has(key)) throw Error(m.id + ": there is no setting " + key);
        m.settings.Set(key, value);
    }
    for (const auto& [key, def] : m.settings_def.Members()) {
        const Json& v = *m.settings.Find(key);
        const std::string type = def.GetString("type", v.IsBool() ? "bool" : v.IsNumber() ? "number" : "string");
        const std::string at = m.id + ": setting " + key + ": ";
        if (type == "bool" && !v.IsBool()) throw Error(at + "must be true or false");
        if (type == "number" && !v.IsNumber()) throw Error(at + "must be a number");
        if ((type == "string" || type == "choice") && !v.IsString()) throw Error(at + "must be a text");
        if (type != "bool" && type != "number" && type != "string" && type != "choice") throw Error(at + "unknown type " + type);
        if (type == "number") {
            if (const Json* lo = def.Find("min")) if (v.AsNumber() < lo->AsNumber()) throw Error(at + "is below " + lo->Dump(0));
            if (const Json* hi = def.Find("max")) if (v.AsNumber() > hi->AsNumber()) throw Error(at + "is above " + hi->Dump(0));
        }
        if (type == "choice") {
            const Json* ch = def.Find("choices");
            bool ok = false;
            if (ch) for (const Json& c : ch->Items()) ok = ok || (c.IsString() && c.AsString() == v.AsString());
            if (!ok) throw Error(at + "is not one of its choices");
        }
    }
}

namespace {

// Patch values {"$setting": "name"} stand for the value of that setting of the mod.
Json Substitute(const Json& j, const Mod& m) {
    if (j.IsObject()) {
        if (j.Members().size() == 1 && j.Members()[0].first == "$setting" && j.Members()[0].second.IsString()) {
            const Json* v = m.settings.Find(j.Members()[0].second.AsString());
            if (!v) throw Error(m.id + ": the patch uses the setting " + j.Members()[0].second.AsString() + " which the mod does not have");
            return *v;
        }
        Json out = Json::Object();
        for (const auto& [k, v] : j.Members()) out.Set(k, Substitute(v, m));
        return out;
    }
    if (j.IsArray()) {
        Json out = Json::Array();
        for (const Json& v : j.Items()) out.Push(Substitute(v, m));
        return out;
    }
    return j;
}

std::string LuaLiteral(const Json& v) {
    if (v.IsBool()) return v.AsBool() ? "true" : "false";
    if (v.IsNumber()) {
        std::string n = v.Dump(0);
        while (!n.empty() && (n.back() == '\n' || n.back() == ' ')) n.pop_back();
        return n;
    }
    std::string out = "\"";
    for (unsigned char c : v.AsString()) {
        char buf[8];
        if (c == '"' || c == '\\' || c < 32 || c > 126) std::snprintf(buf, sizeof buf, "\\%03u", c), out += buf;
        else out.push_back(char(c));
    }
    return out + "\"";
}

}  // namespace

std::string SettingsLua(const std::vector<Mod>& mods) {
    std::string lua = "dcmod.settings = {\n";
    for (const Mod& m : mods) {
        if (m.settings.Members().empty()) continue;
        lua += "  [\"" + m.id + "\"] = {";
        for (const auto& [k, v] : m.settings.Members()) lua += " [\"" + k + "\"] = " + LuaLiteral(v) + ",";
        lua += " },\n";
    }
    lua += "}\nfunction dcmod.setting(mod, name, fallback)\n  local m = dcmod.settings[mod]\n  if m and m[name] ~= nil then return m[name] end\n  return fallback\nend\n";
    return lua;
}

Mod LoadMod(const fs::path& dir, const std::string& fallback_id) {
    Mod m;
    m.dir = dir;
    Json j = Json::Object();
    if (fs::exists(dir / "mod.json")) j = Json::ParseFile(dir / "mod.json");
    std::string fallback = Lower(dir.filename().string());
    for (char& c : fallback) {
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '-')) c = '-';
    }
    m.id = j.GetString("id", fallback_id.empty() ? fallback : fallback_id);
    if (!ValidId(m.id)) throw Error(dir.string() + ": the mod id \"" + m.id + "\" may only use a-z, 0-9, '.', '-' and '_'");
    m.name = j.GetString("name", m.id);
    m.version = j.GetString("version", "1.0");
    m.author = j.GetString("author");
    m.description = j.GetString("description");
    m.game = j.GetString("game");
    m.conflicts = Strings(j, "conflicts");
    m.after = Strings(j, "after");
    m.before = Strings(j, "before");
    m.scripts = Strings(j, "scripts");
    if (const Json* st = j.Find("settings")) {
        if (!st->IsObject()) throw Error(m.id + ": \"settings\" must be an object");
        m.settings_def = *st;
        for (const auto& [key, def] : st->Members()) {
            const Json* d = def.Find("default");
            if (!d) throw Error(m.id + ": the setting " + key + " has no default");
            m.settings.Set(key, *d);
        }
        ApplySettings(m, Json::Object());  // checks the defaults
    }
    if (const Json* r = j.Find("requires")) {
        for (const Json& d : r->Items()) {
            ModDep dep;
            if (d.IsString()) dep.id = d.AsString();
            else {
                dep.id = d.GetString("id");
                dep.version = d.GetString("version");
                dep.optional = d.GetBool("optional");
            }
            if (dep.id.empty()) throw Error(m.id + ": a requirement has no id");
            m.requires_.push_back(dep);
        }
    }
    if (const Json* p = j.Find("patches")) {
        if (!p->IsArray()) throw Error(m.id + ": \"patches\" must be a list");
        m.patches = *p;
    }
    if (j.Has("files")) {
        m.files_dir = dir / j.GetString("files");
    } else {
        for (const char* name : {"files", "mods", "app0"}) {
            if (fs::is_directory(dir / name)) {
                m.files_dir = dir / name;
                break;
            }
        }
        if (m.files_dir.empty() && m.patches.Items().empty()) m.files_dir = dir;  // the old layout: files at their game paths
    }
    if (!m.files_dir.empty() && !fs::is_directory(m.files_dir)) throw Error(m.id + ": the files folder " + m.files_dir.string() + " does not exist");
    return m;
}

std::vector<size_t> OrderMods(const std::vector<Mod>& mods, std::vector<std::string>& problems) {
    std::map<std::string, size_t> by_id;
    for (size_t i = 0; i < mods.size(); ++i) {
        if (!by_id.emplace(mods[i].id, i).second) throw Error("two mods have the id " + mods[i].id);
    }
    std::vector<std::set<size_t>> preds(mods.size());
    for (size_t i = 0; i < mods.size(); ++i) {
        const Mod& m = mods[i];
        for (const ModDep& d : m.requires_) {
            const auto it = by_id.find(d.id);
            if (it == by_id.end()) {
                if (!d.optional) problems.push_back(m.id + " needs " + d.id + ", which is not installed");
                continue;
            }
            if (!Satisfies(mods[it->second].version, d.version)) {
                problems.push_back(m.id + " needs " + d.id + " " + d.version + ", but " + mods[it->second].version + " is installed");
            }
            preds[i].insert(it->second);
        }
        for (const std::string& a : m.after) {
            if (by_id.count(a)) preds[i].insert(by_id[a]);
        }
        for (const std::string& b : m.before) {
            if (by_id.count(b)) preds[by_id[b]].insert(i);
        }
        for (const std::string& c : m.conflicts) {
            if (by_id.count(c)) problems.push_back(m.id + " cannot be used together with " + c);
        }
    }
    std::vector<size_t> order;
    std::vector<bool> done(mods.size());
    while (order.size() < mods.size()) {
        bool placed = false;
        for (size_t i = 0; i < mods.size() && !placed; ++i) {
            if (done[i]) continue;
            bool ready = true;
            for (size_t p : preds[i]) ready = ready && done[p];
            if (ready) {
                done[i] = true;
                order.push_back(i);
                placed = true;
            }
        }
        if (!placed) throw Error("the mods' requirements and load order rules go around in a circle");
    }
    return order;
}

std::map<std::string, Bytes> BuildMods(const std::vector<Mod>& ordered, FileSource& source, PatchReport& report) {
    Workspace ws(source, report);
    for (const Mod& m : ordered) {
        try {
            if (!m.files_dir.empty()) {
                std::vector<fs::path> files;
                for (const auto& it : fs::recursive_directory_iterator(m.files_dir)) {
                    if (it.is_regular_file()) files.push_back(it.path());
                }
                std::sort(files.begin(), files.end());
                for (const fs::path& f : files) {
                    const fs::path rel = f.lexically_relative(m.files_dir);
                    if (m.files_dir == m.dir && rel.has_parent_path() == false && IsMetadata(rel.string())) continue;
                    ws.Write(rel.generic_string(), ReadFile(f), m.id, "file");
                }
            }
            PatchEnv env{ws, m.id, m.dir};
            ApplyPatches(Substitute(m.patches, m), env);
        } catch (const Error& e) {
            throw Error(m.id + ": " + e.what());
        }
    }
    AddScriptLoader(ws, ordered);
    return ws.Finish();
}

void NewMod(const fs::path& dir, const std::string& id, const std::string& name) {
    if (fs::exists(dir / "mod.json")) throw Error(dir.string() + " already holds a mod");
    if (!ValidId(id)) throw Error("the id may only use a-z, 0-9, '.', '-' and '_'");
    Json j = Json::Object();
    j.Set("id", id);
    j.Set("name", name);
    j.Set("version", "1.0");
    j.Set("author", "");
    j.Set("description", "What this mod changes.");
    Json patch = Json::Object();
    patch.Set("op", "csv.set");
    patch.Set("comment", "Example: change a value in the vehicle table. Remove or edit it.");
    patch.Set("file", "newdata/vehicles.csv");
    patch.Set("key", "Vehicle Name");
    Json match = Json::Array();
    match.Push("a45_amg");
    patch.Set("match", match);
    Json set = Json::Object();
    set.Set("TopSpeed", 300);
    patch.Set("set", set);
    Json patches = Json::Array();
    patches.Push(patch);
    j.Set("patches", patches);
    WriteText(dir / "mod.json", j.Dump());
    std::error_code ec;
    fs::create_directories(dir / "files", ec);
}

}  // namespace dcmod
