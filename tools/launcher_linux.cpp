// DriveClub Recomp - native Linux launcher.
//
// The AppImage has no Win32/WPF launcher. This window is the Linux equivalent: it picks the
// game folders, edits the settings the runtime reads (BB_* variables, camera.ini, picture.ini,
// driveclub.ini and patches_enabled.txt), prepares the game and starts it.
//
// The launcher only writes inside DRIVECLUB_DATA_DIR (default ~/.local/share/driveclub-recomp),
// never inside the game folders.
//
//   driveclub-launcher              window
//   driveclub-launcher --version
//   driveclub-launcher --help

#include <SDL3/SDL.h>
#include <SDL3/SDL_dialog.h>

#include "imgui.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_sdlrenderer3.h"
#include "prepare.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cfloat>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

namespace fs = std::filesystem;

namespace {

// ------------------------------------------------------------------------------------------
// look
// ------------------------------------------------------------------------------------------
const ImVec4 kBg(0.063f, 0.071f, 0.086f, 1.0f);
const ImVec4 kHeaderBg(0.043f, 0.051f, 0.063f, 1.0f);
const ImVec4 kCard(0.094f, 0.106f, 0.129f, 1.0f);
const ImVec4 kCardLine(0.153f, 0.169f, 0.200f, 1.0f);
const ImVec4 kText(0.910f, 0.918f, 0.929f, 1.0f);
const ImVec4 kMuted(0.545f, 0.576f, 0.631f, 1.0f);
const ImVec4 kTrack(0.129f, 0.145f, 0.176f, 1.0f);
const ImVec4 kTrackHover(0.176f, 0.196f, 0.235f, 1.0f);
const ImVec4 kAccent(0.878f, 0.251f, 0.149f, 1.0f);
const ImVec4 kAccentHover(0.941f, 0.329f, 0.227f, 1.0f);
const ImVec4 kOff(0.247f, 0.271f, 0.322f, 1.0f);
const ImVec4 kWhite(0.961f, 0.965f, 0.973f, 1.0f);
const ImVec4 kOk(0.353f, 0.784f, 0.471f, 1.0f);
const ImVec4 kBad(0.941f, 0.376f, 0.329f, 1.0f);

constexpr float kHeaderH = 88.0f;
constexpr float kTabsH = 52.0f;
constexpr float kFooterH = 82.0f;
constexpr float kMargin = 24.0f;

// ------------------------------------------------------------------------------------------
// paths and state
// ------------------------------------------------------------------------------------------
struct Paths {
  fs::path appdir, share, data, data_pointer, default_data, patch_xml, font, probe, apprun,
      appimage, version;
};

Paths g_paths;
std::string g_version = "dev";
int g_display_hz = 0;
bool g_quit = false;
SDL_Window* g_window = nullptr;
ImFont* g_font_body = nullptr;
ImFont* g_font_title = nullptr;
ImFont* g_font_small = nullptr;

enum Tab { kTabGame, kTabDisplay, kTabDriving, kTabPatches, kTabLogs, kTabCount };
const char* const kTabName[kTabCount] = {"GAME", "DISPLAY", "DRIVING", "PATCHES", "LOGS"};
int g_tab = kTabGame;
bool g_scroll_top = false;

// ------------------------------------------------------------------------------------------
// settings
// ------------------------------------------------------------------------------------------
struct Settings {
  std::string update, base;
  int frame_rate = 1;  // 0 30, 1 60 deltatime, 2 60 fixed tick, 3 unlocked VRR, 4 unlimited
  int fps_cap = 0;     // 0 = display refresh rate - 3
  int output = 0;      // Default, 720p, 1440p, 1800p, 4K, 5K, 8K
  int render = 0;      // Default, 720p, 1440p, 1800p, 4K, 8K
  int wide = 0;        // Off + the widescreen patches
  bool fov_fix = false;
  int aa = 0;          // 0 game, 1 FSR 3.1, 2 DLSS
  int upscale = 0;     // Native, Quality, Balanced, Performance, Ultra Performance
  int sharpness = 80;
  bool hdr = false;
  int hdr_paper = 350;
  int hdr_peak = 1000;
  int aniso = 0;       // Automatic, Off, 2x, 4x, 8x, 16x
  int mirror = 0;      // Live, Static
  int language = 1;    // console language number
  char name[64] = "Player";
  bool fullscreen = true;
  bool show_fps = false;
  bool show_pedals = false;
  int lock_horizon = 0;  // Off, Cockpit, All views
  int camera_shake = 100;
  bool camera_patch = true;
  int fov = 100;
  int brightness = 0;
  int contrast = 100;
  int gamma = 240;
  int gearbox = 0;        // In-game, Automatic, Manual
  int brake_assist = 0;   // In-game, Off, Low, Medium, High
  int hardcore = 0;       // In-game, Off, On
  int wheel_assist = 0;   // In-game, Off, On
  std::map<std::string, bool> extras;  // patches not managed by the rows above
  bool dirty = false;
};

Settings g;
bool g_update_ok = false, g_base_ok = false, g_merged = false;
std::string g_update_problem, g_base_problem;
char g_update_buf[1024] = {};
char g_base_buf[1024] = {};

// ------------------------------------------------------------------------------------------
// patches
// ------------------------------------------------------------------------------------------
struct Patch {
  std::string name, note;
  bool always = false;
  bool default_on = false;
};

std::vector<Patch> g_patches;
std::set<std::string> g_selected;
bool g_has_selection = false;

constexpr const char* kFpsDelta = "60 FPS with deltatime";
constexpr const char* kFpsFixed = "60 FPS with fixed tickrate";
constexpr const char* kFovFix = "FOV fix for wide resolutions";
constexpr const char* kBlurPatch = "Disable motion blur and depth of field";

struct Choice {
  const char* label;
  const char* patch;
};

const Choice kOutput[7] = {
    {"Default (1080p)", ""},
    {"720p (1280x720)", "720p [1280x720] Output resolution"},
    {"1440p (2560x1440)", "1440p [2560x1440] Output resolution"},
    {"1800p (3200x1800)", "1800p [3200x1800] Output resolution"},
    {"4K (3840x2160)", "4K [3840x2160] Output resolution"},
    {"5K (5120x2880)", "5K [5120x2880] Output resolution"},
    {"8K (7680x4320)", "8K [7680x4320] Output resolution"},
};

const Choice kRender[6] = {
    {"Default (1080p)", ""},
    {"720p (1280x720)", "720p [1280x720] Internal resolution"},
    {"1440p (2560x1440)", "1440p [2560x1440] Internal resolution [DEPTH BUG]"},
    {"1800p (3200x1800)", "1800p [3200x1800] Internal resolution [DEPTH BUG]"},
    {"4K (3840x2160)", "4K [3840x2160] Internal resolution [DEPTH BUG]"},
    {"8K (7680x4320)", "8K [7680x4320] Internal resolution [DEPTH BUG]"},
};

const Choice kWide[7] = {
    {"Off", ""},
    {"2560x1080 (21:9)", "1080p 21:9 [2560x1080] Internal + Output resolution [DEPTH BUG]"},
    {"3440x1080 (32:10)", "1080p 32:10 [3440x1080] Internal + Output resolution [DEPTH BUG]"},
    {"3840x1080 (32:9)", "1080p 32:9 [3840x1080] Internal + Output resolution [DEPTH BUG]"},
    {"3440x1440 (21:9)", "1440p 21:9 [3440x1440] Internal + Output resolution [DEPTH BUG]"},
    {"5120x1440 (32:9)", "1440p 32:9 [5120x1440] Internal + Output resolution [DEPTH BUG]"},
    {"5120x2160 (21:9)", "2160p 21:9 [5120x2160] Internal + Output resolution [DEPTH BUG]"},
};

const char* const kFrameRateName[5] = {"30 FPS", "60 FPS (deltatime)", "60 FPS (fixed tick)",
                                       "Unlocked (VRR)", "Unlimited"};
const char* const kOutputName[7] = {"Default (1080p)", "720p (1280x720)", "1440p (2560x1440)",
                                    "1800p (3200x1800)", "4K (3840x2160)", "5K (5120x2880)",
                                    "8K (7680x4320)"};
const char* const kRenderName[6] = {"Default (1080p)", "720p (1280x720)", "1440p (2560x1440)",
                                    "1800p (3200x1800)", "4K (3840x2160)", "8K (7680x4320)"};
const char* const kWideName[7] = {"Off", "2560x1080 (21:9)", "3440x1080 (32:10)", "3840x1080 (32:9)",
                                  "3440x1440 (21:9)", "5120x1440 (32:9)", "5120x2160 (21:9)"};
const char* const kAaName[3] = {"Game (original)", "FSR 3.1", "DLSS (RTX)"};
const char* const kUpscaleName[5] = {"Native", "Quality", "Balanced", "Performance", "Ultra Performance"};
const char* const kAnisoName[6] = {"Automatic", "Off", "2x", "4x", "8x", "16x"};
const int kAnisoValue[6] = {0, 0, 2, 4, 8, 16};
const char* const kMirrorName[2] = {"Live", "Static (faster)"};
const char* const kHorizonName[3] = {"Off", "Cockpit", "All views"};
const char* const kGearboxName[3] = {"In-game", "Automatic", "Manual"};
const char* const kBrakeName[5] = {"In-game", "Off", "Low", "Medium", "High"};
const char* const kOnOffName[3] = {"In-game", "Off", "On"};
const char* const kLanguageName[21] = {
    "Japanese", "English (US)", "French", "Spanish", "German", "Italian", "Dutch",
    "Portuguese", "Russian", "Korean", "Chinese (Traditional)", "Chinese (Simplified)",
    "Finnish", "Swedish", "Danish", "Norwegian", "Polish", "Portuguese (Brazil)",
    "English (UK)", "Turkish", "Spanish (Mexico)"};

// ------------------------------------------------------------------------------------------
// forward declarations
// ------------------------------------------------------------------------------------------
void SetStatus(const std::string& text, bool error);
void BeginDataFolderChange(const fs::path& path);
void Sanitize();
void SyncFolderBuffers();
void ValidateFolders();
bool FoldersOk();

// ------------------------------------------------------------------------------------------
// small helpers
// ------------------------------------------------------------------------------------------
std::string Trim(std::string s) {
  while (!s.empty() && (s.back() == '\r' || s.back() == '\n' || s.back() == ' ' || s.back() == '\t')) s.pop_back();
  size_t first = 0;
  while (first < s.size() && (s[first] == ' ' || s[first] == '\t')) ++first;
  return first ? s.substr(first) : s;
}

bool ParseInt(const std::map<std::string, std::string>& kv, const char* key, int& out) {
  const auto it = kv.find(key);
  if (it == kv.end() || it->second.empty()) return false;
  char* end = nullptr;
  const long value = std::strtol(it->second.c_str(), &end, 10);
  if (end == it->second.c_str()) return false;
  out = int(value);
  return true;
}

bool ParseBool(const std::map<std::string, std::string>& kv, const char* key, bool& out) {
  const auto it = kv.find(key);
  if (it == kv.end()) return false;
  const std::string value = Trim(it->second);
  out = value == "1" || value == "true" || value == "yes" || value == "on";
  return true;
}

bool Exists(const fs::path& path) {
  std::error_code ec;
  return fs::exists(path, ec);
}

bool IsFile(const fs::path& path) {
  std::error_code ec;
  return fs::is_regular_file(path, ec);
}

bool IsDir(const fs::path& path) {
  std::error_code ec;
  return fs::is_directory(path, ec);
}

std::string ReadFile(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return {};
  std::ostringstream out;
  out << in.rdbuf();
  return out.str();
}

std::string ReadTail(const fs::path& path, std::size_t max) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return {};
  in.seekg(0, std::ios::end);
  const std::streamoff size = in.tellg();
  if (size <= 0) return {};
  const std::streamoff start = size > std::streamoff(max) ? size - std::streamoff(max) : 0;
  in.seekg(start, std::ios::beg);
  std::ostringstream out;
  out << in.rdbuf();
  return out.str();
}

std::vector<std::string> ReadLines(const fs::path& path) {
  std::vector<std::string> lines;
  std::ifstream in(path, std::ios::binary);
  for (std::string line; std::getline(in, line);) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    lines.push_back(line);
  }
  return lines;
}

