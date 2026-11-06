// SPDX-License-Identifier: GPL-2.0-or-later
#include "patch.hpp"

#include "csv.hpp"
#include "lua.hpp"
#include "image.hpp"
#include "tex.hpp"
#include "xml.hpp"

#include <algorithm>
#include <regex>

namespace dcmod {

// ---- sources and workspace -------------------------------------------------------------------------------------

std::string GameSource::Canonical(const std::string& path) {
    if (const IndexEntry* e = archive_.Find(path)) return e->name;
    const std::string p = GamePath(path);
    for (const fs::path& f : folders_) {
        std::error_code ec;
        if (fs::is_regular_file(f / p, ec)) return p;
    }
    return {};
}

Bytes GameSource::Read(const std::string& canonical) {
    for (const fs::path& f : folders_) {
        std::error_code ec;
        if (fs::is_regular_file(f / canonical, ec)) return ReadFile(f / canonical);
    }
    const IndexEntry* e = archive_.Find(canonical);
    if (!e) throw Error("the game has no file " + canonical);
    return archive_.Read(*e);
}

Workspace::Item& Workspace::Get(const std::string& path, bool must_exist) {
    const std::string key = Key(path);
    auto it = items_.find(key);
    if (it != items_.end()) return it->second;
    const std::string canon = source_.Canonical(path);
    if (canon.empty() && must_exist) throw Error("the game has no file " + GamePath(path));
    Item item;
    item.name = canon.empty() ? GamePath(path) : canon;
    return items_.emplace(key, std::move(item)).first->second;
}

bool Workspace::Exists(const std::string& path) {
    return items_.count(Key(path)) != 0 || !source_.Canonical(path).empty();
}

namespace {
void Sync(Bytes& data, bool& changed, std::unique_ptr<RpkPack>& pack, bool& pack_changed) {
    if (pack && pack_changed) {
        data = pack->Write();
        pack_changed = false;
        changed = true;
    }
}
}  // namespace

Bytes Workspace::Read(const std::string& path) {
    Item& it = Get(path, true);
    if (!it.loaded) {
        it.data = source_.Read(it.name);
        it.loaded = true;
    }
    Sync(it.data, it.changed, it.pack, it.pack_changed);
    return it.data;
}

void Workspace::Write(const std::string& path, Bytes data, const std::string& mod, const std::string& action) {
    Item& it = Get(path, false);
    if (it.loaded && !it.pack_changed && it.data == data) return;
    it.data = std::move(data);
    it.loaded = it.changed = true;
    it.pack.reset();
    it.pack_changed = false;
    report_.history[it.name].push_back({mod, action});
}

RpkPack& Workspace::Pack(const std::string& path) {
    Item& it = Get(path, true);
    if (!it.pack) {
        if (!it.loaded) {
            it.data = source_.Read(it.name);
            it.loaded = true;
        }
        try {
            it.pack = std::make_unique<RpkPack>(RpkPack::Parse(it.data));
        } catch (const Error& e) {
            throw Error(it.name + ": " + e.what());
        }
    }
    return *it.pack;
}

void Workspace::PackChanged(const std::string& path, const std::string& mod, const std::string& action) {
    Item& it = Get(path, true);
    it.pack_changed = true;
    report_.history[it.name].push_back({mod, action});
}

void Workspace::NoteSetting(const std::string& slot, const std::string& value, const std::string& mod) {
    const auto it = settings_.find(slot);
    if (it != settings_.end() && it->second.mod != mod && it->second.value != value) {
        report_.conflicts.push_back(slot + ": \"" + it->second.value + "\" from " + it->second.mod + " is replaced by \"" + value + "\" from " + mod);
    }
    settings_[slot] = {value, mod};
}

bool GameSource::ExtendArchive(const std::vector<std::pair<std::string, Bytes>>& files, std::map<std::string, Bytes>& out) {
    const ArchiveExtension x = archive_.Extend(files);
    char name[32];
    std::snprintf(name, sizeof name, "game%03u.dat", x.volume);
    out["game.ndx"] = x.index;
    out[name] = x.volume_data;
    return true;
}

std::map<std::string, Bytes> Workspace::Finish() {
    std::map<std::string, Bytes> out;
    for (auto& [key, it] : items_) {
        Sync(it.data, it.changed, it.pack, it.pack_changed);
        if (it.changed) out[it.name] = it.data;
    }
    if (!registered_.empty()) {
        std::vector<std::pair<std::string, Bytes>> files;
        for (const std::string& key : registered_) {
            const auto it = items_.find(key);
            if (it == items_.end() || !it->second.loaded) throw Error("archive.register: " + key + " is not a file the mods made");
            files.emplace_back(it->second.name, it->second.data);
        }
        if (!source_.ExtendArchive(files, out)) throw Error("archive.register needs the game's archive");
        report_.history["game.ndx"].push_back({"dcmod", "archive index with " + std::to_string(files.size()) + " new files"});
    }
    return out;
}

// ---- operation plumbing ------------------------------------------------------------------------------------------

namespace {

class Params {
public:
    explicit Params(const Json& op) : op_(op) {
        if (!op.IsObject()) throw Error("a patch must be an object like {\"op\": \"text.replace\", ...}");
        for (const char* k : {"op", "comment", "optional"}) used_.insert(k);
    }
    bool Has(const char* k) {
        used_.insert(k);
        return op_.Has(k);
    }
    const Json* Get(const char* k) {
        used_.insert(k);
        return op_.Find(k);
    }
    std::string Str(const char* k) {
        const Json* j = Get(k);
        if (!j || !j->IsString()) throw Error(std::string("the field \"") + k + "\" (text) is required");
        return j->AsString();
    }
    std::string Str(const char* k, const std::string& def) {
        const Json* j = Get(k);
        return j && j->IsString() ? j->AsString() : def;
    }
    bool Bool(const char* k, bool def) {
        const Json* j = Get(k);
        return j && j->IsBool() ? j->AsBool() : def;
    }
    int64_t Int(const char* k, int64_t def) {
        const Json* j = Get(k);
        return j && j->IsNumber() ? j->AsInt() : def;
    }
    void Done() const {
        for (const Json::Member& m : op_.Members()) {
            if (!used_.count(m.first)) throw Error("unknown field \"" + m.first + "\" (check the spelling)");
        }
    }

private:
    const Json& op_;
    std::set<std::string> used_;
};

Bytes ModFile(const PatchEnv& env, const std::string& rel) {
    const fs::path root = fs::weakly_canonical(env.dir);
    const fs::path p = fs::weakly_canonical(root / fs::path(rel));
    const fs::path r = p.lexically_relative(root);
    if (fs::path(rel).is_absolute() || r.empty() || *r.begin() == "..") throw Error("\"" + rel + "\" is outside the mod's folder");
    return ReadFile(p);
}

// Content from "from" (a file of the mod) or "text".
Bytes Content(Params& p, const PatchEnv& env) {
    if (p.Has("from")) return ModFile(env, p.Str("from"));
    const std::string t = p.Str("text");
    return Bytes(t.begin(), t.end());
}

std::string Scalar(const Json& j, const TextFile& tf) {
    if (j.IsString()) return tf.FromPatch(j.AsString());
    if (j.IsBool()) return j.AsBool() ? "true" : "false";
    if (j.IsNumber()) {
        const double d = j.AsNumber();
        char buf[40];
        if (d == double(int64_t(d))) std::snprintf(buf, sizeof buf, "%lld", static_cast<long long>(int64_t(d)));
        else std::snprintf(buf, sizeof buf, "%.10g", d);
        return buf;
    }
    throw Error("a table value must be text, a number or true/false");
}

Bytes FromHex(const std::string& hex) {
    Bytes out;
    int hi = -1;
    for (char c : hex) {
        if (c == ' ') continue;
        int v;
        if (c >= '0' && c <= '9') v = c - '0';
        else if (c >= 'a' && c <= 'f') v = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') v = c - 'A' + 10;
        else throw Error("\"" + hex + "\" is not hex");
        if (hi < 0) hi = v;
        else {
            out.push_back(uint8_t(hi * 16 + v));
            hi = -1;
        }
    }
    if (hi >= 0) throw Error("\"" + hex + "\" has an odd number of hex digits");
    return out;
}

struct Hit {
    size_t pos, len;
    std::string repl;
};

std::vector<Hit> FindHits(const std::string& text, const std::string& pat, bool regex, const std::string& repl) {
    std::vector<Hit> hits;
    if (pat.empty()) throw Error("the text to find is empty");
    if (regex) {
        std::regex re;
        try {
            re = std::regex(pat, std::regex::ECMAScript);
        } catch (const std::regex_error& e) {
            throw Error(std::string("bad regular expression: ") + e.what());
        }
        for (std::sregex_iterator it(text.begin(), text.end(), re), end; it != end; ++it) {
            if (it->length() == 0) continue;
            hits.push_back({size_t(it->position()), size_t(it->length()), it->format(repl)});
        }
    } else {
        for (size_t at = text.find(pat); at != std::string::npos; at = text.find(pat, at + pat.size())) hits.push_back({at, pat.size(), repl});
    }
    return hits;
}

// The hits an operation acts on: all of them, the nth, or the only one.
std::vector<Hit> Choose(std::vector<Hit> hits, Params& p, const std::string& what) {
    const bool all = p.Bool("all", false);
    const int64_t nth = p.Int("occurrence", 0);
    if (hits.empty()) throw Error(what + " was not found (is this file a different game version, or did another mod change it?)");
    if (all) return hits;
    if (nth > 0) {
        if (size_t(nth) > hits.size()) throw Error(what + " occurs only " + std::to_string(hits.size()) + " times");
        return {hits[size_t(nth) - 1]};
    }
    if (hits.size() > 1) {
        throw Error(what + " occurs " + std::to_string(hits.size()) + " times; use a longer text, \"occurrence\": N or \"all\": true");
    }
    return hits;
}

std::vector<size_t> ChooseNodes(std::vector<size_t> nodes, Params& p, const std::string& path) {
    const bool all = p.Bool("all", false);
    if (nodes.empty()) throw Error("nothing in the XML matches the path " + path);
    if (nodes.size() > 1 && !all) {
        throw Error("the path " + path + " matches " + std::to_string(nodes.size()) + " elements; narrow it down or use \"all\": true");
    }
    return nodes;
}

void MergeJson(Json& into, const Json& patch) {
    if (!patch.IsObject() || !into.IsObject()) {
        into = patch;
        return;
    }
    for (const Json::Member& m : patch.Members()) {
        if (m.second.IsNull()) {
            into.Remove(m.first);
        } else if (const Json* cur = into.Find(m.first); cur && cur->IsObject() && m.second.IsObject()) {
            Json copy = *cur;
            MergeJson(copy, m.second);
            into.Set(m.first, copy);
        } else {
            into.Set(m.first, m.second);
        }
    }
}

LuaConst ToConst(const Json& j) {
    LuaConst c;
    if (j.IsNull()) c.type = LuaConst::Nil;
    else if (j.IsBool()) {
        c.type = LuaConst::Bool;
        c.b = j.AsBool();
    } else if (j.IsNumber()) {
        c.type = LuaConst::Number;
        c.n = j.AsNumber();
    } else if (j.IsString()) {
        c.type = LuaConst::String;
        c.s = j.AsString();
    } else {
        throw Error("a Lua constant must be text, a number, true/false or null");
    }
    return c;
}

bool SameConst(const LuaConst& a, const LuaConst& b) {
    if (a.type != b.type) return false;
    switch (a.type) {
        case LuaConst::Nil: return true;
        case LuaConst::Bool: return a.b == b.b;
        case LuaConst::Number: return a.n == b.n;
        case LuaConst::String: return a.s == b.s;
    }
    return false;
}

size_t ReplaceConsts(LuaProto& f, const LuaConst& from, const LuaConst& to, const std::vector<size_t>& path, size_t depth) {
    if (depth < path.size()) {
        if (path[depth] >= f.protos.size()) throw Error("the script has no function with that number");
        return ReplaceConsts(f.protos[path[depth]], from, to, path, depth + 1);
    }
    size_t n = 0;
    for (LuaConst& k : f.k) {
        if (SameConst(k, from)) {
            k = to;
            ++n;
        }
    }
    for (LuaProto& p : f.protos) n += ReplaceConsts(p, from, to, {}, 0);
    return n;
}

// scripts/filemanifest.cfg lists every file of the game by folder: "/folder" opens one, its files follow by name, "/.."
// closes it. The game's own file system works from this list, so a new file that is not in it is unknown to the game.
struct ManifestNode {
    std::vector<std::pair<std::string, int>> kids;  // the line as written, and the folder's index (or -1 for a file)
};

std::vector<ManifestNode> ParseManifest(const std::string& text) {
    std::vector<ManifestNode> nodes(1);
    std::vector<int> stack{0};
    size_t pos = 0;
    while (pos < text.size()) {
        size_t e = text.find('\n', pos);
        if (e == std::string::npos) e = text.size();
        const std::string line = text.substr(pos, e - pos);
        pos = e + 1;
        if (line == "/..") {
            if (stack.size() > 1) stack.pop_back();
            else nodes[0].kids.emplace_back(line, -1);  // the file ends with one closing line too many; keep it
        } else if (!line.empty() && line[0] == '/') {
            nodes.emplace_back();
            nodes[size_t(stack.back())].kids.emplace_back(line, int(nodes.size()) - 1);
            stack.push_back(int(nodes.size()) - 1);
        } else {
            nodes[size_t(stack.back())].kids.emplace_back(line, -1);
        }
    }
    return nodes;
}

void WriteManifest(const std::vector<ManifestNode>& nodes, int at, std::string& out) {
    for (const auto& [line, child] : nodes[size_t(at)].kids) {
        out += line + "\n";
        if (child >= 0) {
            WriteManifest(nodes, child, out);
            out += "/..\n";
        }
    }
}

void ManifestAdd(std::vector<ManifestNode>& nodes, const std::string& path) {
    int at = 0;
    std::string rest = GamePath(path);
    for (size_t slash; (slash = rest.find('/')) != std::string::npos; rest.erase(0, slash + 1)) {
        const std::string dir = rest.substr(0, slash);
        int next = -1;
        for (const auto& [line, child] : nodes[size_t(at)].kids) {
            if (child >= 0 && Lower(line.substr(1)) == Lower(dir)) {
                next = child;
                break;
            }
        }
        if (next < 0) {
            nodes.emplace_back();
            next = int(nodes.size()) - 1;
            nodes[size_t(at)].kids.emplace_back("/" + dir, next);
        }
        at = next;
    }
    auto& kids = nodes[size_t(at)].kids;
    for (const auto& k : kids) {
        if (k.second < 0 && Lower(k.first) == Lower(rest)) return;  // already listed
    }
    size_t place = kids.size();
    for (size_t i = 0; i < kids.size(); ++i) {
        if (kids[i].second < 0 && Lower(kids[i].first) > Lower(rest)) {
            place = i;
            break;
        }
    }
    kids.insert(kids.begin() + std::ptrdiff_t(place), {rest, -1});
}

// The operations that work on the content of a file or of a resource inside a pack. Returns false when `name` is not
// one of them.
bool ContentOp(const std::string& name, Params& p, const std::string& label, Bytes& data, PatchEnv& env) {
    Workspace& ws = env.ws;
    if (name == "text.replace" || name == "text.insert" || name == "text.append" || name == "text.prepend") {
        TextFile tf = TextFile::Load(data);
        if (name == "text.replace") {
            const bool rx = p.Has("regex");
            const std::string pat = rx ? p.Str("regex") : tf.FromPatch(p.Str("find"));
            const std::string rep = rx ? p.Str("replace") : tf.FromPatch(p.Str("replace"));
            std::vector<Hit> hits = Choose(FindHits(tf.text, pat, rx, rx ? tf.FromPatch(rep) : rep), p, "\"" + pat + "\"");
            p.Done();
            for (size_t i = hits.size(); i-- > 0;) tf.text.replace(hits[i].pos, hits[i].len, hits[i].repl);
        } else if (name == "text.insert") {
            const bool after = p.Has("after");
            if (after == p.Has("before")) throw Error("give either \"after\" or \"before\"");
            const bool rx = p.Bool("regex", false);
            const std::string anchor = tf.FromPatch(p.Str(after ? "after" : "before"));
            const std::string ins = tf.FromPatch(p.Str("text"));
            std::vector<Hit> hits = Choose(FindHits(tf.text, anchor, rx, ""), p, "\"" + anchor + "\"");
            p.Done();
            for (size_t i = hits.size(); i-- > 0;) tf.text.insert(after ? hits[i].pos + hits[i].len : hits[i].pos, ins);
        } else {
            const Bytes c = Content(p, env);
            p.Done();
            std::string add = tf.FromPatch(std::string(c.begin(), c.end()));
            if (name == "text.append") {
                if (!tf.text.empty() && tf.text.back() != '\n' && !StartsWith(add, "\n")) tf.text.push_back('\n');
                tf.text += add;
            } else {
                if (!add.empty() && add.back() != '\n') add.push_back('\n');
                tf.text.insert(0, add);
            }
        }
        data = tf.Save();
        return true;
    }
    if (StartsWith(name, "csv.")) {
        TextFile tf = TextFile::Load(data);
        CsvDoc doc = CsvDoc::Parse(tf.text);
        if (doc.rows.empty()) throw Error("the table is empty");
        const std::string key = p.Str("key", doc.rows[0].fields[0].value);
        const int kc = doc.Column(key);
        if (kc < 0) throw Error("the table has no column \"" + key + "\"");
        const auto column = [&](const std::string& c) {
            const int i = doc.Column(c);
            if (i < 0) throw Error("the table has no column \"" + c + "\"");
            return size_t(i);
        };
        const auto matching_rows = [&]() {
            const Json* m = p.Get("match");
            if (!m) throw Error("the field \"match\" is required");
            std::vector<std::string> wanted;
            bool any = false;
            if (m->IsArray()) {
                for (const Json& j : m->Items()) wanted.push_back(Scalar(j, tf));
            } else if (m->IsString() && m->AsString() == "*") {
                any = true;
            } else {
                wanted.push_back(Scalar(*m, tf));
            }
            std::vector<std::pair<size_t, std::string>> where;
            if (const Json* w = p.Get("where")) {
                for (const Json::Member& wm : w->Members()) where.emplace_back(column(wm.first), Scalar(wm.second, tf));
            }
            std::vector<size_t> rows;
            for (size_t r = 1; r < doc.rows.size(); ++r) {
                if (doc.rows[r].fields.size() <= size_t(kc)) continue;
                if (!any && std::find(wanted.begin(), wanted.end(), doc.Get(r, size_t(kc))) == wanted.end()) continue;
                bool ok = true;
                for (const auto& [c, v] : where) ok = ok && doc.Get(r, c) == v;
                if (ok) rows.push_back(r);
            }
            if (rows.empty()) throw Error("no row has " + key + " matching that");
            return rows;
        };
        if (name == "csv.set") {
            const Json* set = p.Get("set");
            if (!set || !set->IsObject()) throw Error("the field \"set\" (columns and values) is required");
            const std::vector<size_t> rows = matching_rows();
            p.Done();
            for (size_t r : rows) {
                for (const Json::Member& m : set->Members()) {
                    const size_t c = column(m.first);
                    const std::string v = Scalar(m.second, tf);
                    ws.NoteSetting(label + " | " + doc.Get(r, size_t(kc)) + " | " + m.first, v, env.mod);
                    doc.Set(r, c, v);
                }
            }
        } else if (name == "csv.remove") {
            std::vector<size_t> rows = matching_rows();
            p.Done();
            std::sort(rows.rbegin(), rows.rend());
            for (size_t r : rows) doc.Remove(r);
        } else if (name == "csv.add") {
            const Json* row = p.Get("row");
            if (!row || !row->IsObject()) throw Error("the field \"row\" (columns and values) is required");
            long like = -1, after = -1;
            std::vector<std::string> values(doc.rows[0].fields.size());
            const auto find_row = [&](const std::string& v) {
                for (size_t r = 1; r < doc.rows.size(); ++r) {
                    if (doc.Get(r, size_t(kc)) == tf.FromPatch(v)) return long(r);
                }
                throw Error("no row has " + key + " \"" + v + "\"");
            };
            if (p.Has("like")) {
                like = find_row(p.Str("like"));
                for (size_t c = 0; c < values.size(); ++c) values[c] = doc.Get(size_t(like), c);
            }
            if (p.Has("after")) after = find_row(p.Str("after"));
            const bool replace = p.Bool("replace", false);
            p.Done();
            for (const Json::Member& m : row->Members()) values[column(m.first)] = Scalar(m.second, tf);
            for (size_t r = 1; r < doc.rows.size(); ++r) {
                if (doc.Get(r, size_t(kc)) == values[size_t(kc)]) {
                    if (!replace) throw Error("a row with " + key + " \"" + values[size_t(kc)] + "\" exists; use csv.set, or \"replace\": true");
                    doc.Remove(r);
                    break;
                }
            }
            doc.Add(values, like, after);
            ws.NoteSetting(label + " | row | " + values[size_t(kc)], "added", env.mod);
        } else {
            return false;
        }
        tf.text = doc.Serialize();
        data = tf.Save();
        return true;
    }
    if (StartsWith(name, "xml.")) {
        TextFile tf = TextFile::Load(data);
        const std::string path = p.Str("path");
        std::string text = tf.text;
        const size_t total = ChooseNodes(XmlDoc::Parse(text).Select(path), p, path).size();
        if (name == "xml.set") {
            const bool has_attr = p.Has("attr"), has_text = p.Has("text"), drop = p.Has("remove_attr");
            if (int(has_attr) + int(has_text) + int(drop) != 1) throw Error("give exactly one of \"attr\" (with \"value\"), \"text\" or \"remove_attr\"");
            const std::string attr = has_attr ? p.Str("attr") : p.Str("remove_attr", "");
            const std::string value = has_attr ? tf.FromPatch(p.Str("value")) : (has_text ? tf.FromPatch(p.Str("text")) : "");
            const bool raw = p.Bool("raw", false);
            p.Done();
            for (size_t k = total; k-- > 0;) {
                const XmlDoc doc = XmlDoc::Parse(text);
                const std::vector<size_t> nodes = doc.Select(path);
                if (k >= nodes.size()) continue;
                if (has_attr) {
                    ws.NoteSetting(label + " | " + path + " | @" + attr, value, env.mod);
                    text = doc.SetAttr(nodes[k], attr, value, !raw);
                } else if (has_text) {
                    ws.NoteSetting(label + " | " + path + " | text", value, env.mod);
                    text = doc.SetText(nodes[k], value, !raw);
                } else {
                    text = doc.RemoveAttr(nodes[k], attr);
                }
            }
        } else if (name == "xml.remove") {
            p.Done();
            for (size_t k = total; k-- > 0;) {
                const XmlDoc doc = XmlDoc::Parse(text);
                const std::vector<size_t> nodes = doc.Select(path);
                if (k < nodes.size()) text = doc.Remove(nodes[k]);
            }
        } else if (name == "xml.insert") {
            const std::string where = p.Str("where", "append");
            const std::string frag = tf.FromPatch(p.Str("xml"));
            p.Done();
            XmlDoc::Parse("<r>" + frag + "</r>");  // the fragment must be well formed
            for (size_t k = total; k-- > 0;) {
                const XmlDoc doc = XmlDoc::Parse(text);
                const std::vector<size_t> nodes = doc.Select(path);
                if (k < nodes.size()) text = doc.Insert(nodes[k], where, frag);
            }
        } else {
            return false;
        }
        XmlDoc::Parse(text);  // never write XML that does not parse
        tf.text = text;
        data = tf.Save();
        return true;
    }
    if (name == "json.merge") {
        const Json* value = p.Get("value");
        if (!value) throw Error("the field \"value\" is required");
        p.Done();
        Json doc = Json::Parse(std::string(data.begin(), data.end()));
        MergeJson(doc, *value);
        const std::string out = doc.Dump();
        data.assign(out.begin(), out.end());
        return true;
    }
    if (name == "bytes.patch" || name == "bytes.replace") {
        if (name == "bytes.patch") {
            const Json* o = p.Get("offset");
            if (!o) throw Error("the field \"offset\" is required");
            const size_t off = o->IsString() ? size_t(std::stoull(o->AsString(), nullptr, 0)) : size_t(o->AsInt());
            const Bytes write = FromHex(p.Str("write"));
            const Bytes expect = p.Has("expect") ? FromHex(p.Str("expect")) : Bytes();
            p.Done();
            if (off + write.size() > data.size()) throw Error("the offset is beyond the end of the file");
            if (!expect.empty() && (off + expect.size() > data.size() || !std::equal(expect.begin(), expect.end(), data.begin() + std::ptrdiff_t(off)))) {
                throw Error("the bytes at that offset are not the expected ones (different game version?)");
            }
            std::copy(write.begin(), write.end(), data.begin() + std::ptrdiff_t(off));
        } else {
            const Bytes find = FromHex(p.Str("find")), repl = FromHex(p.Str("replace"));
            if (find.empty()) throw Error("the bytes to find are empty");
            std::vector<Hit> hits;
            for (auto it = std::search(data.begin(), data.end(), find.begin(), find.end()); it != data.end();
                 it = std::search(it + std::ptrdiff_t(find.size()), data.end(), find.begin(), find.end())) {
                hits.push_back({size_t(it - data.begin()), find.size(), ""});
            }
            hits = Choose(hits, p, "that byte pattern");
            p.Done();
            for (size_t i = hits.size(); i-- > 0;) {
                data.erase(data.begin() + std::ptrdiff_t(hits[i].pos), data.begin() + std::ptrdiff_t(hits[i].pos + hits[i].len));
                data.insert(data.begin() + std::ptrdiff_t(hits[i].pos), repl.begin(), repl.end());
            }
        }
        return true;
    }
    if (name == "manifest.add") {
        const Json* paths = p.Get("paths");
        if (!paths || !paths->IsArray()) throw Error("the field \"paths\" (a list of game paths) is required");
        p.Done();
        TextFile tf = TextFile::Load(data);
        std::vector<ManifestNode> nodes = ParseManifest(tf.text);
        for (const Json& j : paths->Items()) ManifestAdd(nodes, j.AsString());
        std::string out;
        WriteManifest(nodes, 0, out);
        tf.text = out;
        data = tf.Save();
        return true;
    }
    if (name == "vdef.set") {
        // Sets a number in one record of vehicledefs.vdef: "offset" is counted from the start of the record (its name).
        const std::string rec = p.Str("name");
        const int64_t offset = p.Int("offset", -1);
        const Json* value = p.Get("value");
        const std::string type = p.Str("type", "float");
        const Json* expect = p.Get("expect");
        p.Done();
        constexpr size_t kHead = 20, kRecord = 3616, kName = 32;
        if (data.size() < kHead || std::memcmp(data.data(), "EVOSLITLVDEF", 12) != 0) throw Error("this is not a vehicle definitions file");
        if (offset < int64_t(kName) || offset + 4 > int64_t(kRecord) || offset % 4 != 0) throw Error("\"offset\" must be a multiple of 4 inside the record, after its name (32 to 3612)");
        if (!value || !value->IsNumber()) throw Error("\"value\" must be a number");
        const uint32_t count = Rd32(&data[16]);
        for (uint32_t i = 0; i < count; ++i) {
            uint8_t* r = &data[kHead + size_t(i) * kRecord];
            if (std::strncmp(reinterpret_cast<const char*>(r), rec.c_str(), kName) != 0) continue;
            uint8_t* at = r + offset;
            if (expect && expect->IsNumber()) {
                float have;
                std::memcpy(&have, at, 4);
                if (type == "float" ? have != float(expect->AsNumber()) : Rd32(at) != uint32_t(expect->AsInt())) throw Error("the value at that offset is not the expected one (different game version?)");
            }
            if (type == "float") {
                const float f = float(value->AsNumber());
                std::memcpy(at, &f, 4);
            } else if (type == "u32") {
                Put32(data, size_t(at - data.data()), uint32_t(value->AsInt()));
            } else {
                throw Error("\"type\" must be float or u32");
            }
            return true;
        }
        throw Error("no vehicle definition named " + rec);
    }
    if (name == "vdef.clone") {
        // vehicledefs.vdef: "EVOSLITLVDEF3-02", a u32 count, then records of 3616 bytes that start with a 32 byte name
        // (padded with 'w'). A copy of one record under a new name is added at the end.
        const std::string from = p.Str("from"), fresh = p.Str("name");
        p.Done();
        constexpr size_t kHead = 20, kRecord = 3616, kName = 32;
        if (data.size() < kHead || std::memcmp(data.data(), "EVOSLITLVDEF", 12) != 0) throw Error("this is not a vehicle definitions file");
        const uint32_t count = Rd32(&data[16]);
        if (kHead + size_t(count) * kRecord > data.size()) throw Error("the vehicle definitions file is not laid out as expected (version " + std::string(reinterpret_cast<const char*>(&data[12]), 4) + ")");
        if (fresh.empty() || fresh.size() >= kName) throw Error("the new name must be 1 to 31 characters");
        const auto record_name = [&](size_t i) { return std::string(reinterpret_cast<const char*>(&data[kHead + i * kRecord]), strnlen(reinterpret_cast<const char*>(&data[kHead + i * kRecord]), kName)); };
        size_t found = count;
        for (size_t i = 0; i < count; ++i) {
            if (record_name(i) == fresh) throw Error("a vehicle definition named " + fresh + " exists");
            if (record_name(i) == from) found = i;
        }
        if (found == count) throw Error("no vehicle definition named " + from);
        Bytes copy(data.begin() + std::ptrdiff_t(kHead + found * kRecord), data.begin() + std::ptrdiff_t(kHead + (found + 1) * kRecord));
        std::fill(copy.begin(), copy.begin() + std::ptrdiff_t(kName), uint8_t('w'));
        std::copy(fresh.begin(), fresh.end(), copy.begin());
        copy[fresh.size()] = 0;
        const size_t end = kHead + size_t(count) * kRecord;
        data.insert(data.begin() + std::ptrdiff_t(end), copy.begin(), copy.end());
        Put32(data, 16, count + 1);
        return true;
    }
    if (name == "lua.const") {
        const Json* f = p.Get("find");
        const Json* r = p.Get("replace");
        if (!f || !r) throw Error("\"find\" and \"replace\" are required");
        std::vector<size_t> path;
        if (p.Has("function")) {
            std::string fn = p.Str("function");
            if (StartsWith(fn, "main")) fn.erase(0, 4);
            for (size_t i = 0; i < fn.size();) {
                if (fn[i] == '/') {
                    ++i;
                    continue;
                }
                size_t used = 0;
                path.push_back(std::stoul(fn.substr(i), &used));
                i += used;
            }
        }
        const int64_t expect = p.Int("expect", 1);
        p.Done();
        LuaChunk chunk = ParseLua(data);
        const size_t n = ReplaceConsts(chunk.main, ToConst(*f), ToConst(*r), path, 0);
        if (n == 0) throw Error("the script has no such constant (different game version?)");
        if (expect > 0 && size_t(expect) != n) throw Error("the constant occurs " + std::to_string(n) + " times, expected " + std::to_string(expect) + " (set \"expect\")");
        data = WriteLua(chunk);
        return true;
    }
    return false;
}

RpkResource& FindResource(RpkPack& pack, const std::string& ref) {
    const bool hex = (ref.size() == 16 || StartsWith(ref, "0x")) && ref.find_first_not_of("0123456789abcdefABCDEFx") == std::string::npos;
    RpkResource* r = hex ? pack.Find(ParseHexId(ref)) : pack.FindByName(ref);
    if (!r) throw Error("the pack has no resource " + ref);
    return *r;
}

void ApplyOne(const Json& opjson, PatchEnv& env) {
    Params p(opjson);
    const std::string name = p.Str("op");
    Workspace& ws = env.ws;
    const auto action = [&](const std::string& what) { return name + (what.empty() ? "" : " " + what); };

    if (name == "file.set") {
        const std::string file = p.Str("file");
        const Bytes c = Content(p, env);
        p.Done();
        ws.Write(file, c, env.mod, action(""));
        return;
    }
    if (name == "archive.register") {
        const Json* paths = p.Get("paths");
        if (!paths || !paths->IsArray()) throw Error("the field \"paths\" (a list of game paths) is required");
        p.Done();
        for (const Json& j : paths->Items()) {
            if (!ws.Exists(j.AsString())) throw Error(j.AsString() + " does not exist (make it with an earlier patch)");
            (void)ws.Read(j.AsString());
            ws.Register(j.AsString());
        }
        return;
    }
    if (name == "file.copy") {
        const std::string from = p.Str("from"), file = p.Str("file");
        p.Done();
        ws.Write(file, ws.Read(from), env.mod, action("from " + from));
        return;
    }
    if (name == "rpk.set" || name == "rpk.patch" || name == "rpk.add" || name == "rpk.remove" || name == "tex.set" || name == "rpk.rename" || name == "rpk.copy") {
        const std::string file = p.Str("file");
        RpkPack& pack = ws.Pack(file);
        if (name == "rpk.copy") {
            // A copy of a resource under another name, with an identifier made from the new name.
            const std::string ref = p.Str("resource"), fresh = p.Str("name");
            p.Done();
            RpkResource& src = FindResource(pack, ref);
            RpkResource copy = src;
            uint64_t h = 0xcbf29ce484222325ull;
            for (unsigned char c : fresh) h = (h ^ c) * 0x100000001b3ull;
            copy.id = (uint64_t(src.Type()) << 48) | (h & 0xFFFFFFFFFFFFull);
            while (pack.Find(copy.id)) ++copy.id;
            if (copy.data.size() >= 16 && std::memcmp(copy.data.data(), "EVOSLITL", 8) == 0) {
                for (int i = 0; i < 8; ++i) copy.data[size_t(8 + i)] = uint8_t(copy.id >> (8 * i));
            }
            copy.names.assign(1, fresh);
            pack.Add(std::move(copy));
            ws.PackChanged(file, env.mod, action(ref + " as " + fresh));
            return;
        }
        if (name == "rpk.rename") {
            // Text in the names a pack lists: the packs it requires, and optionally resource names or source paths.
            const std::string find = p.Str("find"), repl = p.Str("replace"), scope = p.Str("scope", "required");
            p.Done();
            if (find.empty()) throw Error("\"find\" is empty");
            if (scope != "required" && scope != "names" && scope != "sources" && scope != "all") throw Error("scope must be required, names, sources or all");
            size_t changed = 0;
            const auto edit = [&](std::string& s) {
                for (size_t at = s.find(find); at != std::string::npos; at = s.find(find, at + repl.size())) {
                    s.replace(at, find.size(), repl);
                    ++changed;
                }
            };
            if (scope == "required" || scope == "all") for (std::string& s : pack.required) edit(s);
            for (RpkResource& r : pack.resources) {
                if (scope == "names" || scope == "all") for (std::string& s : r.names) edit(s);
                if (scope == "sources" || scope == "all") for (std::string& s : r.sources) edit(s);
            }
            if (scope == "names" || scope == "all") for (RpkAlias& a : pack.aliases) edit(a.name);
            if (changed == 0) throw Error("\"" + find + "\" was not found in the " + scope + " names");
            ws.PackChanged(file, env.mod, action(find + " -> " + repl));
            return;
        }
        if (name == "rpk.add") {
            RpkResource r;
            const int type = RpkTypeFromName(p.Str("type"));
            if (type < 0) throw Error("unknown resource type \"" + p.Str("type") + "\"");
            r.id = ParseHexId(p.Str("id"));
            if (unsigned(r.id >> 48) != unsigned(type)) throw Error("the first four hex digits of \"id\" must be the type number (" + HexId(uint64_t(type) << 48).substr(0, 4) + ")");
            r.names.push_back(p.Str("name", "Unnamed"));
            if (const Json* s = p.Get("sources")) for (const Json& j : s->Items()) r.sources.push_back(j.AsString());
            if (r.sources.empty()) r.sources.push_back("");
            if (const Json* d = p.Get("deps")) for (const Json& j : d->Items()) r.deps.push_back(ParseHexId(j.AsString()));
            const uint32_t version = uint32_t(p.Int("version", 0x10000));
            const Bytes c = Content(p, env);
            p.Done();
            r.data = (type == int(kRpkXml) || type == int(kRpkBin)) ? RpkResource::MakeBlobPayload(r.id, version, c) : c;
            pack.Add(std::move(r));
            ws.PackChanged(file, env.mod, action(p.Str("name", "")));
            return;
        }
        const std::string ref = p.Str("resource");
        RpkResource& res = FindResource(pack, ref);
        if (name == "rpk.remove") {
            p.Done();
            pack.Remove(res.id);
        } else if (name == "tex.set") {
            const Bytes c = Content(p, env);
            const std::string top_file = p.Str("top", "");
            p.Done();
            if (res.Type() != kRpkPixelBuffer) throw Error("the resource " + ref + " is not a texture");
            PixelBuffer pb = PixelBuffer::Parse(res.data);
            if (pb.start_mip != 0) {
                // the largest levels are in a top mips resource of another pack
                if (top_file.empty()) throw Error("the largest levels of " + ref + " are in a top mips pack: name it in \"top\" (for a car, vehicles/<asset>_top_mips.rpk)");
                RpkPack& top_pack = ws.Pack(top_file);
                RpkResource* tr = top_pack.Find(pb.top_mips_id);
                if (!tr) throw Error(top_file + " does not hold the largest levels of " + ref);
                pb.SetData(DdsToTexData(pb, TextureSource(pb, c), &tr->data));
                ws.PackChanged(top_file, env.mod, action(ref + " (largest levels)"));
            } else {
                pb.SetData(DdsToTexData(pb, TextureSource(pb, c)));
            }
            res.data = pb.payload;
        } else if (name == "rpk.set") {
            const Bytes c = Content(p, env);
            p.Done();
            ws.NoteSetting(file + " | resource | " + HexId(res.id), std::to_string(c.size()) + ":" + std::to_string(std::hash<std::string>()(std::string(c.begin(), c.end()))), env.mod);
            if (res.Type() == kRpkXml || res.Type() == kRpkBin) res.SetBlob(c);
            else res.data = c;
        } else {
            const Json* ops = p.Get("ops");
            if (!ops || !ops->IsArray()) throw Error("the field \"ops\" (a list of changes to the resource) is required");
            p.Done();
            if (!res.HasBlob()) throw Error("only XML and binary resources can be patched this way; use rpk.set for the others");
            Bytes blob = res.Blob();
            for (const Json& sub : ops->Items()) {
                Params sp(sub);
                const std::string sname = sp.Str("op");
                if (!ContentOp(sname, sp, file + " | " + ref, blob, env)) throw Error("\"" + sname + "\" cannot be used inside rpk.patch");
            }
            res.SetBlob(blob);
        }
        ws.PackChanged(file, env.mod, action(ref));
        return;
    }
    // everything else changes the content of one file
    const std::string file = p.Str("file");
    Bytes data = ws.Read(file);
    const Bytes before = data;
    if (!ContentOp(name, p, GamePath(file), data, env)) throw Error("unknown operation \"" + name + "\"");
    if (data != before) ws.Write(file, std::move(data), env.mod, action(""));
}

}  // namespace

void ApplyPatches(const Json& ops, PatchEnv& env) {
    if (!ops.IsArray()) throw Error("\"patches\" must be a list");
    size_t number = 0;
    for (const Json& op : ops.Items()) {
        ++number;
        const std::string label = op.IsObject() ? op.GetString("op") + " " + op.GetString("file") : std::string("?");
        try {
            ApplyOne(op, env);
        } catch (const Error& e) {
            if (op.IsObject() && op.GetBool("optional", false)) {
                env.ws.report().warnings.push_back(env.mod + ": patch " + std::to_string(number) + " (" + label + ") skipped: " + e.what());
                continue;
            }
            throw Error("patch " + std::to_string(number) + " (" + label + "): " + e.what());
        }
    }
}

}  // namespace dcmod
