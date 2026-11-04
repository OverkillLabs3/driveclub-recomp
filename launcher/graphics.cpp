#include "graphics.h"

#include <windows.h>

#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "datx.h"

// The game reads its graphics quality from viewportquality.xml (one <Configuration> per kind of view: race
// camera, in-car, photo mode, mirrors, light probes ...; a configuration with a parent starts as a copy of it)
// and the car LOD bias from vehiclelodgroupdata.xml. Both are plain text inside global.rpk. The game looks for a
// loose /app0/global.rpk before it reads the archive, so a copy with other values, served from a mods folder,
// replaces the original.
//
// The resource pack lists every file with its size, so the edited text must keep the original length: comments
// and indentation are dropped to make room and the rest is filled with spaces before the closing tag.

namespace {

constexpr const char* kVersion = "graphics-2";

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

struct Attr {
  const char* name;
  const char* value;
};

// One change: in configuration `conf`, the element `element` (or, with `tag`, the <Settings> of the
// <SolQualityTag name="tag">) gets these attribute values. Missing elements and attributes are added. With `match`
// (for example split="3"), only the element that holds that text is changed, and it must exist.
// A configuration with a parent may only name a <SolQualityTag> that its parent chain has: the game writes through a
// null pointer for any other.
struct Edit {
  const char* conf;
  const char* element;
  const char* tag;
  std::vector<Attr> attrs;
  const char* match = nullptr;
};

// Sets the attributes of one element "<Name ... />".
std::string SetAttrs(std::string elem, const std::vector<Attr>& attrs) {
  for (const Attr& a : attrs) {
    const std::string key = std::string(" ") + a.name + "=\"";
    const size_t at = elem.find(key);
    if (at != std::string::npos) {
      const size_t from = at + key.size(), to = elem.find('"', from);
      elem.replace(from, to - from, a.value);
    } else {
      size_t end = elem.rfind("/>");
      while (end > 0 && (elem[end - 1] == ' ' || elem[end - 1] == '\t' || elem[end - 1] == '\r' || elem[end - 1] == '\n')) --end;
      elem.replace(end, elem.size() - end, std::string(" ") + a.name + "=\"" + a.value + "\" />");
    }
  }
  return elem;
}

std::string NewElement(const char* name, const std::vector<Attr>& attrs) {
  std::string s = std::string("<") + name;
  for (const Attr& a : attrs) s += std::string(" ") + a.name + "=\"" + a.value + "\"";
  return s + " />\n";
}

bool ApplyEdit(std::string& xml, const Edit& e) {
  const std::string open = std::string("<Configuration name=\"") + e.conf + "\"";
  const size_t cs = xml.find(open);
  if (cs == std::string::npos) return false;
  const size_t ce = xml.find("</Configuration>", cs);
  if (ce == std::string::npos) return false;
  std::string block = xml.substr(cs, ce - cs);  // without the closing tag
  if (e.tag) {
    const std::string tag_open = std::string("<SolQualityTag name=\"") + e.tag + "\">";
    const size_t ts = block.find(tag_open);
    if (ts == std::string::npos) {
      block += "<SolQualityTag name=\"" + std::string(e.tag) + "\">" + NewElement("Settings", e.attrs) + "</SolQualityTag>\n";
    } else {
      const size_t te = block.find("</SolQualityTag>", ts);
      const size_t ss = block.find("<Settings", ts);
      const size_t se = ss == std::string::npos ? ss : block.find("/>", ss);
      if (te == std::string::npos || ss == std::string::npos || se == std::string::npos || se > te) return false;
      block.replace(ss, se + 2 - ss, SetAttrs(block.substr(ss, se + 2 - ss), e.attrs));
    }
  } else {
    // "<Shadows " is not "<ShadowSplit": the name must end at a space.
    const std::string el_open = std::string("<") + e.element;
    size_t es = std::string::npos;
    for (size_t at = block.find(el_open); at != std::string::npos; at = block.find(el_open, at + 1)) {
      const char next = at + el_open.size() < block.size() ? block[at + el_open.size()] : '\0';
      if (next != ' ' && next != '\t' && next != '\r' && next != '\n') continue;
      const size_t ee = block.find("/>", at);
      if (e.match && (ee == std::string::npos || block.substr(at, ee - at).find(e.match) == std::string::npos)) continue;
      es = at;
      break;
    }
    if (es == std::string::npos) {
      if (e.match) return false;
      block += NewElement(e.element, e.attrs);
    } else {
      const size_t ee = block.find("/>", es);
      if (ee == std::string::npos) return false;
      block.replace(es, ee + 2 - es, SetAttrs(block.substr(es, ee + 2 - es), e.attrs));
    }
  }
  xml.replace(cs, ce - cs, block);
  return true;
}

// Drops comments and collapses white space (one line break between tags, one space elsewhere).
std::string Compact(const std::string& in) {
  std::string s;
  s.reserve(in.size());
  for (size_t i = 0; i < in.size();) {
    if (in.compare(i, 4, "<!--") == 0) {
      const size_t end = in.find("-->", i + 4);
      i = end == std::string::npos ? in.size() : end + 3;
      continue;
    }
    s += in[i++];
  }
  std::string out;
  out.reserve(s.size());
  for (size_t i = 0; i < s.size();) {
    const char c = s[i];
    if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
      size_t j = i;
      while (j < s.size() && (s[j] == ' ' || s[j] == '\t' || s[j] == '\r' || s[j] == '\n')) ++j;
      const bool between_tags = !out.empty() && out.back() == '>' && j < s.size() && s[j] == '<';
      out += between_tags ? '\n' : ' ';
      i = j;
      continue;
    }
    out += c;
    ++i;
  }
  return out;
}

