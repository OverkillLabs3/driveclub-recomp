// SPDX-License-Identifier: GPL-2.0-or-later
// dcmod tex ...: textures of resource packs as DDS files.
#include "cli.hpp"
#include "image.hpp"
#include "rpk.hpp"
#include "tex.hpp"

#include <cctype>
#include <functional>

namespace dcmod {

namespace {

int Usage() {
    std::puts(
        "dcmod tex: textures (pixel buffer resources) of resource packs\n"
        "\n"
        "  dcmod tex info PACK                         every texture: size, format, mips, whether the converter understands it\n"
        "  dcmod tex extract PACK -o DIR [PATTERN...]  write textures as DDS files (names as in 'rpk list')\n"
        "  dcmod tex import PACK TARGET FILE.dds -o OUT  put a DDS into the texture TARGET (name or id) of the pack\n"
        "  dcmod tex verify PACK... [--max-mb N]       tile each texture again; the bytes must be the same\n"
        "\n"
        "PACK is a file or @path in the game's archive. The DDS must have the texture's size, a matching block format and\n"
        "at least as many mip levels as the texture (tools such as texconv write all of them). Level 0 is the largest.");
    return 2;
}

void ForEachPack(const Args& args, const std::string& spec, uint64_t max_bytes,
                 const std::function<void(const std::string&, RpkPack&)>& each) {
    if (spec.empty() || spec[0] != '@') {
        RpkPack p = RpkPack::Parse(ReadFile(spec));
        each(spec, p);
        return;
    }
    auto archive = OpenGame(args);
    const std::string pattern = GamePath(spec.substr(1));
    const bool wild = pattern.find_first_of("*?") != std::string::npos;
    size_t found = 0;
    for (const IndexEntry& e : archive->Entries()) {
        if (e.size <= max_bytes && (wild ? WildMatch(pattern, e.name) : Lower(e.name) == Lower(pattern))) {
            ++found;
            RpkPack p = RpkPack::Parse(archive->Read(e));
            each("@" + e.name, p);
        }
    }
    if (!found) throw Error("nothing in the archive matches " + spec);
}

std::string Safe(const std::string& n) {
    std::string out;
    for (unsigned char c : GamePath(n)) out.push_back(std::isalnum(c) || c == '.' || c == '_' || c == '-' ? char(c) : '_');
    return out.empty() ? "unnamed" : out;
}

}  // namespace

int RunTex(const std::vector<std::string>& argv) {
    if (argv.empty()) return Usage();
    const std::string cmd = argv[0];
    const Args args = ParseArgs(argv, {"--update", "--base", "--game", "-o", "--output", "--max-mb"});
    if (args.positional.size() < 2) return Usage();
    const std::string spec = args.positional[1];
    const uint64_t max_bytes = args.Has("--max-mb") ? std::stoull(args.Get("--max-mb")) << 20 : ~0ull;

    if (cmd == "info") {
        ForEachPack(args, spec, max_bytes, [&](const std::string&, RpkPack& pack) {
            for (const RpkResource& r : pack.resources) {
                if (r.Type() != kRpkPixelBuffer) continue;
                try {
                    const PixelBuffer pb = PixelBuffer::Parse(r.data);
                    std::string note;
                    const TexFormat* f = FindTexFormat(pb.format);
                    if (!f) note = "format not supported";
                    else if (pb.start_mip != 0) note = "largest levels are in a top mips resource";
                    else if (pb.Data().size() != pb.ExpectedDataSize()) note = "size does not fit the layout";
                    else note = "ok";
                    const std::string fname = f ? f->name : "format " + std::to_string(pb.format);
                    std::printf("%-40s %4ux%-4u %-14s mips %2u  %s\n", r.DisplayName().c_str(), pb.width, pb.height, fname.c_str(), pb.mips, note.c_str());
                } catch (const Error& e) {
                    std::printf("%-40s %s\n", r.DisplayName().c_str(), e.what());
                }
            }
        });
        return 0;
    }
    if (cmd == "extract") {
        const std::string out = args.Get("-o", args.Get("--output"));
        if (out.empty()) throw Error("extract needs -o DIR");
        const std::vector<std::string> patterns(args.positional.begin() + 2, args.positional.end());
        size_t done = 0, skipped = 0;
        ForEachPack(args, spec, max_bytes, [&](const std::string&, RpkPack& pack) {
            for (const RpkResource& r : pack.resources) {
                if (r.Type() != kRpkPixelBuffer) continue;
                bool wanted = patterns.empty();
                for (const std::string& p : patterns) {
                    for (const std::string& n : r.names) wanted = wanted || WildMatch(GamePath(p), GamePath(n));
                }
                if (!wanted) continue;
                try {
                    const Dds dds = TexToDds(PixelBuffer::Parse(r.data));
                    std::string name = Safe(r.DisplayName());
                    if (!EndsWith(Lower(name), ".dds")) name += ".dds";
                    WriteFile(fs::path(out) / name, dds.Write());
                    ++done;
                } catch (const Error& e) {
                    std::printf("skipped %s: %s\n", r.DisplayName().c_str(), e.what());
                    ++skipped;
                }
            }
        });
        std::printf("%zu textures written, %zu skipped\n", done, skipped);
        return done ? 0 : 1;
    }
    if (cmd == "import") {
        const std::string out = args.Get("-o", args.Get("--output"));
        if (out.empty() || args.positional.size() != 4) throw Error("usage: dcmod tex import PACK TARGET FILE.dds -o OUT");
        RpkPack pack;
        ForEachPack(args, spec, ~0ull, [&](const std::string&, RpkPack& p) { pack = std::move(p); });
        RpkResource* r = nullptr;
        const std::string& t = args.positional[2];
        if (t.size() == 16 && t.find_first_not_of("0123456789abcdefABCDEF") == std::string::npos) r = pack.Find(ParseHexId(t));
        if (!r) r = pack.FindByName(t);
        if (!r || r->Type() != kRpkPixelBuffer) throw Error("no texture named or numbered " + t);
        PixelBuffer pb = PixelBuffer::Parse(r->data);
        pb.SetData(DdsToTexData(pb, TextureSource(pb, ReadFile(args.positional[3]))));
        r->data = pb.payload;
        WriteFile(out, pack.Write());
        std::printf("%s replaced; written to %s\n", r->DisplayName().c_str(), out.c_str());
        return 0;
    }
    if (cmd == "verify") {
        size_t good = 0, layout = 0, unsupported = 0, topmips = 0, volumes = 0, bad = 0;
        for (size_t i = 1; i < args.positional.size(); ++i) {
            ForEachPack(args, args.positional[i], max_bytes, [&](const std::string& label, RpkPack& pack) {
                for (const RpkResource& r : pack.resources) {
                    if (r.Type() != kRpkPixelBuffer) continue;
                    try {
                        PixelBuffer pb = PixelBuffer::Parse(r.data);
                        if (!FindTexFormat(pb.format)) {
                            ++unsupported;
                            continue;
                        }
                        if (pb.start_mip != 0) {
                            ++topmips;
                            continue;
                        }
                        if (pb.depth > 1 || pb.type != 0) {
                            ++volumes;
                            continue;
                        }
                        if (pb.Data().size() != pb.ExpectedDataSize()) {
                            ++layout;
                            if (layout <= 5) {
                                std::printf("layout %s %s: %ux%u fmt %u mips %u data %zu expected %zu\n", label.c_str(), r.DisplayName().c_str(), pb.width, pb.height, pb.format, pb.mips, pb.Data().size(), pb.ExpectedDataSize());
                            }
                            continue;
                        }
                        const Dds back = Dds::Parse(TexToDds(pb).Write());
                        if (DdsToTexData(pb, back) != pb.Data()) throw Error("tiling it again gives different bytes");
                        ++good;
                    } catch (const Error& e) {
                        ++bad;
                        std::printf("FAIL %s %s: %s\n", label.c_str(), r.DisplayName().c_str(), e.what());
                    }
                }
            });
        }
        std::printf("%zu textures round trip, %zu with a size the layout does not predict, %zu with unsupported formats, %zu with top mips elsewhere, %zu failed\n", good, layout, unsupported, topmips, bad);
        return bad ? 1 : 0;
    }
    return Usage();
}

}  // namespace dcmod
