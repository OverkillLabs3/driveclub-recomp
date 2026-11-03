// Builds the data files that make the game's add-on content playable on Linux: a port of launcher/unlock.cpp and
// the archive reader in launcher/datx.cpp. Only the file access differs: std::filesystem and std::fopen instead of
// the Win32 calls, so the three lists can be built from the player's own dump inside the AppImage.
#include "unlock_linux.h"

#include <sys/types.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <system_error>
#include <vector>

namespace {

namespace fs = std::filesystem;

uint32_t Rd32(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (uint32_t(p[3]) << 24); }
uint64_t Rd64(const uint8_t* p) { return uint64_t(Rd32(p)) | (uint64_t(Rd32(p + 4)) << 32); }

bool ReadWhole(const fs::path& path, std::vector<uint8_t>& out) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return false;
  in.seekg(0, std::ios::end);
  const std::streamoff size = in.tellg();
  if (size < 0) return false;
  in.seekg(0, std::ios::beg);
  out.resize(std::size_t(size));
  return size == 0 || bool(in.read(reinterpret_cast<char*>(out.data()), size));
}

bool WriteWhole(const fs::path& path, const void* data, std::size_t size) {
  std::error_code ec;
  fs::create_directories(path.parent_path(), ec);
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out) return false;
  if (size) out.write(static_cast<const char*>(data), std::streamsize(size));
  out.flush();
  return bool(out);
}

// The name hash of the archive (all arithmetic wraps at 32 bits).
uint32_t NameHash(const std::string& name, uint32_t a, uint32_t b) {
  uint32_t r = 0;
  for (unsigned char ch : name) {
    uint32_t c = ch >= 'a' && ch <= 'z' ? ch - 32 : ch;
    if (c == '\\') c = '/';
    a = uint32_t(a * b) % 0x7FFFFFFEu;
    r = (uint32_t(r * a) + c) % 0x7FFFFFFFu;
  }
  return r;
}

// One LZ4 block.
bool Lz4Block(const uint8_t* src, size_t n, uint8_t* dst, size_t dsize) {
  size_t i = 0, o = 0;
  while (i < n) {
    const uint8_t token = src[i++];
    size_t lit = token >> 4;
    if (lit == 15) {
      uint8_t s;
      do {
        if (i >= n) return false;
        s = src[i++];
        lit += s;
      } while (s == 255);
    }
    if (i + lit > n || o + lit > dsize) return false;
    std::memcpy(dst + o, src + i, lit);
    i += lit;
    o += lit;
    if (i >= n) break;  // the last sequence has no match
    if (i + 2 > n) return false;
    const size_t offset = src[i] | (src[i + 1] << 8);
    i += 2;
    size_t match = token & 15;
    if (match == 15) {
      uint8_t s;
      do {
        if (i >= n) return false;
        s = src[i++];
        match += s;
      } while (s == 255);
    }
    match += 4;
    if (offset == 0 || offset > o || o + match > dsize) return false;
    for (size_t k = 0; k < match; ++k, ++o) dst[o] = dst[o - offset];
  }
  return o == dsize;
}

std::string VolumeName(unsigned vol) {
  char buf[32];
  std::snprintf(buf, sizeof buf, "game%03u.dat", vol);
  return buf;
}

