// The cockpit camera's head (headcam.h): the file's own text with the numbers of the lines the choices touch
// changed, everything else byte for byte (comments, tabs, CRLF, the IsHMD block, the trailing line without a
// newline).
//
// Where the targets come from:
//   look: fully into the direction of travel (VELOCITY_LOOKAT_FACTOR 1.0), half of the yaw term the file calls
//     violent (0.5), up to 90 degrees to the sides; measured on a hairpin track with the AI driving, weaker values
//     (0.45 / 0.12) could not be told apart from the game's own camera.
//   weight: the drive-eyes build's values (the head sways and the springs soften to x0.25).
//
// Left as the game has them: the camera shake (the CAMERA_SHAKE_INPUT_MODIFIER lines: the chase camera's values,
// measured in the cockpit, could not be told apart from the game's own) and the field of view (the launcher has its
// own slider).
#include "headcam.h"

#include <windows.h>

#include <cctype>
#include <cstdio>
#include <cstring>
#include <regex>
#include <string>
#include <vector>

#include "datx.h"

namespace {

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

bool WriteFile(const std::wstring& path, const void* data, size_t size) {
  FILE* f = _wfopen(path.c_str(), L"wb");
  if (!f) return false;
  const bool ok = size == 0 || fwrite(data, 1, size, f) == size;
  return fclose(f) == 0 && ok;
}

int Clamp(int v) { return v < 0 ? 0 : v > 100 ? 100 : v; }

// The shortest form of the value: the right number of decimal places, no trailing zeros, no dangling dot
// (0.45, 60, 937.5, 0.225).
std::string Fmt(double v) {
  char buf[64];
  snprintf(buf, sizeof buf, "%.4f", v);
  size_t len = strlen(buf);
  while (len > 0 && buf[len - 1] == '0') --len;
  while (len > 0 && buf[len - 1] == '.') --len;
  buf[len] = '\0';
  if (strcmp(buf, "-0") == 0) strcpy(buf, "0");
  return buf;
}

bool IsNameFirst(char c) {
  return isalpha(static_cast<unsigned char>(c)) || c == '_';
}

bool IsNameChar(char c) {
  return isalnum(static_cast<unsigned char>(c)) || c == '_';
}

bool IsNumberStart(char c) {
  return c == '-' || c == '+' || c == '.' || isdigit(static_cast<unsigned char>(c));
}

// One [A-Za-z_][A-Za-z0-9_]* token at `pos`; empty when it is not one.
std::string NameAt(const std::string& s, size_t pos) {
  if (pos >= s.size() || !IsNameFirst(s[pos])) return "";
  size_t end = pos;
  while (end < s.size() && IsNameChar(s[end])) ++end;
  return s.substr(pos, end - pos);
}

// The line's key: its first word, or "word1 word2" when the second word is also a name followed by numbers
// (the CAMERA_SHAKE_INPUT_MODIFIER Speed / Collision / ObjectImpact lines).
bool KeyOf(const std::string& line, std::string& key) {
  const size_t j = line.find_first_not_of(" \t");
  if (j == std::string::npos) return false;
  key = NameAt(line, j);
  if (key.empty()) return false;
  size_t k = line.find_first_not_of(" \t", j + key.size());
  if (k == std::string::npos) return true;
  const std::string second = NameAt(line, k);
  if (second.empty()) return true;
  const size_t m = line.find_first_not_of(" \t", k + second.size());
  if (m == std::string::npos || !IsNumberStart(line[m])) return true;
  key += " " + second;
  return true;
}

bool TargetsFor(const std::string& key, int look, int weight, const std::vector<double>& v0,
                std::vector<double>& out) {
  const auto lerp = [&](int pct, double target) { return v0[0] + (target - v0[0]) * (pct / 100.0); };
  if (look && !v0.empty()) {
    if (key == "VELOCITY_LOOKAT_FACTOR") return out = {lerp(look, 1.0)}, true;
    if (key == "YAW_LOOKAT_FACTOR") return out = {lerp(look, 0.5)}, true;
    if (key == "LOOKAT_HEAD_FRACTION_HEADING") return out = {lerp(look, 1)}, true;
    if (key == "LOOKAT_HEAD_SMOOTHING") return out = {lerp(look, 0.6)}, true;
    if (key == "LOOKAT_CLAMP_LEFT") return out = {lerp(look, 90)}, true;
    if (key == "LOOKAT_CLAMP_RIGHT") return out = {lerp(look, 90)}, true;
  }
  if (weight) {
    if (key == "NECK_SPRING_CONSTANTS_NEG" || key == "NECK_SPRING_CONSTANTS_POS" ||
        key == "HEAD_SPRING_CONSTANTS_NEG" || key == "HEAD_SPRING_CONSTANTS_POS") {
      const double s = 1 - 0.75 * weight / 100.0;
      out.resize(v0.size());
      for (size_t i = 0; i < v0.size(); ++i) out[i] = v0[i] * s;
      return true;
    }
  }
  return false;
}

}  // namespace