// Replaces the XML document whose root element is `root` inside `pack` by its changed and compacted text, keeping
// the length. Returns "" or the problem.
std::string ReplaceDocument(std::vector<uint8_t>& pack, const std::string& root, const std::function<bool(std::string&)>& change) {
  const std::string open = "<" + root + ">", close = "</" + root + ">";
  const char* base = reinterpret_cast<const char*>(pack.data());
  const std::string_view all(base, pack.size());
  const size_t r = all.find(open);
  if (r == std::string_view::npos) return root + " not found";
  const size_t decl = all.rfind("<?xml", r);
  const size_t start = decl != std::string_view::npos && r - decl < 64 ? decl : r;
  const size_t end_tag = all.find(close, r);
  if (end_tag == std::string_view::npos) return root + " is cut short";
  const size_t end = end_tag + close.size();
  std::string xml(all.substr(start, end - start));
  const size_t old_size = xml.size();
  if (!change(xml)) return root + " has an unexpected layout";
  xml = Compact(xml);
  if (xml.size() < close.size() || xml.compare(xml.size() - close.size(), close.size(), close) != 0) return root + " has an unexpected end";
  if (xml.size() > old_size) return root + ": the changes do not fit";
  xml.insert(xml.size() - close.size(), old_size - xml.size(), ' ');
  std::memcpy(pack.data() + start, xml.data(), xml.size());
  return "";
}

// The values: between the race camera's and photo mode's; the developers' own pre/post-race camera values for the
// scenery (their comment: "relaxed cull thresholds so we see less popping"). Tested on an RTX 3070 Laptop.
std::vector<Edit> Edits(const GraphicsChoice& c) {
  std::vector<Edit> e;
  if (c.own_car) {
    // Race views draw the player's car one detail step down (minimumLod 1); the HQ, photo and car select views use 0.
    e.push_back({"default", nullptr, "viewed_vehicle", {{"minimumLod", "0"}, {"cullThreshold", "0.0"}}});
    e.push_back({"default", "CabCharacter", nullptr, {{"lodBias", "0.0"}}});
  }
  if (c.opponents) {
    e.push_back({"default", nullptr, "unviewed_vehicle", {{"lodBias", "-0.8"}, {"minimumLod", "-1"}, {"cullThreshold", "0.0"}}});
  }
  if (c.scenery == 1) {
    e.push_back({"default", nullptr, "static_objects", {{"lodBias", "-2.5"}, {"cullThreshold", "0.005"}, {"shadowCullThreshold", "0.005"}}});
    e.push_back({"default", nullptr, "ground", {{"lodBias", "-2.5"}, {"cullThreshold", "0.005"}, {"shadowCullThreshold", "0.005"}}});
    e.push_back({"default", nullptr, "dynamic_objects", {{"lodBias", "-1.5"}, {"cullThreshold", "0.005"}, {"shadowCullThreshold", "0.005"}}});
    e.push_back({"default", nullptr, "transient", {{"lodBias", "-1.5"}, {"shadowCullThreshold", "0.005"}}});
    e.push_back({"OnePlayer2DHQ", nullptr, "ground", {{"cullThreshold", "0.005"}}});
  } else if (c.scenery == 2) {
    // Photo mode's values.
    e.push_back({"default", nullptr, "static_objects", {{"lodBias", "-4"}, {"cullThreshold", "0.001"}, {"shadowCullThreshold", "0.001"}}});
    e.push_back({"default", nullptr, "ground", {{"lodBias", "-4"}, {"cullThreshold", "0.001"}, {"shadowCullThreshold", "0.001"}}});
    e.push_back({"default", nullptr, "dynamic_objects", {{"lodBias", "-4"}, {"cullThreshold", "0.001"}, {"shadowCullThreshold", "0.001"}}});
    e.push_back({"default", nullptr, "transient", {{"lodBias", "-3"}, {"shadowCullThreshold", "0.001"}}});
    e.push_back({"OnePlayer2DHQ", nullptr, "ground", {{"cullThreshold", "0.001"}}});
  }
  if (c.high_quality_ssr) {
    e.push_back({"default", "General", nullptr, {{"highQualitySSR", "true"}}});
    e.push_back({"OnePlayer2D", "General", nullptr, {{"highQualitySSR", "true"}}});
  }
  // Shadows: the race views filter the nearest of the four shadow cascades (4 m) with 32 samples and the others
  // (to 11, 42 and 256 m) with the engine's 8. Set in "default", every race view without its own value follows.
  if (c.shadows) {
    const char* n = c.shadows == 1 ? "16" : "32";
    for (const char* split : {"split=\"1\"", "split=\"2\"", "split=\"3\""})
      e.push_back({"default", "ShadowSplit", nullptr, {{"numLookups", n}}, split});
  }
  if (c.particles) {
    // Race views drop distant particles (spray, smoke, dust); the replay camera keeps them.
    e.push_back({"OnePlayer2D", "Particles", nullptr, {{"killDistantParticles", "false"}}});
    e.push_back({"OnePlayer2DInCar", "Particles", nullptr, {{"killDistantParticles", "false"}}});
    e.push_back({"default", "ScreenRush", nullptr, {{"ParticleLimitScalar", "1.5"}}});
  }
  if (!c.motion_blur) {
    e.push_back({"default", "General", nullptr, {{"enableMotionBlur", "false"}}});
    e.push_back({"OnePlayer2D", "General", nullptr, {{"enableMotionBlur", "false"}}});
  }
  if (!c.lens_dust) e.push_back({"default", "General", nullptr, {{"lensdust", "false"}}});
  if (!c.heat_haze) e.push_back({"OnePlayer2D", "General", nullptr, {{"enableMirage", "false"}}});
  // The game AA adds an FXAA pass after its temporal AA (photo mode turns it off).
  if (!c.fxaa) e.push_back({"default", "AA", nullptr, {{"enableFXAA", "false"}}});
  if (!c.dynamic_res) e.push_back({"default", "ViewportScaling", nullptr, {{"variableScaling", "false"}}});
  return e;
}