// Finds `path` in the archive whose index is in update_dir (or, when missing there, base_dir). The volume files
// are taken from update_dir first, then base_dir. Same layout as launcher/datx.cpp.
std::string DatxExtract(const fs::path& update_dir, const fs::path& base_dir, const std::string& path,
                        std::vector<uint8_t>& out) {
  std::vector<uint8_t> ndx;
  if (!ReadWhole(update_dir / "game.ndx", ndx) && !ReadWhole(base_dir / "game.ndx", ndx)) return "game.ndx not found";
  if (ndx.size() < 0x40 || std::memcmp(ndx.data(), "DATX", 4) != 0) return "game.ndx has an unexpected format";
  const uint64_t filetime = Rd64(&ndx[8]);
  const uint32_t seed_a = Rd32(&ndx[0x18]), seed_b = Rd32(&ndx[0x1C]);
  const uint32_t chunk_size = Rd32(&ndx[0x24]);
  const uint32_t files = Rd32(&ndx[0x38]);
  if (chunk_size == 0 || chunk_size > 0x40000 || files == 0 || files > 1000000)
    return "game.ndx has unexpected values";
  size_t p = 0x3C;
  const size_t half = ndx[p];
  p += 1 + 2 * half;
  if (p + 4 > ndx.size()) return "game.ndx is cut short";
  const size_t names = Rd32(&ndx[p]);
  p += 4 + names;
  if (p + 4 > ndx.size() || Rd32(&ndx[p]) != 0x12345678u) return "game.ndx marker not found";
  p += 4;
  if (p + size_t(files) * 32 > ndx.size()) return "game.ndx records are cut short";
  const uint8_t* rec = &ndx[p];

  const uint32_t want = NameHash(path, seed_a, seed_b);
  size_t lo = 0, hi = files;
  while (lo < hi) {
    const size_t mid = (lo + hi) / 2;
    const uint32_t h = Rd32(rec + mid * 32 + 12);
    if (h < want) lo = mid + 1;
    else hi = mid;
  }
  if (lo >= files || Rd32(rec + lo * 32 + 12) != want) return "not in the archive: " + path;
  const uint64_t loc = Rd64(rec + lo * 32);
  const uint32_t size = Rd32(rec + lo * 32 + 8);
  // Indexes dated before 2014-08-20 pack the location as 8 / 56 bits, later ones as 16 / 48.
  const uint64_t old_style = (1408492800ull + 11644473600ull) * 10000000ull;
  const unsigned vol = filetime <= old_style ? unsigned(loc & 0xFF) : unsigned(loc & 0xFFFF);
  const uint64_t offset = filetime <= old_style ? (loc >> 8) : (loc >> 16);

  fs::path dat_path = update_dir / VolumeName(vol);
  std::FILE* f = std::fopen(dat_path.c_str(), "rb");
  if (!f) {
    dat_path = base_dir / VolumeName(vol);
    f = std::fopen(dat_path.c_str(), "rb");
  }
  if (!f) return "volume file missing: " + VolumeName(vol);

  std::string err;
  uint8_t head[0x24];
  if (std::fread(head, 1, sizeof head, f) != sizeof head || std::memcmp(head, "DATF", 4) != 0 ||
      std::memcmp(head + 0x20, "CHNK", 4) != 0) {
    err = "volume file has an unexpected format";
  }
  uint32_t count = 0, vchunk = 0;
  std::vector<uint8_t> table;
  if (err.empty()) {
    count = Rd32(head + 0x18);
    vchunk = Rd32(head + 0x1C);
    if (vchunk != chunk_size || count == 0 || count > 4000000) err = "volume file has unexpected values";
  }
  if (err.empty()) {
    table.resize(size_t(count) * 4 + 4);
    if (std::fread(table.data(), 1, table.size(), f) != table.size() ||
        std::memcmp(&table[size_t(count) * 4], "DATA", 4) != 0)
      err = "volume chunk table is damaged";
  }
  out.clear();
  if (err.empty()) {
    const uint64_t data_start = 0x24 + uint64_t(count) * 4 + 4;
    const uint64_t first = offset / chunk_size;
    const uint64_t last = size ? (offset + size - 1) / chunk_size : first;
    if (last >= count) err = "file lies outside its volume";
    uint64_t pos = data_start;
    for (uint64_t k = 0; err.empty() && k < first; ++k) pos += Rd32(&table[k * 4]);
    out.reserve(size);
    std::vector<uint8_t> raw, plain(chunk_size);
    for (uint64_t k = first; err.empty() && k <= last; ++k) {
      const uint32_t stored = Rd32(&table[k * 4]);
      if (stored == 0) {
        std::fill(plain.begin(), plain.end(), 0);
      } else {
        raw.resize(stored);
        if (::fseeko(f, static_cast<off_t>(pos), SEEK_SET) != 0 ||
            std::fread(raw.data(), 1, stored, f) != stored) {
          err = "volume file is cut short";
          break;
        }
        if (stored == chunk_size) {
          std::memcpy(plain.data(), raw.data(), chunk_size);
        } else if (stored > 4 && Rd32(raw.data()) == chunk_size) {
          if (!Lz4Block(raw.data() + 4, stored - 4, plain.data(), chunk_size)) {
            err = "a chunk could not be unpacked";
            break;
          }
        } else {
          err = "a chunk has an unexpected format";
          break;
        }
      }
      pos += stored;
      const uint64_t from = k == first ? offset % chunk_size : 0;
      const uint64_t to = k == last ? (offset + size - 1) % chunk_size + 1 : chunk_size;
      if (size) out.insert(out.end(), plain.begin() + from, plain.begin() + to);
    }
  }
  std::fclose(f);
  if (!err.empty()) {
    out.clear();
    return err;
  }
  return out.size() == size ? "" : "extracted size does not match the index";
}

