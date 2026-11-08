// SPDX-License-Identifier: GPL-2.0-or-later
#include "atrac9enc.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

extern "C" {
#include "libatrac9.h"
#include "tables.h"
#include "huffCodes.h"
}

namespace dcmod {

namespace {

constexpr int kN = 256;  // samples per frame and coefficients per block (48 kHz streams)

// Writes bits, most significant first, the order the decoder reads.
class BitWriter {
public:
    void Put(uint32_t value, int bits) {
        for (int i = bits - 1; i >= 0; --i) {
            if ((bit_ & 7) == 0) bytes_.push_back(0);
            if ((value >> i) & 1) bytes_.back() |= uint8_t(0x80 >> (bit_ & 7));
            ++bit_;
        }
    }
    size_t bits() const { return bit_; }
    void Align() { bit_ = (bit_ + 7) & ~size_t(7); }
    const Bytes& bytes() const { return bytes_; }

private:
    Bytes bytes_;
    size_t bit_ = 0;
};

// The decoder's own choice of a spectrum codebook set per quantization unit (unpack.c), from the scale factors.
void CodebookSets(const int* sf_in, int units, int* set) {
    int sf[32];
    std::memcpy(sf, sf_in, sizeof(int) * size_t(units));
    for (int i = 0; i < units; ++i) set[i] = 0;
    if (units <= 1) return;
    sf[units] = sf[units - 1];
    int avg = 0;
    if (units > 12) {
        for (int i = 0; i < 12; ++i) avg += sf[i];
        avg = (avg + 6) / 12;
    }
    for (int i = 8; i < units; ++i) {
        const int prev = sf[i - 1], next = sf[i + 1];
        const int min_sf = std::min(prev, next);
        if (sf[i] - min_sf >= 3 || sf[i] - prev + sf[i] - next >= 3) set[i] = 1;
    }
    for (int i = 12; i < units; ++i) {
        if (set[i] == 0) {
            const int min_sf = std::min(sf[i - 1], sf[i + 1]);
            if (sf[i] - min_sf >= 2 && sf[i] >= avg - (QuantUnitToCoeffCount[i] == 16 ? 1 : 0)) set[i] = 1;
        }
    }
}

struct Trial {
    int bc = 0, g = 0, len = 0, lo = 0, hi = 0;
    bool ok = false;
    size_t bits = 0;
    double error = 0;
    Bytes data;
};

// One mono block: `x` are the 256 coefficients in the decoder's scale. `band_count` 3..18 and the gradient value `g` set the
// resolution; `not_first` is 1 for every frame of a superframe but the first.
Trial EncodeBlock(const double* x, int band_count, int g, bool not_first, size_t budget_bits) {
    Trial t;
    const auto fail = [&](int) { return t; };
    const int units = BandToQuantUnitCount[band_count];
    int sf[32] = {0}, prec[32] = {0};
    for (int i = 0; i < units; ++i) {
        double peak = 0;
        for (int c = QuantUnitToCoeffIndex[i]; c < QuantUnitToCoeffIndex[i + 1]; ++c) peak = std::max(peak, std::fabs(x[c]));
        int s = 0;
        while (s < 31 && SpectrumScale[s] < peak) ++s;
        sf[i] = s;
    }
    for (int i = 0; i < units; ++i) {
        prec[i] = std::max(1, sf[i] - g);
        if (prec[i] > 15) return fail(1);  // the fine part of the spectrum is not written
    }
    int set[32];
    CodebookSets(sf, units, set);
    BitWriter bw;
    bw.Put(not_first ? 1 : 0, 1);
    bw.Put(0, 1);                  // band parameters follow
    bw.Put(uint32_t(band_count - 3), 4);
    bw.Put(0, 1);                  // no band extension
    bw.Put(0, 2);                  // gradient mode 0
    bw.Put(0, 6);                  // start unit
    bw.Put(0, 6);                  // end unit - 1
    bw.Put(uint32_t(g), 5);
    bw.Put(uint32_t(g), 5);
    bw.Put(0, 4);                  // boundary
    bw.Put(0, 1);                  // no extension data
    // scale factors, fixed length
    int lo = 31, hi = 0;
    for (int i = 0; i < units; ++i) lo = std::min(lo, sf[i]), hi = std::max(hi, sf[i]);
    int len = 2;
    while (len < 5 && hi - lo >= (1 << len)) ++len;
    const int base = len < 5 ? lo : 0;
    bw.Put(1, 2);
    bw.Put(uint32_t(len - 2), 2);
    if (len < 5) bw.Put(uint32_t(base), 5);
    for (int i = 0; i < units; ++i) bw.Put(uint32_t(sf[i] - base), len);
    // spectra
    double err = 0;
    for (int c = QuantUnitToCoeffIndex[units]; c < kN; ++c) err += x[c] * x[c];
    for (int i = 0; i < units; ++i) {
        const int count = QuantUnitToCoeffCount[i], at = QuantUnitToCoeffIndex[i], precision = prec[i] + 1;
        const double scale = SpectrumScale[sf[i]], step = QuantizerStepSize[prec[i]];
        const int qmax = (1 << prec[i]) - 1, qmin = -(1 << prec[i]);
        int q[16];
        for (int k = 0; k < count; ++k) {
            int v = int(std::lround(x[at + k] / scale / step));
            v = std::max(qmin, std::min(qmax, v));
            q[k] = v;
        }
        if (precision <= MaxHuffPrecision[0]) {
            const HuffmanCodebook* cb = &HuffmanSpectrum[set[i]][precision][QuantUnitToCodebookIndex[i]];
            if (cb->Length == 0) return fail(2);
            const int mask = (1 << cb->ValueBits) - 1;
            for (int j = 0; j < count; j += cb->ValueCount) {
                // a group the table has no symbol for (a table of the low units has none for all zeros, say) takes the
                // symbol that is nearest
                uint32_t value = 0;
                for (int m = 0; m < cb->ValueCount; ++m) value |= uint32_t(q[j + m] & mask) << (m * cb->ValueBits);
                if (!(value < uint32_t(cb->Length) && cb->Bits[value] != 0)) {
                    long best_d = -1;
                    uint32_t best_v = 0;
                    for (uint32_t cand = 0; cand < uint32_t(cb->Length); ++cand) {
                        if (cb->Bits[cand] == 0) continue;
                        long d = 0;
                        for (int m = 0; m < cb->ValueCount; ++m) {
                            int cv = int((cand >> (m * cb->ValueBits)) & uint32_t(mask));
                            if (cv >= (1 << (cb->ValueBits - 1))) cv -= 1 << cb->ValueBits;
                            d += long(cv - q[j + m]) * long(cv - q[j + m]);
                        }
                        if (best_d < 0 || d < best_d) best_d = d, best_v = cand;
                    }
                    if (best_d < 0) return fail(3);
                    value = best_v;
                    for (int m = 0; m < cb->ValueCount; ++m) {
                        int cv = int((value >> (m * cb->ValueBits)) & uint32_t(mask));
                        if (cv >= (1 << (cb->ValueBits - 1))) cv -= 1 << cb->ValueBits;
                        q[j + m] = cv;
                    }
                }
                bw.Put(cb->Codes[value], cb->Bits[value]);
            }
        } else {
            for (int k = 0; k < count; ++k) bw.Put(uint32_t(q[k]) & ((1u << precision) - 1), precision);
        }
        for (int k = 0; k < count; ++k) {
            const double d = x[at + k] - q[k] * step * scale;
            err += d * d;
        }
        if (bw.bits() > budget_bits) return fail(4);
    }
    bw.Align();
    if (bw.bits() > budget_bits) return fail(5);
    t.bc = band_count, t.g = g, t.len = len, t.lo = lo, t.hi = hi;
    t.ok = true;
    t.bits = bw.bits();
    t.error = err;
    t.data = bw.bytes();
    return fail(6);
}

// The best block for a budget: every band count and gradient value is tried, the least error wins.
Bytes BestBlock(const double* x, bool not_first, size_t budget_bytes, Trial* chosen = nullptr) {
    Trial best;
    for (int bc = 18; bc >= 3; --bc) {
        for (int g = 0; g <= 31; ++g) {
            Trial t = EncodeBlock(x, bc, g, not_first, budget_bytes * 8);
            if (!t.ok) continue;
            if (!best.ok || t.error < best.error) best = std::move(t);
            break;  // a higher gradient value only lowers the resolution
        }
    }
    if (!best.ok) {
        // silence: the smallest block there is
        double zero[kN] = {0};
        best = EncodeBlock(zero, 3, 0, not_first, 1u << 20);
    }
    if (chosen) *chosen = best;
    return best.data;
}

}  // namespace

At9Config At9Config::Parse(const uint8_t* c) {
    At9Config cfg;
    std::memcpy(cfg.data, c, 4);
    const uint32_t v = (uint32_t(c[0]) << 24) | (uint32_t(c[1]) << 16) | (uint32_t(c[2]) << 8) | c[3];
    if (c[0] != 0xFE) throw Error("not an ATRAC9 configuration (it starts with FE)");
    cfg.sample_rate_index = (v >> 20) & 15;
    cfg.channel_config = (v >> 17) & 7;
    cfg.frame_bytes = ((v >> 5) & 0x7FF) + 1;
    cfg.superframe_index = (v >> 3) & 3;
    if (cfg.sample_rate_index != 7 && cfg.sample_rate_index != 9) throw Error("only 48 kHz ATRAC9 streams can be written");
    if (cfg.channel_config > 1) throw Error("only mono streams and streams of two mono blocks can be written");
    cfg.blocks = cfg.channel_config == 0 ? 1 : 2;
    cfg.channels = cfg.blocks;
    cfg.frames_per_superframe = 1u << cfg.superframe_index;
    cfg.superframe_bytes = cfg.frame_bytes << cfg.superframe_index;
    cfg.frame_samples = kN;
    cfg.sample_rate = 48000;
    return cfg;
}

Bytes At9Encode(const At9Config& cfg, const std::vector<std::vector<float>>& pcm, size_t* samples) {
    if (pcm.size() != cfg.channels) throw Error("the audio has " + std::to_string(pcm.size()) + " channels, the stream " + std::to_string(cfg.channels));
    // the decoder fills its tables when it is set up
    void* h = Atrac9GetHandle();
    if (Atrac9InitDecoder(h, const_cast<unsigned char*>(cfg.data)) != 0) throw Error("the ATRAC9 configuration was refused");
    Atrac9ReleaseHandle(h);

    const size_t len = pcm[0].size();
    // the decoder's first output block is lost to the overlap: one silent block goes in front
    const size_t blocks = (len + kN - 1) / kN + 1;
    size_t frames = blocks + 1;
    frames = (frames + cfg.frames_per_superframe - 1) / cfg.frames_per_superframe * cfg.frames_per_superframe;
    std::vector<std::vector<double>> x(cfg.channels, std::vector<double>((frames + 1) * kN, 0.0));
    for (unsigned c = 0; c < cfg.channels; ++c) {
        for (size_t i = 0; i < len; ++i) x[c][kN + i] = pcm[c][i];
    }
    // windowed MDCT with the kernel the decoder's inverse needs (checked to rebuild the signal exactly)
    static std::vector<float> kernel;
    static std::vector<double> window;
    if (kernel.empty()) {
        kernel.resize(size_t(2 * kN) * kN);
        for (int n = 0; n < 2 * kN; ++n) {
            for (int k = 0; k < kN; ++k) kernel[size_t(n) * kN + size_t(k)] = float(std::cos(M_PI / kN * (n + 0.5 + kN / 2.0) * (k + 0.5)));
        }
        window.resize(2 * kN);
        for (int i = 0; i < kN; ++i) {
            window[size_t(i)] = (std::sin(((i + 0.5) / kN - 0.5) * M_PI) + 1.0) * 0.5;
            window[size_t(2 * kN - 1 - i)] = window[size_t(i)];
        }
    }
    Bytes out;
    size_t sf_remaining = cfg.superframe_bytes;
    Bytes super;
    for (size_t f = 0; f < frames; ++f) {
        const unsigned in_super = unsigned(f % cfg.frames_per_superframe);
        if (in_super == 0) {
            super.clear();
            sf_remaining = cfg.superframe_bytes;
        }
        const size_t frame_budget = sf_remaining / (cfg.frames_per_superframe - in_super);
        const size_t block_budget = frame_budget / cfg.blocks;
        for (unsigned c = 0; c < cfg.blocks; ++c) {
            double z[2 * kN];
            for (int n = 0; n < 2 * kN; ++n) z[n] = x[c][f * kN + size_t(n)] * window[size_t(n)];
            double coef[kN];
            for (int k = 0; k < kN; ++k) {
                double s = 0;
                for (int n = 0; n < 2 * kN; ++n) s += z[n] * kernel[size_t(n) * kN + size_t(k)];
                coef[k] = s * (2.0 / kN);
            }
            const Bytes block = BestBlock(coef, in_super != 0, block_budget);
            super.insert(super.end(), block.begin(), block.end());
        }
        if (super.size() > cfg.superframe_bytes) throw Error("internal error: a superframe is too big");
        sf_remaining = cfg.superframe_bytes - super.size();
        if (in_super == cfg.frames_per_superframe - 1) {
            super.resize(cfg.superframe_bytes, 0);
            out.insert(out.end(), super.begin(), super.end());
        }
    }
    if (samples) *samples = frames * kN;
    return out;
}

std::vector<std::vector<float>> At9Decode(const At9Config& cfg, const uint8_t* data, size_t size) {
    void* h = Atrac9GetHandle();
    if (Atrac9InitDecoder(h, const_cast<unsigned char*>(cfg.data)) != 0) {
        Atrac9ReleaseHandle(h);
        throw Error("the ATRAC9 configuration was refused");
    }
    Atrac9CodecInfo info;
    Atrac9GetCodecInfo(h, &info);
    std::vector<std::vector<float>> out(size_t(info.channels));
    std::vector<int16_t> pcm(size_t(info.frameSamples) * size_t(info.channels));
    for (size_t pos = 0; pos + size_t(info.superframeSize) <= size; pos += size_t(info.superframeSize)) {
        int used = 0;
        for (int i = 0; i < info.framesInSuperframe; ++i) {
            int u = 0;
            const int status = Atrac9Decode(h, data + pos + size_t(used), reinterpret_cast<short*>(pcm.data()), &u);
            if (status != 0) {
                Atrac9ReleaseHandle(h);
                char where[96];
                std::snprintf(where, sizeof where, " (decoder status %08x in superframe %zu, frame %d)", unsigned(status), pos / size_t(info.superframeSize), i);
                throw Error(std::string("the ATRAC9 data is damaged") + where);
            }
            used += u;
            for (int s = 0; s < info.frameSamples; ++s) {
                for (int c = 0; c < info.channels; ++c) out[size_t(c)].push_back(pcm[size_t(s) * size_t(info.channels) + size_t(c)]);
            }
        }
    }
    Atrac9ReleaseHandle(h);
    return out;
}

}  // namespace dcmod