bool WriteFileAtomic(const fs::path& path, const std::string& text) {
  std::error_code ec;
  fs::create_directories(path.parent_path(), ec);
  const fs::path temp = path.string() + ".launcher-tmp";
  {
    std::ofstream out(temp, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out.write(text.data(), std::streamsize(text.size()));
    out.flush();
    if (!out) {
      std::error_code ignored;
      fs::remove(temp, ignored);
      return false;
    }
  }
  fs::rename(temp, path, ec);
  if (ec) {
    std::error_code ignored;
    fs::remove(temp, ignored);
    return false;
  }
  return true;
}

std::map<std::string, std::string> ReadIni(const fs::path& path) {
  std::map<std::string, std::string> kv;
  for (const std::string& line : ReadLines(path)) {
    const std::string text = Trim(line);
    if (text.empty() || text[0] == '#' || text[0] == ';') continue;
    const size_t eq = text.find('=');
    if (eq == std::string::npos) continue;
    kv[Trim(text.substr(0, eq))] = Trim(text.substr(eq + 1));
  }
  return kv;
}

std::string IniValue(const fs::path& path, const char* key) {
  const auto kv = ReadIni(path);
  const auto it = kv.find(key);
  return it == kv.end() ? std::string() : it->second;
}

bool SetIniValue(const fs::path& path, const char* key, const std::string& value) {
  std::vector<std::string> lines = ReadLines(path);
  const std::string wanted(key);
  bool replaced = false;
  for (std::string& line : lines) {
    const std::string text = Trim(line);
    if (text.empty() || text[0] == '#' || text[0] == ';') continue;
    const size_t eq = text.find('=');
    if (eq == std::string::npos) continue;
    if (Trim(text.substr(0, eq)) == wanted) {
      line = wanted + "=" + value;
      replaced = true;
    }
  }
  if (!replaced) lines.push_back(wanted + "=" + value);
  std::string text;
  for (const std::string& line : lines) text += line + "\n";
  return WriteFileAtomic(path, text);
}

std::string ShellQuote(const std::string& value) {
  std::string out = "'";
  for (char c : value) {
    if (c == '\'') out += "'\\''";
    else out += c;
  }
  out += "'";
  return out;
}

std::string Timestamp() {
  const std::time_t now = std::time(nullptr);
  std::tm tm{};
  localtime_r(&now, &tm);
  char text[32];
  std::strftime(text, sizeof text, "%Y-%m-%d %H:%M:%S", &tm);
  return text;
}

void AppendLog(const std::string& text) {
  if (g_paths.data.empty()) return;
  std::error_code ec;
  fs::create_directories(g_paths.data / "logs", ec);
  std::ofstream out(g_paths.data / "logs" / "launcher.log", std::ios::binary | std::ios::app);
  if (out) out << Timestamp() << " " << text << "\n";
}

// ------------------------------------------------------------------------------------------
// paths, config and environment
// ------------------------------------------------------------------------------------------
fs::path XdgConfigDir() {
  const char* xdg = std::getenv("XDG_CONFIG_HOME");
  if (xdg && *xdg) return fs::path(xdg) / "driveclub-recomp";
  const char* home = std::getenv("HOME");
  return fs::path(home && *home ? home : ".") / ".config/driveclub-recomp";
}

bool WritableDir(const fs::path& path) {
  std::error_code ec;
  return fs::is_directory(path, ec) && access(path.c_str(), W_OK) == 0;
}

fs::path NormalizePath(const fs::path& path) {
  std::error_code ec;
  fs::path result = fs::weakly_canonical(path, ec);
  if (!ec) return result;
  result = fs::absolute(path, ec);
  return ec ? path.lexically_normal() : result.lexically_normal();
}

bool SamePath(const fs::path& a, const fs::path& b) {
  return !a.empty() && !b.empty() && NormalizePath(a) == NormalizePath(b);
}

bool PathStartsWith(const fs::path& path, const fs::path& prefix) {
  if (path.empty() || prefix.empty()) return false;
  const fs::path p = NormalizePath(path);
  const fs::path q = NormalizePath(prefix);
  auto pi = p.begin();
  for (auto qi = q.begin(); qi != q.end(); ++qi, ++pi) {
    if (pi == p.end() || *pi != *qi) return false;
  }
  return true;
}

bool DirHasEntries(const fs::path& path) {
  std::error_code ec;
  if (!fs::is_directory(path, ec)) return false;
  const bool empty = fs::is_empty(path, ec);
  return ec ? true : !empty;
}

bool HasFiles(const fs::path& path) {
  std::error_code ec;
  if (!fs::is_directory(path, ec)) return false;
  for (fs::recursive_directory_iterator it(path, fs::directory_options::skip_permission_denied,
                                           ec),
       end;
       it != end; it.increment(ec)) {
    if (ec) return true;
    std::error_code type_ec;
    if (it->is_regular_file(type_ec)) return true;
  }
  return ec.value() != 0;
}

bool HasLegacyData(const fs::path& path) {
  return IsFile(path / "launcher.ini") || IsFile(path / "config") ||
         IsFile(path / "prep/boot-linked.bin") || HasFiles(path / "user") ||
         HasFiles(path / "mods");
}

bool EnsureDataDir(const fs::path& path, std::string& error) {
  std::error_code ec;
  fs::create_directories(path, ec);
  if (ec) {
    error = "Could not create " + path.string() + ": " + ec.message();
    return false;
  }
  if (access(path.c_str(), W_OK) != 0) {
    error = "No write permission for " + path.string();
    return false;
  }
  return true;
}

fs::path DataPointerPath() {
  if (const char* env = std::getenv("DRIVECLUB_DATA_POINTER"); env && *env) return env;
  if (!g_paths.appimage.empty()) {
    const fs::path dir = g_paths.appimage.parent_path();
    if (WritableDir(dir)) return dir / "data-path";
  }
  return XdgConfigDir() / "data-path";
}

fs::path ReadDataPointer(const fs::path& pointer) {
  if (!IsFile(pointer)) return {};
  const std::string text = Trim(ReadFile(pointer));
  if (text.empty()) return {};
  fs::path path = text;
  if (path.is_relative()) path = pointer.parent_path() / path;
  return NormalizePath(path);
}

bool WriteDataPointer(const fs::path& path) {
  if (g_paths.data_pointer.empty()) return false;
  if (!g_paths.default_data.empty() && SamePath(path, g_paths.default_data)) {
    std::error_code ec;
    fs::remove(g_paths.data_pointer, ec);
    return !ec;
  }
  return WriteFileAtomic(g_paths.data_pointer, path.string() + "\n");
}

void InitPaths() {
  std::error_code ec;
  const fs::path self = fs::read_symlink("/proc/self/exe", ec);
  const char* env_appdir = std::getenv("DRIVECLUB_APPDIR");
  if (env_appdir && *env_appdir) g_paths.appdir = env_appdir;
  if (g_paths.appdir.empty()) {
    g_paths.appdir = self.empty() ? fs::current_path() : self.parent_path().parent_path();
  }
  const char* env_share = std::getenv("DRIVECLUB_SHARE");
  if (env_share && *env_share) g_paths.share = env_share;
  if (g_paths.share.empty()) g_paths.share = g_paths.appdir / "usr/share/driveclub-recomp";
  g_paths.patch_xml = g_paths.share / "patches/DriveClub.xml";
  const char* env_font = std::getenv("DRIVECLUB_FONT");
  if (env_font && *env_font) g_paths.font = env_font;
  if (g_paths.font.empty()) g_paths.font = g_paths.share / "fonts/001futura-webfont.ttf";
  g_paths.probe = g_paths.appdir / "usr/bin/driveclub-probe";
  g_paths.apprun = g_paths.appdir / "AppRun";
  g_paths.version = g_paths.share / "version.txt";
  if (const char* image = std::getenv("DRIVECLUB_APPIMAGE")) g_paths.appimage = image;
  if (const char* default_env = std::getenv("DRIVECLUB_DEFAULT_DATA_DIR")) {
    if (*default_env) g_paths.default_data = default_env;
  } else if (!g_paths.appimage.empty()) {
    g_paths.default_data = g_paths.appimage.parent_path() / "data";
  }
  g_paths.data_pointer = DataPointerPath();

  const char* data = std::getenv("DRIVECLUB_DATA_DIR");
  if (data && *data) {
    g_paths.data = data;
  } else {
    const fs::path saved = ReadDataPointer(g_paths.data_pointer);
    const char* xdg = std::getenv("XDG_DATA_HOME");
    const fs::path xdg_data = xdg && *xdg
                                  ? fs::path(xdg) / "driveclub-recomp"
                                  : fs::path(std::getenv("HOME") && *std::getenv("HOME")
                                                 ? std::getenv("HOME")
                                                 : ".") /
                                        ".local/share/driveclub-recomp";
    const bool alongside_ok =
        !g_paths.default_data.empty() &&
        (IsDir(g_paths.default_data) ? WritableDir(g_paths.default_data)
                                     : WritableDir(g_paths.default_data.parent_path()));
    if (!saved.empty()) g_paths.data = saved;
    else if (alongside_ok && (IsDir(g_paths.default_data) || !HasLegacyData(xdg_data)))
      g_paths.data = g_paths.default_data;
    else
      g_paths.data = xdg_data;
  }
  fs::create_directories(g_paths.data, ec);
  fs::create_directories(g_paths.data / "logs", ec);
  fs::create_directories(g_paths.data / "user", ec);
  fs::create_directories(g_paths.data / "mods", ec);
  fs::create_directories(g_paths.data / "prep", ec);

  g_version = Trim(ReadFile(g_paths.version));
  if (g_version.empty()) g_version = "dev";
}

fs::path LauncherIni() { return g_paths.data / "launcher.ini"; }
fs::path LegacyConfig() { return g_paths.data / "config"; }
fs::path PatchesEnabled() { return g_paths.data / "user/patches_enabled.txt"; }
fs::path LaunchEnv() { return g_paths.data / "launch.env"; }
fs::path CameraIni() { return g_paths.data / "user/camera.ini"; }
fs::path DriveClubIni() { return g_paths.data / "user/driveclub.ini"; }
fs::path HdrIni() { return g_paths.data / "user/hdr.ini"; }
fs::path PictureIni() { return g_paths.data / "user/picture.ini"; }

std::string EffectiveUpdate() { return (g.update.empty() ? g.base : g.update); }

std::string SfoTitle(const fs::path& folder) {
  return SfoField(folder / "sce_sys" / "param.sfo", "TITLE_ID");
}

bool IsDriveClub(const fs::path& folder) {
  const std::string title = SfoTitle(folder);
  return title == "CUSA00093" || title == "CUSA00003";
}

bool UpdateLooksRight(const fs::path& folder) {
  if (folder.empty() || !IsDir(folder)) return false;
  if (!IsFile(folder / "eboot.bin") || !IsFile(folder / "sce_sys/param.sfo")) return false;
  if (IsFile(folder / "game000.dat")) return false;  // that is the base game, not the update
  return IsDriveClub(folder) && FolderIsVersion128(folder);
}

bool BaseLooksRight(const fs::path& folder) {
  if (folder.empty() || !IsDir(folder)) return false;
  return IsFile(folder / "game000.dat") && IsFile(folder / "sce_sys/param.sfo") && IsDriveClub(folder);
}

bool MergedLooksRight(const fs::path& folder) {
  return BaseLooksRight(folder) && IsFile(folder / "eboot.bin") && FolderIsVersion128(folder);
}

std::string UpdateProblem() {
  const fs::path folder = g.update;
  if (folder.empty()) return "Not chosen.";
  if (!IsDir(folder)) return "This folder does not exist (any more).";
  if (IsFile(folder / "game000.dat"))
    return "This is the base game. Choose the v1.28 update folder here, or use the base folder for both if it is a merged dump.";
  if (!IsFile(folder / "eboot.bin") || !IsFile(folder / "sce_sys/param.sfo"))
    return "Not the update: no eboot.bin and sce_sys/param.sfo here. If this is a parent folder, open it and choose the folder inside.";
  if (!IsDriveClub(folder)) return "This is another game, not DriveClub (CUSA00093 or CUSA00003).";
  if (!FolderIsVersion128(folder)) return "This update is not version 1.28 (its sce_sys/param.sfo has another version).";
  return {};
}

std::string BaseProblem() {
  const fs::path folder = g.base;
  if (folder.empty()) return "Not chosen.";
  if (!IsDir(folder)) return "This folder does not exist (any more).";
  if (!IsFile(folder / "game000.dat"))
    return "Not the base game: no game000.dat here. If this is a parent folder, open it and choose the folder inside.";
  if (!IsFile(folder / "sce_sys/param.sfo")) return "Not a game dump: no sce_sys/param.sfo here.";
  if (!IsDriveClub(folder)) return "This is another game, not DriveClub (CUSA00093 or CUSA00003).";
  return {};
}

void ValidateFolders() {
  const bool base_merged = MergedLooksRight(g.base);
  g_base_ok = BaseLooksRight(g.base);
  g_update_ok = UpdateLooksRight(g.update) || (base_merged && (g.update.empty() || g.update == g.base));
  g_merged = base_merged && (g.update.empty() || g.update == g.base);
  g_update_problem = g_update_ok ? std::string() : UpdateProblem();
  g_base_problem = g_base_ok ? std::string() : BaseProblem();
}

bool FoldersOk() {
  return g_base_ok && g_update_ok;
}

// ------------------------------------------------------------------------------------------
// patch XML
// ------------------------------------------------------------------------------------------
std::string Attr(const std::string& tag, const char* name) {
  const std::string wanted(name);
  size_t at = 0;
  while ((at = tag.find(wanted, at)) != std::string::npos) {
    const bool left = at == 0 || std::isspace(static_cast<unsigned char>(tag[at - 1]));
    size_t p = at + wanted.size();
    while (p < tag.size() && std::isspace(static_cast<unsigned char>(tag[p]))) ++p;
    if (left && p < tag.size() && tag[p] == '=') {
      ++p;
      while (p < tag.size() && std::isspace(static_cast<unsigned char>(tag[p]))) ++p;
      if (p < tag.size() && (tag[p] == '"' || tag[p] == '\'')) {
        const char quote = tag[p];
        const size_t end = tag.find(quote, p + 1);
        if (end != std::string::npos) return tag.substr(p + 1, end - p - 1);
      }
    }
    at += wanted.size();
  }
  return {};
}

std::vector<Patch> LoadPatches(const fs::path& path) {
  std::vector<Patch> out;
  const std::string xml = ReadFile(path);
  size_t at = 0;
  while ((at = xml.find("<Metadata", at)) != std::string::npos) {
    size_t end = at + 9;
    char quote = 0;
    for (; end < xml.size(); ++end) {
      if (quote) {
        if (xml[end] == quote) quote = 0;
      } else if (xml[end] == '"' || xml[end] == '\'') {
        quote = xml[end];
      } else if (xml[end] == '>') {
        break;
      }
    }
    const std::string tag = xml.substr(at + 1, end - at - 1);
    Patch patch;
    patch.name = Attr(tag, "Name");
    patch.note = Attr(tag, "Note");
    std::string enabled = Attr(tag, "isEnabled");
    std::string always = Attr(tag, "Always");
    for (char& c : enabled) c = char(std::tolower(static_cast<unsigned char>(c)));
    for (char& c : always) c = char(std::tolower(static_cast<unsigned char>(c)));
    patch.default_on = enabled == "true";
    patch.always = always == "true";
    if (!patch.name.empty()) out.push_back(std::move(patch));
    at = end + 1;
  }
  return out;
}

bool IsManagedPatch(const std::string& name) {
  if (name == kFpsDelta || name == kFpsFixed || name == kFovFix) return true;
  if (name.rfind("Memory for up to ", 0) == 0) return true;
  if (name.rfind("Resolution patch (", 0) == 0) return true;
  if (name.find("Output resolution") != std::string::npos) return true;
  if (name.find("Internal resolution") != std::string::npos) return true;
  if (name.find("Internal + Output resolution") != std::string::npos) return true;
  return false;
}

bool IsKnownPatch(const std::string& name) {
  for (const Patch& patch : g_patches) {
    if (patch.name == name) return true;
  }
  return false;
}

std::set<std::string> LoadSelection() {
  std::set<std::string> out;
  const fs::path path = PatchesEnabled();
  if (!Exists(path)) return out;
  for (const std::string& line : ReadLines(path)) {
    const std::string name = Trim(line);
    if (!name.empty()) out.insert(name);
  }
  return out;
}

int FindChoice(const Choice* choices, int count, const std::set<std::string>& selected) {
  for (int i = 0; i < count; ++i) {
    if (choices[i].patch && *choices[i].patch && selected.count(choices[i].patch)) return i;
  }
  return -1;
}

void LoadSettings() {
  const auto kv = ReadIni(LauncherIni());
  const auto legacy = ReadIni(LegacyConfig());
  g.update = kv.count("update") ? kv.at("update") : (legacy.count("update") ? legacy.at("update") : std::string());
  g.base = kv.count("base") ? kv.at("base") : (legacy.count("base") ? legacy.at("base") : std::string());

  g_patches = LoadPatches(g_paths.patch_xml);
  g_has_selection = Exists(PatchesEnabled());
  g_selected = LoadSelection();
  if (!g_has_selection) {
    for (const Patch& patch : g_patches) {
      if (patch.default_on) g_selected.insert(patch.name);
    }
  }

  ParseInt(kv, "frame_rate", g.frame_rate);
  if (!kv.count("frame_rate")) {
    if (g_selected.count(kFpsFixed)) g.frame_rate = 2;
    else if (g_selected.count(kFpsDelta)) g.frame_rate = 1;
    else g.frame_rate = 0;
  }
  int unlocked = 0;
  if (ParseInt(kv, "unlocked", unlocked) && unlocked >= 1 && unlocked <= 2 && g.frame_rate == 1) {
    g.frame_rate = 2 + unlocked;
  }
  ParseInt(kv, "fps_cap", g.fps_cap);

  int output = FindChoice(kOutput, 7, g_selected);
  int render = FindChoice(kRender, 6, g_selected);
  int wide = FindChoice(kWide, 7, g_selected);
  if (output < 0 && !ParseInt(kv, "output", output)) output = 0;
  if (render < 0 && !ParseInt(kv, "render", render)) render = 0;
  if (wide < 0 && !ParseInt(kv, "wide", wide)) wide = 0;
  g.output = output;
  g.render = render;
  g.wide = wide;
  if (g.wide > 0) g.output = g.render = 0;
  if (kv.count("fov_fix")) ParseBool(kv, "fov_fix", g.fov_fix);
  else g.fov_fix = g_selected.count(kFovFix) != 0;

  ParseInt(kv, "aa", g.aa);
  ParseInt(kv, "upscale", g.upscale);
  ParseInt(kv, "sharpness", g.sharpness);
  ParseBool(kv, "hdr", g.hdr);
  ParseInt(kv, "hdr_paper", g.hdr_paper);
  ParseInt(kv, "hdr_peak", g.hdr_peak);
  ParseInt(kv, "aniso", g.aniso);
  ParseInt(kv, "mirror", g.mirror);
  ParseInt(kv, "language", g.language);
  if (kv.count("name")) {
    const std::string name = kv.at("name").substr(0, sizeof g.name - 1);
    std::snprintf(g.name, sizeof g.name, "%s", name.c_str());
  }
  ParseBool(kv, "fullscreen", g.fullscreen);
  ParseBool(kv, "show_fps", g.show_fps);
  ParseBool(kv, "show_pedals", g.show_pedals);
  ParseInt(kv, "lock_horizon", g.lock_horizon);
  ParseInt(kv, "camera_shake", g.camera_shake);
  if (!ParseBool(kv, "camera_patch", g.camera_patch)) g.camera_patch = g.frame_rate != 0;
  ParseInt(kv, "fov", g.fov);
  ParseInt(kv, "brightness", g.brightness);
  ParseInt(kv, "contrast", g.contrast);
  ParseInt(kv, "gamma", g.gamma);
  ParseInt(kv, "gearbox", g.gearbox);
  ParseInt(kv, "brake_assist", g.brake_assist);
  ParseInt(kv, "hardcore", g.hardcore);
  ParseInt(kv, "wheel_assist", g.wheel_assist);

  // The files the game itself writes win over the launcher's last copy.
  const std::string hdr_paper = IniValue(HdrIni(), "paper");
  if (!hdr_paper.empty()) g.hdr_paper = std::atoi(hdr_paper.c_str());
  const std::string fov = IniValue(CameraIni(), "fov");
  if (!fov.empty()) g.fov = std::atoi(fov.c_str());
  const std::string brightness = IniValue(PictureIni(), "brightness");
  if (!brightness.empty()) g.brightness = std::atoi(brightness.c_str());
  const std::string contrast = IniValue(PictureIni(), "contrast");
  if (!contrast.empty()) g.contrast = std::atoi(contrast.c_str());
  const std::string gamma = IniValue(PictureIni(), "gamma");
  if (!gamma.empty()) g.gamma = std::atoi(gamma.c_str());
  const std::string show_fps = IniValue(DriveClubIni(), "show_fps");
  if (!show_fps.empty()) g.show_fps = show_fps != "0";
  const std::string show_pedals = IniValue(DriveClubIni(), "show_pedals");
  if (!show_pedals.empty()) g.show_pedals = show_pedals != "0";

  g.extras.clear();
  for (const Patch& patch : g_patches) {
    if (patch.always || IsManagedPatch(patch.name)) continue;
    g.extras[patch.name] = g_has_selection ? g_selected.count(patch.name) != 0 : patch.default_on;
  }
  Sanitize();
  SyncFolderBuffers();
  ValidateFolders();
  g.dirty = false;
}

int MemoryTier() {
  int output = 0, render = 0;
  if (g.output >= 5) output = 3;
  else if (g.output >= 3) output = 2;
  else if (g.output == 2) output = 1;
  if (g.render >= 5) render = 3;
  else if (g.wide == 6) render = 3;
  else if (g.wide > 0 || g.render >= 3) render = 2;
  else if (g.render == 2) render = 1;
  return std::max(output, render);
}

int EffectiveCap() {
  const int top = g_display_hz >= 50 ? g_display_hz - 3 : 117;
  return g.fps_cap > 0 ? std::clamp(g.fps_cap, 30, std::max(60, top)) : std::clamp(top, 30, 240);
}

void Sanitize() {
  g.frame_rate = std::clamp(g.frame_rate, 0, 4);
  g.output = std::clamp(g.output, 0, 6);
  g.render = std::clamp(g.render, 0, 5);
  g.wide = std::clamp(g.wide, 0, 6);
  if (g.wide > 0) g.output = g.render = 0;
  g.aa = std::clamp(g.aa, 0, 2);
  g.upscale = std::clamp(g.upscale, 0, 4);
  g.sharpness = std::clamp(g.sharpness, 0, 100);
  g.hdr_paper = std::clamp(g.hdr_paper, 100, 600);
  g.hdr_peak = std::clamp(g.hdr_peak, 400, 2000);
  g.aniso = std::clamp(g.aniso, 0, 5);
  g.mirror = std::clamp(g.mirror, 0, 1);
  g.language = std::clamp(g.language, 0, 20);
  g.lock_horizon = std::clamp(g.lock_horizon, 0, 2);
  g.camera_shake = std::clamp(g.camera_shake, 0, 100);
  g.fov = std::clamp(g.fov, 50, 150);
  g.brightness = std::clamp(g.brightness, -50, 50);
  g.contrast = std::clamp(g.contrast, 50, 150);
  g.gamma = std::clamp(g.gamma, 180, 280);
  g.gearbox = std::clamp(g.gearbox, 0, 2);
  g.brake_assist = std::clamp(g.brake_assist, 0, 4);
  g.hardcore = std::clamp(g.hardcore, 0, 2);
  g.wheel_assist = std::clamp(g.wheel_assist, 0, 2);
  if (g.name[0] == '\0') std::snprintf(g.name, sizeof g.name, "Player");
  if (g.aa != 0 && g.extras.count(kBlurPatch)) g.extras[kBlurPatch] = false;
}

void SyncFolderBuffers() {
  std::snprintf(g_update_buf, sizeof g_update_buf, "%s", g.update.c_str());
  std::snprintf(g_base_buf, sizeof g_base_buf, "%s", g.base.c_str());
}

std::map<std::string, std::string> EnvironmentForLaunch() {
  std::map<std::string, std::string> env;
  auto set = [&](const char* key, const std::string& value) { env[key] = value; };

  set("DRIVECLUB_UPDATE_DIR", EffectiveUpdate());
  set("DRIVECLUB_BASE_DIR", g.base);
  set("BB_GPU_USER_DIR", (g_paths.data / "user").string());
  set("BB_CRASH_DIR", (g_paths.data / "logs").string());
  set("BB_CONFIG", (g_paths.data / "user/driveclub.ini").string());
  set("BB_TOGGLE_FILE", (g_paths.data / "user/toggles.txt").string());
  set("BB_USER_NAME", g.name);
  set("BB_LANGUAGE", std::to_string(g.language));
  set("BB_FULLSCREEN", g.fullscreen ? "1" : "0");
  set("BB_CAMERA_PATCH", g.camera_patch ? "1" : "0");
  set("BB_LOCK_HORIZON", std::to_string(g.lock_horizon));
  set("BB_CAMERA_SHAKE", std::to_string(g.camera_shake));
  set("BB_MIRROR_RATE", g.mirror == 1 ? "0" : "1");
  set("BB_FRAME_GEN", "off");
  set("BB_AA", g.aa == 0 ? "off" : g.aa == 1 ? "fsr3" : "dlss");
  set("BB_AA_SCALE", g.upscale == 0 ? "native" : g.upscale == 1 ? "quality" : g.upscale == 2 ? "balanced"
                            : g.upscale == 3 ? "performance" : "ultra_performance");
  char sharpness[16];
  std::snprintf(sharpness, sizeof sharpness, "%.2f", g.sharpness / 100.0);
  set("BB_FSR_SHARPNESS", sharpness);
  const int tier = MemoryTier();
  set("BB_DMEM_MB", tier == 3 ? "17056" : tier == 2 ? "10056" : tier == 1 ? "8056" : "5056");

  if (g.hdr) {
    set("BB_HDR", "1");
    set("BB_HDR_PAPER", std::to_string(g.hdr_paper));
    set("BB_HDR_PEAK", std::to_string(g.hdr_peak));
  }
  if (g.aniso > 0) set("BB_ANISO", std::to_string(kAnisoValue[g.aniso]));
  if (g.frame_rate == 1) set("BB_FPS_LIMIT", "60");
  else if (g.frame_rate == 3) set("BB_FPS_LIMIT", std::to_string(EffectiveCap()));
  else if (g.frame_rate == 4) set("BB_FPS_LIMIT", "1000");
  if (g.frame_rate == 3) set("BB_PRESENT_MODE", "Fifo");
  else if (g.frame_rate == 4) set("BB_PRESENT_MODE", "Immediate");

  if (g.gearbox == 1) set("BB_GEARBOX", "automatic");
  else if (g.gearbox == 2) set("BB_GEARBOX", "manual");
  if (g.brake_assist == 1) set("BB_BRAKE_ASSIST", "off");
  else if (g.brake_assist == 2) set("BB_BRAKE_ASSIST", "low");
  else if (g.brake_assist == 3) set("BB_BRAKE_ASSIST", "medium");
  else if (g.brake_assist == 4) set("BB_BRAKE_ASSIST", "high");
  if (g.hardcore == 1) set("BB_HARDCORE", "off");
  else if (g.hardcore == 2) set("BB_HARDCORE", "on");
  if (g.wheel_assist == 1) set("BB_WHEEL_ASSIST", "off");
  else if (g.wheel_assist == 2) set("BB_WHEEL_ASSIST", "on");

  const fs::path runner = Exists(g_paths.apprun) ? g_paths.apprun : g_paths.appimage;
  if (!runner.empty()) set("BB_RESTART_COMMAND", "exec " + ShellQuote(runner.string()) + " --play");

  std::string summary = "release " + g_version + ": frame_rate=" + std::to_string(g.frame_rate) +
                        "; output=" + std::to_string(g.output) + "; render=" + std::to_string(g.render) +
                        "; wide=" + std::to_string(g.wide) + "; aa=" + kAaName[g.aa] +
                        "; upscale=" + kUpscaleName[g.upscale] + "; hdr=" + (g.hdr ? "on" : "off") + ";";
  set("BB_SETTINGS_SUMMARY", summary);
  return env;
}

void ApplyEnvironment(const std::map<std::string, std::string>& env) {
  static const char* const names[] = {
      "DRIVECLUB_UPDATE_DIR", "DRIVECLUB_BASE_DIR", "BB_GPU_USER_DIR", "BB_CRASH_DIR", "BB_CONFIG",
      "BB_TOGGLE_FILE", "BB_USER_NAME", "BB_LANGUAGE", "BB_FULLSCREEN", "BB_CAMERA_PATCH",
      "BB_LOCK_HORIZON", "BB_CAMERA_SHAKE", "BB_MIRROR_RATE", "BB_FRAME_GEN", "BB_AA", "BB_AA_SCALE",
      "BB_FSR_SHARPNESS", "BB_DMEM_MB", "BB_HDR", "BB_HDR_PAPER", "BB_HDR_PEAK", "BB_ANISO",
      "BB_FPS_LIMIT", "BB_PRESENT_MODE", "BB_GEARBOX", "BB_BRAKE_ASSIST", "BB_HARDCORE",
      "BB_WHEEL_ASSIST", "BB_RESTART_COMMAND", "BB_SETTINGS_SUMMARY"};
  for (const char* name : names) {
    if (!env.count(name)) unsetenv(name);
  }
  for (const auto& [key, value] : env) setenv(key.c_str(), value.c_str(), 1);
}

bool WriteLaunchEnv(const std::map<std::string, std::string>& env) {
  std::string text = "# Written by driveclub-launcher; read by AppRun --play.\n";
  for (const auto& [key, value] : env) {
    text += "export ";
    text += key;
    text += "=";
    text += ShellQuote(value);
    text += "\n";
  }
  return WriteFileAtomic(LaunchEnv(), text);
}

std::set<std::string> BuildSelection() {
  std::set<std::string> out;
  auto add = [&](const char* name) {
    if (name && *name) out.insert(name);
  };
  if (g.frame_rate == 1 || g.frame_rate == 3 || g.frame_rate == 4) add(kFpsDelta);
  else if (g.frame_rate == 2) add(kFpsFixed);
  if (g.wide > 0) add(kWide[g.wide].patch);
  else {
    add(kOutput[g.output].patch);
    add(kRender[g.render].patch);
  }
  if (g.fov_fix) add(kFovFix);
  const int tier = MemoryTier();
  if (tier == 1) add("Memory for up to 1440p");
  else if (tier == 2) add("Memory for up to 4K");
  else if (tier == 3) add("Memory for up to 8K");
  for (const auto& [name, on] : g.extras) {
    if (!on) continue;
    if (g.aa != 0 && name == kBlurPatch) continue;
    add(name.c_str());
  }
  for (const std::string& name : g_selected) {
    if (!IsKnownPatch(name)) out.insert(name);
  }
  return out;
}

bool SaveSettings() {
  ValidateFolders();
  if (!FoldersOk()) {
    SetStatus("Choose both game folders first.", true);
    return false;
  }
  std::error_code ec;
  fs::create_directories(g_paths.data / "user", ec);
  Sanitize();

  const std::set<std::string> selection = BuildSelection();
  std::string patches_text;
  for (const std::string& name : selection) patches_text += name + "\n";
  if (!WriteFileAtomic(PatchesEnabled(), patches_text)) {
    SetStatus("Could not write the patch selection. The previous settings were kept.", true);
    return false;
  }

  std::ostringstream ini;
  ini << "# DriveClub Recomp Linux launcher settings.\n"
      << "update=" << EffectiveUpdate() << "\n"
      << "base=" << g.base << "\n"
      << "frame_rate=" << g.frame_rate << "\n"
      << "fps_cap=" << (g.frame_rate == 3 || g.frame_rate == 4 ? EffectiveCap() : 0) << "\n"
      << "output=" << g.output << "\n"
      << "render=" << g.render << "\n"
      << "wide=" << g.wide << "\n"
      << "fov_fix=" << (g.fov_fix ? 1 : 0) << "\n"
      << "aa=" << g.aa << "\n"
      << "upscale=" << g.upscale << "\n"
      << "sharpness=" << g.sharpness << "\n"
      << "hdr=" << (g.hdr ? 1 : 0) << "\n"
      << "hdr_paper=" << g.hdr_paper << "\n"
      << "hdr_peak=" << g.hdr_peak << "\n"
      << "aniso=" << g.aniso << "\n"
      << "mirror=" << g.mirror << "\n"
      << "language=" << g.language << "\n"
      << "name=" << g.name << "\n"
      << "fullscreen=" << (g.fullscreen ? 1 : 0) << "\n"
      << "show_fps=" << (g.show_fps ? 1 : 0) << "\n"
      << "show_pedals=" << (g.show_pedals ? 1 : 0) << "\n"
      << "lock_horizon=" << g.lock_horizon << "\n"
      << "camera_shake=" << g.camera_shake << "\n"
      << "camera_patch=" << (g.camera_patch ? 1 : 0) << "\n"
      << "fov=" << g.fov << "\n"
      << "brightness=" << g.brightness << "\n"
      << "contrast=" << g.contrast << "\n"
      << "gamma=" << g.gamma << "\n"
      << "gearbox=" << g.gearbox << "\n"
      << "brake_assist=" << g.brake_assist << "\n"
      << "hardcore=" << g.hardcore << "\n"
      << "wheel_assist=" << g.wheel_assist << "\n";
  if (!WriteFileAtomic(LauncherIni(), ini.str())) {
    SetStatus("Could not write launcher.ini. The previous settings were kept.", true);
    return false;
  }

  SetIniValue(CameraIni(), "fov", std::to_string(g.fov));
  SetIniValue(DriveClubIni(), "show_fps", g.show_fps ? "1" : "0");
  SetIniValue(DriveClubIni(), "show_pedals", g.show_pedals ? "1" : "0");
  SetIniValue(HdrIni(), "paper", std::to_string(g.hdr_paper));
  SetIniValue(PictureIni(), "brightness", std::to_string(g.brightness));
  SetIniValue(PictureIni(), "contrast", std::to_string(g.contrast));
  SetIniValue(PictureIni(), "gamma", std::to_string(g.gamma));

  const auto env = EnvironmentForLaunch();
  if (!WriteLaunchEnv(env)) {
    SetStatus("Could not write launch.env. The game may not use these settings after a restart.", true);
    return false;
  }
  ApplyEnvironment(env);
  g.dirty = false;
  AppendLog("settings saved");
  SetStatus("Settings saved.", false);
  return true;
}

void ResetDefaults() {
  const std::string update = g.update, base = g.base;
  Settings fresh;
  g = fresh;
  std::snprintf(g.name, sizeof g.name, "Player");
  g.update = update;
  g.base = base;
  g.camera_patch = g.frame_rate != 0;
  g.extras.clear();
  for (const Patch& patch : g_patches) {
    if (patch.always || IsManagedPatch(patch.name)) continue;
    g.extras[patch.name] = patch.default_on;
  }
  Sanitize();
  SyncFolderBuffers();
  ValidateFolders();
  g.dirty = true;
}

// ------------------------------------------------------------------------------------------
// status, preparation and launch
// ------------------------------------------------------------------------------------------
std::string g_status;
bool g_status_error = false;
Uint64 g_status_until = 0;

void SetStatus(const std::string& text, bool error) {
  g_status = text;
  g_status_error = error;
  g_status_until = SDL_GetTicks() + 8000;
  AppendLog(std::string(error ? "error: " : "status: ") + text);
}

std::atomic<bool> g_preparing{false};
std::atomic<bool> g_prep_done{false};
std::atomic<bool> g_prep_joined{false};
std::atomic<int> g_prep_step{0};
std::mutex g_prep_mutex;
std::string g_prep_text;
std::string g_prep_error;
std::thread g_prep_thread;

bool SpawnGame(const std::vector<std::string>& args, const fs::path& cwd, const fs::path& log_path,
               std::string& error) {
  std::vector<char*> argv;
  argv.reserve(args.size() + 1);
  for (const std::string& arg : args) argv.push_back(const_cast<char*>(arg.c_str()));
  argv.push_back(nullptr);

  int pipefd[2];
  if (pipe(pipefd) != 0) {
    error = std::strerror(errno);
    return false;
  }
  fcntl(pipefd[1], F_SETFD, FD_CLOEXEC);
  const pid_t pid = fork();
  if (pid < 0) {
    error = std::strerror(errno);
    close(pipefd[0]);
    close(pipefd[1]);
    return false;
  }
  if (pid == 0) {
    setsid();
    close(pipefd[0]);
    const int log = open(log_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (log < 0) {
      const int code = errno;
      (void)write(pipefd[1], &code, sizeof code);
      _exit(127);
    }
    if (dup2(log, STDOUT_FILENO) < 0 || dup2(log, STDERR_FILENO) < 0) {
      const int code = errno;
      (void)write(pipefd[1], &code, sizeof code);
      _exit(127);
    }
    if (log > STDERR_FILENO) close(log);
    if (chdir(cwd.c_str()) != 0) {
      const int code = errno;
      (void)write(pipefd[1], &code, sizeof code);
      _exit(127);
    }
    execv(argv[0], argv.data());
    const int code = errno;
    (void)write(pipefd[1], &code, sizeof code);
    _exit(127);
  }
  close(pipefd[1]);
  int child_error = 0;
  const ssize_t got = read(pipefd[0], &child_error, sizeof child_error);
  close(pipefd[0]);
  if (got == ssize_t(sizeof child_error)) {
    int status = 0;
    waitpid(pid, &status, 0);
    error = std::strerror(child_error);
    return false;
  }
  return true;
}

bool LaunchGame() {
  std::vector<std::string> args;
  args.push_back(g_paths.probe.string());
  args.push_back((g_paths.data / "prep/boot-linked.bin").string());
  args.push_back("--content-profile");
  args.push_back((g_paths.data / "prep/content.bin").string());
  args.push_back("--patches");
  args.push_back((g_paths.data / "prep/patches.bin").string());
  args.push_back("--app0");
  args.push_back(EffectiveUpdate());
  if (g.base != EffectiveUpdate()) {
    args.push_back("--app0-base");
    args.push_back(g.base);
  }
  args.push_back("--user");
  args.push_back((g_paths.data / "user").string());
  args.push_back("--mods");
  args.push_back((g_paths.data / "mods").string());
  args.push_back("--stub-unresolved");
  args.push_back("--timeout");
  args.push_back("0");

  if (!IsFile(g_paths.probe)) {
    SetStatus("The runtime (driveclub-probe) was not found in the AppImage.", true);
    return false;
  }
  std::error_code ec;
  fs::create_directories(g_paths.data / "logs", ec);
  std::string error;
  AppendLog("starting " + g_paths.probe.string());
  if (!SpawnGame(args, g_paths.share, g_paths.data / "logs/last-run.log", error)) {
    SetStatus("Could not start the game: " + error, true);
    return false;
  }
  return true;
}

void StartPrepare() {
  if (g_preparing) return;
  if (!SaveSettings()) return;
  if (!FoldersOk()) {
    SetStatus("Choose both game folders first.", true);
    return;
  }
  if (!IsFile(g_paths.patch_xml)) {
    SetStatus("The patch file is missing from the AppImage: patches/DriveClub.xml", true);
    return;
  }
  if (g_prep_thread.joinable()) g_prep_thread.join();
  g_prep_step.store(0);
  {
    std::lock_guard<std::mutex> lock(g_prep_mutex);
    g_prep_text = "Preparing the game files ...";
    g_prep_error.clear();
  }
  g_prep_done.store(false);
  g_prep_joined.store(false);
  g_preparing.store(true);
  const std::string update = EffectiveUpdate();
  const std::string base = g.base;
  const std::string data = g_paths.data.string();
  const std::string xml = g_paths.patch_xml.string();
  g_prep_thread = std::thread([update, base, data, xml] {
    std::string error;
    try {
      error = PrepareGame(fs::path(update), fs::path(base), fs::path(data), fs::path(xml), false,
                          [](int step, const std::string& text) {
                            g_prep_step.store(step);
                            std::lock_guard<std::mutex> lock(g_prep_mutex);
                            g_prep_text = text;
                          });
    } catch (const std::exception& e) {
      error = e.what();
    } catch (...) {
      error = "unknown error";
    }
    {
      std::lock_guard<std::mutex> lock(g_prep_mutex);
      g_prep_error = error;
    }
    g_prep_done.store(true);
  });
}

void PollPrepare() {
  if (!g_preparing || !g_prep_done) return;
  if (!g_prep_joined) {
    if (g_prep_thread.joinable()) g_prep_thread.join();
    g_prep_joined.store(true);
  }
  std::string error;
  {
    std::lock_guard<std::mutex> lock(g_prep_mutex);
    error = g_prep_error;
  }
  if (!error.empty()) {
    SetStatus(error, true);
    return;
  }
  g_preparing.store(false);
  AppendLog("prepare: ready");
  if (LaunchGame()) {
    g_quit = true;
  } else {
    g_preparing.store(false);
  }
}

// ------------------------------------------------------------------------------------------
// dialogs
// ------------------------------------------------------------------------------------------
struct DialogResult {
  std::mutex mutex;
  bool done = false;
  int which = -1;
  std::string path;
  std::string error;
};

DialogResult g_dialog;

void SDLCALL FolderCallback(void* userdata, const char* const* files, int) {
  const int which = int(reinterpret_cast<intptr_t>(userdata));
  std::lock_guard<std::mutex> lock(g_dialog.mutex);
  g_dialog.which = which;
  g_dialog.done = true;
  g_dialog.path.clear();
  g_dialog.error.clear();
  if (!files) {
    g_dialog.error = SDL_GetError();
  } else if (files[0]) {
    g_dialog.path = files[0];
  }
}

void OpenFolderDialog(int which) {
  const std::string start = which == 0 ? g.update : which == 1 ? g.base : g_paths.data.string();
  SDL_ShowOpenFolderDialog(FolderCallback, reinterpret_cast<void*>(intptr_t(which)), g_window,
                           start.empty() ? nullptr : start.c_str(), false);
}

void SetFolder(int which, const std::string& path) {
  if (which == 0) {
    g.update = path;
    std::snprintf(g_update_buf, sizeof g_update_buf, "%s", g.update.c_str());
  } else {
    g.base = path;
    std::snprintf(g_base_buf, sizeof g_base_buf, "%s", g.base.c_str());
  }
  ValidateFolders();
  g.dirty = true;
}

void PollDialog() {
  DialogResult result;
  {
    std::lock_guard<std::mutex> lock(g_dialog.mutex);
    if (!g_dialog.done) return;
    result.which = g_dialog.which;
    result.path = g_dialog.path;
    result.error = g_dialog.error;
    g_dialog.done = false;
    g_dialog.which = -1;
  }
  if (result.which == 2 && !result.path.empty()) {
    BeginDataFolderChange(result.path);
  } else if (result.which >= 0 && !result.path.empty()) {
    SetFolder(result.which, result.path);
  } else if (!result.error.empty()) {
    SetStatus("Could not open the folder dialog: " + result.error + " Type the path instead.", true);
  }
}

// ------------------------------------------------------------------------------------------
// UI helpers
// ------------------------------------------------------------------------------------------
void Tooltip(const std::string& text) {
  if (text.empty()) return;
  ImGui::BeginTooltip();
  ImGui::PushTextWrapPos(ImGui::GetFontSize() * 32.0f);
  ImGui::TextUnformatted(text.c_str());
  ImGui::PopTextWrapPos();
  ImGui::EndTooltip();
}

bool BeginCard(const char* title) {
  ImGui::PushStyleColor(ImGuiCol_ChildBg, kCard);
  ImGui::PushStyleColor(ImGuiCol_Border, kCardLine);
  ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 8.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(18, 16));
  const bool open = ImGui::BeginChild(title, ImVec2(ImGui::GetContentRegionAvail().x, 0),
                                      ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY);
  if (open) {
    ImGui::TextColored(kAccent, "%s", title);
    ImGui::Spacing();
  }
  return open;
}

void EndCard() {
  ImGui::EndChild();
  ImGui::PopStyleVar(2);
  ImGui::PopStyleColor(2);
  ImGui::Dummy(ImVec2(0, 10));
}

void RowLabel(const char* label, const char* hint) {
  ImGui::AlignTextToFramePadding();
  ImGui::TextUnformatted(label);
  if (hint && *hint) {
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered()) Tooltip(hint);
  }
}

