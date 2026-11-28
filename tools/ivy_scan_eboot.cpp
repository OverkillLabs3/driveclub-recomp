// Scans PS4 SELF/ELF binaries (eboot.bin + *.prx) using Zydis and PT_GNU_EH_FRAME (.eh_frame_hdr)
// to report EVERY instruction unsupported on Intel Ivy Bridge (and check for any unexpected ISA extensions!).
#include <Zydis/Zydis.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

using Bytes = std::vector<uint8_t>;

template <typename T>
T Rd(const Bytes& d, uint64_t off) {
    T v{};
    if (off + sizeof(T) <= d.size()) std::memcpy(&v, d.data() + off, sizeof(T));
    return v;
}

Bytes ReadFile(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return {};
    in.seekg(0, std::ios::end);
    const auto sz = in.tellg();
    in.seekg(0);
    Bytes b(static_cast<size_t>(sz));
    if (sz > 0) in.read(reinterpret_cast<char*>(b.data()), sz);
    return b;
}

struct Ph {
    uint32_t type = 0, flags = 0;
    uint64_t offset = 0, vaddr = 0, filesz = 0, memsz = 0;
};

struct ElfImage {
    Bytes image;
    uint64_t entry = 0;
    std::vector<Ph> ph;
    Ph eh_frame_hdr{};
    bool has_eh = false;
};

bool ParseSelfToImage(const Bytes& data, ElfImage* out) {
    if (data.size() < 64) return false;
    static const uint8_t self_magic[4] = {'O', 0x15, '=', 0x1d};
    static const uint8_t elf_magic[4] = {0x7f, 'E', 'L', 'F'};
    const bool plain = std::memcmp(data.data(), elf_magic, 4) == 0;
    if (!plain && std::memcmp(data.data(), self_magic, 4) != 0) return false;

    const uint16_t scnt = plain ? 0 : Rd<uint16_t>(data, 24);
    const uint64_t base = plain ? 0 : 32 + uint64_t(scnt) * 32;
    if (base + 64 > data.size() || std::memcmp(data.data() + base, elf_magic, 4) != 0) return false;

    out->entry = Rd<uint64_t>(data, base + 24);
    const uint64_t phoff = Rd<uint64_t>(data, base + 32);
    const uint16_t phnum = Rd<uint16_t>(data, base + 56);
    uint64_t max_file_end = 0, max_vaddr_end = 0;
    for (uint16_t i = 0; i < phnum; ++i) {
        const uint64_t at = base + phoff + uint64_t(i) * 56;
        Ph p{};
        p.type = Rd<uint32_t>(data, at);
        p.flags = Rd<uint32_t>(data, at + 4);
        p.offset = Rd<uint64_t>(data, at + 8);
        p.vaddr = Rd<uint64_t>(data, at + 16);
        p.filesz = Rd<uint64_t>(data, at + 32);
        p.memsz = Rd<uint64_t>(data, at + 40);
        out->ph.push_back(p);
        if (p.type == 1) {
            max_file_end = std::max(max_file_end, p.offset + p.filesz);
            max_vaddr_end = std::max(max_vaddr_end, p.vaddr + p.memsz);
        }
        if (p.type == 0x6474e550u) {
            out->eh_frame_hdr = p;
            out->has_eh = true;
        }
    }

    Bytes elf(static_cast<size_t>(max_file_end), 0);
    if (plain) {
        for (const auto& p : out->ph) {
            if (p.type == 1 && p.filesz && p.offset + p.filesz <= data.size()) {
                std::memcpy(elf.data() + p.offset, data.data() + p.offset, static_cast<size_t>(p.filesz));
            }
        }
    } else {
        for (uint16_t i = 0; i < scnt; ++i) {
            const uint64_t flags = Rd<uint64_t>(data, 32 + i * 32);
            const uint64_t off = Rd<uint64_t>(data, 32 + i * 32 + 8);
            const uint64_t sz = Rd<uint64_t>(data, 32 + i * 32 + 16);
            if (!(flags & 0x800)) continue;
            const uint64_t idx = (flags >> 20) & 4095;
            if (idx >= out->ph.size()) continue;
            const Ph& p = out->ph[static_cast<size_t>(idx)];
            if (p.type == 1 && p.offset + sz <= elf.size() && off + sz <= data.size()) {
                std::memcpy(elf.data() + p.offset, data.data() + off, static_cast<size_t>(sz));
            }
        }
    }

    out->image.assign(static_cast<size_t>(max_vaddr_end), 0);
    for (const auto& p : out->ph) {
        if (p.type == 1 && p.filesz && p.offset + p.filesz <= elf.size() && p.vaddr + p.filesz <= out->image.size()) {
            std::memcpy(out->image.data() + p.vaddr, elf.data() + p.offset, static_cast<size_t>(p.filesz));
        }
    }
    return true;
}

