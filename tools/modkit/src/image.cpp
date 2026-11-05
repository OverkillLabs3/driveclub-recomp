// SPDX-License-Identifier: GPL-2.0-or-later
#include "image.hpp"

#include <algorithm>

namespace dcmod {

namespace {

// ---- inflate (RFC 1951), after the structure of zlib's puff.c ---------------------------------------------------------

struct Huffman {
    uint16_t count[16] = {};
    uint16_t symbol[320] = {};
};

class Inflater {
public:
    Inflater(const uint8_t* p, size_t n) : p_(p), end_(p + n) {}

    Bytes Run() {
        Bytes out;
        bool last;
        do {
            last = Bits(1) != 0;
            const unsigned type = Bits(2);
            if (type == 0) {
                bit_count_ = 0;
                if (end_ - p_ < 4) throw Error("the PNG data is cut short");
                const unsigned len = p_[0] | (p_[1] << 8);
                p_ += 4;
                if (size_t(end_ - p_) < len) throw Error("the PNG data is cut short");
                out.insert(out.end(), p_, p_ + len);
                p_ += len;
            } else if (type == 1) {
                Huffman lit, dist;
                uint8_t lengths[320];
                int i = 0;
                for (; i < 144; ++i) lengths[i] = 8;
                for (; i < 256; ++i) lengths[i] = 9;
                for (; i < 280; ++i) lengths[i] = 7;
                for (; i < 288; ++i) lengths[i] = 8;
                Build(lit, lengths, 288);
                for (i = 0; i < 30; ++i) lengths[i] = 5;
                Build(dist, lengths, 30);
                Codes(out, lit, dist);
            } else if (type == 2) {
                static const uint8_t order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
                const unsigned nlen = Bits(5) + 257, ndist = Bits(5) + 1, ncode = Bits(4) + 4;
                if (nlen > 286 || ndist > 30) throw Error("the PNG data is damaged");
                uint8_t lengths[320] = {};
                for (unsigned i = 0; i < ncode; ++i) lengths[order[i]] = uint8_t(Bits(3));
                Huffman code;
                Build(code, lengths, 19);
                uint8_t all[320] = {};
                unsigned idx = 0;
                while (idx < nlen + ndist) {
                    int sym = Decode(code);
                    if (sym < 16) {
                        all[idx++] = uint8_t(sym);
                    } else {
                        unsigned rep, val = 0;
                        if (sym == 16) {
                            if (idx == 0) throw Error("the PNG data is damaged");
                            val = all[idx - 1];
                            rep = 3 + Bits(2);
                        } else if (sym == 17) {
                            rep = 3 + Bits(3);
                        } else {
                            rep = 11 + Bits(7);
                        }
                        if (idx + rep > nlen + ndist) throw Error("the PNG data is damaged");
                        while (rep--) all[idx++] = uint8_t(val);
                    }
                }
                Huffman lit, dist;
                Build(lit, all, nlen);
                Build(dist, all + nlen, ndist);
                Codes(out, lit, dist);
            } else {
                throw Error("the PNG data is damaged");
            }
        } while (!last);
        return out;
    }

private:
    unsigned Bits(int need) {
        unsigned val = bit_buf_;
        while (bit_count_ < need) {
            if (p_ >= end_) throw Error("the PNG data is cut short");
            val |= unsigned(*p_++) << bit_count_;
            bit_count_ += 8;
        }
        bit_buf_ = val >> need;
        bit_count_ -= need;
        return val & ((1u << need) - 1);
    }

    static void Build(Huffman& h, const uint8_t* lengths, unsigned n) {
        std::fill(std::begin(h.count), std::end(h.count), 0);
        for (unsigned i = 0; i < n; ++i) h.count[lengths[i]]++;
        uint16_t offs[16];
        offs[1] = 0;
        for (int len = 1; len < 15; ++len) offs[len + 1] = uint16_t(offs[len] + h.count[len]);
        for (unsigned i = 0; i < n; ++i) {
            if (lengths[i]) h.symbol[offs[lengths[i]]++] = uint16_t(i);
        }
    }