bool ComboRow(const char* label, const char* hint, int* value, const char* const* items, int count) {
  bool changed = false;
  ImGui::PushID(label);
  if (ImGui::BeginTable("##row", 2, ImGuiTableFlags_SizingStretchProp)) {
    ImGui::TableSetupColumn("##label", ImGuiTableColumnFlags_WidthStretch, 1.45f);
    ImGui::TableSetupColumn("##control", ImGuiTableColumnFlags_WidthStretch, 1.0f);
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    RowLabel(label, hint);
    ImGui::TableSetColumnIndex(1);
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
    const int current = std::clamp(*value, 0, count - 1);
    if (ImGui::BeginCombo("##combo", items[current])) {
      for (int i = 0; i < count; ++i) {
        if (ImGui::Selectable(items[i], i == current)) {
          *value = i;
          changed = true;
        }
      }
      ImGui::EndCombo();
    }
    ImGui::EndTable();
  }
  ImGui::PopID();
  return changed;
}

bool ToggleSwitch(const char* id, bool* value) {
  ImGui::PushID(id);
  const float height = ImGui::GetFrameHeight();
  const ImVec2 size(height * 1.8f, height);
  ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - size.x);
  const ImVec2 pos = ImGui::GetCursorScreenPos();
  const bool pressed = ImGui::InvisibleButton("##switch", size);
  bool changed = false;
  if (pressed) {
    *value = !*value;
    changed = true;
  }
  const bool hovered = ImGui::IsItemHovered();
  ImDrawList* draw = ImGui::GetWindowDrawList();
  const ImU32 track = ImGui::GetColorU32(*value ? (hovered ? kAccentHover : kAccent) : (hovered ? kTrackHover : kOff));
  draw->AddRectFilled(pos, ImVec2(pos.x + size.x, pos.y + size.y), track, size.y * 0.5f);
  const float radius = size.y * 0.5f - 3.0f;
  const float cx = *value ? pos.x + size.x - radius - 3.0f : pos.x + radius + 3.0f;
  draw->AddCircleFilled(ImVec2(cx, pos.y + size.y * 0.5f), radius, ImGui::GetColorU32(kWhite));
  ImGui::PopID();
  return changed;
}

