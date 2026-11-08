// First-run preparation: the user's game dumps in, the runtime image out.
//
// Reads the update eboot and the game's bundled modules, converts them to the flat image the game
// runtime maps (boot-linked.bin), writes the AppContent profile (content.bin) and compiles the chosen
// patches (patches.bin). Nothing in the game folders is modified.

#include "prepare.h"

#include <algorithm>
#include <cctype>
#include <tuple>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace fs = std::filesystem;

namespace {

using Bytes = std::vector<uint8_t>;

[[noreturn]] void Fail(const std::string& message) { throw std::runtime_error(message); }

template <typename T>
T Rd(const Bytes& d, uint64_t off) {
  if (off > d.size() || sizeof(T) > d.size() - off) Fail("out of bounds read");
  T v;
  std::memcpy(&v, d.data() + off, sizeof(T));
  return v;
}

void Span(const Bytes& d, uint64_t off, uint64_t size) {
  if (off > d.size() || size > d.size() - off) Fail("out of bounds");
}

template <typename T>
void Put(Bytes& out, T v) {
  const uint8_t* p = reinterpret_cast<const uint8_t*>(&v);
  out.insert(out.end(), p, p + sizeof(T));
}

Bytes ReadFile(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) Fail("cannot read " + path.string());
  in.seekg(0, std::ios::end);
  const auto size = in.tellg();
  in.seekg(0);
  Bytes data(static_cast<size_t>(std::streamoff(size)));
  if (size > 0) in.read(reinterpret_cast<char*>(data.data()), size);
  return data;
}

void WriteFile(const fs::path& path, const Bytes& data) {
  fs::create_directories(path.parent_path());
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out) Fail("cannot write " + path.string());
  out.write(reinterpret_cast<const char*>(data.data()), std::streamsize(data.size()));
}

