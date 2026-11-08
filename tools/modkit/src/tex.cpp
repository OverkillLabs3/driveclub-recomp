// SPDX-License-Identifier: GPL-2.0-or-later
#include "tex.hpp"

#include <algorithm>

namespace dcmod {

namespace {

// The game's pixel format numbers (evo::renderer::PixelFormat) and the DXGI formats that match them.
const TexFormat kFormats[] = {
    {1, "RGBA_32_I_LIN", 1, 4, 28},   {2, "RGBA_32_I_SRGB", 1, 4, 29}, {3, "RGBA_64_F", 1, 8, 10},
    {4, "RGBA_128_F", 1, 16, 2},      {9, "DXT1_LIN", 4, 8, 71},       {10, "DXT1_SRGB", 4, 8, 72},
    {11, "DXT3_LIN", 4, 16, 74},      {12, "DXT3_SRGB", 4, 16, 75},    {13, "DXT5_LIN", 4, 16, 77},
    {14, "DXT5_SRGB", 4, 16, 78},     {15, "A8_UNORM", 1, 1, 65},      {19, "R8G8", 1, 2, 49},
    {27, "R8_UNORM", 1, 1, 61},       {28, "BGRA_32_I_SRGB", 1, 4, 91}, {29, "BC4_UNORM", 4, 8, 80},
    {30, "BC4_SNORM", 4, 8, 81},      {31, "BC5_UNORM", 4, 16, 83},    {32, "BC5_SNORM", 4, 16, 84},
    {33, "BC6H_UF", 4, 16, 95},       {34, "BC6H_SF", 4, 16, 96},      {35, "BC7_UNORM", 4, 16, 98},
    {36, "BC7_SRGB", 4, 16, 99}};

size_t CeilDiv(size_t a, size_t b) { return (a + b - 1) / b; }

// Position of the nth element inside an 8x8 tile: bits of n alternate x, y, x, y, x, y.
void Morton(unsigned n, unsigned& x, unsigned& y) {
    x = y = 0;
    for (unsigned bit = 0; bit < 3; ++bit) {
        x |= ((n >> (2 * bit)) & 1u) << bit;
        y |= ((n >> (2 * bit + 1)) & 1u) << bit;
    }
}

struct Level {
    size_t w, h;  // in elements
    size_t padded_bytes;
};

Level LevelOf(const PixelBuffer& pb, const TexFormat& f, unsigned n) {
    const size_t w = std::max<size_t>(1, size_t(pb.width) >> n), h = std::max<size_t>(1, size_t(pb.height) >> n);
    Level l;
    l.w = CeilDiv(w, f.block);
    l.h = CeilDiv(h, f.block);
    l.padded_bytes = CeilDiv(l.w, 8) * 8 * CeilDiv(l.h, 8) * 8 * f.elem_bytes;
    return l;
}

Bytes Untile(const uint8_t* src, const Level& l, unsigned elem) {
    Bytes out(l.w * l.h * elem);
    size_t index = 0;
    for (size_t ty = 0; ty < CeilDiv(l.h, 8); ++ty) {
        for (size_t tx = 0; tx < CeilDiv(l.w, 8); ++tx) {
            for (unsigned t = 0; t < 64; ++t, ++index) {
                unsigned cx, cy;
                Morton(t, cx, cy);
                const size_t x = tx * 8 + cx, y = ty * 8 + cy;
                if (x < l.w && y < l.h) std::memcpy(&out[(y * l.w + x) * elem], src + index * elem, elem);
            }
        }
    }
    return out;
}

Bytes Tile(const Bytes& linear, const Level& l, unsigned elem) {
    Bytes out(l.padded_bytes, 0);
    size_t index = 0;
    for (size_t ty = 0; ty < CeilDiv(l.h, 8); ++ty) {
        for (size_t tx = 0; tx < CeilDiv(l.w, 8); ++tx) {
            for (unsigned t = 0; t < 64; ++t, ++index) {
                unsigned cx, cy;
                Morton(t, cx, cy);
                const size_t x = tx * 8 + cx, y = ty * 8 + cy;
                if (x < l.w && y < l.h) std::memcpy(&out[index * elem], &linear[(y * l.w + x) * elem], elem);
            }
        }
    }
    return out;
}

}  // namespace

const TexFormat* FindTexFormat(unsigned id) {
    for (const TexFormat& f : kFormats) {
        if (f.id == id) return &f;
    }
    return nullptr;
}

PixelBuffer PixelBuffer::Parse(const Bytes& payload) {
    PixelBuffer pb;
    pb.payload = payload;
    ByteReader r(payload, "the pixel buffer");
    if (payload.size() < 20 || std::memcmp(payload.data(), "EVOSLITL", 8) != 0) throw Error("this resource is not a pixel buffer");
    r.Seek(16);
    pb.version = r.U32();
    if (pb.version < 0x20003) throw Error("pixel buffers of version " + std::to_string(pb.version) + " (older games) are not supported");
    pb.width = r.U16();
    pb.height = r.U16();
    pb.depth = r.U16();
    pb.format = r.U16();
    pb.mips = r.U8();
    pb.type = r.U8();
    pb.usage = r.U8();
    pb.unk = r.U8();
    pb.start_mip = r.U8();
    if (pb.version >= 0x20005) pb.unk3 = r.U8();
    pb.top_mips_id = r.U64();
    const uint32_t size = r.U32();
    pb.data_offset = r.pos();
    if (size != payload.size() - pb.data_offset) throw Error("the pixel data size does not match the resource");
    return pb;
}

const TexFormat& PixelBuffer::Format() const {
    const TexFormat* f = FindTexFormat(format);
    if (!f) throw Error("pixel format " + std::to_string(format) + " is not supported by the texture converter");
    return *f;
}

size_t PixelBuffer::ExpectedDataSize() const {
    const TexFormat& f = Format();
    size_t total = 0;
    for (unsigned n = 0; n < mips; ++n) total += LevelOf(*this, f, n).padded_bytes;
    return total;
}

void PixelBuffer::SetData(const Bytes& data) {
    payload.resize(data_offset - 4);
    Wr32(payload, uint32_t(data.size()));
    payload.insert(payload.end(), data.begin(), data.end());
}

size_t TopMipsDataOffset(const Bytes& top) {
    if (top.size() < 32 || std::memcmp(top.data(), "EVOSLITL", 8) != 0) throw Error("this resource is not a top mips resource");
    if (Rd32(&top[28]) != top.size() - 32) throw Error("the top mips size does not match the resource");
    return 32;
}

Dds TexToDds(const PixelBuffer& pb, const Bytes* top) {
    const TexFormat& f = pb.Format();
    if (pb.start_mip != 0 && !top) throw Error("the largest levels of this texture are stored in a top mips resource of another pack (name that pack to convert it)");
    if (pb.depth > 1 || pb.type != 0) throw Error("only plain 2D textures are supported");
    const Bytes data = pb.Data();
    size_t expect = 0, top_expect = 0;
    for (unsigned n = pb.start_mip; n < pb.mips; ++n) expect += LevelOf(pb, f, n).padded_bytes;
    for (unsigned n = 0; n < pb.start_mip; ++n) top_expect += LevelOf(pb, f, n).padded_bytes;
    if (data.size() != expect) throw Error("the pixel data does not have the size the layout predicts (" + std::to_string(data.size()) + " instead of " + std::to_string(expect) + ")");
    Dds d;
    d.width = pb.width;
    d.height = pb.height;
    d.dxgi = f.dxgi;
    if (pb.start_mip != 0) {
        const size_t at = TopMipsDataOffset(*top);
        if (top->size() - at != top_expect) throw Error("the top mips data does not have the size the layout predicts (" + std::to_string(top->size() - at) + " instead of " + std::to_string(top_expect) + ")");
        size_t pos = at;
        for (unsigned n = 0; n < pb.start_mip; ++n) {
            const Level l = LevelOf(pb, f, n);
            d.mips.push_back(Untile(top->data() + pos, l, f.elem_bytes));
            pos += l.padded_bytes;
        }
    }
    size_t pos = 0;
    for (unsigned n = pb.start_mip; n < pb.mips; ++n) {
        const Level l = LevelOf(pb, f, n);
        d.mips.push_back(Untile(data.data() + pos, l, f.elem_bytes));
        pos += l.padded_bytes;
    }
    return d;
}

Bytes DdsToTexData(const PixelBuffer& pb, const Dds& dds, Bytes* top) {
    if (pb.start_mip != 0 && !top) throw Error("the largest levels of this texture are stored in a top mips resource of another pack (name that pack to change it)");
    const TexFormat& f = pb.Format();
    if (dds.width != pb.width || dds.height != pb.height) {
        throw Error("the DDS is " + std::to_string(dds.width) + "x" + std::to_string(dds.height) + " but the texture is " + std::to_string(pb.width) + "x" + std::to_string(pb.height));
    }
    // sRGB and linear versions of a format share their layout, so only the compression has to agree.
    bool same = dds.dxgi == f.dxgi;
    for (const TexFormat& g : kFormats) {
        if (g.dxgi == dds.dxgi && g.block == f.block && g.elem_bytes == f.elem_bytes && std::string(g.name).substr(0, 4) == std::string(f.name).substr(0, 4)) same = true;
    }
    if (!same) throw Error("the DDS format (DXGI " + std::to_string(dds.dxgi) + ") does not match the texture's " + f.name + "; convert the image to that format");
    if (dds.mips.size() < pb.mips) {
        throw Error("the DDS has " + std::to_string(dds.mips.size()) + " mip levels but the texture needs " + std::to_string(pb.mips) + "; generate the mip chain when saving");
    }
    Bytes out, big;
    for (unsigned n = 0; n < pb.mips; ++n) {
        const Level l = LevelOf(pb, f, n);
        if (dds.mips[n].size() != l.w * l.h * f.elem_bytes) throw Error("mip level " + std::to_string(n) + " of the DDS has the wrong size");
        const Bytes tiled = Tile(dds.mips[n], l, f.elem_bytes);
        Bytes& dst = n < pb.start_mip ? big : out;
        dst.insert(dst.end(), tiled.begin(), tiled.end());
    }
    if (pb.start_mip != 0) {
        const size_t at = TopMipsDataOffset(*top);
        top->resize(at);
        const uint32_t size = uint32_t(big.size());
        std::memcpy(&(*top)[28], &size, 4);
        top->insert(top->end(), big.begin(), big.end());
    }
    return out;
}

Bytes Dds::Write() const {
    Bytes b{'D', 'D', 'S', ' '};
    Wr32(b, 124);
    Wr32(b, 0x1007u | 0x80000u | (mips.size() > 1 ? 0x20000u : 0u));
    Wr32(b, height);
    Wr32(b, width);
    Wr32(b, uint32_t(mips.empty() ? 0 : mips[0].size()));
    Wr32(b, 0);
    Wr32(b, uint32_t(mips.size()));
    for (int i = 0; i < 11; ++i) Wr32(b, 0);
    Wr32(b, 32);
    Wr32(b, 4);
    b.insert(b.end(), {'D', 'X', '1', '0'});
    for (int i = 0; i < 5; ++i) Wr32(b, 0);
    Wr32(b, 0x1000u | (mips.size() > 1 ? 0x400008u : 0u));
    for (int i = 0; i < 4; ++i) Wr32(b, 0);
    Wr32(b, dxgi);
    Wr32(b, 3);
    Wr32(b, 0);
    Wr32(b, 1);
    Wr32(b, 0);
    for (const Bytes& m : mips) b.insert(b.end(), m.begin(), m.end());
    return b;
}

Dds Dds::Parse(const Bytes& file) {
    if (file.size() < 128 || std::memcmp(file.data(), "DDS ", 4) != 0) throw Error("this is not a DDS file");
    const uint8_t* h = file.data() + 4;
    Dds d;
    d.height = Rd32(h + 8);
    d.width = Rd32(h + 12);
    const unsigned mip_count = std::max(1u, Rd32(h + 24));
    const uint32_t pf_flags = Rd32(h + 76);
    size_t pos = 128;
    if ((pf_flags & 4) && std::memcmp(h + 80, "DX10", 4) == 0) {
        if (file.size() < 148) throw Error("the DDS header is cut short");
        d.dxgi = Rd32(file.data() + 128);
        pos = 148;
    } else if ((pf_flags & 4) != 0) {
        const std::string cc(reinterpret_cast<const char*>(h + 80), 4);
        d.dxgi = cc == "DXT1" ? 71 : cc == "DXT3" ? 74 : cc == "DXT5" ? 77 : cc == "ATI1" || cc == "BC4U" ? 80 : cc == "ATI2" || cc == "BC5U" ? 83 : 0;
    } else if ((pf_flags & 0x41) == 0x41 && Rd32(h + 84) == 32) {
        d.dxgi = 28;  // 32 bit RGBA
    }
    if (d.dxgi == 0) throw Error("this DDS format is not supported; save it with a DX10 header, for example with texconv");
    const TexFormat* f = nullptr;
    for (const TexFormat& g : kFormats) {
        if (g.dxgi == d.dxgi) {
            f = &g;
            break;
        }
    }
    if (!f) throw Error("DXGI format " + std::to_string(d.dxgi) + " is not one the game uses");
    for (unsigned n = 0; n < mip_count; ++n) {
        const size_t w = std::max<size_t>(1, d.width >> n), hh = std::max<size_t>(1, d.height >> n);
        const size_t size = CeilDiv(w, f->block) * CeilDiv(hh, f->block) * f->elem_bytes;
        if (pos + size > file.size()) throw Error("the DDS is cut short");
        d.mips.emplace_back(file.begin() + std::ptrdiff_t(pos), file.begin() + std::ptrdiff_t(pos + size));
        pos += size;
    }
    return d;
}

}  // namespace dcmod