bool ToggleRow(const char* label, const char* hint, bool* value) {
  bool changed = false;
  ImGui::PushID(label);
  if (ImGui::BeginTable("##row", 2, ImGuiTableFlags_SizingStretchProp)) {
    ImGui::TableSetupColumn("##label", ImGuiTableColumnFlags_WidthStretch, 1.45f);
    ImGui::TableSetupColumn("##control", ImGuiTableColumnFlags_WidthStretch, 1.0f);
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    RowLabel(label, hint);
    ImGui::TableSetColumnIndex(1);
    changed = ToggleSwitch("##toggle", value);
    if (hint && *hint && ImGui::IsItemHovered()) Tooltip(hint);
    ImGui::EndTable();
  }
  ImGui::PopID();
  return changed;
}

bool SliderRow(const char* label, const char* hint, int* value, int min, int max, const char* format) {
  bool changed = false;
  ImGui::PushID(label);
  if (ImGui::BeginTable("##row", 2, ImGuiTableFlags_SizingStretchProp)) {
    ImGui::TableSetupColumn("##label", ImGuiTableColumnFlags_WidthStretch, 1.45f);
    ImGui::TableSetupColumn("##control", ImGuiTableColumnFlags_WidthStretch, 1.0f);
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    RowLabel(label, hint);
    ImGui::TableSetColumnIndex(1);
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
    changed = ImGui::SliderInt("##slider", value, min, max, format);
    if (hint && *hint && ImGui::IsItemHovered()) Tooltip(hint);
    ImGui::EndTable();
  }
  ImGui::PopID();
  return changed;
}

