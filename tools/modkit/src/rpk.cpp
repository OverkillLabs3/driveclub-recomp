// SPDX-License-Identifier: GPL-2.0-or-later
#include "rpk.hpp"

#include <algorithm>
#include <numeric>
#include <unordered_map>

namespace dcmod {

namespace {

const char* const kTypeNames[] = {
    "unknown", "resource_info_block", "stream_format", "vertex_buffer", "index_buffer", "pixel_buffer", "shader",
    "material", "mesh", "resource_collection", "havok", "hierarchy", "rigid_body_bindings", "world", "meshes", "scene",
    "vehicle", "font", "cameras", "camera", "scene_object", "environment", "character", "basic_bitmap", "solids",
    "ground_rush", "particle_effect", "road_system", "gpu_program", "gpu_program_base", "attribute_set",
    "attribute_set_bindings", "basic_font", "octree", "gui_anim", "plant", "animations", "lights", "bin", "xml", "patch",
    "nexus_character", "nexus_animation", "level_data", "trigger_volume", "do_reset_volume", "no_reset_volume",
    "spawn_point", "road_region_volume", "start_finish_volume", "type_50", "type_51", "road_data", "vehicle_audio",
    "vehicle_sound_effect", "vehicle_sample", "road_network", "split_volume", "dem", "dem_tile", "vehicle_custom_data",
    "scripted_road_line", "procedural_placement", "actor_data", "texture_atlas_index", "crowds", "sound_group", "svg_mesh",
    "drift_zone", "audio_fsb", "animation_lib", "top_mips", "ai_max_speed_volume", "scripted_road_region_volume",
    "trigger_data", "dust_data", "vehicle_particles_data"};
constexpr unsigned kTypeCount = sizeof kTypeNames / sizeof kTypeNames[0];

constexpr char kMagic[] = "EVOSLITL";
constexpr char kPackName[] = "Resource PacK file";
constexpr uint32_t kInfoMarker = 0x11111111u, kInfoEnd = 0x99999999u;
constexpr size_t kPackHeader = 8 + 4 + 18 + 4 + 4;  // 38: the table of contents starts here

using Reader = ByteReader;

// The strings of a pool, found by their text.
class Pool {
public:
    explicit Pool(std::string text) : text_(std::move(text)) {
        size_t p = 0;
        while (p < text_.size()) {
            const size_t e = text_.find('\0', p);
            if (e == std::string::npos) break;
            index_.emplace(text_.substr(p, e - p), uint32_t(p));
            p = e + 1;
        }
    }
    uint32_t Ref(const std::string& s) {
        const auto it = index_.find(s);
        if (it != index_.end()) return it->second;
        const uint32_t at = uint32_t(text_.size());
        text_ += s;
        text_.push_back('\0');
        index_.emplace(s, at);
        return at;
    }
    const std::string& text() const { return text_; }

private:
    std::string text_;
    std::unordered_map<std::string, uint32_t> index_;
};

std::string PoolString(const std::string& pool, uint32_t offset, const char* which) {
    if (offset >= pool.size()) throw Error(std::string("a ") + which + " points outside its string table");
    const size_t e = pool.find('\0', offset);
    if (e == std::string::npos) throw Error(std::string("a string in the ") + which + " table is not terminated");
    return pool.substr(offset, e - offset);
}

void PutId(Bytes& b, uint64_t v) { Wr64(b, v); }

}  // namespace

std::string RpkTypeName(unsigned type) {
    if (type < kTypeCount && std::strncmp(kTypeNames[type], "type_", 5) != 0) return kTypeNames[type];
    return "type_" + std::to_string(type);
}

int RpkTypeFromName(std::string_view name) {
    const std::string n = Lower(std::string(name));
    for (unsigned i = 0; i < kTypeCount; ++i) {
        if (n == kTypeNames[i]) return int(i);
    }
    const std::string digits = StartsWith(n, "type_") ? n.substr(5) : n;
    if (!digits.empty() && digits.find_first_not_of("0123456789") == std::string::npos && digits.size() < 6) {
        const unsigned long v = std::stoul(digits);
        if (v < 0x10000) return int(v);
    }
    return -1;
}

std::string HexId(uint64_t id) {
    char buf[20];
    std::snprintf(buf, sizeof buf, "%016llx", static_cast<unsigned long long>(id));
    return buf;
}

uint64_t ParseHexId(const std::string& s) {
    std::string t = s;
    if (StartsWith(t, "0x") || StartsWith(t, "0X")) t = t.substr(2);
    if (t.empty() || t.size() > 16 || t.find_first_not_of("0123456789abcdefABCDEF") != std::string::npos) {
        throw Error("\"" + s + "\" is not a resource identifier (up to 16 hex digits)");
    }
    return std::stoull(t, nullptr, 16);
}

uint32_t RpkResource::PayloadVersion() const {
    return data.size() >= 20 && std::memcmp(data.data(), kMagic, 8) == 0 ? Rd32(&data[16]) : 0;
}

uint64_t RpkResource::PayloadId() const {
    return data.size() >= 20 && std::memcmp(data.data(), kMagic, 8) == 0 ? Rd64(&data[8]) : 0;
}

bool RpkResource::HasBlob() const {
    return (Type() == kRpkXml || Type() == kRpkBin) && data.size() >= 24 && std::memcmp(data.data(), kMagic, 8) == 0 &&
           Rd32(&data[20]) == data.size() - 24;
}

Bytes RpkResource::Blob() const {
    if (!HasBlob()) throw Error("the resource " + HexId(id) + " does not hold a plain content block");
    return Bytes(data.begin() + 24, data.end());
}

void RpkResource::SetBlob(const Bytes& content) {
    if (data.size() < 24 || std::memcmp(data.data(), kMagic, 8) != 0) {
        throw Error("the resource " + HexId(id) + " has no payload header to keep");
    }
    data.resize(20);
    Wr32(data, uint32_t(content.size()));
    data.insert(data.end(), content.begin(), content.end());
}

Bytes RpkResource::MakeBlobPayload(uint64_t id, uint32_t version, const Bytes& content) {
    Bytes b(kMagic, kMagic + 8);
    PutId(b, id);
    Wr32(b, version);
    Wr32(b, uint32_t(content.size()));
    b.insert(b.end(), content.begin(), content.end());
    return b;
}

RpkPack RpkPack::Parse(const Bytes& file) {
    RpkPack pack;
    Reader r(file, "the resource pack");
    if (file.size() < kPackHeader || std::memcmp(file.data(), kMagic, 8) != 0) throw Error("this is not a resource pack");
    r.Seek(8);
    const uint32_t name_len = r.U32();
    if (name_len != sizeof kPackName - 1 || std::memcmp(r.Take(name_len), kPackName, name_len) != 0) {
        throw Error("this is not a resource pack (no \"Resource PacK file\" text)");
    }
    const uint32_t toc_size = r.U32();
    const uint32_t toc_offset = r.U32();
    if (toc_offset != kPackHeader) throw Error("the table of contents is not right after the header, which is not supported");
    r.Seek(toc_offset);
    if (std::memcmp(r.Take(8), kMagic, 8) != 0) throw Error("the table of contents has no EVOSLITL header");
    pack.info_id = r.U64();
    pack.info_version = r.U32();
    if ((pack.info_id >> 48) != 1) throw Error("the table of contents has the wrong type");
    if (r.U32() != kInfoMarker) throw Error("the table of contents has no 0x11111111 marker");
    const uint32_t count = r.U32();
    const uint32_t alias_total = r.U32();
    const uint32_t dep_total = r.U32();
    const uint32_t name_pool_size = r.U32();
    const uint32_t source_pool_size = r.U32();
    uint32_t alias_count = 0, required_count = 0;
    if (pack.info_version >= 0x40002) {
        alias_count = r.U32();
        required_count = r.U32();
    }
    if (count > 1000000 || alias_count > 1000000 || required_count > 100000) throw Error("the table of contents has absurd counts");
    pack.root = r.U64();
    pack.name_pool.assign(reinterpret_cast<const char*>(r.Take(name_pool_size)), name_pool_size);
    pack.source_pool.assign(reinterpret_cast<const char*>(r.Take(source_pool_size)), source_pool_size);

    struct Span {
        uint32_t size, offset;
    };
    std::vector<Span> spans(count);
    uint64_t names_seen = 0, deps_seen = 0;
    pack.resources.resize(count);
    for (uint32_t i = 0; i < count; ++i) {
        RpkResource& res = pack.resources[i];
        res.id = r.U64();
        spans[i].size = r.U32();
        spans[i].offset = r.U32();
        const uint16_t nd = r.U16();
        for (uint16_t k = 0; k < nd; ++k) res.deps.push_back(r.U64());
        const uint16_t nn = r.U16();
        for (uint16_t k = 0; k < nn; ++k) res.names.push_back(PoolString(pack.name_pool, r.U32(), "name"));
        const uint16_t ns = r.U16();
        for (uint16_t k = 0; k < ns; ++k) res.sources.push_back(PoolString(pack.source_pool, r.U32(), "source path"));
        names_seen += nn;
        deps_seen += nd;
    }
    if (names_seen != alias_total || deps_seen != dep_total) throw Error("the table of contents counts do not match its entries");
    for (uint32_t i = 0; i < alias_count; ++i) {
        RpkAlias a;
        a.id = r.U64();
        a.name = PoolString(pack.name_pool, r.U32(), "name");
        pack.aliases.push_back(std::move(a));
    }
    for (uint32_t i = 0; i < required_count; ++i) pack.required.push_back(PoolString(pack.name_pool, r.U32(), "name"));
    if (r.U32() != kInfoEnd) throw Error("the table of contents has no end marker");
    if (uint64_t(toc_offset) + toc_size != r.pos()) throw Error("the table of contents size does not match its content");

    // The payloads: one after the other, filling the file to its end.
    std::vector<uint32_t> by_offset(count);
    std::iota(by_offset.begin(), by_offset.end(), 0u);
    std::stable_sort(by_offset.begin(), by_offset.end(), [&](uint32_t a, uint32_t b) { return spans[a].offset < spans[b].offset; });
    uint64_t pos = r.pos();
    for (uint32_t rank = 0; rank < count; ++rank) {
        const uint32_t i = by_offset[rank];
        if (spans[i].offset != pos) {
            throw Error("resource " + HexId(pack.resources[i].id) + " does not follow the one before it (" +
                        std::to_string(spans[i].offset) + " instead of " + std::to_string(pos) +
                        "); packs with gaps or overlaps are not supported");
        }
        if (uint64_t(spans[i].offset) + spans[i].size > file.size()) throw Error("a resource lies beyond the end of the pack");
        pack.resources[i].data.assign(file.begin() + spans[i].offset, file.begin() + spans[i].offset + spans[i].size);
        pack.resources[i].order = rank;
        pos += spans[i].size;
    }
    if (pos != file.size()) throw Error("the pack has " + std::to_string(file.size() - pos) + " bytes after its last resource");
    return pack;
}

Bytes RpkPack::Write() const {
    Pool names(name_pool), sources(source_pool);
    // Intern every string first so that the pool sizes are known before the table is laid out.
    std::vector<std::vector<uint32_t>> name_refs(resources.size()), source_refs(resources.size());
    uint64_t name_total = 0, dep_total = 0;
    for (size_t i = 0; i < resources.size(); ++i) {
        for (const std::string& n : resources[i].names) name_refs[i].push_back(names.Ref(n));
        for (const std::string& s : resources[i].sources) source_refs[i].push_back(sources.Ref(s));
        name_total += resources[i].names.size();
        dep_total += resources[i].deps.size();
    }
    std::vector<uint32_t> alias_refs, required_refs;
    for (const RpkAlias& a : aliases) alias_refs.push_back(names.Ref(a.name));
    for (const std::string& n : required) required_refs.push_back(names.Ref(n));

    const bool has_lists = info_version >= 0x40002;
    Bytes toc;
    toc.insert(toc.end(), kMagic, kMagic + 8);
    PutId(toc, info_id);
    Wr32(toc, info_version);
    Wr32(toc, kInfoMarker);
    Wr32(toc, uint32_t(resources.size()));
    Wr32(toc, uint32_t(name_total));
    Wr32(toc, uint32_t(dep_total));
    Wr32(toc, uint32_t(names.text().size()));
    Wr32(toc, uint32_t(sources.text().size()));
    if (has_lists) {
        Wr32(toc, uint32_t(aliases.size()));
        Wr32(toc, uint32_t(required.size()));
    }
    PutId(toc, root);
    toc.insert(toc.end(), names.text().begin(), names.text().end());
    toc.insert(toc.end(), sources.text().begin(), sources.text().end());

    // Where each payload goes: after the table, in the recorded order.
    std::vector<uint32_t> by_order(resources.size());
    std::iota(by_order.begin(), by_order.end(), 0u);
    std::stable_sort(by_order.begin(), by_order.end(), [&](uint32_t a, uint32_t b) { return resources[a].order < resources[b].order; });
    size_t toc_bytes = toc.size() + 4;
    for (const RpkResource& res : resources) {
        toc_bytes += 8 + 4 + 4 + 2 + 8 * res.deps.size() + 2 + 4 * res.names.size() + 2 + 4 * res.sources.size();
    }
    if (has_lists) toc_bytes += aliases.size() * 12 + required.size() * 4;
    std::vector<uint64_t> offsets(resources.size());
    uint64_t pos = kPackHeader + toc_bytes;
    for (uint32_t i : by_order) {
        offsets[i] = pos;
        pos += resources[i].data.size();
    }
    if (pos > 0xFFFFFFFFull) throw Error("the pack would be larger than 4 GB");

    for (size_t i = 0; i < resources.size(); ++i) {
        const RpkResource& res = resources[i];
        if (res.deps.size() > 0xFFFF || res.names.size() > 0xFFFF || res.sources.size() > 0xFFFF) throw Error("a resource has too many entries");
        PutId(toc, res.id);
        Wr32(toc, uint32_t(res.data.size()));
        Wr32(toc, uint32_t(offsets[i]));
        Wr16(toc, uint16_t(res.deps.size()));
        for (uint64_t d : res.deps) PutId(toc, d);
        Wr16(toc, uint16_t(res.names.size()));
        for (uint32_t o : name_refs[i]) Wr32(toc, o);
        Wr16(toc, uint16_t(res.sources.size()));
        for (uint32_t o : source_refs[i]) Wr32(toc, o);
    }
    if (has_lists) {
        for (size_t i = 0; i < aliases.size(); ++i) {
            PutId(toc, aliases[i].id);
            Wr32(toc, alias_refs[i]);
        }
        for (uint32_t o : required_refs) Wr32(toc, o);
    }
    Wr32(toc, kInfoEnd);

    Bytes out;
    out.reserve(size_t(pos));
    out.insert(out.end(), kMagic, kMagic + 8);
    Wr32(out, uint32_t(sizeof kPackName - 1));
    out.insert(out.end(), kPackName, kPackName + sizeof kPackName - 1);
    Wr32(out, uint32_t(toc.size()));
    Wr32(out, uint32_t(kPackHeader));
    out.insert(out.end(), toc.begin(), toc.end());
    for (uint32_t i : by_order) out.insert(out.end(), resources[i].data.begin(), resources[i].data.end());
    return out;
}

void RpkPack::Compact() {
    Pool names(""), sources("");
    for (const RpkResource& r : resources) {
        for (const std::string& n : r.names) names.Ref(n);
        for (const std::string& s : r.sources) sources.Ref(s);
    }
    for (const RpkAlias& a : aliases) names.Ref(a.name);
    for (const std::string& n : required) names.Ref(n);
    name_pool = names.text();
    source_pool = sources.text();
}

RpkResource* RpkPack::Find(uint64_t id) {
    for (RpkResource& r : resources) {
        if (r.id == id) return &r;
    }
    return nullptr;
}

const RpkResource* RpkPack::Find(uint64_t id) const { return const_cast<RpkPack*>(this)->Find(id); }

RpkResource* RpkPack::FindByName(std::string_view name) {
    const std::string want = Lower(GamePath(std::string(name)));
    for (RpkResource& r : resources) {
        for (const std::string& n : r.names) {
            if (Lower(GamePath(n)) == want) return &r;
        }
    }
    for (RpkResource& r : resources) {
        for (const std::string& n : r.names) {
            const std::string have = Lower(GamePath(n));
            if (have.size() > want.size() && EndsWith(have, want) && have[have.size() - want.size() - 1] == '/') return &r;
        }
    }
    return nullptr;
}

RpkResource& RpkPack::Add(RpkResource r) {
    if (Find(r.id)) throw Error("the pack already holds a resource with the identifier " + HexId(r.id));
    uint32_t next = 0;
    for (const RpkResource& e : resources) next = std::max(next, e.order + 1);
    r.order = next;
    // The table lists resources grouped by type, so a new one goes after the last of its type.
    size_t at = resources.size();
    for (size_t i = resources.size(); i-- > 0;) {
        if (resources[i].Type() <= r.Type()) {
            at = i + 1;
            break;
        }
        at = i;
    }
    return *resources.insert(resources.begin() + std::ptrdiff_t(at), std::move(r));
}

bool RpkPack::Remove(uint64_t id) {
    for (size_t i = 0; i < resources.size(); ++i) {
        if (resources[i].id == id) {
            resources.erase(resources.begin() + std::ptrdiff_t(i));
            for (RpkResource& r : resources) r.deps.erase(std::remove(r.deps.begin(), r.deps.end(), id), r.deps.end());
            aliases.erase(std::remove_if(aliases.begin(), aliases.end(), [&](const RpkAlias& a) { return a.id == id; }), aliases.end());
            return true;
        }
    }
    return false;
}

}  // namespace dcmod
