// SPDX-License-Identifier: GPL-2.0-or-later
#include "archive.hpp"

#include "md5.hpp"

#include <algorithm>

namespace dcmod {

namespace {

// Names in the index are packed with a dictionary of byte pairs: a byte with the high bit set stands for two bytes
// (the first from the first table, the second from the second table), which may be such codes again.
std::string DecodeName(const uint8_t* dict, size_t dict_size, const uint8_t*& p, const uint8_t* end) {
    std::string name;
    uint8_t stack[0x100];
    size_t sp = 0;
    for (;;) {
        uint8_t c;
        if (sp > 0) {
            c = stack[--sp];
        } else {
            if (p >= end) throw Error("the file name table ends in the middle of a name");
            c = *p++;
        }
        if (c & 0x80) {
            const size_t code = size_t(c - 0x80);
            if (code >= dict_size || sp + 2 > sizeof stack) throw Error("the file name table has an invalid code");
            stack[sp++] = dict[dict_size + code];  // the second byte of the pair, taken out last
            stack[sp++] = dict[code];              // the first, taken out first
        } else if (c == 0) {
            return name;
        } else {
            name.push_back(char(c));
            if (name.size() > 0x400) throw Error("a file name in the index is too long");
        }
    }
}

// LZ4 block decoder (the format of one chunk after its 4 byte size).
bool Lz4Block(const uint8_t* src, size_t n, uint8_t* dst, size_t dsize) {
    size_t i = 0, o = 0;
    while (i < n) {
        const uint8_t token = src[i++];
        size_t lit = token >> 4;
        if (lit == 15) {
            uint8_t s;
            do {
                if (i >= n) return false;
                s = src[i++];
                lit += s;
            } while (s == 255);
        }
        if (i + lit > n || o + lit > dsize) return false;
        std::memcpy(dst + o, src + i, lit);
        i += lit;
        o += lit;
        if (i >= n) break;
        if (i + 2 > n) return false;
        const size_t offset = src[i] | (src[i + 1] << 8);
        i += 2;
        size_t match = token & 15;
        if (match == 15) {
            uint8_t s;
            do {
                if (i >= n) return false;
                s = src[i++];
                match += s;
            } while (s == 255);
        }
        match += 4;
        if (offset == 0 || offset > o || o + match > dsize) return false;
        for (size_t k = 0; k < match; ++k, ++o) dst[o] = dst[o - offset];
    }
    return o == dsize;
}

std::string VolumeFileName(unsigned volume) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "game%03u.dat", volume);
    return buf;
}

}  // namespace

uint32_t Archive::Hash(std::string_view name, uint32_t a, uint32_t b) {
    uint32_t result = 0;
    for (unsigned char ch : name) {
        uint32_t c = ch >= 'a' && ch <= 'z' ? ch - 32u : ch;
        if (c == '\\') c = '/';
        // The arithmetic wraps at 32 bits, as the game's does.
        a = uint32_t(a * b) % 0x7FFFFFFEu;
        result = (uint32_t(result * a) + c) % 0x7FFFFFFFu;
    }
    return result;
}

std::unique_ptr<Archive> Archive::Open(const std::vector<fs::path>& folders) {
    auto archive = std::unique_ptr<Archive>(new Archive());
    archive->folders_ = folders;
    Bytes ndx;
    for (const fs::path& folder : folders) {
        const fs::path candidate = folder / "game.ndx";
        if (fs::exists(candidate)) {
            ndx = ReadFile(candidate);
            break;
        }
    }
    if (ndx.empty()) throw Error("game.ndx not found in the game folders");
    if (ndx.size() < 0x40 || std::memcmp(ndx.data(), "DATX", 4) != 0) throw Error("game.ndx is not an archive index");
    const uint32_t version = Rd32(&ndx[4]);
    if (version != 4300) throw Error("game.ndx has version " + std::to_string(version) + ", only 4300 (DriveClub 1.28) is supported");
    archive->filetime_ = int64_t(Rd64(&ndx[8]));
    archive->hash_a_ = Rd32(&ndx[0x18]);
    archive->hash_b_ = Rd32(&ndx[0x1C]);
    archive->compression_ = Rd32(&ndx[0x20]);
    archive->chunk_size_ = Rd32(&ndx[0x24]);
    const uint32_t files = Rd32(&ndx[0x38]);
    if (archive->chunk_size_ == 0 || archive->chunk_size_ > 0x40000 || files == 0 || files > 1000000) {
        throw Error("game.ndx has unexpected values");
    }
    if (archive->compression_ != 0 && archive->compression_ != 6 && archive->compression_ != 7) {
        throw Error("game.ndx uses compression " + std::to_string(archive->compression_) + ", only none and LZ4 are supported");
    }
    size_t p = 0x3C;
    const size_t dict_size = ndx[p++];
    if (dict_size > 0x80 || p + 2 * dict_size + 4 > ndx.size()) throw Error("game.ndx is cut short");
    const uint8_t* dict = &ndx[p];
    p += 2 * dict_size;
    const size_t packed = Rd32(&ndx[p]);
    p += 4;
    if (p + packed + 4 > ndx.size()) throw Error("game.ndx is cut short");
    const uint8_t* names = &ndx[p];
    p += packed;
    if (Rd32(&ndx[p]) != 0x12345678u) throw Error("game.ndx has no marker after the file names");
    p += 4;
    if (p + size_t(files) * 32 > ndx.size()) throw Error("game.ndx records are cut short");
    archive->raw_index_ = ndx;
    archive->records_offset_ = p;
    const bool old_style = uint64_t(archive->filetime_) <= (1408492800ull + 11644473600ull) * 10000000ull;
    archive->entries_.resize(files);
    const uint8_t* name_p = names;
    for (uint32_t i = 0; i < files; ++i) {
        IndexEntry& e = archive->entries_[i];
        const uint8_t* rec = &ndx[p + size_t(i) * 32];
        const uint64_t loc = Rd64(rec);
        e.volume = uint16_t(old_style ? (loc & 0xFF) : (loc & 0xFFFF));
        e.offset = old_style ? (loc >> 8) : (loc >> 16);
        e.size = Rd32(rec + 8);
        e.hash = Rd32(rec + 12);
        std::memcpy(e.md5.data(), rec + 16, 16);
        e.name = DecodeName(dict, dict_size, name_p, names + packed);
        if (Hash(e.name, archive->hash_a_, archive->hash_b_) != e.hash) {
            throw Error("the name \"" + e.name + "\" does not match its hash in the index");
        }
        archive->by_hash_[e.hash].push_back(i);
    }
    return archive;
}