// ------------------------------------------------------------------------------------------
// SHA-1 and the NID hash used to name library functions
// ------------------------------------------------------------------------------------------
Bytes Sha1(const Bytes& msg) {
  uint32_t h[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
  Bytes data = msg;
  const uint64_t bits = uint64_t(msg.size()) * 8;
  data.push_back(0x80);
  while (data.size() % 64 != 56) data.push_back(0);
  for (int i = 7; i >= 0; --i) data.push_back(uint8_t(bits >> (i * 8)));
  auto rol = [](uint32_t v, int n) { return (v << n) | (v >> (32 - n)); };
  for (size_t chunk = 0; chunk < data.size(); chunk += 64) {
    uint32_t w[80];
    for (int i = 0; i < 16; ++i) {
      w[i] = uint32_t(data[chunk + i * 4]) << 24 | uint32_t(data[chunk + i * 4 + 1]) << 16 |
             uint32_t(data[chunk + i * 4 + 2]) << 8 | uint32_t(data[chunk + i * 4 + 3]);
    }
    for (int i = 16; i < 80; ++i) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
    for (int i = 0; i < 80; ++i) {
      uint32_t f, k;
      if (i < 20) { f = (b & c) | (~b & d); k = 0x5A827999; }
      else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1; }
      else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
      else { f = b ^ c ^ d; k = 0xCA62C1D6; }
      const uint32_t t = rol(a, 5) + f + e + k + w[i];
      e = d; d = c; c = rol(b, 30); b = a; a = t;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
  }
  Bytes out;
  for (int i = 0; i < 5; ++i) {
    for (int j = 3; j >= 0; --j) out.push_back(uint8_t(h[i] >> (j * 8)));
  }
  return out;
}

std::string Nid(const std::string& name) {
  static const uint8_t salt[16] = {0x51, 0x8d, 0x64, 0xa6, 0x35, 0xde, 0xd8, 0xc1,
                                   0xe6, 0xb0, 0x39, 0xb1, 0xc3, 0xe5, 0x52, 0x30};
  Bytes msg(name.begin(), name.end());
  msg.insert(msg.end(), salt, salt + 16);
  const Bytes digest = Sha1(msg);
  uint8_t eight[8];
  for (int i = 0; i < 8; ++i) eight[i] = digest[7 - i];
  static const char* table = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  for (int i = 0; i < 8; i += 3) {
    const uint32_t v = uint32_t(eight[i]) << 16 | uint32_t(i + 1 < 8 ? eight[i + 1] : 0) << 8 |
                       uint32_t(i + 2 < 8 ? eight[i + 2] : 0);
    out += table[(v >> 18) & 63];
    out += table[(v >> 12) & 63];
    if (i + 1 < 8) out += table[(v >> 6) & 63];
    if (i + 2 < 8) out += table[v & 63];
  }
  for (auto& c : out) {
    if (c == '/') c = '-';
  }
  return out;
}

std::string EncodeId(uint64_t value) {
  static const char* alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+-";
  std::string result(1, alphabet[value & 63]);
  value >>= 6;
  while (value) {
    result.insert(result.begin(), alphabet[value & 63]);
    value >>= 6;
  }
  return result;
}

// ------------------------------------------------------------------------------------------
// SELF / ELF
// ------------------------------------------------------------------------------------------
struct Ph {
  uint32_t type = 0, flags = 0;
  uint64_t offset = 0, vaddr = 0, paddr = 0, filesz = 0, memsz = 0, align = 0;
};

struct SelfFile {
  Bytes elf;
  uint64_t entry = 0;
  std::vector<Ph> ph;
};

SelfFile ParseSelf(const Bytes& data) {
  static const uint8_t magic[4] = {'O', 0x15, '=', 0x1d};
  Span(data, 0, 4);
  // Normally a PS4 SELF; an already decrypted dump may hold the plain ELF.
  static const uint8_t plain_magic[4] = {0x7f, 'E', 'L', 'F'};
  const bool plain_elf = std::memcmp(data.data(), plain_magic, 4) == 0;
  if (!plain_elf && std::memcmp(data.data(), magic, 4) != 0) {
    char head[16];
    std::snprintf(head, sizeof head, "%02X %02X %02X %02X", data[0], data[1], data[2], data[3]);
    Fail(std::string("this eboot.bin is not a PS4 SELF file (it starts with ") + head + "). Choose the folder with the version 1.28 update (its eboot.bin is the one the game ships with, not a converted or renamed file)");
  }
  const uint16_t count = plain_elf ? 0 : Rd<uint16_t>(data, 24);
  const uint64_t base = plain_elf ? 0 : 32 + uint64_t(count) * 32;
  Span(data, base, 64);
  static const uint8_t elfmagic[7] = {0x7f, 'E', 'L', 'F', 2, 1, 1};
  if (std::memcmp(data.data() + base, elfmagic, 7) != 0 || Rd<uint16_t>(data, base + 18) != 62) Fail("expected little-endian x86-64 ELF");
  SelfFile s;
  s.entry = Rd<uint64_t>(data, base + 24);
  const uint64_t phoff = Rd<uint64_t>(data, base + 32);
  const uint16_t phentsize = Rd<uint16_t>(data, base + 54), phnum = Rd<uint16_t>(data, base + 56);
  if (phentsize != 56 || phnum == 0 || phnum >= 256) Fail("unsupported program headers");
  for (unsigned i = 0; i < phnum; ++i) {
    const uint64_t at = base + phoff + uint64_t(i) * 56;
    Ph p;
    p.type = Rd<uint32_t>(data, at);
    p.flags = Rd<uint32_t>(data, at + 4);
    p.offset = Rd<uint64_t>(data, at + 8);
    p.vaddr = Rd<uint64_t>(data, at + 16);
    p.paddr = Rd<uint64_t>(data, at + 24);
    p.filesz = Rd<uint64_t>(data, at + 32);
    p.memsz = Rd<uint64_t>(data, at + 40);
    p.align = Rd<uint64_t>(data, at + 48);
    s.ph.push_back(p);
  }
  uint64_t end = 0;
  for (const auto& p : s.ph) end = std::max(end, p.offset + p.filesz);
  if (end > 512ull * 1024 * 1024) Fail("probe image exceeds 512 MiB limit");
  s.elf.assign(size_t(end), 0);
  const uint64_t header_end = phoff + uint64_t(phnum) * 56;
  Span(data, base, header_end);
  if (header_end > s.elf.size()) Fail("bad ELF header size");
  std::memcpy(s.elf.data(), data.data() + base, size_t(header_end));
  std::vector<std::pair<uint64_t, uint64_t>> covered{{0, header_end}};
  if (plain_elf) {
    for (const auto& p : s.ph) {
      if (!p.filesz) continue;
      Span(data, p.offset, p.filesz);
      std::memcpy(s.elf.data() + p.offset, data.data() + p.offset, size_t(p.filesz));
      covered.push_back({p.offset, p.offset + p.filesz});
    }
  }
  for (unsigned i = 0; i < count; ++i) {
    const uint64_t flags = Rd<uint64_t>(data, 32 + i * 32), off = Rd<uint64_t>(data, 32 + i * 32 + 8),
                   size = Rd<uint64_t>(data, 32 + i * 32 + 16), mem = Rd<uint64_t>(data, 32 + i * 32 + 24);
    if (!(flags & 0x800)) continue;
    if (flags & 10) Fail("encrypted/compressed SELF segment is unsupported");
    const uint64_t index = (flags >> 20) & 4095;
    if (index >= s.ph.size()) Fail("invalid segment index");
    const Ph& p = s.ph[size_t(index)];
    if (size != p.filesz || size != mem) Fail("unsupported blocked segment layout");
    Span(data, off, size);
    if (p.offset + size > s.elf.size()) Fail("segment outside the image");
    std::memcpy(s.elf.data() + p.offset, data.data() + off, size_t(size));
    covered.push_back({p.offset, p.offset + size});
  }
  for (const auto& p : s.ph) {
    if (!p.filesz) continue;
    bool ok = false;
    for (const auto& c : covered) ok |= c.first <= p.offset && p.offset + p.filesz <= c.second;
    if (!ok && (p.type == 1 || p.type == 2 || p.type == 0x61000000 || p.type == 0x61000010)) Fail("required segment is unavailable");
  }
  return s;
}

struct Sym {
  std::string name;
  uint8_t type = 0, binding = 0;
  uint16_t section = 0;
  uint64_t value = 0, size = 0;
};

struct Reloc {
  uint64_t target, info;
  int64_t addend;
};

struct Module {
  SelfFile self;
  std::vector<std::pair<uint64_t, uint64_t>> dyn;
  std::map<uint64_t, uint64_t> tags;
  Bytes blob;
  Bytes strings;
  std::vector<Sym> syms;
  std::vector<Reloc> relocs;
  std::map<std::string, std::pair<std::string, uint64_t>> libraries, modules;  // encoded id -> (name, version)
  std::string String(uint64_t off) const {
    if (off >= strings.size()) Fail("string out of bounds");
    const char* p = reinterpret_cast<const char*>(strings.data()) + off;
    return std::string(p, strnlen(p, strings.size() - size_t(off)));
  }
  uint64_t Tag(uint64_t t) const {
    auto it = tags.find(t);
    if (it == tags.end()) Fail("missing dynamic tag");
    return it->second;
  }
};

Bytes SubBytes(const Bytes& d, uint64_t off, uint64_t size) {
  Span(d, off, size);
  return Bytes(d.begin() + std::ptrdiff_t(off), d.begin() + std::ptrdiff_t(off + size));
}

Module LoadModule(const fs::path& path) {
  Module m;
  m.self = ParseSelf(ReadFile(path));
  const Ph* dp = nullptr;
  const Ph* lp = nullptr;
  for (const auto& p : m.self.ph) {
    if (p.type == 2 && !dp) dp = &p;
    if (p.type == 0x61000000 && !lp) lp = &p;
  }
  if (!dp || !lp) Fail("module has no dynamic data");
  for (uint64_t pos = dp->offset; pos < dp->offset + dp->filesz; pos += 16) {
    const uint64_t tag = Rd<uint64_t>(m.self.elf, pos), value = Rd<uint64_t>(m.self.elf, pos + 8);
    if (tag == 0) break;
    m.dyn.push_back({tag, value});
    m.tags[tag] = value;
  }
  m.blob = SubBytes(m.self.elf, lp->offset, lp->filesz);
  m.strings = SubBytes(m.blob, m.Tag(0x61000035), m.Tag(0x61000037));
  const Bytes table = SubBytes(m.blob, m.Tag(0x61000039), m.Tag(0x6100003f));
  if (table.size() % 24) Fail("bad symbol table size");
  for (size_t pos = 0; pos < table.size(); pos += 24) {
    Sym s;
    const uint32_t name = Rd<uint32_t>(table, pos);
    const uint8_t info = Rd<uint8_t>(table, pos + 4);
    s.section = Rd<uint16_t>(table, pos + 6);
    s.value = Rd<uint64_t>(table, pos + 8);
    s.size = Rd<uint64_t>(table, pos + 16);
    s.type = info & 15;
    s.binding = info >> 4;
    s.name = m.String(name);
    m.syms.push_back(s);
  }
  const uint64_t pairs[2][2] = {{0x61000029, 0x6100002d}, {0x6100002f, 0x61000031}};
  for (const auto& pr : pairs) {
    const Bytes rt = SubBytes(m.blob, m.Tag(pr[0]), m.Tag(pr[1]));
    if (rt.size() % 24) Fail("bad relocation table size");
    for (size_t pos = 0; pos < rt.size(); pos += 24) {
      m.relocs.push_back({Rd<uint64_t>(rt, pos), Rd<uint64_t>(rt, pos + 8), Rd<int64_t>(rt, pos + 16)});
    }
  }
  for (const auto& [tag, value] : m.dyn) {
    if (tag != 0x61000013 && tag != 0x61000015 && tag != 0x6100000d && tag != 0x6100000f) continue;
    auto& table_ref = (tag == 0x61000013 || tag == 0x61000015) ? m.libraries : m.modules;
    const std::string key = EncodeId(value >> 48);
    const std::pair<std::string, uint64_t> identity{m.String(value & 0xffffffff), (value >> 32) & 65535};
    auto it = table_ref.find(key);
    if (it != table_ref.end() && it->second != identity) Fail("conflicting module/library IDs");
    table_ref[key] = identity;
  }
  return m;
}

// ------------------------------------------------------------------------------------------
// prepare: eboot -> flat image
// ------------------------------------------------------------------------------------------
struct Seg {
  uint64_t vaddr, memsz, flags;
};
struct OutReloc {
  uint64_t target, kind;
  int64_t value, addend;
};

struct Image {
  Bytes bytes;
  uint64_t size = 0, entry = 0, flags = 0;
  std::vector<Seg> segs;
  std::vector<std::string> names;
  std::vector<OutReloc> relocs;
};

std::map<std::string, std::string> ParseSfo(const Bytes& d, std::map<std::string, uint32_t>* ints) {
  if (d.size() < 20 || std::memcmp(d.data(), "\0PSF", 4) != 0) Fail("bad SFO signature");
  const uint32_t keys = Rd<uint32_t>(d, 8), values = Rd<uint32_t>(d, 12), count = Rd<uint32_t>(d, 16);
  std::map<std::string, std::string> out;
  for (uint32_t i = 0; i < count; ++i) {
    const uint64_t at = 20 + uint64_t(i) * 16;
    const uint16_t key = Rd<uint16_t>(d, at), fmt = Rd<uint16_t>(d, at + 2);
    const uint32_t size = Rd<uint32_t>(d, at + 4), off = Rd<uint32_t>(d, at + 12);
    std::string k;
    for (uint64_t p = keys + key; p < d.size() && d[p]; ++p) k += char(d[p]);
    Span(d, uint64_t(values) + off, size);
    if (fmt == 0x404) {
      const uint32_t v = Rd<uint32_t>(d, uint64_t(values) + off);
      if (ints) (*ints)[k] = v;
      out[k] = std::to_string(v);
    } else {
      std::string v(reinterpret_cast<const char*>(d.data() + values + off), size);
      while (!v.empty() && v.back() == 0) v.pop_back();
      out[k] = v;
    }
  }
  return out;
}

bool Version128(const std::map<std::string, std::string>& sfo) {
  for (const char* key : {"APP_VER", "VERSION"}) {
    const auto it = sfo.find(key);
    if (it != sfo.end() && it->second == "01.28") return true;
  }
  return false;
}

bool InitEnvIsRet(const fs::path& libc_path) {
  const Module m = LoadModule(libc_path);
  const std::string want = Nid("_init_env"), other = Nid("_ZNSt8ios_base4InitC1Ev");
  bool is_ret = false;
  for (const auto& s : m.syms) {
    const std::string key = s.name.substr(0, s.name.find('#'));
    if (key != want && key != other) continue;
    if (!s.section || (s.type | (s.binding << 4)) & 1) continue;  // same filter the converter has always used
    const Ph* p = nullptr;
    for (const auto& q : m.self.ph) {
      if (q.type == 1 && (q.flags & 1) && q.vaddr <= s.value && s.value < q.vaddr + q.filesz) { p = &q; break; }
    }
    if (!p) Fail("symbol outside executable segments");
    const Bytes code = SubBytes(m.self.elf, p->offset + s.value - p->vaddr, s.size);
    if (key == want) is_ret = code.size() == 1 && code[0] == 0xc3;
  }
  return is_ret;
}

Image PrepareImage(const SelfFile& self, bool init_env_ret, const Module& main) {
  std::vector<const Ph*> loads;
  for (const auto& p : self.ph) {
    if (p.type == 1 || p.type == 0x61000010) loads.push_back(&p);
  }
  auto mapped = [&](uint64_t addr, uint64_t size = 8) {
    for (auto* p : loads) {
      if (p->vaddr <= addr && addr + size <= p->vaddr + p->memsz) return true;
    }
    return false;
  };
  uint64_t size = 0;
  for (auto* p : loads) size = std::max(size, p->vaddr + p->memsz);
  if (size > 512ull * 1024 * 1024 || !mapped(self.entry, 1)) Fail("invalid memory image");
  Image img;
  img.size = size;
  img.entry = self.entry;
  img.flags = init_env_ret ? 1 : 0;
  img.bytes.assign(size_t(size), 0);
  for (auto* p : loads) {
    if (p->filesz > p->memsz) Fail("segment file size exceeds memory size");
    Span(self.elf, p->offset, p->filesz);
    std::memcpy(img.bytes.data() + p->vaddr, self.elf.data() + p->offset, size_t(p->filesz));
    img.segs.push_back({p->vaddr, p->memsz, p->flags});
  }
  std::map<uint64_t, uint64_t> imports;  // symbol index -> import index
  std::vector<uint64_t> import_order;
  for (const auto& r : main.relocs) {
    const uint32_t kind = uint32_t(r.info & 0xffffffff);
    const uint64_t sym = r.info >> 32;
    if (!mapped(r.target)) Fail("relocation outside mapped segments");
    if (kind == 8) {
      img.relocs.push_back({r.target, 0, r.addend, 0});
    } else if (kind == 1 || kind == 6 || kind == 7) {
      if (sym >= main.syms.size()) Fail("bad symbol index");
      const Sym& s = main.syms[size_t(sym)];
      if (s.section) {
        if (s.section == 0xfff1) {
          const uint64_t v = s.value + uint64_t(r.addend);
          std::memcpy(img.bytes.data() + r.target, &v, 8);
        } else {
          img.relocs.push_back({r.target, 0, int64_t(s.value) + r.addend, 0});
        }
      } else {
        if (!imports.count(sym)) {
          imports[sym] = imports.size();
          import_order.push_back(sym);
        }
        const uint64_t k = s.type == 1 ? 2 : 1;
        if ((k == 1 && r.addend) || r.addend < 0 || r.addend >= 4096) Fail("unsupported import addend");
        img.relocs.push_back({r.target, k, int64_t(imports[sym]), r.addend});
      }
    } else {
      Fail("unsupported relocation");
    }
  }
  for (uint64_t sym : import_order) img.names.push_back(main.syms[size_t(sym)].name);
  return img;
}

// ------------------------------------------------------------------------------------------
// link: the eboot plus the game's own modules in one image
// ------------------------------------------------------------------------------------------
struct LinkedModule {
  uint64_t base, size, init, tls_address, tls_memsz, tls_filesz, tls_module, flags;
  std::string name;
};
// LinkedModule::flags: the module starts when the game loads it with sceKernelLoadStartModule, which hands
// module_start its arguments, instead of with the others before the game.
constexpr uint64_t kStartOnLoad = 1;

struct Identity {
  std::string nid, lib, mod;
  bool operator<(const Identity& o) const { return std::tie(nid, lib, mod) < std::tie(o.nid, o.lib, o.mod); }
  bool operator==(const Identity& o) const { return nid == o.nid && lib == o.lib && mod == o.mod; }
};

std::string IdentityKey(const std::string& nid, const std::pair<std::string, uint64_t>& lib,
                        const std::pair<std::string, uint64_t>& mod) {
  return nid + "|" + lib.first + ":" + std::to_string(lib.second) + "|" + mod.first + ":" + std::to_string(mod.second);
}

// mov <reg64>, fs:[0] for any destination register -> the same load through GS: the host owns FS.
unsigned PatchFsLoads(Bytes& image, const std::vector<Ph>& ph, uint64_t base) {
  unsigned patched = 0;
  for (const auto& p : ph) {
    if (p.type != 1 || !(p.flags & 1)) continue;
    const uint64_t start = base + p.vaddr, end = std::min<uint64_t>(base + p.vaddr + p.filesz, image.size());
    for (uint64_t i = start; i + 9 <= end;) {
      const uint8_t* b = image.data() + i;
      const bool modrm = b[3] == 0x04 || b[3] == 0x0c || b[3] == 0x14 || b[3] == 0x1c || b[3] == 0x24 || b[3] == 0x2c ||
                         b[3] == 0x34 || b[3] == 0x3c;
      if (b[0] == 0x64 && (b[1] == 0x48 || b[1] == 0x4c) && b[2] == 0x8b && modrm && b[4] == 0x25 && b[5] == 0 &&
          b[6] == 0 && b[7] == 0 && b[8] == 0) {
        image[size_t(i)] = 0x65;
        ++patched;
        i += 9;
      } else {
        ++i;
      }
    }
  }
  return patched;
}

Bytes Link(Image& img, const Module& main, const std::vector<std::pair<std::string, Module>>& mods,
           const std::set<std::string>& on_load) {
  const uint64_t size = img.size;
  std::unordered_map<std::string, uint32_t> identities;  // key -> import index (insertion order = names)
  std::map<std::string, std::string> main_lib_key, main_mod_key;  // "name:ver" -> encoded id
  for (const auto& [key, id] : main.libraries) main_lib_key[id.first + ":" + std::to_string(id.second)] = key;
  for (const auto& [key, id] : main.modules) main_mod_key[id.first + ":" + std::to_string(id.second)] = key;
  auto identity_of = [](const Module& m, const std::string& name, bool* ok) {
    *ok = false;
    std::vector<std::string> parts;
    size_t from = 0;
    for (size_t at; (at = name.find('#', from)) != std::string::npos; from = at + 1) parts.push_back(name.substr(from, at - from));
    parts.push_back(name.substr(from));
    if (parts.size() != 3) return std::string();
    auto l = m.libraries.find(parts[1]);
    auto d = m.modules.find(parts[2]);
    if (l == m.libraries.end() || d == m.modules.end()) Fail("unknown library or module id");
    *ok = true;
    return IdentityKey(parts[0], l->second, d->second);
  };
  struct IdParts { std::string nid; std::pair<std::string, uint64_t> lib, mod; };
  std::unordered_map<std::string, IdParts> id_parts;
  auto remember = [&](const Module& m, const std::string& name) -> std::string {
    bool ok;
    const std::string key = identity_of(m, name, &ok);
    if (!ok) return key;
    std::vector<std::string> parts;
    size_t from = 0;
    for (size_t at; (at = name.find('#', from)) != std::string::npos; from = at + 1) parts.push_back(name.substr(from, at - from));
    parts.push_back(name.substr(from));
    id_parts[key] = {parts[0], m.libraries.at(parts[1]), m.modules.at(parts[2])};
    return key;
  };
  for (size_t i = 0; i < img.names.size(); ++i) {
    const std::string key = remember(main, img.names[i]);
    if (!key.empty()) identities[key] = uint32_t(i);
  }
  std::vector<std::string> ident_order;  // keys in index order for the main names first
  for (size_t i = 0; i < img.names.size(); ++i) {
    bool ok;
    ident_order.push_back(identity_of(main, img.names[i], &ok));
  }

  auto imported = [&](const Module& m, const Sym& s) -> uint32_t {
    bool ok;
    std::vector<std::string> parts;
    size_t from = 0;
    for (size_t at; (at = s.name.find('#', from)) != std::string::npos; from = at + 1) parts.push_back(s.name.substr(from, at - from));
    parts.push_back(s.name.substr(from));
    const std::string key = remember(m, s.name);
    (void)ok;
    auto it = identities.find(key);
    if (it == identities.end()) {
      const auto& idp = id_parts[key];
      const std::string lk = idp.lib.first + ":" + std::to_string(idp.lib.second);
      const std::string mk = idp.mod.first + ":" + std::to_string(idp.mod.second);
      std::string name;
      if (main_lib_key.count(lk) && main_mod_key.count(mk)) name = idp.nid + "#" + main_lib_key[lk] + "#" + main_mod_key[mk];
      else name = idp.nid + "#" + idp.lib.first;
      it = identities.emplace(key, uint32_t(img.names.size())).first;
      img.names.push_back(name);
      ident_order.push_back(key);
    }
    return it->second;
  };

  struct Export { uint64_t address; uint64_t kind; };
  std::unordered_map<std::string, Export> exports;
  std::map<std::pair<std::string, std::string>, std::vector<Export>> by_nid;
  std::vector<LinkedModule> table;
  uint64_t base = (size + 65535) & ~uint64_t(65535);
  unsigned fs_patched = PatchFsLoads(img.bytes, main.self.ph, 0);
  uint64_t tls_module = 2;
  for (const auto& [filename, m] : mods) {
    std::vector<const Ph*> loads;
    for (const auto& p : m.self.ph) {
      if (p.type == 1 || p.type == 0x61000010) loads.push_back(&p);
    }
    uint64_t modsize = 0;
    for (auto* p : loads) modsize = std::max(modsize, p->vaddr + p->memsz);
    if (base + modsize > 512ull * 1024 * 1024) Fail("linked image exceeds probe limit");
    img.bytes.resize(size_t(base + modsize), 0);
    auto mapped = [&](uint64_t address, uint64_t length = 8) {
      for (auto* p : loads) {
        if (p->vaddr <= address && address + length <= p->vaddr + p->memsz) return true;
      }
      return false;
    };
    for (auto* p : loads) {
      if (p->filesz > p->memsz) Fail(filename + ": invalid segment size");
      Span(m.self.elf, p->offset, p->filesz);
      std::memcpy(img.bytes.data() + base + p->vaddr, m.self.elf.data() + p->offset, size_t(p->filesz));
      img.segs.push_back({base + p->vaddr, p->memsz, p->flags});
    }
    const Ph* tls = nullptr;
    for (const auto& p : m.self.ph) {
      if (p.type == 7) { tls = &p; break; }
    }
    uint64_t module_id = 0;
    if (tls) {
      if (tls->filesz > tls->memsz || !mapped(tls->vaddr, tls->memsz)) Fail(filename + ": unsupported TLS layout");
      module_id = tls_module++;
    }
    for (const auto& r : m.relocs) {
      const uint32_t kind = uint32_t(r.info & 0xffffffff);
      const uint64_t symid = r.info >> 32;
      if (!mapped(r.target)) Fail(filename + ": relocation outside image");
      const uint64_t target = r.target + base;
      if (kind == 8) {
        img.relocs.push_back({target, 0, int64_t(base) + r.addend, 0});
      } else if (kind == 16) {
        const Sym& s = m.syms.at(size_t(symid));
        if (!s.name.empty() || s.type != 3 || r.addend || !module_id) Fail(filename + ": unsupported external TLS relocation");
        std::memcpy(img.bytes.data() + target, &module_id, 8);
      } else if (kind == 1 || kind == 6 || kind == 7) {
        const Sym& s = m.syms.at(size_t(symid));
        if (s.section) {
          if (s.section == 0xfff1) {
            const uint64_t v = s.value + uint64_t(r.addend);
            std::memcpy(img.bytes.data() + target, &v, 8);
          } else {
            img.relocs.push_back({target, 0, int64_t(base + s.value) + r.addend, 0});
          }
        } else {
          bool linkable;
          identity_of(m, s.name, &linkable);
          if (!linkable && s.binding != 2) Fail(filename + ": import without a library identity");
          if (!linkable && s.binding == 2) {
            const uint64_t zero = 0;
            std::memcpy(img.bytes.data() + target, &zero, 8);
          } else {
            const uint64_t rk = s.type == 1 ? 2 : 1;
            if ((rk == 1 && r.addend) || r.addend < 0 || r.addend >= 4096) Fail(filename + ": unsupported import addend");
            img.relocs.push_back({target, rk, int64_t(imported(m, s)), r.addend});
          }
        }
      } else {
        Fail(filename + ": unsupported relocation type");
      }
    }
    for (const auto& s : m.syms) {
      if (!s.section || (s.binding != 1 && s.binding != 2) || (s.type != 1 && s.type != 2)) continue;
      bool linkable;
      identity_of(m, s.name, &linkable);
      if (s.name.empty() || !linkable) continue;
      if (!mapped(s.value, std::max<uint64_t>(1, s.size))) Fail(filename + ": export outside load segments");
      const std::string key = remember(m, s.name);
      if (exports.count(key)) Fail(filename + ": ambiguous export");
      const Export e{base + s.value, s.type == 1 ? 2u : 1u};
      exports[key] = e;
      const auto& idp = id_parts[key];
      by_nid[{idp.nid, idp.lib.first}].push_back(e);
    }
    fs_patched += PatchFsLoads(img.bytes, m.self.ph, base);
    const auto init = m.tags.find(12);
    table.push_back({base, modsize, base + (init == m.tags.end() ? 0 : init->second), tls ? base + tls->vaddr : 0,
                     tls ? tls->memsz : 0, tls ? tls->filesz : 0, module_id, on_load.count(filename) ? kStartOnLoad : 0,
                     filename});
    base = (base + modsize + 65535) & ~uint64_t(65535);
  }

  struct Binding { uint64_t index, address, kind; };
  std::vector<Binding> bindings;
  for (size_t index = 0; index < ident_order.size(); ++index) {
    const std::string& key = ident_order[index];
    if (key.empty()) continue;
    auto it = exports.find(key);
    const Export* found = it == exports.end() ? nullptr : &it->second;
    if (!found) {
      const auto& idp = id_parts[key];
      if (idp.lib.first == "libSceLibcInternal") {
        auto c = by_nid.find({idp.nid, "libc"});
        if (c != by_nid.end() && c->second.size() == 1) found = &c->second[0];
      }
    }
    if (found) bindings.push_back({index, found->address, found->kind});
  }

  const Ph* tlsm = nullptr;
  for (const auto& p : main.self.ph) {
    if (p.type == 7) { tlsm = &p; break; }
  }
  uint64_t tls_values[4] = {0, 0, 0, 0};
  if (tlsm) {
    tls_values[0] = tlsm->vaddr; tls_values[1] = tlsm->filesz; tls_values[2] = tlsm->memsz; tls_values[3] = tlsm->align;
    if (tlsm->filesz > tlsm->memsz || tlsm->memsz > 1024 * 1024 || tlsm->vaddr + tlsm->filesz > size) Fail("unsupported eboot TLS layout");
  }
  const Ph* procparam = nullptr;
  for (const auto& p : main.self.ph) {
    if (p.type == 0x61000001) { procparam = &p; break; }
  }
  if (!procparam) Fail("no procparam segment");

  Bytes out;
  out.insert(out.end(), kBootFormat, kBootFormat + 8);
  Put<uint64_t>(out, img.bytes.size());
  Put<uint64_t>(out, img.entry);
  Put<uint64_t>(out, img.segs.size());
  Put<uint64_t>(out, img.relocs.size());
  Put<uint64_t>(out, img.names.size());
  Put<uint64_t>(out, img.flags);
  Put<uint64_t>(out, procparam->vaddr);
  for (int i = 0; i < 4; ++i) Put<uint64_t>(out, tls_values[i]);
  Put<uint64_t>(out, table.size());
  for (const auto& t : table) {
    Put<uint64_t>(out, t.base); Put<uint64_t>(out, t.size); Put<uint64_t>(out, t.init); Put<uint64_t>(out, t.tls_address);
    Put<uint64_t>(out, t.tls_memsz); Put<uint64_t>(out, t.tls_filesz); Put<uint64_t>(out, t.tls_module);
    Put<uint64_t>(out, t.flags);
    if (t.name.size() >= 32) Fail("module name too long");
    const size_t at = out.size();
    out.resize(at + 32, 0);
    std::memcpy(out.data() + at, t.name.data(), t.name.size());
  }
  Put<uint64_t>(out, bindings.size());
  for (const auto& b : bindings) { Put<uint64_t>(out, b.index); Put<uint64_t>(out, b.address); Put<uint64_t>(out, b.kind); }
  for (const auto& s : img.segs) { Put<uint64_t>(out, s.vaddr); Put<uint64_t>(out, s.memsz); Put<uint64_t>(out, s.flags); }
  for (const auto& n : img.names) {
    if (n.size() >= 128) Fail("name too long");
    const size_t at = out.size();
    out.resize(at + 128, 0);
    std::memcpy(out.data() + at, n.data(), n.size());
  }
  for (const auto& r : img.relocs) {
    Put<uint64_t>(out, r.target); Put<uint64_t>(out, r.kind); Put<int64_t>(out, r.value); Put<int64_t>(out, r.addend);
  }
  out.insert(out.end(), img.bytes.begin(), img.bytes.end());
  (void)fs_patched;
  return out;
}

// ------------------------------------------------------------------------------------------
// patches (shadPS4 style XML)
// ------------------------------------------------------------------------------------------
struct PatchLine {
  std::string type, address, value, hash;
};
struct PatchEntry {
  std::string name, note;
  bool default_on = false;
  bool always = false; // internal fix, applied whatever the launcher selects
  std::vector<PatchLine> lines;
};

std::string Attr(const std::string& tag, const std::string& name) {
  size_t at = 0;
  while ((at = tag.find(name, at)) != std::string::npos) {
    const bool left = at == 0 || std::isspace(static_cast<unsigned char>(tag[at - 1]));
    size_t p = at + name.size();
    while (p < tag.size() && std::isspace(static_cast<unsigned char>(tag[p]))) ++p;
    if (left && p < tag.size() && tag[p] == '=') {
      ++p;
      while (p < tag.size() && std::isspace(static_cast<unsigned char>(tag[p]))) ++p;
      if (p < tag.size() && (tag[p] == '"' || tag[p] == '\'')) {
        const char q = tag[p];
        const size_t e = tag.find(q, p + 1);
        if (e != std::string::npos) return tag.substr(p + 1, e - p - 1);
      }
    }
    at += name.size();
  }
  return "";
}

std::vector<PatchEntry> ParsePatches(const std::string& xml) {
  std::vector<PatchEntry> out;
  size_t i = 0;
  while ((i = xml.find('<', i)) != std::string::npos) {
    if (xml.compare(i, 4, "<!--") == 0) {
      const size_t e = xml.find("-->", i);
      i = e == std::string::npos ? xml.size() : e + 3;
      continue;
    }
    // the tag ends at the first '>' outside quotes
    size_t j = i + 1;
    char quote = 0;
    for (; j < xml.size(); ++j) {
      if (quote) { if (xml[j] == quote) quote = 0; }
      else if (xml[j] == '"' || xml[j] == '\'') quote = xml[j];
      else if (xml[j] == '>') break;
    }
    const std::string tag = xml.substr(i + 1, j - i - 1);
    i = j + 1;
    if (tag.rfind("Metadata", 0) == 0 && !tag.empty() && tag[0] != '/') {
      PatchEntry e;
      e.name = Attr(tag, "Name");
      e.note = Attr(tag, "Note");
      std::string on = Attr(tag, "isEnabled");
      for (auto& c : on) c = char(std::tolower(static_cast<unsigned char>(c)));
      e.default_on = on == "true";
      std::string always = Attr(tag, "Always");
      for (auto& c : always) c = char(std::tolower(static_cast<unsigned char>(c)));
      e.always = always == "true";
      out.push_back(std::move(e));
    } else if (tag.rfind("Line", 0) == 0 && !out.empty()) {
      out.back().lines.push_back({Attr(tag, "Type"), Attr(tag, "Address"), Attr(tag, "Value"), Attr(tag, "OriginalHash")});
    }
  }
  return out;
}

Bytes HexBytes(const std::string& hex) {
  Bytes out;
  int hi = -1;
  for (char c : hex) {
    int v;
    if (c >= '0' && c <= '9') v = c - '0';
    else if (c >= 'a' && c <= 'f') v = c - 'a' + 10;
    else if (c >= 'A' && c <= 'F') v = c - 'A' + 10;
    else continue;
    if (hi < 0) hi = v;
    else { out.push_back(uint8_t(hi << 4 | v)); hi = -1; }
  }
  return out;
}

std::string Fnv1a64(const Bytes& data) {
  uint64_t h = 0xcbf29ce484222325ull;
  for (uint8_t b : data) h = (h ^ b) * 0x100000001b3ull;
  char buf[24];
  std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(h));
  return buf;
}