bool GammaRow(const char* label, const char* hint, int* value) {
  bool changed = false;
  ImGui::PushID(label);
  if (ImGui::BeginTable("##row", 2, ImGuiTableFlags_SizingStretchProp)) {
    ImGui::TableSetupColumn("##label", ImGuiTableColumnFlags_WidthStretch, 1.45f);
    ImGui::TableSetupColumn("##control", ImGuiTableColumnFlags_WidthStretch, 1.0f);
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    RowLabel(label, hint);
    ImGui::TableSetColumnIndex(1);
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
    float gamma = *value / 100.0f;
    if (ImGui::SliderFloat("##slider", &gamma, 1.80f, 2.80f, "%.2f")) {
      *value = int(gamma * 100.0f + 0.5f);
      changed = true;
    }
    if (hint && *hint && ImGui::IsItemHovered()) Tooltip(hint);
    ImGui::EndTable();
  }
  ImGui::PopID();
  return changed;
}

void FolderRow(int which, const char* label, const char* hint, char* buffer, std::size_t size,
               std::string& path, bool ok, const std::string& problem) {
  ImGui::PushID(which);
  RowLabel(label, hint);
  const float width = std::max(160.0f, ImGui::GetContentRegionAvail().x - 108.0f);
  ImGui::SetNextItemWidth(width);
  if (ImGui::InputText("##path", buffer, size)) {
    path = buffer;
    g.dirty = true;
  }
  if (ImGui::IsItemDeactivatedAfterEdit()) ValidateFolders();
  ImGui::SameLine();
  if (ImGui::Button("Browse...", ImVec2(96, 0))) OpenFolderDialog(which);
  if (path.empty()) {
    ImGui::TextDisabled("Not chosen.");
  } else if (ok) {
    ImGui::TextColored(kOk, "Folder looks right.");
  } else {
    ImGui::TextColored(kBad, "%s", problem.c_str());
  }
  ImGui::PopID();
}

bool PrimaryButton(const char* label, const ImVec2& size) {
  ImGui::PushStyleColor(ImGuiCol_Button, kAccent);
  ImGui::PushStyleColor(ImGuiCol_ButtonHovered, kAccentHover);
  ImGui::PushStyleColor(ImGuiCol_ButtonActive, kAccent);
  ImGui::PushStyleColor(ImGuiCol_Text, kWhite);
  const bool pressed = ImGui::Button(label, size);
  ImGui::PopStyleColor(4);
  return pressed;
}

void OpenPath(const fs::path& path) {
  const pid_t pid = fork();
  if (pid == 0) {
    setsid();
    execlp("xdg-open", "xdg-open", path.c_str(), (char*)nullptr);
    _exit(127);
  }
}

// ------------------------------------------------------------------------------------------
// data folder
// ------------------------------------------------------------------------------------------
bool g_data_dialog_open = false;
bool g_data_copying = false;
std::atomic<bool> g_data_copy_done{false};
bool g_data_copy_joined = false;
bool g_data_ready = false;
bool g_data_failed = false;
bool g_data_has_files = false;
bool g_data_move = true;
fs::path g_data_pending;
std::string g_data_error;
std::mutex g_data_mutex;
std::string g_data_copy_text;
std::string g_data_copy_error;
std::thread g_data_copy_thread;
std::string g_restart_data;

bool CopyDataTree(const fs::path& from, const fs::path& to, std::string& error) {
  std::error_code ec;
  std::size_t files = 0;
  std::uintmax_t copied_bytes = 0;
  for (fs::recursive_directory_iterator it(from, fs::directory_options::skip_permission_denied, ec),
       end;
       it != end; it.increment(ec)) {
    if (ec) {
      error = "Could not read " + from.string() + ": " + ec.message();
      return false;
    }
    const fs::path relative = fs::relative(it->path(), from, ec);
    if (ec) {
      error = "Could not copy " + it->path().string() + ": " + ec.message();
      return false;
    }
    const fs::path target = to / relative;
    std::error_code type_ec;
    if (it->is_directory(type_ec)) {
      fs::create_directories(target, type_ec);
      if (type_ec) {
        error = "Could not create " + target.string() + ": " + type_ec.message();
        return false;
      }
    } else if (it->is_regular_file(type_ec)) {
      fs::create_directories(target.parent_path(), type_ec);
      if (type_ec) {
        error = "Could not create " + target.parent_path().string() + ": " + type_ec.message();
        return false;
      }
      std::error_code size_ec;
      const std::uintmax_t size = it->file_size(size_ec);
      {
        std::lock_guard<std::mutex> lock(g_data_mutex);
        g_data_copy_text = "Copying " + it->path().filename().string() + " ...";
      }
      fs::copy_file(it->path(), target, fs::copy_options::overwrite_existing, type_ec);
      if (type_ec) {
        error = "Could not copy " + it->path().string() + ": " + type_ec.message();
        return false;
      }
      ++files;
      if (!size_ec) copied_bytes += size;
      if (files % 64 == 0) {
        std::lock_guard<std::mutex> lock(g_data_mutex);
        g_data_copy_text = "Copying the data ... " + std::to_string(files) + " files, " +
                           std::to_string(copied_bytes / (1024 * 1024)) + " MB";
      }
    }
  }
  if (ec) {
    error = "Could not read " + from.string() + ": " + ec.message();
    return false;
  }
  return true;
}

