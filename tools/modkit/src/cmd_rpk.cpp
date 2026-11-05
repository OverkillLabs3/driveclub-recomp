// SPDX-License-Identifier: GPL-2.0-or-later
// dcmod rpk ...: look into resource packs, take them apart and put them together.
#include "cli.hpp"
#include "json.hpp"
#include "rpk.hpp"

#include <algorithm>
#include <cctype>
#include <functional>
#include <map>
#include <set>

namespace dcmod {

namespace {

constexpr const char* kManifestName = "pack.json";

int Usage() {
    std::puts(
        "dcmod rpk: resource packs (.rpk)\n"
        "\n"
        "  dcmod rpk info PACK                     header, counts per type, required packs\n"
        "  dcmod rpk list PACK [PATTERN...] [-l]   the resources (-l adds identifiers, sizes and source paths)\n"
        "  dcmod rpk extract PACK -o DIR [PATTERN...]  write the resources and a pack.json that describes them\n"
        "  dcmod rpk build DIR -o PACK             put an extracted folder back together\n"
        "  dcmod rpk replace PACK TARGET FILE -o OUT  change one resource to the content of FILE\n"
        "  dcmod rpk verify PACK...                read each pack and write it again; the bytes must be the same\n"
        "\n"
        "PACK is a file, or @PATH for a file inside the game's archive (@data/leveldata/globaldata.rpk; verify also\n"
        "takes wildcards such as \"@*.rpk\"). A PATTERN matches the names of resources and may use '*' and '?';\n"
        "type:xml selects a type and id:HEX one resource. TARGET is a name or an identifier. For XML and binary\n"
        "resources FILE is just the content; for the others it is the whole payload, as 'extract' writes it.\n"
        "The game folders come from --update DIR --base DIR, DRIVECLUB_UPDATE_DIR and DRIVECLUB_BASE_DIR, or launcher.ini.");
    return 2;
}

struct Source {
    std::string label;
    Bytes data;
};

// Calls `each` for the pack in the file, or for every pack in the game's archive that the "@" pattern matches. The
// packs are handled one at a time because there are thousands and some are very large.
void ForEachPack(const Args& args, const std::string& spec, bool allow_wild, uint64_t max_bytes,
                 const std::function<void(Source&)>& each) {
    if (spec.empty() || spec[0] != '@') {
        Source s{spec, ReadFile(spec)};
        each(s);
        return;
    }
    auto archive = OpenGame(args);
    const std::string pattern = GamePath(spec.substr(1));
    const bool wild = pattern.find_first_of("*?") != std::string::npos;
    if (wild && !allow_wild) throw Error("a wildcard is not allowed here");
    size_t found = 0;
    for (const IndexEntry& e : archive->Entries()) {
        if (e.size <= max_bytes && (wild ? WildMatch(pattern, e.name) : Lower(e.name) == Lower(pattern))) {
            ++found;
            Source s{"@" + e.name, archive->Read(e)};
            each(s);
        }
    }
    if (found == 0) throw Error("nothing in the archive matches " + spec);
}

RpkPack LoadPack(const Args& args, const std::string& spec) {
    RpkPack pack;
    ForEachPack(args, spec, false, ~0ull, [&](Source& s) {
        try {
            pack = RpkPack::Parse(s.data);
        } catch (const Error& e) {
            throw Error(s.label + ": " + e.what());
        }
    });
    return pack;
}

bool Matches(const RpkResource& r, const std::vector<std::string>& patterns) {
    if (patterns.empty()) return true;
    for (const std::string& p : patterns) {
        if (StartsWith(p, "type:")) {
            if (int(r.Type()) == RpkTypeFromName(p.substr(5))) return true;
        } else if (StartsWith(p, "id:")) {
            if (r.id == ParseHexId(p.substr(3))) return true;
        } else {
            const std::string pat = GamePath(p);
            const bool wild = pat.find_first_of("*?") != std::string::npos;
            for (const std::string& n : r.names) {
                if (wild ? WildMatch(pat, GamePath(n)) : Lower(GamePath(n)) == Lower(pat)) return true;
            }
        }
    }
    return false;
}

std::string Describe(const RpkResource& r, bool longer) {
    char buf[96];
    std::snprintf(buf, sizeof buf, "%-22s %s %10zu  ", RpkTypeName(r.Type()).c_str(), HexId(r.id).c_str(), r.data.size());
    std::string line = buf;
    line += r.DisplayName();
    if (r.names.size() > 1) line += " (+" + std::to_string(r.names.size() - 1) + " more names)";
    if (longer && !r.sources.empty() && !r.sources.front().empty()) line += "  <- " + r.sources.front();
    if (longer && !r.deps.empty()) line += "  [" + std::to_string(r.deps.size()) + " deps]";
    return line;
}

// A name as a relative file path: folders are kept, odd characters become '_', nothing can climb out of the folder.
std::string Sanitize(const std::string& name) {
    std::string out, seg;
    const auto flush = [&] {
        if (!seg.empty() && seg != "." && seg != "..") {
            if (seg.size() > 80) seg.resize(80);
            if (!out.empty()) out.push_back('/');
            out += seg;
        }
        seg.clear();
    };
    for (unsigned char c : GamePath(name)) {
        if (c == '/') flush();
        else seg.push_back(std::isalnum(c) || c == '.' || c == '_' || c == '-' || c == '+' ? char(c) : '_');
    }
    flush();
    return out.empty() ? "unnamed" : out;
}

const Json& Member(const Json& obj, const char* key) {
    const Json* j = obj.Find(key);
    if (!j) throw Error(std::string("pack.json is missing \"") + key + "\"");
    return *j;
}

// The strings of a pool, in order, as a list (and back).
Json PoolToJson(const std::string& pool) {
    Json list = Json::Array();
    size_t p = 0;
    while (p < pool.size()) {
        const size_t e = pool.find('\0', p);
        if (e == std::string::npos) break;
        list.Push(pool.substr(p, e - p));
        p = e + 1;
    }
    return list;
}

std::string PoolFromJson(const Json& list) {
    std::string pool;
    for (const Json& s : list.Items()) {
        pool += s.AsString();
        pool.push_back('\0');
    }
    return pool;
}

const char* Extension(const RpkResource& r, bool block) {
    if (block && r.Type() == kRpkXml) return ".xml";
    if (block && r.Type() == kRpkBin) return ".bin";
    return ".res";
}

int Extract(const Args& args, const std::string& spec) {
    const std::string out = args.Get("-o", args.Get("--output"));
    if (out.empty()) throw Error("extract needs -o DIR");
    RpkPack pack = LoadPack(args, spec);
    std::vector<std::string> patterns(args.positional.begin() + 2, args.positional.end());
    Json manifest = Json::Object();
    manifest.Set("format", "dcmod-rpk");
    Json info = Json::Object();
    info.Set("id", HexId(pack.info_id));
    info.Set("version", int64_t(pack.info_version));
    manifest.Set("info", info);
    manifest.Set("root", HexId(pack.root));
    // The order of the strings in the pools is kept so that 'build' gives back the same bytes.
    Json pools = Json::Object();
    pools.Set("names", PoolToJson(pack.name_pool));
    pools.Set("sources", PoolToJson(pack.source_pool));
    manifest.Set("pools", pools);
    Json required = Json::Array();
    for (const std::string& n : pack.required) required.Push(n);
    manifest.Set("required", required);
    Json aliases = Json::Array();
    for (const RpkAlias& a : pack.aliases) {
        Json j = Json::Object();
        j.Set("id", HexId(a.id));
        j.Set("name", a.name);
        aliases.Push(j);
    }
    manifest.Set("aliases", aliases);
    Json list = Json::Array();
    std::set<std::string> used;
    size_t written = 0;
    uint64_t bytes = 0;
    const bool everything = patterns.empty();
    for (const RpkResource& r : pack.resources) {
        Json j = Json::Object();
        j.Set("id", HexId(r.id));
        j.Set("type", RpkTypeName(r.Type()));
        j.Set("order", int64_t(r.order));
        Json names = Json::Array();
        for (const std::string& n : r.names) names.Push(n);
        j.Set("names", names);
        Json sources = Json::Array();
        for (const std::string& n : r.sources) sources.Push(n);
        j.Set("sources", sources);
        Json deps = Json::Array();
        for (uint64_t d : r.deps) deps.Push(HexId(d));
        j.Set("deps", deps);
        const bool block = r.HasBlob();
        j.Set("content", block ? "block" : "raw");
        if (block) j.Set("version", int64_t(r.PayloadVersion()));
        if (everything || Matches(r, patterns)) {
            std::string base = RpkTypeName(r.Type()) + "/" + Sanitize(r.DisplayName());
            const std::string ext = Extension(r, block);
            if (!EndsWith(Lower(base), ext)) base += ext;
            if (!used.insert(Lower(base)).second) {
                base = base.substr(0, base.size() - ext.size()) + "-" + HexId(r.id).substr(4) + ext;
                used.insert(Lower(base));
            }
            const Bytes content = block ? r.Blob() : r.data;
            WriteFile(fs::path(out) / base, content);
            j.Set("file", base);
            ++written;
            bytes += content.size();
        }
        list.Push(j);
    }
    manifest.Set("resources", list);
    if (written == 0) throw Error("no resource matches");
    WriteText(fs::path(out) / kManifestName, manifest.Dump());
    std::printf("%zu of %zu resources, %s, written under %s\n", written, pack.resources.size(), HumanSize(bytes).c_str(), out.c_str());
    if (!everything) std::puts("(the folder describes the whole pack, but only the selected resources have files; 'build' needs all of them)");
    return 0;
}

int Build(const Args& args, const std::string& dir) {
    const std::string out = args.Get("-o", args.Get("--output"));
    if (out.empty()) throw Error("build needs -o PACK");
    const Json m = Json::ParseFile(fs::path(dir) / kManifestName);
    if (m.GetString("format") != "dcmod-rpk") throw Error(std::string(kManifestName) + " is not a pack description");
    RpkPack pack;
    const Json& info = Member(m, "info");
    pack.info_id = ParseHexId(info.GetString("id"));
    pack.info_version = uint32_t(info.GetInt("version"));
    pack.root = ParseHexId(m.GetString("root"));
    for (const Json& j : Member(m, "required").Items()) pack.required.push_back(j.AsString());
    for (const Json& j : Member(m, "aliases").Items()) pack.aliases.push_back({ParseHexId(j.GetString("id")), j.GetString("name")});
    if (const Json* pools = m.Find("pools")) {
        pack.name_pool = PoolFromJson(Member(*pools, "names"));
        pack.source_pool = PoolFromJson(Member(*pools, "sources"));
    }
    for (const Json& j : Member(m, "resources").Items()) {
        RpkResource r;
        r.id = ParseHexId(j.GetString("id"));
        r.order = uint32_t(j.GetInt("order"));
        for (const Json& n : Member(j, "names").Items()) r.names.push_back(n.AsString());
        for (const Json& n : Member(j, "sources").Items()) r.sources.push_back(n.AsString());
        for (const Json& n : Member(j, "deps").Items()) r.deps.push_back(ParseHexId(n.AsString()));
        const std::string file = j.GetString("file");
        if (file.empty()) throw Error("the resource " + HexId(r.id) + " has no file; extract everything before building");
        const Bytes content = ReadFile(fs::path(dir) / file);
        if (j.GetString("content") == "block") r.data = RpkResource::MakeBlobPayload(r.id, uint32_t(j.GetInt("version")), content);
        else r.data = content;
        pack.resources.push_back(std::move(r));
    }
    const Bytes file = pack.Write();
    WriteFile(out, file);
    std::printf("%zu resources, %s, written to %s\n", pack.resources.size(), HumanSize(file.size()).c_str(), out.c_str());
    return 0;
}

int Replace(const Args& args) {
    const std::string out = args.Get("-o", args.Get("--output"));
    if (out.empty() || args.positional.size() != 4) throw Error("usage: dcmod rpk replace PACK TARGET FILE -o OUT");
    RpkPack pack = LoadPack(args, args.positional[1]);
    const std::string& target = args.positional[2];
    RpkResource* r = nullptr;
    if (target.find_first_not_of("0123456789abcdefABCDEFx") == std::string::npos && target.size() >= 12) r = pack.Find(ParseHexId(target));
    if (!r) r = pack.FindByName(target);
    if (!r) throw Error("no resource named or numbered " + target);
    const Bytes content = ReadFile(args.positional[3]);
    if (r->HasBlob()) r->SetBlob(content);
    else r->data = content;
    WriteFile(out, pack.Write());
    std::printf("%s now holds %zu bytes; written to %s\n", r->DisplayName().c_str(), r->data.size(), out.c_str());
    return 0;
}

int Verify(const Args& args) {
    if (args.positional.size() < 2) throw Error("verify needs at least one pack");
    size_t checked = 0, bad = 0, id_mismatch = 0;
    const uint64_t max_bytes = args.Has("--max-mb") ? std::stoull(args.Get("--max-mb")) << 20 : ~0ull;
    for (size_t i = 1; i < args.positional.size(); ++i) {
        ForEachPack(args, args.positional[i], true, max_bytes, [&](Source& s) {
            ++checked;
            try {
                const RpkPack pack = RpkPack::Parse(s.data);
                if (pack.Write() != s.data) throw Error("writing it again gives different bytes");
                for (const RpkResource& r : pack.resources) {
                    if (r.PayloadId() != 0 && r.PayloadId() != r.id) ++id_mismatch;
                }
            } catch (const Error& e) {
                ++bad;
                std::printf("FAIL %s: %s\n", s.label.c_str(), e.what());
                std::fflush(stdout);
            }
        });
    }
    std::printf("%zu packs checked, %zu failed; resources whose payload names another identifier: %zu\n", checked, bad, id_mismatch);
    return bad ? 1 : 0;
}

}  // namespace

int RunRpk(const std::vector<std::string>& argv) {
    if (argv.empty()) return Usage();
    const std::string cmd = argv[0];
    const Args args = ParseArgs(argv, {"--update", "--base", "--game", "-o", "--output", "--max-mb"});
    // positional[0] is the command again, because ParseArgs saw all of argv
    if (args.positional.size() < 2 && cmd != "verify") return Usage();
    if (cmd == "extract") return Extract(args, args.positional[1]);
    if (cmd == "build") return Build(args, args.positional[1]);
    if (cmd == "replace") return Replace(args);
    if (cmd == "verify") return Verify(args);
    if (cmd == "info" || cmd == "list") {
        const RpkPack pack = LoadPack(args, args.positional[1]);
        if (cmd == "list") {
            const std::vector<std::string> patterns(args.positional.begin() + 2, args.positional.end());
            const bool longer = args.Has("-l") || args.Has("--long");
            for (const RpkResource& r : pack.resources) {
                if (Matches(r, patterns)) std::printf("%s\n", Describe(r, longer).c_str());
            }
            return 0;
        }
        std::printf("table version   0x%x\n", pack.info_version);
        std::printf("root            %s (%s)\n", HexId(pack.root).c_str(), RpkTypeName(unsigned(pack.root >> 48)).c_str());
        std::printf("resources       %zu\n", pack.resources.size());
        std::map<unsigned, std::pair<size_t, uint64_t>> per_type;
        uint64_t total = 0;
        for (const RpkResource& r : pack.resources) {
            per_type[r.Type()].first++;
            per_type[r.Type()].second += r.data.size();
            total += r.data.size();
        }
        for (const auto& [type, v] : per_type) {
            std::printf("  %-26s %6zu  %s\n", RpkTypeName(type).c_str(), v.first, HumanSize(v.second).c_str());
        }
        std::printf("payload size    %s\n", HumanSize(total).c_str());
        std::printf("aliases         %zu\n", pack.aliases.size());
        for (const std::string& n : pack.required) std::printf("requires        %s\n", n.c_str());
        return 0;
    }
    return Usage();
}

}  // namespace dcmod