bool DatxContains(const fs::path& update_dir, const fs::path& base_dir, const std::string& path) {
  std::vector<uint8_t> ndx;
  if (!ReadWhole(update_dir / "game.ndx", ndx) && !ReadWhole(base_dir / "game.ndx", ndx)) return false;
  if (ndx.size() < 0x40 || std::memcmp(ndx.data(), "DATX", 4) != 0) return false;
  const uint32_t seed_a = Rd32(&ndx[0x18]), seed_b = Rd32(&ndx[0x1C]);
  const uint32_t files = Rd32(&ndx[0x38]);
  if (files == 0 || files > 1000000) return false;
  size_t p = 0x3C;
  const size_t half = ndx[p];
  p += 1 + 2 * half;
  if (p + 4 > ndx.size()) return false;
  const size_t names = Rd32(&ndx[p]);
  p += 4 + names;
  if (p + 4 > ndx.size() || Rd32(&ndx[p]) != 0x12345678u) return false;
  p += 4;
  if (p + size_t(files) * 32 > ndx.size()) return false;
  const uint8_t* rec = &ndx[p];
  const uint32_t want = NameHash(path, seed_a, seed_b);
  size_t lo = 0, hi = files;
  while (lo < hi) {
    const size_t mid = (lo + hi) / 2;
    if (Rd32(rec + mid * 32 + 12) < want) lo = mid + 1;
    else hi = mid;
  }
  return lo < files && Rd32(rec + lo * 32 + 12) == want;
}

// ------------------------------------------------------------------------------------------------ vehicles.csv
// The list marks every add-on car as DLC and every bike as needing the bike edition, and the game then asks the
// store whether the player owns it. Marking them as ordinary cars skips that question.
struct Field {
  std::string text;  // without quotes
  bool quoted = false;
};

std::vector<Field> SplitLine(const std::string& line) {
  std::vector<Field> out;
  size_t i = 0;
  while (true) {
    Field f;
    if (i < line.size() && line[i] == '"') {
      f.quoted = true;
      ++i;
      while (i < line.size()) {
        if (line[i] == '"') {
          if (i + 1 < line.size() && line[i + 1] == '"') {
            f.text += '"';
            i += 2;
            continue;
          }
          ++i;
          break;
        }
        f.text += line[i++];
      }
    } else {
      while (i < line.size() && line[i] != ',') f.text += line[i++];
    }
    out.push_back(std::move(f));
    if (i >= line.size() || line[i] != ',') break;
    ++i;
  }
  return out;
}

std::string JoinLine(const std::vector<Field>& fields) {
  std::string s;
  for (size_t i = 0; i < fields.size(); ++i) {
    if (i) s += ',';
    if (fields[i].quoted) {
      s += '"';
      for (char c : fields[i].text) {
        if (c == '"') s += '"';
        s += c;
      }
      s += '"';
    } else {
      s += fields[i].text;
    }
  }
  return s;
}