std::string Describe(const GraphicsChoice& c) {
  char s[200];
  // The runtime's benchmark report reads the first five (runtime_benchmark.c).
  snprintf(s, sizeof s,
           "car %d opponents %d scenery %d reflections %d dynres %d shadows %d particles %d blur %d dust %d haze %d fxaa %d",
           c.own_car, c.opponents, c.scenery, c.high_quality_ssr, c.dynamic_res, c.shadows, c.particles, c.motion_blur,
           c.lens_dust, c.heat_haze, c.fxaa);
  return s;
}

}  // namespace

std::wstring BuildGraphics(const std::wstring& update_dir, const std::wstring& base_dir, const std::wstring& out_dir,
                           const GraphicsChoice& choice, bool& active) {
  active = false;
  const std::wstring pack_path = out_dir + L"\\global.rpk", stamp_path = out_dir + L"\\stamp.txt";
  if (choice.Default()) {
    DeleteFileW(pack_path.c_str());
    DeleteFileW(stamp_path.c_str());
    return L"";
  }
  char stamp[256];
  snprintf(stamp, sizeof stamp, "%s %llx %llx %s", kVersion, (unsigned long long)FileStamp(update_dir + L"\\game.ndx"),
           (unsigned long long)FileStamp(base_dir + L"\\game.ndx"), Describe(choice).c_str());
  if (ReadText(stamp_path) == stamp && GetFileAttributesW(pack_path.c_str()) != INVALID_FILE_ATTRIBUTES) {
    active = true;
    return L"";
  }
  DeleteFileW(stamp_path.c_str());
  DeleteFileW(pack_path.c_str());

  std::vector<uint8_t> pack;
  std::string err = DatxExtract(update_dir, base_dir, "global.rpk", pack);
  if (!err.empty()) return Widen("global.rpk: " + err);
  const std::vector<Edit> edits = Edits(choice);
  err = ReplaceDocument(pack, "ViewportQuality", [&](std::string& xml) {
    for (const Edit& e : edits) {
      if (!ApplyEdit(xml, e)) return false;
    }
    return true;
  });
  if (!err.empty()) return Widen(err);
  if (choice.opponents) {
    // Every part of every car (body, technical, interior, wheels, other) one detail step closer.
    err = ReplaceDocument(pack, "lodgroupdata", [](std::string& xml) {
      size_t n = 0;
      for (size_t at; (at = xml.find("lodBias=\"0.000000\"")) != std::string::npos; ++n) xml.replace(at, 18, "lodBias=\"-1.0\"");
      return n > 0;
    });
    if (!err.empty()) return Widen(err);
  }
  MakeDirs(out_dir);
  if (!WriteFile(pack_path, pack.data(), pack.size()) || !WriteFile(stamp_path, stamp, strlen(stamp))) {
    DeleteFileW(pack_path.c_str());
    return L"the graphics file could not be written";
  }
  active = true;
  return L"";
}