std::string HeadCamApply(const std::string& text, const HeadCamChoice& choice) {
  HeadCamChoice c;
  c.look = Clamp(choice.look);
  c.weight = Clamp(choice.weight);
  if (!c.look && !c.weight) return text;

  static const std::regex num(R"([-+]?(?:\d+\.\d*|\.\d+|\d+)(?:[eE][-+]?\d+)?)");
  std::string out;
  out.reserve(text.size());
  size_t pos = 0;
  while (pos < text.size()) {
    const size_t nl = text.find('\n', pos);
    std::string line = nl == std::string::npos ? text.substr(pos) : text.substr(pos, nl - pos);
    pos = nl == std::string::npos ? text.size() : nl + 1;

    std::string key;
    if (KeyOf(line, key)) {
      std::vector<double> v0;
      for (auto it = std::sregex_iterator(line.begin(), line.end(), num); it != std::sregex_iterator(); ++it)
        v0.push_back(std::stod(it->str()));
      std::vector<double> tv;
      if (!v0.empty() && TargetsFor(key, c.look, c.weight, v0, tv) && tv.size() == v0.size()) {
        std::string rebuilt;
        rebuilt.reserve(line.size());
        size_t prev = 0;
        int i = 0;
        for (auto it = std::sregex_iterator(line.begin(), line.end(), num); it != std::sregex_iterator(); ++it, ++i) {
          rebuilt.append(line, prev, it->position(0) - prev);
          rebuilt += Fmt(tv[i]);
          prev = it->position(0) + it->length(0);
        }
        rebuilt.append(line, prev, std::string::npos);
        line = rebuilt;
      }
    }

    out += line;
    if (nl != std::string::npos) out += '\n';
  }
  return out;
}

std::wstring BuildHeadCam(const std::wstring& update_dir, const std::wstring& base_dir, const std::wstring& out_dir,
                          const HeadCamChoice& choice, bool& active) {
  active = false;
  const std::wstring file =
      L"data\\cameras\\vehicle\\driver_head_cameras\\include\\driver_head_camera_params.txt";
  const std::wstring path = out_dir + L"\\" + file;
  if (choice.Default()) {
    DeleteFileW(path.c_str());
    return L"";
  }

  std::vector<uint8_t> buf;
  std::string err = DatxExtract(update_dir, base_dir, "data/cameras/vehicle/driver_head_cameras/include/driver_head_camera_params.txt", buf);
  if (!err.empty()) return Widen(err);
  std::string text(reinterpret_cast<const char*>(buf.data()), buf.size());
  const std::string out = HeadCamApply(text, choice);
  // Written when the copy in out_dir is not already this one (it may hold the values of an earlier choice).
  std::string old;
  if (FILE* f = _wfopen(path.c_str(), L"rb")) {
    char chunk[4096];
    for (size_t n; (n = fread(chunk, 1, sizeof chunk, f)) > 0;) old.append(chunk, n);
    fclose(f);
  }
  if (old != out) {
    const size_t last = path.find_last_of(L"\\");
    MakeDirs(last == std::wstring::npos ? L"." : path.substr(0, last));
    if (!WriteFile(path, out.data(), out.size())) return L"the head camera file could not be written";
  }
  active = true;
  return L"";
}