constexpr uint64_t kPatchBase = 0x400000;

// The bytes the runtime image holds at `offset` before patching: file data where a load segment has
// it, zeros elsewhere (bss, padding). Empty when the range is outside the image.
bool OriginalBytes(const SelfFile& self, uint64_t offset, uint64_t size, uint64_t image_size, Bytes* out) {
  for (const auto& p : self.ph) {
    if (p.type != 1) continue;
    if (p.vaddr <= offset && offset + size <= p.vaddr + p.memsz) {
      out->assign(size_t(size), 0);
      for (uint64_t i = 0; i < size; ++i) {
        const uint64_t rel = offset + i - p.vaddr;
        if (rel < p.filesz && p.offset + rel < self.elf.size()) (*out)[size_t(i)] = self.elf[size_t(p.offset + rel)];
      }
      return true;
    }
  }
  if (offset + size <= image_size) {
    out->assign(size_t(size), 0);
    return true;
  }
  return false;
}

Bytes CompilePatches(const std::vector<PatchEntry>& list, const std::set<std::string>* chosen, const SelfFile& self,
                     uint64_t image_size) {
  std::vector<std::pair<uint64_t, Bytes>> writes;
  for (const auto& e : list) {
    const bool on = e.always || (chosen ? chosen->count(e.name) != 0 : e.default_on);
    if (!on) continue;
    std::vector<std::pair<uint64_t, Bytes>> mine;
    bool skip = false;
    for (const auto& line : e.lines) {
      if (line.type != "bytes") { skip = true; break; }
      const uint64_t address = std::strtoull(line.address.c_str(), nullptr, 16);
      if (address < kPatchBase) { skip = true; break; }
      const uint64_t offset = address - kPatchBase;
      Bytes data = HexBytes(line.value), before;
      if (data.empty() || !OriginalBytes(self, offset, data.size(), image_size, &before)) { skip = true; break; }
      if (!line.hash.empty()) {
        std::string want = line.hash;
        for (auto& c : want) c = char(std::tolower(static_cast<unsigned char>(c)));
        if (Fnv1a64(before) != want) { skip = true; break; }
      }
      mine.push_back({offset, std::move(data)});
    }
    if (!skip) writes.insert(writes.end(), mine.begin(), mine.end());
  }
  Bytes out;
  const char magic[8] = {'B', 'B', 'P', 'A', 'T', 'C', 'H', '2'};
  out.insert(out.end(), magic, magic + 8);
  Put<uint64_t>(out, kPatchBase);
  Put<uint64_t>(out, writes.size());
  for (const auto& w : writes) {
    Put<uint64_t>(out, w.first);
    Put<uint64_t>(out, w.second.size());
    out.insert(out.end(), w.second.begin(), w.second.end());
  }
  return out;
}