const IndexEntry* Archive::Find(std::string_view path) const {
    const uint32_t h = Hash(path, hash_a_, hash_b_);
    const auto it = by_hash_.find(h);
    if (it == by_hash_.end()) return nullptr;
    const std::string want = Lower(GamePath(std::string(path)));
    for (size_t i : it->second) {
        if (Lower(entries_[i].name) == want) return &entries_[i];
    }
    return &entries_[it->second.front()];
}

fs::path Archive::VolumePath(unsigned volume) const {
    for (const fs::path& folder : folders_) {
        const fs::path candidate = folder / VolumeFileName(volume);
        if (fs::exists(candidate)) return candidate;
    }
    return {};
}

std::vector<unsigned> Archive::UsedVolumes() const {
    std::vector<unsigned> v;
    for (const IndexEntry& e : entries_) v.push_back(e.volume);
    std::sort(v.begin(), v.end());
    v.erase(std::unique(v.begin(), v.end()), v.end());
    return v;
}

Archive::Volume& Archive::OpenVolume(unsigned volume) {
    const auto found = volumes_.find(volume);
    if (found != volumes_.end()) return found->second;
    Volume v;
    v.path = VolumePath(volume);
    if (v.path.empty()) throw Error("volume " + VolumeFileName(volume) + " is missing from the game folders");
    std::ifstream f(v.path, std::ios::binary);
    uint8_t head[0x24];
    if (!f.read(reinterpret_cast<char*>(head), sizeof head) || std::memcmp(head, "DATF", 4) != 0 ||
        std::memcmp(head + 0x20, "CHNK", 4) != 0) {
        throw Error(VolumeFileName(volume) + " is not a data volume");
    }
    const uint32_t count = Rd32(head + 0x18);
    v.chunk = Rd32(head + 0x1C);
    if (v.chunk != chunk_size_ || count == 0 || count > 4000000) throw Error(VolumeFileName(volume) + " has unexpected values");
    Bytes table(size_t(count) * 4 + 4);
    if (!f.read(reinterpret_cast<char*>(table.data()), std::streamsize(table.size())) ||
        std::memcmp(&table[size_t(count) * 4], "DATA", 4) != 0) {
        throw Error(VolumeFileName(volume) + " has a damaged chunk table");
    }
    v.stored.resize(count);
    v.start.resize(count);
    uint64_t pos = 0x24 + uint64_t(count) * 4 + 4;
    for (uint32_t k = 0; k < count; ++k) {
        v.stored[k] = Rd32(&table[size_t(k) * 4]);
        v.start[k] = pos;
        pos += v.stored[k];
    }
    return volumes_.emplace(volume, std::move(v)).first->second;
}