void StartDataCopy() {
  if (g_data_copy_thread.joinable()) g_data_copy_thread.join();
  g_data_copying = true;
  g_data_copy_done = false;
  g_data_copy_joined = false;
  g_data_ready = false;
  g_data_failed = false;
  {
    std::lock_guard<std::mutex> lock(g_data_mutex);
    g_data_copy_error.clear();
    g_data_copy_text = "Moving the data ...";
  }
  const fs::path from = g_paths.data;
  const fs::path to = g_data_pending;
  g_data_copy_thread = std::thread([from, to] {
    std::string error;
    bool moved = false;
    std::error_code ec;
    fs::rename(from, to, ec);
    if (!ec) {
      moved = true;
    } else if (DirHasEntries(to)) {
      error = "The new folder already has files; choose an empty folder.";
    } else {
      CopyDataTree(from, to, error);
    }
    std::lock_guard<std::mutex> lock(g_data_mutex);
    if (!error.empty()) {
      g_data_copy_error = error;
    } else if (moved) {
      g_data_copy_text = "Data moved.";
    } else {
      g_data_copy_text = "Data copied; the old folder was kept.";
    }
    g_data_copy_done = true;
  });
}

void FinishDataFolderChange() {
  // launch.env holds absolute paths into the old folder; let AppRun use the new data folder's
  // defaults until the launcher saves the settings again.
  std::error_code ignored;
  fs::remove(g_data_pending / "launch.env", ignored);
  if (!WriteDataPointer(g_data_pending)) {
    g_data_error = "Could not save the data folder location" +
                   (g_paths.data_pointer.empty() ? std::string(".")
                                                 : " to " + g_paths.data_pointer.string() + ".");
    g_data_ready = true;
    g_data_copying = false;
    return;
  }
  g_data_dialog_open = false;
  g_data_copying = false;
  g_data_ready = true;
  g_restart_data = g_data_pending.string();
  g_quit = true;
}

void ApplyDataFolder() {
  if (g_data_pending.empty() || g_data_copying) return;
  if (g_data_ready) {
    FinishDataFolderChange();
    return;
  }
  std::string error;
  if (!EnsureDataDir(g_data_pending, error)) {
    g_data_error = error;
    return;
  }
  if (g_data_move && DirHasEntries(g_paths.data)) {
    StartDataCopy();
    return;
  }
  FinishDataFolderChange();
}

void BeginDataFolderChange(const fs::path& requested) {
  if (requested.empty()) return;
  const fs::path target = NormalizePath(requested);
  if (SamePath(target, g_paths.data)) {
    SetStatus("DriveClub already uses this data folder.", false);
    return;
  }
  if (PathStartsWith(target, g_paths.data) || PathStartsWith(g_paths.data, target)) {
    SetStatus("Choose a data folder that does not contain the current one.", true);
    return;
  }
  if (g.dirty && FoldersOk() && !SaveSettings()) return;
  g_data_pending = target;
  g_data_has_files = DirHasEntries(target);
  g_data_move = !g_data_has_files;
  g_data_ready = false;
  g_data_failed = false;
  g_data_copying = false;
  g_data_error.clear();
  g_data_dialog_open = true;
}

void PollDataCopy() {
  if (!g_data_copying || !g_data_copy_done) return;
  if (!g_data_copy_joined) {
    if (g_data_copy_thread.joinable()) g_data_copy_thread.join();
    g_data_copy_joined = true;
  }
  std::string error;
  {
    std::lock_guard<std::mutex> lock(g_data_mutex);
    error = g_data_copy_error;
  }
  if (!error.empty()) {
    g_data_error = error;
    g_data_copying = false;
    g_data_failed = true;
    g_data_has_files = true;
    g_data_move = false;
    return;
  }
  FinishDataFolderChange();
}

void DrawDataFolderDialog() {
  if (!g_data_dialog_open) return;
  const ImVec2 pos = ImGui::GetWindowPos();
  const ImVec2 size = ImGui::GetWindowSize();
  ImGui::GetForegroundDrawList()->AddRectFilled(pos, ImVec2(pos.x + size.x, pos.y + size.y),
                                                IM_COL32(0, 0, 0, 175));
  ImGui::SetNextWindowPos(ImVec2(pos.x + size.x * 0.5f, pos.y + size.y * 0.5f), ImGuiCond_Always,
                          ImVec2(0.5f, 0.5f));
  ImGui::SetNextWindowSize(ImVec2(620, 0));
  ImGui::PushStyleColor(ImGuiCol_WindowBg, kCard);
  ImGui::Begin("##datafolder", nullptr,
               ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize);
  ImGui::TextColored(kAccent, "DATA FOLDER");
  ImGui::Spacing();
  ImGui::TextWrapped("DriveClub will keep settings, prepared game files, saves and logs in:");
  ImGui::TextWrapped("%s", g_data_pending.string().c_str());
  ImGui::Spacing();
  if (g_data_failed) {
    ImGui::TextColored(kBad, "The move did not finish. Choose another folder.");
  } else if (g_data_has_files) {
    ImGui::TextColored(kBad, "This folder already has files. They will be used as they are.");
  } else if (!g_data_ready) {
    ImGui::BeginDisabled(g_data_copying);
    ImGui::Checkbox("Move the current data here", &g_data_move);
    ImGui::EndDisabled();
    ImGui::TextDisabled("On another drive the data is copied and the old folder is kept.");
  }
  if (g_data_copying) {
    std::string text;
    {
      std::lock_guard<std::mutex> lock(g_data_mutex);
      text = g_data_copy_text;
    }
    ImGui::Spacing();
    ImGui::TextWrapped("%s", text.c_str());
  }
  if (!g_data_error.empty()) {
    ImGui::Spacing();
    ImGui::TextColored(kBad, "%s", g_data_error.c_str());
  }
  ImGui::Spacing();
  const bool can_use = !g_data_copying && (!g_data_failed || g_data_ready);
  ImGui::BeginDisabled(!can_use);
  if (PrimaryButton(g_data_ready ? "RETRY" : "USE THIS FOLDER", ImVec2(190, 40))) {
    ApplyDataFolder();
  }
  ImGui::EndDisabled();
  ImGui::SameLine();
  ImGui::BeginDisabled(g_data_copying);
  if (ImGui::Button("CANCEL", ImVec2(100, 40))) {
    g_data_dialog_open = false;
    g_data_error.clear();
  }
  ImGui::EndDisabled();
  ImGui::End();
  ImGui::PopStyleColor();
}

// ------------------------------------------------------------------------------------------
// pages
// ------------------------------------------------------------------------------------------
void GamePage() {
  if (BeginCard("GAME FILES")) {
    FolderRow(0, "Update folder (version 1.28)",
              "The DriveClub v1.28 update dump: the folder that contains eboot.bin.",
              g_update_buf, sizeof g_update_buf, g.update, g_update_ok, g_update_problem);
    ImGui::Spacing();
    FolderRow(1, "Base game folder", "The DriveClub base game dump: the folder that contains game000.dat.",
              g_base_buf, sizeof g_base_buf, g.base, g_base_ok, g_base_problem);
    if (g_merged) {
      ImGui::TextColored(kOk, "This is a merged 1.28 dump: the base folder is used for both.");
    } else if (g.update.empty() && g_base_ok) {
      ImGui::TextDisabled("If the base folder is a merged 1.28 dump, you can leave the update folder empty.");
    }
    ImGui::Spacing();
    ImGui::TextWrapped("Your game folders are only read. The prepared image, settings, saves and logs go to %s.",
                       g_paths.data.string().c_str());
  }
  EndCard();

  if (BeginCard("PLAYER AND STARTUP")) {
    if (ComboRow("Language", "The language of the game's menus and text. Japanese, Korean and Chinese may show missing characters.",
                 &g.language, kLanguageName, 21)) g.dirty = true;
    ImGui::Spacing();
    RowLabel("Player name", "The name the game shows for you.");
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
    if (ImGui::InputText("##player", g.name, sizeof g.name)) g.dirty = true;
    ImGui::Spacing();
    if (ToggleRow("Fullscreen", "Play borderless fullscreen. Turn off for a window.", &g.fullscreen)) g.dirty = true;
    if (ToggleRow("Show FPS counter", "Draws the frame rate in the top right corner while playing.", &g.show_fps)) g.dirty = true;
    if (ToggleRow("Show pedal inputs", "Draws the clutch, brake and throttle travel in the bottom left while playing.",
                  &g.show_pedals)) g.dirty = true;
  }
  EndCard();

  if (BeginCard("DATA FOLDER")) {
    ImGui::TextWrapped("%s", g_paths.data.string().c_str());
    if (ImGui::Button("Change...")) OpenFolderDialog(2);
    if (!g_paths.default_data.empty() && !SamePath(g_paths.data, g_paths.default_data)) {
      ImGui::SameLine();
      if (ImGui::Button("Use AppImage folder")) BeginDataFolderChange(g_paths.default_data);
    }
    if (!g_paths.default_data.empty() && SamePath(g_paths.data, g_paths.default_data)) {
      ImGui::TextColored(kOk, "Stored next to the AppImage.");
    } else if (!g_paths.default_data.empty()) {
      ImGui::TextDisabled("The folder next to the AppImage is preferred.");
    }
    ImGui::Spacing();
    if (ImGui::Button("Open data folder")) OpenPath(g_paths.data);
    ImGui::SameLine();
    if (ImGui::Button("Open logs")) OpenPath(g_paths.data / "logs");
    ImGui::SameLine();
    if (ImGui::Button("Prepare again")) StartPrepare();
  }
  EndCard();
}

void DisplayPage() {
  if (BeginCard("FRAME RATE")) {
    if (ComboRow("Frame rate",
                 "30 FPS as on PS4; 60 FPS with deltatime; the engine's fixed 60 Hz tick; Unlocked for G-Sync/FreeSync; "
                 "Unlimited with no cap and no V-Sync. Above about 120 FPS the game's timing can break.",
                 &g.frame_rate, kFrameRateName, 5)) {
      if (g.frame_rate == 3 || g.frame_rate == 4) g.fps_cap = EffectiveCap();
      g.dirty = true;
    }
    if (g.frame_rate == 3) {
      int cap = EffectiveCap();
      if (SliderRow("Frame cap", "The highest frame rate with Unlocked (VRR). The right end is the display's refresh "
                                 "rate less 3. A little below what the PC can hold gives the steadiest motion.",
                    &cap, 30, std::max(60, g_display_hz ? g_display_hz : 240), "%d FPS")) {
        g.fps_cap = cap;
        g.dirty = true;
      }
    } else if (g.frame_rate == 4) {
      ImGui::TextDisabled("Unlimited has no frame cap and no V-Sync; frames are shown as soon as they are done.");
    }
  }
  EndCard();

  if (BeginCard("RESOLUTION")) {
    if (ComboRow("Output resolution", "The size of the finished picture (menus and HUD). It is scaled to fill the screen.",
                 &g.output, kOutputName, 7)) {
      if (g.output > 0) g.wide = 0;
      g.dirty = true;
    }
    const bool upscaled = g.aa != 0 && g.upscale != 0;
    if (upscaled) {
      ImGui::PushID("render-upscaled");
      RowLabel("Render resolution", "With FSR or DLSS upscaling the runtime sets the render size from the output size.");
      ImGui::TextDisabled("Set by upscaling.");
      ImGui::PopID();
    } else if (ComboRow("Render resolution", "The size the 3D scene is drawn at. Lower is faster; higher is sharper.",
                        &g.render, kRenderName, 6)) {
      if (g.render > 0) g.wide = 0;
      g.dirty = true;
    }
    if (ComboRow("Widescreen", "Ultrawide sizes change both the render and the output size. Choosing one clears the sizes above.",
                 &g.wide, kWideName, 7)) {
      if (g.wide > 0) g.output = g.render = 0;
      g.dirty = true;
    }
    if (ToggleRow("Widescreen field-of-view fix", "For wide and ultrawide resolutions only.", &g.fov_fix)) g.dirty = true;
  }
  EndCard();

  if (BeginCard("ANTI-ALIASING")) {
    if (ComboRow("Anti-aliasing", "Game keeps the original. FSR 3.1 works on any card; DLSS needs a GeForce RTX card.",
                 &g.aa, kAaName, 3)) {
      Sanitize();
      g.dirty = true;
    }
    if (g.aa != 0) {
      if (ComboRow("Upscaling", "Native draws every pixel; Quality, Balanced, Performance and Ultra Performance "
                                "render lower and rebuild the picture. Needs FSR or DLSS above.",
                   &g.upscale, kUpscaleName, 5)) g.dirty = true;
      if (SliderRow("Sharpness", "How crisp the picture looks with DLSS or FSR. 80 is a good start.",
                    &g.sharpness, 0, 100, "%d%%")) g.dirty = true;
    } else {
      ImGui::TextDisabled("Upscaling needs FSR or DLSS; it is hidden while the game's own anti-aliasing is selected.");
    }
    if (ComboRow("Anisotropic filtering", "Keeps textures on roads and terrain sharp at a shallow angle.",
                 &g.aniso, kAnisoName, 6)) g.dirty = true;
  }
  EndCard();

  if (BeginCard("HDR")) {
    if (ToggleRow("HDR output", "For HDR monitors and TVs. Turn on HDR in the desktop's display settings first.",
                  &g.hdr)) g.dirty = true;
    if (g.hdr) {
      if (SliderRow("HDR brightness", "How bright white is: the HUD, menus and the scene overall. Page Up / Page Down "
                                      "change it while playing.",
                    &g.hdr_paper, 100, 600, "%d nits")) g.dirty = true;
      if (SliderRow("HDR peak brightness", "Should match the screen's HDR peak (for example 600 or 1000 nits).",
                    &g.hdr_peak, 400, 2000, "%d nits")) g.dirty = true;
    } else {
      ImGui::TextDisabled("HDR brightness applies only when HDR output is on.");
    }
  }
  EndCard();

  if (BeginCard("PICTURE")) {
    if (g.hdr) {
      ImGui::TextDisabled("HDR uses its own brightness. These sliders are for SDR output.");
    }
    ImGui::BeginDisabled(g.hdr);
    if (SliderRow("Brightness", "Lifts or lowers the whole picture.", &g.brightness, -50, 50, "%d")) g.dirty = true;
    if (SliderRow("Contrast", "The difference between dark and bright.", &g.contrast, 50, 150, "%d%%")) g.dirty = true;
    if (GammaRow("Gamma", "2.40 is the TVs the game was made for; 2.20 is a PC monitor.", &g.gamma)) g.dirty = true;
    ImGui::EndDisabled();
  }
  EndCard();

  if (BeginCard("REFLECTIONS")) {
    if (ComboRow("Mirror and reflection updates", "Live draws the mirror/rain scene every frame. Static draws it once: "
                                                  "faster, but reflections and rain on the glass stop updating.",
                 &g.mirror, kMirrorName, 2)) g.dirty = true;
  }
  EndCard();
}

