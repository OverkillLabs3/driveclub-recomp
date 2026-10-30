#include "unlock.h"

#include <windows.h>

#include <algorithm>
#include <functional>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "datx.h"

namespace {

constexpr const char* kVersion = "unlock-3";

std::wstring Widen(const std::string& s) {
  if (s.empty()) return L"";
  const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
  std::wstring w(size_t(n), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
  return w;
}

void MakeDirs(const std::wstring& dir) {
  for (size_t i = 3; i <= dir.size(); ++i) {
    if (i == dir.size() || dir[i] == L'\\') CreateDirectoryW(dir.substr(0, i).c_str(), nullptr);
  }
}

bool WriteFile(const std::wstring& path, const std::vector<uint8_t>& data) {
  const size_t slash = path.find_last_of(L'\\');
  if (slash != std::wstring::npos) MakeDirs(path.substr(0, slash));
  FILE* f = _wfopen(path.c_str(), L"wb");
  if (!f) return false;
  const bool ok = data.empty() || fwrite(data.data(), 1, data.size(), f) == data.size();
  return fclose(f) == 0 && ok;
}

std::string ReadText(const std::wstring& path) {
  FILE* f = _wfopen(path.c_str(), L"rb");
  if (!f) return "";
  std::string s;
  char buf[4096];
  size_t n;
  while ((n = fread(buf, 1, sizeof buf, f)) > 0) s.append(buf, n);
  fclose(f);
  return s;
}

uint64_t FileStamp(const std::wstring& path) {
  WIN32_FILE_ATTRIBUTE_DATA d;
  if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &d)) return 0;
  return (uint64_t(d.nFileSizeLow) << 20) ^ uint64_t(d.ftLastWriteTime.dwLowDateTime) ^ (uint64_t(d.nFileSizeHigh) << 40);
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
          std::transform(name.begin(), name.end(), name.begin(), [](unsigned char ch) { return char(std::tolower(ch)); });
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
  if (d.size() < 16 || memcmp(d.data(), "EVOSLITL", 8) != 0) return false;
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
        memset(&d[mask_at], 0, mask_len);
        changed = true;
        break;
      }
    }
    if (req_len == 4 && memcmp(&d[req_at], "DLC1", 4) == 0) {
      memset(&d[req_at], 0xFF, 4);
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

}  // namespace

std::wstring BuildUnlock(const std::wstring& update_dir, const std::wstring& base_dir, const std::wstring& out_dir,
                         bool all_cars) {
  const std::wstring stamp_path = out_dir + L"\\stamp.txt";
  char stamp[256];
  snprintf(stamp, sizeof stamp, "%s%s %llx %llx", kVersion, all_cars ? "-all" : "", (unsigned long long)FileStamp(update_dir + L"\\game.ndx"),
           (unsigned long long)FileStamp(base_dir + L"\\game.ndx"));
  const std::wstring files[3] = {L"\\newdata\\vehicles.csv", L"\\data\\databases\\dc.tour", L"\\liveryeditor\\assets\\assets.txt"};
  if (ReadText(stamp_path) == stamp) {
    bool all = true;
    for (const auto& f : files) all = all && GetFileAttributesW((out_dir + f).c_str()) != INVALID_FILE_ATTRIBUTES;
    if (all) return L"";
  }
  DeleteFileW(stamp_path.c_str());

  std::vector<uint8_t> raw;
  std::string err = DatxExtract(update_dir, base_dir, "newdata/vehicles.csv", raw);
  if (!err.empty()) return Widen("vehicle list: " + err);
  std::string vehicles(raw.begin(), raw.end());
  const auto has_file = [&](const std::string& path) { return DatxContains(update_dir, base_dir, path); };
  if (!PatchVehicles(vehicles, has_file, all_cars)) return L"the vehicle list has an unexpected layout";

  err = DatxExtract(update_dir, base_dir, "data/databases/dc.tour", raw);
  if (!err.empty()) return Widen("tour list: " + err);
  std::vector<uint8_t> tour = raw;
  if (!PatchTours(tour)) return L"the tour list has an unexpected layout";

  err = DatxExtract(update_dir, base_dir, "liveryeditor/assets/assets.txt", raw);
  if (!err.empty()) return Widen("livery list: " + err);
  std::string liveries(raw.begin(), raw.end());
  PatchLiveries(liveries);

  if (!WriteFile(out_dir + files[0], std::vector<uint8_t>(vehicles.begin(), vehicles.end())) ||
      !WriteFile(out_dir + files[1], tour) ||
      !WriteFile(out_dir + files[2], std::vector<uint8_t>(liveries.begin(), liveries.end())))
    return L"the unlock files could not be written";
  if (!WriteFile(stamp_path, std::vector<uint8_t>(stamp, stamp + strlen(stamp)))) return L"the unlock files could not be written";
  return L"";
}