std::vector<std::string> ReadLines(const fs::path& path) {
  std::vector<std::string> lines;
  std::ifstream in(path, std::ios::binary);
  for (std::string line; std::getline(in, line);) {
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
    if (!line.empty()) lines.push_back(line);
  }
  return lines;
}

struct ModuleSource {
  const char* name;
  const char* folder;
  const char* order;  // U = update dump first, B = base dump first
  bool on_load = false;  // started when the game loads it (kStartOnLoad); a dump without it only lacks that feature
};
const ModuleSource kModules[] = {
    {"libc.prx", "sce_module/", "UB"},      {"libSceFios2.prx", "sce_module/", "UB"}, {"libtbb.prx", "", "UB"},
    {"libfmod.prx", "", "BU"},              {"libfmodstudio.prx", "", "BU"},          {"iksemelGNMm.prx", "", "UB"},
    {"logiWheel.prx", "", "UB"},
    // The on-screen keyboard (club names, searches): its module_start registers the "OnScreenKeyboard" service with
    // the service list the game passes, and the service shows the system text dialog (libSceImeDialog).
    {"oskGNMm.prx", "", "UB", true},
};

}  // namespace

bool SfoIsVersion128(const fs::path& param_sfo) {
  try {
    return Version128(ParseSfo(ReadFile(param_sfo), nullptr));
  } catch (const std::exception&) {
    return false;
  }
}

