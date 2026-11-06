// SPDX-License-Identifier: GPL-2.0-or-later
// dcmod audio ...: the game's FMOD sound banks (.bank), whose sample data is an FSB5 stream.
#include "atrac9enc.hpp"
#include "cli.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>

namespace dcmod {

namespace {

int Usage() {
    std::puts(
        "dcmod audio: FMOD sound banks\n"
        "\n"
        "  dcmod audio info BANK                   the streams in the bank: name, rate, channels, samples, bytes\n"
        "  dcmod audio extract BANK -o DIR [NAME]  write the encoded streams (.raw) and a streams.json, or just NAME\n"
        "  dcmod audio wav BANK NAME -o FILE.wav   decode a stream to a 16 bit WAV file\n"
        "  dcmod audio import BANK NAME FILE.wav -o OUT   encode FILE.wav (48 kHz, 16 bit or float, mono or stereo) as ATRAC9\n"
        "                                          in the stream's own configuration and put it in place of the stream NAME;\n"
        "                                          the bank is rebuilt, so the new sound may be longer or shorter\n"
        "  dcmod audio replace BANK NAME FILE -o OUT   put FILE (an encoded stream: the superframes of 'extract') in place of NAME\n"
        "\n"
        "BANK is a file or @path in the game's archive (for example @audio/fmodstudio/music.bank). Convert other audio to\n"
        "WAV first (for example: ffmpeg -i in.mp3 -ar 48000 -sample_fmt s16 out.wav).");
    return 2;
}

struct Stream {
    std::string name;
    uint32_t rate = 0, channels = 0, samples = 0;
    size_t offset = 0, size = 0;  // inside the bank file
    size_t header = 0;            // where the stream's 8 byte header is in the bank file
    uint8_t config[4] = {0, 0, 0, 0};
    bool has_config = false;
};

struct Fsb {
    size_t at = 0, data_at = 0, data_size = 0;  // the FSB5 header, its sample data and the size of that
    uint32_t codec = 0;
    std::vector<Stream> streams;
};

const char* CodecName(uint32_t c) {
    static const char* n[] = {"none", "pcm8", "pcm16", "pcm24", "pcm32", "pcmfloat", "gcadpcm", "imaadpcm", "vag",
                              "hevag", "xma", "mpeg", "celt", "atrac9", "xwma", "vorbis", "fadpcm"};
    return c < 17 ? n[c] : "unknown";
}

Fsb ParseBank(const Bytes& d) {
    if (d.size() < 16 || std::memcmp(d.data(), "RIFF", 4) != 0) throw Error("not a bank (no RIFF header)");
    size_t at = 0;
    bool found = false;
    for (size_t i = 0; i + 4 <= d.size(); ++i) {
        if (d[i] == 'F' && std::memcmp(d.data() + i, "FSB5", 4) == 0) {
            at = i;
            found = true;
            break;
        }
    }
    if (!found) throw Error("the bank has no FSB5 sample data");
    const Bytes head(d.begin() + at, d.begin() + std::min(d.size(), at + 64));
    ByteReader r(head, "FSB5 header");
    r.Take(4);
    const uint32_t version = r.U32(), count = r.U32(), head_size = r.U32(), name_size = r.U32(), data_size = r.U32();
    Fsb out;
    out.codec = r.U32();
    out.at = at;
    const size_t base = 60 + (version == 0 ? 4 : 0);
    const size_t data_at = at + base + head_size + name_size;
    if (count > 100000 || data_at + data_size > d.size()) throw Error("damaged FSB5 header");
    out.data_at = data_at;
    out.data_size = data_size;
    static const uint32_t rates[] = {4000, 8000, 11000, 11025, 16000, 22050, 24000, 32000, 44100, 48000};
    const uint8_t* heads = d.data() + at + base;
    const uint8_t* names = heads + head_size;
    for (uint32_t i = 0; i < count; ++i) {
        uint64_t v = Rd64(heads);
        heads += 8;
        Stream s;
        s.header = size_t(heads - 8 - d.data());
        const uint32_t rate_idx = (v >> 1) & 15;
        s.rate = rate_idx < 10 ? rates[rate_idx] : 0;
        s.channels = ((v >> 5) & 1) + 1;
        s.offset = data_at + ((v >> 6) & 0xFFFFFFF) * 16;
        s.samples = uint32_t(v >> 34);
        bool extra = v & 1;
        while (extra) {
            const uint32_t e = Rd32(heads);
            if (((e >> 25) & 0x7F) == 9 && ((e >> 1) & 0xFFFFFF) >= 8) {
                std::memcpy(s.config, heads + 8, 4);
                s.has_config = s.config[0] == 0xFE;
            }
            heads += 4 + ((e >> 1) & 0xFFFFFF);
            extra = e & 1;
        }
        const uint32_t name_off = name_size ? Rd32(names + i * 4) : 0;
        s.name = name_size ? std::string(reinterpret_cast<const char*>(names + name_off)) : "stream" + std::to_string(i);
        out.streams.push_back(s);
    }
    for (size_t i = 0; i < out.streams.size(); ++i) {
        const size_t end = i + 1 < out.streams.size() ? out.streams[i + 1].offset : data_at + data_size;
        out.streams[i].size = end - out.streams[i].offset;
    }
    return out;
}

struct Wav {
    unsigned rate = 0;
    std::vector<std::vector<float>> pcm;  // per channel, in the range of 16 bit samples
};

Wav ReadWav(const Bytes& d) {
    if (d.size() < 44 || std::memcmp(d.data(), "RIFF", 4) != 0 || std::memcmp(d.data() + 8, "WAVE", 4) != 0) throw Error("not a WAV file");
    unsigned format = 0, channels = 0, bits = 0;
    Wav w;
    const uint8_t* data = nullptr;
    size_t data_size = 0;
    for (size_t p = 12; p + 8 <= d.size();) {
        const uint32_t size = Rd32(&d[p + 4]);
        const uint8_t* body = &d[p + 8];
        if (p + 8 + size > d.size()) break;
        if (std::memcmp(&d[p], "fmt ", 4) == 0 && size >= 16) {
            format = Rd16(body);
            channels = Rd16(body + 2);
            w.rate = Rd32(body + 4);
            bits = Rd16(body + 14);
            if (format == 0xFFFE && size >= 26) format = Rd16(body + 24);  // WAVE_FORMAT_EXTENSIBLE: the sub format
        } else if (std::memcmp(&d[p], "data", 4) == 0) {
            data = body;
            data_size = size;
        }
        p += 8 + size + (size & 1);
    }
    if (!data || !channels) throw Error("the WAV file has no audio");
    const bool is_float = format == 3 && bits == 32;
    if (!(format == 1 && (bits == 16 || bits == 24 || bits == 32 || bits == 8)) && !is_float) throw Error("the WAV file must hold PCM (8, 16, 24 or 32 bit) or 32 bit float samples");
    const size_t bytes = bits / 8, frames = data_size / (bytes * channels);
    w.pcm.assign(channels, std::vector<float>(frames));
    for (size_t i = 0; i < frames; ++i) {
        for (unsigned c = 0; c < channels; ++c) {
            const uint8_t* q = data + (i * channels + c) * bytes;
            double v;
            if (is_float) {
                float f;
                std::memcpy(&f, q, 4);
                v = double(f) * 32768.0;
            } else if (bits == 8) {
                v = (int(q[0]) - 128) * 256.0;
            } else if (bits == 16) {
                v = int16_t(Rd16(q));
            } else if (bits == 24) {
                v = double(int32_t((uint32_t(q[0]) << 8) | (uint32_t(q[1]) << 16) | (uint32_t(q[2]) << 24)) >> 8) / 256.0;
            } else {
                v = double(int32_t(Rd32(q))) / 65536.0;
            }
            w.pcm[c][i] = float(v);
        }
    }
    return w;
}

Bytes MakeWav(const std::vector<std::vector<float>>& pcm, unsigned rate) {
    const size_t channels = pcm.size(), frames = pcm.empty() ? 0 : pcm[0].size();
    Bytes out{'R', 'I', 'F', 'F'};
    Wr32(out, uint32_t(36 + frames * channels * 2));
    for (const char* t : {"WAVE", "fmt "}) out.insert(out.end(), t, t + 4);
    Wr32(out, 16);
    Wr16(out, 1);
    Wr16(out, uint16_t(channels));
    Wr32(out, rate);
    Wr32(out, uint32_t(rate * channels * 2));
    Wr16(out, uint16_t(channels * 2));
    Wr16(out, 16);
    out.insert(out.end(), {'d', 'a', 't', 'a'});
    Wr32(out, uint32_t(frames * channels * 2));
    for (size_t i = 0; i < frames; ++i) {
        for (size_t c = 0; c < channels; ++c) Wr16(out, uint16_t(int16_t(std::max(-32768.f, std::min(32767.f, std::round(pcm[c][i]))))));
    }
    return out;
}

// The bank with the stream `which` holding `data` (a whole number of superframes) and `samples` samples. The data of the
// other streams moves as far as it has to; the sample headers, the FSB5 header and the sizes of the chunks around it follow.
Bytes RebuildBank(const Bytes& bank, const Fsb& fsb, size_t which, const Bytes& data, uint32_t samples) {
    std::vector<size_t> order(fsb.streams.size());
    for (size_t i = 0; i < order.size(); ++i) order[i] = i;
    std::sort(order.begin(), order.end(), [&](size_t a, size_t b) { return fsb.streams[a].offset < fsb.streams[b].offset; });
    Bytes out(bank.begin(), bank.begin() + std::ptrdiff_t(fsb.data_at));
    size_t pos = 0;  // in the new data
    std::vector<size_t> new_off(fsb.streams.size());
    for (const size_t i : order) {
        while (pos % 32) ++pos, out.push_back(0);
        const Stream& s = fsb.streams[i];
        new_off[i] = pos;
        if (i == which) out.insert(out.end(), data.begin(), data.end()), pos += data.size();
        else out.insert(out.end(), bank.begin() + std::ptrdiff_t(s.offset), bank.begin() + std::ptrdiff_t(s.offset + s.size)), pos += s.size;
    }
    while (pos % 32) ++pos, out.push_back(0);
    const size_t old_end = fsb.data_at + fsb.data_size;
    const size_t delta_pos = pos;
    out.insert(out.end(), bank.begin() + std::ptrdiff_t(old_end), bank.end());
    for (size_t i = 0; i < fsb.streams.size(); ++i) {
        const Stream& s = fsb.streams[i];
        uint64_t v = Rd64(&out[s.header]);
        v &= ~(uint64_t(0xFFFFFFF) << 6);
        v |= uint64_t(new_off[i] / 16) << 6;
        if (i == which) {
            v &= (uint64_t(1) << 34) - 1;
            v |= uint64_t(samples) << 34;
        }
        for (int b = 0; b < 8; ++b) out[s.header + size_t(b)] = uint8_t(v >> (8 * b));
    }
    Put32(out, fsb.at + 20, uint32_t(delta_pos));  // the size of the sample data
    // the chunks around the FSB5: every one that holds it changes size by the same amount
    const long delta = long(delta_pos) - long(fsb.data_size);
    std::vector<size_t> sizes;
    std::function<bool(size_t, size_t)> walk = [&](size_t p, size_t end) -> bool {
        while (p + 8 <= end) {
            const uint32_t size = Rd32(&bank[p + 4]);
            const size_t body = p + 8, chunk_end = body + size;
            if (chunk_end > end) return false;
            if (fsb.at >= body && fsb.at < chunk_end) {
                sizes.push_back(p + 4);
                if (std::memcmp(&bank[p], "LIST", 4) == 0 || std::memcmp(&bank[p], "RIFF", 4) == 0) {
                    walk(body + 4, chunk_end);
                }
                return true;
            }
            p = chunk_end + (size & 1);
        }
        return false;
    };
    walk(0, bank.size());
    for (const size_t at : sizes) Put32(out, at, uint32_t(long(Rd32(&bank[at])) + delta));
    return out;
}

Bytes LoadBank(const Args& args, const std::string& spec) {
    if (spec.empty() || spec[0] != '@') return ReadFile(spec);
    auto archive = OpenGame(args);
    const IndexEntry* e = archive->Find(GamePath(spec.substr(1)));
    if (!e) throw Error("not in the archive: " + spec);
    return archive->Read(*e);
}

}  // namespace

int RunAudio(const std::vector<std::string>& argv) {
    if (argv.empty()) return Usage();
    const std::string cmd = argv[0];
    const Args args = ParseArgs(argv, {"--update", "--base", "--game", "-o", "--output"});
    if (args.positional.size() < 2) return Usage();
    const Bytes bank = LoadBank(args, args.positional[1]);
    const Fsb fsb = ParseBank(bank);

    if (cmd == "info") {
        std::printf("codec %s, %zu streams\n", CodecName(fsb.codec), fsb.streams.size());
        for (const Stream& s : fsb.streams) {
            std::printf("%-40s %5u Hz  %u ch  %9u samples  %9zu bytes\n", s.name.c_str(), s.rate, s.channels, s.samples, s.size);
        }
        return 0;
    }
    if (cmd == "extract") {
        const std::string out = args.Get("-o", args.Get("--output"));
        if (out.empty()) throw Error("extract needs -o DIR");
        const std::string only = args.positional.size() > 2 ? args.positional[2] : "";
        std::string json = "{\"codec\":\"" + std::string(CodecName(fsb.codec)) + "\",\"streams\":[";
        size_t n = 0;
        for (const Stream& s : fsb.streams) {
            if (!only.empty() && s.name != only) continue;
            WriteFile(fs::path(out) / (s.name + ".raw"), bank.data() + s.offset, s.size);
            json += (n++ ? "," : "") + std::string("{\"name\":\"") + s.name + "\",\"rate\":" + std::to_string(s.rate) +
                    ",\"channels\":" + std::to_string(s.channels) + ",\"samples\":" + std::to_string(s.samples) + "}";
        }
        WriteText(fs::path(out) / "streams.json", json + "]}\n");
        std::printf("wrote %zu streams to %s\n", n, out.c_str());
        return 0;
    }
    const auto find_stream = [&](const std::string& name) -> size_t {
        for (size_t i = 0; i < fsb.streams.size(); ++i) {
            if (fsb.streams[i].name == name) return i;
        }
        throw Error("no stream named " + name);
    };
    const auto config_of = [&](const Stream& st) {
        if (fsb.codec != 13 || !st.has_config) throw Error("the stream is not ATRAC9 with a configuration the kit knows");
        return At9Config::Parse(st.config);
    };
    if (cmd == "replace") {
        const std::string out = args.Get("-o", args.Get("--output"));
        if (out.empty() || args.positional.size() < 4) throw Error("replace needs BANK NAME FILE -o OUT");
        const size_t i = find_stream(args.positional[2]);
        const Bytes data = ReadFile(args.positional[3]);
        WriteFile(out, RebuildBank(bank, fsb, i, data, fsb.streams[i].samples));
        std::printf("replaced %s (%zu bytes instead of %zu); written to %s\n", fsb.streams[i].name.c_str(), data.size(), fsb.streams[i].size, out.c_str());
        return 0;
    }
    if (cmd == "wav") {
        const std::string out = args.Get("-o", args.Get("--output"));
        if (out.empty() || args.positional.size() < 3) throw Error("wav needs BANK NAME -o FILE.wav");
        const Stream& st = fsb.streams[find_stream(args.positional[2])];
        const At9Config cfg = config_of(st);
        auto pcm = At9Decode(cfg, bank.data() + st.offset, st.size);
        // the decoder's first 256 samples are its start up: drop them, and stop at the stream's length
        for (auto& ch : pcm) {
            ch.erase(ch.begin(), ch.begin() + std::min<size_t>(256, ch.size()));
            if (ch.size() > st.samples) ch.resize(st.samples);
        }
        WriteFile(out, MakeWav(pcm, cfg.sample_rate));
        std::printf("wrote %s (%zu samples, %zu channels)\n", out.c_str(), pcm.empty() ? size_t(0) : pcm[0].size(), pcm.size());
        return 0;
    }
    if (cmd == "import") {
        const std::string out = args.Get("-o", args.Get("--output"));
        if (out.empty() || args.positional.size() < 4) throw Error("import needs BANK NAME FILE.wav -o OUT");
        const size_t i = find_stream(args.positional[2]);
        const Stream& st = fsb.streams[i];
        const At9Config cfg = config_of(st);
        Wav w = ReadWav(ReadFile(args.positional[3]));
        if (w.rate != cfg.sample_rate) throw Error("the WAV file is " + std::to_string(w.rate) + " Hz, the stream " + std::to_string(cfg.sample_rate) + " Hz; convert it (ffmpeg -i in.wav -ar 48000 out.wav)");
        if (w.pcm.size() != cfg.channels) {
            std::vector<std::vector<float>> fixed(cfg.channels);
            for (unsigned c = 0; c < cfg.channels; ++c) {
                if (cfg.channels == 1) {
                    fixed[0].assign(w.pcm[0].size(), 0.f);
                    for (const auto& ch : w.pcm) {
                        for (size_t k = 0; k < ch.size(); ++k) fixed[0][k] += ch[k] / float(w.pcm.size());
                    }
                } else {
                    fixed[c] = w.pcm[std::min<size_t>(c, w.pcm.size() - 1)];
                }
            }
            w.pcm = fixed;
        }
        const size_t length = w.pcm[0].size();
        size_t produced = 0;
        const Bytes data = At9Encode(cfg, w.pcm, &produced);
        WriteFile(out, RebuildBank(bank, fsb, i, data, uint32_t(length)));
        std::printf("imported %zu samples as %zu bytes of ATRAC9 (the stream had %zu); written to %s\n", length, data.size(), st.size, out.c_str());
        return 0;
    }
    return Usage();
}

}  // namespace dcmod