void DrivingPage() {
  if (BeginCard("VIEW AND CAMERA")) {
    if (SliderRow("Field of view", "How much of the road you see. 100% is the game's own view; F5 narrows it and F6 widens it while driving.",
                  &g.fov, 50, 150, "%d%%")) g.dirty = true;
    if (ComboRow("Lock to horizon", "Keeps the horizon level when the car leans and dips. Cockpit only, or all views.",
                 &g.lock_horizon, kHorizonName, 3)) g.dirty = true;
    if (SliderRow("Camera shake", "How much the camera shakes over bumps and kerbs. Lower it if the shaking is tiring.",
                  &g.camera_shake, 0, 100, "%d%%")) g.dirty = true;
    if (ToggleRow("High-FPS camera fix", "Smooths camera and replay shake at high frame rates. On by default for 60 FPS and unlocked modes.",
                  &g.camera_patch)) g.dirty = true;
  }
  EndCard();

  if (BeginCard("ASSISTS")) {
    if (ComboRow("Gearbox", "In-game leaves it to the game's Options menu. Manual is for paddles or a shifter.",
                 &g.gearbox, kGearboxName, 3)) g.dirty = true;
    if (ComboRow("Brake assist", "In-game leaves it to the game's Options menu.", &g.brake_assist, kBrakeName, 5)) g.dirty = true;
    if (ComboRow("Hardcore handling", "In-game leaves it to the game's Options menu. Recommended with a wheel.",
                 &g.hardcore, kOnOffName, 3)) g.dirty = true;
    if (ComboRow("Wheel assist", "In-game leaves it to the game's Options menu. Off is recommended with a wheel.",
                 &g.wheel_assist, kOnOffName, 3)) g.dirty = true;
  }
  EndCard();

  if (BeginCard("STEERING WHEEL")) {
    ImGui::TextWrapped("Steering wheels, pedals, shifters and force feedback work through SDL3. The Windows Wheel Setup "
                       "tool is not part of the Linux build; the controls are mapped in data/user/input_config/default.ini "
                       "after the first start. The force feedback settings live in the same file.");
  }
  EndCard();
}

void PatchesPage() {
  if (BeginCard("DISPLAY PATCHES")) {
    ImGui::TextWrapped("The launcher chooses these from the Display page. Memory patches are added automatically.");
    ImGui::Spacing();
    ImGui::BulletText("Frame rate: %s", kFrameRateName[g.frame_rate]);
    ImGui::BulletText("Output: %s", g.wide ? kWideName[g.wide] : kOutputName[g.output]);
    if (g.wide == 0) ImGui::BulletText("Render: %s", g.aa != 0 && g.upscale != 0 ? "set by upscaling" : kRenderName[g.render]);
    const int tier = MemoryTier();
    ImGui::BulletText("Memory: %s", tier == 3 ? "up to 8K" : tier == 2 ? "up to 4K" : tier == 1 ? "up to 1440p" : "default");
    if (g.fov_fix) ImGui::BulletText("Widescreen field-of-view fix");
  }
  EndCard();

  if (BeginCard("COMMUNITY PATCHES")) {
    bool any = false;
    for (const Patch& patch : g_patches) {
      if (patch.always || IsManagedPatch(patch.name)) continue;
      any = true;
      const bool locked = g.aa != 0 && patch.name == kBlurPatch;
      bool on = g.extras.count(patch.name) ? g.extras[patch.name] : false;
      if (locked) on = false;
      ImGui::PushID(patch.name.c_str());
      if (ImGui::BeginTable("##patch", 2, ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("##name", ImGuiTableColumnFlags_WidthStretch, 1.55f);
        ImGui::TableSetupColumn("##on", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - 8.0f);
        ImGui::TextUnformatted(patch.name.c_str());
        ImGui::PopTextWrapPos();
        if (locked) {
          ImGui::TextColored(kMuted, "Unavailable with FSR/DLSS: they need the game's blur pass.");
        }
        ImGui::TableSetColumnIndex(1);
        ImGui::BeginDisabled(locked);
        if (ImGui::Checkbox("##on", &on)) {
          g.extras[patch.name] = on;
          g.dirty = true;
        }
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered() && !patch.note.empty()) Tooltip(patch.note);
        ImGui::EndTable();
      }
      ImGui::PopID();
    }
    if (!any) ImGui::TextDisabled("No optional patches were found in patches/DriveClub.xml.");
  }
  EndCard();

  if (BeginCard("ALWAYS APPLIED")) {
    bool any = false;
    for (const Patch& patch : g_patches) {
      if (!patch.always) continue;
      any = true;
      ImGui::BulletText("%s", patch.name.c_str());
      if (!patch.note.empty() && ImGui::IsItemHovered()) Tooltip(patch.note);
    }
    if (!any) ImGui::TextDisabled("None.");
  }
  EndCard();
}

std::string g_log_text;

void LogsPage() {
  if (BeginCard("LAST RUN")) {
    if (g_log_text.empty()) {
      ImGui::TextDisabled("No run log yet. Press Play to prepare and start the game.");
    } else {
      ImGui::PushStyleColor(ImGuiCol_ChildBg, kTrack);
      ImGui::BeginChild("##last-run", ImVec2(ImGui::GetContentRegionAvail().x, 360), ImGuiChildFlags_Borders);
      ImGui::PushTextWrapPos(ImGui::GetContentRegionAvail().x);
      ImGui::TextUnformatted(g_log_text.c_str());
      ImGui::PopTextWrapPos();
      ImGui::EndChild();
      ImGui::PopStyleColor();
    }
    if (ImGui::Button("Refresh")) g_log_text = ReadTail(g_paths.data / "logs/last-run.log", 256 * 1024);
    ImGui::SameLine();
    if (ImGui::Button("Open logs folder")) OpenPath(g_paths.data / "logs");
    ImGui::SameLine();
    ImGui::TextDisabled("launcher.log and last-run.log");
  }
  EndCard();

  if (BeginCard("LAUNCHER")) {
    ImGui::TextWrapped("Launcher log: %s", (g_paths.data / "logs/launcher.log").string().c_str());
    ImGui::TextWrapped("Game folders, settings and saves are never written to. The prepared image lives in %s.",
                       (g_paths.data / "prep").string().c_str());
  }
  EndCard();
}

// ------------------------------------------------------------------------------------------
// root window
// ------------------------------------------------------------------------------------------
void ApplyTheme() {
  ImGuiStyle& style = ImGui::GetStyle();
  style.WindowPadding = ImVec2(8, 8);
  style.FramePadding = ImVec2(12, 7);
  style.ItemSpacing = ImVec2(10, 10);
  style.ItemInnerSpacing = ImVec2(8, 6);
  style.CellPadding = ImVec2(8, 6);
  style.WindowRounding = 0.0f;
  style.ChildRounding = 8.0f;
  style.FrameRounding = 6.0f;
  style.PopupRounding = 8.0f;
  style.ScrollbarRounding = 8.0f;
  style.GrabRounding = 6.0f;
  style.WindowBorderSize = 0.0f;
  style.ChildBorderSize = 1.0f;
  style.FrameBorderSize = 0.0f;
  style.PopupBorderSize = 1.0f;
  style.ScrollbarSize = 12.0f;
  style.GrabMinSize = 10.0f;

  auto set = [&](ImGuiCol col, const ImVec4& color) { style.Colors[col] = color; };
  set(ImGuiCol_Text, kText);
  set(ImGuiCol_TextDisabled, kMuted);
  set(ImGuiCol_WindowBg, kBg);
  set(ImGuiCol_ChildBg, kCard);
  set(ImGuiCol_PopupBg, kCard);
  set(ImGuiCol_Border, kCardLine);
  set(ImGuiCol_FrameBg, kTrack);
  set(ImGuiCol_FrameBgHovered, kTrackHover);
  set(ImGuiCol_FrameBgActive, kTrack);
  set(ImGuiCol_TitleBg, kHeaderBg);
  set(ImGuiCol_TitleBgActive, kHeaderBg);
  set(ImGuiCol_TitleBgCollapsed, kHeaderBg);
  set(ImGuiCol_Button, kTrack);
  set(ImGuiCol_ButtonHovered, kTrackHover);
  set(ImGuiCol_ButtonActive, kTrack);
  set(ImGuiCol_Header, kTrack);
  set(ImGuiCol_HeaderHovered, kTrackHover);
  set(ImGuiCol_HeaderActive, kTrack);
  set(ImGuiCol_Separator, kCardLine);
  set(ImGuiCol_SeparatorHovered, kAccent);
  set(ImGuiCol_SeparatorActive, kAccentHover);
  set(ImGuiCol_ScrollbarBg, ImVec4(0, 0, 0, 0));
  set(ImGuiCol_ScrollbarGrab, kOff);
  set(ImGuiCol_ScrollbarGrabHovered, kTrackHover);
  set(ImGuiCol_ScrollbarGrabActive, kAccent);
  set(ImGuiCol_CheckMark, kAccent);
  set(ImGuiCol_SliderGrab, kAccent);
  set(ImGuiCol_SliderGrabActive, kAccentHover);
  set(ImGuiCol_ResizeGrip, ImVec4(0, 0, 0, 0));
  set(ImGuiCol_ResizeGripHovered, kAccent);
  set(ImGuiCol_ResizeGripActive, kAccentHover);
  set(ImGuiCol_NavHighlight, kAccent);
  set(ImGuiCol_TableHeaderBg, kTrack);
  set(ImGuiCol_TableBorderStrong, kCardLine);
  set(ImGuiCol_TableBorderLight, kCardLine);
  set(ImGuiCol_TableRowBg, ImVec4(0, 0, 0, 0));
  set(ImGuiCol_TableRowBgAlt, ImVec4(1, 1, 1, 0.015f));
  set(ImGuiCol_TextSelectedBg, ImVec4(kAccent.x, kAccent.y, kAccent.z, 0.35f));
  set(ImGuiCol_ModalWindowDimBg, ImVec4(0, 0, 0, 0.65f));
}

void LoadFonts() {
  ImGuiIO& io = ImGui::GetIO();
  if (!IsFile(g_paths.font)) {
    ImFont* fallback = io.Fonts->AddFontDefault();
    g_font_body = g_font_title = g_font_small = fallback;
    return;
  }
  ImFontConfig config;
  config.OversampleH = 2;
  config.OversampleV = 2;
  config.PixelSnapH = true;
  const ImWchar* ranges = io.Fonts->GetGlyphRangesDefault();
  g_font_body = io.Fonts->AddFontFromFileTTF(g_paths.font.c_str(), 18.0f, &config, ranges);
  g_font_title = io.Fonts->AddFontFromFileTTF(g_paths.font.c_str(), 30.0f, &config, ranges);
  g_font_small = io.Fonts->AddFontFromFileTTF(g_paths.font.c_str(), 14.0f, &config, ranges);
  if (!g_font_body) g_font_body = io.Fonts->AddFontDefault();
  if (!g_font_title) g_font_title = g_font_body;
  if (!g_font_small) g_font_small = g_font_body;
}

void DrawHeader() {
  const ImVec2 pos = ImGui::GetWindowPos();
  const ImVec2 size = ImGui::GetWindowSize();
  ImDrawList* draw = ImGui::GetWindowDrawList();
  draw->AddRectFilled(pos, ImVec2(pos.x + size.x, pos.y + kHeaderH), ImGui::GetColorU32(kHeaderBg));
  draw->AddRectFilled(ImVec2(pos.x + kMargin, pos.y + 20), ImVec2(pos.x + kMargin + 4, pos.y + kHeaderH - 20),
                      ImGui::GetColorU32(kAccent));

  ImGui::PushFont(g_font_title);
  const ImVec2 title = ImGui::CalcTextSize("DRIVECLUB");
  ImGui::SetCursorScreenPos(ImVec2(pos.x + kMargin + 16, pos.y + 14));
  ImGui::TextColored(kAccent, "DRIVECLUB");
  ImGui::PopFont();

  ImGui::SetCursorScreenPos(ImVec2(pos.x + kMargin + 16 + title.x + 12, pos.y + 26));
  ImGui::TextColored(kText, "RECOMP");

  ImGui::PushFont(g_font_small);
  ImGui::SetCursorScreenPos(ImVec2(pos.x + kMargin + 16, pos.y + 58));
  ImGui::TextColored(kMuted, "NATIVE LINUX LAUNCHER");

  std::string version = g_version;
  if (!version.empty() && version[0] != 'v') version = "v" + version;
  const ImVec2 version_size = ImGui::CalcTextSize(version.c_str());
  ImGui::SetCursorScreenPos(ImVec2(pos.x + size.x - kMargin - version_size.x, pos.y + 26));
  ImGui::TextColored(kMuted, "%s", version.c_str());
  ImGui::SetCursorScreenPos(ImVec2(pos.x + size.x - kMargin - ImGui::CalcTextSize("APPIMAGE").x, pos.y + 48));
  ImGui::TextColored(kMuted, "APPIMAGE");
  ImGui::PopFont();

  draw->AddLine(ImVec2(pos.x, pos.y + kHeaderH - 1), ImVec2(pos.x + size.x, pos.y + kHeaderH - 1),
                ImGui::GetColorU32(kCardLine));
}