uint64_t ReadEncodedPtr(const Bytes& img, uint64_t* pos, uint8_t enc, uint64_t eh_hdr_vaddr) {
    if (enc == 0xFF) return 0;
    const uint64_t at = *pos;
    uint64_t val = 0;
    switch (enc & 0x0Fu) {
    case 0x03: val = Rd<uint32_t>(img, at); *pos += 4; break;
    case 0x04: val = Rd<uint64_t>(img, at); *pos += 8; break;
    case 0x0B: val = static_cast<uint64_t>(static_cast<int64_t>(Rd<int32_t>(img, at))); *pos += 4; break;
    case 0x0C: val = static_cast<uint64_t>(Rd<int64_t>(img, at)); *pos += 8; break;
    default: return 0;
    }
    switch (enc & 0x70u) {
    case 0x00: return val;
    case 0x10: return at + val;           // DW_EH_PE_pcrel
    case 0x30: return eh_hdr_vaddr + val; // DW_EH_PE_datarel
    default: return val;
    }
}

struct FuncRange {
    uint64_t start;
    uint64_t end;
};

std::vector<FuncRange> ParseEhFrameFuncs(const ElfImage& elf) {
    std::vector<FuncRange> funcs;
    if (!elf.has_eh) return funcs;
    const uint64_t hdr = elf.eh_frame_hdr.vaddr;
    if (hdr + 4 >= elf.image.size() || elf.image[hdr] != 1) return funcs;
    const uint8_t eh_enc = elf.image[hdr + 1];
    const uint8_t fde_cnt_enc = elf.image[hdr + 2];
    const uint8_t tbl_enc = elf.image[hdr + 3];
    uint64_t pos = hdr + 4;
    (void)ReadEncodedPtr(elf.image, &pos, eh_enc, hdr);
    const uint64_t fde_count = ReadEncodedPtr(elf.image, &pos, fde_cnt_enc, hdr);
    if (fde_count == 0 || fde_count > 500000) return funcs;

    for (uint64_t i = 0; i < fde_count; ++i) {
        const uint64_t init_loc = ReadEncodedPtr(elf.image, &pos, tbl_enc, hdr);
        const uint64_t fde_ptr = ReadEncodedPtr(elf.image, &pos, tbl_enc, hdr);
        if (fde_ptr + 16 > elf.image.size()) continue;
        const uint32_t fde_len = Rd<uint32_t>(elf.image, fde_ptr);
        const uint32_t cie_off = Rd<uint32_t>(elf.image, fde_ptr + 4);
        if (fde_len == 0 || cie_off == 0) continue;
        // Read CIE to get FDE pointer encoding (usually sdata4 pcrel = 0x1B on PS4)
        const uint64_t cie_ptr = (fde_ptr + 4) - cie_off;
        uint8_t fde_enc = 0x1B;
        if (cie_ptr + 16 < elf.image.size()) {
            uint64_t p = cie_ptr + 9; // len(4)+id(4)+ver(1)
            std::string aug_str;
            while (p < elf.image.size() && elf.image[p]) aug_str.push_back(char(elf.image[p++]));
            ++p;
            auto skip_leb = [&]() { while (p < elf.image.size() && (elf.image[p++] & 0x80u)) {} };
            skip_leb(); // code_align
            skip_leb(); // data_align
            skip_leb(); // ret_reg
            if (!aug_str.empty() && aug_str[0] == 'z') {
                skip_leb(); // aug_len
                for (size_t k = 1; k < aug_str.size(); ++k) {
                    if (aug_str[k] == 'R') { fde_enc = elf.image[p++]; break; }
                    if (aug_str[k] == 'L') { ++p; }
                    if (aug_str[k] == 'P') {
                        const uint8_t penc = elf.image[p++];
                        (void)ReadEncodedPtr(elf.image, &p, penc, hdr);
                    }
                }
            }
        }
        uint64_t fp = fde_ptr + 8;
        const uint64_t pc_begin = ReadEncodedPtr(elf.image, &fp, fde_enc, hdr);
        const uint64_t pc_range = ReadEncodedPtr(elf.image, &fp, fde_enc & 0x0Fu, hdr);
        if (pc_begin < elf.image.size() && pc_range > 0 && pc_begin + pc_range <= elf.image.size()) {
            funcs.push_back({pc_begin, pc_begin + pc_range});
        } else if (init_loc < elf.image.size()) {
            funcs.push_back({init_loc, init_loc + 16});
        }
    }
    return funcs;
}