Bytes Archive::Read(const IndexEntry& e, bool verify) {
    Volume& v = OpenVolume(e.volume);
    Bytes out;
    out.reserve(e.size);
    if (e.size) {
        const uint64_t first = e.offset / v.chunk;
        const uint64_t last = (e.offset + e.size - 1) / v.chunk;
        if (last >= v.stored.size()) throw Error("\"" + e.name + "\" lies outside its volume");
        std::ifstream f(v.path, std::ios::binary);
        Bytes raw, plain(v.chunk);
        for (uint64_t k = first; k <= last; ++k) {
            const uint32_t stored = v.stored[size_t(k)];
            if (stored == 0) {
                std::fill(plain.begin(), plain.end(), uint8_t(0));
            } else {
                raw.resize(stored);
                f.clear();
                f.seekg(std::streamoff(v.start[size_t(k)]), std::ios::beg);
                if (!f.read(reinterpret_cast<char*>(raw.data()), stored)) throw Error(v.path.string() + " is cut short");
                if (stored == v.chunk) {
                    std::memcpy(plain.data(), raw.data(), v.chunk);
                } else if (stored > 4 && Rd32(raw.data()) == v.chunk) {
                    if (!Lz4Block(raw.data() + 4, stored - 4, plain.data(), v.chunk)) {
                        throw Error("a chunk of \"" + e.name + "\" could not be unpacked");
                    }
                } else {
                    throw Error("a chunk of \"" + e.name + "\" has an unexpected format");
                }
            }
            const uint64_t from = k == first ? e.offset % v.chunk : 0;
            const uint64_t to = k == last ? (e.offset + e.size - 1) % v.chunk + 1 : v.chunk;
            out.insert(out.end(), plain.begin() + std::ptrdiff_t(from), plain.begin() + std::ptrdiff_t(to));
        }
    }
    if (out.size() != e.size) throw Error("\"" + e.name + "\" came out with the wrong size");
    if (verify) {
        Md5 md5;
        md5.Update(out.data(), out.size());
        if (md5.Final() != e.md5) throw Error("the MD5 of \"" + e.name + "\" does not match the index");
    }
    return out;
}

ArchiveExtension Archive::Extend(const std::vector<std::pair<std::string, Bytes>>& files) const {
    ArchiveExtension out;
    unsigned top = 0;
    for (const IndexEntry& e : entries_) top = std::max<unsigned>(top, e.volume);
    out.volume = top + 1;
    if (out.volume > 0xFFFF) throw Error("the archive has no free volume number");

    // The new volume: the files one after the other, cut into chunks that are stored as they are.
    Bytes stream;
    std::vector<IndexEntry> added;
    std::map<std::string, size_t> replaces;
    for (const auto& [name, data] : files) {
        IndexEntry e;
        e.name = Lower(GamePath(name));
        e.volume = uint16_t(out.volume);
        e.offset = stream.size();
        e.size = uint32_t(data.size());
        e.hash = Hash(e.name, hash_a_, hash_b_);
        Md5 md5;
        md5.Update(data.data(), data.size());
        e.md5 = md5.Final();
        stream.insert(stream.end(), data.begin(), data.end());
        added.push_back(e);
    }
    const size_t chunks = (stream.size() + chunk_size_ - 1) / chunk_size_;
    stream.resize(chunks * chunk_size_, 0);
    Bytes head;
    {
        const fs::path template_volume = VolumePath(entries_.front().volume);
        if (template_volume.empty()) throw Error("no volume to take the volume header from");
        const Bytes t = ReadFile(template_volume);
        if (t.size() < 0x24) throw Error("a data volume is too short");
        head.assign(t.begin(), t.begin() + 0x24);
    }
    Put32(head, 0x18, uint32_t(chunks));
    Put32(head, 0x1C, chunk_size_);
    out.volume_data = head;
    for (size_t i = 0; i < chunks; ++i) Wr32(out.volume_data, chunk_size_);
    out.volume_data.insert(out.volume_data.end(), {'D', 'A', 'T', 'A'});
    out.volume_data.insert(out.volume_data.end(), stream.begin(), stream.end());

    // The new index: the old records as they were, the new ones after them, the names written out in full.
    std::vector<std::string> names;
    Bytes records;
    for (size_t i = 0; i < entries_.size(); ++i) {
        names.push_back(entries_[i].name);
        records.insert(records.end(), raw_index_.begin() + std::ptrdiff_t(records_offset_ + i * 32), raw_index_.begin() + std::ptrdiff_t(records_offset_ + (i + 1) * 32));
    }
    for (const IndexEntry& e : added) {
        Bytes rec;
        Wr64(rec, uint64_t(e.volume) | (e.offset << 16));
        Wr32(rec, e.size);
        Wr32(rec, e.hash);
        rec.insert(rec.end(), e.md5.begin(), e.md5.end());
        bool replaced = false;
        for (size_t i = 0; i < entries_.size() && !replaced; ++i) {
            if (entries_[i].name == e.name) {
                std::copy(rec.begin(), rec.end(), records.begin() + std::ptrdiff_t(i * 32));
                replaced = true;
            }
        }
        if (!replaced) {
            names.push_back(e.name);
            records.insert(records.end(), rec.begin(), rec.end());
        }
    }
    Bytes packed;
    for (const std::string& n : names) {
        packed.insert(packed.end(), n.begin(), n.end());
        packed.push_back(0);
    }
    out.index.assign(raw_index_.begin(), raw_index_.begin() + 0x3C);
    Put32(out.index, 0x38, uint32_t(names.size()));
    out.index.push_back(0);  // no dictionary: names are written in full
    Wr32(out.index, uint32_t(packed.size()));
    out.index.insert(out.index.end(), packed.begin(), packed.end());
    Wr32(out.index, 0x12345678u);
    out.index.insert(out.index.end(), records.begin(), records.end());
    return out;
}

}  // namespace dcmod