    int Decode(const Huffman& h) {
        int code = 0, first = 0, index = 0;
        for (int len = 1; len <= 15; ++len) {
            code |= int(Bits(1));
            const int count = h.count[len];
            if (code - count < first) return h.symbol[index + (code - first)];
            index += count;
            first += count;
            first <<= 1;
            code <<= 1;
        }
        throw Error("the PNG data is damaged");
    }

    void Codes(Bytes& out, const Huffman& lit, const Huffman& dist) {
        static const uint16_t lbase[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
        static const uint8_t lext[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
        static const uint16_t dbase[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
        static const uint8_t dext[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
        for (;;) {
            int sym = Decode(lit);
            if (sym < 256) {
                out.push_back(uint8_t(sym));
            } else if (sym == 256) {
                return;
            } else {
                sym -= 257;
                if (sym >= 29) throw Error("the PNG data is damaged");
                const unsigned len = lbase[sym] + Bits(lext[sym]);
                const int ds = Decode(dist);
                if (ds >= 30) throw Error("the PNG data is damaged");
                const size_t d = dbase[ds] + Bits(dext[ds]);
                if (d > out.size()) throw Error("the PNG data is damaged");
                for (unsigned i = 0; i < len; ++i) out.push_back(out[out.size() - d]);
            }
        }
    }

    const uint8_t* p_;
    const uint8_t* end_;
    unsigned bit_buf_ = 0;
    int bit_count_ = 0;
};

uint32_t Be32(const uint8_t* p) { return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3]; }

int Paeth(int a, int b, int c) {
    const int p = a + b - c, pa = std::abs(p - a), pb = std::abs(p - b), pc = std::abs(p - c);
    return pa <= pb && pa <= pc ? a : pb <= pc ? b : c;
}

// ---- block encoders ----------------------------------------------------------------------------------------------------

struct Px {
    int r, g, b, a;
};

Px At(const Image& im, unsigned x, unsigned y) {
    x = std::min(x, im.width - 1);
    y = std::min(y, im.height - 1);
    const uint8_t* p = &im.rgba[(size_t(y) * im.width + x) * 4];
    return {p[0], p[1], p[2], p[3]};
}

uint16_t Pack565(int r, int g, int b) { return uint16_t(((r * 31 + 127) / 255) << 11 | ((g * 63 + 127) / 255) << 5 | ((b * 31 + 127) / 255)); }
Px Unpack565(uint16_t c) {
    const int r = (c >> 11) & 31, g = (c >> 5) & 63, b = c & 31;
    return {(r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2), 255};
}

void Bc1Block(const Px (&p)[16], uint8_t* out) {
    Px lo = p[0], hi = p[0];
    for (const Px& q : p) {
        lo = {std::min(lo.r, q.r), std::min(lo.g, q.g), std::min(lo.b, q.b), 255};
        hi = {std::max(hi.r, q.r), std::max(hi.g, q.g), std::max(hi.b, q.b), 255};
    }
    // pull the endpoints in a little: the extremes of the box are rarely colours that occur
    const int ir = (hi.r - lo.r) / 16, ig = (hi.g - lo.g) / 16, ib = (hi.b - lo.b) / 16;
    uint16_t c0 = Pack565(hi.r - ir, hi.g - ig, hi.b - ib), c1 = Pack565(lo.r + ir, lo.g + ig, lo.b + ib);
    if (c0 < c1) std::swap(c0, c1);
    Px pal[4];
    pal[0] = Unpack565(c0);
    pal[1] = Unpack565(c1);
    if (c0 == c1) {
        pal[2] = pal[0];
        pal[3] = pal[0];
    } else {
        pal[2] = {(2 * pal[0].r + pal[1].r) / 3, (2 * pal[0].g + pal[1].g) / 3, (2 * pal[0].b + pal[1].b) / 3, 255};
        pal[3] = {(pal[0].r + 2 * pal[1].r) / 3, (pal[0].g + 2 * pal[1].g) / 3, (pal[0].b + 2 * pal[1].b) / 3, 255};
    }
    uint32_t bits = 0;
    for (int i = 15; i >= 0; --i) {
        int best = 0, best_d = 1 << 30;
        for (int k = 0; k < 4; ++k) {
            const int d = (p[i].r - pal[k].r) * (p[i].r - pal[k].r) + (p[i].g - pal[k].g) * (p[i].g - pal[k].g) + (p[i].b - pal[k].b) * (p[i].b - pal[k].b);
            if (d < best_d) {
                best_d = d;
                best = k;
            }
        }
        bits = (bits << 2) | uint32_t(best);
    }
    out[0] = uint8_t(c0);
    out[1] = uint8_t(c0 >> 8);
    out[2] = uint8_t(c1);
    out[3] = uint8_t(c1 >> 8);
    for (int i = 0; i < 4; ++i) out[4 + i] = uint8_t(bits >> (8 * i));
}

// BC7 mode 6: one subset, 7 bit endpoints with a shared low bit each, 4 bit indices.
void Bc7Block(const Px (&p)[16], uint8_t* out) {
    static const int weights[16] = {0, 4, 9, 13, 17, 21, 26, 30, 34, 38, 43, 47, 51, 55, 60, 64};
    int lo_i = 0, hi_i = 0, lo_v = 1 << 30, hi_v = -1;
    for (int i = 0; i < 16; ++i) {
        const int v = p[i].r * 3 + p[i].g * 6 + p[i].b + p[i].a * 2;
        if (v < lo_v) {
            lo_v = v;
            lo_i = i;
        }
        if (v > hi_v) {
            hi_v = v;
            hi_i = i;
        }
    }
    Px e[2] = {p[lo_i], p[hi_i]};
    int idx[16];
    auto quant = [&](const Px& c, int& pbit, int (&q)[4]) {
        const int v[4] = {c.r, c.g, c.b, c.a};
        pbit = (v[0] + v[1] + v[2] + v[3]) & 2 ? 1 : 0;
        for (int k = 0; k < 4; ++k) q[k] = std::clamp((v[k] - pbit + 1) >> 1, 0, 127);
    };
    for (int pass = 0; pass < 2; ++pass) {
        int pb[2], q[2][4];
        quant(e[0], pb[0], q[0]);
        quant(e[1], pb[1], q[1]);
        Px dec[2];
        for (int k = 0; k < 2; ++k) dec[k] = {(q[k][0] << 1) | pb[k], (q[k][1] << 1) | pb[k], (q[k][2] << 1) | pb[k], (q[k][3] << 1) | pb[k]};
        for (int i = 0; i < 16; ++i) {
            int best = 0, best_d = 1 << 30;
            for (int w = 0; w < 16; ++w) {
                const int r = (dec[0].r * (64 - weights[w]) + dec[1].r * weights[w] + 32) >> 6, g = (dec[0].g * (64 - weights[w]) + dec[1].g * weights[w] + 32) >> 6;
                const int b = (dec[0].b * (64 - weights[w]) + dec[1].b * weights[w] + 32) >> 6, a = (dec[0].a * (64 - weights[w]) + dec[1].a * weights[w] + 32) >> 6;
                const int d = (p[i].r - r) * (p[i].r - r) + (p[i].g - g) * (p[i].g - g) + (p[i].b - b) * (p[i].b - b) + (p[i].a - a) * (p[i].a - a);
                if (d < best_d) {
                    best_d = d;
                    best = w;
                }
            }
            idx[i] = best;
        }
        if (idx[0] >= 8 && pass == 0) {  // the first index has only 3 bits: swap the endpoints so that it fits
            std::swap(e[0], e[1]);
            continue;
        }
        // write
        std::memset(out, 0, 16);
        size_t bit = 0;
        const auto put = [&](unsigned v, int n) {
            for (int i = 0; i < n; ++i, ++bit) {
                if ((v >> i) & 1) out[bit >> 3] |= uint8_t(1u << (bit & 7));
            }
        };
        put(1u << 6, 7);  // mode 6: six zeros, then a one
        for (int k = 0; k < 4; ++k) {
            put(unsigned(q[0][k]), 7);
            put(unsigned(q[1][k]), 7);
        }
        put(unsigned(pb[0]), 1);
        put(unsigned(pb[1]), 1);
        for (int i = 0; i < 16; ++i) put(unsigned(i == 0 ? std::min(idx[0], 7) : idx[i]), i == 0 ? 3 : 4);
        return;
    }
}

Bytes EncodeLevel(const Image& im, const TexFormat& f) {
    const unsigned bw = (im.width + 3) / 4, bh = (im.height + 3) / 4;
    Bytes out;
    if (f.block == 1) {
        out.reserve(size_t(im.width) * im.height * 4);
        for (size_t i = 0; i < size_t(im.width) * im.height; ++i) {
            const uint8_t* p = &im.rgba[i * 4];
            if (f.id == 28) out.insert(out.end(), {p[2], p[1], p[0], p[3]});
            else out.insert(out.end(), {p[0], p[1], p[2], p[3]});
        }
        return out;
    }
    out.resize(size_t(bw) * bh * f.elem_bytes);
    for (unsigned by = 0; by < bh; ++by) {
        for (unsigned bx = 0; bx < bw; ++bx) {
            Px blk[16];
            for (unsigned i = 0; i < 16; ++i) blk[i] = At(im, bx * 4 + i % 4, by * 4 + i / 4);
            uint8_t* dst = &out[(size_t(by) * bw + bx) * f.elem_bytes];
            if (f.id == 9 || f.id == 10) Bc1Block(blk, dst);
            else Bc7Block(blk, dst);
        }
    }
    return out;
}

}  // namespace

bool IsPng(const Bytes& b) { return b.size() > 8 && std::memcmp(b.data(), "\x89PNG\r\n\x1a\n", 8) == 0; }

Image LoadPng(const Bytes& file) {
    if (!IsPng(file)) throw Error("this is not a PNG file");
    size_t pos = 8;
    unsigned w = 0, h = 0, depth = 0, color = 0;
    Bytes idat, palette, trns;
    while (pos + 12 <= file.size()) {
        const uint32_t len = Be32(&file[pos]);
        const std::string type(reinterpret_cast<const char*>(&file[pos + 4]), 4);
        if (pos + 12 + size_t(len) > file.size()) throw Error("the PNG file is cut short");
        const uint8_t* d = &file[pos + 8];
        if (type == "IHDR") {
            if (len < 13) throw Error("the PNG header is damaged");
            w = Be32(d);
            h = Be32(d + 4);
            depth = d[8];
            color = d[9];
            if (d[12] != 0) throw Error("interlaced PNG files are not supported; save without interlacing");
        } else if (type == "PLTE") {
            palette.assign(d, d + len);
        } else if (type == "tRNS") {
            trns.assign(d, d + len);
        } else if (type == "IDAT") {
            idat.insert(idat.end(), d, d + len);
        } else if (type == "IEND") {
            break;
        }
        pos += 12 + len;
    }
    if (w == 0 || h == 0 || w > 16384 || h > 16384) throw Error("the PNG size is not usable");
    if (depth != 8 && depth != 16) throw Error("only 8 and 16 bit PNG files are supported");
    unsigned channels;
    switch (color) {
        case 0: channels = 1; break;
        case 2: channels = 3; break;
        case 3: channels = 1; break;
        case 4: channels = 2; break;
        case 6: channels = 4; break;
        default: throw Error("this PNG colour type is not supported");
    }
    if (color == 3 && depth != 8) throw Error("palette PNG files must have 8 bits");
    if (idat.size() < 6) throw Error("the PNG file has no image data");
    const Bytes raw = Inflater(idat.data() + 2, idat.size() - 2).Run();  // skip the zlib header
    const size_t bpp = channels * depth / 8, stride = size_t(w) * bpp;
    if (raw.size() < (stride + 1) * h) throw Error("the PNG image data is too short");
    Bytes pix(stride * h);
    for (unsigned y = 0; y < h; ++y) {
        const uint8_t* line = &raw[(stride + 1) * y];
        uint8_t* row = &pix[stride * y];
        const uint8_t* prev = y ? &pix[stride * (y - 1)] : nullptr;
        for (size_t x = 0; x < stride; ++x) {
            const int a = x >= bpp ? row[x - bpp] : 0, b = prev ? prev[x] : 0, c = (prev && x >= bpp) ? prev[x - bpp] : 0;
            int v = line[x + 1];
            switch (line[0]) {
                case 0: break;
                case 1: v += a; break;
                case 2: v += b; break;
                case 3: v += (a + b) / 2; break;
                case 4: v += Paeth(a, b, c); break;
                default: throw Error("the PNG image data is damaged");
            }
            row[x] = uint8_t(v);
        }
    }
    Image im;
    im.width = w;
    im.height = h;
    im.rgba.resize(size_t(w) * h * 4);
    const unsigned step = depth / 8;
    for (size_t i = 0; i < size_t(w) * h; ++i) {
        const uint8_t* s = &pix[i * channels * step];
        uint8_t* d = &im.rgba[i * 4];
        const auto ch = [&](unsigned k) { return s[k * step]; };  // the high byte of 16 bit samples
        switch (color) {
            case 0: d[0] = d[1] = d[2] = ch(0), d[3] = 255; break;
            case 2: d[0] = ch(0), d[1] = ch(1), d[2] = ch(2), d[3] = 255; break;
            case 3: {
                const size_t k = ch(0);
                d[0] = k * 3 + 2 < palette.size() ? palette[k * 3] : 0;
                d[1] = k * 3 + 2 < palette.size() ? palette[k * 3 + 1] : 0;
                d[2] = k * 3 + 2 < palette.size() ? palette[k * 3 + 2] : 0;
                d[3] = k < trns.size() ? trns[k] : 255;
                break;
            }
            case 4: d[0] = d[1] = d[2] = ch(0), d[3] = ch(1); break;
            default: d[0] = ch(0), d[1] = ch(1), d[2] = ch(2), d[3] = ch(3);
        }
    }
    return im;
}

Image HalfSize(const Image& im) {
    Image out;
    out.width = std::max(1u, im.width / 2);
    out.height = std::max(1u, im.height / 2);
    out.rgba.resize(size_t(out.width) * out.height * 4);
    for (unsigned y = 0; y < out.height; ++y) {
        for (unsigned x = 0; x < out.width; ++x) {
            int sum[4] = {};
            for (unsigned dy = 0; dy < 2; ++dy) {
                for (unsigned dx = 0; dx < 2; ++dx) {
                    const Px p = At(im, x * 2 + dx, y * 2 + dy);
                    sum[0] += p.r;
                    sum[1] += p.g;
                    sum[2] += p.b;
                    sum[3] += p.a;
                }
            }
            for (int k = 0; k < 4; ++k) out.rgba[(size_t(y) * out.width + x) * 4 + size_t(k)] = uint8_t((sum[k] + 2) / 4);
        }
    }
    return out;
}

Dds ImageToDds(const PixelBuffer& pb, const Image& image) {
    const TexFormat& f = pb.Format();
    if (image.width != pb.width || image.height != pb.height) {
        throw Error("the picture is " + std::to_string(image.width) + "x" + std::to_string(image.height) + " but the texture is " + std::to_string(pb.width) + "x" + std::to_string(pb.height));
    }
    const bool ok = f.id == 9 || f.id == 10 || f.id == 35 || f.id == 36 || f.id == 1 || f.id == 2 || f.id == 28;
    if (!ok) throw Error(std::string("pictures cannot be converted to ") + f.name + " yet; supply a DDS in that format");
    Dds d;
    d.width = pb.width;
    d.height = pb.height;
    d.dxgi = f.dxgi;
    Image level = image;
    for (unsigned n = 0; n < pb.mips; ++n) {
        d.mips.push_back(EncodeLevel(level, f));
        level = HalfSize(level);
    }
    return d;
}

Dds TextureSource(const PixelBuffer& pb, const Bytes& file) { return IsPng(file) ? ImageToDds(pb, LoadPng(file)) : Dds::Parse(file); }

}  // namespace dcmod
