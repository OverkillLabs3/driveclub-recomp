// SPDX-License-Identifier: GPL-2.0-or-later
// dcmod archive ...: look into the game's own archive.
#include "cli.hpp"

#include <algorithm>
#include <chrono>
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

namespace dcmod {

namespace {

std::vector<const IndexEntry*> Select(Archive& a, const std::vector<std::string>& patterns) {
    std::vector<const IndexEntry*> out;
    for (const IndexEntry& e : a.Entries()) {
        if (patterns.empty()) {
            out.push_back(&e);
            continue;
        }
        for (const std::string& p : patterns) {
            const bool wild = p.find_first_of("*?") != std::string::npos;
            if (wild ? WildMatch(GamePath(p), e.name) : Lower(e.name) == Lower(GamePath(p))) {
                out.push_back(&e);
                break;
            }
        }
    }
    return out;
}

int Usage() {
    std::puts(
        "dcmod archive: the game's own archive (game.ndx and gameNNN.dat)\n"
        "\n"
        "  dcmod archive info                       index version, file count, volumes\n"
        "  dcmod archive list [PATTERN...] [-l]     the files, with '*' and '?' wildcards; -l adds size and volume\n"
        "  dcmod archive extract PATTERN... -o DIR  write the files under DIR with their game paths (--verify checks MD5)\n"
        "  dcmod archive cat PATH                   write one file to the standard output\n"
        "  dcmod archive verify [--md5] [PATTERN..] check names, hashes and (with --md5) every file's MD5\n"
        "  dcmod archive grep TEXT [PATTERN...]     lines of the game's text files that contain TEXT (any case)\n"
        "  dcmod archive names -o FILE              write the list of file paths\n"
        "\n"
        "The game folders come from --update DIR --base DIR (update first), the environment variables\n"
        "DRIVECLUB_UPDATE_DIR and DRIVECLUB_BASE_DIR, or the update= and base= lines of launcher.ini.");
    return 2;
}

}  // namespace

int RunArchive(const std::vector<std::string>& argv) {
    if (argv.empty()) return Usage();
    const std::string cmd = argv[0];
    const Args args = ParseArgs({argv.begin() + 1, argv.end()}, {"--update", "--base", "--game", "-o", "--output", "--limit"});
    auto archive = OpenGame(args);
    Archive& a = *archive;

    if (cmd == "info") {
        std::printf("entries      %zu\n", a.Entries().size());
        std::printf("chunk size   %u\n", a.ChunkSize());
        std::printf("compression  %u (%s)\n", a.Compression(), a.Compression() == 0 ? "none" : "LZ4");
        std::printf("hash seeds   %u %u\n", a.HashA(), a.HashB());
        uint64_t total = 0;
        for (const IndexEntry& e : a.Entries()) total += e.size;
        std::printf("total size   %s\n", HumanSize(total).c_str());
        const auto volumes = a.UsedVolumes();
        size_t missing = 0;
        for (unsigned v : volumes) missing += a.VolumePath(v).empty() ? 1 : 0;
        std::printf("volumes      %zu used, %zu missing from the game folders\n", volumes.size(), missing);
        return missing ? 1 : 0;
    }
    if (cmd == "list") {
        const bool longer = args.Has("-l") || args.Has("--long");
        for (const IndexEntry* e : Select(a, args.positional)) {
            if (longer) std::printf("%10u  game%03u.dat  %s\n", e->size, unsigned(e->volume), e->name.c_str());
            else std::printf("%s\n", e->name.c_str());
        }
        return 0;
    }
    if (cmd == "grep") {
        // dcmod archive grep TEXT [PATTERN...]: lines of the text files that contain TEXT (any case)
        if (args.positional.empty()) throw Error("grep needs the text to look for");
        const std::string needle = Lower(args.positional[0]);
        const std::vector<std::string> patterns(args.positional.begin() + 1, args.positional.end());
        size_t hits = 0, shown = 0;
        const size_t limit = args.Has("--limit") ? size_t(std::stoull(args.Get("--limit"))) : 200;
        for (const IndexEntry* e : Select(a, patterns)) {
            if (e->size > 8u << 20 || EndsWith(Lower(e->name), ".rpk")) continue;
            const Bytes data = a.Read(*e);
            bool text = true;
            for (size_t i = 0; i < data.size() && i < 4096; ++i) text = text && data[i] != 0;
            if (!text) continue;
            size_t line = 1, start = 0;
            const std::string s(data.begin(), data.end());
            for (size_t i = 0; i <= s.size(); ++i) {
                if (i == s.size() || s[i] == '\n') {
                    const std::string l = s.substr(start, i - start);
                    if (Lower(l).find(needle) != std::string::npos) {
                        ++hits;
                        if (shown++ < limit) std::printf("%s:%zu: %.200s\n", e->name.c_str(), line, l.c_str());
                    }
                    ++line;
                    start = i + 1;
                }
            }
        }
        std::printf("%zu matching lines%s\n", hits, hits > shown ? " (use --limit N to see more)" : "");
        return hits ? 0 : 1;
    }
    if (cmd == "names") {
        const std::string out = args.Get("-o", args.Get("--output"));
        if (out.empty()) throw Error("names needs -o FILE");
        std::string text;
        std::vector<std::string> names;
        for (const IndexEntry& e : a.Entries()) names.push_back(e.name);
        std::sort(names.begin(), names.end());
        for (const std::string& n : names) text += n + "\n";
        WriteText(out, text);
        std::printf("%zu paths written to %s\n", names.size(), out.c_str());
        return 0;
    }
    if (cmd == "cat") {
        if (args.positional.size() != 1) throw Error("cat needs one path");
        const IndexEntry* e = a.Find(args.positional[0]);
        if (!e) throw Error("not in the archive: " + args.positional[0]);
        const Bytes data = a.Read(*e, args.Has("--verify"));
#ifdef _WIN32
        _setmode(_fileno(stdout), _O_BINARY);  // text mode would rewrite line breaks
#endif
        std::fwrite(data.data(), 1, data.size(), stdout);
        return 0;
    }
    if (cmd == "extract") {
        const std::string out = args.Get("-o", args.Get("--output", "."));
        if (args.positional.empty()) throw Error("extract needs at least one path or pattern");
        const auto selected = Select(a, args.positional);
        if (selected.empty()) throw Error("nothing in the archive matches");
        size_t n = 0;
        uint64_t bytes = 0;
        for (const IndexEntry* e : selected) {
            const Bytes data = a.Read(*e, args.Has("--verify"));
            WriteFile(fs::path(out) / e->name, data);
            ++n;
            bytes += data.size();
        }
        std::printf("%zu files, %s, written under %s\n", n, HumanSize(bytes).c_str(), out.c_str());
        return 0;
    }
    if (cmd == "verify") {
        const auto selected = Select(a, args.positional);
        const bool md5 = args.Has("--md5");
        const size_t limit = args.Has("--limit") ? size_t(std::stoull(args.Get("--limit"))) : selected.size();
        size_t checked = 0, bad = 0;
        const auto t0 = std::chrono::steady_clock::now();
        for (const IndexEntry* e : selected) {
            if (checked >= limit) break;
            ++checked;
            if (!md5) continue;
            try {
                (void)a.Read(*e, true);
            } catch (const Error& err) {
                ++bad;
                std::printf("FAIL %s: %s\n", e->name.c_str(), err.what());
            }
        }
        const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        std::printf("%zu entries: every name matches its hash%s\n", a.Entries().size(),
                    md5 ? "" : " (use --md5 to read and check the files too)");
        if (md5) std::printf("%zu files read and checked in %.1f s, %zu failed\n", checked, s, bad);
        return bad ? 1 : 0;
    }
    return Usage();
}

}  // namespace dcmod
