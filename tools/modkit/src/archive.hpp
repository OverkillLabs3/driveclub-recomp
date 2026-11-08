// SPDX-License-Identifier: GPL-2.0-or-later
// The game's own archive: an index (game.ndx) and data volumes (gameNNN.dat).
//
// The index lists every file by a hash of its name and also carries the names themselves, packed with a small
// dictionary code, so the whole file list can be recovered. Each volume is cut into chunks of one fixed size
// (65536 bytes in version 1.28); a chunk is stored as it is, as zeros (size 0) or compressed with LZ4. A file's
// position is an offset into the volume's uncompressed data. The format was first described by the DriveClubFS
// project (MIT licence); this is an independent implementation.
#pragma once

#include "common.hpp"

#include <array>
#include <map>
#include <memory>
#include <unordered_map>

namespace dcmod {

struct IndexEntry {
    std::string name;     // "data/leveldata/canada_road_circuit_02_n.rpk"
    uint32_t hash = 0;    // of the name, see Archive::Hash
    uint16_t volume = 0;  // gameNNN.dat that holds it
    uint64_t offset = 0;  // into the volume's uncompressed data
    uint32_t size = 0;
    std::array<uint8_t, 16> md5{};
};

// New files added to the game's archive: a new index and one new volume that holds them.
struct ArchiveExtension {
    Bytes index;        // the new game.ndx
    unsigned volume = 0;
    Bytes volume_data;  // the new gameNNN.dat
};

class Archive {
public:
    // The game folders to look in, in order of priority: the update (it has the newest index and volumes) first, then
    // the base game. Every volume is taken from the first folder that has it.
    static std::unique_ptr<Archive> Open(const std::vector<fs::path>& folders);

    const std::vector<IndexEntry>& Entries() const { return entries_; }
    uint32_t HashA() const { return hash_a_; }
    uint32_t HashB() const { return hash_b_; }
    uint32_t ChunkSize() const { return chunk_size_; }
    uint32_t Compression() const { return compression_; }
    int64_t Timestamp() const { return filetime_; }

    // The entry for a path (any case, '/' or '\\'), or null.
    const IndexEntry* Find(std::string_view path) const;

    // The file's bytes. With `verify` the MD5 in the index is checked too.
    Bytes Read(const IndexEntry& entry, bool verify = false);

    // Which volumes the index uses, and which of them no folder has.
    std::vector<unsigned> UsedVolumes() const;
    fs::path VolumePath(unsigned volume) const;  // empty when no folder has it

    static uint32_t Hash(std::string_view name, uint32_t a, uint32_t b);

    // An index and a volume that make `files` (game path and content) part of the archive, next to every file it
    // already has. A path the archive already has points to the new content instead. The game reads its files
    // through the index, so a file the index does not list is unknown to parts of the game even when the path
    // overlay serves it.
    ArchiveExtension Extend(const std::vector<std::pair<std::string, Bytes>>& files) const;

private:
    struct Volume {
        fs::path path;
        std::vector<uint32_t> stored;  // stored size of each chunk
        std::vector<uint64_t> start;   // where each chunk starts in the file
        uint32_t chunk = 0;
    };
    Volume& OpenVolume(unsigned volume);

    std::vector<fs::path> folders_;
    std::vector<IndexEntry> entries_;
    std::unordered_map<uint32_t, std::vector<size_t>> by_hash_;
    std::map<unsigned, Volume> volumes_;
    Bytes raw_index_;
    size_t names_offset_ = 0, names_size_ = 0, records_offset_ = 0;
    uint32_t hash_a_ = 0, hash_b_ = 0, chunk_size_ = 0, compression_ = 0;
    int64_t filetime_ = 0;
};

}  // namespace dcmod
