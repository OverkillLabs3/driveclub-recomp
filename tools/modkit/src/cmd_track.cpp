// SPDX-License-Identifier: GPL-2.0-or-later
// dcmod track ...: shows a track: its row of newdata/tracks.csv and the files named after its RPKName (the level data, the
// world pack and its patch packs). A copy under a new name does not load yet, see docs/modding/tracks.md.
#include "cli.hpp"
#include "csv.hpp"
#include "mod.hpp"

#include <algorithm>

namespace dcmod {

namespace {

int Usage() {
    std::puts(
        "dcmod track: the files and table row of a track\n"
        "\n"
        "  dcmod track files NAME     the table row of a track (NAME is its Track Name) and every file named after it\n"
        "\n"
        "The game folders come from --update DIR --base DIR, DRIVECLUB_UPDATE_DIR and DRIVECLUB_BASE_DIR, or launcher.ini.");
    return 2;
}

// True when a path segment starts with `asset` followed by nothing, '.', '_' or '/'.
bool Named(const std::string& path, const std::string& asset) {
    size_t pos = 0;
    while (pos <= path.size()) {
        size_t slash = path.find('/', pos);
        if (slash == std::string::npos) slash = path.size();
        const std::string seg = Lower(path.substr(pos, slash - pos));
        if (seg.compare(0, asset.size(), Lower(asset)) == 0) {
            const char next = seg.size() > asset.size() ? seg[asset.size()] : '\0';
            if (next == '\0' || next == '.' || next == '_') return true;
        }
        pos = slash + 1;
    }
    return false;
}

}  // namespace

int RunTrack(const std::vector<std::string>& argv) {
    if (argv.size() < 2 || argv[0] != "files") return Usage();
    const Args args = ParseArgs(argv, {"--update", "--base", "--game"});
    if (args.positional.size() < 2) return Usage();
    auto archive = OpenGame(args);
    GameSource source(*archive, GameFolders(args));
    const std::string table_path = source.Canonical("newdata/tracks.csv");
    if (table_path.empty()) throw Error("newdata/tracks.csv is not in the game files");
    const TextFile tf = TextFile::Load(source.Read(table_path));
    const CsvDoc doc = CsvDoc::Parse(tf.latin1 ? Latin1ToUtf8(tf.text) : tf.text);
    const int name_col = doc.Column("Track Name"), rpk_col = doc.Column("RPKName");
    if (name_col < 0 || rpk_col < 0) throw Error("the track table does not have the expected columns");
    size_t row = 0;
    for (size_t r = 1; r < doc.rows.size(); ++r) {
        if (doc.Get(r, size_t(name_col)) == args.positional[1]) row = r;
    }
    if (!row) throw Error("no track has the Track Name \"" + args.positional[1] + "\" in newdata/tracks.csv");
    const std::string asset = doc.Get(row, size_t(rpk_col));
    if (asset.empty()) throw Error("that track has no RPKName");
    std::vector<std::string> files;
    for (const IndexEntry& e : archive->Entries()) {
        if (Named(e.name, asset) && (StartsWith(e.name, "worlds/") || StartsWith(e.name, "data/leveldata/"))) files.push_back(e.name);
    }
    std::sort(files.begin(), files.end());
    for (size_t c = 0; c < doc.rows[0].fields.size(); ++c) {
        if (!doc.Get(row, c).empty()) std::printf("%-26s %s\n", doc.Get(0, c).c_str(), doc.Get(row, c).c_str());
    }
    std::printf("\n%zu files are named after \"%s\":\n", files.size(), asset.c_str());
    for (const std::string& f : files) std::printf("  %s\n", f.c_str());
    return 0;
}

}  // namespace dcmod