bool UnsupportedOnIvy(const ZydisDecodedInstruction& inst, std::string* isa_tag) {
    switch (inst.meta.isa_set) {
    case ZYDIS_ISA_SET_BMI1: *isa_tag = "BMI1"; return true;
    case ZYDIS_ISA_SET_BMI2: *isa_tag = "BMI2"; return true;
    case ZYDIS_ISA_SET_LZCNT: *isa_tag = "LZCNT"; return true;
    case ZYDIS_ISA_SET_MOVBE: *isa_tag = "MOVBE"; return true;
    case ZYDIS_ISA_SET_SSE4A: *isa_tag = "SSE4A"; return true;
    case ZYDIS_ISA_SET_AVX2: *isa_tag = "AVX2"; return true;
    case ZYDIS_ISA_SET_FMA: *isa_tag = "FMA3"; return true;
    case ZYDIS_ISA_SET_FMA4: *isa_tag = "FMA4"; return true;
    case ZYDIS_ISA_SET_TBM: *isa_tag = "TBM"; return true;
    case ZYDIS_ISA_SET_XOP: *isa_tag = "XOP"; return true;
    case ZYDIS_ISA_SET_ADOX_ADCX: *isa_tag = "ADX"; return true;
    case ZYDIS_ISA_SET_SHA: *isa_tag = "SHA"; return true;
    default: break;
    }
    switch (inst.mnemonic) {
    case ZYDIS_MNEMONIC_LZCNT: *isa_tag = "LZCNT"; return true;
    case ZYDIS_MNEMONIC_TZCNT: *isa_tag = "BMI1"; return true;
    case ZYDIS_MNEMONIC_MOVBE: *isa_tag = "MOVBE"; return true;
    case ZYDIS_MNEMONIC_ANDN:
    case ZYDIS_MNEMONIC_BEXTR:
    case ZYDIS_MNEMONIC_BLSI:
    case ZYDIS_MNEMONIC_BLSMSK:
    case ZYDIS_MNEMONIC_BLSR: *isa_tag = "BMI1"; return true;
    case ZYDIS_MNEMONIC_EXTRQ:
    case ZYDIS_MNEMONIC_INSERTQ:
    case ZYDIS_MNEMONIC_MOVNTSD:
    case ZYDIS_MNEMONIC_MOVNTSS: *isa_tag = "SSE4A"; return true;
    default: return false;
    }
}

void ScanBinary(const fs::path& path) {
    Bytes raw = ReadFile(path);
    ElfImage elf{};
    if (!ParseSelfToImage(raw, &elf)) {
        std::printf("SKIP %s (not SELF/ELF)\n", path.string().c_str());
        return;
    }
    auto funcs = ParseEhFrameFuncs(elf);
    ZydisDecoder dec{};
    ZydisDecoderInit(&dec, ZYDIS_MACHINE_MODE_LONG_64, ZYDIS_STACK_WIDTH_64);
    ZydisFormatter fmt{};
    ZydisFormatterInit(&fmt, ZYDIS_FORMATTER_STYLE_INTEL);

    struct Stat {
        uint64_t total = 0;
        uint64_t len_lt5 = 0;
        uint64_t len_ge5 = 0;
        std::string sample_lt5;
        std::string sample_ge5;
    };
    std::map<std::string, Stat> stats;
    uint64_t total_insns = 0;

    for (const auto& f : funcs) {
        uint64_t pc = f.start;
        while (pc < f.end) {
            ZydisDecodedInstruction inst{};
            ZydisDecodedOperand ops[ZYDIS_MAX_OPERAND_COUNT]{};
            const size_t avail = std::min<size_t>(15, static_cast<size_t>(f.end - pc));
            if (!ZYAN_SUCCESS(ZydisDecoderDecodeFull(&dec, elf.image.data() + pc, avail, &inst, ops))) {
                ++pc;
                continue;
            }
            ++total_insns;
            std::string tag;
            if (UnsupportedOnIvy(inst, &tag)) {
                const char* mnem = ZydisMnemonicGetString(inst.mnemonic);
                const std::string key = tag + ":" + (mnem ? mnem : "?");
                auto& st = stats[key];
                ++st.total;
                char buf[128] = {};
                ZydisFormatterFormatInstruction(&fmt, &inst, ops, inst.operand_count_visible, buf, sizeof(buf), pc, nullptr);
                if (inst.length < 5) {
                    ++st.len_lt5;
                    if (st.sample_lt5.empty()) st.sample_lt5 = buf;
                } else {
                    ++st.len_ge5;
                    if (st.sample_ge5.empty()) st.sample_ge5 = buf;
                }
            }
            pc += inst.length;
        }
    }

    std::printf("\n=== %s (funcs=%zu, insns=%llu) ===\n", path.filename().string().c_str(),
                funcs.size(), (unsigned long long)total_insns);
    if (stats.empty()) {
        std::printf("  (none! 100%% Ivy Bridge native)\n");
    }
    for (const auto& [k, st] : stats) {
        std::printf("  %-18s total=%6llu  >=5B=%6llu  <5B=%5llu",
                    k.c_str(), (unsigned long long)st.total,
                    (unsigned long long)st.len_ge5, (unsigned long long)st.len_lt5);
        if (!st.sample_ge5.empty()) std::printf("  ex5=[%s]", st.sample_ge5.c_str());
        if (!st.sample_lt5.empty()) std::printf("  ex4=[%s]", st.sample_lt5.c_str());
        std::printf("\n");
    }
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "Usage: %s <file-or-dir> ...\n", argv[0]);
        return 1;
    }
    for (int i = 1; i < argc; ++i) {
        fs::path p(argv[i]);
        if (fs::is_directory(p)) {
            for (const auto& e : fs::recursive_directory_iterator(p)) {
                if (!e.is_regular_file()) continue;
                const auto ext = e.path().extension().string();
                if (e.path().filename() == "eboot.bin" || ext == ".prx" || ext == ".sprx") {
                    ScanBinary(e.path());
                }
            }
        } else {
            ScanBinary(p);
        }
    }
    return 0;
}