// `has_file(path)` says whether the game archive holds a file: a car is only made customisable when the template
// of its livery editor exists (one add-on car has none, and the game stops when it is asked for it).
bool PatchVehicles(std::string& text, const std::function<bool(const std::string&)>& has_file, bool all_cars) {
  std::string out;
  size_t pos = 0;
  int status = -1, versions = -1, customise = -1, player = -1, club = -1, vehicle_name = -1;
  bool header = true, changed = false;
  while (pos < text.size()) {
    size_t end = text.find('\n', pos);
    if (end == std::string::npos) end = text.size();
    std::string line = text.substr(pos, end - pos);
    std::string eol;
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
      eol = "\r";
    }
    if (end < text.size()) eol += "\n";
    if (!line.empty()) {
      auto f = SplitLine(line);
      if (header) {
        for (size_t i = 0; i < f.size(); ++i) {
          if (f[i].text == "Status") status = int(i);
          if (f[i].text == "GameVersions") versions = int(i);
          if (f[i].text == "customise") customise = int(i);
          if (f[i].text == "Vehicle Name") vehicle_name = int(i);
          if (f[i].text == "PlayerUnlockLevel") player = int(i);
          if (f[i].text == "ClubUnlockLevel") club = int(i);
        }
        if (status < 0 || versions < 0) return false;
        header = false;
      } else if (f.size() > size_t(std::max(status, versions))) {
        if (f[status].text == "DLC") {
          f[status].text = "In-Game";
          changed = true;
        }
        if (f[versions].text == "Bikes") {
          f[versions].text = "Bikes-PS+Edition";
          changed = true;
        }
        if (customise >= 0 && f.size() > size_t(customise) && f[customise].text == "FALSE") {
          std::string name = vehicle_name >= 0 && f.size() > size_t(vehicle_name) ? f[vehicle_name].text : "";
          std::transform(name.begin(), name.end(), name.begin(),
                         [](unsigned char ch) { return char(std::tolower(ch)); });
          if (!name.empty() && has_file("liveryeditor/templates/" + name + ".tmp")) {
            f[customise].text = "TRUE";
            changed = true;
          }
        }
        // Cars that online club levels used to unlock are unlocked by the driver level instead.
        if (club >= 0 && player >= 0 && f.size() > size_t(std::max(club, player)) && !f[club].text.empty() &&
            f[player].text.empty()) {
          f[player] = f[club];
          f[club] = Field{};
          changed = true;
        }
        // Every car and bike from the first level on.
        if (all_cars && player >= 0 && f.size() > size_t(player)) {
          if (!f[player].text.empty() && f[player].text != "1") {
            f[player].text = "1";
            changed = true;
          }
          if (club >= 0 && f.size() > size_t(club) && !f[club].text.empty()) {
            f[club] = Field{};
            changed = true;
          }
        }
        line = JoinLine(f);
      }
    }
    out += line + eol;
    pos = end < text.size() ? end + 1 : end;
  }
  text = std::move(out);
  return changed;
}

// ------------------------------------------------------------------------------------------------ dc.tour
// Each tour names a licence the player must own. Blank licence names always pass the check; the two bike
// expansion tours check a different way, which the ordinary add-on kind does not.
bool PatchTours(std::vector<uint8_t>& d) {
  size_t p = 0;
  auto u32 = [&](uint32_t& v) {
    if (p + 4 > d.size()) return false;
    v = d[p] | (d[p + 1] << 8) | (d[p + 2] << 16) | (uint32_t(d[p + 3]) << 24);
    p += 4;
    return true;
  };
  auto str = [&](size_t& at, uint32_t& len) {
    if (!u32(len) || p + len > d.size()) return false;
    at = p;
    p += len;
    return true;
  };
  if (d.size() < 16 || std::memcmp(d.data(), "EVOSLITL", 8) != 0) return false;
  p = 8;
  uint32_t v = 0, len = 0, count = 0;
  size_t at = 0;
  if (!str(at, len) || !u32(v) || !str(at, len) || !u32(count) || count > 1000) return false;
  bool changed = false;
  for (uint32_t i = 0; i < count; ++i) {
    if (!u32(v)) return false;                                             // id
    for (int k = 0; k < 2; ++k) if (!str(at, len)) return false;           // lams id, unknown
    size_t mask_at;
    uint32_t mask_len;
    if (!str(mask_at, mask_len)) return false;                             // licence name
    if (!str(at, len)) return false;                                       // menu texture
    for (int k = 0; k < 3; ++k) if (!u32(v)) return false;                 // tile set, active, unknown
    size_t req_at;
    uint32_t req_len;
    if (!str(req_at, req_len)) return false;                               // add-on requirement
    if (!str(at, len)) return false;                                       // completed texture
    const size_t type_at = p;
    uint32_t type;
    if (!u32(type) || !u32(v)) return false;                               // licence type, collection
    for (uint32_t k = 0; k < mask_len; ++k) {
      if (d[mask_at + k]) {
        std::memset(&d[mask_at], 0, mask_len);
        changed = true;
        break;
      }
    }
    if (req_len == 4 && std::memcmp(&d[req_at], "DLC1", 4) == 0) {
      std::memset(&d[req_at], 0xFF, 4);
      changed = true;
    }
    if (type == 4) {
      d[type_at] = 5;
      changed = true;
    }
  }
  return changed;
}

