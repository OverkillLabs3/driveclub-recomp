// SPDX-License-Identifier: GPL-2.0-or-later
// Resource packs (.rpk): the game's container for models, textures, track data and settings.
//
// A pack is a header, a table of contents ("resource info block") and then the payload of every resource one after
// the other. Each resource has a 64 bit identifier whose top 16 bits are its type, a list of the identifiers it
// depends on, any number of names and source asset paths, and a payload that starts with "EVOSLITL", the identifier
// and a version. The table lists the resources grouped by type, while the payloads sit in a different order, so the
// order of the payloads is kept as well. Reading a pack and writing it again gives the same bytes.
#pragma once

#include "common.hpp"

namespace dcmod {

constexpr unsigned kRpkXml = 39, kRpkBin = 38, kRpkPixelBuffer = 5, kRpkTopMips = 71;

// The name of a resource type ("xml", "pixel_buffer", ...), or "type_N".
std::string RpkTypeName(unsigned type);
// The type number for a name given by RpkTypeName (or "type_N", or a plain number); -1 when unknown.
int RpkTypeFromName(std::string_view name);

struct RpkResource {
    uint64_t id = 0;
    std::vector<uint64_t> deps;
    std::vector<std::string> names;
    std::vector<std::string> sources;
    Bytes data;          // the payload exactly as stored, starting with "EVOSLITL"
    uint32_t order = 0;  // where the payload sits among the others; the lowest comes first

    unsigned Type() const { return unsigned(id >> 48); }
    uint32_t PayloadVersion() const;  // from the payload's header; 0 when there is none
    uint64_t PayloadId() const;       // the identifier written in the payload's header; 0 when there is none

    // XML (39) and binary (38) payloads are a header, a 4 byte length and the content. Blob() is the content.
    bool HasBlob() const;
    Bytes Blob() const;
    void SetBlob(const Bytes& content);  // keeps the header, writes the new length and content
    static Bytes MakeBlobPayload(uint64_t id, uint32_t version, const Bytes& content);

    std::string DisplayName() const { return names.empty() ? std::string("Unnamed") : names.front(); }
};

// An identifier that the pack lists with a name (the table has a section of these; the packs seen so far name
// their roads, vertex and index buffers "Unnamed").
struct RpkAlias {
    uint64_t id = 0;
    std::string name;
};

class RpkPack {
public:
    static RpkPack Parse(const Bytes& file);  // throws Error on anything that is not a well formed pack
    Bytes Write() const;                      // the pack as a file

    uint64_t info_id = 0;              // identifier of the table of contents itself
    uint32_t info_version = 0x40002;   // 0x40002 and later have the alias and required pack lists
    uint64_t root = 0;                 // the resource the game loads the pack for
    std::vector<RpkResource> resources;  // in the table's order
    std::vector<RpkAlias> aliases;
    std::vector<std::string> required;   // packs that have to be loaded with this one

    // The text the table's names point into. A name that is already in a pool is reused and a new one is added to
    // the end, so a pack that is written without changes keeps its exact bytes. Compact() drops names that nothing
    // uses any more.
    std::string name_pool, source_pool;
    void Compact();

    RpkResource* Find(uint64_t id);
    const RpkResource* Find(uint64_t id) const;
    // By name, any case. A bare name also matches the end of a name that has folders in it.
    RpkResource* FindByName(std::string_view name);
    // Add a resource at the end (a payload that is placed after all the others). Fails if the id is taken.
    RpkResource& Add(RpkResource r);
    bool Remove(uint64_t id);
};

std::string HexId(uint64_t id);             // 16 hex digits
uint64_t ParseHexId(const std::string& s);  // accepts "0x" and any number of digits; throws Error

}  // namespace dcmod