void DrawTabs() {
  const ImVec2 pos = ImGui::GetWindowPos();
  const ImVec2 size = ImGui::GetWindowSize();
  ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 16.0f);
  float x = kMargin;
  for (int i = 0; i < kTabCount; ++i) {
    const char* label = kTabName[i];
    const float button_width = ImGui::CalcTextSize(label).x + 28.0f;
    ImGui::SetCursorScreenPos(ImVec2(pos.x + x, pos.y + kHeaderH + 10));
    const bool active = g_tab == i;
    ImGui::PushStyleColor(ImGuiCol_Button, active ? kAccent : ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, active ? kAccentHover : kTrackHover);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, active ? kAccent : kTrack);
    ImGui::PushStyleColor(ImGuiCol_Text, active ? kWhite : kMuted);
    if (ImGui::Button(label, ImVec2(button_width, 32))) {
      if (g_tab != i) {
        g_tab = i;
        g_scroll_top = true;
      }
    }
    ImGui::PopStyleColor(4);
    x += button_width + 6.0f;
  }
  ImGui::PopStyleVar();
  ImGui::GetWindowDrawList()->AddLine(ImVec2(pos.x, pos.y + kHeaderH + kTabsH - 1),
                                      ImVec2(pos.x + size.x, pos.y + kHeaderH + kTabsH - 1),
                                      ImGui::GetColorU32(kCardLine));
}

void DrawContent() {
  const ImVec2 pos = ImGui::GetWindowPos();
  const ImVec2 size = ImGui::GetWindowSize();
  const float top = kHeaderH + kTabsH + 12.0f;
  const float height = std::max(100.0f, size.y - top - kFooterH - 12.0f);
  ImGui::SetCursorScreenPos(ImVec2(pos.x + kMargin, pos.y + top));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
  ImGui::PushStyleColor(ImGuiCol_ChildBg, kBg);
  ImGui::BeginChild("##content", ImVec2(size.x - 2 * kMargin, height), ImGuiChildFlags_None,
                    ImGuiWindowFlags_AlwaysVerticalScrollbar);
  if (g_scroll_top) {
    ImGui::SetScrollY(0);
    g_scroll_top = false;
  }
  switch (g_tab) {
    case kTabGame: GamePage(); break;
    case kTabDisplay: DisplayPage(); break;
    case kTabDriving: DrivingPage(); break;
    case kTabPatches: PatchesPage(); break;
    case kTabLogs: LogsPage(); break;
    default: break;
  }
  ImGui::EndChild();
  ImGui::PopStyleColor();
  ImGui::PopStyleVar();
}

void DrawFooter() {
  const ImVec2 pos = ImGui::GetWindowPos();
  const ImVec2 size = ImGui::GetWindowSize();
  const float y = size.y - kFooterH;
  ImDrawList* draw = ImGui::GetWindowDrawList();
  draw->AddRectFilled(ImVec2(pos.x, pos.y + y), ImVec2(pos.x + size.x, pos.y + size.y), ImGui::GetColorU32(kHeaderBg));
  draw->AddLine(ImVec2(pos.x, pos.y + y), ImVec2(pos.x + size.x, pos.y + y), ImGui::GetColorU32(kCardLine));

  if (!g_status.empty() && !g_status_error && SDL_GetTicks() > g_status_until) g_status.clear();

  const float buttons = 140.0f + 12.0f + 100.0f + 12.0f + 170.0f;
  const float status_width = std::max(120.0f, size.x - 2 * kMargin - buttons - 24.0f);
  ImGui::SetCursorScreenPos(ImVec2(pos.x + kMargin, pos.y + y + 24));
  ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + status_width);
  if (g_preparing) {
    ImGui::TextColored(kMuted, "Preparing the game files ...");
  } else if (!g_status.empty()) {
    ImGui::TextColored(g_status_error ? kBad : kMuted, "%s", g_status.c_str());
  } else if (FoldersOk()) {
    ImGui::TextDisabled("Ready. Press Play to prepare and start.");
  } else {
    ImGui::TextDisabled("Choose both game folders, then press Play.");
  }
  ImGui::PopTextWrapPos();

  ImGui::BeginDisabled(g_preparing || g_data_dialog_open || g_data_copying);
  const float play_x = size.x - kMargin - 140.0f;
  const float save_x = play_x - 12.0f - 100.0f;
  const float reset_x = save_x - 12.0f - 170.0f;
  ImGui::SetCursorScreenPos(ImVec2(pos.x + reset_x, pos.y + y + 20));
  if (ImGui::Button("Reset to defaults", ImVec2(170, 40))) {
    ResetDefaults();
    SetStatus("Defaults restored.", false);
  }
  ImGui::SetCursorScreenPos(ImVec2(pos.x + save_x, pos.y + y + 20));
  if (ImGui::Button("SAVE", ImVec2(100, 40))) SaveSettings();
  ImGui::SetCursorScreenPos(ImVec2(pos.x + play_x, pos.y + y + 20));
  if (PrimaryButton("PLAY", ImVec2(140, 40))) StartPrepare();
  ImGui::EndDisabled();
}

void DrawPrepare() {
  const ImVec2 pos = ImGui::GetWindowPos();
  const ImVec2 size = ImGui::GetWindowSize();
  ImGui::GetForegroundDrawList()->AddRectFilled(pos, ImVec2(pos.x + size.x, pos.y + size.y),
                                                IM_COL32(0, 0, 0, 175));
  std::string text, error;
  int step = 0;
  {
    std::lock_guard<std::mutex> lock(g_prep_mutex);
    text = g_prep_text;
    error = g_prep_error;
    step = g_prep_step.load();
  }
  ImGui::SetNextWindowPos(ImVec2(pos.x + size.x * 0.5f, pos.y + size.y * 0.5f), ImGuiCond_Always,
                          ImVec2(0.5f, 0.5f));
  ImGui::SetNextWindowSize(ImVec2(520, 0));
  ImGui::PushStyleColor(ImGuiCol_WindowBg, kCard);
  ImGui::Begin("##prepare", nullptr,
               ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize);
  ImGui::TextColored(kAccent, "PREPARING THE GAME");
  ImGui::Spacing();
  ImGui::TextWrapped("%s", text.c_str());
  ImGui::Spacing();
  ImGui::ProgressBar(std::clamp(step / 6.0f, 0.0f, 1.0f), ImVec2(ImGui::GetContentRegionAvail().x, 8), "");
  ImGui::TextDisabled("The first start takes a few minutes; later starts reuse the prepared image.");
  if (g_prep_done && !error.empty()) {
    ImGui::Spacing();
    ImGui::TextColored(kBad, "%s", error.c_str());
    if (ImGui::Button("Back", ImVec2(100, 34))) {
      g_preparing.store(false);
      g_prep_done.store(false);
      g_prep_joined.store(false);
    }
  }
  ImGui::End();
  ImGui::PopStyleColor();
}

void DrawUI() {
  const ImGuiViewport* viewport = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(viewport->Pos);
  ImGui::SetNextWindowSize(viewport->Size);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
  ImGui::Begin("##root", nullptr,
               ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                   ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus |
                   ImGuiWindowFlags_NoScrollbar);
  DrawHeader();
  DrawTabs();
  const bool busy = g_preparing || g_data_dialog_open || g_data_copying;
  if (busy) ImGui::BeginDisabled();
  DrawContent();
  if (busy) ImGui::EndDisabled();
  DrawFooter();
  if (g_preparing) DrawPrepare();
  if (g_data_dialog_open) DrawDataFolderDialog();
  ImGui::End();
  ImGui::PopStyleVar();
}

// ------------------------------------------------------------------------------------------
// entry point
// ------------------------------------------------------------------------------------------
int Run(int argc, char** argv) {
  InitPaths();
  if (argc > 1 && std::strcmp(argv[1], "--version") == 0) {
    std::printf("%s\n", g_version.c_str());
    return 0;
  }
  if (argc > 1 && (std::strcmp(argv[1], "--help") == 0 || std::strcmp(argv[1], "-h") == 0)) {
    std::printf("DriveClub Recomp Linux launcher\n\n"
                "  driveclub-launcher             open the launcher\n"
                "  driveclub-launcher --version   print the packaged version\n"
                "  driveclub-launcher --help      this text\n\n"
                "The AppImage starts this window when it is run without arguments.\n");
    return 0;
  }

  LoadSettings();

  if (!SDL_Init(SDL_INIT_VIDEO)) {
    std::fprintf(stderr, "driveclub-launcher: could not start SDL: %s\n", SDL_GetError());
    return 1;
  }
  if (const SDL_DisplayMode* mode = SDL_GetCurrentDisplayMode(SDL_GetPrimaryDisplay())) {
    if (mode->refresh_rate >= 50.0f) g_display_hz = int(mode->refresh_rate + 0.5f);
  }
  Sanitize();

  std::string title = "DriveClub Recomp " + g_version + " - Launcher";
  g_window = SDL_CreateWindow(title.c_str(), 1180, 800, SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
  if (!g_window) {
    std::fprintf(stderr, "driveclub-launcher: could not create the window: %s\n", SDL_GetError());
    SDL_Quit();
    return 1;
  }
  SDL_SetWindowMinimumSize(g_window, 900, 600);
  SDL_Renderer* renderer = SDL_CreateRenderer(g_window, nullptr);
  if (!renderer) {
    std::fprintf(stderr, "driveclub-launcher: could not create the renderer: %s\n", SDL_GetError());
    SDL_DestroyWindow(g_window);
    SDL_Quit();
    return 1;
  }
  SDL_SetRenderVSync(renderer, 1);

  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO& io = ImGui::GetIO();
  io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
  io.IniFilename = nullptr;
  LoadFonts();
  if (!ImGui_ImplSDL3_InitForSDLRenderer(g_window, renderer)) {
    std::fprintf(stderr, "driveclub-launcher: ImGui SDL3 backend failed: %s\n", SDL_GetError());
    ImGui::DestroyContext();
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(g_window);
    SDL_Quit();
    return 1;
  }
  if (!ImGui_ImplSDLRenderer3_Init(renderer)) {
    std::fprintf(stderr, "driveclub-launcher: ImGui renderer backend failed: %s\n", SDL_GetError());
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(g_window);
    SDL_Quit();
    return 1;
  }
  ApplyTheme();
  g_log_text = ReadTail(g_paths.data / "logs/last-run.log", 256 * 1024);

  while (!g_quit) {
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
      ImGui_ImplSDL3_ProcessEvent(&event);
      if (event.type == SDL_EVENT_QUIT) {
        if (!g_preparing) g_quit = true;
      } else if (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED && event.window.windowID == SDL_GetWindowID(g_window)) {
        if (!g_preparing) g_quit = true;
      }
    }
    PollDialog();
    PollPrepare();
    PollDataCopy();
    if (g_quit) break;

    ImGui_ImplSDL3_NewFrame();
    ImGui_ImplSDLRenderer3_NewFrame();
    ImGui::NewFrame();
    DrawUI();
    ImGui::Render();
    SDL_SetRenderDrawColor(renderer, 16, 18, 22, 255);
    SDL_RenderClear(renderer);
    ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), renderer);
    SDL_RenderPresent(renderer);
  }

  ImGui_ImplSDLRenderer3_Shutdown();
  ImGui_ImplSDL3_Shutdown();
  ImGui::DestroyContext();
  SDL_DestroyRenderer(renderer);
  SDL_DestroyWindow(g_window);
  SDL_Quit();
  if (!g_restart_data.empty()) {
    setenv("DRIVECLUB_DATA_DIR", g_restart_data.c_str(), 1);
    fs::path target;
    if (!g_paths.appimage.empty()) {
      target = g_paths.appimage;
    } else if (!g_paths.apprun.empty() && Exists(g_paths.apprun)) {
      target = g_paths.apprun;
    } else {
      std::error_code ec;
      target = fs::read_symlink("/proc/self/exe", ec);
    }
    if (!target.empty()) execl(target.c_str(), target.c_str(), (char*)nullptr);
    std::fprintf(stderr,
                 "driveclub-launcher: data folder changed but the launcher could not restart: %s\n",
                 std::strerror(errno));
    return 1;
  }
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  try {
    return Run(argc, argv);
  } catch (const std::exception& error) {
    std::fprintf(stderr, "driveclub-launcher: %s\n", error.what());
    return 1;
  } catch (...) {
    std::fprintf(stderr, "driveclub-launcher: unknown error\n");
    return 1;
  }
}