// ------------------------------------------------------------------------------------------------ assets.txt
// Livery decals name the add-on they belong to; without that name the game does not ask for it.
bool PatchLiveries(std::string& text) {
  bool changed = false;
  size_t pos = 0;
  while ((pos = text.find("entitlement", pos)) != std::string::npos) {
    size_t start = pos;
    if (start >= 9 && text.compare(start - 9, 9, "purchase_") == 0) start -= 9;
    size_t q = pos + 11;
    while (q < text.size() && text[q] != '=' && text[q] != '\n') ++q;
    if (q >= text.size() || text[q] != '=' || q + 1 >= text.size() || text[q + 1] != '"') {
      pos += 11;
      continue;
    }
    const size_t close = text.find('"', q + 2);
    if (close == std::string::npos || text.find('\n', q) < close) {
      pos += 11;
      continue;
    }
    size_t from = start;
    while (from > 0 && text[from - 1] == ' ') --from;
    text.erase(from, close + 1 - from);
    pos = from;
    changed = true;
  }
  return changed;
}

std::string StampPart(const fs::path& path) {
  std::error_code ec;
  const std::uintmax_t size = fs::file_size(path, ec);
  if (ec) return "0:0";
  const auto time = fs::last_write_time(path, ec);
  if (ec) return "0:0";
  return std::to_string(size) + ":" + std::to_string(static_cast<long long>(time.time_since_epoch().count()));
}

}  // namespace

std::string BuildUnlockFiles(const std::string& update_dir, const std::string& base_dir, const std::string& out_dir,
                             bool all_cars) {
  const fs::path update(update_dir), base(base_dir), out(out_dir);
  const fs::path stamp_path = out / "stamp.txt";
  char stamp_text[256];
  std::snprintf(stamp_text, sizeof stamp_text, "unlock-3%s %s %s", all_cars ? "-all" : "",
                StampPart(update / "game.ndx").c_str(), StampPart(base / "game.ndx").c_str());
  const std::string stamp = stamp_text;

  std::vector<uint8_t> previous;
  if (ReadWhole(stamp_path, previous) && std::string(previous.begin(), previous.end()) == stamp &&
      fs::exists(out / "newdata/vehicles.csv") && fs::exists(out / "data/databases/dc.tour") &&
      fs::exists(out / "liveryeditor/assets/assets.txt")) {
    return "";
  }
  std::error_code ec;
  fs::remove(stamp_path, ec);

  std::vector<uint8_t> raw;
  std::string err = DatxExtract(update, base, "newdata/vehicles.csv", raw);
  if (!err.empty()) return "vehicle list: " + err;
  std::string vehicles(raw.begin(), raw.end());
  const auto has_file = [&](const std::string& path) { return DatxContains(update, base, path); };
  if (!PatchVehicles(vehicles, has_file, all_cars)) return "the vehicle list has an unexpected layout";

  err = DatxExtract(update, base, "data/databases/dc.tour", raw);
  if (!err.empty()) return "tour list: " + err;
  std::vector<uint8_t> tour = raw;
  if (!PatchTours(tour)) return "the tour list has an unexpected layout";

  err = DatxExtract(update, base, "liveryeditor/assets/assets.txt", raw);
  if (!err.empty()) return "livery list: " + err;
  std::string liveries(raw.begin(), raw.end());
  PatchLiveries(liveries);

  if (!WriteWhole(out / "newdata/vehicles.csv", vehicles.data(), vehicles.size()) ||
      !WriteWhole(out / "data/databases/dc.tour", tour.data(), tour.size()) ||
      !WriteWhole(out / "liveryeditor/assets/assets.txt", liveries.data(), liveries.size()) ||
      !WriteWhole(stamp_path, stamp.data(), stamp.size())) {
    return "the unlock files could not be written";
  }
  return "";
}
