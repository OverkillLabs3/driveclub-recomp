// SPDX-License-Identifier: GPL-2.0-or-later
// dcmod audio ...: the game's FMOD sound banks (.bank), whose sample data is an FSB5 stream.
#include "cli.hpp"

#include <cstring>

namespace dcmod {

namespace {

int Usage() {
    std::puts(
        "dcmod audio: FMOD sound banks\n"
        "\n"
        "  dcmod audio info BANK                   the streams in the bank: name, codec, rate, channels, samples, bytes\n"
        "  dcmod audio extract BANK -o DIR [NAME]  write the encoded streams (.raw) and a streams.json, or just NAME\n"
        "  dcmod audio replace BANK NAME FILE -o OUT   put FILE (an encoded stream of the same codec, not longer than the\n"
        "                                          old one) in place of the stream NAME; the bank keeps its size\n"
        "\n"
        "BANK is a file or @path in the game's archive (for example @audio/fmodstudio/music.bank). The streams are in the\n"
        "codec the bank uses (ATRAC9 on the console); encoding to it needs an external encoder.");
    return 2;
}

struct Stream {
    std::string name;
    uint32_t rate = 0, channels = 0, samples = 0;
    size_t offset = 0, size = 0;  // inside the bank file
};

struct Fsb {
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
    const size_t base = 60 + (version == 0 ? 4 : 0);
    const size_t data_at = at + base + head_size + name_size;
    if (count > 100000 || data_at + data_size > d.size()) throw Error("damaged FSB5 header");
    static const uint32_t rates[] = {4000, 8000, 11000, 11025, 16000, 22050, 24000, 32000, 44100, 48000};
    const uint8_t* heads = d.data() + at + base;
    const uint8_t* names = heads + head_size;
    for (uint32_t i = 0; i < count; ++i) {
        uint64_t v = Rd64(heads);
        heads += 8;
        Stream s;
        const uint32_t rate_idx = (v >> 1) & 15;
        s.rate = rate_idx < 10 ? rates[rate_idx] : 0;
        s.channels = ((v >> 5) & 3) + 1;
        s.offset = data_at + ((v >> 7) & 0x7FFFFFF) * 16;
        s.samples = uint32_t(v >> 34);
        bool extra = v & 1;
        while (extra) {
            const uint32_t e = Rd32(heads);
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
    if (cmd == "replace") {
        const std::string out = args.Get("-o", args.Get("--output"));
        if (out.empty() || args.positional.size() < 4) throw Error("replace needs BANK NAME FILE -o OUT");
        for (const Stream& s : fsb.streams) {
            if (s.name != args.positional[2]) continue;
            const Bytes data = ReadFile(args.positional[3]);
            if (data.size() > s.size) throw Error("the new stream is " + std::to_string(data.size()) + " bytes, the old one " + std::to_string(s.size));
            Bytes copy = bank;
            std::memset(copy.data() + s.offset, 0, s.size);
            std::memcpy(copy.data() + s.offset, data.data(), data.size());
            WriteFile(out, copy);
            std::printf("replaced %s (%zu of %zu bytes used)\n", s.name.c_str(), data.size(), s.size);
            return 0;
        }
        throw Error("no stream named " + args.positional[2]);
    }
    return Usage();
}

}  // namespace dcmod
