// SPDX-License-Identifier: GPL-2.0-or-later
// Textures: the pixel buffer resources of the packs hold PS4 tiled data. This turns them into DDS files, which image
// tools read and write, and DDS files back into pixel buffer payloads.
//
// Layout (checked against the sizes of the shipped textures): the mip levels follow each other, level n having the
// size max(1, w >> n) by max(1, h >> n). Block compressed formats count 4x4 blocks as the elements. The elements of a
// level are cut into tiles of 8x8, tiles run left to right and top to bottom, and inside a tile the 64 elements are in
// Morton (Z) order. A level is padded to whole tiles. The tile layout was first described by the DriveClubFS project
// (MIT licence); this is an independent implementation.
#pragma once

#include "common.hpp"

namespace dcmod {

struct TexFormat {
    unsigned id;
    const char* name;
    unsigned block;       // 1 for plain pixels, 4 for block compressed
    unsigned elem_bytes;  // bytes per pixel or per block
    unsigned dxgi;
};

const TexFormat* FindTexFormat(unsigned pixel_format);

struct PixelBuffer {
    uint32_t version = 0;
    uint16_t width = 0, height = 0, depth = 0, format = 0;
    uint8_t mips = 0, type = 0, usage = 0, unk = 0, start_mip = 0, unk3 = 0;
    uint64_t top_mips_id = 0;
    size_t data_offset = 0;  // in the payload: where the pixel data starts (after its 4 byte size)
    Bytes payload;           // the whole resource payload

    static PixelBuffer Parse(const Bytes& payload);  // throws Error
    const TexFormat& Format() const;                 // throws Error for formats the kit does not know
    size_t ExpectedDataSize() const;                 // size of all mips from the layout above
    Bytes Data() const { return Bytes(payload.begin() + std::ptrdiff_t(data_offset), payload.end()); }
    void SetData(const Bytes& data);                 // rewrites the size field and the data
};

// A DDS file: the header says how the mips are stored, `mips` hold each level untiled.
struct Dds {
    unsigned width = 0, height = 0, dxgi = 0;
    std::vector<Bytes> mips;
    Bytes Write() const;
    static Dds Parse(const Bytes& file);  // throws Error
};

// Pixel buffer -> DDS. The largest levels of some textures (start_mip > 0) live in a top mips resource of another pack;
// pass that resource's payload as `top` to get the whole texture, without it only a texture with all levels inside works.
Dds TexToDds(const PixelBuffer& pb, const Bytes* top = nullptr);
// DDS -> new pixel data for `pb` (the levels it holds). The DDS must have the same size and a matching format. For a texture
// with a top mips resource, `top` is the resource's payload, which is rewritten with the largest levels.
Bytes DdsToTexData(const PixelBuffer& pb, const Dds& dds, Bytes* top = nullptr);
// Where the pixel data of a top mips resource starts (after its 32 byte header), and its size field.
size_t TopMipsDataOffset(const Bytes& top);

}  // namespace dcmod