bool FolderIsVersion128(const fs::path& folder) {
  if (SfoIsVersion128(folder / "sce_sys" / "param.sfo")) return true;
  try {
    const Bytes eboot = ReadFile(folder / "eboot.bin");
    return eboot.size() == 25650713 && Fnv1a64(eboot) == "c706870aa4881011";
  } catch (const std::exception&) {
    return false;
  }
}

std::string SfoField(const fs::path& param_sfo, const char* key) {
  try {
    const auto fields = ParseSfo(ReadFile(param_sfo), nullptr);
    const auto it = fields.find(key);
    return it == fields.end() ? std::string() : it->second;
  } catch (const std::exception&) {
    return "";
  }
}

std::string PrepareGame(const fs::path& update, const fs::path& base, const fs::path& work, const fs::path& patch_xml,
                        bool patches_only, const std::function<void(int, const std::string&)>& progress) {
  try {
    const fs::path prep = work / "prep", user = work / "user";
    fs::create_directories(prep);
    fs::create_directories(user);

    // the patches only need the already converted eboot
    auto compile_patches = [&](const SelfFile& self, uint64_t image_size) {
      progress(5, "Compiling the patches");
      const Bytes xml_bytes = ReadFile(patch_xml);
      const auto list = ParsePatches(std::string(xml_bytes.begin(), xml_bytes.end()));
      std::set<std::string> chosen;
      const fs::path selection = user / "patches_enabled.txt";
      const bool has_selection = fs::exists(selection);
      for (const auto& l : ReadLines(selection)) chosen.insert(l);
      WriteFile(prep / "patches.bin", CompilePatches(list, has_selection ? &chosen : nullptr, self, image_size));
    };

    if (patches_only) {
      progress(0, "Reading the prepared game");
      SelfFile self;
      self.elf = ReadFile(prep / "eboot.elf");
      const Bytes meta = ReadFile(prep / "image.meta");
      // image.meta: image size, then the program headers of the eboot
      uint64_t pos = 0;
      const uint64_t image_size = Rd<uint64_t>(meta, pos);
      pos += 8;
      const uint64_t count = Rd<uint64_t>(meta, pos);
      pos += 8;
      for (uint64_t i = 0; i < count; ++i, pos += 56) {
        Ph p;
        p.type = Rd<uint32_t>(meta, pos); p.flags = Rd<uint32_t>(meta, pos + 4);
        p.offset = Rd<uint64_t>(meta, pos + 8); p.vaddr = Rd<uint64_t>(meta, pos + 16); p.paddr = Rd<uint64_t>(meta, pos + 24);
        p.filesz = Rd<uint64_t>(meta, pos + 32); p.memsz = Rd<uint64_t>(meta, pos + 40); p.align = Rd<uint64_t>(meta, pos + 48);
        self.ph.push_back(p);
      }
      compile_patches(self, image_size);
      progress(6, "Ready");
      return "";
    }

    progress(0, "Checking the game folders");
    if (!fs::exists(update / "eboot.bin")) return "No eboot.bin in the update folder.";
    if (!fs::exists(update / "sce_sys" / "param.sfo")) return "No sce_sys/param.sfo in the update folder.";
    std::map<std::string, uint32_t> ints;
    const auto sfo = ParseSfo(ReadFile(update / "sce_sys" / "param.sfo"), &ints);
    // The US release (CUSA00093) or the European one (CUSA00003).
    const std::string title = sfo.count("TITLE_ID") ? sfo.at("TITLE_ID") : std::string();
    if (title != "CUSA00093" && title != "CUSA00003") return "The update folder is not DriveClub (CUSA00093 or CUSA00003).";
    if (!FolderIsVersion128(update)) {
      const auto field = [&](const char* key) { return sfo.count(key) ? sfo.at(key) : std::string("none"); };
      return "The update folder must be DriveClub version 1.28 (its param.sfo says APP_VER " + field("APP_VER") + ", VERSION " + field("VERSION") + ").";
    }
    if (!fs::exists(base / "game000.dat")) return "The base game folder has no game000.dat.";

    progress(1, "Collecting the game modules");
    std::vector<std::pair<std::string, Module>> mods;
    std::set<std::string> on_load;
    fs::path libc_path;
    for (const auto& src : kModules) {
      fs::path found;
      for (const char* o = src.order; *o; ++o) {
        const fs::path candidate = (*o == 'U' ? update : base) / (std::string(src.folder) + src.name);
        if (fs::exists(candidate)) { found = candidate; break; }
      }
      if (found.empty() && src.on_load) continue;
      if (found.empty()) return std::string(src.name) + " was not found in the game folders.";
      if (std::string(src.name) == "libc.prx") libc_path = found;
      if (src.on_load) on_load.insert(src.name);
      mods.push_back({src.name, LoadModule(found)});
    }

    progress(2, "Converting the game executable");
    const Module main = LoadModule(update / "eboot.bin");
    const bool init_env_ret = InitEnvIsRet(libc_path);
    Image img = PrepareImage(main.self, init_env_ret, main);
    WriteFile(prep / "eboot.elf", main.self.elf);
    {
      Bytes meta;
      Put<uint64_t>(meta, img.size);
      Put<uint64_t>(meta, main.self.ph.size());
      for (const auto& p : main.self.ph) {
        Put<uint32_t>(meta, p.type); Put<uint32_t>(meta, p.flags); Put<uint64_t>(meta, p.offset); Put<uint64_t>(meta, p.vaddr);
        Put<uint64_t>(meta, p.paddr); Put<uint64_t>(meta, p.filesz); Put<uint64_t>(meta, p.memsz); Put<uint64_t>(meta, p.align);
      }
      WriteFile(prep / "image.meta", meta);
    }

    progress(3, "Linking the game modules");
    WriteFile(prep / "boot-linked.bin", Link(img, main, mods, on_load));

    progress(4, "Writing the content profile");
    {
      Bytes content;
      const char magic[8] = {'B', 'B', 'C', 'O', 'N', 'T', '0', '1'};
      content.insert(content.end(), magic, magic + 8);
      Put<uint32_t>(content, 3);  // full SKU
      for (int i = 1; i <= 4; ++i) {
        auto it = ints.find("USER_DEFINED_PARAM_" + std::to_string(i));
        Put<uint32_t>(content, it == ints.end() ? 0 : it->second);
      }
      WriteFile(prep / "content.bin", content);
    }

    compile_patches(main.self, img.size);
    progress(6, "Ready");
    return "";
  } catch (const std::exception& e) {
    return std::string("Preparation failed: ") + e.what();
  }
}
