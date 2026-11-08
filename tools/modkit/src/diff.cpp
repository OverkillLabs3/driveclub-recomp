// SPDX-License-Identifier: GPL-2.0-or-later
#include "diff.hpp"

#include "csv.hpp"

#include <algorithm>
#include <optional>

namespace dcmod {

namespace {

std::vector<std::string> SplitLines(const std::string& s) {
    std::vector<std::string> out;
    size_t p = 0;
    for (;;) {
        const size_t e = s.find('\n', p);
        if (e == std::string::npos) {
            out.push_back(s.substr(p));
            return out;
        }
        out.push_back(s.substr(p, e - p));
        p = e + 1;
    }
}

std::string Join(const std::vector<std::string>& l, size_t from, size_t to) {
    std::string out;
    for (size_t i = from; i < to; ++i) {
        if (i > from) out.push_back('\n');
        out += l[i];
    }
    return out;
}

size_t Count(const std::string& hay, const std::string& needle) {
    if (needle.empty()) return 0;
    size_t n = 0;
    for (size_t at = hay.find(needle); at != std::string::npos; at = hay.find(needle, at + 1)) ++n;
    return n;
}

struct Hunk {
    size_t os, oe, ns, ne;  // old lines [os,oe) become new lines [ns,ne)
};

// Line differences through the longest common subsequence; null when the files are too large for that.
std::optional<std::vector<Hunk>> Hunks(const std::vector<std::string>& a, const std::vector<std::string>& b) {
    size_t pre = 0;
    while (pre < a.size() && pre < b.size() && a[pre] == b[pre]) ++pre;
    size_t suf = 0;
    while (suf < a.size() - pre && suf < b.size() - pre && a[a.size() - 1 - suf] == b[b.size() - 1 - suf]) ++suf;
    const size_t n = a.size() - pre - suf, m = b.size() - pre - suf;
    if (n == 0 && m == 0) return std::vector<Hunk>{};
    if ((n + 1) * (m + 1) > 25'000'000) return std::vector<Hunk>{{pre, pre + n, pre, pre + m}};
    std::vector<uint32_t> dp((n + 1) * (m + 1), 0);
    const auto at = [&](size_t i, size_t j) -> uint32_t& { return dp[i * (m + 1) + j]; };
    for (size_t i = n; i-- > 0;) {
        for (size_t j = m; j-- > 0;) {
            at(i, j) = a[pre + i] == b[pre + j] ? at(i + 1, j + 1) + 1 : std::max(at(i + 1, j), at(i, j + 1));
        }
    }
    std::vector<Hunk> out;
    size_t i = 0, j = 0;
    while (i < n || j < m) {
        if (i < n && j < m && a[pre + i] == b[pre + j]) {
            ++i;
            ++j;
            continue;
        }
        Hunk h{pre + i, pre + i, pre + j, pre + j};
        while ((i < n || j < m) && !(i < n && j < m && a[pre + i] == b[pre + j])) {
            if (j < m && (i >= n || at(i, j + 1) >= at(i + 1, j))) ++j;
            else ++i;
        }
        h.oe = pre + i;
        h.ne = pre + j;
        out.push_back(h);
    }
    return out;
}

Json TextOp(const std::string& file, const std::string& find, const std::string& replace) {
    Json op = Json::Object();
    op.Set("op", "text.replace");
    op.Set("file", file);
    op.Set("find", find);
    op.Set("replace", replace);
    return op;
}

// text.replace patches that turn `from` into `to`; null when no set of unambiguous patches was found.
std::optional<std::vector<Json>> TextPatches(const std::string& file, const std::string& from, const std::string& to) {
    const std::vector<std::string> a = SplitLines(from), b = SplitLines(to);
    const auto hunks = Hunks(a, b);
    if (!hunks) return std::nullopt;
    struct Range {
        size_t os, oe, ns, ne;
    };
    std::vector<Range> ranges;
    for (const Hunk& h : *hunks) {
        Range r{h.os, h.oe, h.ns, h.ne};
        // Context lines are added until the text to find is there exactly once.
        for (int step = 0; step < 12; ++step) {
            const std::string find = Join(a, r.os, r.oe);
            if (!find.empty() && Count(from, find) == 1) break;
            if (r.os > 0 && (step % 2 == 0 || r.oe >= a.size())) {
                --r.os;
                --r.ns;
            } else if (r.oe < a.size()) {
                ++r.oe;
                ++r.ne;
            }
        }
        if (!ranges.empty() && r.os <= ranges.back().oe) {
            ranges.back().oe = std::max(ranges.back().oe, r.oe);
            ranges.back().ne = r.ne;
        } else {
            ranges.push_back(r);
        }
    }
    std::vector<Json> ops;
    for (const Range& r : ranges) {
        const std::string find = Join(a, r.os, r.oe);
        if (find.empty() || Count(from, find) != 1) return std::nullopt;
        ops.push_back(TextOp(file, find, Join(b, r.ns, r.ne)));
    }
    return ops;
}

// A column whose values are all different, to find rows by.
int KeyColumn(const CsvDoc& d) {
    if (d.rows.size() < 2) return -1;
    for (size_t c = 0; c < d.rows[0].fields.size(); ++c) {
        std::set<std::string> seen;
        bool ok = true;
        for (size_t r = 1; r < d.rows.size() && ok; ++r) {
            const std::string& v = d.Get(r, c);
            ok = !v.empty() && v.find('"') == std::string::npos && seen.insert(v).second;
        }
        if (ok) return int(c);
    }
    return -1;
}

std::optional<std::vector<Json>> CsvPatches(const std::string& file, const std::string& from, const std::string& to) {
    const CsvDoc a = CsvDoc::Parse(from), b = CsvDoc::Parse(to);
    if (a.rows.empty() || b.rows.empty() || a.rows[0].raw != b.rows[0].raw) return std::nullopt;
    const int kc = KeyColumn(a);
    if (kc < 0 || KeyColumn(b) < 0) return std::nullopt;
    const std::string key = a.rows[0].fields[size_t(kc)].value;
    std::map<std::string, size_t> old_rows, new_rows;
    for (size_t r = 1; r < a.rows.size(); ++r) old_rows[a.Get(r, size_t(kc))] = r;
    for (size_t r = 1; r < b.rows.size(); ++r) new_rows[b.Get(r, size_t(kc))] = r;
    std::vector<Json> ops;
    const size_t width = a.rows[0].fields.size();
    for (size_t r = 1; r < b.rows.size(); ++r) {
        const std::string k = b.Get(r, size_t(kc));
        const auto it = old_rows.find(k);
        Json op = Json::Object();
        if (it == old_rows.end()) {
            op.Set("op", "csv.add");
            op.Set("file", file);
            op.Set("key", key);
            Json row = Json::Object();
            for (size_t c = 0; c < width; ++c) row.Set(a.rows[0].fields[c].value, b.Get(r, c));
            op.Set("row", row);
            if (r > 1) op.Set("after", b.Get(r - 1, size_t(kc)));
            ops.push_back(op);
            continue;
        }
        Json set = Json::Object();
        for (size_t c = 0; c < width; ++c) {
            if (a.Get(it->second, c) != b.Get(r, c)) set.Set(a.rows[0].fields[c].value, b.Get(r, c));
        }
        if (set.Members().empty()) continue;
        op.Set("op", "csv.set");
        op.Set("file", file);
        op.Set("key", key);
        op.Set("match", k);
        op.Set("set", set);
        ops.push_back(op);
    }
    for (size_t r = 1; r < a.rows.size(); ++r) {
        const std::string k = a.Get(r, size_t(kc));
        if (new_rows.count(k)) continue;
        Json op = Json::Object();
        op.Set("op", "csv.remove");
        op.Set("file", file);
        op.Set("key", key);
        op.Set("match", k);
        ops.push_back(op);
    }
    return ops;
}

bool LooksLikeText(const Bytes& b) {
    for (size_t i = 0; i < b.size() && i < 8192; ++i) {
        if (b[i] == 0) return false;
    }
    return true;
}

std::string Rel(const fs::path& root, const fs::path& p) { return p.lexically_relative(root).generic_string(); }

}  // namespace

std::optional<std::vector<Json>> DiffText(const std::string& canon, const Bytes& before, const Bytes& now) {
    if (!LooksLikeText(before) || !LooksLikeText(now)) return std::nullopt;
    const TextFile a = TextFile::Load(before), b = TextFile::Load(now);
    if (a.crlf != b.crlf || a.latin1 != b.latin1 || a.bom != b.bom || a.raw || b.raw) return std::nullopt;
    const std::string from = a.latin1 ? Latin1ToUtf8(a.text) : a.text, to = b.latin1 ? Latin1ToUtf8(b.text) : b.text;
    std::optional<std::vector<Json>> ops;
    if (EndsWith(Lower(canon), ".csv")) ops = CsvPatches(canon, from, to);
    if (!ops) ops = TextPatches(canon, from, to);
    return ops;
}

DiffResult MakeModFromEdits(FileSource& source, const fs::path& edited, const fs::path& out, const std::string& id, const std::string& name) {
    DiffResult res;
    Json patches = Json::Array();
    std::map<std::string, Bytes> expected;  // what building the mod must give back, by game path
    std::vector<std::string> pack_checks;
    std::vector<fs::path> files;
    std::vector<fs::path> pack_dirs;
    for (auto it = fs::recursive_directory_iterator(edited); it != fs::recursive_directory_iterator(); ++it) {
        if (it->is_directory() && EndsWith(Lower(it->path().filename().string()), ".rpk") && fs::exists(it->path() / "pack.json")) {
            pack_dirs.push_back(it->path());
            it.disable_recursion_pending();
        } else if (it->is_regular_file()) {
            files.push_back(it->path());
        }
    }
    std::sort(files.begin(), files.end());
    for (const fs::path& f : files) {
        const std::string rel = Rel(edited, f);
        const Bytes now = ReadFile(f);
        const std::string canon = source.Canonical(rel);
        if (canon.empty()) {
            WriteFile(out / "files" / rel, now);
            expected[Lower(rel)] = now;
            ++res.loose_files;
            res.notes.push_back(rel + ": not a game file, added as a new file");
            continue;
        }
        const Bytes before = source.Read(canon);
        if (before == now) {
            ++res.unchanged;
            continue;
        }
        expected[Lower(canon)] = now;
        const std::optional<std::vector<Json>> ops = DiffText(canon, before, now);
        if (ops) {
            for (const Json& op : *ops) {
                patches.Push(op);
                ++res.patches;
            }
        } else {
            WriteFile(out / "files" / canon, now);
            ++res.loose_files;
            res.notes.push_back(canon + ": changes could not be described as patches, the whole file is included");
        }
    }
    for (const fs::path& dir : pack_dirs) {
        const std::string rel = Rel(edited, dir);
        const std::string canon = source.Canonical(rel);
        if (canon.empty()) {
            res.notes.push_back(rel + ": not a pack of the game, skipped");
            continue;
        }
        RpkPack original = RpkPack::Parse(source.Read(canon));
        const Json m = Json::ParseFile(dir / "pack.json");
        const Json* list = m.Find("resources");
        if (!list) throw Error(rel + "/pack.json has no resources");
        size_t changed = 0;
        for (const Json& j : list->Items()) {
            const std::string file = j.GetString("file");
            if (file.empty()) continue;
            const RpkResource* r = original.Find(ParseHexId(j.GetString("id")));
            if (!r) {
                res.notes.push_back(rel + ": " + j.GetString("id") + " is not in the game's pack, skipped");
                continue;
            }
            const Bytes now = ReadFile(dir / file);
            const bool block = j.GetString("content") == "block";
            if (now == (block ? r->Blob() : r->data)) continue;
            WriteFile(out / "resources" / canon / file, now);
            Json op = Json::Object();
            op.Set("op", "rpk.set");
            op.Set("file", canon);
            op.Set("resource", HexId(r->id));
            op.Set("from", "resources/" + canon + "/" + file);
            op.Set("comment", r->DisplayName());
            patches.Push(op);
            ++res.patches;
            ++changed;
        }
        if (changed == 0) ++res.unchanged;
    }
    Json manifest = Json::Object();
    manifest.Set("id", id);
    manifest.Set("name", name);
    manifest.Set("version", "1.0");
    manifest.Set("description", "");
    manifest.Set("patches", patches);
    WriteText(out / "mod.json", manifest.Dump());

    // Build the mod again and compare with the edited files.
    Mod mod = LoadMod(out);
    PatchReport report;
    const std::map<std::string, Bytes> built = BuildMods({mod}, source, report);
    std::map<std::string, const Bytes*> lower;
    for (const auto& [path, data] : built) lower[Lower(path)] = &data;
    for (const auto& [path, data] : expected) {
        const auto it = lower.find(path);
        if (it == lower.end() || *it->second != data) res.notes.push_back("CHECK FAILED: building the mod does not give back " + path);
    }
    return res;
}

}  // namespace dcmod
