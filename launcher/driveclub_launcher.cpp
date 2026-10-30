// DriveClub (PC) - launcher.
//
// One native window: the two game folders, the game patches, a benchmark, Play. The look follows a dark card layout
// (rounded cards, pill switches, segmented choices, a footer with a red Play button).
// Play prepares the game once (a few minutes the first time, seconds afterwards), then starts it; this
// program closes. Settings live next to this program (launcher.ini, data\), nothing is installed.
//
// Only components that ship with Windows are used (user32, gdi32, GDI+, shell32, ole32), and the C++ runtime
// is linked statically.
//
//   DriveClub.exe              launcher window
//   DriveClub.exe --play       prepare if needed and start the game without a window

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <iphlpapi.h>
#include <windowsx.h>
#include <commctrl.h>
#include <objbase.h>
#include <shlobj.h>
#include <shellapi.h>
#include <tlhelp32.h>
#include <objidl.h>
#include <gdiplus.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdint>
#include <iterator>
#include <cstdio>
#include <cmath>
#include <cstring>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <string>
#include <sstream>
#include <thread>
#include <vector>
#include <nlohmann/json.hpp>

#include "datx.h"
#include "scene_track_names.h"
#include "gpu_check.h"
#include "graphics.h"
#include "prepare.h"
#include "unlock.h"
#include "carswitch.h"
#include "updater.h"

using namespace Gdiplus;

namespace {

// ------------------------------------------------------------------------------------------
// look
// ------------------------------------------------------------------------------------------
const Color kBg(255, 16, 18, 22), kCard(255, 24, 27, 33), kCardLine(255, 38, 42, 51), kText(255, 232, 234, 237),
    kMuted(255, 139, 147, 161), kTrack(255, 33, 37, 45), kTrackHover(255, 45, 50, 60), kAccent(255, 224, 64, 38),
    kAccentHover(255, 240, 84, 58), kOff(255, 63, 69, 82), kWhite(255, 245, 246, 248), kOk(255, 90, 200, 120),
    kWarn(255, 240, 170, 60), kBad(255, 240, 96, 84);

// The launcher's font: Segoe UI where there is one. Under Wine and Proton there often is not, and GDI+ then draws no text at
// all, so the first of the others that exists is used.
const FontFamily* UiFamily() {
  static const FontFamily* family = []() -> const FontFamily* {
    for (const wchar_t* name : {L"Segoe UI", L"Tahoma", L"Arial", L"Liberation Sans", L"DejaVu Sans", L"Verdana"}) {
      auto* candidate = new FontFamily(name);
      if (candidate->IsAvailable()) return candidate;
      delete candidate;
    }
    return FontFamily::GenericSansSerif();
  }();
  return family;
}

constexpr float kMargin = 24.f, kGap = 18.f, kHeaderH = 92.f, kFooterH = 96.f, kPad = 22.f, kTitleH = 44.f;
constexpr float kTabH = 46.f;  // the row of tabs under the header: one group of cards at a time
constexpr float kWinW = 960.f;

// ------------------------------------------------------------------------------------------
// model
// ------------------------------------------------------------------------------------------
// Picker: one box that opens a menu (benchmark track and car). Action: the benchmark's Run button and its result.
// Slider: a value from min to max (sel), dragged or stepped with the arrow keys. Button: a button across the row.
enum class Kind { Segmented, Toggle, Picker, Action, Slider, Button, Binding, Info, Monitor };

struct Ctl {
  const wchar_t* label;
  const wchar_t* hint;
  Kind kind;
  std::vector<std::wstring> items;     // segmented choices
  std::vector<std::string> patches;    // patch name per choice ("" = none); a toggle uses patches[0]
  int def = 0;                         // default choice (toggle: 1 = on)
  int sel = 0;
  RectF row{}, ctl{};
  RectF info{};                        // the (i) after the name, where it was last drawn (pixels; empty when not shown)
  const wchar_t* more = nullptr;       // the details the (i) opens, after the hint
  int min = 0, max = 0;                // slider range
  const wchar_t* unit = L"%";          // after a slider's value
  int decimals = 0;                    // a slider's value is shown divided by 10^decimals (gamma 240 as 2.40)
  bool physics_only = false;           // a steering wheel row shown with the physics force model only
  bool hidden = false;                 // a row that is not shown at all right now (a pad that is not plugged in)
  int loaded = 0;                      // steering wheel rows: the value default.ini had, so only changes are written
};

struct Card {
  const wchar_t* title;
  std::vector<int> ctls;
  int column;
  int tab = 0;  // the tab whose page this card is on (kTab*)
  const wchar_t* note = nullptr;
  float note_h = 0;  // the note's height at the current column width (LayoutPass)
  RectF rect{};
  int sub = -1;  // the inner tab of its tab (the Driving tab's kSub*), -1: none
};

std::vector<Ctl> g_c;
std::vector<Card> g_cards;

struct FolderRow {
  const wchar_t* label;
  const wchar_t* hint;
  std::wstring path;
  bool ok = false;
  RectF row{}, browse{};
};
FolderRow g_folder[2];  // 0 = update, 1 = base game

std::wstring g_home, g_data, g_runtime;
HWND g_hwnd = nullptr;
HWND g_edit = nullptr;  // the in-place box for the player name (or the host address)
bool g_edit_host = false;  // g_edit holds the host address, not the name
float g_dpi = 1.f;  // the display scale; g_scale is what is drawn at (smaller when the window is too short)
float g_scale = 1.f, g_content_h = 0, g_ox = 0, g_oy = 0;
constexpr float kMaxW = 1700.f;
float g_win_w = kWinW;       // logical window width: the layout reflows when the window is widened
float g_win_h = 0;           // logical window height; 0 or less than the content: as tall as the content needs
float g_client_h = 0;        // client height in pixels
float g_scroll = 0;          // pixels the content is scrolled up when the window is shorter than it
float g_natural_h = 0;       // the height the content needs
float g_row_extra = 0;       // extra space given to every row when the window is taller than the content
int g_hover_ctl = -1, g_hover_seg = -1, g_hover_btn = -1, g_hover_browse = -1, g_press_btn = -1;
int g_hover_tab = -1;
// Help: a click on the (i) after a row's name (g_hover_info: the one under the pointer) opens its details over the page.
int g_hover_info = -1, g_info_open = -1;

// The cards are split over tabs so a page holds what belongs together and needs no scrolling.
enum { kTabGraphics, kTabDetail, kTabGameplay, kTabDriving, kTabBench, kTabPatches, kTabCount };
const wchar_t* const kTabName[kTabCount] = {L"Display", L"Graphics", L"General", L"Driving", L"Benchmark", L"Patches"};
const wchar_t* const kTabHint[kTabCount] = {
    L"Frame rate, the sizes the game draws and shows, anti-aliasing and texture filtering.",
    L"Model, scenery, reflection and shadow detail, and the game's own camera effects.",
    L"Player identity, add-on content, startup and overlays.",
    L"Driving: the field of view and the game's driving assists, and the steering wheel (its setup and force feedback).",
    L"Drive one circuit on its own and get back the frame rate and the 1% and 0.1% lows.",
    L"Compatibility workarounds, community patches and advanced runtime options."};
int g_tab = kTabGraphics;
RectF g_tab_rect[kTabCount];
// The Discord button in the header: where it was drawn (layout pixels, before scaling), and whether the pointer is on it.
RectF g_discord_rect;
bool g_hover_discord = false;
const wchar_t* const kDiscordUrl = L"https://discord.gg/62b8jceumR";
bool HitDiscord(float x, float y) {
  const float m = 2.f * g_scale;
  return g_discord_rect.Width > 0 && x >= g_discord_rect.X * g_scale - m && x <= (g_discord_rect.X + g_discord_rect.Width) * g_scale + m &&
         y >= g_discord_rect.Y * g_scale - m && y <= (g_discord_rect.Y + g_discord_rect.Height) * g_scale + m;
}
// The Driving tab has tabs of its own, in a smaller row under the main one (HitTab gives them as kTabCount + n).
enum { kSubAssists, kSubWheel, kSubInput, kSubCount };
const wchar_t* const kSubName[kSubCount] = {L"Assists and view", L"Wheel and force feedback", L"Controller and keyboard"};
const wchar_t* const kSubHint[kSubCount] = {
    L"The field of view, and the game's gearbox, brake assist, hardcore handling and wheel assist.",
    L"Your steering wheel: Wheel Setup for its controls, and the force feedback.",
    L"Dead zones, the buttons and sticks of a pad, and what every controller reports right now."};
constexpr float kSubTabH = 62.f;
int g_subtab = kSubAssists;
RectF g_sub_rect[kSubCount];
bool HasSubTabs() { return g_tab == kTabDriving; }
bool OnPage(const Card& card) { return card.tab == g_tab && (card.sub < 0 || card.sub == g_subtab); }
std::wstring g_status;
std::wstring g_settings_error;
std::wstring g_gpu_problem;  // from CheckGpu() at start; Play stays off while it is set, unless the check is skipped
int g_lock_horizon = 0;    // the horizon stays level: 0 off, 1 cockpit, 2 all views (BB_LOCK_HORIZON); launcher.ini lock_horizon=
int g_camera_shake = -1;   // the Camera shake slider (BB_CAMERA_SHAKE, percent); launcher.ini camera_shake=
int camera_shake_saved = -1;
bool g_skip_gpu_check = false;  // "Skip the graphics card check" in the launcher (skip_gpu_check= in launcher.ini)
bool g_allow_overlays = false;  // "Let overlay programs into the game" (overlays= in launcher.ini)
bool g_two_thread = true;       // "Two-thread rendering" (draw_threads= in launcher.ini; off by default on AMD)
bool g_check_updates = true;  // "Check for new versions" (check_updates= in launcher.ini)
std::atomic<bool> g_update_ready{false};  // a newer release was found (g_update)
Updater::Release g_update;
RectF g_update_btn;  // the Update button, left of Reset to defaults (drawn while g_update_ready)
bool g_auto_play = false;  // "Start the game without this window" (auto_play= in launcher.ini)
bool GpuBlocked() { return !g_gpu_problem.empty() && !g_skip_gpu_check; }
Color g_status_color = kMuted;
bool g_status_sticky = false;  // a warning about the last click: shown over the hints until the next click
RectF g_btn[3];  // reset, save, play
float FooterButtonsLeft() { return g_update_ready ? g_update_btn.X : g_btn[0].X; }
const wchar_t* kBtnText[3] = {L"Reset to defaults", L"Save", L"Play"};
std::atomic<bool> g_busy{false};
std::atomic<int> g_progress{0};
std::wstring g_progress_text;
bool g_changed = false;

// ------------------------------------------------------------------------------------------
// text helpers
// ------------------------------------------------------------------------------------------
std::string Narrow(const std::wstring& w) {
  if (w.empty()) return "";
  int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), nullptr, 0, nullptr, nullptr);
  std::string s(n, '\0');
  WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), s.data(), n, nullptr, nullptr);
  return s;
}

std::wstring Widen(const std::string& s) {
  if (s.empty()) return L"";
  int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
  std::wstring w(n, L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
  return w;
}

std::wstring Dir(const std::wstring& path) {
  size_t i = path.find_last_of(L"\\/");
  return i == std::wstring::npos ? L"." : path.substr(0, i);
}

bool Exists(const std::wstring& p) { return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES; }

// This PC's IPv4 addresses a friend could join (a VPN first), each with what it belongs to.
std::vector<std::wstring> LocalAddresses() {
  std::vector<std::wstring> vpn, other;
  ULONG size = 0;
  GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER, nullptr, nullptr, &size);
  std::vector<char> buffer(size ? size : 1);
  auto* first = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());
  if (GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER, nullptr, first, &size) != NO_ERROR)
    return {};
  for (auto* a = first; a; a = a->Next) {
    if (a->OperStatus != IfOperStatusUp || a->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
    for (auto* u = a->FirstUnicastAddress; u; u = u->Next) {
      const auto* sa = reinterpret_cast<const sockaddr_in*>(u->Address.lpSockaddr);
      const unsigned char* b = reinterpret_cast<const unsigned char*>(&sa->sin_addr);
      if (b[0] == 169 && b[1] == 254) continue;
      wchar_t text[16];
      swprintf(text, 16, L"%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
      const char* kind = (b[0] == 100 && b[1] >= 64 && b[1] <= 127) ? "Tailscale" : b[0] == 25 ? "Hamachi" : b[0] == 26 ? "Radmin" : nullptr;
      std::wstring line = text;
      line += L"  (" + (kind ? Widen(kind) : std::wstring(a->FriendlyName ? a->FriendlyName : L"network")) + L")";
      (kind ? vpn : other).push_back(line);
    }
  }
  vpn.insert(vpn.end(), other.begin(), other.end());
  return vpn;
}

void MakeDirs(const std::wstring& path) {
  for (size_t i = 3; i < path.size(); ++i) {
    if (path[i] == L'\\') CreateDirectoryW(path.substr(0, i).c_str(), nullptr);
  }
  CreateDirectoryW(path.c_str(), nullptr);
}

std::vector<std::string> ReadLines(const std::wstring& path) {
  std::vector<std::string> lines;
  std::ifstream in(path.c_str(), std::ios::binary);
  for (std::string line; std::getline(in, line);) {
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
    lines.push_back(line);
  }
  return lines;
}

// ------------------------------------------------------------------------------------------
// settings: launcher.ini (folders) and data\user\patches_enabled.txt (what the game applies)
// ------------------------------------------------------------------------------------------
std::wstring IniPath() { return g_home + L"\\launcher.ini"; }
// The draw threads start off on AMD cards (they lost the device and ran slower there) and under Wine or Proton
// (stage B ran flat out and cost about a third of the frame rate in a report).
bool PrimaryGpuIsAmd() {
  if (GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "wine_get_version")) return true;
  DISPLAY_DEVICEW dd{};
  dd.cb = sizeof(dd);
  for (DWORD i = 0; EnumDisplayDevicesW(nullptr, i, &dd, 0); ++i, dd.cb = sizeof(dd)) {
    if (!(dd.StateFlags & DISPLAY_DEVICE_PRIMARY_DEVICE)) continue;
    std::wstring name = dd.DeviceString;
    for (auto& c : name) c = wchar_t(towlower(c));
    return name.find(L"amd") != std::wstring::npos || name.find(L"radeon") != std::wstring::npos;
  }
  return false;
}
std::wstring SelectionPath() { return g_data + L"\\user\\patches_enabled.txt"; }

// True when the first 64 KiB of the file contain the text.
bool FileHas(const std::wstring& path, const char* text) {
  std::ifstream in(path.c_str(), std::ios::binary);
  std::string data(65536, '\0');
  in.read(data.data(), std::streamsize(data.size()));
  data.resize(size_t(in.gcount()));
  return data.find(text) != std::string::npos;
}

// DriveClub's param.sfo: the US release (CUSA00093) or the European one (CUSA00003), whose 1.28 dump
// prepares and plays the same way.
bool IsDriveClub(const std::wstring& sfo) { return FileHas(sfo, "CUSA00093") || FileHas(sfo, "CUSA00003"); }

// The base game dump has an eboot.bin and an sce_sys\param.sfo too, so the update folder is told apart by what
// the base game has and it does not (game000.dat) and by its version (01.28 in param.sfo).
void CheckFolders() {
  // A dump that already is version 1.28 in one folder (game000.dat next to the 1.28 eboot.bin) serves as
  // both: the update field may stay empty or name the same folder.
  const std::wstring& b0 = g_folder[1].path;
  const bool merged = !b0.empty() && Exists(b0 + L"\\game000.dat") && Exists(b0 + L"\\eboot.bin") &&
                      Exists(b0 + L"\\sce_sys\\param.sfo") &&
                      IsDriveClub(b0 + L"\\sce_sys\\param.sfo") &&
                      FolderIsVersion128(std::filesystem::path(b0));
  if (merged && g_folder[0].path.empty()) g_folder[0].path = b0;
  const std::wstring& u = g_folder[0].path;
  if (merged && u == b0) {
    g_folder[0].ok = true;
    g_folder[1].ok = true;
    return;
  }
  g_folder[0].ok = !u.empty() && Exists(u + L"\\eboot.bin") && Exists(u + L"\\sce_sys\\param.sfo") &&
                   !Exists(u + L"\\game000.dat") && IsDriveClub(u + L"\\sce_sys\\param.sfo") &&
                   FolderIsVersion128(std::filesystem::path(u));
  const std::wstring& b = g_folder[1].path;
  g_folder[1].ok = !b.empty() && Exists(b + L"\\game000.dat") && IsDriveClub(b + L"\\sce_sys\\param.sfo");
}

// Why a chosen folder is not accepted, in words a player can act on ("" when it is fine or not chosen yet).
std::wstring FolderProblem(int which) {
  const std::wstring& p = g_folder[which].path;
  if (p.empty() || g_folder[which].ok) return L"";
  if (!Exists(p)) return L"This folder does not exist (any more). Press Browse and choose it again.";
  const bool eboot = Exists(p + L"\\eboot.bin"), base = Exists(p + L"\\game000.dat");
  const bool sfo = Exists(p + L"\\sce_sys\\param.sfo");
  if (which == 0) {
    if (base) return L"This folder holds the base game. If your dump is already version 1.28 in this one folder, choose the same folder for both fields. Otherwise choose the v1.28 update folder here.";
    if (!eboot || !sfo) return L"Not the update: no eboot.bin and sce_sys here. If it is a parent folder, open it and choose the folder inside.";
    if (!IsDriveClub(p + L"\\sce_sys\\param.sfo")) return L"This is another game, not DriveClub (CUSA00093 or CUSA00003). Choose the DriveClub v1.28 update.";
    return L"This update is not version 1.28 (its sce_sys\\param.sfo has another version). Choose the v1.28 update dump.";
  }
  if (eboot && !base) return L"This is the update, not the base game. Choose the base game folder here (it contains game000.dat).";
  if (base && !IsDriveClub(p + L"\\sce_sys\\param.sfo")) return L"This is another game, not DriveClub (CUSA00093 or CUSA00003). Choose the DriveClub base game folder.";
  return L"Not the base game: no game000.dat here. If it is a parent folder, open it and choose the folder inside.";
}

int lang_saved = 1;   // console language number, from launcher.ini (1 = English, US)
// The game has a text file for each of these (gui/lams), in console language-number order.
const wchar_t* const kLangName[21] = {L"Japanese", L"English (US)", L"French", L"Spanish", L"German", L"Italian", L"Dutch",
                                      L"Portuguese", L"Russian", L"Korean", L"Chinese (Traditional)",
                                      L"Chinese (Simplified)", L"Finnish", L"Swedish", L"Danish", L"Norwegian",
                                      L"Polish", L"Portuguese (Brazil)", L"English (UK)", L"Turkish",
                                      L"Spanish (Mexico)"};
int g_lang_id = 1;
int up_saved = 0;     // "Upscaling" choice, from launcher.ini
int sharp_saved = -1;  // "Sharpness" percent, from launcher.ini (-1: not saved yet)
int g_sharp = -1;
// "Frame cap" with the Unlocked (VRR) frame rate (fps_cap= in launcher.ini; 0 or missing: the display's refresh
// rate less 3, which follows the display when it changes).
int fps_cap_saved = 0;
int g_fps_cap = -1;
// The main display's refresh rate in Hz, 0 when unknown.
int DisplayHz() {
  DEVMODEW mode{};
  mode.dmSize = sizeof mode;
  return EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &mode) && mode.dmDisplayFrequency >= 50 ? int(mode.dmDisplayFrequency) : 0;
}
// HDR output (BB_HDR): on/off, and the brightness of white and of the highlights in nits (hdr=, hdr_paper=,
// hdr_peak= in launcher.ini; -1: not saved yet).
int g_hdr = -1, g_hdr_paper = -1, g_hdr_peak = -1;
int g_picture[3] = {-1, -1, -1};  // brightness, contrast, gamma (picture.ini; F4 in the game)
constexpr const char* kPictureKey[3] = {"brightness=", "contrast=", "gamma="};
int hdr_saved = -1, hdr_paper_saved = -1, hdr_peak_saved = -1;
void NameNativeUpscale();
int aa_saved = 0;     // "Anti-aliasing" choice, from launcher.ini
int fgen_saved = 0;   // "Frame generation" choice, from launcher.ini
int mirror_saved = 0;  // "Mirror and reflection updates" choice, from launcher.ini
int aniso_saved = 0;   // "Anisotropic filtering" choice, from launcher.ini
std::wstring g_name = L"Player";  // in-game player name, from "name=" in launcher.ini
std::wstring g_online_host;       // address of the host to join, from "online_host=" in launcher.ini
int g_online_row = -1, g_online_host_row = -1, g_online_ip_row = -1;  // "Online play" (Off, Host, Join) and "Host address" rows
int g_profile_row = -1, g_save_backup_row = -1, g_save_restore_row = -1;  // "Save profile" and its two buttons
int profile_saved = 0;  // "Save profile" choice, from "profile=" in launcher.ini
int online_saved = 0;             // "Online play" choice, from "online=" in launcher.ini
int g_inres = -1, g_wide = -1, g_outres = -1, g_preset = -1, g_mirror = -1, g_aniso = -1, g_fps = -1, g_fgen = -1, g_aa = -1, g_up = -1, g_lang = -1;
// Graphics detail rows (graphics.cpp): the control of each, and its choice from launcher.ini (-1 = not saved yet).
enum GfxRow { kGfxCar, kGfxOpp, kGfxScenery, kGfxSsr, kGfxShadows, kGfxParticles, kGfxDynres, kGfxBlur, kGfxDust,
              kGfxHaze, kGfxFxaa, kGfxRows };
int g_gfx[kGfxRows] = {-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1};
int g_motion_blur = -1;
int g_camera_patch_row = -1;
bool g_camera_patch = true;
bool g_camera_patch_saved = false;
int motion_blur_saved = 0;  // Slow by default; Fast uses the native 60 FPS exposure.
const wchar_t* MotionBlurHint(int choice) {
  return choice == 1
      ? L"Fast: shorter motion blur, matching the game's 60 FPS exposure."
      : L"Slow: longer motion blur, matching the original game's 30 FPS exposure. Best for a cinematic look.";
}
const wchar_t* MotionBlurReference() {
  return g_motion_blur >= 0 && g_c[g_motion_blur].sel == 1 ? L"short" : L"long";
}
HWND g_motion_blur_tooltip = nullptr;

int g_blur_patch = -1;  // the "Disable motion blur and depth of field" patch row
int gfx_saved[kGfxRows] = {-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1};
const char* const kGfxKey[kGfxRows] = {"gfx_car=", "gfx_opponents=", "gfx_scenery=", "gfx_reflections=", "gfx_shadows=",
                                       "gfx_particles=", "gfx_dynres=", "gfx_motion_blur=", "gfx_lens_dust=",
                                       "gfx_heat_haze=", "gfx_fxaa="};
int unlocked_saved = 0;  // "Unlocked" frame rate choice, from launcher.ini: 1 VRR, 2 no V-Sync  // indices of the render resolution and widescreen choices

// ------------------------------------------------------------------------------------------
// benchmark: the game's AI drives one event and the runtime measures the frame times (BB_BENCHMARK, see
// src/runtime_benchmark.c). The ids are the game's own: "Track Name" in newdata/tracks.csv and "Vehicle Name" in
// newdata/vehicles.csv, all from the base game (no add-on content needed). The ones marked "not run" are taken
// from the game data; the others were run to a result. Circuits only: a point-to-point route can end before the
// measurement does.
// ------------------------------------------------------------------------------------------
struct BenchChoice {
  const char* id;
  const wchar_t* name;
  const wchar_t* group;  // the submenu: location or car class
};
const BenchChoice kBenchTracks[] = {
    {"canada_road_circuit_02_n", L"Fraser Valley", L"Canada"},
    {"canada_road_circuit_01_n", L"Oliver's Landing", L"Canada"},
    {"canada_dlc_road_circuit_03_n", L"Sinclair Point", L"Canada"},
    {"canada_race_track_v1", L"Maplewood Motorsport Park", L"Canada"},
    {"chile_road_circuit_01_n", L"Chungará Lake", L"Chile"},
    {"chile_road_circuit_02_n", L"Salar de Surire", L"Chile"},
    {"chile_race_track_v1", L"Autodromo Frontera", L"Chile"},
    {"india_road_circuit_01_n", L"Glenmorgan", L"India"},
    {"india_road_circuit_02_n", L"Bandipur", L"India"},
    {"india_dlc_road_circuit_03_n", L"Yedappalli", L"India"},
    {"india_race_track_v1", L"Tamil Nadu Circuit", L"India"},  // not run
    {"norway_road_circuit_01_n", L"Sentraltind", L"Norway"},
    {"norway_road_circuit_02_n", L"Skjolden", L"Norway"},
    {"norway_dlc_road_circuit_03_n", L"Atlanterhavsvegen", L"Norway"},  // not run
    {"norway_race_track_01_v1", L"Sognefjord Raceway", L"Norway"},
    {"uk_road_circuit_01_n", L"Loch Duich", L"Scotland"},  // not run
    {"uk_road_circuit_02_n", L"Kinloch", L"Scotland"},
    {"uk_race_track_v1", L"Black Hills Circuit", L"Scotland"},
    {"japan_road_circuit_01_n", L"Takahagi Hills", L"Japan"},
    {"japan_road_circuit_02_n", L"Lake Shoji", L"Japan"},
    {"japan_dlc_road_circuit_03_n", L"Kobago", L"Japan"},
    {"japan_race_track_01_v1", L"Asigiri Highland Raceway", L"Japan"},  // not run
};
const BenchChoice kBenchCars[] = {
    {"mc_p1", L"McLaren P1", L"Hyper"},
    {"pa_hu", L"Pagani Huayra", L"Hyper"},
    {"ko_ag", L"Koenigsegg Agera R", L"Hyper"},
    {"he_ve_gt", L"Hennessey Venom GT", L"Hyper"},
    {"pa_zo", L"Pagani Zonda R", L"Hyper"},
    {"fe_458", L"Ferrari 458 Italia", L"Super"},
    {"fe_f12", L"Ferrari F12berlinetta", L"Super"},
    {"au_r8", L"Audi R8 V10 plus", L"Super"},
    {"mc_mp_12", L"McLaren 12C", L"Super"},
    {"as_ma_177", L"Aston Martin One-77", L"Super"},
    {"me_sl_am", L"Mercedes-Benz SLS AMG Black Series", L"Super"},  // not run
    {"bm_m4", L"BMW M4", L"Performance"},
    {"me_gt_am", L"Mercedes-AMG GT S", L"Performance"},
    {"au_rs6", L"Audi RS 6 Avant", L"Performance"},  // not run
    {"ma_mc_st", L"Maserati GranTurismo MC Stradale", L"Performance"},
    {"lo_ex", L"Lotus Exige S", L"Performance"},  // not run
    {"au_rs5", L"Audi RS 5 Coupé", L"Sports"},
    {"bm_m2", L"BMW M235i", L"Sports"},
    {"ren_dez", L"Renault DeZir", L"Sports"},
    {"vw_golf_gti", L"Volkswagen Golf GTI", L"Hot hatch"},
    {"mi_jcw", L"MINI John Cooper Works GP", L"Hot hatch"},
    {"ren_cl", L"Renault Clio RS", L"Hot hatch"},  // not run
};
constexpr int kBenchTrackCount = int(sizeof(kBenchTracks) / sizeof(kBenchTracks[0]));
constexpr int kBenchCarCount = int(sizeof(kBenchCars) / sizeof(kBenchCars[0]));
constexpr int kBenchDefaultCar = 5;  // Ferrari 458 Italia
const int kBenchTod[] = {8, 12, 18, 22};
const char* const kBenchWeather[] = {"Clear", "Cloudy", "Overcast", "Stormy"};
const char* const kBenchRain[] = {"", "Light", "Heavy", "Wet"};
const int kBenchSeconds[] = {30, 60, 120, 300};
int g_bench_track = 0, g_bench_car = kBenchDefaultCar;
int g_btrack = -1, g_bcar = -1, g_bmode = -1, g_btod = -1, g_bweather = -1, g_brain = -1, g_blen = -1, g_brun = -1;
bool IsBenchRow(const Ctl& c) { return c.patches[0].rfind("@bench", 0) == 0; }
// Driving: the game's own gearbox, brake assist, hardcore handling and wheel assist options, set by the launcher
// (choice 0: the game's own; the runtime sets the others with the game's setters: BB_GEARBOX, BB_BRAKE_ASSIST,
// BB_HARDCORE, BB_WHEEL_ASSIST, see src/runtime_pad.c). Kept in launcher.ini as driving_<name>=<choice>.
struct DriveRow { const char* key; const wchar_t* env; const wchar_t* const* values; int row; };
const wchar_t* const kGearboxEnv[] = {nullptr, L"automatic", L"manual"};
const wchar_t* const kBrakeEnv[] = {nullptr, L"off", L"low", L"medium", L"high"};
const wchar_t* const kOnOffEnv[] = {nullptr, L"off", L"on"};
DriveRow g_drive[4] = {{"driving_gearbox=", L"BB_GEARBOX", kGearboxEnv, -1}, {"driving_brake_assist=", L"BB_BRAKE_ASSIST", kBrakeEnv, -1},
                       {"driving_hardcore=", L"BB_HARDCORE", kOnOffEnv, -1}, {"driving_wheel_assist=", L"BB_WHEEL_ASSIST", kOnOffEnv, -1}};
bool IsDriveRow(const Ctl& c) { return c.patches[0].rfind("@drive", 0) == 0; }
// Field of view, percent of the game's own: data\user\camera.ini fov=, read by the game at start; F5 and F6 change
// it while playing and save it there (src/runtime_pad.c).
int g_fov = -1;
// The driving presets: the choice of each row of g_drive (0 = In-game). Sim: manual, no assists, hardcore handling;
// Assisted: automatic, medium brake assist, wheel assist; Game Defaults: the game's own options for all.
int g_drive_preset = -1;
const int kDrivePreset[3][4] = {{2, 1, 2, 1}, {1, 3, 1, 2}, {0, 0, 0, 0}};
void DeriveDrivePreset() {
  if (g_drive_preset < 0) return;
  g_c[g_drive_preset].sel = -1;
  for (int p = 0; p < 3; ++p) {
    bool same = true;
    for (int k = 0; k < 4; ++k) same = same && g_drive[k].row >= 0 && g_c[g_drive[k].row].sel == kDrivePreset[p][k];
    if (same) g_c[g_drive_preset].sel = p;
  }
}
void ApplyDrivePreset(int p) {
  for (int k = 0; k < 4; ++k) if (g_drive[k].row >= 0) g_c[g_drive[k].row].sel = kDrivePreset[p][k];
}
std::wstring CameraIniPath() { return g_data + L"\\user\\camera.ini"; }
// HDR brightness: Page Up / Page Down change it while playing and save it in data\user\hdr.ini paper= (gpu/shim/
// bbport_settings.cpp); it wins over launcher.ini's, being newer.
std::wstring HdrIniPath() { return g_data + L"\\user\\hdr.ini"; }
std::wstring PictureIniPath() { return g_data + L"\\user\\picture.ini"; }

// The last result, from data\user\benchmark\result.txt.
struct BenchResult {
  bool ok = false;
  std::wstring fps, lows, about;  // "87.3", "1% low 71.2  0.1% low 60.4", "Fraser Valley, Ferrari 458 Italia, ..."
};
BenchResult g_bench_result;
bool g_bench_running = false;
bool g_bench_ran_hover = false;  // the pointer is over the result: the panel with every setting the game ran with is shown
std::wstring g_bench_ran_short;               // one line: what the game actually ran with (render -> output, frame limit, ...)
std::vector<std::wstring> g_bench_ran_lines;  // every setting the game reported (result.txt line "ran ...")

// The game options the launcher sets for every start that were already set when it started: the player's own
// overrides (BB_FPS_LIMIT=40, ...). Only these are left alone; the launcher's own values are replaced each time.
std::vector<std::wstring> g_user_env;
void SnapshotUserEnv() {
  for (const wchar_t* name : {L"BB_FPS_LIMIT", L"BB_PRESENT_MODE", L"BB_LANGUAGE", L"BB_AA_SCALE", L"BB_AA", L"BB_FRAME_GEN", L"BB_FRAME_GEN_MULTIPLIER",
                             L"BB_GEARBOX", L"BB_BRAKE_ASSIST", L"BB_HARDCORE", L"BB_WHEEL_ASSIST"}) {
    if (GetEnvironmentVariableW(name, nullptr, 0) != 0) g_user_env.push_back(name);
  }
}
bool UserEnv(const wchar_t* name) { return std::find(g_user_env.begin(), g_user_env.end(), name) != g_user_env.end(); }

// ------------------------------------------------------------------------------------------
// steering wheel: the force feedback lines of data\user\input_config\default.ini (wheel_ffb_*, read by the game's
// runtime, src/runtime_wheel.inc; a running game takes changes within a second) and WheelSetup.exe for the controls.
// Only the lines changed here are written; the rest of the file stays as it is. Until the game has made the file
// (its first start) the choices wait in launcher.ini (ffb.<key>=) and are written into it once it exists.
// ------------------------------------------------------------------------------------------
int g_ffb_model = -1, g_ffb_preset = -1, g_ffb_softlock = -1, g_ffb_wave = -1, g_wheel_setup = -1;
int g_ffb_slider[7] = {-1, -1, -1, -1, -1, -1, -1};
int g_drag = -1;  // the slider being dragged
// The two starting points (gain, filter, minimum force, kerbs, road, slip, damper), as FFB Tuner's.
const int kWheelPreset[2][7] = {{100, 0, 0, 50, 50, 30, 10}, {130, 10, 8, 80, 80, 50, 0}};
const int kWheelPresetWave[2] = {0, 1};  // sine on a direct drive, square on gear and belt wheels
const char* const kSoftLock[6] = {"game", "off", "540", "720", "900", "1080"};
constexpr char kLayoutMarker[] = "# DriveClub racing layout";  // the game replaces a default.ini without it

bool PhysicsChosen() { return g_ffb_model >= 0 && g_c[g_ffb_model].sel == 0; }
bool UpscaleActive();
// FSR and DLSS work from the game's own anti-aliasing, which shares its shader with the blur: any way of turning
// the blur off turns them off too. While one is chosen the blur rows cannot be changed and are ignored.
bool BlurLocked() { return g_aa >= 0 && g_c[g_aa].sel > 0; }
// The two blur-off rows stay in view but cannot be changed while one is chosen.
bool BlurRowLocked(const Ctl& c) {
  return BlurLocked() && ((g_blur_patch >= 0 && &c == &g_c[g_blur_patch]) || (g_gfx[kGfxBlur] >= 0 && &c == &g_c[g_gfx[kGfxBlur]]));
}
// Rows that do nothing with the other choices: the physics settings with Original Physics; Upscaling and
// Sharpness without FSR or DLSS; Render resolution while Upscaling sets the render size.
bool RowHidden(const Ctl& c) {
  if (c.hidden) return true;
  if (c.physics_only && !PhysicsChosen()) return true;
  if ((g_up >= 0 && &c == &g_c[g_up]) || (g_sharp >= 0 && &c == &g_c[g_sharp])) return g_aa >= 0 && g_c[g_aa].sel == 0;
  if (g_inres >= 0 && &c == &g_c[g_inres]) return UpscaleActive();
  if (g_fps_cap >= 0 && &c == &g_c[g_fps_cap]) return g_fps >= 0 && g_c[g_fps].sel != 3;
  if (g_online_host_row >= 0 && &c == &g_c[g_online_host_row]) return g_online_row >= 0 && g_c[g_online_row].sel != 2;
  if (g_online_ip_row >= 0 && &c == &g_c[g_online_ip_row]) return g_online_row >= 0 && g_c[g_online_row].sel != 1;
  if ((g_hdr_paper >= 0 && &c == &g_c[g_hdr_paper]) || (g_hdr_peak >= 0 && &c == &g_c[g_hdr_peak]))
    return g_hdr >= 0 && g_c[g_hdr].sel == 0;
  for (int p : g_picture)  // the SDR picture; HDR has its own brightness
    if (p >= 0 && &c == &g_c[p]) return g_hdr >= 0 && g_c[g_hdr].sel != 0;
  return false;
}
bool IsWheelRow(const Ctl& c) { return c.patches[0].rfind("@ffb:", 0) == 0 || c.patches[0].rfind("@wheel", 0) == 0; }
bool IsFfbKeyRow(const Ctl& c) { return c.patches[0].rfind("@ffb:", 0) == 0; }
std::wstring WheelIniPath() { return g_data + L"\\user\\input_config\\default.ini"; }

int g_monitor_row = -1;
// The pad the launcher reads (binding capture + monitor): an XInput slot, -1 = the first connected one.
// Double-clicking a name in DEVICES sets it, and it is remembered for the next start.
int g_read_slot = -1;
std::vector<int> g_device_rows;        // one row per XInput slot, always created, hidden when unplugged
int g_dev_sdl_row = -1;                // one line listing the pads SDL3 sees
constexpr int kDeviceSlots = 4;        // XInput slots the launcher can read
bool g_dev_loaded = false;             // launcher.ini has been read: a fallback pick may be saved
int g_dev_click_row = -1;
ULONGLONG g_dev_click_at = 0;
std::string g_dev_sig;                 // what was plugged in at the last check
bool WheelIniReady() { return FileHas(WheelIniPath(), kLayoutMarker); }

// A row's value as default.ini has it, and back (INT32_MIN: a value the launcher does not offer, kept as it is).
std::string WheelValue(int i) {
  const Ctl& c = g_c[size_t(i)];
  if (i == g_ffb_model) return c.sel == 0 ? "physics" : "game";
  if (i == g_ffb_softlock) return kSoftLock[c.sel];
  if (i == g_ffb_wave) return c.sel == 0 ? "sine" : "square";
  if (c.kind == Kind::Toggle) return c.sel ? "true" : "false";
  return std::to_string(c.sel);
}
int WheelParse(int i, std::string v) {
  const Ctl& c = g_c[size_t(i)];
  for (char& ch : v) ch = char(tolower((unsigned char)ch));
  if (i == g_ffb_model) return v == "physics" ? 0 : v == "game" ? 1 : INT32_MIN;
  if (i == g_ffb_softlock) {
    for (int k = 0; k < 6; ++k) if (v == kSoftLock[k]) return k;
    return INT32_MIN;
  }
  if (i == g_ffb_wave) return v == "sine" ? 0 : v == "square" ? 1 : INT32_MIN;
  if (c.kind == Kind::Toggle) return v == "true" || v == "on" || v == "yes" || v == "1" ? 1 : 0;
  if (v.empty() || !isdigit((unsigned char)v[0])) return INT32_MIN;
  return (std::clamp)(atoi(v.c_str()), c.min, c.max);
}
// The preset row shows the preset whose values the sliders have, else none.
void DeriveWheelPreset() {
  if (g_ffb_preset < 0) return;
  g_c[g_ffb_preset].sel = -1;
  for (int p = 0; p < 2; ++p) {
    bool same = true;
    for (int k = 0; k < 7; ++k) same = same && g_c[g_ffb_slider[k]].sel == kWheelPreset[p][k];
    if (same) g_c[g_ffb_preset].sel = p;
  }
}
void ApplyWheelPreset(int p) {
  for (int k = 0; k < 7; ++k) g_c[g_ffb_slider[k]].sel = kWheelPreset[p][k];
  if (g_ffb_wave >= 0) g_c[g_ffb_wave].sel = kWheelPresetWave[p];
}
// "key = value" lines (comments after '#' dropped) into the rows.
void ApplyWheelLines(const std::vector<std::string>& lines, const char* prefix) {
  const size_t plen = std::strlen(prefix);
  for (std::string line : lines) {
    const size_t hash = line.find('#');
    if (hash != std::string::npos) line.resize(hash);
    size_t s = line.find_first_not_of(" \t");
    if (s == std::string::npos || line.compare(s, plen, prefix) != 0) continue;
    s += plen;
    const size_t eq = line.find('=', s);
    if (eq == std::string::npos) continue;
    std::string key = line.substr(s, eq - s), value = line.substr(eq + 1);
    while (!key.empty() && (key.back() == ' ' || key.back() == '\t')) key.pop_back();
    for (char& ch : key) ch = char(tolower((unsigned char)ch));
    value.erase(0, value.find_first_not_of(" \t"));
    while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) value.pop_back();
    for (size_t i = 0; i < g_c.size(); ++i) {
      if (!IsFfbKeyRow(g_c[i]) || g_c[i].patches[0].compare(5, std::string::npos, key) != 0) continue;
      const int v = WheelParse(int(i), value);
      g_c[i].sel = v == INT32_MIN ? -1 : v;
    }
  }
}
// Writes the rows that changed into default.ini: each key's line replaced, or added after the last wheel_ffb line.
bool WriteWheelChanges() {
  std::ifstream in(WheelIniPath().c_str(), std::ios::binary);
  if (!in) return false;
  std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  in.close();
  const std::string nl = text.find("\r\n") != std::string::npos ? "\r\n" : "\n";
  std::vector<std::string> lines;
  for (size_t p = 0; p <= text.size();) {
    size_t e = text.find('\n', p);
    if (e == std::string::npos) { if (p < text.size()) lines.push_back(text.substr(p)); break; }
    std::string l = text.substr(p, e - p);
    if (!l.empty() && l.back() == '\r') l.pop_back();
    lines.push_back(l);
    p = e + 1;
  }
  auto key_of = [](const std::string& l) {
    const size_t s = l.find_first_not_of(" \t");
    const size_t eq = l.find('=');
    if (s == std::string::npos || eq == std::string::npos || eq < s || l[s] == '#') return std::string();
    std::string k = l.substr(s, eq - s);
    while (!k.empty() && (k.back() == ' ' || k.back() == '\t')) k.pop_back();
    for (char& ch : k) ch = char(tolower((unsigned char)ch));
    return k;
  };
  bool any = false;
  for (size_t i = 0; i < g_c.size(); ++i) {
    Ctl& c = g_c[i];
    if (!IsFfbKeyRow(c) || c.sel < 0 || c.sel == c.loaded) continue;
    const std::string key = c.patches[0].substr(5), line = key + " = " + WheelValue(int(i));
    int at = -1, last_ffb = -1;
    for (size_t k = 0; k < lines.size(); ++k) {
      const std::string lk = key_of(lines[k]);
      if (lk == key) at = int(k);
      if (lk.rfind("wheel_ffb", 0) == 0) last_ffb = int(k);
    }
    if (at >= 0) lines[size_t(at)] = line;
    else if (last_ffb >= 0) lines.insert(lines.begin() + last_ffb + 1, line);
    else lines.push_back(line);
    any = true;
  }
  if (!any) return true;
  static bool backed_up = false;  // the file as it was before the launcher first changed it, once per start
  if (!backed_up) {
    CopyFileW(WheelIniPath().c_str(), (WheelIniPath() + L".launcher-backup").c_str(), FALSE);
    backed_up = true;
  }
  std::ofstream out(WheelIniPath().c_str(), std::ios::binary | std::ios::trunc);
  if (!out) return false;
  for (const auto& l : lines) out << l << nl;
  if (!out) return false;
  for (auto& c : g_c) if (IsFfbKeyRow(c) && c.sel >= 0) c.loaded = c.sel;
  return true;
}
// The rows from default.ini (the runtime's defaults where it has no line); choices still waiting in launcher.ini
// are written into it now that it exists.
bool IsPendingWheelSetting(const std::string& line) {
  if(line.rfind("ffb.",0)!=0) return false;
  const auto equal=line.find('='); if(equal==std::string::npos) return false;
  const auto key=line.substr(4,equal-4);
  for(const auto& c:g_c) if(IsFfbKeyRow(c) && c.patches[0].substr(5)==key) return true;
  return false;
}
void LoadWheel() {
  for (auto& c : g_c) if (IsFfbKeyRow(c)) c.sel = c.def;
  std::vector<std::string> pending;
  for (const auto& l : ReadLines(IniPath())) if (IsPendingWheelSetting(l)) pending.push_back(l);
  if (WheelIniReady()) ApplyWheelLines(ReadLines(WheelIniPath()), "");
  for (auto& c : g_c) if (IsFfbKeyRow(c)) c.loaded = c.sel;
  ApplyWheelLines(pending, "ffb.");
  if (!pending.empty() && WheelIniReady() && WriteWheelChanges()) {
    // written: the waiting lines go from launcher.ini
    std::vector<std::string> keep;
    for (const auto& l : ReadLines(IniPath())) if (!IsPendingWheelSetting(l)) keep.push_back(l);
    std::ofstream ini(IniPath().c_str(), std::ios::binary | std::ios::trunc);
    for (const auto& l : keep) ini << l << "\r\n";
  }
  DeriveWheelPreset();
}
// On Save and Play: into default.ini, or, before the game has made it, as ffb.<key>= lines for launcher.ini.
std::string SaveWheel() {
  if (WheelIniReady()) {
    if(!WriteWheelChanges()) g_settings_error=L"Could not save steering-wheel settings.";
    return "";
  }
  std::string pending;
  for (size_t i = 0; i < g_c.size(); ++i) {
    if (IsFfbKeyRow(g_c[i]) && g_c[i].sel >= 0) pending += "ffb." + g_c[i].patches[0].substr(5) + "=" + WheelValue(int(i)) + "\r\n";
  }
  return pending;
}
// WheelSetup.exe next to this program (or in the development tree).
void OpenWheelSetup() {
  for (const std::wstring& p : {g_home + L"\\WheelSetup.exe", g_home + L"\\..\\WheelSetup.exe", g_home + L"\\..\\tools\\WheelSetup.exe"}) {
    if (!Exists(p)) continue;
    const auto opened = reinterpret_cast<INT_PTR>(ShellExecuteW(g_hwnd, L"open", p.c_str(), nullptr, Dir(p).c_str(), SW_SHOWNORMAL));
    if (opened <= 32) {
      g_status = L"Wheel Setup could not be started (Windows error " + std::to_wstring(opened) + L").";
      g_status_color = kBad;
      return;
    }
    g_status = L"Wheel Setup is open: map the wheel, pedals, handbrake and shifter there, then press Save in its window.";
    g_status_color = kMuted;
    return;
  }
  g_status = L"WheelSetup.exe was not found next to DriveClub.exe.";
  g_status_color = kBad;
}

// ------------------------------------------------------------------------------------------

bool SaveTextAtomically(const std::wstring&, const std::string&);  // defined near the settings writer
// ------------------------------------------------------------------------------------------
// dead zones: analog_deadzone = leftjoystick|rightjoystick|l2|r2, inner, outer  (the runtime keeps
// inner < outer). Two slider rows per device; every other line of the file stays as it is.
// ------------------------------------------------------------------------------------------
struct DeadRowDef {
  const char* dev;
  const wchar_t* label;
  const wchar_t* hint;
  int part;
  int def;
};
const DeadRowDef kDeadRows[] = {
    {"leftjoystick", L"Left stick, inner",
     L"Steering that does nothing around the centre. Raise it if the car twitches while you hold the stick still.", 0, 5},
    {"leftjoystick", L"Left stick, outer",
     L"Where the stick counts as fully pushed. Lower it to reach full steering sooner.", 1, 127},
    {"rightjoystick", L"Right stick, inner",
     L"The same for the camera stick.", 0, 5},
    {"rightjoystick", L"Right stick, outer",
     L"The same for the camera stick.", 1, 127},
    {"l2", L"L2 trigger, inner",
     L"Travel before the brake starts to count. Raise it if a trigger rests slightly pressed.", 0, 0},
    {"l2", L"L2 trigger, outer",
     L"Where the trigger counts as fully pressed; lower it for a shorter pedal travel.", 1, 127},
    {"r2", L"R2 trigger, inner", L"The same for the throttle.", 0, 0},
    {"r2", L"R2 trigger, outer", L"The same for the throttle.", 1, 127},
};
constexpr int kDeadCount = int(sizeof(kDeadRows) / sizeof(kDeadRows[0]));
int g_dead_row[kDeadCount] = {-1, -1, -1, -1, -1, -1, -1, -1};
std::wstring g_dead_label[kDeadCount];

int AddDeadRow(int index) {
  const DeadRowDef& def = kDeadRows[index];
  g_dead_label[index] = def.label;
  Ctl c{g_dead_label[index].c_str(), def.hint, Kind::Slider, {},
        {std::string("@dead:") + def.dev + ":" + std::to_string(def.part)}, def.def, def.def};
  c.min = def.part == 0 ? 0 : 1;
  c.max = def.part == 0 ? 126 : 127;
  c.unit = L"";
  c.loaded = c.sel;   // untouched rows must not be written back
  g_c.push_back(std::move(c));
  return int(g_c.size()) - 1;
}
bool IsDeadRow(const Ctl& c) { return c.patches[0].rfind("@dead:", 0) == 0; }

void LoadDeadzones() {
  for (const auto& line : ReadLines(WheelIniPath())) {
    if (line.find("analog_deadzone") == std::string::npos) continue;
    const auto eq = line.find('=');
    if (eq == std::string::npos) continue;
    char dev[32] = {};
    int inner = 0, outer = 127;
    if (std::sscanf(line.c_str() + eq + 1, " %31[^, ] , %d , %d", dev, &inner, &outer) < 3) continue;
    for (char* p = dev; *p; ++p) *p = char(tolower((unsigned char)*p));
    for (int i = 0; i < kDeadCount; ++i) {
      if (std::string(kDeadRows[i].dev) != dev || g_dead_row[i] < 0) continue;
      Ctl& c = g_c[size_t(g_dead_row[i])];
      c.sel = kDeadRows[i].part == 0 ? inner : outer;
      c.loaded = c.sel;
    }
  }
}

// Writes one line per device whose two rows changed; everything else is kept byte for byte.
void FlushDeadzones() {
  const std::wstring path = WheelIniPath();
  if (!Exists(path)) return;
  auto lines = ReadLines(path);
  bool any = false;
  for (int i = 0; i < kDeadCount; i += 2) {
    if (g_dead_row[i] < 0 || g_dead_row[i + 1] < 0) continue;
    Ctl& inner_row = g_c[size_t(g_dead_row[i])];
    Ctl& outer_row = g_c[size_t(g_dead_row[i + 1])];
    if (inner_row.sel == inner_row.loaded && outer_row.sel == outer_row.loaded) continue;
    int inner = inner_row.sel, outer = outer_row.sel;
    if (inner >= outer) {  // the runtime requires inner < outer
      outer = (std::min)(127, inner + 1);
      if (outer <= inner) inner = outer - 1;
      inner_row.sel = inner;
      outer_row.sel = outer;
    }
    const std::string dev = kDeadRows[i].dev;
    const std::string line =
        "analog_deadzone = " + dev + ", " + std::to_string(inner) + ", " + std::to_string(outer);
    int at = -1;
    for (size_t k = 0; k < lines.size(); ++k) {
      if (lines[k].find("analog_deadzone") == std::string::npos) continue;
      if (lines[k].find(dev) != std::string::npos) at = int(k);
    }
    if (at >= 0) lines[size_t(at)] = line;
    else lines.push_back(line);
    inner_row.loaded = inner;
    outer_row.loaded = outer;
    any = true;
  }
  if (!any) return;
  static bool backed_up = false;
  if (!backed_up) {
    CopyFileW(path.c_str(), (path + L".launcher-backup").c_str(), FALSE);
    backed_up = true;
  }
  std::ofstream out(path.c_str(), std::ios::binary | std::ios::trunc);
  if (!out) return;
  for (const auto& line : lines) out << line << "\r\n";
}

// ------------------------------------------------------------------------------------------
// Controller and keyboard bindings. default.ini holds two lines for one action: a keyboard line whose
// right side is a key name (`r2 = w`) and a controller line whose right side is a pad input name
// (`r2 = r2`). That is the runtime's own rule (src/runtime_input.inc: a pad name on the right remaps the
// button, a key name on the right binds it), so the right side decides which chip a line belongs to —
// never the section comment, which the runtime ignores. Writing replaces that one line and leaves every
// other line of the file untouched.
// ------------------------------------------------------------------------------------------
struct BindRowDef;
bool PadTokenLine(const std::string& value);
const BindRowDef* FindBindRow(const std::string& out);
void ReadPair(const std::string& out, std::wstring* key, std::wstring* pad);


std::string TrimCopy(std::string value) {
  const auto first = value.find_first_not_of(" \t\r\n");
  if (first == std::string::npos) return {};
  value.erase(0, first);
  while (!value.empty() && (value.back() == ' ' || value.back() == '\t' || value.back() == '\r' ||
                            value.back() == '\n'))
    value.pop_back();
  return value;
}
// The left side of a `key = value` line, lower case; empty for comments and every other line.
std::string LhsOf(const std::string& line) {
  if (line.find('#') == 0) return {};
  const auto eq = line.find('=');
  if (eq == std::string::npos) return {};
  std::string key = TrimCopy(line.substr(0, eq));
  for (char& ch : key) ch = char(tolower((unsigned char)ch));
  return key;
}
// The right side up to the first comma, comment removed: a key name or a pad input name.
std::string RhsOf(const std::string& line) {
  const auto eq = line.find('=');
  if (eq == std::string::npos) return {};
  std::string value = line.substr(eq + 1);
  const auto hash = value.find('#');
  if (hash != std::string::npos) value.resize(hash);
  return TrimCopy(value.substr(0, value.find(',')));
}


// The only writer used for the input config: create/truncate, write the whole buffer, close.
// (SaveTextAtomically chained a temp file and could leave stale bytes; this cannot.)
bool SaveTextTruncate(const std::wstring& path, const std::string& data) {
  HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                         FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) return false;
  DWORD written = 0;
  const bool ok = data.empty() ||
                  WriteFile(h, data.data(), DWORD(data.size()), &written, nullptr) != 0;
  const bool flushed = ok && SetEndOfFile(h) != 0;
  CloseHandle(h);
  return ok && flushed && written == DWORD(data.size());
}

// Rewrites the one line that carries this action for that chip. A line is keyboard or controller by its
// right side (the runtime's rule), so the D-pad's `pad_up` and the Up arrow's `up` can never be confused,
// and every other line of the file is kept as it was, down to its line endings.
bool WriteBindValue(const std::string& key, const std::string& value, bool pad) {
  const std::wstring path = WheelIniPath();
  if (!Exists(path)) return false;   // no input config to change yet
  std::string raw;
  {
    std::ifstream in(path.c_str(), std::ios::binary);
    if (!in) return false;
    raw.assign((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  }
  std::vector<std::string> kept;
  int at = -1;
  for (const auto& line : ReadLines(path)) {
    if (LhsOf(line) == key && PadTokenLine(RhsOf(line)) == pad) {
      if (at >= 0) continue;   // a leftover second line for the same input: one is enough
      at = int(kept.size());
    }
    kept.push_back(line);
  }
  const std::string line = key + " = " + value;
  if (at >= 0) {
    kept[size_t(at)] = line;
  } else {   // first time this action is bound: keep the new line beside the others of its kind
    int after = -1;
    for (size_t i = 0; i < kept.size(); ++i) {
      if (!FindBindRow(LhsOf(kept[i]))) continue;
      if (PadTokenLine(RhsOf(kept[i])) == pad) after = int(i);
    }
    if (after >= 0) kept.insert(kept.begin() + after + 1, line);
    else kept.push_back(line);
  }
  // The file's own line ending and final newline stay as they are.
  const std::string eol = raw.find("\r\n") != std::string::npos ? "\r\n" : "\n";
  const bool final_newline = !raw.empty() && (raw.back() == '\n' || raw.back() == '\r');
  std::string joined;
  for (size_t i = 0; i < kept.size(); ++i) {
    joined += kept[i];
    if (i + 1 < kept.size() || final_newline) joined += eol;
  }
  static bool backed_up = false;
  if (!backed_up) {
    CopyFileW(path.c_str(), (path + L".launcher-backup").c_str(), FALSE);
    backed_up = true;
  }
  return SaveTextTruncate(path, joined);
}

// ------------------------------------------------------------------------------------------
// Binding capture: the next key the player presses becomes the value of that default.ini line.
// The names are exactly the ones the runtime reads (src/runtime_input.inc: key_names, plus a single
// letter, a digit, f1..f12 and kp0..kp9). Anything else would be written but never work.
// ------------------------------------------------------------------------------------------
std::string VkName(unsigned vk) {
  if (vk >= 'A' && vk <= 'Z') return std::string(1, char(vk - 'A' + 'a'));
  if (vk >= '0' && vk <= '9') return std::string(1, char(vk));
  if (vk >= VK_F1 && vk <= VK_F12) return "f" + std::to_string(vk - VK_F1 + 1);
  if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) return "kp" + std::to_string(vk - VK_NUMPAD0);
  switch (vk) {
    case VK_SPACE: return "space";
    case VK_TAB: return "tab";
    case VK_RETURN: return "enter";
    case VK_ESCAPE: return "escape";
    case VK_BACK: return "backspace";
    case VK_DELETE: return "delete";
    case VK_INSERT: return "insert";
    case VK_HOME: return "home";
    case VK_END: return "end";
    case VK_PRIOR: return "pageup";
    case VK_NEXT: return "pagedown";
    case VK_LEFT: return "left";
    case VK_RIGHT: return "right";
    case VK_UP: return "up";
    case VK_DOWN: return "down";
    case VK_LSHIFT: case VK_SHIFT: return "lshift";
    case VK_RSHIFT: return "rshift";
    case VK_LCONTROL: case VK_CONTROL: return "lctrl";
    case VK_RCONTROL: return "rctrl";
    case VK_LMENU: case VK_MENU: return "lalt";
    case VK_RMENU: return "ralt";
    case VK_CAPITAL: return "capslock";
    case VK_OEM_COMMA: return "comma";
    case VK_OEM_PERIOD: return "period";
    case VK_OEM_1: return "semicolon";
    case VK_OEM_2: return "slash";
    case VK_OEM_3: return "grave";
    case VK_OEM_4: return "leftbracket";
    case VK_OEM_5: return "backslash";
    case VK_OEM_6: return "rightbracket";
    case VK_OEM_7: return "apostrophe";
    case VK_OEM_MINUS: return "minus";
    case VK_OEM_PLUS: return "equals";
    case VK_MULTIPLY: return "kpmultiply";
    case VK_ADD: return "kpplus";
    case VK_SUBTRACT: return "kpminus";
    case VK_DIVIDE: return "kpdivide";
    case VK_DECIMAL: return "kpperiod";
    default: return {};
  }
}
// The generic VK_SHIFT / VK_CONTROL / VK_MENU report either side, so the poller skips them and reads the
// left and right keys, which are the names the runtime knows.
bool IsGenericModifier(unsigned vk) { return vk == VK_SHIFT || vk == VK_CONTROL || vk == VK_MENU; }

// Waits for a key press. controlFile may be empty; when it holds "cancel" the wait stops, and Esc stops
// it too (0.3.0 consumed the key in its own window, so Esc could never become a binding).
std::string CaptureKey(int timeoutMs, const std::wstring& controlFile) {
  const ULONGLONG until = GetTickCount64() + (ULONGLONG)timeoutMs;
  while (GetTickCount64() < until) {
    if (!controlFile.empty() && !ReadLines(controlFile).empty()) return {};   // cancelled
    if (GetAsyncKeyState(VK_ESCAPE) & 0x8000) return {};                     // Esc cancels
    for (unsigned vk = 8; vk < 256; ++vk) {
      if (vk >= VK_LBUTTON && vk <= VK_XBUTTON2) continue;   // mouse is not a binding
      if (IsGenericModifier(vk)) continue;
      if (vk == VK_ESCAPE) continue;
      if (!(GetAsyncKeyState(int(vk)) & 0x8000)) continue;
      std::string name = VkName(vk);
      if (!name.empty()) return name;
    }
    Sleep(25);
  }
  return {};
}

// XInput button mask -> the token default.ini uses for that button (PlayStation names).
const char* PadTokenFromMask(unsigned short b) {
  if (b & 0x1000) return "cross";
  if (b & 0x2000) return "circle";
  if (b & 0x4000) return "square";
  if (b & 0x8000) return "triangle";
  if (b & 0x0100) return "l1";
  if (b & 0x0200) return "r1";
  if (b & 0x0040) return "l3";
  if (b & 0x0080) return "r3";
  if (b & 0x0010) return "options";   // Start
  if (b & 0x0020) return "back";      // Back / Share
  if (b & 0x0001) return "pad_up";    // the D-pad tokens carry the `pad_` prefix: `up` is the Up arrow key
  if (b & 0x0002) return "pad_down";
  if (b & 0x0004) return "pad_left";
  if (b & 0x0008) return "pad_right";
  return nullptr;
}
// One exact XInput slot (0-based): the buttons held there, 0 when that slot has no pad.
int XInputRead(int slot, unsigned short* mask) {
  struct Pad { uint32_t packet; unsigned short buttons;
    unsigned char lt, rt; int16_t lx, ly, rx, ry; };
  using GetState = uint32_t(__stdcall*)(uint32_t, Pad*);
  HMODULE lib = LoadLibraryExW(L"xinput1_4.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
  if (!lib) return 0;
  const auto get = reinterpret_cast<GetState>(reinterpret_cast<void*>(GetProcAddress(lib, "XInputGetState")));
  int found = 0;
  if (get && slot >= 0 && slot < 4) {
    Pad pad{};
    if (get(uint32_t(slot), &pad) == 0) { *mask = pad.buttons; found = slot + 1; }
  }
  FreeLibrary(lib);
  return found;
}
// The pad the launcher reads: the chosen device, or the first connected one. 0 when none.
int XInputPressedMask(unsigned short* mask) {
  if (g_read_slot >= 0) return XInputRead(g_read_slot, mask);
  for (int slot = 0; slot < 4; ++slot)
    if (XInputRead(slot, mask)) return slot + 1;
  return 0;
}


// A controller button for the right chip, waiting for a new press (used by the WPF bridge). The
// cancellation file works like the wheel capture's: the shell writes "cancel" and the wait stops.
const char* CapturePadToken(int timeoutMs, const std::wstring& controlFile) {
  unsigned short mask = 0;
  if (!XInputPressedMask(&mask)) return nullptr;
  unsigned short held = mask;
  const ULONGLONG until = GetTickCount64() + (ULONGLONG)timeoutMs;
  while (GetTickCount64() < until) {
    Sleep(20);
    if (!controlFile.empty() && !ReadLines(controlFile).empty()) return nullptr;   // cancelled
    unsigned short now = 0;
    if (!XInputPressedMask(&now)) continue;
    const unsigned short fresh = (unsigned short)(now & ~held);
    held = now;
    if (const char* token = PadTokenFromMask(fresh)) return token;
  }
  return nullptr;
}

// The action a binding row edits, and whether its right chip only reads (an axis, or a trigger the
// runtime reads as an axis, so it cannot be remapped: 0.3.0's BindChipLocked).
std::string BindOutOf(const Ctl& c) {
  if (c.patches[0].size() <= 6) return {};
  std::string key = c.patches[0].substr(6);
  const auto ro = key.find('@');
  if (ro != std::string::npos) key.resize(ro);
  return key;
}
bool BindPadLocked(const Ctl& c) {
  return c.patches[0].size() > 3 && c.patches[0].compare(c.patches[0].size() - 3, 3, "@ro") == 0;
}

bool ApplyBinding(int ctlIndex, int side, const std::string& value) {
  if (ctlIndex < 0 || ctlIndex >= int(g_c.size())) return false;
  Ctl& c = g_c[size_t(ctlIndex)];
  if (c.kind != Kind::Binding) return false;
  if (side == 1 && BindPadLocked(c)) return false;
  const std::string key = BindOutOf(c);
  if (key.empty()) return false;
  if (!WriteBindValue(key, value, side == 1)) return false;
  if (c.items.size() < 2) c.items.resize(2);
  c.items[size_t(side)] = Widen(value);
  g_changed = true;
  return true;
}


// A binding row waits for a key the same way the FFB buttons wait for a wheel button: a timer polls,
// so the window keeps drawing and closing while it waits.
int g_bind_capture = -1;                 // the Ctl index waiting for a key
int g_bind_side = 0;                     // 0: keyboard chip, 1: pad chip
unsigned short g_bind_pad_mask = 0;      // the pad buttons held when the capture started
ULONGLONG g_bind_capture_until = 0;
constexpr UINT_PTR kBindCaptureTimer = 27;

void NameBindCapture() {
  if (g_bind_capture < 0) return;
  if (g_c[size_t(g_bind_capture)].items.size() < 2) g_c[size_t(g_bind_capture)].items.resize(2);
  g_c[size_t(g_bind_capture)].items[size_t(g_bind_side)] =
      g_bind_side == 1 ? L"Press a button\u2026  (Esc: cancel)" : L"Press a key\u2026  (Esc: cancel)";
}
void StopBindCapture(HWND h, bool cancelled) {
  KillTimer(h, kBindCaptureTimer);
  if (g_bind_capture >= 0 && cancelled) {
    Ctl& c = g_c[size_t(g_bind_capture)];
    const std::string key = BindOutOf(c);
    if (!key.empty()) {
      // What the file holds now, both chips: the wait never touched the file.
      std::wstring kb, pad;
      ReadPair(key, &kb, &pad);
      if (c.items.size() < 2) c.items.resize(2);
      c.items[0] = kb;
      c.items[1] = pad;
    }
    g_status = L"Binding unchanged.";
    g_status_color = kMuted;
    g_status_sticky = true;
  }
  g_bind_capture = -1;
  g_bind_side = 0;
}
void StartBindCapture(HWND h, int ctl, int side) {
  if (g_bind_capture >= 0) StopBindCapture(h, true);
  if (ctl < 0 || ctl >= int(g_c.size())) return;
  if (side == 1) {
    if (BindPadLocked(g_c[size_t(ctl)])) {
      g_status = L"That control is read as an axis and cannot be remapped.";
      g_status_color = kMuted;
      g_status_sticky = true;
      InvalidateRect(h, nullptr, FALSE);
      return;
    }
    unsigned short mask = 0;   // ignore buttons already held when the chip was clicked
    if (!XInputPressedMask(&mask)) {
      g_status = L"No controller found. Connect a pad, or use Wheel Setup for a wheel.";
      g_status_color = kWarn;
      g_status_sticky = true;
      InvalidateRect(h, nullptr, FALSE);
      return;
    }
    g_bind_pad_mask = mask;
  }
  g_bind_capture = ctl;
  g_bind_side = side;
  g_bind_capture_until = GetTickCount64() + 6000;
  NameBindCapture();
  SetTimer(h, kBindCaptureTimer, 40, nullptr);
  InvalidateRect(h, nullptr, FALSE);
}

void PollBindCapture(HWND h) {
  if (g_bind_capture < 0) return;
  if (GetTickCount64() > g_bind_capture_until) {
    StopBindCapture(h, true);
    InvalidateRect(h, nullptr, FALSE);
    return;
  }
  if (g_bind_side == 1) {   // the pad chip waits for a controller button
    unsigned short mask = 0;
    if (XInputPressedMask(&mask)) {
      const unsigned short fresh = (unsigned short)(mask & ~g_bind_pad_mask);
      g_bind_pad_mask = mask;
      const char* token = PadTokenFromMask(fresh);
      if (token) {
        const int ctl = g_bind_capture;
        g_bind_capture = -1;
        KillTimer(h, kBindCaptureTimer);
        ApplyBinding(ctl, 1, token);
        g_status = L"Binding saved.";
        g_status_color = kOk;
        g_status_sticky = true;
        InvalidateRect(h, nullptr, FALSE);
        return;
      }
    }
    return;
  }
  for (unsigned vk = 8; vk < 256; ++vk) {
    if (vk >= VK_LBUTTON && vk <= VK_XBUTTON2) continue;
    if (IsGenericModifier(vk)) continue;   // VK_LSHIFT / VK_LCONTROL / VK_LMENU carry the real names
    if (!(GetAsyncKeyState(int(vk)) & 0x8000)) continue;
    if (vk == VK_ESCAPE) {
      StopBindCapture(h, true);
      InvalidateRect(h, nullptr, FALSE);
      return;
    }
    const std::string name = VkName(vk);
    if (name.empty()) continue;
    const int ctl = g_bind_capture, side = g_bind_side;
    g_bind_capture = -1;
    KillTimer(h, kBindCaptureTimer);
    ApplyBinding(ctl, side, name);
    g_status = L"Binding saved.";
    g_status_color = kOk;
    g_status_sticky = true;
    InvalidateRect(h, nullptr, FALSE);
    return;
  }
}



// FFB gain + / - (force feedback card): two wheel buttons that make the force of the car being driven 5% stronger or
// weaker during a race (default.ini: wheel_button = <n>, ffb_gain_up / ffb_gain_down; the game keeps each car's
// percentage). A click waits for a button of the wheel the game reads (wheel_device). SDL3 is loaded at run time from
// runtime\, as the game has it.
// ------------------------------------------------------------------------------------------
void LauncherLog(const std::string& text);
struct SdlJoyApi {
  HMODULE lib = nullptr;
  bool (*InitSubSystem)(uint32_t) = nullptr;
  uint32_t (*WasInit)(uint32_t) = nullptr;
  uint32_t* (*GetJoysticks)(int*) = nullptr;
  void* (*OpenJoystick)(uint32_t) = nullptr;
  void (*CloseJoystick)(void*) = nullptr;
  const char* (*GetJoystickName)(void*) = nullptr;
  int (*GetNumJoystickButtons)(void*) = nullptr;
  bool (*GetJoystickButton)(void*, int) = nullptr;
  int (*GetNumJoystickAxes)(void*) = nullptr;
  int (*GetJoystickAxis)(void*, int) = nullptr;
  void (*UpdateJoysticks)() = nullptr;
  void (*Free)(void*) = nullptr;
};
SdlJoyApi g_joy;
bool LoadSdlJoysticks() {
  static bool tried = false;
  if (tried) return g_joy.lib != nullptr;
  tried = true;
  HMODULE lib = LoadLibraryExW((g_home + L"\\runtime\\SDL3.dll").c_str(), nullptr,
                               LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
  if (!lib) lib = LoadLibraryW(L"SDL3.dll");
  if (!lib) return false;
  auto get = [&](const char* name) { return reinterpret_cast<void*>(GetProcAddress(lib, name)); };
  g_joy.InitSubSystem = reinterpret_cast<bool (*)(uint32_t)>(get("SDL_InitSubSystem"));
  g_joy.WasInit = reinterpret_cast<uint32_t (*)(uint32_t)>(get("SDL_WasInit"));
  g_joy.GetJoysticks = reinterpret_cast<uint32_t* (*)(int*)>(get("SDL_GetJoysticks"));
  g_joy.OpenJoystick = reinterpret_cast<void* (*)(uint32_t)>(get("SDL_OpenJoystick"));
  g_joy.CloseJoystick = reinterpret_cast<void (*)(void*)>(get("SDL_CloseJoystick"));
  g_joy.GetJoystickName = reinterpret_cast<const char* (*)(void*)>(get("SDL_GetJoystickName"));
  g_joy.GetNumJoystickButtons = reinterpret_cast<int (*)(void*)>(get("SDL_GetNumJoystickButtons"));
  g_joy.GetJoystickButton = reinterpret_cast<bool (*)(void*, int)>(get("SDL_GetJoystickButton"));
  g_joy.GetNumJoystickAxes = reinterpret_cast<int (*)(void*)>(get("SDL_GetNumJoystickAxes"));
  g_joy.GetJoystickAxis = reinterpret_cast<int (*)(void*, int)>(get("SDL_GetJoystickAxis"));
  g_joy.UpdateJoysticks = reinterpret_cast<void (*)()>(get("SDL_UpdateJoysticks"));
  g_joy.Free = reinterpret_cast<void (*)(void*)>(get("SDL_free"));
  if (!g_joy.InitSubSystem || !g_joy.WasInit || !g_joy.GetJoysticks || !g_joy.OpenJoystick || !g_joy.CloseJoystick ||
      !g_joy.GetJoystickName || !g_joy.GetNumJoystickButtons || !g_joy.GetJoystickButton ||
      !g_joy.GetNumJoystickAxes || !g_joy.GetJoystickAxis || !g_joy.UpdateJoysticks ||
      !g_joy.Free) {
    FreeLibrary(lib);
    g_joy = SdlJoyApi{};
    return false;
  }
  g_joy.lib = lib;
  return true;
}
const char* const kFfbGainAction[2] = {"ffb_gain_up", "ffb_gain_down"};
int g_ffb_gain_row[2] = {-1, -1};
int g_ffb_gain_button[2] = {-1, -1};   // the wheel button of each, -1: none
int g_ffb_capture = -1;                // the one waiting for a wheel button (0 +, 1 -), -1: none
std::vector<void*> g_ffb_capture_joys;
std::vector<std::vector<char>> g_ffb_capture_held;   // buttons held: a press counts once they are let go
constexpr UINT_PTR kFfbCaptureTimer = 23;
constexpr uint32_t kSdlInitJoystick = 0x00000200;
// What SDL3, XInput and this window can see: the pads and wheels the game itself will use.
// A pad SDL names as a PlayStation one keeps the PlayStation button names; anything else (an Xbox pad, or
// the virtual pad a PS4 tool exposes over XInput) is shown with Xbox names. 0.3.0's rule, so the header,
// the binding chips and the monitor all agree. Must run before the rows are built.
bool DetectXboxNaming() {
  if (LoadSdlJoysticks()) {
    if (!g_joy.WasInit(kSdlInitJoystick)) g_joy.InitSubSystem(kSdlInitJoystick);
    int count = 0;
    uint32_t* ids = g_joy.GetJoysticks(&count);
    bool playstation = false;
    if (ids) {
      for (int i = 0; i < count && !playstation; ++i) {
        void* pad = g_joy.OpenJoystick(ids[i]);
        if (!pad) continue;
        const char* got = g_joy.GetJoystickName(pad);
        std::string name;
        for (const char* p = got; p && *p; ++p) name += char(tolower((unsigned char)*p));
        for (const char* key : {"dualsense", "dualshock", "wireless controller", "sony", "ps4", "ps5",
                                "playstation"}) {
          if (name.find(key) != std::string::npos) { playstation = true; break; }
        }
        g_joy.CloseJoystick(pad);
      }
      g_joy.Free(ids);
    }
    if (playstation) return false;   // a PlayStation pad is in use
  }
  unsigned short mask = 0;
  return XInputPressedMask(&mask) != 0;
}

extern bool g_xbox_names;
// The devices plugged in right now. No button-name line: each row already says what its buttons are.
std::wstring DeviceSummary() {
  std::wstring out;
  int slot = 1;
  if (LoadSdlJoysticks()) {
    if (!g_joy.WasInit(kSdlInitJoystick)) g_joy.InitSubSystem(kSdlInitJoystick);
    int count = 0;
    uint32_t* ids = g_joy.GetJoysticks(&count);
    if (ids) {
      for (int i = 0; i < count; ++i) {
        void* pad = g_joy.OpenJoystick(ids[i]);
        if (!pad) continue;
        const char* name = g_joy.GetJoystickName(pad);
        out += L"\nController " + std::to_wstring(slot++) + L": " +
               Widen(name && *name ? name : "Controller") + L"  (SDL)";
        g_joy.CloseJoystick(pad);
      }
      g_joy.Free(ids);
    }
  } else {
    out += L"\nSDL3.dll was not found in the runtime folder, so pads and wheels cannot be listed.";
  }

  struct PadState {
    uint32_t packet;
    unsigned short buttons;
    unsigned char lt, rt;
    int16_t lx, ly, rx, ry;
  };
  using GetState = uint32_t(__stdcall*)(uint32_t, PadState*);
  if (HMODULE xi = LoadLibraryExW(L"xinput1_4.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32)) {
    const auto get = reinterpret_cast<GetState>(reinterpret_cast<void*>(GetProcAddress(xi, "XInputGetState")));
    if (get) {
      for (uint32_t k = 0; k < 4; ++k) {
        PadState st{};
        if (get(k, &st) != 0) continue;
        out += L"\nXbox pad  \x00b7  XInput slot " + std::to_wstring(k);
      }
    }
    FreeLibrary(xi);
  }
  if (out.empty()) out = L"No controller is connected. The keyboard still works.";
  return out;
}
// The game's button names as players see them: the same wording the original input page used.
const wchar_t* FriendlyBind(const std::string& key) {
  struct Entry { const char* key; const wchar_t* label; };
  static const Entry kMap[] = {
      {"triangle", L"Triangle"}, {"circle", L"Circle"}, {"cross", L"Cross"}, {"square", L"Square"},
      {"up", L"D-pad up"}, {"down", L"D-pad down"}, {"left", L"D-pad left"}, {"right", L"D-pad right"},
      {"l1", L"Shoulder L1"}, {"r1", L"Shoulder R1"}, {"l2", L"Brake / reverse (L2)"},
      {"r2", L"Throttle (R2)"}, {"l3", L"LS click"}, {"r3", L"RS click"},
      {"options", L"Pause menu (OPTIONS)"}, {"touchpad", L"Touchpad"},
      {"touchpad_left", L"Touchpad left"}, {"touchpad_right", L"Touchpad right"},
      {"axis_left_x_minus", L"Steer left"}, {"axis_left_x_plus", L"Steer right"},
      {"axis_left_y_minus", L"Left stick up"}, {"axis_left_y_plus", L"Left stick down"},
      {"axis_right_x_minus", L"Right stick left"}, {"axis_right_x_plus", L"Right stick right"},
      {"axis_right_y_minus", L"Right stick up"}, {"axis_right_y_plus", L"Right stick down"},
  };
  for (const auto& e : kMap)
    if (key == e.key) return e.label;
  return nullptr;
}


// What every pad reports right now: SDL3 devices, then XInput slots.
std::wstring MonitorText() {
  std::wstring out;
  if (LoadSdlJoysticks()) {
    if (!g_joy.WasInit(kSdlInitJoystick)) g_joy.InitSubSystem(kSdlInitJoystick);
    g_joy.UpdateJoysticks();
    int count = 0;
    uint32_t* ids = g_joy.GetJoysticks(&count);
    if (ids) {
      for (int i = 0; i < count; ++i) {
        void* pad = g_joy.OpenJoystick(ids[i]);
        if (!pad) continue;
        const char* name = g_joy.GetJoystickName(pad);
        if (!out.empty()) out += L"\n";
        out += Widen(name && *name ? name : "Controller");
        out += L" (SDL3)";
        const int axes = g_joy.GetNumJoystickAxes(pad);
        const int buttons = g_joy.GetNumJoystickButtons(pad);
        out += L"\n  axes:";
        for (int a = 0; a < axes && a < 6; ++a) {
          const int raw = g_joy.GetJoystickAxis(pad, a);
          const int pct = raw < 0 ? -(int)((-raw) * 100 / 32768) : (int)(raw * 100 / 32768);
          out += L" " + std::to_wstring(pct);
        }
        std::wstring held;
        for (int b = 0; b < buttons && b < 64; ++b) {
          if (!g_joy.GetJoystickButton(pad, b)) continue;
          if (!held.empty()) held += L" ";
          held += std::to_wstring(b);
        }
        out += L"\n  buttons: " + (held.empty() ? std::wstring(L"none") : held);
        g_joy.CloseJoystick(pad);
      }
      g_joy.Free(ids);
    }
  }
  struct XInputState2 {
    uint32_t packet;
    struct {
      uint16_t buttons;
      uint8_t left_trigger, right_trigger;
      int16_t thumb_lx, thumb_ly, thumb_rx, thumb_ry;
    } gamepad;
  };
  using GetState2 = uint32_t(__stdcall*)(uint32_t, XInputState2*);
  if (HMODULE xi = LoadLibraryExW(L"xinput1_4.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32)) {
    const auto get = reinterpret_cast<GetState2>(reinterpret_cast<void*>(GetProcAddress(xi, "XInputGetState")));
    if (get) {
      const int read = g_read_slot;   // -1: the first connected pad
      for (uint32_t slot = 0; slot < 4; ++slot) {
        if (read >= 0 && int(slot) != read) continue;
        XInputState2 state{};
        if (get(slot, &state) != 0) continue;
        const auto& g = state.gamepad;
        auto pct = [](int v) { return v < 0 ? -(int)((-v) * 100 / 32768) : (int)(v * 100 / 32768); };
        if (!out.empty()) out += L"\n";
        out += L"Xbox controller " + std::to_wstring(slot + 1) + L" (XInput)";
        out += L"\n  LX " + std::to_wstring(pct(g.thumb_lx)) + L"  LY " + std::to_wstring(pct(g.thumb_ly)) +
               L"  RX " + std::to_wstring(pct(g.thumb_rx)) + L"  RY " + std::to_wstring(pct(g.thumb_ry));
        out += L"\n  LT " + std::to_wstring(g.left_trigger * 100 / 255) + L"  RT " +
               std::to_wstring(g.right_trigger * 100 / 255) + L"  buttons 0x" +
               [&] { wchar_t buf[8]; swprintf(buf, 8, L"%04X", g.buttons); return std::wstring(buf); }();
        break;   // one pad: the one the launcher reads
      }
    }
    FreeLibrary(xi);
  }
  return out.empty() ? std::wstring(L"No game controller is connected.") : out;
}

// The monitor keeps itself current while its card is on screen.
constexpr UINT_PTR kMonitorTimer = 29;
ULONGLONG g_dev_poll_at = 0;
void Relayout(HWND h);
bool SyncDevices(bool announce);
void RefreshMonitorRows(HWND h) {
  if (g_tab != kTabDriving || g_subtab != kSubInput) return;
  // A pad plugged in or removed while the page is open shows up on its own.
  if (GetTickCount64() - g_dev_poll_at > 500) {
    g_dev_poll_at = GetTickCount64();
    if (SyncDevices(true)) Relayout(h);   // a pad came or went: the rows change size
  }
  bool any = false;
  for (auto& c : g_c) {
    if (c.kind != Kind::Monitor || RowHidden(c)) continue;
    const std::wstring text = MonitorText();
    if (text != c.items[0]) { c.items[0] = text; any = true; }
  }
  if (any) InvalidateRect(h, nullptr, FALSE);
}
void NameFfbGainRows() {
  for (int k = 0; k < 2; ++k) {
    if (g_ffb_gain_row[k] < 0) continue;
    const std::wstring name = k ? L"FFB gain \x2212" : L"FFB gain +";
    g_c[size_t(g_ffb_gain_row[k])].items[0] =
        g_ffb_capture == k            ? L"Press the wheel button for " + name + L"   (Esc: cancel, Delete: none)"
        : g_ffb_gain_button[k] >= 0 ? name + L":  wheel button " + std::to_wstring(g_ffb_gain_button[k])
                                    : name + L":  not set (click, then press a wheel button)";
  }
}
// wheel_button = <n>, ffb_gain_up / ffb_gain_down
bool FfbGainLine(const std::string& line, int* button, int* which) {
  int n = -1;
  char what[32] = "";
  if (sscanf(line.c_str(), " wheel_button = %d , %31s", &n, what) != 2 || n < 0) return false;
  for (int k = 0; k < 2; ++k) {
    if (_stricmp(what, kFfbGainAction[k]) == 0) {
      *button = n;
      *which = k;
      return true;
    }
  }
  return false;
}
void LoadFfbGainButtons() {
  g_ffb_gain_button[0] = g_ffb_gain_button[1] = -1;
  for (const auto& l : ReadLines(WheelIniPath())) {
    int n, k;
    if (FfbGainLine(l, &n, &k)) g_ffb_gain_button[k] = n;
  }
  NameFfbGainRows();
}
// The two lines in default.ini after its other wheel_button lines, as WheelSetup writes them.
bool SaveFfbGainButtons() {
  std::ifstream in(WheelIniPath().c_str(), std::ios::binary);
  if (!in) return false;
  const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  in.close();
  const std::string nl = text.find("\r\n") != std::string::npos ? "\r\n" : "\n";
  std::vector<std::string> lines;
  int last_button = -1;
  for (const auto& l : ReadLines(WheelIniPath())) {
    int n, k;
    if (FfbGainLine(l, &n, &k)) continue;
    lines.push_back(l);
    if (sscanf(l.c_str(), " wheel_button = %d", &n) == 1) last_button = int(lines.size()) - 1;
  }
  std::vector<std::string> add;
  for (int k = 0; k < 2; ++k) {
    if (g_ffb_gain_button[k] >= 0) add.push_back("wheel_button = " + std::to_string(g_ffb_gain_button[k]) + ", " + kFfbGainAction[k]);
  }
  lines.insert(last_button >= 0 ? lines.begin() + last_button + 1 : lines.end(), add.begin(), add.end());
  CopyFileW(WheelIniPath().c_str(), (WheelIniPath() + L".launcher-backup").c_str(), FALSE);
  std::ofstream out(WheelIniPath().c_str(), std::ios::binary | std::ios::trunc);
  if (!out) return false;
  for (const auto& l : lines) out << l << nl;
  return bool(out);
}
// The wheel the game reads: wheel_device from default.ini without its " @..." part, lower case; empty for auto.
std::string FfbWheelName() {
  std::string name;
  for (std::string l : ReadLines(WheelIniPath())) {
    const size_t hash = l.find('#');
    if (hash != std::string::npos) l.resize(hash);
    const size_t s = l.find_first_not_of(" \t"), eq = l.find('=');
    if (s == std::string::npos || eq == std::string::npos || l.compare(s, 12, "wheel_device") != 0) continue;
    name = l.substr(eq + 1);
  }
  const size_t at = name.find(" @");
  if (at != std::string::npos) name.resize(at);
  name.erase(0, name.find_first_not_of(" \t"));
  while (!name.empty() && (name.back() == ' ' || name.back() == '\t')) name.pop_back();
  for (char& ch : name) ch = char(tolower((unsigned char)ch));
  return name == "auto" ? std::string() : name;
}
void StopFfbCapture(HWND h) {
  KillTimer(h, kFfbCaptureTimer);
  for (void* j : g_ffb_capture_joys) g_joy.CloseJoystick(j);
  g_ffb_capture_joys.clear();
  g_ffb_capture_held.clear();
  g_ffb_capture = -1;
  NameFfbGainRows();
}
void StartFfbCapture(HWND h, int k) {
  const bool again = g_ffb_capture == k;
  if (g_ffb_capture >= 0) StopFfbCapture(h);
  if (again) return;  // a second click cancels
  g_status_sticky = true;
  if (!WheelIniReady()) {
    g_status = L"Start the game once with the wheel connected: it writes the wheel settings these buttons go into.";
    g_status_color = kWarn;
    return;
  }
  if (!LoadSdlJoysticks()) {
    g_status = L"SDL3.dll was not found in the runtime folder, so the launcher cannot read the wheel.";
    g_status_color = kBad;
    return;
  }
  if (!(g_joy.WasInit(kSdlInitJoystick) & kSdlInitJoystick)) g_joy.InitSubSystem(kSdlInitJoystick);
  const std::string want = FfbWheelName();
  int count = 0;
  uint32_t* ids = g_joy.GetJoysticks(&count);
  for (int i = 0; i < count; ++i) {
    void* j = g_joy.OpenJoystick(ids[i]);
    if (!j) continue;
    std::string name = g_joy.GetJoystickName(j) ? g_joy.GetJoystickName(j) : "";
    for (char& ch : name) ch = char(tolower((unsigned char)ch));
    if (!want.empty() && name.find(want) == std::string::npos) {
      g_joy.CloseJoystick(j);  // the game reads the buttons of the wheel only
      continue;
    }
    g_ffb_capture_joys.push_back(j);
  }
  if (ids) g_joy.Free(ids);
  if (g_ffb_capture_joys.empty()) {
    g_status = L"The wheel was not found: connect it, or choose it in Wheel Setup.";
    g_status_color = kWarn;
    return;
  }
  g_joy.UpdateJoysticks();
  for (void* j : g_ffb_capture_joys) {
    std::vector<char> held(size_t((std::max)(0, g_joy.GetNumJoystickButtons(j))));
    for (size_t b = 0; b < held.size(); ++b) held[b] = g_joy.GetJoystickButton(j, int(b)) ? 1 : 0;
    g_ffb_capture_held.push_back(std::move(held));
  }
  g_ffb_capture = k;
  g_status.clear();
  NameFfbGainRows();
  SetTimer(h, kFfbCaptureTimer, 30, nullptr);
  LauncherLog("FFB gain button: waiting on " + std::to_string(g_ffb_capture_joys.size()) + " controller(s) of the wheel");
}
void SetFfbGainButton(HWND h, int k, int button) {
  g_ffb_gain_button[k] = button;
  if (button >= 0 && g_ffb_gain_button[1 - k] == button) g_ffb_gain_button[1 - k] = -1;  // one action a button
  const bool saved = SaveFfbGainButtons();
  StopFfbCapture(h);
  const std::wstring name = k ? L"FFB gain \x2212" : L"FFB gain +";
  g_status = !saved      ? L"Could not save the wheel buttons to default.ini."
             : button >= 0 ? name + L" is wheel button " + std::to_wstring(button) + L" (from the next start of the game)."
                           : name + L" has no button now.";
  g_status_color = saved ? kOk : kBad;
  g_status_sticky = true;
}
void PollFfbCapture(HWND h) {
  if (g_ffb_capture < 0) return;
  g_joy.UpdateJoysticks();
  for (size_t a = 0; a < g_ffb_capture_joys.size(); ++a) {
    auto& held = g_ffb_capture_held[a];
    for (size_t b = 0; b < held.size(); ++b) {
      const bool down = g_joy.GetJoystickButton(g_ffb_capture_joys[a], int(b));
      if (down && !held[b]) {
        SetFfbGainButton(h, g_ffb_capture, int(b));
        InvalidateRect(h, nullptr, FALSE);
        return;
      }
      if (!down) held[b] = 0;
    }
  }
}


// One row per action, so the page shows the same rows whatever default.ini holds yet.
struct BindRowDef {
  const char* out;
  const wchar_t* label;   // PlayStation name
  const wchar_t* xbox;    // the same button on an Xbox pad
  const wchar_t* hint;
  const char* key;   // the runtime's own default
  const char* pad;   // "" none, "=" not remappable (an axis: the right chip only reads)
};
const BindRowDef kBindButtons[] = {
    {"cross", L"Cross", L"A", L"Accept, and the handbrake in a race.", "enter", "cross"},
    {"circle", L"Circle", L"B", L"Back, and the handbrake in a race.", "space", "circle"},
    {"square", L"Square", L"X", L"Reset to track.", "x", "square"},
    {"triangle", L"Triangle", L"Y", L"Change camera.", "c", "triangle"},
    {"l1", L"L1", L"LB", L"Shift down.", "q", "l1"},
    {"r1", L"R1", L"RB", L"Shift up.", "e", "r1"},
    {"l2", L"Brake / reverse (L2)", L"Brake / reverse (LT)", L"Brake; hold to reverse.", "s", "l2"},
    {"r2", L"Throttle (R2)", L"Throttle (RT)", L"Accelerate.", "w", "r2"},
    {"l3", L"L3", L"LS click", L"Horn, and look behind.", "lctrl", "l3"},
    {"r3", L"R3", L"RS click", L"Look back.", "v", "r3"},
    {"options", L"Pause menu (OPTIONS)", L"Pause menu (Menu)", L"Pause the game.", "escape", "options"},
    {"touchpad_left", L"Touchpad left", L"View (left)", L"Reset to track (the left half of the touchpad).", "r", "back"},
    {"touchpad_right", L"Touchpad right", L"View (right)", L"Photo mode (the right half of the touchpad).", "t", "back"},
};
const BindRowDef kBindSticks[] = {
    {"pad_up", L"D-pad up", L"D-pad up", L"Menu up.", "up", "pad_up"},
    {"pad_down", L"D-pad down", L"D-pad down", L"Menu down.", "down", "pad_down"},
    {"pad_left", L"D-pad left", L"D-pad left", L"Menu left.", "left", "pad_left"},
    {"pad_right", L"D-pad right", L"D-pad right", L"Menu right.", "right", "pad_right"},
    {"axis_left_x_minus", L"Steer left", L"Steer left", L"Steering, left.", "a", ""},
    {"axis_left_x_plus", L"Steer right", L"Steer right", L"Steering, right.", "d", ""},
    {"axis_left_y_minus", L"Left stick up", L"Left stick up", L"Unbound unless you set it.", "", ""},
    {"axis_left_y_plus", L"Left stick down", L"Left stick down", L"Unbound unless you set it.", "", ""},
    {"axis_right_x_minus", L"Right stick left", L"Right stick left", L"Look left.", "j", ""},
    {"axis_right_x_plus", L"Right stick right", L"Right stick right", L"Look right.", "l", ""},
    {"axis_right_y_minus", L"Right stick up", L"Right stick up", L"Look up.", "i", ""},
    {"axis_right_y_plus", L"Right stick down", L"Right stick down", L"Look down.", "k", ""},
};
constexpr int kBindButtonCount = int(sizeof(kBindButtons) / sizeof(kBindButtons[0]));
constexpr int kBindStickCount = int(sizeof(kBindSticks) / sizeof(kBindSticks[0]));

// The controller inputs a row can be remapped to (src/runtime_input.inc, sdl_button_names).
struct PadChoice { const char* id; const wchar_t* name; const wchar_t* xbox; const wchar_t* group; };
const PadChoice kPadChoices[] = {
    {"cross", L"Cross", L"A", L"Face buttons"}, {"circle", L"Circle", L"B", L"Face buttons"},
    {"square", L"Square", L"X", L"Face buttons"}, {"triangle", L"Triangle", L"Y", L"Face buttons"},
    {"l1", L"L1", L"LB", L"Shoulders"}, {"r1", L"R1", L"RB", L"Shoulders"},
    {"l3", L"L3", L"LS click", L"Sticks and buttons"}, {"r3", L"R3", L"RS click", L"Sticks and buttons"},
    {"options", L"OPTIONS", L"Menu", L"Sticks and buttons"}, {"back", L"Back / Share", L"View", L"Sticks and buttons"},
    {"touchpad_center", L"Touchpad", L"Touchpad", L"Sticks and buttons"},
    {"pad_up", L"D-pad up", L"D-pad up", L"D-pad"}, {"pad_down", L"D-pad down", L"D-pad down", L"D-pad"},
    {"pad_left", L"D-pad left", L"D-pad left", L"D-pad"}, {"pad_right", L"D-pad right", L"D-pad right", L"D-pad"},
};
constexpr int kPadChoiceCount = int(sizeof(kPadChoices) / sizeof(kPadChoices[0]));



bool IsAxisRow(const std::string& out) { return out.rfind("axis_", 0) == 0; }
bool LockedChip(const std::string& out) { return out == "l2" || out == "r2" || IsAxisRow(out); }
const wchar_t* AxisText(const std::string& out) {
  if (out == "axis_left_x_minus") return L"Left stick \u2190";
  if (out == "axis_left_x_plus") return L"Left stick \u2192";
  if (out == "axis_left_y_minus") return L"Left stick \u2191";
  if (out == "axis_left_y_plus") return L"Left stick \u2193";
  if (out == "axis_right_x_minus") return L"Right stick \u2190";
  if (out == "axis_right_x_plus") return L"Right stick \u2192";
  if (out == "axis_right_y_minus") return L"Right stick \u2191";
  return L"Right stick \u2193";
}

// The rows of the Controller and keyboard page: the 0.3.0 action tables, so a row exists even when
// default.ini has no line for it yet. items[0] is the keyboard key, items[1] the pad button.
int g_bind_buttons[kBindButtonCount];
int g_bind_sticks[kBindStickCount];
int g_bind_n_buttons = 0, g_bind_n_sticks = 0;

bool PadTokenLine(const std::string& value) {
  for (int i = 0; i < kPadChoiceCount; ++i)
    if (value == kPadChoices[i].id) return true;
  return false;
}

// The game's own action rows, so `axis_...` and every other foreign line stays out of the way.
const BindRowDef* FindBindRow(const std::string& out) {
  for (int i = 0; i < kBindButtonCount; ++i)
    if (out == kBindButtons[i].out) return &kBindButtons[i];
  for (int i = 0; i < kBindStickCount; ++i)
    if (out == kBindSticks[i].out) return &kBindSticks[i];
  return nullptr;
}

// The two lines of one action: the keyboard value (the first keyboard line) and the controller value.
// A pad input name on the right is a controller line; anything else is a key name. That is exactly how
// the runtime sorts them, and it is why the D-pad's `pad_up` and the Up arrow's `up` cannot be mixed up.
void ReadPair(const std::string& out, std::wstring* key, std::wstring* pad) {
  key->clear();
  pad->clear();
  bool have_key = false;
  for (const auto& line : ReadLines(WheelIniPath())) {
    if (LhsOf(line) != out) continue;
    const std::string value = RhsOf(line);
    if (value.empty()) continue;
    if (PadTokenLine(value)) {
      *pad = Widen(value);
    } else if (!have_key) {
      *key = Widen(value);
      have_key = true;
    }
  }
}


bool g_xbox_names = false;   // an XInput pad is connected: show A/B/X/Y instead of Cross/Circle/...

const wchar_t* PadShowName(const std::wstring& token) {
  const std::string t = Narrow(token);
  for (int i = 0; i < kPadChoiceCount; ++i)
    if (t == kPadChoices[i].id) return g_xbox_names ? kPadChoices[i].xbox : kPadChoices[i].name;
  return nullptr;
}

void AddTableRow(const BindRowDef& def, int* list, int* count) {
  const std::string out = def.out;
  const bool locked = LockedChip(out);
  std::wstring key, pad;
  ReadPair(out, &key, &pad);
  // No fallback to the table's defaults: a slot with no line for this action really has no binding,
  // and the chip has to say so instead of claiming the template's key.
  if (locked) {
    if (out == "l2") pad = g_xbox_names ? L"LT trigger" : L"L2 trigger";
    else if (out == "r2") pad = g_xbox_names ? L"RT trigger" : L"R2 trigger";
    else pad = AxisText(out);
  }
  const wchar_t* label = def.label;
  Ctl c{label, def.hint, Kind::Binding, {key, pad},
        {std::string("@bind:") + out + (locked ? "@ro" : "")}, 0, 0};
  g_c.push_back(std::move(c));
  list[(*count)++] = int(g_c.size()) - 1;
}

// ------------------------------------------------------------------------------------------
// The pads the launcher can see: one row per XInput slot, always created and hidden while that slot has
// no pad, so a pad plugged in later shows up without rebuilding the page. Double-clicking a pad's name
// makes the launcher read it (binding capture and the input monitor), and the row is red for that pad.
// ------------------------------------------------------------------------------------------
// One line of launcher.ini, rewritten where it is (the file's other lines are kept as they are).
void SaveIniLine(const std::string& key, const std::string& value) {
  const std::wstring path = IniPath();
  auto lines = ReadLines(path);
  bool done = false;
  for (auto& line : lines) {
    if (line.rfind(key + "=", 0) != 0) continue;
    line = key + "=" + value;
    done = true;
    break;
  }
  if (!done) lines.push_back(key + "=" + value);
  std::string out;
  for (const auto& l : lines) out += l + "\r\n";
  MakeDirs(g_home);
  SaveTextAtomically(path, out);
}
// The pad in use is remembered at once: every click in the shell is a new process, and it has to find
// the same pad.
void SaveReadSlot() { SaveIniLine("read_slot", std::to_string(g_read_slot)); }

// Reads what is plugged in and shows it: a row per connected pad, red for the one in use.
bool SyncDevices(bool announce) {
  if (g_device_rows.size() != size_t(kDeviceSlots)) return false;
  bool on[4] = {false, false, false, false};
  int count = 0, first = -1;
  for (int slot = 0; slot < 4; ++slot) {
    unsigned short mask = 0;
    on[slot] = XInputRead(slot, &mask) != 0;
    if (on[slot]) {
      ++count;
      if (first < 0) first = slot;
    }
  }
  // A pad that is not there (or none chosen yet) hands the launcher to the first one connected, so
  // there is always a red name while a pad is plugged in.
  if (g_read_slot < 0 || g_read_slot > 3 || !on[g_read_slot]) {
    const int before = g_read_slot;
    g_read_slot = first;
    if (g_dev_loaded && before != g_read_slot) SaveReadSlot();
  }
  for (int slot = 0; slot < 4; ++slot) {
    Ctl& row = g_c[size_t(g_device_rows[size_t(slot)])];
    row.hidden = !on[slot];
    row.sel = on[slot] && slot == g_read_slot ? 1 : 0;
  }
  if (g_dev_sdl_row >= 0) {
    Ctl& row = g_c[size_t(g_dev_sdl_row)];
    std::wstring names;
    int seen = 0;
    if (LoadSdlJoysticks()) {
      if (!g_joy.WasInit(kSdlInitJoystick)) g_joy.InitSubSystem(kSdlInitJoystick);
      int got = 0;
      uint32_t* ids = g_joy.GetJoysticks(&got);
      if (ids) {
        for (int i = 0; i < got; ++i) {
          void* pad = g_joy.OpenJoystick(ids[i]);
          if (!pad) continue;
          const char* name = g_joy.GetJoystickName(pad);
          if (seen++) names += L"\n";
          names += Widen(name && *name ? name : "Controller") + L"  (SDL)";
          g_joy.CloseJoystick(pad);
        }
        g_joy.Free(ids);
      }
    }
    row.hidden = seen == 0;
    row.items[0] = names;
  }
  if (g_monitor_row >= 0) g_c[size_t(g_monitor_row)].items[0] = MonitorText();
  std::string sig;
  for (int slot = 0; slot < 4; ++slot) sig += on[slot] ? '1' : '0';
  const bool changed = sig != g_dev_sig;
  if (changed) {
    g_dev_sig = sig;
    if (announce) {
      if (count != 0) {
        g_status = L"Controller connected: XInput slot " + std::to_wstring(g_read_slot + 1) +
                   L" is now the pad the launcher reads.";
        g_status_color = kOk;
      } else {
        g_status = L"Controller disconnected. The keyboard still works.";
        g_status_color = kWarn;
      }
      g_status_sticky = true;
    }
  }
  return changed;
}
// ------------------------------------------------------------------------------------------
// The built-in template: the game's own pad inputs and its racing keyboard layout, one line per action.
// Picking a pad in DEVICES writes both into default.ini (every other line stays), and the pad's kind
// decides how the buttons are named on the page.
// ------------------------------------------------------------------------------------------
std::vector<std::string> PadTemplateLines() {
  std::vector<std::string> out;
  for (int i = 0; i < kBindButtonCount; ++i) {
    const BindRowDef& d = kBindButtons[i];
    if (!d.pad || !*d.pad) continue;
    out.push_back(std::string(d.out) + " = " + d.pad);
  }
  for (int i = 0; i < kBindStickCount; ++i) {
    const BindRowDef& d = kBindSticks[i];
    if (!d.pad || !*d.pad) continue;
    out.push_back(std::string(d.out) + " = " + d.pad);
  }
  return out;
}
// The keyboard half of the template: the game's racing layout (WASD, the arrows and the menu keys).
std::vector<std::string> KeyboardTemplateLines() {
  std::vector<std::string> out;
  for (int i = 0; i < kBindButtonCount; ++i)
    if (kBindButtons[i].key && *kBindButtons[i].key)
      out.push_back(std::string(kBindButtons[i].out) + " = " + kBindButtons[i].key);
  for (int i = 0; i < kBindStickCount; ++i)
    if (kBindSticks[i].key && *kBindSticks[i].key)
      out.push_back(std::string(kBindSticks[i].out) + " = " + kBindSticks[i].key);
  return out;
}
bool IsPadBindingLine(const std::string& line) { return PadTokenLine(RhsOf(line)); }
// Replaces the binding lines with the built-in template: `pad` and/or `kb` (nullptr keeps that half as it
// is). Every other line of default.ini stays where it is.
bool WriteBindingLines(const std::vector<std::string>* kb, const std::vector<std::string>* pad) {
  const std::wstring path = WheelIniPath();
  if (!Exists(path)) return false;
  std::vector<std::string> out;
  bool kb_done = false, pad_done = false;
  for (const auto& line : ReadLines(path)) {
    if (FindBindRow(LhsOf(line))) {
      const bool is_pad = IsPadBindingLine(line);
      if (is_pad ? pad != nullptr : kb != nullptr) {
        if (is_pad ? !pad_done : !kb_done) {
          out.insert(out.end(), is_pad ? pad->begin() : kb->begin(), is_pad ? pad->end() : kb->end());
          (is_pad ? pad_done : kb_done) = true;
        }
        continue;   // the old lines of this half are replaced
      }
      out.push_back(line);   // the half not being written stays as it is
      continue;
    }
    out.push_back(line);
  }
  if (!kb_done && kb != nullptr) out.insert(out.end(), kb->begin(), kb->end());
  if (!pad_done && pad != nullptr) out.insert(out.end(), pad->begin(), pad->end());
  // The file's own line ending and final newline stay as they are.
  std::string raw;
  {
    std::ifstream in(path.c_str(), std::ios::binary);
    if (!in) return false;
    raw.assign((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  }
  const std::string eol = raw.find("\r\n") != std::string::npos ? "\r\n" : "\n";
  const bool final_newline = !raw.empty() && (raw.back() == '\n' || raw.back() == '\r');
  std::string joined;
  for (size_t i = 0; i < out.size(); ++i) {
    joined += out[i];
    if (i + 1 < out.size() || final_newline) joined += eol;
  }
  CopyFileW(path.c_str(), (path + L".launcher-backup").c_str(), FALSE);
  return SaveTextTruncate(path, joined);
}
// Picking a pad writes the pad half only: the keyboard bindings are the player's own.
bool WritePadTemplate() {
  const std::vector<std::string> pad = PadTemplateLines();
  return WriteBindingLines(nullptr, &pad);
}
bool WriteKeyboardTemplate() {
  const std::vector<std::string> kb = KeyboardTemplateLines();
  return WriteBindingLines(&kb, nullptr);
}
// "Reset this page" on Controller and keyboard: both halves go back to the built-in template.
bool WriteFullTemplate() {
  const std::vector<std::string> kb = KeyboardTemplateLines();
  const std::vector<std::string> pad = PadTemplateLines();
  return WriteBindingLines(&kb, &pad);
}
// The keyboard side is never left empty: when default.ini has no keyboard binding at all, the game's own
// racing layout goes in. (The pad side is only filled by picking a pad in DEVICES.)
bool EnsureKeyboardTemplate() {
  const std::wstring path = WheelIniPath();
  if (!Exists(path)) return false;
  for (const auto& line : ReadLines(path))
    if (FindBindRow(LhsOf(line)) && !IsPadBindingLine(line)) return false;   // keys are already there
  return WriteKeyboardTemplate();
}
// ------------------------------------------------------------------------------------------
// L3 + R3 switch: both sticks pressed together tap the right half of the touchpad (the game's photo
// mode), which a pad without a touchpad does not have. The launcher keeps the switch in default.ini,
// where the runtime reads it.
// ------------------------------------------------------------------------------------------
bool g_l3r3_touchpad = true;
int g_l3r3_row = -1;
const char* kL3R3Key = "l3r3_touchpad_right";
// One `key = value` line of default.ini, rewritten where it is; every other line stays as it is.
bool WriteInputLine(const char* key, const char* value) {
  const std::wstring path = WheelIniPath();
  if (!Exists(path)) return false;
  std::string raw;
  {
    std::ifstream in(path.c_str(), std::ios::binary);
    if (!in) return false;
    raw.assign((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  }
  const std::string want = std::string(key) + " = " + value;
  std::vector<std::string> out;
  bool done = false;
  for (const auto& line : ReadLines(path)) {
    if (LhsOf(line) == key) {
      if (!done) {
        out.push_back(want);
        done = true;
      }
      continue;
    }
    out.push_back(line);
  }
  if (!done) out.push_back(want);
  const std::string eol = raw.find("\r\n") != std::string::npos ? "\r\n" : "\n";
  const bool final_newline = !raw.empty() && (raw.back() == '\n' || raw.back() == '\r');
  std::string joined;
  for (size_t i = 0; i < out.size(); ++i) {
    joined += out[i];
    if (i + 1 < out.size() || final_newline) joined += eol;
  }
  CopyFileW(path.c_str(), (path + L".launcher-backup").c_str(), FALSE);
  return SaveTextTruncate(path, joined);
}
bool WriteL3R3Setting(bool on) { return WriteInputLine(kL3R3Key, on ? "true" : "false"); }
// Reads the switch from default.ini; false when the file has no such line.
bool ReadL3R3Setting() {
  for (const auto& line : ReadLines(WheelIniPath())) {
    if (LhsOf(line) != kL3R3Key) continue;
    const std::string value = RhsOf(line);
    g_l3r3_touchpad = value == "true" || value == "1";
    return true;
  }
  return false;
}

// Re-reads both chips of every binding row (after the template was written).
void RefreshBindingChips() {
  for (int pass = 0; pass < 2; ++pass) {
    const int n = pass == 0 ? g_bind_n_buttons : g_bind_n_sticks;
    const int* list = pass == 0 ? g_bind_buttons : g_bind_sticks;
    for (int i = 0; i < n; ++i) {
      Ctl& c = g_c[size_t(list[i])];
      if (c.kind != Kind::Binding) continue;
      std::wstring kb, pad;
      ReadPair(BindOutOf(c), &kb, &pad);
      if (c.items.size() < 2) c.items.resize(2);
      c.items[0] = kb;
      if (!BindPadLocked(c)) c.items[1] = pad;
    }
  }
}

void BuildBindingRows() {
  g_bind_n_buttons = g_bind_n_sticks = 0;
  for (int i = 0; i < kBindButtonCount; ++i) AddTableRow(kBindButtons[i], g_bind_buttons, &g_bind_n_buttons);
  for (int i = 0; i < kBindStickCount; ++i) AddTableRow(kBindSticks[i], g_bind_sticks, &g_bind_n_sticks);
}

void BuildModel() {
  g_folder[0].label = L"Update folder (version 1.28)";
  g_folder[0].hint = L"The DriveClub v1.28 update dump: the folder that contains eboot.bin. If your dump is already 1.28, leave this empty and choose it below.";
  g_folder[1].label = L"Base game folder";
  g_folder[1].hint = L"The DriveClub base game dump: the folder that contains game000.dat.";
  auto seg = [](const wchar_t* label, const wchar_t* hint, std::vector<std::wstring> items,
                std::vector<std::string> patches, int def) {
    Ctl c{label, hint, Kind::Segmented, std::move(items), std::move(patches), def, def};
    g_c.push_back(std::move(c));
    return int(g_c.size()) - 1;
  };
  auto toggle = [](const wchar_t* label, const wchar_t* hint, const char* patch, bool def) {
    Ctl c{label, hint, Kind::Toggle, {}, {patch}, def ? 1 : 0, def ? 1 : 0};
    g_c.push_back(std::move(c));
    return int(g_c.size()) - 1;
  };
  g_preset = seg(L"Performance preset",
                 L"Picks the render and output sizes for your PC. Lower the preset if the game is slow; "
                 L"you can still change each size below. PS4 restores 30 FPS, native 1080p, the game's anti-aliasing and original graphics effects, without changing core compatibility fixes.",
                 {L"Custom", L"Handheld", L"Low", L"Medium", L"High", L"Ultra", L"PS4"}, {"@preset", "", "", "", "", "", ""}, 0);
  const int fps = (g_fps = int(g_c.size()), seg(L"Frame rate",
                      L"How fast the game runs.\n"
                      L"\x2022 30 FPS: as on PS4.\n"
                      L"\x2022 60 FPS: smooth, the speed stays right.\n"
                      L"\x2022 60 FPS (fixed tick): the engine's own 60 Hz mode.\n"
                      L"\x2022 Unlocked (VRR): for G-Sync or FreeSync, up to your refresh rate.\n"
                      L"\x2022 Unlimited: no cap, the picture can tear. Above 120 FPS the game can have issues.",
                      {L"30 FPS", L"60 FPS", L"60 FPS (fixed tick)", L"Unlocked (VRR)", L"Unlimited"},
                      {"", "60 FPS with deltatime", "60 FPS with fixed tickrate", "@unlocked", "@unlocked"}, 1));
  {
    const int top = DisplayHz() ? DisplayHz() - 3 : 117;
    Ctl c{L"Frame cap", L"The highest frame rate with Unlocked (VRR). The right end is your display's refresh rate less 3. "
          L"A little below what your PC can hold (for example 90 when it runs about 95-100) gives the steadiest, smoothest motion: "
          L"the graphics card keeps some room, so every frame takes the same time.",
          Kind::Slider, {}, {"@fpscap"}, top, top};
    c.min = 30;
    c.max = top;
    c.unit = L" FPS";
    g_fps_cap = int(g_c.size());
    g_c.push_back(std::move(c));
  }
  const int outres = (g_outres = int(g_c.size()), seg(L"Output resolution", L"The size of the game's finished picture (with the menus and HUD). It is then scaled to fill your screen, so the window and the signal to your display always stay at your screen's own size.",
                         {L"Default", L"720p", L"1440p", L"1800p", L"4K", L"5K", L"8K"},
                         {"", "720p [1280x720] Output resolution", "1440p [2560x1440] Output resolution",
                          "1800p [3200x1800] Output resolution", "4K [3840x2160] Output resolution",
                          "5K [5120x2880] Output resolution", "8K [7680x4320] Output resolution"},
                         0));
  const int inres = (g_inres = int(g_c.size()), seg(L"Render resolution",
                        L"The size the game draws the 3D scene at, before it is scaled to the output resolution: lower is faster, higher is sharper. The game's dynamic resolution can lower it further under load. Sizes above 1080p use more memory; the needed memory patch is added for you.",
                        {L"Default", L"720p", L"1440p", L"1800p", L"4K", L"8K"},
                        {"", "720p [1280x720] Internal resolution", "1440p [2560x1440] Internal resolution [DEPTH BUG]",
                         "1800p [3200x1800] Internal resolution [DEPTH BUG]", "4K [3840x2160] Internal resolution [DEPTH BUG]",
                         "8K [7680x4320] Internal resolution [DEPTH BUG]"},
                        00));
  const int wide = (g_wide = int(g_c.size()), seg(L"Widescreen (render and output)",
                       L"Ultrawide sizes. These change both the render and the output size. The needed memory patch is added for you.",
                       {L"Off", L"2560x1080", L"3440x1080", L"3840x1080", L"3440x1440",
                        L"5120x1440", L"5120x2160"},
                       {"", "1080p 21:9 [2560x1080] Internal + Output resolution [DEPTH BUG]",
                        "1080p 32:10 [3440x1080] Internal + Output resolution [DEPTH BUG]",
                        "1080p 32:9 [3840x1080] Internal + Output resolution [DEPTH BUG]",
                        "1440p 21:9 [3440x1440] Internal + Output resolution [DEPTH BUG]",
                        "1440p 32:9 [5120x1440] Internal + Output resolution [DEPTH BUG]",
                        "2160p 21:9 [5120x2160] Internal + Output resolution [DEPTH BUG]"},
                       00));
  const int fov = toggle(L"Widescreen field-of-view fix", L"For wide and ultrawide resolutions only.",
                         "FOV fix for wide resolutions", false);
  const int camera_patch = g_camera_patch_row = toggle(L"High-FPS camera fix",
      L"Smooths camera and replay shake at high frame rates. On by default for 60 FPS and unlocked modes, off at 30 FPS. Turn it off if the camera looks wrong.",
      "@camera_patch", true);
  const int blur = g_blur_patch = toggle(L"Disable motion blur and depth of field",
                          L"They share one shader, so both are turned off together. Cannot be changed while FSR or DLSS anti-aliasing is on: they need the game's blur pass.",
                          "Disable motion blur and depth of field", false);
  const int reset = toggle(L"No reset-to-track timer", L"Disables the reset to track timer. Players report stutter and cars teleporting with it on; turn it off if races feel wrong.", "Reset Disable", false);
  const int barrier = toggle(L"Disable track barriers", L"Removes the invisible barriers at the track edges. Players report stutter and cars teleporting with it on; turn it off if races feel wrong.",
                             "Barrier Disable", false);
  const int pcol = toggle(L"Photo mode: free camera through cars",
                          L"Disables car collision with the free camera, so you can look inside any car.",
                          "Disable Photomode Free Camera Collision", false);
  const int pproc = toggle(L"Photo mode: skip processing", L"Disables photo mode processing.",
                           "Disable Photomode Processing", false);
  g_lang = seg(L"Language",
               L"The language of the game's menus and text. Click to switch to the next one: all 21 languages of the game are available. Japanese, Korean and Chinese may show missing characters.",
               {L"English (US)"}, {"@lang"}, 0);
  const int pname = seg(L"Player name", L"The name the game shows for you. Click it to type a new one.", {g_name}, {"@name"}, 0);
  g_online_row = seg(L"Online play",
                     L"Off: the game runs as always. Host: a private server starts on this PC; tell your friends your IP address on the LAN or VPN (Tailscale, Hamachi, Radmin). "
                     L"Join: you connect to a friend who hosts; you will be asked for their IP address.",
                     {L"Off", L"Host a session", L"Join a session"}, {"@online", "@online", "@online"}, 0);
  g_online_ip_row = [&] {
    auto addresses = LocalAddresses();
    if (addresses.empty()) addresses.push_back(L"(no network found)");
    std::vector<std::string> tags(addresses.size(), "@myip");
    return seg(L"Your IP address", L"What your friends type under Host's IP address to join you. A VPN address (Tailscale, Hamachi, Radmin) works over the internet, a LAN address only at home.",
               addresses, tags, 0);
  }();
  g_online_host_row = seg(L"Host's IP address", L"The IP address of the friend who hosts, over the LAN or VPN (for example 100.67.1.2). Click it to type one.",
                          {g_online_host.empty() ? std::wstring(L"(not set)") : g_online_host}, {"@onhost"}, 0);
  g_mirror = seg(L"Mirror and reflection updates",
                 L"The game draws a second small scene (the mirrors, rain and reflections) every frame, about a quarter of all draw calls. "
                 L"Static draws it once and keeps that picture: much faster and steady, but reflections and rain on the glass stop updating.",
                 {L"Live", L"Static (faster)"}, {"@mirror", ""}, 0);
  g_aa = seg(L"Anti-aliasing",
             L"Replaces the game's own anti-aliasing with NVIDIA DLSS (GeForce RTX cards) or AMD FSR 3.1 (any graphics card) for a sharper, steadier picture, and makes the Upscaling choice below possible. The game's own is the default. Motion blur must stay enabled with FSR or DLSS; its exposure preset can still be changed.",
             {L"Game", L"FSR 3.1", L"DLSS (RTX)"}, {"@aa", "", ""}, 0);
  g_up = seg(L"Upscaling",
             L"More FPS for nearly the same picture: the game is drawn at a lower resolution and DLSS (or FSR) rebuilds it at your screen's resolution. DLAA (Native with FSR) draws every pixel: the best picture, the lowest FPS. Quality is the best balance; Balanced, Performance and Ultra Perf. give more FPS each. Needs DLSS or FSR 3.1 above.",
             {L"DLAA", L"Quality", L"Balanced", L"Performance", L"Ultra Perf."}, {"@up", "", "", "", ""}, 0);
  {
    Ctl c{L"Sharpness", L"How crisp the picture looks with DLSS or FSR. 0 adds no sharpening; 80 is recommended; very high values can make edges look rough.",
          Kind::Slider, {}, {"@sharpness"}, 80, 80};
    c.min = 0;
    c.max = 100;
    g_sharp = int(g_c.size());
    g_c.push_back(std::move(c));
  }
  g_aniso = seg(L"Anisotropic filtering",
                L"Keeps textures on roads and terrain sharp at a shallow angle. Automatic is 16x (4x on integrated graphics). Off uses only what the game asks for. It costs little on any graphics card.",
                {L"Automatic", L"Off", L"2x", L"4x", L"8x", L"16x"}, {"@aniso", "", "", "", "", ""}, 0);
  g_fgen = seg(L"Frame generation (experimental)",
               L"Adds a made-up frame between each two real ones (FSR 3.1 works on any graphics card; DLSS needs a GeForce RTX 40 series for 2x, RTX 50 series for 3x and 4x), so the picture looks two to four times as smooth. It adds a little delay and can smear fast motion. Best when the game already runs at 45 FPS or more; turn on the Unlocked frame rate as well.",
               {L"Off", L"FSR 3.1 (2x)", L"DLSS (2x)", L"DLSS (3x)", L"DLSS (4x)"}, {"@fgen", "", "", "", ""}, 0);
  g_fgen = -1;  // frame generation is not offered in this release
  const int skipgpu = toggle(L"Skip the hardware check",
                             L"The launcher checks that your processor and graphics card can run the game (Vulkan 1.3). If it refuses hardware that does work, turn this on to start anyway.",
                             "@skipgpu", false);
  const int autoplay = toggle(L"Start the game without this window",
                              L"Double-clicking DriveClub.exe goes straight to the game. When it cannot start, "
                              L"this window opens anyway with the reason.", "@autoplay", false);
  const int updatecheck = toggle(L"Check for new versions",
                                 L"When the launcher starts it asks the Releases page once whether a newer version exists "
                                 L"and offers to install it. Your saves, settings and game folders stay as they are.",
                                 "@updatecheck", true);
  const int overlays = toggle(L"Let overlay programs into the game",
                              L"Steam, Epic, Discord, OBS and tuning tools add themselves to the game's graphics. "
                              L"A part of the crashes players report comes from those additions, so the launcher keeps them "
                              L"out. Turn this on if you want the in-game overlay while playing. Players report random collision penalties and crashes with it on.", "@overlays", false);
  const int twothread = toggle(L"Two-thread rendering",
                               L"Records each frame's draw commands on a second thread, so the thread that runs the game's "
                               L"graphics commands waits less. Faster on most PCs, but it lost the graphics device in races on AMD cards and ran slower there, so it starts off on AMD cards and under Wine or Proton. Turn it off if you see flicker, broken graphics or a crash.",
                               "@twothread", !PrimaryGpuIsAmd());
  g_hdr = toggle(L"HDR",
                 L"For HDR monitors and TVs: brighter highlights (the sun, lights, reflections) and deeper contrast. Turn on \"Use HDR\" in Windows' display settings first.",
                 "@hdr", false);
  {
    Ctl c{L"HDR brightness", L"How bright white is: the HUD, menus and the scene overall. Raise it if the picture looks dull or dark, lower it if it is tiring. While playing: Page Up brighter, Page Down darker.",
          Kind::Slider, {}, {"@hdr_paper"}, 350, 350};
    c.min = 100;
    c.max = 600;
    c.unit = L" nits";
    g_hdr_paper = int(g_c.size());
    g_c.push_back(std::move(c));
  }
  {
    Ctl c{L"HDR peak brightness", L"The brightest your screen can show; the sun and lights reach it. Set it to your screen's HDR peak (for example 600 or 1000 nits).",
          Kind::Slider, {}, {"@hdr_peak"}, 1000, 1000};
    c.min = 400;
    c.max = 2000;
    c.unit = L" nits";
    g_hdr_peak = int(g_c.size());
    g_c.push_back(std::move(c));
  }
  {
    const struct { const wchar_t* label; const wchar_t* hint; int def, min, max, decimals; const wchar_t* unit; } kPicture[3] = {
        {L"Brightness", L"Lifts or lowers the whole picture.\n\x2022 Raise it if dark corners and night races are too hard to see.\nIn game: Page Up brighter, Page Down darker.",
         0, -50, 50, 0, L""},
        {L"Contrast", L"The difference between dark and bright.\n\x2022 Lower it if the picture looks harsh; raise it if it looks flat.\nIn game: F4 twice, then Page Up / Page Down.",
         100, 50, 150, 0, L"%"},
        {L"Gamma", L"The kind of screen the picture is made for.\n\x2022 2.40: the TVs the game was made for (the default).\n\x2022 2.20: a PC monitor. Brighter and paler in the shadows.\nIn game: F4 three times, then Page Up / Page Down.",
         240, 180, 280, 2, L""},
    };
    for (int k = 0; k < 3; ++k) {
      Ctl c{kPicture[k].label, kPicture[k].hint, Kind::Slider, {}, {"@picture"}, kPicture[k].def, kPicture[k].def};
      c.min = kPicture[k].min;
      c.max = kPicture[k].max;
      c.decimals = kPicture[k].decimals;
      c.unit = kPicture[k].unit;
      g_picture[k] = int(g_c.size());
      g_c.push_back(std::move(c));
    }
  }
  const int showfps = toggle(L"Show FPS counter", L"Draws the frame rate in the top right corner while playing.", "@fps", false);
  const int showpedals = toggle(L"Show pedal inputs",
                                L"Draws how far the clutch, brake and throttle are pressed, in the bottom left corner while "
                                L"playing, so you can see a pedal reach 100%. F2 turns it on and off in the game.",
                                "@pedals", false);
  const int full = toggle(L"Fullscreen", L"Plays borderless fullscreen. Turn off for a window.", "@full", true);
  const int unlock = toggle(L"Unlock DLC content",
                            L"Unlocks the DLC content: add-on cars, bikes, tours and livery decals. Cars you earn by playing still unlock through normal progression. Built from your own game files; the game folders are not modified.",
                            "@unlock", false);
  const int unlock_all = toggle(L"Unlock every car and bike",
                                L"Every car and bike is free from the start, with no driver or club level needed. It includes the add-on content, so Unlock DLC content is not needed with it. Cars you earn by winning an event stay as the game has them. Off by default.",
                                "@unlockall", false);
  g_profile_row = seg(L"Save profile",
                      L"Four separate careers on one install, for different players. Picking another profile swaps your saves in when you press Play or Save. "
                      L"The saves of the others are kept.",
                      {L"Profile 1", L"Profile 2", L"Profile 3", L"Profile 4"}, {"@profile", "@profile", "@profile", "@profile"}, 0);
  g_save_backup_row = int(g_c.size());
  g_c.push_back(Ctl{L"Back up saves", L"Copies this profile's saves to a dated backup. The launcher also makes one at every start and keeps the last ten.",
                    Kind::Button, {L"Back up now"}, {"@savebackup"}, 0, 0});
  g_save_restore_row = int(g_c.size());
  g_c.push_back(Ctl{L"Restore saves", L"Puts back this profile's newest backup. The current saves are backed up first.",
                    Kind::Button, {L"Restore the latest backup"}, {"@saverestore"}, 0, 0});
  // Graphics detail the PS4 version keeps low and the game's camera effects, from the game's own quality file
  // (graphics.cpp).
  g_gfx[kGfxCar] = toggle(L"Your car: full detail",
                          L"Draws your own car with its most detailed model during races; the PS4 version uses the next one down.",
                          "@gfx", false);
  g_gfx[kGfxOpp] = toggle(L"Other cars: full detail",
                          L"The other cars also get their most detailed models up close, and every car keeps its detailed parts a little further away.",
                          "@gfx", false);
  g_gfx[kGfxScenery] = seg(L"Scenery detail (less pop-in)",
                           L"Trees, rocks and roadside objects switch to their detailed models further away and small objects stay visible longer, so less pops in while you drive. "
                           L"High uses the developers' own pre-race camera values, Ultra photo mode's.",
                           {L"Game", L"High", L"Ultra"}, {"@gfx", "", ""}, 0);
  g_gfx[kGfxSsr] = toggle(L"High-quality reflections",
                          L"Uses photo mode's finer reflections on the road and the cars in races; the difference is hard to see.",
                          "@gfx", false);
  g_gfx[kGfxShadows] = seg(L"Shadow edges",
                           L"The game smooths the edges of shadows near the car with 32 samples and of every shadow further than 4 m with 8. "
                           L"High uses 16 samples, Ultra 32.",
                           {L"Game", L"High", L"Ultra"}, {"@gfx", "", ""}, 0);
  g_gfx[kGfxParticles] = toggle(L"More particles",
                                L"Keeps the distant spray, smoke and dust that races drop (replays keep them), and allows 1.5 times as many particles.",
                                "@gfx", false);
  g_gfx[kGfxDynres] = toggle(L"Dynamic resolution",
                             L"On (as on PS4), the game draws at a lower resolution for a moment when a frame takes too long.",
                             "@gfx", true);
  g_gfx[kGfxBlur] = toggle(L"Motion blur", L"The game's motion blur on the road and the scenery while you drive. Cannot be changed while FSR or DLSS anti-aliasing is on: they need the same shader.",
                           "@gfx", true);
  g_motion_blur = seg(L"Motion blur exposure",
      L"Slow: original 30 FPS exposure, best for a cinematic look. Fast: shorter 60 FPS exposure.",
      {L"Slow", L"Fast"}, {"@motion_blur", ""}, 0);
  g_gfx[kGfxDust] = toggle(L"Lens dust", L"Dirt on the camera lens when you face the sun.", "@gfx", true);
  g_gfx[kGfxHaze] = toggle(L"Heat haze", L"The shimmer over hot roads in the distance.", "@gfx", true);
  g_gfx[kGfxFxaa] = toggle(L"FXAA pass",
                           L"With the game's anti-aliasing, an FXAA pass softens the picture after it (photo mode turns it off). "
                           L"Off is a little sharper.",
                           "@gfx", true);
  auto picker = [](const wchar_t* label, const wchar_t* hint, const char* tag) {
    Ctl c{label, hint, Kind::Picker, {L""}, {tag}, 0, 0};
    g_c.push_back(std::move(c));
    return int(g_c.size()) - 1;
  };
  g_btrack = picker(L"Track", L"Where the benchmark drives. Click to choose a circuit.", "@bench_track");
  g_bcar = picker(L"Car", L"The car the game's AI drives. Click to choose a car.", "@bench_car");
  g_bmode = seg(L"Cars on track",
                L"Alone is a time trial and gives the steadiest numbers. Race adds seven AI cars, which loads the processor more.",
                {L"Alone", L"Race (8 cars)"}, {"@bench_mode", ""}, 0);
  g_btod = seg(L"Time of day", L"Morning 8:00, noon 12:00, evening 18:00, night 22:00.",
               {L"Morning", L"Noon", L"Evening", L"Night"}, {"@bench_tod", "", "", ""}, 1);
  g_bweather = seg(L"Weather", L"The sky and the clouds.", {L"Clear", L"Cloudy", L"Overcast", L"Stormy"},
                   {"@bench_weather", "", "", ""}, 0);
  g_brain = seg(L"Rain", L"Rain and wet roads make the scene heavier to draw. Wet road is a wet track without rain.",
                {L"Dry", L"Light", L"Heavy", L"Wet road"}, {"@bench_rain", "", "", ""}, 0);
  g_blen = seg(L"Length", L"How long the frame times are measured, after 5 seconds of warm-up. Loading takes about half a minute more.",
               {L"30 s", L"1 min", L"2 min", L"5 min"}, {"@bench_length", "", "", ""}, 1);
  g_brun = int(g_c.size());
  g_c.push_back(Ctl{L"Last result",
                    L"Starts the event with all the settings on this page. The game's AI drives and the game closes by itself when it is done. "
                    L"The 1% and 0.1% lows are the average frame rate of the slowest 1% and 0.1% of frames.",
                    Kind::Action, {}, {"@bench_run"}, 0, 0});
  g_drive_preset = seg(L"Driving preset",
                       L"Sets the four rows below in one click. Sim: manual gears, no assists, hardcore handling. Assisted: automatic gears and help with braking and steering. Game Defaults: the game's own Options menu decides.",
                       {L"Sim", L"Assisted", L"Game Defaults"}, {"@drive_preset", "", ""}, 2);
  g_drive[0].row = seg(L"Gearbox", L"In-game: whatever is set in the game's own Options menu. Automatic changes gear for you; Manual is for paddles or a shifter.",
                       {L"In-game", L"Automatic", L"Manual"}, {"@drive_gearbox", "", ""}, 0);
  g_drive[1].row = seg(L"Brake assist", L"In-game: whatever is set in the game's own Options menu. The assist brakes for you before corners; Off leaves the braking to you.",
                       {L"In-game", L"Off", L"Low", L"Medium", L"High"}, {"@drive_brake", "", "", "", ""}, 0);
  g_drive[2].row = seg(L"Hardcore handling", L"In-game: whatever is set in the game's own Options menu. On makes the cars handle more like real ones, with fewer hidden helpers. Recommended with a wheel.",
                       {L"In-game", L"Off", L"On"}, {"@drive_hardcore", "", ""}, 0);
  g_drive[3].row = seg(L"Wheel assist", L"In-game: whatever is set in the game's own Options menu. The game's steering help for wheels. Off is recommended with the Reworked Physics force feedback.",
                       {L"In-game", L"Off", L"On"}, {"@drive_wheel_assist", "", ""}, 0);
  {
    Ctl c{L"Field of view (F5 / F6 while driving)", L"How much of the road you see. 100% is the game's own view; higher shows more to the sides, lower zooms in. While playing: F5 narrower, F6 wider.",
          Kind::Slider, {}, {"@drive_fov"}, 100, 100};
    c.min = 50;
    c.max = 150;
    g_fov = int(g_c.size());
    g_c.push_back(std::move(c));
  }
  const int lock_horizon = seg(L"Lock to horizon",
                               L"Keeps the horizon level when the car leans in corners and dips under braking, as in Assetto Corsa.\n"
                               L"\x2022 Off: the view moves with the car's body, as on PS4.\n"
                               L"\x2022 Cockpit: the driver's view only.\n"
                               L"\x2022 All views: the bonnet, bumper and chase views too.\n"
                               L"\x2022 The driver's head still looks into corners; its sway is smoothed.\n"
                               L"Tip: helps with motion sickness, together with a lower Camera shake.",
                               {L"Off", L"Cockpit", L"All views"}, {"@lock_horizon", "", ""}, 0);
  {
    Ctl c{L"Camera shake",
          L"How much the camera shakes over bumps, kerbs and at speed.\n"
          L"\x2022 100%: as the game made it.\n"
          L"\x2022 Lower it if the shaking is tiring or makes you feel sick.\n"
          L"Tip: the game's own Options menu has a Camera shake setting that adds more shake on top. Set it to 0 there too for the calmest view.",
          Kind::Slider, {}, {"@camera_shake"}, 100, 100};
    c.min = 0;
    c.max = 100;
    g_camera_shake = int(g_c.size());
    g_c.push_back(std::move(c));
  }
  // Steering wheel: Wheel Setup, then the force feedback.
  g_wheel_setup = int(g_c.size());
  g_c.push_back(Ctl{L"Controls", L"Set up your wheel, pedals, handbrake and shifter. Click Map next to a control, then move it.",
                    Kind::Button, {L"Wheel Setup..."}, {"@wheelsetup"}, 0, 0});
  g_ffb_model = seg(L"Force feedback",
                    L"How the wheel's force is made. Reworked Physics comes from the car's tyres, like a racing sim, and is the one for wheels. Original Physics is the PS4 game's own feel, not recommended for wheels.",
                    {L"Reworked Physics", L"Original (not recommended)"}, {"@ffb:wheel_ffb_model", ""}, 1);
  g_ffb_preset = seg(L"Preset", L"Ready-made settings for your type of wheel. You can fine-tune the sliders below afterwards.",
                     {L"Direct drive", L"Gear or belt"}, {"@wheelpreset", ""}, -1);
  g_c[g_ffb_preset].physics_only = true;
  auto slider = [](const wchar_t* label, const wchar_t* hint, const char* key, int lo, int hi, int def) {
    Ctl c{label, hint, Kind::Slider, {}, {std::string("@ffb:") + key}, def, def};
    c.min = lo;
    c.max = hi;
    c.physics_only = true;
    g_c.push_back(std::move(c));
    return int(g_c.size()) - 1;
  };
  g_ffb_slider[0] = slider(L"Gain", L"How strong the steering force is overall.",
                           "wheel_ffb_physics_gain", 0, 300, 100);
  g_ffb_slider[1] = slider(L"Filter", L"Smooths the force. Higher feels softer, lower feels more detailed.",
                           "wheel_ffb_physics_filter", 0, 100, 0);
  g_ffb_slider[2] = slider(L"Minimum force", L"Helps weaker wheels feel small forces around the centre.",
                           "wheel_ffb_physics_min_force", 0, 30, 0);
  g_ffb_slider[3] = slider(L"Kerb effects", L"How much you feel kerbs and rumble strips.",
                           "wheel_ffb_physics_kerbs", 0, 200, 50);
  g_ffb_slider[4] = slider(L"Road effects", L"How much you feel bumps and the road surface.",
                           "wheel_ffb_physics_road", 0, 200, 50);
  g_ffb_slider[5] = slider(L"Slip effects", L"A light vibration when the tyres slide, spin or lock up.",
                           "wheel_ffb_physics_slip", 0, 200, 30);
  g_ffb_slider[6] = slider(L"Damper", L"Makes the wheel a little heavier to turn so it stays calm and does not wobble.",
                           "wheel_ffb_physics_damper", 0, 100, 10);
  g_ffb_softlock = seg(L"Soft lock", L"Stops the wheel where the car's own steering stops, so you cannot turn it further than the car can.",
                       {L"Game", L"Off", L"540\x00b0", L"720\x00b0", L"900\x00b0", L"1080\x00b0"},
                       {"@ffb:wheel_ffb_soft_lock", "", "", "", "", ""}, 0);
  g_ffb_wave = seg(L"Vibration wave", L"The feel of vibrations: Sine is smooth, Square is sharper.",
                   {L"Sine", L"Square"}, {"@ffb:wheel_ffb_vibration_wave", ""}, 1);
  const int ffb_invert = toggle(L"Invert forces", L"Turn on only if the wheel pulls away from the centre instead of towards it.", "@ffb:wheel_ffb_invert", false);
  // FFB gain + / -: two wheel buttons (NameFfbGainRows writes their text)
  for (int k = 0; k < 2; ++k) {
    g_ffb_gain_row[k] = int(g_c.size());
    Ctl c{k ? L"FFB gain \x2212" : L"FFB gain +",
          L"A button on your wheel that makes the force of the car you are driving 5% stronger (or weaker) during a race. Each car keeps its own. Click, then press the button on the wheel.",
          Kind::Button, {L""}, {k ? "@ffbgaindown" : "@ffbgainup"}, 0, 0};
    c.physics_only = true;
    g_c.push_back(std::move(c));
  }
  // Controller dead zones: two rows per device, as analog_deadzone lines in default.ini.
  for (int i = 0; i < kDeadCount; ++i) g_dead_row[i] = AddDeadRow(i);
  LoadDeadzones();
  g_xbox_names = DetectXboxNaming();   // the pad in use decides how the buttons are named
  // One row per XInput slot, always created and hidden while that slot has no pad, so a pad plugged in
  // later shows up without rebuilding the page. Double-clicking a pad's name reads that pad; the row is
  // red for the pad in use.
  g_device_rows.clear();
  for (int slot = 0; slot < 4; ++slot) {
    Ctl c{L"Device",
          L"Double-click a pad's name to read that pad for the binding buttons and the monitor.",
          Kind::Info, {L"Xbox pad  \x00b7  XInput slot " + std::to_wstring(slot)},
          {std::string("@dev:") + std::to_string(slot)}, 0, 0};
    c.hidden = true;
    g_device_rows.push_back(int(g_c.size()));
    g_c.push_back(std::move(c));
  }

  {
    Ctl c{L"Device", L"A pad SDL3 reports. Double-click a pad's name to read that pad.",
          Kind::Info, {L""}, {"@devsdl"}, 0, 0};
    c.hidden = true;
    g_dev_sdl_row = int(g_c.size());
    g_c.push_back(std::move(c));
  }

  EnsureKeyboardTemplate();   // a file with no keyboard bindings at all gets the game's racing layout
  SyncDevices(false);         // show what is plugged in now
  BuildBindingRows();
  {
    Ctl c{L"Input monitor",
          L"Movement of the sticks and the buttons that are held right now. Click the row to take a new reading.",
          Kind::Monitor, {MonitorText()}, {"@monitor"}, 0, 0};
    g_monitor_row = int(g_c.size());
    g_c.push_back(std::move(c));
  }
  NameFfbGainRows();
  // The cards flow into as many columns as the window width allows (see LayoutPass), on the page of their tab.
  g_cards.push_back({L"FRAME RATE", {g_preset, fps, g_fps_cap}, 0, kTabGraphics});
  if (g_fgen >= 0) g_cards.back().ctls.push_back(g_fgen);
  g_cards.push_back({L"RESOLUTION AND WINDOW", {outres, inres, wide, g_gfx[kGfxDynres], full}, 0, kTabGraphics});
  g_cards.push_back({L"ANTI-ALIASING", {g_aa, g_up, g_sharp, g_aniso}, 0, kTabGraphics});
  g_cards.push_back({L"HDR", {g_hdr, g_hdr_paper, g_hdr_peak}, 0, kTabGraphics});
  g_cards.push_back({L"PICTURE", {g_picture[0], g_picture[1], g_picture[2]}, 0, kTabGraphics,
                     L"For SDR. With HDR on, use HDR brightness.\nIn game: Page Up / Page Down change the brightness; F4 picks contrast or gamma."});
  g_cards.push_back({L"SCENE DETAIL", {g_gfx[kGfxCar], g_gfx[kGfxOpp], g_gfx[kGfxScenery],
                                      g_gfx[kGfxShadows], g_gfx[kGfxParticles]}, 0, kTabDetail});
  g_cards.push_back({L"REFLECTIONS", {g_gfx[kGfxSsr], g_mirror}, 0, kTabDetail});
  g_cards.push_back({L"CAMERA EFFECTS", {g_gfx[kGfxBlur], g_motion_blur, g_gfx[kGfxDust], g_gfx[kGfxHaze], g_gfx[kGfxFxaa]}, 0, kTabDetail});
  g_cards.push_back({L"PLAYER", {pname, g_lang}, 0, kTabGameplay});
  g_cards.push_back({L"ONLINE", {g_online_row, g_online_ip_row, g_online_host_row}, 0, kTabGameplay,
                     L"Private multiplayer with friends. Everyone needs the same version of the game."});
  g_cards.push_back({L"CONTENT", {unlock, unlock_all}, 0, kTabGameplay});
  g_cards.push_back({L"SAVES", {g_profile_row, g_save_backup_row, g_save_restore_row}, 0, kTabGameplay});
  g_cards.push_back({L"STARTUP AND OVERLAYS", {showfps, showpedals, overlays, autoplay, updatecheck}, 0, kTabGameplay});
  g_cards.push_back({L"VIEW AND KEYS", {g_fov, lock_horizon, g_camera_shake}, 0, kTabDriving,
                     L"F2: pedal inputs. F5 / F6: field of view. F7: mouse look. F9 / F10: anti-aliasing. F12: performance overlay. Page Up / Page Down: HDR brightness."});
  g_cards.back().sub = kSubAssists;
  g_cards.push_back({L"ASSISTS", {g_drive_preset, g_drive[0].row, g_drive[1].row, g_drive[2].row, g_drive[3].row}, 0, kTabDriving,
                     L"In-game leaves the setting to the game's Options menu. Other choices are applied when the game starts."});
  g_cards.back().sub = kSubAssists;
  g_cards.push_back({L"CAMERA", {camera_patch}, 0, kTabPatches,
                     L"Reduces camera and replay shake at high frame rates. Not needed at 30 FPS."});
  g_cards.push_back({L"DISPLAY COMPATIBILITY", {fov}, 0, kTabPatches});
  g_cards.push_back({L"GAMEPLAY PATCHES", {reset, barrier}, 0, kTabPatches});
  g_cards.push_back({L"PHOTO MODE", {pcol, pproc}, 0, kTabPatches});
  g_cards.push_back({L"EFFECT OVERRIDES", {blur}, 0, kTabPatches});
  g_cards.push_back({L"ADVANCED", {twothread, skipgpu}, 0, kTabGameplay});
  g_cards.push_back({L"BENCHMARK", {g_btrack, g_bcar, g_bmode, g_btod, g_bweather, g_brain, g_blen}, 0, kTabBench});
  g_cards.push_back({L"BENCHMARK RUN AND RESULT", {g_brun}, 0, kTabBench});
  // The details each (i) opens, after the row's hint.
  const struct { const wchar_t* label; const wchar_t* more; } kMore[] = {
      {L"Widescreen (render and output)", L"Leave Output resolution and Render resolution on Default when you use a widescreen size: the widescreen size sets both, and other sizes there crop the picture. Choosing a widescreen size puts them back on Default for you."},
      {L"Force feedback", L"Reworked Physics is recommended for every wheel. The wheel gets heavier in fast corners, goes light when the front tyres lose grip (the car runs wide), and turns itself the right way when the back of the car slides, like in Assetto Corsa. It learns each car as you drive. Original Physics gives the forces the PS4 game made for Logitech wheels in 2014: a spring that follows the car, without the tyres' grip. It is not recommended for wheels."},
      {L"Preset", L"Direct drive: wheels with the motor on the steering shaft (for example Fanatec DD, Moza, Simagic, Simucube). Gear or belt: most Logitech, Thrustmaster and Fanatec CSL or ClubSport belt wheels. A preset only moves the sliders below."},
      {L"Gain", L"Too weak and the car feels vague; too strong and the wheel hits its limit in fast corners, where it feels flat and you lose the detail. Start at 100 on a strong wheel; weaker wheels may want 150 to 200."},
      {L"Filter", L"Leave it at 0. If the wheel feels grainy or rattles, try 10 to 20."},
      {L"Minimum force", L"Gear and belt wheels often feel dead around the centre because they cannot make very small forces. If yours does, try 5 to 10. Keep it at 0 on a direct drive wheel."},
      {L"Kerb effects", L"Also used for wheelspin. 50 is a good start; turn it down if kerbs shake the wheel too much."},
      {L"Road effects", L"You feel dips, bumps and rough road through the wheel. Turn it down for a calmer wheel."},
      {L"Slip effects", L"A warning that you are past the grip limit. Many players keep it low or off, because the wheel already goes light when the tyres slide."},
      {L"Damper", L"Like real steering, it is heaviest when the car stands still. On a direct drive wheel, 10 to 30 stops the wheel wobbling when you let go. Gear and belt wheels usually want 0."},
      {L"Soft lock", L"Game uses the right angle for each car. Also set your wheel's own software to 900 degrees."},
      {L"Vibration wave", L"Sine suits direct drive wheels. Square feels stronger on gear and belt wheels."},
      {L"Invert forces", L"Only needed on a few wheels. If the wheel pushes away from the centre when you drive straight, turn this on."},
      {L"Field of view (F5 / F6 while driving)", L"A wider view (110 to 130%) helps in the cockpit view, especially on a big or wide screen; a narrower one makes the car feel faster. It changes every camera. Other than 100%, it replaces the widescreen field-of-view fix. In the game, F5 and F6 change it in steps of 5% and it is kept for next time."},
      {L"Driving preset", L"Sim is for wheel players who want the real thing: you change gear yourself and nothing helps you brake or steer. Assisted is the easiest way to start: the car changes gear, helps you brake before corners and helps the steering. Game Defaults uses whatever you set in the game's own Options menu (each row then shows In-game). You can change any row afterwards."},
      {L"Gearbox", L"Pick Manual if you change gear yourself with paddles or an H-pattern shifter; the shifter only works with Manual."},
      {L"Brake assist", L"Good for learning the tracks. Experienced players and wheel users usually turn it Off."},
      {L"Hardcore handling", L"The game's sim-like handling: the cars react more honestly to throttle, brakes and weight. Recommended with a wheel and the Reworked Physics force feedback."},
      {L"Wheel assist", L"Helps controller-style steering on a wheel. With a proper wheel and Reworked Physics force feedback, Off feels most natural."},
  };  for (auto& c : g_c) {
    for (const auto& m : kMore) {
      if (wcscmp(c.label, m.label) == 0) c.more = m.more;
    }
  }
  // Wheel: Wheel Setup for the controls, then the force feedback.
  // Three cards, so the page fits beside the game files card without scrolling: the controls, the steering force, the
  // effects (gain, filter, minimum force, damper are sliders 0, 1, 2 and 6; kerbs, road, slip 3, 4 and 5).
  g_cards.push_back({L"CONTROLS", {g_wheel_setup, ffb_invert}, 0, kTabDriving,
                     L"Wheel Setup shows every controller plugged in. Click Map next to a control (steering, pedals, handbrake, buttons, gears), then move it."});
  g_cards.back().sub = kSubWheel;
  g_cards.push_back({L"FORCE FEEDBACK", {g_ffb_model, g_ffb_preset, g_ffb_slider[0], g_ffb_slider[1], g_ffb_slider[2], g_ffb_slider[6], g_ffb_softlock,
                                         g_ffb_gain_row[0], g_ffb_gain_row[1]}, 0, kTabDriving});
  g_cards.back().sub = kSubWheel;
  // The page keeps the five cards the original design had: the buttons, the devices, the live
  // monitor, the dead zones and the d-pad/stick bindings.
  {
    std::vector<int> buttons, sticks;
    for (int i = 0; i < g_bind_n_buttons; ++i) buttons.push_back(g_bind_buttons[i]);
    for (int i = 0; i < g_bind_n_sticks; ++i) sticks.push_back(g_bind_sticks[i]);
    if (!buttons.empty()) {
      g_cards.push_back({L"BUTTONS", buttons, 0, kTabDriving,
                         L"One row per action: the left chip is the keyboard key, the right chip the pad button. Only the line you change is rewritten in data\\user\\input_config\\default.ini."});
      g_cards.back().sub = kSubInput;
    }
    {
      std::vector<int> dev = g_device_rows;
      if (g_dev_sdl_row >= 0) dev.push_back(g_dev_sdl_row);
      g_cards.push_back({L"DEVICES", dev, 0, kTabDriving,
                         L"Double-click a device name to switch the input device the launcher reads. A red name is the one in use."});
    }
    g_cards.back().sub = kSubInput;
    g_cards.push_back({L"INPUT MONITOR", {g_monitor_row}, 0, kTabDriving,
                       L"Live from the pad in use: a stick, trigger or button that never moves here is not reaching the launcher either."});
    g_cards.back().sub = kSubInput;
    std::vector<int> dead;
    for (int i = 0; i < kDeadCount; ++i) dead.push_back(g_dead_row[i]);
    g_cards.push_back({L"DEAD ZONES", dead, 0, kTabDriving,
                       L"How far a stick or trigger must move before the game sees it (inner) and where it counts as fully moved (outer)."});
    g_cards.back().sub = kSubInput;
    {
      Ctl c{L"L3 + R3 taps the touchpad right",
            L"Press both sticks down together and the game gets a tap on the right half of the touchpad, "
            L"which it uses for photo mode. Meant for pads that have no touchpad of their own.",
            Kind::Toggle, {}, {"@l3r3touch"}, 1, 1};
      g_l3r3_row = int(g_c.size());
      g_c.push_back(std::move(c));
    }
    if (g_l3r3_row >= 0) sticks.push_back(g_l3r3_row);
    if (!sticks.empty()) {
      g_cards.push_back({L"D-PAD AND STICKS", sticks, 0, kTabDriving,
                         L"Steering, the d-pad and the sticks. The right chip shows what the pad reports for that axis."});
      g_cards.back().sub = kSubInput;
    }
  }
  g_cards.push_back({L"EFFECTS", {g_ffb_slider[3], g_ffb_slider[4], g_ffb_slider[5], g_ffb_wave}, 0, kTabDriving});
  g_cards.back().sub = kSubWheel;
}

// The rows show the chosen track and car by name.
void ShowBenchChoices() {
  if (g_btrack >= 0) g_c[g_btrack].items[0] = std::wstring(kBenchTracks[g_bench_track].name) + L"  (" + kBenchTracks[g_bench_track].group + L")";
  if (g_bcar >= 0) g_c[g_bcar].items[0] = kBenchCars[g_bench_car].name;
}

int FindBench(const BenchChoice* list, int count, const std::string& id, int fallback) {
  for (int i = 0; i < count; ++i) {
    if (id == list[i].id) return i;
  }
  return fallback;
}

std::wstring BenchName(const BenchChoice* list, int count, const std::string& id) {
  const int i = FindBench(list, count, id, -1);
  return i >= 0 ? std::wstring(list[i].name) : Widen(id);
}

bool unlock_all_cars = false;  // "Unlock every car and bike" (unlock_all= in launcher.ini)
bool fps_counter = false, pedal_display = false, fast_render = false, full_screen = true, unlock_dlc = false;  // the settings that are not patches

// Output / render choices per preset: indices into the "Output resolution" and "Render resolution" rows,
// and the "Mirror and reflection updates" choice (slower presets update the mirrors less often).
const int kPresetOut[6] = {0, 1, 0, 2, 4, 4};     // Default 720p 1440p 1800p 4K 5K 8K
const int kPresetIn[6] = {0, 1, 1, 0, 2, 4};      // Default(1080p) 720p 1440p 1800p 4K 8K
const int kPresetMirror[6] = {0, 1, 0, 0, 0, 0};  // Live, Static (Handheld takes the speed)

// Temporary compatibility policy: verify the native correction without this
// workaround, then retire its auto-enable and UI entry. Do not stack a known
// corrected native path with the workaround or infer correctness from version.
bool FrameModeNeedsCameraPatch() {
  return g_fps>=0 && (g_c[g_fps].sel==1 || g_c[g_fps].sel==3 || g_c[g_fps].sel==4);
}
void ApplyFrameModeCameraDefault() {
  if(g_camera_patch_row>=0)g_c[g_camera_patch_row].sel=FrameModeNeedsCameraPatch()?1:0;
}
constexpr int kPs4Preset = 6;
std::vector<std::pair<int, int>> Ps4Profile() {
  std::vector<std::pair<int,int>> values = {{g_fps,0},{g_outres,0},{g_inres,0},{g_wide,0},
      {g_aa,0},{g_up,0},{g_aniso,1},{g_mirror,0},{g_hdr,0},{g_fgen,0},
      {g_motion_blur,0},{g_fov,100},{g_camera_patch_row,0}};
  for (int i=0;i<kGfxRows;++i) if(g_gfx[i]>=0) values.emplace_back(g_gfx[i],g_c[g_gfx[i]].def);
  for(size_t i=0;i<g_c.size();++i) {
    const auto& tag=g_c[i].patches[0];
    if(tag=="FOV fix for wide resolutions" || tag=="Disable motion blur and depth of field" ||
       tag=="Reset Disable" || tag=="Barrier Disable" || tag=="Disable Photomode Free Camera Collision" ||
       tag=="Disable Photomode Processing") values.emplace_back(int(i),0);
  }
  return values;
}
bool IsPs4Profile() {
  for(const auto& [id,value]:Ps4Profile()) if(id>=0 && g_c[id].sel!=value) return false;
  return true;
}

// The preset row has no patch of its own: it shows the preset whose sizes are selected, else Custom.
void DerivePreset() {
  if (g_preset < 0 || g_outres < 0 || g_inres < 0) return;
  g_c[g_preset].sel = 0;
  if (IsPs4Profile()) { g_c[g_preset].sel=kPs4Preset; return; }
  if (g_wide >= 0 && g_c[g_wide].sel != 0) return;
  for (int p = 1; p <= 5; ++p) {
    if (g_c[g_outres].sel == kPresetOut[p] && g_c[g_inres].sel == kPresetIn[p]) g_c[g_preset].sel = p;
  }
}

bool ReadOptimizationMask(uint64_t& mask) {
  std::ifstream in((g_data+L"\\user\\toggles.txt").c_str());
  if(!(in>>mask)) return false;
  in>>std::ws; return in.eof();
}
void LoadSettings() {
  g_two_thread = !PrimaryGpuIsAmd();  // the saved choice (draw_threads=) replaces it below
    for (const auto& line : ReadLines(IniPath())) {
    if (line.rfind("update=", 0) == 0) g_folder[0].path = Widen(line.substr(7));
    if (line.rfind("base=", 0) == 0) g_folder[1].path = Widen(line.substr(5));
    if (line.rfind("fps=", 0) == 0) fps_counter = line.substr(4) == "1";
    if (line.rfind("pedals=", 0) == 0) pedal_display = line.substr(7) == "1";
    if (line.rfind("fast=", 0) == 0) fast_render = line.substr(5) == "1";
    if (line.rfind("name=", 0) == 0) g_name = Widen(line.substr(5));
    if (line.rfind("online=", 0) == 0) online_saved = atoi(line.substr(7).c_str());
    if (line.rfind("online_host=", 0) == 0) g_online_host = Widen(line.substr(12));
    if (line.rfind("mirror=", 0) == 0) mirror_saved = atoi(line.substr(7).c_str());
    if (line.rfind("aniso=", 0) == 0) aniso_saved = atoi(line.substr(6).c_str());
    if (line.rfind("language=", 0) == 0) lang_saved = atoi(line.substr(9).c_str());
    if (line.rfind("up=", 0) == 0) up_saved = atoi(line.substr(3).c_str());
    if (line.rfind("sharpness=", 0) == 0) sharp_saved = atoi(line.substr(10).c_str());
    if (line.rfind("fps_cap=", 0) == 0) fps_cap_saved = atoi(line.substr(8).c_str());
    if (line.rfind("aa=", 0) == 0) aa_saved = atoi(line.substr(3).c_str());
    if (line.rfind("fgen=", 0) == 0) fgen_saved = atoi(line.substr(5).c_str());
    if (line.rfind("unlocked=", 0) == 0) unlocked_saved = atoi(line.substr(9).c_str());
    if (line.rfind("camera_patch=",0)==0) { g_camera_patch=line.substr(13)=="1";g_camera_patch_saved=true; }
    if (line.rfind("lock_horizon=", 0) == 0) g_lock_horizon = (std::clamp)(atoi(line.substr(13).c_str()), 0, 2);
    if (line.rfind("camera_shake=", 0) == 0) camera_shake_saved = atoi(line.substr(13).c_str());
    if (line.rfind("skip_gpu_check=", 0) == 0) g_skip_gpu_check = line.substr(15) == "1";
    if (line.rfind("overlays=", 0) == 0) g_allow_overlays = line.substr(9) == "1";
    if (line.rfind("draw_threads=", 0) == 0) g_two_thread = line.substr(13) == "1";
    if (line.rfind("hdr=", 0) == 0) hdr_saved = atoi(line.substr(4).c_str());
    if (line.rfind("hdr_paper=", 0) == 0) hdr_paper_saved = atoi(line.substr(10).c_str());
    if (line.rfind("hdr_peak=", 0) == 0) hdr_peak_saved = atoi(line.substr(9).c_str());
    for (const auto& d : g_drive) {
      const size_t n = std::strlen(d.key);
      if (d.row < 0 || line.compare(0, n, d.key) != 0) continue;
      const int v = atoi(line.substr(n).c_str());
      if (v >= 0 && v < int(g_c[d.row].items.size())) g_c[d.row].sel = v;
    }
    if (line.rfind("auto_play=", 0) == 0) g_auto_play = line.substr(10) == "1";
    if (line.rfind("check_updates=", 0) == 0) g_check_updates = line.substr(14) != "0";
    if (line.rfind("full=", 0) == 0) full_screen = line.substr(5) == "1";
    for (int k = 0; k < kGfxRows; ++k) {
      if (line.rfind(kGfxKey[k], 0) == 0) gfx_saved[k] = atoi(line.substr(strlen(kGfxKey[k])).c_str());
    }
    if (line.rfind("motion_blur=", 0) == 0) motion_blur_saved = line.substr(12) == "fast" ? 1 : 0;
    if (line.rfind("unlock_all=", 0) == 0) unlock_all_cars = line.substr(11) == "1";
    if (line.rfind("profile=", 0) == 0) profile_saved = atoi(line.substr(8).c_str());
    if (line.rfind("dlc_unlock=", 0) == 0) unlock_dlc = line.substr(11) == "1";  // the "unlock=" of earlier versions is not read: off for everyone
    if (line.rfind("bench_track=", 0) == 0) g_bench_track = FindBench(kBenchTracks, kBenchTrackCount, line.substr(12), 0);
    if (line.rfind("bench_car=", 0) == 0) g_bench_car = FindBench(kBenchCars, kBenchCarCount, line.substr(10), kBenchDefaultCar);
    if (line.rfind("read_slot=", 0) == 0) g_read_slot = atoi(line.substr(10).c_str());
    const struct { const char* key; int row; } bench_rows[] = {{"bench_mode=", g_bmode}, {"bench_tod=", g_btod},
                                                                {"bench_weather=", g_bweather}, {"bench_rain=", g_brain},
                                                                {"bench_length=", g_blen}};
    for (const auto& b : bench_rows) {
      const size_t n = std::strlen(b.key);
      if (b.row < 0 || line.compare(0, n, b.key) != 0) continue;
      const int v = atoi(line.substr(n).c_str());
      if (v >= 0 && v < int(g_c[b.row].items.size())) g_c[b.row].sel = v;
    }
  }
  uint64_t existing_mask=0;
  if(ReadOptimizationMask(existing_mask)) g_two_thread=(existing_mask & (1ull<<37))==0;

  g_dev_loaded = true;   // from here on a fallback pick is a real choice and may be saved
  SyncDevices(false);    // the rows were built before the saved choice was read
  if (g_l3r3_row >= 0) {
    if (!ReadL3R3Setting()) WriteL3R3Setting(true);   // on by default, written once
    g_c[size_t(g_l3r3_row)].sel = g_l3r3_touchpad ? 1 : 0;
    g_c[size_t(g_l3r3_row)].def = g_c[size_t(g_l3r3_row)].sel;
  }
  ShowBenchChoices();
  for (auto& c : g_c) if (c.patches[0] == "@name") c.items[0] = g_name;
  if (g_online_row >= 0 && online_saved >= 0 && online_saved < 3) g_c[g_online_row].sel = online_saved;
  if (g_profile_row >= 0 && profile_saved >= 0 && profile_saved < 4) g_c[g_profile_row].sel = profile_saved;
  if (g_online_host_row >= 0) g_c[g_online_host_row].items[0] = g_online_host.empty() ? L"(not set)" : g_online_host;
  CheckFolders();
  const auto lines = ReadLines(SelectionPath());
  const bool has_selection = !lines.empty() || Exists(SelectionPath());
  for (auto& c : g_c) {
    if (c.kind == Kind::Toggle && c.patches[0] == "@fps") {
      c.sel = fps_counter ? 1 : 0;
      continue;
    }
    if (c.kind == Kind::Toggle && c.patches[0] == "@pedals") {
      c.sel = pedal_display ? 1 : 0;
      continue;
    }
    if (c.kind == Kind::Toggle && c.patches[0] == "@fast") {
      c.sel = fast_render ? 1 : 0;
      continue;
    }
    if (c.kind == Kind::Toggle && c.patches[0] == "@overlays") {
      c.sel = g_allow_overlays ? 1 : 0;
      continue;
    }
    if (c.kind == Kind::Toggle && c.patches[0] == "@twothread") {
      c.sel = g_two_thread ? 1 : 0;
      continue;
    }
    if (c.kind == Kind::Toggle && c.patches[0] == "@autoplay") {
      c.sel = g_auto_play ? 1 : 0;
      continue;
    }
    if (c.kind == Kind::Toggle && c.patches[0] == "@updatecheck") {
      c.sel = g_check_updates ? 1 : 0;
      continue;
    }
    if (c.kind == Kind::Toggle && c.patches[0] == "@camera_patch") { c.sel=g_camera_patch?1:0; continue; }
    if (c.kind == Kind::Segmented && c.patches[0] == "@lock_horizon") {
      c.sel = g_lock_horizon;
      continue;
    }
    if (c.kind == Kind::Slider && c.patches[0] == "@camera_shake") {
      c.sel = camera_shake_saved >= 0 ? (std::clamp)(camera_shake_saved, 0, 100) : c.def;
      continue;
    }
    if (c.kind == Kind::Toggle && c.patches[0] == "@skipgpu") {
      c.sel = g_skip_gpu_check ? 1 : 0;
      continue;
    }
    if (c.kind == Kind::Toggle && c.patches[0] == "@full") {
      c.sel = full_screen ? 1 : 0;
      continue;
    }
    if (c.kind == Kind::Toggle && c.patches[0] == "@unlock") {
      c.sel = unlock_dlc ? 1 : 0;
      continue;
    }
    if (c.kind == Kind::Toggle && c.patches[0] == "@unlockall") {
      c.sel = unlock_all_cars ? 1 : 0;
      continue;
    }
    if (!has_selection || IsBenchRow(c) || IsWheelRow(c) || IsDriveRow(c) || IsDeadRow(c)) continue;  // defaults; the benchmark and driving rows are in launcher.ini, the wheel's in default.ini    c.sel = 0;
    for (size_t i = 0; i < c.patches.size(); ++i) {
      if (c.patches[i].empty()) continue;
      if (std::find(lines.begin(), lines.end(), c.patches[i]) != lines.end()) c.sel = c.kind == Kind::Toggle ? 1 : int(i);
    }
  }
  // After the loop above, which resets every row that has no patch of its own.
  if (g_mirror >= 0 && mirror_saved == 1) g_c[g_mirror].sel = mirror_saved;
  if (g_aniso >= 0 && aniso_saved >= 1 && aniso_saved <= 5) g_c[g_aniso].sel = aniso_saved;
  if (g_lang >= 0) {
    g_lang_id = lang_saved >= 0 && lang_saved <= 20 ? lang_saved : 1;
    g_c[g_lang].items[0] = kLangName[g_lang_id];
  }
  if (g_up >= 0 && up_saved >= 1 && up_saved <= 4) g_c[g_up].sel = up_saved;
  // The sliders' saved values, else their defaults (the patch list sets nothing for them).
  if (g_sharp >= 0) g_c[g_sharp].sel = sharp_saved >= 0 ? (std::clamp)(sharp_saved, 0, 100) : g_c[g_sharp].def;
  if (g_fps_cap >= 0) g_c[g_fps_cap].sel = fps_cap_saved > 0 ? (std::clamp)(fps_cap_saved, g_c[g_fps_cap].min, g_c[g_fps_cap].max) : g_c[g_fps_cap].def;
  if (g_hdr >= 0 && hdr_saved >= 0) g_c[g_hdr].sel = hdr_saved ? 1 : 0;
  {
    std::ifstream hdr_ini(HdrIniPath().c_str());
    std::string hdr_line;
    while (std::getline(hdr_ini, hdr_line))
      if (hdr_line.rfind("paper=", 0) == 0) hdr_paper_saved = atoi(hdr_line.substr(6).c_str());
  }
  if (g_hdr_paper >= 0) {
    g_c[g_hdr_paper].sel = hdr_paper_saved >= 0 ? (std::clamp)(hdr_paper_saved, 100, 600) : g_c[g_hdr_paper].def;
    g_c[g_hdr_paper].loaded = g_c[g_hdr_paper].sel;
  }
  if (g_hdr_peak >= 0)
    g_c[g_hdr_peak].sel = hdr_peak_saved >= 0 ? (std::clamp)(hdr_peak_saved, 400, 2000) : g_c[g_hdr_peak].def;
  {
    // The picture as last set here or with F4 in the game.
    for (int p : g_picture) if (p >= 0) g_c[p].sel = g_c[p].def;
    std::ifstream picture_ini(PictureIniPath().c_str());
    std::string picture_line;
    while (std::getline(picture_ini, picture_line))
      for (int k = 0; k < 3; ++k)
        if (g_picture[k] >= 0 && picture_line.rfind(kPictureKey[k], 0) == 0) {
          Ctl& c = g_c[g_picture[k]];
          c.sel = (std::clamp)(atoi(picture_line.substr(strlen(kPictureKey[k])).c_str()), c.min, c.max);
        }
    for (int p : g_picture) if (p >= 0) g_c[p].loaded = g_c[p].sel;
  }
  if (g_aa >= 0 && aa_saved >= 1 && aa_saved <= 2) g_c[g_aa].sel = aa_saved;
  if (g_fgen >= 0 && fgen_saved >= 1 && fgen_saved <= 4) g_c[g_fgen].sel = fgen_saved;
  for (int k = 0; k < kGfxRows; ++k) {
    if (g_gfx[k] < 0) continue;
    Ctl& c = g_c[g_gfx[k]];
    const int choices = c.kind == Kind::Toggle ? 2 : int(c.items.size());
    c.sel = gfx_saved[k] >= 0 && gfx_saved[k] < choices ? gfx_saved[k] : c.def;
  }
  if (g_motion_blur >= 0) g_c[g_motion_blur].sel = motion_blur_saved;
  // Unlocked is deltatime with the launcher's own cap and present mode
  if (g_fps >= 0 && unlocked_saved >= 1 && unlocked_saved <= 2 && g_c[g_fps].sel == 1) g_c[g_fps].sel = 2 + unlocked_saved;
  NameNativeUpscale();
  if(!g_camera_patch_saved)ApplyFrameModeCameraDefault();
  LoadWheel();
  LoadFfbGainButtons();
  DeriveDrivePreset();
  if (g_fov >= 0) {
    for (const auto& line : ReadLines(CameraIniPath())) {
      if (line.rfind("fov=", 0) == 0) g_c[g_fov].sel = (std::clamp)(atoi(line.substr(4).c_str()), g_c[g_fov].min, g_c[g_fov].max);
    }
  }
  DerivePreset();
}

// Resolutions above 1080p need more memory: the game's own "Memory for up to ..." patch plus a larger
// direct memory budget. 0 = default, 1 = up to 1440p, 2 = up to 4K.
bool full_screen_now() {
  for (const auto& c : g_c) {
    if (c.kind == Kind::Toggle && c.patches[0] == "@full") return c.sel != 0;
  }
  return true;
}

// Upscaling: with FSR 3.1 or DLSS chosen, the runtime sets the render size from the output size (src/probe.c).
bool UpscaleActive() {
  return g_up >= 0 && g_aa >= 0 && g_c[g_up].sel > 0 && g_c[g_aa].sel > 0;
}

// Upscaling's first choice draws every pixel: DLAA with DLSS, "Native" with FSR (or the game's own).
void NameNativeUpscale() {
  if (g_up >= 0 && g_aa >= 0) g_c[g_up].items[0] = g_c[g_aa].sel == 2 ? L"DLAA" : L"Native";
}

// The output size counts too: 4K output with the Default render size crashed at boot on the default
// memory budget, since the presented images are output-sized.
int OutputMemoryTier() {
  if (g_outres < 0) return 0;
  const int o = g_c[g_outres].sel;  // output: Default 720p 1440p 1800p 4K 5K 8K
  return o >= 5 ? 3 : o >= 3 ? 2 : o == 2 ? 1 : 0;
}

int RenderMemoryTier() {
  if (g_inres < 0 || g_wide < 0) return 0;
  const int r = g_c[g_inres].sel, w = g_c[g_wide].sel;  // render: Default 720p 1440p 1800p 4K; wide: Off + sizes
  if (r >= 5) return 3;
  if (w == 6) return 3;  // 5120x2160 is 11 megapixels
  if (w > 0 || r >= 3) return 2;
  return r == 2 ? 1 : 0;
}

int MemoryTier() { return (std::max)(RenderMemoryTier(), OutputMemoryTier()); }

// Output / render choices per preset: indices into the "Output resolution" and "Render resolution" rows.
void ApplyPreset(int preset) {
  if(preset==kPs4Preset) {
    for(const auto& [id,value]:Ps4Profile()) if(id>=0) g_c[id].sel=value;
    NameNativeUpscale(); return;
  }
  if (preset <= 0 || preset > 5 || g_outres < 0 || g_inres < 0) return;
  g_c[g_outres].sel = kPresetOut[preset];
  g_c[g_inres].sel = kPresetIn[preset];
  if (g_wide >= 0) g_c[g_wide].sel = 0;
  if (g_mirror >= 0) g_c[g_mirror].sel = kPresetMirror[preset];
}

bool SaveTextAtomically(const std::wstring& path,const std::string& text) {
  const auto temp=path+L".launcher-"+std::to_wstring(GetCurrentProcessId())+L".tmp";
  { std::ofstream out(temp.c_str(),std::ios::binary|std::ios::trunc);
    if(out) { out<<text; out.flush(); }
    if(!out) { DeleteFileW(temp.c_str()); g_settings_error=L"Could not write settings; the previous file was retained."; return false; }
  }
  if(!MoveFileExW(temp.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)) {
    DeleteFileW(temp.c_str());g_settings_error=L"Could not replace settings; the previous file was retained.";return false;
  }
  return true;
}
std::string PreserveUnmanagedIni(const std::vector<std::string>& old,const std::string& managed,bool launcher=false) {
  std::vector<std::string> keys; std::istringstream values(managed);std::string line;
  while(std::getline(values,line)) { const auto at=line.find('=');if(at!=std::string::npos)keys.push_back(line.substr(0,at)); }
  std::string result=managed;
  for(const auto& previous:old) {
    const auto at=previous.find('=');
    if(at!=std::string::npos && std::find(keys.begin(),keys.end(),previous.substr(0,at))!=keys.end()) continue;
    if(launcher && IsPendingWheelSetting(previous)) continue;
    result+=previous+"\r\n";
  }
  return result;
}
bool SaveIniValue(const std::wstring& path,const std::string& key,const std::string& value) {
  return SaveTextAtomically(path,PreserveUnmanagedIni(ReadLines(path),key+"="+value+"\r\n"));
}
bool SaveSettings() {
  MakeDirs(g_data + L"\\user");
  g_settings_error.clear();
  uint64_t saved_mask=2199023792128ull;
  const auto toggles=g_data+L"\\user\\toggles.txt";
  if(Exists(toggles) && !ReadOptimizationMask(saved_mask)) {
    g_settings_error=L"The existing optimization mask is invalid; settings were not changed.";return false;
  }
  const auto old_ini=ReadLines(IniPath());
  const auto old_patches=ReadLines(SelectionPath());
  {
    std::ostringstream ini;
    bool fps_on = false, pedals_on = false, fast_on = fast_render, full_on = true, unlock_on = false, unlock_all_on = false;
    for (const auto& c : g_c) {
      if (c.kind == Kind::Segmented && c.patches[0] == "@lock_horizon") g_lock_horizon = c.sel;
      if (c.kind == Kind::Toggle && c.patches[0] == "@fps") fps_on = c.sel != 0;
      if (c.kind == Kind::Toggle && c.patches[0] == "@pedals") pedals_on = c.sel != 0;
      if (c.kind == Kind::Toggle && c.patches[0] == "@fast") fast_on = c.sel != 0;
      if (c.kind == Kind::Toggle && c.patches[0] == "@full") full_on = c.sel != 0;
      if (c.kind == Kind::Toggle && c.patches[0] == "@unlock") unlock_on = c.sel != 0;
      if (c.kind == Kind::Toggle && c.patches[0] == "@unlockall") unlock_all_on = c.sel != 0;
      if (c.kind == Kind::Toggle && c.patches[0] == "@overlays") g_allow_overlays = c.sel != 0;
      if (c.kind == Kind::Toggle && c.patches[0] == "@twothread") g_two_thread = c.sel != 0;
      if (c.kind == Kind::Toggle && c.patches[0] == "@autoplay") g_auto_play = c.sel != 0;
      if (c.kind == Kind::Toggle && c.patches[0] == "@updatecheck") g_check_updates = c.sel != 0;
    }
    ini << "update=" << Narrow(g_folder[0].path) << "\r\nbase=" << Narrow(g_folder[1].path) << "\r\nfps=" << (fps_on ? 1 : 0)
        << "\r\nfast=" << (fast_on ? 1 : 0) << "\r\nunlocked=" << ((g_fps >= 0 && g_c[g_fps].sel >= 3) ? g_c[g_fps].sel - 2 : 0) << "\r\nskip_gpu_check=" << (g_skip_gpu_check ? 1 : 0) << "\r\nmirror=" << (g_mirror >= 0 ? g_c[g_mirror].sel : 0) << "\r\nlanguage=" << g_lang_id << "\r\naniso=" << (g_aniso >= 0 ? g_c[g_aniso].sel : 0) << "\r\nup=" << (g_up >= 0 ? g_c[g_up].sel : 0) << "\r\nsharpness=" << (g_sharp >= 0 ? g_c[g_sharp].sel : 80) << "\r\nfps_cap=" << (g_fps_cap >= 0 && g_c[g_fps_cap].sel < g_c[g_fps_cap].max ? g_c[g_fps_cap].sel : 0) <<"\r\nhdr=" << (g_hdr >= 0 ? g_c[g_hdr].sel : 0) << "\r\nhdr_paper=" << (g_hdr_paper >= 0 ? g_c[g_hdr_paper].sel : 350) << "\r\nhdr_peak=" << (g_hdr_peak >= 0 ? g_c[g_hdr_peak].sel : 1000) << "\r\naa=" << (g_aa >= 0 ? g_c[g_aa].sel : 0) << "\r\nfgen=" << (g_fgen >= 0 ? g_c[g_fgen].sel : fgen_saved) <<"\r\nname=" << Narrow(g_name) << "\r\nprofile=" << (g_profile_row >= 0 ? g_c[g_profile_row].sel : 0) << "\r\nonline=" << (g_online_row >= 0 ? g_c[g_online_row].sel : 0) << "\r\nonline_host=" << Narrow(g_online_host) <<"\r\nfull=" << (full_on ? 1 : 0) << "\r\nunlock_all=" << (unlock_all_on ? 1 : 0) << "\r\ndlc_unlock=" << (unlock_on ? 1 : 0) << "\r\noverlays=" << (g_allow_overlays ? 1 : 0) << "\r\nauto_play=" << (g_auto_play ? 1 : 0) << "\r\ncheck_updates=" << (g_check_updates ? 1 : 0) << "\r\ndraw_threads=" << (g_two_thread ? 1 : 0) << "\r\n";
    for (int k = 0; k < kGfxRows; ++k) {
      if (g_gfx[k] >= 0) ini << kGfxKey[k] << g_c[g_gfx[k]].sel << "\r\n";
    }
    ini << "pedals=" << (pedals_on ? 1 : 0) << "\r\n";
    ini << "camera_patch=" << (g_c[g_camera_patch_row].sel ? 1 : 0) << "\r\n";
    ini << "lock_horizon=" << g_lock_horizon << "\r\n";
    ini << "camera_shake=" << (g_camera_shake >= 0 ? g_c[g_camera_shake].sel : 100) << "\r\n";
    ini << "motion_blur=" << (g_c[g_motion_blur].sel == 1 ? "fast" : "slow") << "\r\n";
    ini << "bench_track=" << kBenchTracks[g_bench_track].id << "\r\nbench_car=" << kBenchCars[g_bench_car].id
        << "\r\nbench_mode=" << g_c[g_bmode].sel << "\r\nbench_tod=" << g_c[g_btod].sel << "\r\nbench_weather=" << g_c[g_bweather].sel
        << "\r\nbench_rain=" << g_c[g_brain].sel << "\r\nbench_length=" << g_c[g_blen].sel << "\r\n"
        << "read_slot=" << g_read_slot << "\r\n";
    for (const auto& d : g_drive) if (d.row >= 0) ini << d.key << g_c[d.row].sel << "\r\n";
    // Only when moved here: the game may have saved a newer one since this window read it.
    if (g_hdr_paper >= 0 && g_c[g_hdr_paper].sel != g_c[g_hdr_paper].loaded) {
      if(!SaveIniValue(HdrIniPath(),"paper",std::to_string(g_c[g_hdr_paper].sel))) return false;
      g_c[g_hdr_paper].loaded = g_c[g_hdr_paper].sel;
    }
    // The picture, only when moved here (the game may have saved a newer one with F4 since this window read it).
    bool picture_moved = false;
    for (int p : g_picture) picture_moved |= p >= 0 && g_c[p].sel != g_c[p].loaded;
    if (picture_moved) {
      std::ofstream picture_ini(PictureIniPath().c_str(), std::ios::binary | std::ios::trunc);
      for (int k = 0; k < 3; ++k)
        if (g_picture[k] >= 0) {
          picture_ini << kPictureKey[k] << g_c[g_picture[k]].sel << "\r\n";
          g_c[g_picture[k]].loaded = g_c[g_picture[k]].sel;
        }
    }
    if (g_fov >= 0 && (g_c[g_fov].sel != 100 || Exists(CameraIniPath()))) {
      if(!SaveIniValue(CameraIniPath(),"fov",std::to_string(g_c[g_fov].sel))) return false;
    }
    ini << SaveWheel();  // into default.ini; before the game has made it, kept here
    if(!g_settings_error.empty()) return false;
    if(!SaveTextAtomically(IniPath(),PreserveUnmanagedIni(old_ini,ini.str(),true))) return false;
    // This control owns only bit 37. Keep every other saved optimization bit.
    const uint64_t mask=g_two_thread ? (saved_mask & ~(1ull<<37)) : (saved_mask | (1ull<<37));
    if(!SaveTextAtomically(toggles,std::to_string(mask))) return false;
    if(!SaveIniValue(g_data+L"\\user\\driveclub.ini","show_fps",fps_on ? "1" : "0")) return false;
    if(!SaveIniValue(g_data+L"\\user\\driveclub.ini","show_pedals",pedals_on ? "1" : "0")) return false;
  }
  std::ostringstream out;
  for (const auto& c : g_c) {
    if (IsWheelRow(c) || c.kind == Kind::Slider || c.kind == Kind::Button ||
        c.kind == Kind::Binding || c.kind == Kind::Info || c.kind == Kind::Monitor) continue;  // no patches
    if (g_blur_patch >= 0 && &c == &g_c[g_blur_patch] && BlurLocked()) continue;
    if (c.kind == Kind::Toggle) {
      if (c.sel && c.patches[0][0] != '@') out << c.patches[0] << "\n";
    } else if (c.sel > 0 && c.patches[c.sel] == "@unlocked") {
      out << "60 FPS with deltatime\n";  // the same timing patch; only the frame cap differs
    } else if (c.sel > 0 && !c.patches[c.sel].empty() && c.patches[c.sel][0] != '@') {
      out << c.patches[c.sel] << "\n";
    }
  }
  if (MemoryTier() == 1) out << "Memory for up to 1440p\n";
  if (MemoryTier() == 2) out << "Memory for up to 4K\n";
  if (MemoryTier() == 3) out << "Memory for up to 8K\n";
  for(const auto& previous:old_patches) {
    bool managed=previous=="Memory for up to 1440p" || previous=="Memory for up to 4K" || previous=="Memory for up to 8K";
    for(const auto& c:g_c) for(const auto& patch:c.patches) if(!patch.empty() && patch[0]!='@' && patch==previous) managed=true;
    if(!managed) out<<previous<<"\n";
  }
    FlushDeadzones();
  if(!SaveTextAtomically(SelectionPath(),out.str())) return false;
  g_changed = false;return true;
}

void ResetDefaults() {
  // On Controller and keyboard the reset also puts every binding back on the built-in template.
  if (g_tab == kTabDriving && g_subtab == kSubInput && WriteFullTemplate()) {
    RefreshBindingChips();
    Relayout(g_hwnd);
  }
  for (auto& c : g_c) {
    if (IsWheelRow(c)) continue;  // the player's wheel tuning stays (its presets are in its card)
    c.sel = c.def;
    if (c.kind == Kind::Toggle && c.patches[0] == "@skipgpu") g_skip_gpu_check = c.sel != 0;
  }
  g_bench_track = 0;
  g_bench_car = kBenchDefaultCar;
  ShowBenchChoices();
  g_changed = true;
}

// ------------------------------------------------------------------------------------------
// layout
// ------------------------------------------------------------------------------------------
// full: as if every row were shown (where the cards are placed, so hiding rows does not move them).
// The input monitor keeps its proportions: taller when the card is wider (0.3.0's MonitorHeight).
float g_card_inner_w = 320.f;   // the width a card's rows get, set by LayoutPass
float MonitorScale() { return (std::clamp)(g_card_inner_w / 240.f, 0.85f, 2.2f); }
float MonitorHeight() { return 200.f * MonitorScale() + 18.f; }
// An Info row carries its own text (the device list is a few lines), so the row grows with the lines —
// including the lines the text wraps into at the card's width.
float InfoHeight(const Ctl& c) {
  int lines = 0;
  const float per_line = (std::max)(20.f, (g_card_inner_w - 16.f) / 7.2f);   // characters that fit
  if (c.items.empty()) {
    lines = 1;
  } else {
    size_t run = 0;
    for (size_t i = 0; i <= c.items[0].size(); ++i) {
      if (i == c.items[0].size() || c.items[0][i] == L'\n') {
        lines += int((float(run) + per_line - 1.f) / per_line) > 0 ? int((float(run) + per_line - 1.f) / per_line) : 1;
        run = 0;
      } else {
        ++run;
      }
    }
    if (lines == 0) lines = 1;
  }
  return (std::max)(54.f, 24.f + 20.f * float(lines));
}
float RowHeight(const Ctl& c, bool full = false) {
  if (!full && RowHidden(c)) return 0.f;
  return (c.kind == Kind::Toggle ? 46.f : c.kind == Kind::Action ? 224.f : c.kind == Kind::Slider ? 54.f
              : c.kind == Kind::Button ? 58.f : c.kind == Kind::Info ? InfoHeight(c)
              : c.kind == Kind::Monitor ? MonitorHeight() : 70.f) +
         g_row_extra;
}
float FolderRowHeight() { return 74.f; }

int g_cols = 2;  // columns of cards: more when the window is wide
RectF g_folders_rect;          // the game folders card
bool g_folders_in_flow = false;  // the folders card sits in a column (3+ columns) instead of across the top

void LayoutPass(float bottom_extra) {
  g_cols = (std::clamp)(int((g_win_w - 2 * kMargin + kGap) / (380.f + kGap)), 2, 4);
  const float colw = (g_win_w - 2 * kMargin - float(g_cols - 1) * kGap) / float(g_cols);
  g_card_inner_w = colw - 2 * kPad;
  // Game folders: a full-width card on top with 2 columns. With 3 or 4 the benchmark card alone sets the height
  // and the other columns have room, so the folders card goes into one of them instead of taking a full-width strip.
  g_folders_in_flow = g_cols >= 3;
  const float folders_h = kTitleH + 2 * FolderRowHeight() + kPad * 0.4f + 8.f;
  // On the Graphics tab only (the first a player sees); elsewhere it is off the page.
  const bool folders_shown = g_tab == kTabGraphics;
  g_folders_rect = RectF(-1e6f, -1e6f, 0.f, 0.f);
  const float content_top = kHeaderH + kTabH + (HasSubTabs() ? kSubTabH : 0.f);
  {
    // a full-width bar of equal halves under the main tabs, so the inner tabs cannot be missed
    const float bar_w = g_win_w - 2 * kMargin, sw = bar_w / float(kSubCount);
    for (int s = 0; s < kSubCount; ++s) {
      g_sub_rect[s] = HasSubTabs() ? RectF(kMargin + sw * float(s), kHeaderH + kTabH + 4.f, sw, 46.f) : RectF(-1e6f, -1e6f, 0.f, 0.f);
    }
  }
  // The tab row: as many pills as tabs, each as wide as its label needs.
  {
    float tx = kMargin;
    for (int t = 0; t < kTabCount; ++t) {
      const float tw = 30.f + 8.2f * float(wcslen(kTabName[t]));
      g_tab_rect[t] = RectF(tx, kHeaderH + 6.f, tw, 32.f);
      tx += tw + 8.f;
    }
  }
  float y[4];
  for (float& v : y) v = content_top + (g_folders_in_flow || !folders_shown ? 0.f : folders_h + kGap);
  // Card heights; the tallest cards are placed first, each into the shortest column. Only the cards of the
  // current tab are placed; the others move off the page, so they neither draw nor catch the mouse.
  std::vector<float> heights(g_cards.size() + 1, 0.f), place(g_cards.size() + 1, 0.f);  // as shown; with every row
  std::vector<size_t> order;
  for (size_t k = 0; k < g_cards.size(); ++k) {
    if (!OnPage(g_cards[k])) {
      g_cards[k].rect = RectF(-1e6f, -1e6f, 0.f, 0.f);
      for (int i : g_cards[k].ctls) g_c[i].row = g_c[i].ctl = RectF(-1e6f, -1e6f, 0.f, 0.f);
      continue;
    }
    float h = kTitleH + kPad * 0.4f, hf = h;
    for (int i : g_cards[k].ctls) {
      h += RowHeight(g_c[i]);
      hf += RowHeight(g_c[i], true);
    }
    h += kPad * 0.6f;
    hf += kPad * 0.6f;
    if (g_cards[k].note) {
      // The note wraps to as many lines as the column width needs (about 6.3 units per character of the hint font).
      const float per_line = (std::max)(1.f, std::floor((colw - 2 * kPad) / 6.3f));
      const float lines = (std::max)(1.f, std::ceil(float(wcslen(g_cards[k].note)) / per_line));
      g_cards[k].note_h = lines * 17.f + 4.f;
      h += g_cards[k].note_h + 10.f;
      hf += g_cards[k].note_h + 10.f;
    }
    heights[k] = h;
    place[k] = hf;
    order.push_back(k);
  }
  // The folders card, when in a column, takes part as index g_cards.size() and goes on top of its column.
  const size_t folders_k = g_cards.size();
  if (g_folders_in_flow && folders_shown) {
    heights[folders_k] = place[folders_k] = folders_h;
    order.push_back(folders_k);
  }
  std::sort(order.begin(), order.end(), [&](size_t a, size_t b) { return place[a] > place[b]; });
  // Every split of the page's cards into columns is tried (at most 4^4) and the one with the shortest tallest
  // column is kept, so the page needs the least height. The tallest card stays in the first column.
  const size_t n = order.size();
  size_t splits = 1;
  for (size_t i = 1; i < n; ++i) splits *= size_t(g_cols);
  std::vector<int> col_of(heights.size(), 0), best(heights.size(), 0);
  float best_h = 1e30f;
  for (size_t m = 0; m < splits; ++m) {
    float fill[4] = {0, 0, 0, 0};
    size_t r = m;
    for (size_t i = 0; i < n; ++i) {
      const int c = i == 0 ? 0 : int(r % size_t(g_cols));
      if (i) r /= size_t(g_cols);
      col_of[order[i]] = c;
      fill[c] += place[order[i]] + kGap;
    }
    const float tallest = *std::max_element(fill, fill + g_cols);
    if (tallest < best_h - 1.f) {
      best_h = tallest;
      best = col_of;
    }
  }
  int folders_col = 0;
  for (size_t k : order) (k == folders_k ? folders_col : g_cards[k].column) = best[k];
  // Top to bottom in each column: the tallest card (the benchmark: top left), the folders card, then the others in
  // their usual order.
  std::vector<size_t> seq{order[0]};
  if (g_folders_in_flow && folders_shown && order[0] != folders_k) seq.push_back(folders_k);
  for (size_t k = 0; k < g_cards.size(); ++k) {
    if (OnPage(g_cards[k]) && k != order[0]) seq.push_back(k);
  }
  if (!g_folders_in_flow && folders_shown) g_folders_rect = RectF(kMargin, content_top, g_win_w - 2 * kMargin, folders_h);
  int last[4] = {-1, -1, -1, -1};  // the lowest card in each column: it is stretched to the common bottom
  for (size_t k : seq) {
    if (k == folders_k) {
      g_folders_rect = RectF(kMargin + folders_col * (colw + kGap), y[folders_col], colw, folders_h);
      y[folders_col] += folders_h + kGap;
      last[folders_col] = -1;
      continue;
    }
    auto& card = g_cards[k];
    last[card.column] = int(k);
    const float h = heights[k];
    card.rect = RectF(kMargin + card.column * (colw + kGap), y[card.column], colw, h);
    float cy = y[card.column] + kTitleH + kPad * 0.4f;
    for (int i : card.ctls) {
      Ctl& c = g_c[i];
      const float rh = RowHeight(c);
      c.row = RectF(card.rect.X + kPad, cy, colw - 2 * kPad, rh);
      c.ctl = c.kind == Kind::Toggle   ? RectF(c.row.X + c.row.Width - 46.f, cy + 10.f, 46.f, 26.f)
              : c.kind == Kind::Action ? RectF(c.row.X, cy + 8.f, c.row.Width, 44.f)
              : c.kind == Kind::Slider ? RectF(c.row.X, cy + 26.f, c.row.Width, 22.f)
              : c.kind == Kind::Button ? RectF(c.row.X, cy + 8.f, c.row.Width, 42.f)
              : c.kind == Kind::Binding ? RectF(c.row.X, cy + 30.f, c.row.Width, 32.f)
              : c.kind == Kind::Monitor ? RectF(c.row.X, cy + 8.f, c.row.Width, rh - 16.f)
              : c.kind == Kind::Info    ? RectF(c.row.X, cy + 6.f, c.row.Width, rh - 10.f)
                                       : RectF(c.row.X, cy + 26.f, c.row.Width, 34.f);
      cy += rh;
    }
    y[card.column] += h + kGap;
  }
  float fy = g_folders_rect.Y + kTitleH;
  for (auto& f : g_folder) {
    f.row = RectF(g_folders_rect.X + kPad, fy, g_folders_rect.Width - 2 * kPad, FolderRowHeight());
    // In a column the Browse button moves up beside the label, so the path gets the whole width.
    f.browse = g_folders_in_flow ? RectF(f.row.X + f.row.Width - 104.f, fy, 104.f, 28.f)
                                 : RectF(f.row.X + f.row.Width - 104.f, fy + 24.f, 104.f, 36.f);
    fy += FolderRowHeight();
  }
  float bottom_y = y[0];
  for (int col = 1; col < g_cols; ++col) bottom_y = (std::max)(bottom_y, y[col]);
  const float bottom = bottom_y - kGap + bottom_extra;
  for (int col = 0; col < g_cols; ++col) {
    if (last[col] >= 0) g_cards[size_t(last[col])].rect.Height += bottom - (y[col] - kGap);
  }
  g_content_h = bottom + kGap;
  const float bar_y = g_content_h + 2.f;
  const float bw[3] = {170.f, 110.f, 130.f};
  float x = g_win_w - kMargin;
  for (int i = 2; i >= 0; --i) {
    x -= bw[i];
    g_btn[i] = RectF(x, bar_y + 14.f, bw[i], 44.f);
    x -= 12.f;
  }
  g_update_btn = RectF(x - 190.f, bar_y + 14.f, 190.f, 44.f);
}

// Lays the window out for its current size. Wider: the columns widen. Taller: the rows spread out a little and
// the cards take the rest.
void Layout() {
  g_row_extra = 0;
  LayoutPass(0.f);
  g_natural_h = g_content_h + kFooterH;
  const float extra = g_win_h - g_natural_h;
  if (extra > 1.f) {
    int rows = 1;
    for (int col = 0; col < g_cols; ++col) {
      int n = 0;
      for (const auto& card : g_cards) if (card.column == col && OnPage(card)) n += int(card.ctls.size());
      rows = (std::max)(rows, n);
    }
    g_row_extra = (std::min)(extra / float(rows), 14.f);
    LayoutPass(0.f);
    const float left = g_win_h - (g_content_h + kFooterH);
    if (left > 1.f) LayoutPass(left);
  }
}

float TotalHeight() { return g_content_h + kFooterH; }

// A window shorter than the content scrolls it (mouse wheel); the footer stays at the bottom.
float MaxScroll() { return g_client_h > 0 ? (std::max)(0.f, TotalHeight() * g_scale - g_client_h) : 0.f; }
float ContentOy() { return -g_scroll; }
float FooterOy() { return g_client_h > 0 ? (std::min)(0.f, g_client_h - TotalHeight() * g_scale) : 0.f; }
float FooterTop() { return FooterOy() + g_content_h * g_scale; }

// ------------------------------------------------------------------------------------------
// painting
// ------------------------------------------------------------------------------------------
GraphicsPath* Round(const RectF& r, float rad) {
  auto* p = new GraphicsPath();
  const float d = rad * 2;
  p->AddArc(r.X, r.Y, d, d, 180, 90);
  p->AddArc(r.X + r.Width - d, r.Y, d, d, 270, 90);
  p->AddArc(r.X + r.Width - d, r.Y + r.Height - d, d, d, 0, 90);
  p->AddArc(r.X, r.Y + r.Height - d, d, d, 90, 90);
  p->CloseFigure();
  return p;
}

void FillRound(Graphics& g, const RectF& r, float rad, const Color& c) {
  SolidBrush br(c);
  GraphicsPath* p = Round(r, rad);
  g.FillPath(&br, p);
  delete p;
}

void StrokeRound(Graphics& g, const RectF& r, float rad, const Color& c, float w = 1.f) {
  Pen pen(c, w);
  GraphicsPath* p = Round(r, rad);
  g.DrawPath(&pen, p);
  delete p;
}

void Text(Graphics& g, const std::wstring& s, const Font& f, const RectF& r, const Color& c,
          StringAlignment h = StringAlignmentNear, StringAlignment v = StringAlignmentCenter, bool wrap = false,
          bool ellipsis = true) {
  StringFormat fmt;
  fmt.SetAlignment(h);
  fmt.SetLineAlignment(v);
  if (!wrap) fmt.SetFormatFlags(StringFormatFlagsNoWrap);
  fmt.SetTrimming(ellipsis ? StringTrimmingEllipsisCharacter : StringTrimmingNone);
  SolidBrush br(c);
  g.DrawString(s.c_str(), -1, &f, r, &fmt, &br);
}

RectF Scaled(const RectF& r) { return RectF(r.X * g_scale + g_ox, r.Y * g_scale + g_oy, r.Width * g_scale, r.Height * g_scale); }

// A row's name and, after it, the (i) that opens its details; where the (i) is drawn is kept for the mouse.
void RowLabel(Graphics& g, Ctl& c, int i, const Font& f, const RectF& r) {
  Text(g, c.label, f, r, kText);
  StringFormat mf;
  mf.SetFormatFlags(StringFormatFlagsNoWrap);
  RectF box;
  g.MeasureString(c.label, -1, &f, PointF(0, 0), &mf, &box);
  const float S = g_scale, d = 15 * S;
  const float x = (std::min)(r.X + box.Width + 4 * S, r.X + r.Width - d);
  c.info = RectF(x, r.Y + r.Height / 2 - d / 2, d, d);
  const bool hot = g_hover_info == i || g_info_open == i;
  Pen pen(hot ? kWhite : kMuted, 1.2f * S);
  g.DrawEllipse(&pen, c.info);
  const FontFamily& fam = *UiFamily();
  Font fi(&fam, 10.5f * S, FontStyleBold, UnitPixel);
  Text(g, L"i", fi, RectF(c.info.X, c.info.Y + 0.5f * S, c.info.Width, c.info.Height), hot ? kWhite : kMuted, StringAlignmentCenter);
}

// A rounded panel of wrapped text at (x, y) at most `width` wide, kept inside the window; returns its rectangle.
RectF TextPanel(Graphics& g, const std::wstring& title, const std::wstring& body, float x, float y, float width, int w, int h, bool above) {
  const float S = g_scale, pad = 14 * S;
  const FontFamily& fam = *UiFamily();
  Font ft(&fam, 14 * S, FontStyleBold, UnitPixel), fb(&fam, 13 * S, FontStyleRegular, UnitPixel);
  StringFormat mf;
  RectF tb{}, bb{};
  if (!title.empty()) g.MeasureString(title.c_str(), -1, &ft, RectF(0, 0, width - 2 * pad, 0), &mf, &tb);
  g.MeasureString(body.c_str(), -1, &fb, RectF(0, 0, width - 2 * pad, 0), &mf, &bb);
  const float ph = pad * 2 + (title.empty() ? 0 : tb.Height + 6 * S) + bb.Height;
  if (above) y -= ph;
  x = (std::max)(8 * S, (std::min)(x, float(w) - width - 8 * S));
  y = (std::max)(8 * S, (std::min)(y, float(h) - ph - 8 * S));
  const RectF panel(x, y, width, ph);
  FillRound(g, RectF(panel.X + 2 * S, panel.Y + 3 * S, panel.Width, panel.Height), 10 * S, Color(120, 0, 0, 0));
  FillRound(g, panel, 10 * S, Color(252, 34, 38, 47));
  StrokeRound(g, panel, 10 * S, Color(255, 64, 70, 84));
  float ty = panel.Y + pad;
  if (!title.empty()) {
    Text(g, title, ft, RectF(panel.X + pad, ty, width - 2 * pad, tb.Height), kWhite, StringAlignmentNear, StringAlignmentNear, true, false);
    ty += tb.Height + 6 * S;
  }
  Text(g, body, fb, RectF(panel.X + pad, ty, width - 2 * pad, bb.Height + 2 * S), kText, StringAlignmentNear, StringAlignmentNear, true, false);
  return panel;
}

// Over everything: the details a click on an (i) opened.
void PaintHelp(Graphics& g, int w, int h) {
  const float S = g_scale;
  if (g_info_open >= 0) {
    SolidBrush dim(Color(150, 0, 0, 0));
    g.FillRectangle(&dim, 0.f, 0.f, float(w), float(h));
    const Ctl& c = g_c[size_t(g_info_open)];
    std::wstring body = c.hint;
    if (c.more) body += std::wstring(L"\n\n") + c.more;
    body += L"\n\nClick anywhere to close.";
    const float width = (std::min)(560 * S, float(w) - 32 * S);
    TextPanel(g, c.label, body, (float(w) - width) / 2, float(h) * 0.22f, width, w, h, false);
  }
}

bool FoldersOk() { return g_folder[0].ok && g_folder[1].ok; }



// The input monitor, laid out like the 0.3.0 page: one box per axis (LX, LY, RX, RY) with its dead
// zone rings, two trigger bars with their thresholds, a 3x3 d-pad and the button pills. Every size is
// a multiple of one design unit `u`, so the whole picture grows with the card instead of staying small.
void DrawMonitorRow(Graphics& g, const Font& fn, const RectF& box, const std::wstring& text, float S) {
  const float u = S * MonitorScale();
  const bool live = wcsstr(text.c_str(), L"LX") != nullptr;
  if (!live) {   // no pad: say so instead of drawing an empty card
    Text(g, text.empty() ? std::wstring(L"No pad is sending data right now.") : text, fn,
         RectF(box.X, box.Y + 20 * u, box.Width, 40 * u), kMuted, StringAlignmentNear, StringAlignmentNear, true);
    return;
  }
  int lx = 0, ly = 0, rx = 0, ry = 0, lt = 0, rt = 0, buttons = 0;
  const wchar_t* p = text.c_str();
  if (const wchar_t* q = wcsstr(p, L"LX")) swscanf(q, L"LX %d LY %d RX %d RY %d", &lx, &ly, &rx, &ry);
  if (const wchar_t* q = wcsstr(p, L"LT")) swscanf(q, L"LT %d RT %d", &lt, &rt);
  if (const wchar_t* q = wcsstr(p, L"buttons 0x")) swscanf(q, L"buttons 0x%x", &buttons);

  auto dz = [](int i) { return (g_dead_row[i] >= 0) ? g_c[size_t(g_dead_row[i])].sel : 5; };
  // The runtime's own inner/outer rule, so the dot sits where the game reads it.
  auto remap = [](float v, int inner, int outer) {
    const float m = fabsf(v) * 127.f;
    if (m <= float(inner)) return 0.f;
    const float scaled = (m - float(inner)) / float((std::max)(inner + 1, outer) - inner) * 127.f;
    return (v < 0 ? -1.f : 1.f) * (std::min)(scaled, 127.f) / 127.f;
  };
  auto axis_box = [&](float x, const wchar_t* label, int value, int inner, int outer, bool horizontal) {
    const float y = box.Y + 16 * u, size = 46 * u;
    Text(g, label, fn, RectF(box.X + x, box.Y, size, 14 * u), kMuted);
    FillRound(g, RectF(box.X + x, y, size, size), 6 * u, Color(255, 35, 37, 43));
    const float cx = box.X + x + size / 2, cy = y + size / 2, reach = size / 2 - 5 * u;
    Pen ring(Color(255, 120, 128, 142), 1.f * u);
    const float ri = reach * (std::min)((std::max)(inner, 0), 127) / 127.f;
    if (ri > 1.f) g.DrawEllipse(&ring, cx - ri, cy - ri, ri * 2, ri * 2);
    const float ro = reach * (std::max)(0, (std::min)(outer, 127)) / 127.f;
    if (outer < 127) g.DrawEllipse(&ring, cx - ro, cy - ro, ro * 2, ro * 2);
    const float t = remap(float((std::clamp)(value, -100, 100)) / 100.f, inner, outer);
    const float limit = reach * (std::max)(0, (std::min)(outer, 127)) / 127.f;   // full scale is the outer ring
    SolidBrush dot(Color(255, 237, 43, 56));
    const float dx = horizontal ? t * limit : 0.f, dy = horizontal ? 0.f : -t * limit;
    g.FillEllipse(&dot, cx + dx - 3.5f * u, cy + dy - 3.5f * u, 7 * u, 7 * u);
  };
  axis_box(0, L"LX", lx, dz(0), dz(1), true);
  axis_box(58 * u, L"LY", ly, dz(0), dz(1), false);
  axis_box(116 * u, L"RX", rx, dz(2), dz(3), true);
  axis_box(174 * u, L"RY", ry, dz(2), dz(3), false);

  auto bar = [&](float y, const wchar_t* label, int value, int inner, int outer) {
    const RectF track(box.X + 30 * u, box.Y + y, 120 * u, 9 * u);
    Text(g, label, fn, RectF(box.X, box.Y + y - 2 * u, 26 * u, 14 * u), kMuted);
    FillRound(g, track, 4.5f * u, Color(255, 35, 37, 43));
    SolidBrush tick(Color(255, 120, 128, 142));
    for (int edge : {inner, outer}) {   // the two thresholds, so the sliders above and the bar line up
      if (edge <= 0 || edge >= 127) continue;
      const float tx = track.X + track.Width * float(edge) / 127.f;
      g.FillRectangle(&tick, tx - 0.5f * u, track.Y - 2 * u, 1.f * u, track.Height + 4 * u);
    }
    const float t = remap(float((std::clamp)(value, 0, 100)) / 100.f, inner, outer);
    if (t > 0.01f) FillRound(g, RectF(track.X, track.Y, track.Width * t, track.Height), 4.5f * u,
                             Color(255, 237, 43, 56));
  };
  bar(78 * u, L"L2", lt, dz(4), dz(5));
  bar(100 * u, L"R2", rt, dz(6), dz(7));

  {   // d-pad: 3x3 grid with the pressed directions lit
    const float cell = 15 * u;
    const RectF pad(box.X, box.Y + 126 * u, cell * 3, cell * 3);
    FillRound(g, pad, 4 * u, Color(255, 35, 37, 43));
    auto lit = [&](int col, int row) {
      FillRound(g, RectF(pad.X + col * cell + 1, pad.Y + row * cell + 1, cell - 2 * u, cell - 2 * u), 3 * u,
                Color(255, 237, 43, 56));
    };
    if (buttons & 0x0001) lit(1, 0);
    if (buttons & 0x0002) lit(1, 2);
    if (buttons & 0x0004) lit(0, 1);
    if (buttons & 0x0008) lit(2, 1);
  }

  const float bx = box.X + 60 * u;
  auto pill = [&](float x, float y, bool on, const wchar_t* label) {
    const float w = 34 * u, h = 18 * u;
    FillRound(g, RectF(x, box.Y + y, w, h), h / 2, on ? Color(255, 237, 43, 56) : Color(255, 35, 37, 43));
    Text(g, label, fn, RectF(x, box.Y + y, w, h), on ? kWhite : kMuted, StringAlignmentCenter);
  };
  const wchar_t* f1 = g_xbox_names ? L"A" : L"Cross";
  const wchar_t* f2 = g_xbox_names ? L"B" : L"Circle";
  const wchar_t* f3 = g_xbox_names ? L"X" : L"Square";
  const wchar_t* f4 = g_xbox_names ? L"Y" : L"Triangle";
  pill(bx, 126 * u, (buttons & 0x1000) != 0, f1);
  pill(bx + 40 * u, 126 * u, (buttons & 0x2000) != 0, f2);
  pill(bx + 80 * u, 126 * u, (buttons & 0x4000) != 0, f3);
  pill(bx + 120 * u, 126 * u, (buttons & 0x8000) != 0, f4);
  pill(bx, 148 * u, (buttons & 0x0100) != 0, L"L1");
  pill(bx + 40 * u, 148 * u, (buttons & 0x0200) != 0, L"R1");
  pill(bx + 80 * u, 148 * u, (buttons & 0x0040) != 0, L"L3");
  pill(bx + 120 * u, 148 * u, (buttons & 0x0080) != 0, L"R3");
  pill(bx, 170 * u, (buttons & 0x0010) != 0, g_xbox_names ? L"Menu" : L"OPT");
  pill(bx + 40 * u, 170 * u, (buttons & 0x0020) != 0, g_xbox_names ? L"View" : L"Share");
  pill(bx + 80 * u, 170 * u, lt > 40, L"LT");
  pill(bx + 120 * u, 170 * u, rt > 40, L"RT");
}
void Paint(HDC hdc, int w, int h) {
  // The monitor refreshes itself only while its page is on screen.
  if (g_hwnd) {
    if (g_tab == kTabDriving && g_subtab == kSubInput) SetTimer(g_hwnd, kMonitorTimer, 150, nullptr);
    else KillTimer(g_hwnd, kMonitorTimer);
  }
  Graphics g(hdc);
  g.SetSmoothingMode(SmoothingModeAntiAlias);
  g.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);
  g.Clear(kBg);
  const FontFamily& fam = *UiFamily();
  const float S = g_scale;
  g_oy = ContentOy();
  for (auto& c : g_c) c.info = RectF();  // set again for the rows drawn below
  Font fTitle(&fam, 22 * S, FontStyleBold, UnitPixel), fSub(&fam, 13 * S, FontStyleRegular, UnitPixel),
      fCard(&fam, 12 * S, FontStyleBold, UnitPixel), fLabel(&fam, 14 * S, FontStyleRegular, UnitPixel),
      fSegBold(&fam, 13 * S, FontStyleBold, UnitPixel), fHint(&fam, 12.5f * S, FontStyleRegular, UnitPixel),
      fBtn(&fam, 14 * S, FontStyleBold, UnitPixel), fSeg(&fam, 13 * S, FontStyleRegular, UnitPixel);

  // header
  FillRound(g, Scaled(RectF(kMargin, 26, 8, 40)), 3 * S, kAccent);
  Text(g, L"DRIVECLUB", fTitle, Scaled(RectF(kMargin + 22, 20, 520, 32)), kText);
  Text(g, L"PC  -  game folders and patches", fSub, Scaled(RectF(kMargin + 22, 50, 520, 22)), kMuted);
  {
    const bool folder_problem = !FolderProblem(0).empty() || !FolderProblem(1).empty();
    const wchar_t* chip = GpuBlocked() ? (g_gpu_problem.rfind(L"Your processor", 0) == 0 ? L"Processor not supported" : L"Graphics card not supported")
                          : FoldersOk() ? L"Game found" : folder_problem ? L"Check the game folders" : L"Choose the game folders";
    StringFormat measure(StringFormat::GenericTypographic());
    measure.SetFormatFlags(StringFormatFlagsNoWrap | StringFormatFlagsMeasureTrailingSpaces);
    RectF box;
    g.MeasureString(chip, -1, &fSeg, PointF(0, 0), &measure, &box);
    const float cw = box.Width / S + 52.f;
    RectF cr = Scaled(RectF(g_win_w - kMargin - cw, 32, cw, 30));
    const bool chip_ok = FoldersOk() && !GpuBlocked();
    FillRound(g, cr, 15 * S, chip_ok ? Color(255, 28, 52, 38) : Color(255, 60, 40, 28));
    SolidBrush dot(chip_ok ? kOk : kWarn);
    g.FillEllipse(&dot, cr.X + 13 * S, cr.Y + 11 * S, 8 * S, 8 * S);
    Text(g, chip, fSeg, RectF(cr.X + 30 * S, cr.Y, cr.Width - 30 * S, cr.Height), chip_ok ? kOk : kWarn,
         StringAlignmentNear, StringAlignmentCenter, false, false);

    // The Discord button, left of the chip: the community server.
    g_discord_rect = RectF(g_win_w - kMargin - cw - 12.f - 30.f, 32.f, 30.f, 30.f);
    const RectF dr = Scaled(g_discord_rect);
    SolidBrush blurple(g_hover_discord ? Color(255, 112, 125, 255) : Color(255, 88, 101, 242));
    g.FillEllipse(&blurple, dr);
    // The mark: the face of the logo on a 24 x 24 grid, drawn in white with the two eyes cut out.
    const float u = dr.Width * 0.62f / 24.f, ox = dr.X + dr.Width * 0.19f, oy = dr.Y + dr.Height * 0.21f;
    const auto P = [&](float px, float py) { return PointF(ox + px * u, oy + py * u); };
    GraphicsPath mark;
    mark.StartFigure();
    mark.AddBezier(P(4.5f, 4.2f), P(7.0f, 2.9f), P(9.5f, 2.4f), P(12.0f, 2.4f));
    mark.AddBezier(P(12.0f, 2.4f), P(14.5f, 2.4f), P(17.0f, 2.9f), P(19.5f, 4.2f));
    mark.AddBezier(P(19.5f, 4.2f), P(21.4f, 7.2f), P(22.5f, 11.0f), P(22.2f, 16.0f));
    mark.AddBezier(P(22.2f, 16.0f), P(20.5f, 17.5f), P(18.7f, 18.4f), P(16.8f, 19.0f));
    mark.AddLine(P(16.8f, 19.0f), P(15.8f, 17.2f));
    mark.AddBezier(P(15.8f, 17.2f), P(16.6f, 16.9f), P(17.3f, 16.6f), P(18.0f, 16.1f));
    mark.AddBezier(P(18.0f, 16.1f), P(15.0f, 17.5f), P(9.0f, 17.5f), P(6.0f, 16.1f));
    mark.AddBezier(P(6.0f, 16.1f), P(6.7f, 16.6f), P(7.4f, 16.9f), P(8.2f, 17.2f));
    mark.AddLine(P(8.2f, 17.2f), P(7.2f, 19.0f));
    mark.AddBezier(P(7.2f, 19.0f), P(5.3f, 18.4f), P(3.5f, 17.5f), P(1.8f, 16.0f));
    mark.AddBezier(P(1.8f, 16.0f), P(1.5f, 11.0f), P(2.6f, 7.2f), P(4.5f, 4.2f));
    mark.CloseFigure();
    SolidBrush white(Color(255, 255, 255, 255));
    g.FillPath(&white, &mark);
    g.FillEllipse(&blurple, ox + 5.9f * u, oy + 8.4f * u, 4.4f * u, 4.8f * u);
    g.FillEllipse(&blurple, ox + 13.7f * u, oy + 8.4f * u, 4.4f * u, 4.8f * u);
  }

  // tabs: one page of cards at a time
  for (int t = 0; t < kTabCount; ++t) {
    const RectF tr = Scaled(g_tab_rect[t]);
    const bool sel = t == g_tab;
    FillRound(g, tr, tr.Height / 2, sel ? (g_hover_tab == t ? kAccentHover : kAccent)
                                        : (g_hover_tab == t ? kTrackHover : kTrack));
    Text(g, kTabName[t], fSeg, tr, sel ? kWhite : kMuted, StringAlignmentCenter);
  }
  // the Driving tab's own tabs: a full-width two-part switch, the chosen part filled
  if (HasSubTabs()) {
    const RectF a = Scaled(g_sub_rect[0]), b = Scaled(g_sub_rect[kSubCount - 1]);
    const RectF bar(a.X, a.Y, b.X + b.Width - a.X, a.Height);
    FillRound(g, bar, 12 * S, kCard);
    StrokeRound(g, bar, 12 * S, kCardLine, 1.2f * S);
    Font fSub(&fam, 15 * S, FontStyleBold, UnitPixel);
    for (int s = 0; s < kSubCount; ++s) {
      const RectF sr = Scaled(g_sub_rect[s]);
      const RectF in(sr.X + 4 * S, sr.Y + 4 * S, sr.Width - 8 * S, sr.Height - 8 * S);
      const bool sel = s == g_subtab, over = g_hover_tab == kTabCount + s;
      if (sel) FillRound(g, in, 9 * S, over ? kAccentHover : kAccent);
      else if (over) FillRound(g, in, 9 * S, kTrackHover);
      Text(g, kSubName[s], fSub, in, sel ? kWhite : kText, StringAlignmentCenter);
    }
  }

  // game folders card
  {
    const RectF cr = Scaled(g_folders_rect);
    const float path_w = g_folders_in_flow ? 16 * S : 130 * S;  // in a column the Browse button is beside the label, not the path
    FillRound(g, cr, 12 * S, kCard);
    StrokeRound(g, cr, 12 * S, kCardLine);
    Text(g, L"GAME FILES", fCard, RectF(cr.X + kPad * S, cr.Y + 6 * S, cr.Width - 2 * kPad * S, (kTitleH - 6) * S), kMuted);
    for (int i = 0; i < 2; ++i) {
      const FolderRow& f = g_folder[i];
      const RectF row = Scaled(f.row), br = Scaled(f.browse);
      Text(g, f.label, fLabel, RectF(row.X, row.Y + 4 * S, row.Width - 130 * S, 22 * S), kText);
      SolidBrush dot(f.ok ? kOk : (f.path.empty() ? kOff : kWarn));
      g.FillEllipse(&dot, row.X, row.Y + 38 * S, 8 * S, 8 * S);
      const std::wstring shown = f.path.empty() ? std::wstring(L"Not chosen yet") : f.path;
      Text(g, shown, fHint, RectF(row.X + 16 * S, row.Y + 28 * S, row.Width - path_w, 28 * S),
           f.path.empty() ? kMuted : kText);
      if (const std::wstring problem = FolderProblem(i); !problem.empty()) {
        SolidBrush bad(kBad);
        g.FillEllipse(&bad, row.X, row.Y + 38 * S, 8 * S, 8 * S);
        Text(g, problem, fHint, RectF(row.X + 16 * S, row.Y + 50 * S, row.Width - path_w, 22 * S), kBad);
      }
      const bool over = g_hover_browse == i;
      FillRound(g, br, 10 * S, over ? Color(255, 66, 73, 88) : Color(255, 50, 56, 68));
      Text(g, L"Browse...", fBtn, br, kWhite, StringAlignmentCenter);
    }
  }

  // cards
  for (auto& card : g_cards) {
    if (!OnPage(card)) continue;
    RectF cr = Scaled(card.rect);
    FillRound(g, cr, 12 * S, kCard);
    StrokeRound(g, cr, 12 * S, kCardLine);
    Text(g, card.title, fCard, RectF(cr.X + kPad * S, cr.Y + 6 * S, cr.Width - 2 * kPad * S, (kTitleH - 6) * S), kMuted);
    if (card.note) {
      Text(g, card.note, fHint, RectF(cr.X + kPad * S, cr.Y + cr.Height - (card.note_h + 10) * S, cr.Width - 2 * kPad * S, card.note_h * S), kMuted,
           StringAlignmentNear, StringAlignmentFar, true);
    }
    for (int i : card.ctls) {
      Ctl& c = g_c[i];
      if (RowHidden(c)) continue;
      const RectF row = Scaled(c.row), ct = Scaled(c.ctl);
      const bool hot = g_hover_ctl == i || g_drag == i;
      if (c.kind == Kind::Slider) {
        // Label and value above a track with a knob.
        RowLabel(g, c, i, fLabel, RectF(row.X, row.Y + 2 * S, row.Width - 70 * S, 22 * S));
        std::wstring value = std::to_wstring(c.sel);
        if (c.decimals > 0) {
          wchar_t buf[32];
          swprintf(buf, 32, L"%.*f", c.decimals, c.sel / std::pow(10.0, c.decimals));
          value = buf;
        }
        Text(g, value + c.unit, fSegBold, RectF(row.X + row.Width - 90 * S, row.Y + 2 * S, 90 * S, 22 * S),
             hot ? kWhite : kText, StringAlignmentFar);
        const float t = c.max > c.min ? float((std::clamp)(c.sel, c.min, c.max) - c.min) / float(c.max - c.min) : 0.f;
        const RectF track(ct.X + 8 * S, ct.Y + ct.Height / 2 - 3 * S, ct.Width - 16 * S, 6 * S);
        FillRound(g, track, 3 * S, hot ? kTrackHover : kTrack);
        FillRound(g, RectF(track.X, track.Y, (std::max)(track.Height, track.Width * t), track.Height), 3 * S, hot ? kAccentHover : kAccent);
        SolidBrush knob(kWhite);
        const float d = 16 * S;
        g.FillEllipse(&knob, track.X + track.Width * t - d / 2, ct.Y + ct.Height / 2 - d / 2, d, d);
        continue;
      }
      if (c.kind == Kind::Binding) {
        // The action, then two chips: the keyboard key on the left, the pad button on the right.
        RowLabel(g, c, i, fLabel, RectF(row.X, row.Y + 2 * S, row.Width, 22 * S));
        const bool pad_locked = BindPadLocked(c);   // an axis is read as an axis: nothing to remap
        const float gap = 8 * S;
        const float left_w = (row.Width - gap) * 0.5f;
        const RectF kb(row.X, row.Y + 26 * S, left_w, 28 * S);
        const RectF pd(row.X + left_w + gap, row.Y + 26 * S, left_w, 28 * S);
        FillRound(g, kb, 8 * S, hot ? kTrackHover : kTrack);
        FillRound(g, pd, 8 * S, hot ? kTrackHover : kTrack);
        const std::wstring kb_text = c.items.empty() || c.items[0].empty() ? std::wstring(L"\u2014") : c.items[0];
        Text(g, kb_text, fSeg, RectF(kb.X + 10 * S, kb.Y, kb.Width - 20 * S, kb.Height),
             c.items.empty() || c.items[0].empty() ? kMuted : kText, StringAlignmentCenter);
        const std::wstring pad_show =
            (c.items.size() < 2 || c.items[1].empty()) ? std::wstring(L"\u2014")
            : pad_locked              ? c.items[1]                                   // already a readable name
            : PadShowName(c.items[1]) ? std::wstring(PadShowName(c.items[1]))
                                      : c.items[1];
        Text(g, pad_show, fSeg, RectF(pd.X + 10 * S, pd.Y, pd.Width - 20 * S, pd.Height),
             (c.items.size() < 2 || c.items[1].empty() || pad_locked) ? kMuted : kText, StringAlignmentCenter);
        continue;
      }
      if (c.kind == Kind::Info) {
        if (!c.items.empty() && !c.items[0].empty())
          Text(g, c.items[0], fSeg, RectF(row.X, row.Y + 4 * S, row.Width, row.Height - 8 * S),
               c.sel ? kAccent : kText, StringAlignmentNear, StringAlignmentNear, true);   // red: the pad read
        continue;
      }
      if (c.kind == Kind::Monitor) {
        RowLabel(g, c, i, fLabel, RectF(row.X, row.Y + 2 * S, row.Width, 22 * S));
        DrawMonitorRow(g, fHint, RectF(row.X, row.Y + 26 * S, row.Width, row.Height - 30 * S),
                       c.items.empty() ? std::wstring() : c.items[0], S);
        continue;
      }
      if (c.kind == Kind::Button) {
        FillRound(g, ct, 10 * S, hot ? Color(255, 66, 73, 88) : Color(255, 50, 56, 68));
        Text(g, c.items[0], fBtn, ct, kWhite, StringAlignmentCenter);
        continue;
      }
      if (c.kind == Kind::Toggle) {
        const bool locked = BlurRowLocked(c);
        // Locked: shown as the game has it (blur on, the patch off), dimmed, with what changing it would cost.
        const bool on = locked ? &c == &g_c[g_gfx[kGfxBlur]] : c.sel != 0;
        if (locked) {
          const std::wstring name = std::wstring(c.label) + L" (Disables AA)";
          Ctl shown = c;
          shown.label = name.c_str();
          RowLabel(g, shown, i, fLabel, RectF(row.X, row.Y + 2 * S, row.Width - 60 * S, 40 * S));
          c.info = shown.info;
        } else {
          RowLabel(g, c, i, fLabel, RectF(row.X, row.Y + 2 * S, row.Width - 60 * S, 40 * S));
        }
        FillRound(g, ct, ct.Height / 2, locked ? (on ? Color(255, 120, 98, 40) : kOff) : on ? (hot ? kAccentHover : kAccent) : (hot ? Color(255, 80, 87, 102) : kOff));
        SolidBrush knob(locked ? kMuted : kWhite);
        const float d = ct.Height - 6 * S;
        g.FillEllipse(&knob, on ? ct.X + ct.Width - d - 3 * S : ct.X + 3 * S, ct.Y + 3 * S, d, d);
      } else if (c.kind == Kind::Picker) {
        // A box with the chosen name and a down arrow: a click opens the menu of choices.
        RowLabel(g, c, i, fLabel, RectF(row.X, row.Y + 2 * S, row.Width, 22 * S));
        FillRound(g, ct, 9 * S, hot ? kTrackHover : kTrack);
        Text(g, c.items[0], fSegBold, RectF(ct.X + 12 * S, ct.Y, ct.Width - 44 * S, ct.Height), kText);
        SolidBrush arrow(kMuted);
        const float ax = ct.X + ct.Width - 22 * S, ay = ct.Y + ct.Height / 2 - 2 * S;
        const PointF tri[3] = {PointF(ax - 5 * S, ay), PointF(ax + 5 * S, ay), PointF(ax, ay + 6 * S)};
        g.FillPolygon(&arrow, tri, 3);
      } else if (c.kind == Kind::Action) {
        // The benchmark: Run, then the last result.
        RowLabel(g, c, i, fLabel, RectF(row.X, row.Y + 64 * S, row.Width, 22 * S));
        const RectF res(row.X, row.Y + 88 * S, row.Width, row.Height - 88 * S);
        if (g_bench_running) {
          Text(g, L"Running. The game closes by itself when it is done.", fHint, res, kText,
               StringAlignmentNear, StringAlignmentNear, true);
        } else if (g_bench_result.ok) {
          Font fBig(&fam, 24 * S, FontStyleBold, UnitPixel);
          const std::wstring big = g_bench_result.fps + L" FPS";
          Text(g, big, fBig, RectF(res.X, res.Y, res.Width, 32 * S), kWhite);
          Text(g, g_bench_result.lows, fSeg, RectF(res.X, res.Y + 32 * S, res.Width, 20 * S), kText);
          Text(g, g_bench_result.about, fHint, RectF(res.X, res.Y + 56 * S, res.Width, 38 * S), kMuted,
               StringAlignmentNear, StringAlignmentNear, true);
          if (!g_bench_ran_short.empty())
            Text(g, g_bench_ran_short, fHint, RectF(res.X, res.Y + 96 * S, res.Width, res.Height - 96 * S),
                 g_bench_ran_hover ? kText : kMuted, StringAlignmentNear, StringAlignmentNear, true);
        } else {
          Text(g, L"No result yet.", fHint, res, kMuted, StringAlignmentNear,
               StringAlignmentNear, true);
        }
        const bool disabled = g_busy || !FoldersOk() || GpuBlocked();
        FillRound(g, ct, 10 * S, disabled ? kOff : hot && g_hover_seg == 0 ? kAccentHover : kAccent);
        Text(g, L"Run benchmark", fBtn, ct, disabled ? kMuted : kWhite, StringAlignmentCenter);
      } else {
        RowLabel(g, c, i, fLabel, RectF(row.X, row.Y + 2 * S, row.Width, 22 * S));
        FillRound(g, ct, 9 * S, kTrack);
        const float sw = ct.Width / float(c.items.size());
        for (size_t k = 0; k < c.items.size(); ++k) {
          RectF seg(ct.X + sw * k + 3 * S, ct.Y + 3 * S, sw - 6 * S, ct.Height - 6 * S);
          const bool sel = int(k) == c.sel;
          const bool over = hot && g_hover_seg == int(k);
          if (sel) FillRound(g, seg, 7 * S, over ? kAccentHover : kAccent);
          else if (over) FillRound(g, seg, 7 * S, kTrackHover);
          float px = 13.f;
          StringFormat mf;
          mf.SetFormatFlags(StringFormatFlagsNoWrap);
          for (; px > 9.f; px -= 0.5f) {
            Font ft(&fam, px * S, sel ? FontStyleBold : FontStyleRegular, UnitPixel);
            RectF bx;
            g.MeasureString(c.items[k].c_str(), -1, &ft, PointF(0, 0), &mf, &bx);
            if (bx.Width <= seg.Width - 8 * S) break;
          }
          Font fit(&fam, px * S, sel ? FontStyleBold : FontStyleRegular, UnitPixel);
          Text(g, c.items[k], fit, seg, sel ? kWhite : kMuted, StringAlignmentCenter);
        }
      }
    }
  }

  // footer: hint / status / progress, buttons
  g_oy = FooterOy();
  const float fy = g_oy + g_content_h * S;
  SolidBrush footer_brush(Color(255, 12, 13, 16));
  g.FillRectangle(&footer_brush, 0.f, fy, float(w), float(h) - fy);
  std::wstring msg = g_status;
  Color mc = g_status_color;
  if (g_busy) {
    msg = g_progress_text;
    mc = kText;
  } else if (g_status_sticky) {
    // the warning stays in front of the hints
  } else if (g_hover_ctl >= 0) {
    msg = g_hover_ctl == g_motion_blur ? MotionBlurHint(g_hover_seg >= 0 ? g_hover_seg : g_c[g_motion_blur].sel) : g_c[g_hover_ctl].hint;
    mc = kMuted;
  } else if (g_hover_browse >= 0) {
    msg = g_folder[g_hover_browse].hint;
    mc = kMuted;
  } else if (g_hover_discord) {
    msg = L"Join the DriveClub Recomp community on Discord.";
    mc = kMuted;
  } else if (g_hover_tab >= 0) {
    msg = g_hover_tab < kTabCount ? kTabHint[g_hover_tab] : kSubHint[g_hover_tab - kTabCount];
    mc = kMuted;
  } else if (g_hover_btn == 3) {
    msg = L"Download version " + g_update.version + L", check it and install it over this one. Your saves and settings stay.";
    mc = kMuted;
  } else if (g_hover_btn == 0) {
    msg = L"Put every patch back to its default.";
    mc = kMuted;
  } else if (g_hover_btn == 2) {
    msg = FoldersOk() ? L"Save and start the game." : L"Choose both game folders first.";
    mc = kMuted;
  }
  {
    const RectF fr(g_ox + kMargin * S, fy + 8 * S, (FooterButtonsLeft() - kMargin - 12) * S, (kFooterH - 16) * S);
    StringFormat mfmt;
    mfmt.SetTrimming(StringTrimmingNone);
    float px = 12.5f;
    for (; px > 9.f; px -= 0.5f) {
      Font ft(&fam, px * S, FontStyleRegular, UnitPixel);
      RectF bounds;
      g.MeasureString(msg.c_str(), -1, &ft, RectF(0, 0, fr.Width, 0), &mfmt, &bounds);
      if (bounds.Height <= fr.Height) break;
    }
    Font fit(&fam, px * S, FontStyleRegular, UnitPixel);
    Text(g, msg, fit, fr, mc, StringAlignmentNear, StringAlignmentCenter, true, false);
  }
  if (g_busy) {
    const RectF bar = Scaled(RectF(kMargin, g_content_h + kFooterH - 18.f, FooterButtonsLeft() - kMargin - 12, 6.f));
    FillRound(g, bar, 3 * S, kTrack);
    FillRound(g, RectF(bar.X, bar.Y, bar.Width * float(g_progress.load()) / 100.f, bar.Height), 3 * S, kAccent);
  }
  for (int i = 0; i < 3; ++i) {
    RectF br = Scaled(g_btn[i]);
    const bool over = g_hover_btn == i && !g_busy, down = g_press_btn == i && over;
    const bool disabled = (i == 2 && (!FoldersOk() || GpuBlocked())) || g_busy;
    Color fill = i == 2 ? (disabled ? kOff : (down ? Color(255, 200, 52, 30) : over ? kAccentHover : kAccent))
                        : (i == 1 ? (down ? Color(255, 56, 62, 74) : over ? Color(255, 66, 73, 88) : Color(255, 50, 56, 68))
                                  : (over ? kTrackHover : kTrack));
    FillRound(g, br, 10 * S, fill);
    Text(g, kBtnText[i], fBtn, br, disabled ? kMuted : kWhite, StringAlignmentCenter);
  }
  if (g_update_ready && !g_busy) {
    const RectF ub = Scaled(g_update_btn);
    const bool over = g_hover_btn == 3, down = g_press_btn == 3 && over;
    FillRound(g, ub, 10 * S, down ? Color(255, 56, 62, 74) : over ? Color(255, 66, 73, 88) : Color(255, 50, 56, 68));
    Text(g, L"Update to " + g_update.version, fBtn, ub, kOk, StringAlignmentCenter);
  }
  g_oy = ContentOy();
  // The benchmark result under the pointer: every setting the game ran with, in a panel over the page.
  if (g_bench_ran_hover && g_brun >= 0 && !g_bench_running && g_bench_result.ok && !g_bench_ran_lines.empty()) {
    const RectF row = Scaled(g_c[g_brun].row);
    const float line_h = 19 * S, pad = 12 * S;
    const float ph = pad * 2 + line_h * float(g_bench_ran_lines.size()), pw = row.Width;
    float py = row.Y + 88 * S - ph - 8 * S;  // above the result, where the page has room
    if (py < 8 * S) py = row.Y + row.Height + 4 * S;
    if (py + ph > float(h) - 8 * S) py = (std::max)(8 * S, float(h) - 8 * S - ph);
    const RectF panel(row.X, py, pw, ph);
    FillRound(g, panel, 10 * S, Color(250, 30, 34, 42));
    for (size_t i = 0; i < g_bench_ran_lines.size(); ++i) {
      Text(g, g_bench_ran_lines[i], i == 0 ? fCard : fHint,
           RectF(panel.X + pad, panel.Y + pad + line_h * float(i), panel.Width - 2 * pad, line_h), i == 0 ? kWhite : kText);
    }
  }
  PaintHelp(g, w, h);
}

// ------------------------------------------------------------------------------------------
// preparing and starting the game
// ------------------------------------------------------------------------------------------
std::wstring PatchXmlPath() {
  const std::wstring bundled = g_runtime + L"\\patches\\DriveClub.xml";
  return Exists(bundled) ? bundled : g_home + L"\\..\\..\\patches\\DriveClub.xml";  // development tree
}

std::wstring GameExe() {
  wchar_t diagnostic[64] = {};
  const DWORD length = GetEnvironmentVariableW(L"BB_DIAGNOSTIC_RUNTIME", diagnostic, 64);
  if (length > 0 && length < 64 &&
      std::wstring(diagnostic) == L"DriveClubGame.diagnostics.exe")
    return g_runtime + L"\\DriveClubGame.diagnostics.exe";
  const std::wstring bundled = g_runtime + L"\\DriveClubGame.exe";
  return Exists(bundled) ? bundled : g_home + L"\\..\\DriveClubGame.exe";
}

DWORD g_started_game_process_id = 0;
std::wstring g_external_progress;
void WriteExternalProgress(int percent, const std::wstring& text);
void SetProgress(int percent, const std::wstring& text) {
  WriteExternalProgress(percent, text);
  g_progress = percent;
  g_progress_text = text;
  if (g_hwnd) PostMessageW(g_hwnd, WM_APP + 1, 0, 0);
}

// Prepares the game from the two dumps (a few seconds the first time). Returns "" on success, else the error.
std::wstring RunPrepare() {
  const std::wstring stamp_path = g_data + L"\\prep\\stamp.txt";
  const std::wstring stamp_now = g_folder[0].path + L"|" + g_folder[1].path + L"|" + Widen(kBootFormat);
  std::wstring stamp_old;
  for (const auto& l : ReadLines(stamp_path)) stamp_old = Widen(l);
  const bool ready = Exists(g_data + L"\\prep\\boot-linked.bin") && Exists(g_data + L"\\prep\\image.meta");
  const bool patches_only = ready && stamp_old == stamp_now;
  const std::string error = PrepareGame(
      std::filesystem::path(g_folder[0].path), std::filesystem::path(g_folder[1].path), std::filesystem::path(g_data),
      std::filesystem::path(PatchXmlPath()), patches_only, [](int step, const std::string& text) {
        SetProgress(step * 100 / 6, Widen(text));
      });
  if (!error.empty()) return Widen(error);
  std::ofstream out(stamp_path.c_str(), std::ios::binary | std::ios::trunc);
  out << Narrow(stamp_now);
  return L"";
}

std::wstring Timestamp() {
  SYSTEMTIME t;
  GetLocalTime(&t);
  wchar_t b[40];
  wsprintfW(b, L"%04d%02d%02d_%02d%02d%02d", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
  return b;
}

// ------------------------------------------------------------------------------------------
// launcher log and checks before the game starts
// ------------------------------------------------------------------------------------------
// logs\launcher.log keeps what the launcher saw (the log of the game only exists once the game has started, so a
// failure while preparing would leave nothing to attach to a report). The checks refuse only what cannot work.
std::wstring Leaf(const std::wstring& path) {
  std::wstring p = path;
  while (!p.empty() && (p.back() == L'\\' || p.back() == L'/')) p.pop_back();
  const size_t slash = p.find_last_of(L"\\/");
  return slash == std::wstring::npos ? p : p.substr(slash + 1);
}

// Online play (docs/online): every online address of the game goes to a community server, the player's name is
// a local account. Off removes what an earlier session wrote, so offline play is exactly as before.
// ------------------------------------------------------------------------------------------
// Save profiles and backups: data\user\savedata is the active profile's saves. The other profiles wait in
// data\profiles\<n>, the backups in data\backups\profile<n>\<date>. Saves are small (a few hundred KB).
// ------------------------------------------------------------------------------------------
namespace fs = std::filesystem;

bool ProcessRunning(const wchar_t* exe_name) {
  const HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (snap == INVALID_HANDLE_VALUE) return false;
  PROCESSENTRY32W e{sizeof(e)};
  bool found = false;
  for (BOOL ok = Process32FirstW(snap, &e); ok && !found; ok = Process32NextW(snap, &e))
    found = _wcsicmp(e.szExeFile, exe_name) == 0;
  CloseHandle(snap);
  return found;
}

std::wstring SavesDir() { return g_data + L"\\user\\savedata"; }
std::wstring ProfileDir(int p) { return g_data + L"\\profiles\\" + std::to_wstring(p + 1); }
std::wstring BackupDir(int p) { return g_data + L"\\backups\\profile" + std::to_wstring(p + 1); }
std::wstring ActiveProfilePath() { return g_data + L"\\profiles\\active.txt"; }

int ActiveProfile() {
  const auto lines = ReadLines(ActiveProfilePath());
  const int p = lines.empty() ? 0 : atoi(lines[0].c_str());
  return p >= 0 && p < 4 ? p : 0;
}

int SelectedProfile() { return g_profile_row >= 0 ? g_c[g_profile_row].sel : 0; }

std::wstring Stamp() {
  SYSTEMTIME t;
  GetLocalTime(&t);
  wchar_t text[32];
  swprintf(text, 32, L"%04d%02d%02d_%02d%02d%02d", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
  return text;
}

// Keeps the newest `keep` backups of a profile.
void PruneBackups(int p, size_t keep) {
  std::error_code ec;
  std::vector<std::wstring> names;
  for (const auto& d : fs::directory_iterator(BackupDir(p), ec))
    if (d.is_directory()) names.push_back(d.path().filename().wstring());
  std::sort(names.begin(), names.end());
  for (size_t i = 0; i + keep < names.size(); ++i) fs::remove_all(BackupDir(p) + L"\\" + names[i], ec);
}

// Copies the active profile's saves into a new dated backup. "" when done; otherwise a sentence for the player.
std::wstring BackupSaves() {
  std::error_code ec;
  if (!fs::exists(SavesDir(), ec)) return L"There are no saves to back up yet.";
  const int p = ActiveProfile();
  const std::wstring dest = BackupDir(p) + L"\\" + Stamp();
  fs::create_directories(dest, ec);
  fs::copy(SavesDir(), dest, fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
  if (ec) return L"The backup could not be written: " + Widen(ec.message());
  PruneBackups(p, 10);
  return L"";
}

// Makes `to` the active profile: the current saves go to their profile folder and the chosen profile's come back.
std::wstring SwitchProfile(int to) {
  const int from = ActiveProfile();
  if (to == from) return L"";
  if (ProcessRunning(L"DriveClubGame.exe")) return L"Close the game before switching save profiles.";
  std::error_code ec;
  const std::wstring problem = BackupSaves();  // a safety copy; "no saves yet" is fine here
  (void)problem;
  fs::create_directories(g_data + L"\\profiles", ec);
  fs::remove_all(ProfileDir(from), ec);
  if (fs::exists(SavesDir(), ec)) {
    fs::rename(SavesDir(), ProfileDir(from), ec);
    if (ec) return L"The saves could not be moved: " + Widen(ec.message());
  }
  if (fs::exists(ProfileDir(to), ec)) {
    fs::rename(ProfileDir(to), SavesDir(), ec);
    if (ec) return L"The profile could not be loaded: " + Widen(ec.message());
  } else {
    fs::create_directories(SavesDir(), ec);  // a new career
  }
  SaveTextAtomically(ActiveProfilePath(), std::to_string(to));
  return L"";
}

std::wstring RestoreLatestBackup() {
  if (ProcessRunning(L"DriveClubGame.exe")) return L"Close the game before restoring saves.";
  std::wstring problem = SwitchProfile(SelectedProfile());
  if (!problem.empty()) return problem;
  const int p = ActiveProfile();
  std::error_code ec;
  std::vector<std::wstring> names;
  for (const auto& d : fs::directory_iterator(BackupDir(p), ec))
    if (d.is_directory()) names.push_back(d.path().filename().wstring());
  if (names.empty()) return L"There is no backup of this profile yet.";
  std::sort(names.begin(), names.end());
  const std::wstring latest = BackupDir(p) + L"\\" + names.back();
  BackupSaves();  // the current saves are kept too
  fs::remove_all(SavesDir(), ec);
  fs::create_directories(SavesDir(), ec);
  fs::copy(latest, SavesDir(), fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
  if (ec) return L"The backup could not be restored: " + Widen(ec.message());
  return L"";
}

// What the two buttons say afterwards, in the status line of both launchers. Returns false on a problem.
bool RunSaveButton(bool restore, std::wstring& message) {
  std::wstring problem = SwitchProfile(SelectedProfile());
  if (problem.empty()) problem = restore ? RestoreLatestBackup() : BackupSaves();
  if (!problem.empty()) {
    message = problem;
    return false;
  }
  message = restore ? L"The latest backup is restored." : L"The saves are backed up.";
  return true;
}

bool ServerRunning() {
  const HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (snap == INVALID_HANDLE_VALUE) return false;
  PROCESSENTRY32W e{sizeof(e)};
  bool found = false;
  for (BOOL ok = Process32FirstW(snap, &e); ok && !found; ok = Process32NextW(snap, &e))
    found = _wcsicmp(e.szExeFile, L"driveclubsrv.exe") == 0;
  CloseHandle(snap);
  return found;
}

// Returns a problem text, empty when the game can start.
std::wstring PrepareOnline(const std::wstring& user) {
  const std::wstring written = user + L"\\online_files.txt";
  const std::wstring mods = g_home + L"\\mods";
  const int mode = g_online_row >= 0 ? g_c[g_online_row].sel : 0;
  for (const wchar_t* v : {L"BB_ONLINE", L"BB_NP_LOCAL", L"BB_COOP_SERVER"}) SetEnvironmentVariableW(v, nullptr);
  for (const auto& line : ReadLines(written)) DeleteFileW(Widen(line).c_str());
  DeleteFileW(written.c_str());
  if (mode == 0) return L"";

  if (g_name.size() < 3 || g_name.size() > 16 ||
      std::any_of(g_name.begin(), g_name.end(), [](wchar_t ch) { return !(iswalnum(ch) && ch < 128) && ch != L'-' && ch != L'_'; }))
    return L"For online play the player name needs 3 to 16 letters, digits, - or _.";
  std::wstring server = L"127.0.0.1:8080";
  if (mode == 2) {
    if (g_online_host.empty()) return L"Type the IP address of the friend who hosts under Host's IP address.";
    server = g_online_host + L":8080";
  } else if (!ServerRunning()) {
    const std::wstring dir = g_home + L"\\server";
    const std::wstring exe = dir + L"\\driveclubsrv.exe";
    if (!Exists(exe)) return L"The server is missing: " + exe;
    STARTUPINFOW si{sizeof(si)};
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_SHOWMINNOACTIVE;
    PROCESS_INFORMATION pi{};
    std::wstring cmdline = L"\"" + exe + L"\"";
    if (!CreateProcessW(exe.c_str(), cmdline.data(), nullptr, nullptr, FALSE, 0, nullptr, dir.c_str(), &si, &pi))
      return L"The server could not be started: " + exe;
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    Sleep(2000);
  }

  std::string list;
  const auto write = [&](const std::wstring& path, const std::string& text) {
    std::ofstream out(path.c_str(), std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out << text;
    list += Narrow(path) + "\r\n";
    return true;
  };
  const std::string url = "\"http://" + Narrow(server) + "\"";
  CreateDirectoryW(user.c_str(), nullptr);
  CreateDirectoryW(mods.c_str(), nullptr);
  if (!write(user + L"\\host_overrides.json",
             "{\r\n  \"hub.services.otg.scee.com\": " + url + ",\r\n  \"Aegs.driveclub.online.scee.com\": " + url +
                 ",\r\n  \"aegs.driveclub.online.scee.com\": " + url + ",\r\n  \"driveclub.api.wwsga.me\": " + url +
                 ",\r\n  \"*\": " + url + "\r\n}\r\n"))
    return L"Could not write the online settings in " + user;
  // The game's server settings (TSS): slot 0 from the package, slots 1 to 6 empty.
  std::ifstream tss((g_home + L"\\online\\override-0.tss").c_str(), std::ios::binary);
  if (!tss) return L"The online settings file is missing: " + g_home + L"\\online\\override-0.tss";
  std::ostringstream body;
  body << tss.rdbuf();
  if (!write(mods + L"\\override-0.tss", body.str())) return L"Could not write the online settings in " + mods;
  for (int i = 1; i <= 6; ++i) {
    const std::wstring f = mods + L"\\override-" + std::to_wstring(i) + L".tss";
    if (!Exists(f) && !write(f, "<< Config >>\r\n")) return L"Could not write the online settings in " + mods;
  }
  SaveTextAtomically(written, list);
  SetEnvironmentVariableW(L"BB_ONLINE", L"1");
  SetEnvironmentVariableW(L"BB_NP_LOCAL", g_name.c_str());
  SetEnvironmentVariableW(L"BB_COOP_SERVER", server.c_str());
  return L"";
}

void LauncherLog(const std::string& text) {
  if (g_home.empty()) return;
  MakeDirs(g_home + L"\\logs");
  FILE* f = _wfopen((g_home + L"\\logs\\launcher.log").c_str(), L"ab");
  if (!f) return;
  SYSTEMTIME t;
  GetLocalTime(&t);
  std::fprintf(f, "%04d-%02d-%02d %02d:%02d:%02d  %s\r\n", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, text.c_str());
  std::fclose(f);
}

// Keeps the newest logs and crash dumps; older ones are deleted so the folder does not grow without end.
void PruneLogs(const wchar_t* pattern, size_t keep) {
  std::vector<std::pair<unsigned long long, std::wstring>> found;
  WIN32_FIND_DATAW data;
  const std::wstring dir = g_home + L"\\logs\\";
  HANDLE h = FindFirstFileW((dir + pattern).c_str(), &data);
  if (h == INVALID_HANDLE_VALUE) return;
  do {
    found.push_back({(static_cast<unsigned long long>(data.ftLastWriteTime.dwHighDateTime) << 32) | data.ftLastWriteTime.dwLowDateTime,
                     dir + data.cFileName});
  } while (FindNextFileW(h, &data));
  FindClose(h);
  std::sort(found.begin(), found.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
  for (size_t i = keep; i < found.size(); ++i) DeleteFileW(found[i].second.c_str());
}

void StartLauncherLog() {
  PruneLogs(L"driveclub_*.log", 25);
  PruneLogs(L"crash_*.dmp", 5);
  // One file for all starts; it starts over once it is big.
  WIN32_FILE_ATTRIBUTE_DATA info;
  const std::wstring path = g_home + L"\\logs\\launcher.log";
  if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &info) && info.nFileSizeLow > 256 * 1024) DeleteFileW(path.c_str());
  std::string version = "unknown";
  {
    std::ifstream in((g_home + L"\\version.txt").c_str());
    std::string line;
    if (std::getline(in, line)) {
      while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
      if (!line.empty()) version = line;
    }
  }
  LauncherLog("launcher started, release " + version + ", in folder " + Narrow(Leaf(g_home)));
  LauncherLog("system: " + Narrow(DescribeSystem()));
  LauncherLog("processor: " + Narrow(DescribeCpu()));
  const std::wstring home_lower = g_home;
  if (home_lower.find(L"\\Temp\\") != std::wstring::npos || home_lower.find(L"\\Rar$") != std::wstring::npos)
    LauncherLog("warning: the launcher runs from a temporary folder (unzip the download first)");
  if (home_lower.find(L"Program Files") != std::wstring::npos)
    LauncherLog("warning: the folder is under Program Files (settings and logs cannot be written there)");
  if (home_lower.find(L"OneDrive") != std::wstring::npos)
    LauncherLog("warning: the folder is synced by OneDrive (files can be locked while they sync)");
  for (wchar_t ch : g_home) {
    if (ch > 127) {
      LauncherLog("note: the folder path has non-ASCII characters");
      break;
    }
  }
}

// What a game folder looks like, for the log: the facts that decide whether the dump is the right one.
std::string FolderFacts(const char* label, const std::wstring& path) {
  std::string s = std::string(label) + ": " + (path.empty() ? "not chosen" : Narrow(Leaf(path)));
  if (path.empty() || !Exists(path)) return s + (path.empty() ? "" : " (folder missing)");
  WIN32_FILE_ATTRIBUTE_DATA info;
  if (GetFileAttributesExW((path + L"\\eboot.bin").c_str(), GetFileExInfoStandard, &info)) {
    unsigned char head[4] = {0, 0, 0, 0};
    FILE* f = _wfopen((path + L"\\eboot.bin").c_str(), L"rb");
    if (f) {
      if (std::fread(head, 1, 4, f) != 4) std::memset(head, 0, 4);
      std::fclose(f);
    }
    char text[80];
    std::snprintf(text, sizeof text, "; eboot.bin %llu bytes, starts %02X %02X %02X %02X",
                  (unsigned long long)info.nFileSizeLow + ((unsigned long long)info.nFileSizeHigh << 32), head[0], head[1], head[2], head[3]);
    s += text;
  } else {
    s += "; no eboot.bin";
  }
  const std::wstring sfo = path + L"\\sce_sys\\param.sfo";
  if (Exists(sfo)) {
    s += "; TITLE_ID " + SfoField(std::filesystem::path(sfo), "TITLE_ID") + ", APP_VER " + SfoField(std::filesystem::path(sfo), "APP_VER") +
         ", VERSION " + SfoField(std::filesystem::path(sfo), "VERSION");
  } else {
    s += "; no param.sfo";
  }
  s += Exists(path + L"\\game000.dat") ? "; game000.dat present" : "; no game000.dat";
  s += Exists(path + L"\\game.ndx") ? ", game.ndx present" : ", no game.ndx";
  return s;
}

// "" when the game can start. Otherwise a sentence for the player.
std::string g_preflight_note;  // something the player should know about, but not a reason to refuse

std::wstring Preflight() {
  g_preflight_note.clear();
  LauncherLog(FolderFacts("update folder", g_folder[0].path));
  LauncherLog(FolderFacts("base folder", g_folder[1].path));
  // The program files the release came with (antivirus programs sometimes remove some of them).
  {
    std::ifstream in((g_runtime + L"\\files.txt").c_str());
    std::string line;
    std::string missing;
    int count = 0;
    while (std::getline(in, line)) {
      while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
      if (line.empty()) continue;
      std::wstring name(line.begin(), line.end());
      // The DLSS files are optional (players on other graphics cards may delete them): the game then uses FSR.
      if (line.rfind("nvngx_", 0) == 0 || line.rfind("bbport_dlss", 0) == 0) continue;
      if (!Exists(g_runtime + L"\\" + name)) {
        if (++count <= 6) missing += (missing.empty() ? "" : ", ") + line;
      }
    }
    if (count) {
      LauncherLog("missing program files: " + missing + (count > 6 ? " and more" : ""));
      return L"Some program files are missing (" + std::wstring(missing.begin(), missing.end()) +
             (count > 6 ? L" and more" : L"") + L"). Unzip the download again; an antivirus program may have removed them.";
    }
  }
  // The folders for settings and logs must be writable.
  {
    MakeDirs(g_data + L"\\user");
    const std::wstring probe = g_data + L"\\user\\write_test.tmp";
    FILE* f = _wfopen(probe.c_str(), L"wb");
    if (!f) {
      LauncherLog("the data folder cannot be written");
      return L"The folder of the game cannot be written to. Move it somewhere you can write to (not Program Files).";
    }
    std::fclose(f);
    DeleteFileW(probe.c_str());
  }
  // The archive of the game is spread over many volume files.
  {
    unsigned used = 0;
    std::vector<unsigned> missing, truncated;
    const std::string problem = DatxCheckVolumes(g_folder[0].path, g_folder[1].path, used, missing, &truncated);
    if (problem.empty() && !truncated.empty()) {
      std::string list;
      for (size_t i = 0; i < truncated.size() && i < 8; ++i) {
        char name[24];
        std::snprintf(name, sizeof name, "game%03u.dat", truncated[i]);
        list += (list.empty() ? "" : ", ") + std::string(name);
      }
      LauncherLog("archive check: " + std::to_string(truncated.size()) + " volumes are shorter than the archive says (" + list + ")");
      return L"These game files are incomplete: " + std::wstring(list.begin(), list.end()) +
             (truncated.size() > 8 ? L" and more" : L"") +
             L". The copy of the game was cut short. Copy the game files again, then start the game.";
    }
    if (!problem.empty()) {
      LauncherLog("archive check: " + problem);
    } else if (!missing.empty()) {
      std::string list;
      for (size_t i = 0; i < missing.size() && i < 8; ++i) {
        char name[24];
        std::snprintf(name, sizeof name, "game%03u.dat", missing[i]);
        list += (list.empty() ? "" : ", ") + std::string(name);
      }
      // Not a reason to refuse: a dump without some volumes may still run, until the game reads from one.
      g_preflight_note = std::to_string(missing.size()) + " of " + std::to_string(used) + " archive volumes are missing (" + list +
                         "); the game can crash when it needs them";
      LauncherLog("archive check: " + g_preflight_note);
    } else {
      LauncherLog("archive check: all " + std::to_string(used) + " volumes present");
    }
  }
  return L"";
}

std::wstring g_game_log;  // the log file of the game started last

// BB_LOCK_HORIZON and BB_CAMERA_SHAKE, set on every start.
void SetLockHorizonEnvironment() {
  int level = 0, shake = 100;
  for (const Ctl& c : g_c) {
    if (c.patches[0] == "@lock_horizon") level = c.sel;
    if (c.patches[0] == "@camera_shake") shake = c.sel;
  }
  SetEnvironmentVariableW(L"BB_LOCK_HORIZON", std::to_wstring(level).c_str());
  SetEnvironmentVariableW(L"BB_CAMERA_SHAKE", std::to_wstring(shake).c_str());
}

// Starts the game. With `process` the caller gets the game's process handle (and closes it).
std::wstring StartGame(HANDLE* process = nullptr) {
  const std::wstring exe = GameExe();
  if (!Exists(exe)) return L"The game program (DriveClubGame.exe) was not found.";
  MakeDirs(g_home + L"\\logs");
  SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
  g_game_log = g_home + L"\\logs\\driveclub_" + Timestamp() + L".log";
  HANDLE log = CreateFileW(g_game_log.c_str(), GENERIC_WRITE, FILE_SHARE_READ,
                           &sa, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  MakeDirs(g_home + L"\\mods");
  const std::wstring prep = g_data + L"\\prep", user = g_data + L"\\user";
  MakeDirs(user);
  const std::wstring track_names = user + L"\\scene_tracks.tsv";
  const bool have_track_names = SceneTrackNames::Write(g_folder[0].path, g_folder[1].path, track_names);
  SetEnvironmentVariableW(L"BB_SCENE_TRACK_NAMES", have_track_names ? track_names.c_str() : nullptr);
  // Add-on content: changed copies of three data lists, built from the player's own archive. Files in the mods
  // folder come first, so a player's own versions win.
  std::wstring unlock_arg;
  bool unlock_on = false, unlock_all_on = false;
  for (const Ctl& c : g_c) {
    if (c.kind == Kind::Toggle && c.patches[0] == "@unlock") unlock_on = c.sel != 0;
    if (c.kind == Kind::Toggle && c.patches[0] == "@unlockall") unlock_all_on = c.sel != 0;
  }
  if (unlock_on || unlock_all_on) {
    const std::wstring unlock_dir = g_data + L"\\unlock";
    const std::wstring problem = BuildUnlock(g_folder[0].path, g_folder[1].path, unlock_dir, unlock_all_on);
    LauncherLog(problem.empty() ? std::string("add-on content unlock: ok") : "add-on content unlock: " + Narrow(problem));
    if (problem.empty()) unlock_arg = L" --mods \"" + unlock_dir + L"\"";
    else MessageBoxW(nullptr, (L"The add-on content could not be unlocked (" + problem + L"). The game starts without it.").c_str(), L"DriveClub", MB_OK | MB_ICONWARNING);
  }
  // Graphics detail: an edited copy of global.rpk, built from the player's own archive; none when all are default.
  std::wstring graphics_arg;
  {
    GraphicsChoice gc;
    // A row that is not shown keeps the game's value.
    const auto sel = [](GfxRow k, int game) { return g_gfx[k] >= 0 ? g_c[g_gfx[k]].sel : game; };
    gc.own_car = sel(kGfxCar, 0);
    gc.opponents = sel(kGfxOpp, 0);
    gc.scenery = sel(kGfxScenery, 0);
    gc.reflections = sel(kGfxSsr, 0);
    gc.shadows = sel(kGfxShadows, 0);
    gc.particles = sel(kGfxParticles, 0);
    gc.motion_blur = BlurLocked() ? 1 : sel(kGfxBlur, 1);
    gc.lens_dust = sel(kGfxDust, 1);
    gc.heat_haze = sel(kGfxHaze, 1);
    gc.fxaa = sel(kGfxFxaa, 1);
    gc.dynamic_res = sel(kGfxDynres, 1);
    const std::wstring graphics_dir = g_data + L"\\graphics";
    bool active = false;
    const std::wstring problem = BuildGraphics(g_folder[0].path, g_folder[1].path, graphics_dir, gc, active);
    LauncherLog(!problem.empty() ? "graphics detail: " + Narrow(problem)
                : active        ? std::string("graphics detail: ok")
                                : std::string("graphics detail: game defaults"));
    if (problem.empty() && active) graphics_arg = L" --mods \"" + graphics_dir + L"\"";
    else if (!problem.empty())
      MessageBoxW(nullptr, (L"The graphics detail settings could not be applied (" + problem + L"). The game starts with its own.").c_str(),
                  L"DriveClub", MB_OK | MB_ICONWARNING);
  }
  // Car select: the three showroom menu files with shorter blur animations, also from the player's archive.
  {
    const std::wstring carswitch_dir = g_data + L"\\carswitch";
    const std::wstring problem = BuildFastCarSwitch(g_folder[0].path, g_folder[1].path, carswitch_dir);
    LauncherLog(problem.empty() ? std::string("faster car switching: ok") : "faster car switching: " + Narrow(problem));
    if (problem.empty()) unlock_arg += L" --mods \"" + carswitch_dir + L"\"";
  }
  {
    const std::wstring profile_problem = SwitchProfile(SelectedProfile());
    if (!profile_problem.empty()) {
      LauncherLog("saves: " + Narrow(profile_problem));
      return profile_problem;
    }
    const std::wstring backup_problem = BackupSaves();  // one per start; the last ten are kept
    LauncherLog(backup_problem.empty() ? std::string("saves: backed up") : "saves: " + Narrow(backup_problem));
  }
  {
    const std::wstring online_problem = PrepareOnline(user);
    if (!online_problem.empty()) {
      LauncherLog("online: " + Narrow(online_problem));
      return online_problem;
    }
  }
  std::wstring cmd = L"\"" + exe + L"\" \"" + prep + L"\\boot-linked.bin\" --content-profile \"" + prep +
                     L"\\content.bin\" --patches \"" + prep + L"\\patches.bin\" --app0 \"" + g_folder[0].path +
                     L"\" --app0-base \"" + g_folder[1].path + L"\" --user \"" + user + L"\" --mods \"" + g_home + L"\\mods\"" +
                     unlock_arg + graphics_arg + L" --stub-unresolved";
  SetEnvironmentVariableW(L"BB_CAMERA_PATCH",g_c[g_camera_patch_row].sel ? L"1" : L"0");
  SetLockHorizonEnvironment();
  SetEnvironmentVariableW(L"BB_MOTION_BLUR_REFERENCE", MotionBlurReference());
  SetEnvironmentVariableW(L"BB_GPU_USER_DIR", user.c_str());
  SetEnvironmentVariableW(L"BB_PREFLIGHT_NOTE", g_preflight_note.empty() ? nullptr : std::wstring(g_preflight_note.begin(), g_preflight_note.end()).c_str());
  SetEnvironmentVariableW(L"BB_CRASH_DIR", (g_home + L"\\logs").c_str());
  // Programs that add themselves to the Vulkan loader (Steam, Epic, Discord, OBS, tuning tools) sit under a
  // part of the crashes players report, and the switchable-graphics layer of AMD laptops gives a black window
  // on some of them. "~implicit~" drops every layer that installs itself; the launcher row that names the
  // overlays back in keeps only the AMD one out.
  if (GetEnvironmentVariableW(L"VK_LOADER_LAYERS_DISABLE", nullptr, 0) == 0) {
    SetEnvironmentVariableW(L"VK_LOADER_LAYERS_DISABLE",
                            g_allow_overlays ? L"VK_LAYER_AMD_switchable_graphics"
                                             : L"~implicit~,VK_LAYER_AMD_switchable_graphics");
  }
  {
    // With the deltatime patch the game times itself by the wall clock, so each frame must be shown as soon as
    // it is finished; waiting for the next 60 Hz tick alternates 16 and 33 ms frames and shakes the camera.
    bool delta = false;
    for (const Ctl& c : g_c) {
      if (c.kind == Kind::Segmented && c.sel > 0 && c.sel < int(c.patches.size()) && c.patches[c.sel] == "60 FPS with deltatime") delta = true;
    }
    // Unlocked: the deltatime patch keeps the game speed right at any frame rate. Frames go out in step with the
    // display (Fifo) and the rate stays 3 below its refresh, so a variable refresh display (G-Sync, FreeSync) follows
    // each frame; Mailbox showed them on a fixed refresh, some for one refresh and some for two (stutter).
    // Unlimited: unlocked with no cap (1000; 0 would tie frames to the 60 Hz vblank) and Immediate presents, for
    // fixed refresh displays; past about 120 FPS the game's timing breaks (the Frame rate hint says so).
    const bool unlocked = g_fps >= 0 && g_c[g_fps].sel == 3, free = g_fps >= 0 && g_c[g_fps].sel == 4;
    const std::wstring cap = free ? L"1000" : g_fps_cap >= 0 ? std::to_wstring(g_c[g_fps_cap].sel) : DisplayHz() ? std::to_wstring(DisplayHz() - 3) : L"1000";
    if (!UserEnv(L"BB_FPS_LIMIT")) SetEnvironmentVariableW(L"BB_FPS_LIMIT", unlocked || free ? cap.c_str() : delta ? L"60" : nullptr);
    if (!UserEnv(L"BB_PRESENT_MODE")) SetEnvironmentVariableW(L"BB_PRESENT_MODE", unlocked ? L"Fifo" : free ? L"Immediate" : nullptr);
  }
  // A value set before the launcher started is the player's override and is kept. Checking the environment
  // itself is not enough: the launcher sets these for every start, so a second start (Play, or Run benchmark
  // again) would keep the first start's values and ignore every choice changed since.
  if (g_lang >= 0 && !UserEnv(L"BB_LANGUAGE")) {
    SetEnvironmentVariableW(L"BB_LANGUAGE", std::to_wstring(g_lang_id).c_str());
  }
  if (g_up >= 0 && !UserEnv(L"BB_AA_SCALE")) {
    const int u = UpscaleActive() ? g_c[g_up].sel : 0;
    SetEnvironmentVariableW(L"BB_AA_SCALE", u == 0 ? L"native" : u == 1 ? L"quality" : u == 2 ? L"balanced"
                                            : u == 3 ? L"performance" : L"ultra_performance");
  }
  if (g_hdr >= 0 && !UserEnv(L"BB_HDR")) {
    const bool hdr = g_c[g_hdr].sel != 0;
    SetEnvironmentVariableW(L"BB_HDR", hdr ? L"1" : nullptr);
    // The brightness is in hdr.ini (SaveSettings, or the game's Page Up / Page Down).
    SetEnvironmentVariableW(L"BB_HDR_PAPER", nullptr);
    SetEnvironmentVariableW(L"BB_HDR_PEAK", hdr ? std::to_wstring(g_c[g_hdr_peak].sel).c_str() : nullptr);
  }
  if (g_sharp >= 0 && !UserEnv(L"BB_FSR_SHARPNESS")) {  // FSR's RCAS, and the one after DLSS
    wchar_t v[16];
    swprintf(v, 16, L"%.2f", g_c[g_sharp].sel / 100.0);
    SetEnvironmentVariableW(L"BB_FSR_SHARPNESS", v);
  }
  if (g_aa >= 0 && !UserEnv(L"BB_AA")) {
    const int a = g_c[g_aa].sel;
    SetEnvironmentVariableW(L"BB_AA", a == 0 ? L"off" : a == 1 ? L"fsr3" : L"dlss");
  }
  if (g_fgen < 0 && !UserEnv(L"BB_FRAME_GEN")) SetEnvironmentVariableW(L"BB_FRAME_GEN", L"off");
  if (g_fgen >= 0 && !UserEnv(L"BB_FRAME_GEN")) {
    const int f = g_c[g_fgen].sel;
    SetEnvironmentVariableW(L"BB_FRAME_GEN", f == 0 ? L"off" : f == 1 ? L"fsr3" : L"dlss");
    if (!UserEnv(L"BB_FRAME_GEN_MULTIPLIER"))
      SetEnvironmentVariableW(L"BB_FRAME_GEN_MULTIPLIER", f == 2 ? L"2" : f == 3 ? L"3" : f == 4 ? L"4" : nullptr);
  }

  // Disable VK_LAYER_AMD_switchable_graphics_1 (from old AMD drivers) which hangs Vulkan init on hybrid laptops
  // AMD drivers and causes silent hangs at Vulkan device creation on
  // This writes to HKCU\Software\Microsoft\DirectX\UserGpuPreferences.
  if (!UserEnv(L"DISABLE_LAYER_AMD_SWITCHABLE_GRAPHICS_1")) {
    SetEnvironmentVariableW(L"DISABLE_LAYER_AMD_SWITCHABLE_GRAPHICS_1", L"1");
  }

  // Prefer discrete GPU via Windows GPU preference registry
  {
    const std::wstring exe = GameExe();
    if (Exists(exe)) {
      const std::wstring reg_path = L"Software\\Microsoft\\DirectX\\UserGpuPreferences";
      HKEY hkey;
      if (RegCreateKeyExW(HKEY_CURRENT_USER, reg_path.c_str(), 0, nullptr,
                          REG_OPTION_NON_VOLATILE, KEY_WRITE, nullptr, &hkey, nullptr) == ERROR_SUCCESS) {
        const std::wstring value = L"GpuPreference=2;"; // 2 = High performance (discrete GPU)
        RegSetValueExW(hkey, exe.c_str(), 0, REG_SZ,
                       reinterpret_cast<const BYTE*>(value.c_str()),
                       static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
        RegCloseKey(hkey);
      }
    }
  }

  if (g_aniso >= 0 && g_c[g_aniso].sel >= 1) {
    static const wchar_t* const kAniso[] = {L"", L"0", L"2", L"4", L"8", L"16"};
    SetEnvironmentVariableW(L"BB_ANISO", kAniso[g_c[g_aniso].sel]);
  }
  if (g_mirror >= 0) {
    SetEnvironmentVariableW(L"BB_MIRROR_RATE", g_c[g_mirror].sel == 1 ? L"0" : L"1");
    // Diagnostics only: DRIVECLUB_MIRROR_RATE=<n> overrides the choice above (0: the scene is drawn once and then kept).
    wchar_t forced[16];
    if (GetEnvironmentVariableW(L"DRIVECLUB_MIRROR_RATE", forced, 16) > 0) SetEnvironmentVariableW(L"BB_MIRROR_RATE", forced);
  }
  {
    // One line of the launcher's choices for the log (without the player name and folders).
    std::wstring version;
    {
      std::ifstream in((g_home + L"\\version.txt").c_str());
      std::string line;
      if (std::getline(in, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        version.assign(line.begin(), line.end());
      }
    }
    std::wstring summary = L"release " + (version.empty() ? std::wstring(L"unknown") : version) + L":";
    for (const auto& c : g_c) {
      if (c.patches.empty() || c.patches[0] == "@name" || c.patches[0] == "@onhost" || c.patches[0] == "@myip" || c.patches[0] == "@lang" || IsBenchRow(c) || c.sel < 0 ||
          c.kind == Kind::Button)
        continue;
      summary += L" ";
      summary += c.label;
      summary += L"=";
      summary += c.kind == Kind::Toggle   ? (c.sel ? L"on" : L"off")
                 : c.kind == Kind::Slider ? std::to_wstring(c.sel)
                                          : (size_t(c.sel) < c.items.size() ? c.items[size_t(c.sel)] : L"?");
      summary += L";";
    }
    SetEnvironmentVariableW(L"BB_SETTINGS_SUMMARY", summary.c_str());
  }
  SetEnvironmentVariableW(L"BB_USER_NAME", g_name.c_str());
  SetEnvironmentVariableW(L"BB_FULLSCREEN", full_screen_now() ? L"1" : L"0");
  SetEnvironmentVariableW(L"BB_TOGGLE_FILE", (user + L"\\toggles.txt").c_str());
  const int tier = MemoryTier();
  SetEnvironmentVariableW(L"BB_DMEM_MB", tier == 3 ? L"17056" : tier == 2 ? L"10056" : tier == 1 ? L"8056" : L"5056");
  SetEnvironmentVariableW(L"BB_CONFIG", (user + L"\\driveclub.ini").c_str());
  for (const auto& d : g_drive) {
    if (d.row >= 0 && !UserEnv(d.env)) SetEnvironmentVariableW(d.env, d.values[(std::max)(0, g_c[d.row].sel)]);
  }
  STARTUPINFOW si{sizeof(si)};
  si.dwFlags = STARTF_USESTDHANDLES;
  si.hStdOutput = log;
  si.hStdError = log;
  si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
  PROCESS_INFORMATION pi{};
  std::vector<wchar_t> buf(cmd.begin(), cmd.end());
  buf.push_back(L'\0');
  const std::wstring cwd = Dir(exe);
  const BOOL ok = CreateProcessW(exe.c_str(), buf.data(), nullptr, nullptr, TRUE, 0, nullptr, cwd.c_str(), &si, &pi);
  if (log != INVALID_HANDLE_VALUE) CloseHandle(log);
  if (!ok) {
    LauncherLog("CreateProcess failed, error " + std::to_string(GetLastError()));
    return L"Could not start the game.";
  }
  g_started_game_process_id = pi.dwProcessId;
  WriteExternalProgress(100, L"Loading DRIVECLUB...");
  CloseHandle(pi.hThread);
  if (process) *process = pi.hProcess;
  else CloseHandle(pi.hProcess);
  return L"";
}

void UpdateWorker() {
  LauncherLog("update: installing " + Narrow(g_update.version));
  std::wstring error;
  const bool ok = Updater::Install(g_update, g_home, [](int p) { SetProgress(p, L"Downloading " + g_update.version + L"..."); }, error);
  g_busy = false;
  if (ok) {
    LauncherLog("update: installed, restarting");
    if (g_hwnd) PostMessageW(g_hwnd, WM_APP + 2, 0, 0);  // close; the script copies the files and starts the new launcher
  } else {
    LauncherLog("update: " + Narrow(error));
    g_status = L"The update did not work: " + error;
    g_status_color = kWarn;
    if (g_hwnd) PostMessageW(g_hwnd, WM_APP + 1, 0, 0);
  }
}

void StartUpdate() {
  if (g_busy || !g_update_ready) return;
  g_busy = true;
  g_status.clear();
  SetProgress(0, L"Downloading " + g_update.version + L"...");
  std::thread(UpdateWorker).detach();
}

// Asks the Releases page once, in the background; the Update button shows when a newer version exists.
void StartUpdateCheck() {
  std::thread([] {
    std::wstring current;
    {
      std::ifstream in((g_home + L"\\version.txt").c_str());
      std::string line;
      if (in && std::getline(in, line)) current = Widen(line);
    }
    while (!current.empty() && (current.back() == L'\r' || current.back() == L' ')) current.pop_back();
    Updater::Release r;
    std::string why;
    if (!current.empty() && Updater::CheckNewer(current, r, why)) {
      g_update = r;
      g_update_ready = true;
      LauncherLog("update check: " + Narrow(r.version) + " is available");
      if (g_hwnd) PostMessageW(g_hwnd, WM_APP + 1, 0, 0);
    } else {
      LauncherLog(why.empty() ? "update check: version.txt is missing" : why);
    }
  }).detach();
}

void PlayWorker() {
  SetProgress(0, L"Preparing the game...");
  LauncherLog("Play pressed");
  std::wstring error = Preflight();
  if (error.empty()) error = RunPrepare();
  LauncherLog(error.empty() ? "preparation: ok" : "preparation: " + Narrow(error));
  if (error.empty()) {
    SetProgress(100, L"Starting DriveClub...");
    error = StartGame();
    LauncherLog(error.empty() ? "the game was started" : "start: " + Narrow(error));
  }
  g_busy = false;
  if (error.empty()) {
    if (g_hwnd) PostMessageW(g_hwnd, WM_APP + 2, 0, 0);  // close the launcher
  } else {
    g_status = error;
    g_status_color = kWarn;
    if (g_hwnd) PostMessageW(g_hwnd, WM_APP + 1, 0, 0);
  }
}

void Play() {
  if (g_busy || !FoldersOk()) return;
  if (GpuBlocked()) {
    g_status = g_gpu_problem;
    g_status_color = kBad;
    return;
  }
  if(!SaveSettings()) { g_status=g_settings_error;g_status_color=kBad;return; }
  g_busy = true;
  g_status.clear();
  std::thread(PlayWorker).detach();
}

// ------------------------------------------------------------------------------------------
// benchmark: runs the game with BB_BENCHMARK, waits for it and shows data\user\benchmark\result.txt
// ------------------------------------------------------------------------------------------
std::wstring BenchResultPath() { return g_data + L"\\user\\benchmark\\result.txt"; }

// The BB_BENCHMARK value for the choices on the page (keys: src/runtime_benchmark.c).
std::string BenchSpec() {
  char spec[400];
  std::snprintf(spec, sizeof spec, "track=%s,car=%s,mode=%s,tod=%d,weather=%s,rain=%s,laps=20,warmup=5,seconds=%d,timeout=420",
                kBenchTracks[g_bench_track].id, kBenchCars[g_bench_car].id, g_c[g_bmode].sel == 1 ? "Race" : "TimeTrial",
                kBenchTod[g_c[g_btod].sel], kBenchWeather[g_c[g_bweather].sel], kBenchRain[g_c[g_brain].sel],
                kBenchSeconds[g_c[g_blen].sel]);
  return spec;
}

// result.txt is one line of key=value pairs (runtime_benchmark.c, finish()).
void LoadBenchResult() {
  g_bench_result = BenchResult{};
  const auto lines = ReadLines(BenchResultPath());
  if (lines.empty()) return;
  auto value = [&](const char* key) -> std::string {
    const std::string& l = lines[0];
    const std::string k = std::string(key) + "=";
    for (size_t p = l.find(k); p != std::string::npos; p = l.find(k, p + 1)) {
      if (p != 0 && l[p - 1] != ' ') continue;
      const size_t b = p + k.size(), e = l.find(' ', b);
      return l.substr(b, e == std::string::npos ? std::string::npos : e - b);
    }
    return "";
  };
  auto one = [](const std::string& v) {
    char b[32];
    std::snprintf(b, sizeof b, "%.1f", std::atof(v.c_str()));
    return Widen(b);
  };
  if (value("avg_fps").empty()) return;
  g_bench_result.ok = true;
  g_bench_result.fps = one(value("avg_fps"));
  g_bench_result.lows = L"1% low " + one(value("low1_fps")) + L"    0.1% low " + one(value("low01_fps"));
  const std::string weather = value("weather");
  std::string rain = value("rain");  // the game's preset name with '_' for each space
  std::replace(rain.begin(), rain.end(), '_', ' ');
  std::wstring about = BenchName(kBenchTracks, kBenchTrackCount, value("track")) + L", " +
                       BenchName(kBenchCars, kBenchCarCount, value("car")) + (value("mode") == "Race" ? L", race" : L"") + L", " +
                       Widen(value("tod")) + L":00 " + Widen(weather) + (rain.empty() || rain == "dry" ? L"" : L", rain " + Widen(rain)) +
                       L", " + Widen(std::to_string(int(std::atof(value("seconds").c_str()) + 0.5))) + L" s measured";
  WIN32_FILE_ATTRIBUTE_DATA info;
  if (GetFileAttributesExW(BenchResultPath().c_str(), GetFileExInfoStandard, &info)) {
    FILETIME local;
    SYSTEMTIME t;
    FileTimeToLocalFileTime(&info.ftLastWriteTime, &local);
    FileTimeToSystemTime(&local, &t);
    wchar_t when[40];
    wsprintfW(when, L", %04d-%02d-%02d %02d:%02d", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute);
    about += when;
  }
  g_bench_result.about = about;

  // The second line: what the game itself ran with ("ran key=value ...", runtime_benchmark.c ran_with()).
  g_bench_ran_short.clear();
  g_bench_ran_lines.clear();
  if (lines.size() < 2 || lines[1].rfind("ran ", 0) != 0) return;
  auto ran = [&](const char* key) -> std::string {
    const std::string& l = lines[1];
    const std::string k = std::string(" ") + key + "=";
    const size_t p = l.find(k);
    if (p == std::string::npos) return "";
    const size_t b = p + k.size(), e = l.find(' ', b);
    return l.substr(b, e == std::string::npos ? std::string::npos : e - b);
  };
  const std::string fps = ran("fps_limit"), aa = ran("aa"), scale = ran("aa_scale"), fgen = ran("frame_gen");
  const int limit = std::atoi(fps.c_str());
  const std::wstring cap = fps == "none" || limit >= 1000 ? L"no frame cap" : Widen(fps) + L" FPS cap";
  const std::wstring aa_name = aa == "dlss" ? L"DLAA" : aa == "fsr3" ? L"FSR 3.1" : L"game AA";
  const std::wstring up_name = scale.empty() || scale == "native" ? L"" : L" (" + Widen(scale) + L" upscaling)";
  const std::wstring fg_name = fgen.empty() || fgen == "off" ? L"off" : Widen(fgen) + L" " + Widen(ran("frame_gen_x")) + L"x";
  const bool live = ran("mirror_rate") != "0";
  auto onoff = [&](const char* key) { return ran(key) == "1" ? std::wstring(L"on") : std::wstring(L"off"); };
  std::wstring aniso = Widen(ran("aniso"));
  if (aniso != L"auto") aniso += L"x";
  g_bench_ran_short = L"Ran at " + Widen(ran("render")) + L" \x2192 " + Widen(ran("output")) + L", " + cap + L", " + aa_name +
                      L". Point here for every setting.";
  g_bench_ran_lines = {
      L"What the game ran with (read by the game itself)",
      L"Render resolution (the 3D scene): " + Widen(ran("render")),
      L"Output resolution (the finished picture): " + Widen(ran("output")),
      L"The window is always your screen's size; dynamic resolution can lower the render size further under load.",
      L"Frame limit: " + cap + L", vblank " + Widen(ran("vblank_hz")) + L" Hz, present mode " + Widen(ran("present")),
      L"Anti-aliasing: " + aa_name + up_name + L", frame generation " + fg_name,
      L"Texture filtering: " + aniso + L", mirrors and reflections: " + (live ? L"live" : L"static"),
      L"Your car full detail: " + onoff("car_detail") + L", other cars full detail: " + onoff("opponent_detail"),
      L"More scenery detail: " + onoff("scenery_detail") + L", high-quality reflections: " + onoff("hq_reflections"),
      L"Dynamic resolution: " + onoff("dynamic_res") + L", add-on content: " + (ran("dlc") == "1" ? L"unlocked" : L"not unlocked"),
  };
}

// The game's own reason when it stopped the benchmark ("Benchmark: FAILED, ..." in its log), or "".
// The game's output can interleave with other lines, so the text is searched for anywhere in a line.
std::wstring BenchFailure() {
  std::string why;
  for (const auto& l : ReadLines(g_game_log)) {
    const size_t p = l.find("Benchmark: FAILED, ");
    if (p == std::string::npos) continue;
    const size_t b = p + 19, e = l.find(" (", b);  // without the note for developers in brackets
    why = l.substr(b, e == std::string::npos ? std::string::npos : e - b);
  }
  return Widen(why);
}

// When result.txt was last written (0 = not there).
unsigned long long ResultTime() {
  WIN32_FILE_ATTRIBUTE_DATA info;
  if (!GetFileAttributesExW(BenchResultPath().c_str(), GetFileExInfoStandard, &info)) return 0;
  return (static_cast<unsigned long long>(info.ftLastWriteTime.dwHighDateTime) << 32) | info.ftLastWriteTime.dwLowDateTime;
}

void BenchWorker() {
  SetProgress(0, L"Preparing the game...");
  LauncherLog("Run benchmark pressed: " + BenchSpec());
  std::wstring error = Preflight();
  if (error.empty()) error = RunPrepare();
  HANDLE game = nullptr;
  unsigned long long before = 0;
  if (error.empty()) {
    SetProgress(100, L"Benchmark running. The game closes by itself when it is done.");
    before = ResultTime();  // the last result stays on show if this run fails
    const std::wstring spec = Widen(BenchSpec());
    SetEnvironmentVariableW(L"BB_BENCHMARK", spec.c_str());
    SetEnvironmentVariableW(L"BB_BENCHMARK_OUT", BenchResultPath().c_str());
    error = StartGame(&game);
    SetEnvironmentVariableW(L"BB_BENCHMARK", nullptr);  // a later Play is an ordinary game
    SetEnvironmentVariableW(L"BB_BENCHMARK_OUT", nullptr);
  }
  DWORD code = 1;
  if (error.empty() && game) {
    WaitForSingleObject(game, INFINITE);
    GetExitCodeProcess(game, &code);
    CloseHandle(game);
    if (code == 0 && ResultTime() > before) {
      const auto lines = ReadLines(BenchResultPath());
      LauncherLog("benchmark result: " + (lines.empty() ? std::string("(empty)") : lines[0]));
    } else {
      std::wstring why = BenchFailure();
      if (why.empty() && code == 3) why = L"the race did not start, or the game stopped drawing";
      error = why.empty() ? L"The benchmark stopped before the end: the game was closed or crashed. The log is in the logs folder."
                          : L"The benchmark failed: " + why + L". The log is in the logs folder.";
      LauncherLog("benchmark: exit code " + std::to_string(code) + ", " + Narrow(error));
    }
  } else {
    LauncherLog("benchmark: " + Narrow(error));
  }
  g_bench_running = false;
  g_busy = false;
  if (!error.empty()) {
    g_status = error;
    g_status_color = kWarn;
  } else {
    g_status = L"Benchmark done.";
    g_status_color = kOk;
  }
  if (g_hwnd) PostMessageW(g_hwnd, WM_APP + 3, 0, 0);  // show the result
}

void RunBenchmark() {
  if (g_busy || !FoldersOk()) return;
  if (GpuBlocked()) {
    g_status = g_gpu_problem;
    g_status_color = kBad;
    return;
  }
  if(!SaveSettings()) { g_status=g_settings_error;g_status_color=kBad;return; }
  g_busy = true;
  g_bench_running = true;
  g_status.clear();
  std::thread(BenchWorker).detach();
}

// The menu of tracks or cars, grouped by location or class. Returns the chosen index or -1.
int BenchMenu(HWND h, const RectF& at, const BenchChoice* list, int count, int current) {
  HMENU menu = CreatePopupMenu();
  std::vector<std::pair<std::wstring, HMENU>> groups;
  for (int i = 0; i < count; ++i) {
    auto it = std::find_if(groups.begin(), groups.end(), [&](const auto& g) { return g.first == list[i].group; });
    if (it == groups.end()) {
      groups.push_back({list[i].group, CreatePopupMenu()});
      it = groups.end() - 1;
    }
    AppendMenuW(it->second, MF_STRING | (i == current ? MF_CHECKED : 0), UINT_PTR(1 + i), list[i].name);
  }
  for (const auto& g : groups) {
    const bool has_current = current >= 0 && g.first == list[current].group;
    AppendMenuW(menu, MF_POPUP | (has_current ? MF_CHECKED : 0), UINT_PTR(g.second), g.first.c_str());
  }
  POINT pt{LONG(at.X), LONG(at.Y + at.Height)};
  ClientToScreen(h, &pt);
  const int pick = int(TrackPopupMenu(menu, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN | TPM_NONOTIFY, pt.x, pt.y, 0, h, nullptr));
  DestroyMenu(menu);  // and its submenus
  return pick - 1;
}

// ------------------------------------------------------------------------------------------
// folder picker
// ------------------------------------------------------------------------------------------
void BrowseFolder(int which) {
  BROWSEINFOW bi{};
  bi.hwndOwner = g_hwnd;
  bi.lpszTitle = which == 0 ? L"Choose the DriveClub v1.28 update folder (it contains eboot.bin)"
                            : L"Choose the DriveClub base game folder (it contains game000.dat)";
  bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE | BIF_USENEWUI;
  PIDLIST_ABSOLUTE pidl = SHBrowseForFolderW(&bi);
  if (!pidl) return;
  wchar_t path[MAX_PATH];
  if (SHGetPathFromIDListW(pidl, path)) {
    g_folder[which].path = path;
    CheckFolders();
    if (!g_folder[which].ok) {
      g_status = FolderProblem(which);
      g_status_color = kBad;
    } else {
      g_status.clear();
    }
    g_changed = true;
  }
  CoTaskMemFree(pidl);
}

// ------------------------------------------------------------------------------------------
// window
// ------------------------------------------------------------------------------------------
void CommitName() {
  if (!g_edit) return;
  wchar_t buf[32] = {};
  GetWindowTextW(g_edit, buf, 31);
  DestroyWindow(g_edit);
  g_edit = nullptr;
  std::wstring n;
  if (g_edit_host) {
    for (wchar_t ch : std::wstring(buf)) if ((ch >= L'0' && ch <= L'9') || ch == L'.') n += ch;
    if (n != g_online_host) { g_online_host = n; g_changed = true; }
    g_c[g_online_host_row].items[0] = g_online_host.empty() ? L"(not set)" : g_online_host;
    return;
  }
  for (wchar_t ch : std::wstring(buf)) if (ch >= 32 && ch < 127) n += ch;
  if (!n.empty() && n != g_name) { g_name = n; g_changed = true; }
  for (auto& c : g_c) if (c.patches[0] == "@name") c.items[0] = g_name;
}

void StartNameEdit(HWND parent, const Ctl& c) {
  const RectF r = Scaled(c.ctl);
  g_edit_host = c.patches[0] == "@onhost";
  g_edit = CreateWindowExW(0, L"EDIT", g_edit_host ? g_online_host.c_str() : g_name.c_str(), WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, int(r.X + 6), int(r.Y + 6),
                           int(r.Width - 12), int(r.Height - 12), parent, nullptr, GetModuleHandleW(nullptr), nullptr);
  SendMessageW(g_edit, EM_LIMITTEXT, 20, 0);
  SendMessageW(g_edit, WM_SETFONT, (WPARAM)GetStockObject(DEFAULT_GUI_FONT), TRUE);
  SendMessageW(g_edit, EM_SETSEL, 0, -1);
  SetFocus(g_edit);
}

int HitControl(float x, float y, int* seg) {
  if (y >= FooterTop()) { *seg = -1; return -1; }
  *seg = -1;
  for (size_t i = 0; i < g_c.size(); ++i) {
    Ctl& c = g_c[i];
    if (RowHidden(c)) continue;
    RectF r = Scaled(c.row);
    if (x < r.X || x > r.X + r.Width || y < r.Y || y > r.Y + r.Height) continue;
    if (c.kind == Kind::Action) {
      const RectF ct = Scaled(c.ctl);
      if (x >= ct.X && x <= ct.X + ct.Width && y >= ct.Y && y <= ct.Y + ct.Height) *seg = 0;  // over the Run button
    }
    if (c.kind == Kind::Segmented) {
      RectF ct = Scaled(c.ctl);
      if (x >= ct.X && x <= ct.X + ct.Width && y >= ct.Y && y <= ct.Y + ct.Height) {
        *seg = std::clamp(int((x - ct.X) / (ct.Width / float(c.items.size()))), 0, int(c.items.size()) - 1);
      }
    }
    return int(i);
  }
  return -1;
}

int HitTab(float x, float y) {
  if (y >= FooterTop()) return -1;
  for (int t = 0; t < kTabCount; ++t) {
    RectF r = Scaled(g_tab_rect[t]);
    if (x >= r.X && x <= r.X + r.Width && y >= r.Y && y <= r.Y + r.Height) return t;
  }
  for (int s = 0; HasSubTabs() && s < kSubCount; ++s) {
    RectF r = Scaled(g_sub_rect[s]);
    if (x >= r.X && x <= r.X + r.Width && y >= r.Y && y <= r.Y + r.Height) return kTabCount + s;
  }
  return -1;
}

int HitButton(float x, float y) {
  if (g_update_ready && !g_busy) {
    const float content_oy0 = g_oy;
    g_oy = FooterOy();
    const RectF u = Scaled(g_update_btn);
    g_oy = content_oy0;
    if (x >= u.X && x <= u.X + u.Width && y >= u.Y && y <= u.Y + u.Height) return 3;
  }
  const float content_oy = g_oy;
  g_oy = FooterOy();
  struct Restore { float v; ~Restore() { g_oy = v; } } restore{content_oy};
  for (int i = 0; i < 3; ++i) {
    RectF r = Scaled(g_btn[i]);
    if (x >= r.X && x <= r.X + r.Width && y >= r.Y && y <= r.Y + r.Height) return i;
  }
  return -1;
}

int HitBrowse(float x, float y) {
  if (y >= FooterTop()) return -1;
  for (int i = 0; i < 2; ++i) {
    RectF r = Scaled(g_folder[i].browse);
    if (x >= r.X && x <= r.X + r.Width && y >= r.Y && y <= r.Y + r.Height) return i;
  }
  return -1;
}

// The slider value under the pointer.
int SliderValueAt(const Ctl& c, float x) {
  const RectF ct = Scaled(c.ctl);
  const float x0 = ct.X + 8 * g_scale, width = ct.Width - 16 * g_scale;
  const float t = width > 0 ? (std::clamp)((x - x0) / width, 0.f, 1.f) : 0.f;
  return c.min + int(std::lround(t * float(c.max - c.min)));
}
void SetSlider(int i, int v) {
  Ctl& c = g_c[size_t(i)];
  v = (std::clamp)(v, c.min, c.max);
  if (v == c.sel) return;
  c.sel = v;
  DeriveWheelPreset();
  g_changed = true;
}
// Rows shown or hidden (the force feedback model changed): the page laid out again at the same scale, as a tab switch
// does (a taller page scrolls; fitting it to the window again would shrink everything).
void Relayout(HWND h) {
  Layout();
  g_scroll = (std::min)(g_scroll, MaxScroll());
  g_oy = ContentOy();
  InvalidateRect(h, nullptr, FALSE);
}

void UpdateMotionBlurTooltip(HWND window) {
  if (!g_motion_blur_tooltip || g_motion_blur < 0) return;
  const RectF box = Scaled(g_c[g_motion_blur].ctl);
  for (int choice = 0; choice < 2; ++choice) {
    TOOLINFOW tool{}; tool.cbSize = sizeof(tool); tool.hwnd = window; tool.uId = choice + 1;
    if (g_tab == kTabDetail && !g_busy) {
      const float left = box.X + choice * box.Width / 2;
      tool.rect = {LONG(left), LONG(box.Y), LONG(left + box.Width / 2), LONG(box.Y + box.Height)};
    }
    SendMessageW(g_motion_blur_tooltip, TTM_NEWTOOLRECTW, 0, reinterpret_cast<LPARAM>(&tool));
  }
}

LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
  switch (m) {
    case WM_TIMER: {
      if (w == kFfbCaptureTimer) {
        PollFfbCapture(h);
        return 0;
      }
      if (w == kBindCaptureTimer) {
        PollBindCapture(h);
        return 0;
      }
      if (w == kMonitorTimer) {
        RefreshMonitorRows(h);
        return 0;
      }
      break;
    }
    case WM_KEYDOWN: {
      if (g_ffb_capture >= 0 && (w == VK_ESCAPE || w == VK_DELETE || w == VK_BACK)) {  // FFB gain + / - waits
        if (w == VK_ESCAPE) {
          StopFfbCapture(h);
          g_status = L"Wheel button unchanged.";
          g_status_color = kMuted;
          g_status_sticky = true;
        } else {
          SetFfbGainButton(h, g_ffb_capture, -1);
        }
        InvalidateRect(h, nullptr, FALSE);
        return 0;
      }
      if (w == VK_ESCAPE && g_info_open >= 0) {
        g_info_open = -1;
        InvalidateRect(h, nullptr, FALSE);
        return 0;
      }
      // The arrow keys step the slider under the pointer (Shift: by 10).
      if (g_hover_ctl < 0 || g_c[size_t(g_hover_ctl)].kind != Kind::Slider || g_busy) break;
      const int step = (GetKeyState(VK_SHIFT) & 0x8000) ? 10 : 1;
      const int dir = w == VK_RIGHT || w == VK_UP ? 1 : w == VK_LEFT || w == VK_DOWN ? -1 : 0;
      if (!dir) break;
      SetSlider(g_hover_ctl, g_c[size_t(g_hover_ctl)].sel + dir * step);
      InvalidateRect(h, nullptr, FALSE);
      return 0;
    }
    case WM_SIZE: {
      // The layout keeps its scale and reflows: wider windows get wider columns, taller ones more room.
      if (w == SIZE_MINIMIZED || g_scale <= 0.f) return 0;
      g_client_h = float(HIWORD(l));
      {
        // Everything shrinks to fit a window that is too short (more columns open up as it does),
        // down to 60%; below that the content scrolls.
        const float cw = float(LOWORD(l));
        float s = g_dpi;
        for (int i = 0; i < 12; ++i) {
          g_scale = s;
          // The layout fills the window at any scale: a window shrunk to fit its height gets wider columns,
          // not an empty strip on the right (the window itself is at most kMaxW wide at full scale).
          g_win_w = (std::max)(kWinW, cw / s);
          g_win_h = g_client_h / s;
          Layout();
          const float need = TotalHeight() * s;
          if (need <= g_client_h + 0.5f || s <= g_dpi * 0.6f + 0.001f) break;
          s = (std::max)(g_dpi * 0.6f, s * g_client_h / need);
        }
      }
      g_scroll = (std::min)(g_scroll, MaxScroll());
      g_oy = ContentOy();
      InvalidateRect(h, nullptr, FALSE);
      return 0;
    }
    case WM_GETMINMAXINFO: {
      RECT wr, cr;
      GetWindowRect(h, &wr);
      GetClientRect(h, &cr);
      const int ncw = (wr.right - wr.left) - cr.right, nch = (wr.bottom - wr.top) - cr.bottom;
      auto* mm = reinterpret_cast<MINMAXINFO*>(l);
      mm->ptMinTrackSize.x = LONG(kWinW * g_dpi * 0.6f) + ncw;
      mm->ptMinTrackSize.y = LONG((kHeaderH + kTabH + kSubTabH + kFooterH + 160.f) * g_dpi * 0.6f) + nch;  // shorter: the content scrolls
      mm->ptMaxTrackSize.x = LONG(kMaxW * g_dpi) + ncw;
      return 0;
    }
    case WM_DPICHANGED: {
      // Moved to a screen with another scale: take the size Windows suggests and draw at that scale.
      g_dpi = g_scale = float(HIWORD(w)) / 96.f;
      const RECT* r = reinterpret_cast<const RECT*>(l);
      SetWindowPos(h, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
      InvalidateRect(h, nullptr, FALSE);
      return 0;
    }
    case WM_PAINT: {
      PAINTSTRUCT ps;
      HDC dc = BeginPaint(h, &ps);
      RECT rc;
      GetClientRect(h, &rc);
      HDC mem = CreateCompatibleDC(dc);
      HBITMAP bmp = CreateCompatibleBitmap(dc, rc.right, rc.bottom);
      HGDIOBJ old = SelectObject(mem, bmp);
      Paint(mem, rc.right, rc.bottom);
      BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
      SelectObject(mem, old);
      DeleteObject(bmp);
      DeleteDC(mem);
      EndPaint(h, &ps);
      return 0;
    }
    case WM_ERASEBKGND:
      return 1;
    case WM_MOUSEWHEEL: {
      const float before = g_scroll;
      g_scroll -= float(GET_WHEEL_DELTA_WPARAM(w)) / WHEEL_DELTA * 60.f * g_scale;
      g_scroll = (std::max)(0.f, (std::min)(g_scroll, MaxScroll()));
      if (g_scroll != before) {
        CommitName();  // the name box is a child window placed at the old position
        g_oy = ContentOy();
        InvalidateRect(h, nullptr, FALSE);
      }
      return 0;
    }
    case WM_MOUSEMOVE: {
      const float x = float(GET_X_LPARAM(l)), y = float(GET_Y_LPARAM(l));
      if (g_drag >= 0) {
        SetSlider(g_drag, SliderValueAt(g_c[size_t(g_drag)], x));
        InvalidateRect(h, nullptr, FALSE);
        return 0;
      }
      int seg;
      const int c = HitControl(x, y, &seg), b = HitButton(x, y), br = HitBrowse(x, y), tab = HitTab(x, y);
      // Over the benchmark's result (its row below the Run button): the panel of settings the game ran with.
      bool ran_hover = false;
      if (c >= 0 && c == g_brun && seg != 0) {
        const RectF row = Scaled(g_c[c].row);
        ran_hover = y >= row.Y + 88 * g_scale;
      }
      if (ran_hover != g_bench_ran_hover) {
        g_bench_ran_hover = ran_hover;
        InvalidateRect(h, nullptr, FALSE);
      }
      // the (i) under the pointer lights up
      int info = -1;
      for (size_t i = 0; i < g_c.size(); ++i) {
        const RectF& r = g_c[i].info;
        const float m = 3 * g_scale;
        if (r.Width > 0 && x >= r.X - m && x <= r.X + r.Width + m && y >= r.Y - m && y <= r.Y + r.Height + m) info = int(i);
      }
      if (info != g_hover_info) {
        g_hover_info = info;
        InvalidateRect(h, nullptr, FALSE);
      }
      if (c != g_hover_ctl || seg != g_hover_seg || b != g_hover_btn || br != g_hover_browse || tab != g_hover_tab) {
        g_hover_ctl = c;
        g_hover_seg = seg;
        g_hover_btn = b;
        g_hover_browse = br;
        g_hover_tab = tab;
        InvalidateRect(h, nullptr, FALSE);
      }
      if (const bool on_discord = HitDiscord(x, y); on_discord != g_hover_discord) {
        g_hover_discord = on_discord;
        InvalidateRect(h, nullptr, FALSE);
      }
      UpdateMotionBlurTooltip(h);
      TRACKMOUSEEVENT t{sizeof(t), TME_LEAVE, h, 0};
      TrackMouseEvent(&t);
      return 0;
    }
    case WM_MOUSELEAVE:
      g_hover_ctl = g_hover_seg = g_hover_btn = g_hover_browse = g_hover_tab = -1;
      g_hover_info = -1;
      g_hover_discord = false;
      g_bench_ran_hover = false;
      InvalidateRect(h, nullptr, FALSE);
      return 0;
    case WM_SETCURSOR:
      if (g_hover_discord && LOWORD(l) == HTCLIENT) {
        SetCursor(LoadCursorW(nullptr, IDC_HAND));
        return TRUE;
      }
      break;
    case WM_LBUTTONDOWN: {
      const float x = float(GET_X_LPARAM(l)), y = float(GET_Y_LPARAM(l));
      if (g_busy) return 0;
      if (HitDiscord(x, y)) {
        // Where no browser can be opened the link goes to the clipboard. Under Wine and Proton ShellExecute reports success even
        // when xdg-open is missing and nothing opens, so there the link is copied and shown every time.
        const bool opened = reinterpret_cast<INT_PTR>(ShellExecuteW(h, L"open", kDiscordUrl, nullptr, nullptr, SW_SHOWNORMAL)) > 32;
        const bool wine = GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "wine_get_version") != nullptr;
        if (!opened || wine) {
          bool copied = false;
          if (OpenClipboard(h)) {
            const size_t bytes = (wcslen(kDiscordUrl) + 1) * sizeof(wchar_t);
            if (HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, bytes)) {
              if (void* p = GlobalLock(mem)) {
                memcpy(p, kDiscordUrl, bytes);
                GlobalUnlock(mem);
                EmptyClipboard();
                copied = SetClipboardData(CF_UNICODETEXT, mem) != nullptr;
              }
              if (!copied) GlobalFree(mem);
            }
            CloseClipboard();
          }
          const wchar_t* lead = opened ? L"Discord opens in your browser. The link is also " : L"The browser could not be opened. The link is ";
          g_status = std::wstring(lead) + (copied ? L"copied: " : L"here: ") + kDiscordUrl;
          g_status_color = copied || opened ? kOk : kWarn;
          g_status_sticky = true;
          InvalidateRect(h, nullptr, FALSE);
        }
        return 0;
      }
      CommitName();
      g_status_sticky = false;
      // the details panel: a click anywhere closes it; a click on an (i) opens it
      if (g_info_open >= 0) {
        g_info_open = -1;
        InvalidateRect(h, nullptr, FALSE);
        return 0;
      }
      if (g_hover_info >= 0) {
        g_info_open = g_hover_info;
        InvalidateRect(h, nullptr, FALSE);
        return 0;
      }
      if (const int t = HitTab(x, y); t >= 0 && t != g_tab && t != kTabCount + g_subtab) {
        if (t < kTabCount) g_tab = t;
        else g_subtab = t - kTabCount;
        Layout();  // the page has its own height
        g_scroll = (std::min)(g_scroll, MaxScroll());
        g_oy = ContentOy();
        InvalidateRect(h, nullptr, FALSE);
        return 0;
      }
      const int br = HitBrowse(x, y);
      if (br >= 0) {
        BrowseFolder(br);
        InvalidateRect(h, nullptr, FALSE);
        return 0;
      }
      g_press_btn = HitButton(x, y);
      int seg;
      const int c = HitControl(x, y, &seg);
      if (c >= 0) {
        Ctl& ctl = g_c[c];
        if (ctl.patches[0].rfind("@dev:", 0) == 0) {   // double-click: read that pad
          const ULONGLONG now = GetTickCount64();
          const bool again = g_dev_click_row == c && now - g_dev_click_at < 600;
          g_dev_click_row = c;
          g_dev_click_at = now;
          if (again) {
            g_read_slot = atoi(ctl.patches[0].c_str() + 5);
            g_xbox_names = DetectXboxNaming();   // the pad's kind names the buttons and picks the template
            // Double-click: read that pad and put its template in place (the pad half only).
            const bool filled = WritePadTemplate();
            if (filled) RefreshBindingChips();
            SyncDevices(false);
            g_status = L"Reading the pad in XInput slot " + std::to_wstring(g_read_slot + 1) + L".";
            if (filled) g_status += g_xbox_names ? L" The Xbox pad template was filled in."
                                                 : L" The PlayStation pad template was filled in.";
            g_status_color = kOk;
            g_status_sticky = true;
          }
          InvalidateRect(h, nullptr, FALSE);
          return 0;
        }
        if (ctl.patches[0] == "@name" || ctl.patches[0] == "@onhost") {          StartNameEdit(h, ctl);
          return 0;
        }
        if (ctl.kind == Kind::Picker) {
          const bool track = c == g_btrack;
          const int pick = BenchMenu(h, Scaled(ctl.ctl), track ? kBenchTracks : kBenchCars, track ? kBenchTrackCount : kBenchCarCount,
                                     track ? g_bench_track : g_bench_car);
          if (pick >= 0) {
            (track ? g_bench_track : g_bench_car) = pick;
            ShowBenchChoices();
            g_changed = true;
          }
          InvalidateRect(h, nullptr, FALSE);
          return 0;
        }
        if (ctl.kind == Kind::Action) {
          if (seg == 0) RunBenchmark();
          InvalidateRect(h, nullptr, FALSE);
          return 0;
        }
        if (ctl.kind == Kind::Slider) {
          g_drag = c;
          SetSlider(c, SliderValueAt(ctl, x));
          SetCapture(h);
          InvalidateRect(h, nullptr, FALSE);
          return 0;
        }
        if (ctl.kind == Kind::Monitor) {
          ctl.items[0] = MonitorText();
          InvalidateRect(h, nullptr, FALSE);
          return 0;
        }
        if (ctl.kind == Kind::Binding) {
          const RectF ct = Scaled(ctl.ctl);
          const bool right = x > ct.X + ct.Width * 0.55f;   // the right chip belongs to the pad
          if (right && ctl.patches[0].size() > 3 &&
              ctl.patches[0].compare(ctl.patches[0].size() - 3, 3, "@ro") == 0) {
            g_status = L"That control is read as an axis and cannot be remapped.";
            g_status_color = kMuted;
            g_status_sticky = true;
            InvalidateRect(h, nullptr, FALSE);
            return 0;
          }
          StartBindCapture(h, c, right ? 1 : 0);
          InvalidateRect(h, nullptr, FALSE);
          return 0;
        }
        if (ctl.kind == Kind::Button) {
          const RectF ct = Scaled(ctl.ctl);

          if (c == g_wheel_setup && y >= ct.Y && y <= ct.Y + ct.Height) OpenWheelSetup();
          if ((c == g_save_backup_row || c == g_save_restore_row) && y >= ct.Y && y <= ct.Y + ct.Height) {
            std::wstring message;
            const bool ok = RunSaveButton(c == g_save_restore_row, message);
            g_status = message;
            g_status_color = ok ? kOk : kBad;
          }
          for (int k = 0; k < 2; ++k)
            if (c == g_ffb_gain_row[k] && y >= ct.Y && y <= ct.Y + ct.Height) StartFfbCapture(h, k);
          InvalidateRect(h, nullptr, FALSE);
          return 0;
        }
        if (ctl.patches[0] == "@lang") {
          g_lang_id = (g_lang_id + 1) % 21;
          ctl.items[0] = kLangName[g_lang_id];
          g_changed = true;
          InvalidateRect(h, nullptr, FALSE);
          return 0;
        }
        if (BlurRowLocked(ctl)) return 0;  // FSR or DLSS is on: the blur stays as the game has it
        if (ctl.kind == Kind::Toggle) {
          ctl.sel = !ctl.sel;
          if (ctl.patches[0] == "@skipgpu") {
            g_skip_gpu_check = ctl.sel != 0;
            if (!GpuBlocked() && g_status == g_gpu_problem) g_status.clear();
            else if (GpuBlocked()) { g_status = g_gpu_problem; g_status_color = kBad; }
          }
          if (ctl.patches[0] == "@l3r3touch") {
            g_l3r3_touchpad = ctl.sel != 0;
            WriteL3R3Setting(g_l3r3_touchpad);
          }
          g_changed = true;
          if (c == g_hdr) Relayout(h);  // its sliders show or hide
        } else if (seg >= 0) {
          // choices that show or hide other rows (RowHidden)
          const bool model_changed = (c == g_ffb_model || c == g_aa || c == g_up || c == g_fps) && ctl.sel != seg;
          ctl.sel = seg;
          if (c == g_fps && model_changed) ApplyFrameModeCameraDefault();
          if (c == g_preset) ApplyPreset(seg);
          else if (c == g_outres || c == g_inres || c == g_wide) {
            // A widescreen size sets the render and output size itself: other sizes there crop the picture.
            if (c == g_wide && seg > 0 && g_outres >= 0 && g_inres >= 0 && (g_c[g_outres].sel != 0 || g_c[g_inres].sel != 0)) {
              g_c[g_outres].sel = g_c[g_inres].sel = 0;
              g_status = L"Output and render resolution are back on Default: the widescreen size sets both, and other sizes there crop the picture.";
              g_status_color = kWarn;
              g_status_sticky = true;
            } else if ((c == g_outres || c == g_inres) && seg > 0 && g_wide >= 0 && g_c[g_wide].sel > 0) {
              g_status = L"With a widescreen size, leave Output and Render resolution on Default, or the picture is cropped.";
              g_status_color = kWarn;
              g_status_sticky = true;
            }
            DerivePreset();
          }
          else if (c == g_ffb_preset) ApplyWheelPreset(seg);
          else if (c == g_drive_preset) ApplyDrivePreset(seg);

          if (c == g_ffb_model && seg == 1) {
            g_status = L"Original Physics is not recommended for wheels: it is the PS4 game's spring for 2014 Logitech wheels, without the tyres' grip. Reworked Physics gives the feel of a racing sim.";
            g_status_color = kWarn;
            g_status_sticky = true;
          }
          if (IsDriveRow(ctl)) DeriveDrivePreset();
          if (c == g_aa) NameNativeUpscale();
          g_changed = true;
          if (model_changed) Relayout(h);  // rows show or hide
        }
      }
      SetCapture(h);
      InvalidateRect(h, nullptr, FALSE);
      return 0;
    }
    case WM_LBUTTONUP: {
      ReleaseCapture();
      g_drag = -1;
      const float x = float(GET_X_LPARAM(l)), y = float(GET_Y_LPARAM(l));
      const int b = HitButton(x, y);
      const int pressed = g_press_btn;
      g_press_btn = -1;
      if (!g_busy && b >= 0 && b == pressed) {
        if (b == 3) StartUpdate();
        else if (b == 0) ResetDefaults();
        else if (b == 1) {
          const bool saved=SaveSettings();
          g_status=saved ? L"Saved." : g_settings_error;
          g_status_color=saved ? kOk : kBad;
        } else if (b == 2) Play();
      }
      InvalidateRect(h, nullptr, FALSE);
      return 0;
    }
    case WM_COMMAND:
      if (HIWORD(w) == EN_KILLFOCUS && g_edit) {
        CommitName();
        InvalidateRect(h, nullptr, FALSE);
      }
      return 0;
    case WM_APP + 1:
      InvalidateRect(h, nullptr, FALSE);
      return 0;
    case WM_APP + 2:
      DestroyWindow(h);
      return 0;
    case WM_APP + 3:  // the benchmark has ended: show its result in front of the other windows
      LoadBenchResult();
      if (IsIconic(h)) ShowWindow(h, SW_RESTORE);
      SetForegroundWindow(h);
      InvalidateRect(h, nullptr, FALSE);
      return 0;
    case WM_DESTROY:
      PostQuitMessage(0);
      return 0;
  }
  return DefWindowProcW(h, m, w, l);
}

#include "launcher_wpf_bridge.inc"

}  // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int show) {
  {
    int argc = 0; wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    const int deferred = argv ? Updater::RunDeferred(argc, argv) : -1;
    if (argv) LocalFree(argv);
    if (deferred >= 0) return deferred;
  }
  if (wcsstr(GetCommandLineW(), L"--gpu-check")) {
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    for (int i = 1; argv && i + 1 < argc; ++i) {
      if (wcscmp(argv[i], L"--gpu-check") == 0) return GpuCheckChildMain(argv[i + 1]);
    }
    return 0;
  }
  SetProcessDPIAware();
  CoInitialize(nullptr);
  SnapshotUserEnv();
  wchar_t selfbuf[MAX_PATH * 2];
  GetModuleFileNameW(nullptr, selfbuf, MAX_PATH * 2);
  g_home = Dir(selfbuf);
  g_data = g_home + L"\\data";
  g_runtime = g_home + L"\\runtime";
  BuildModel();
  MakeDirs(g_data + L"\\user");
  LoadSettings();
  LoadBenchResult();
  {
    int argc=0; wchar_t** argv=CommandLineToArgvW(GetCommandLineW(), &argc);
    const int result=RunWpfBridge(argc,argv); LocalFree(argv);
    if(result>=0){CoUninitialize();return result;}
  }
  StartLauncherLog();
  if (!g_skip_gpu_check) {
    g_gpu_problem = CheckCpu();
    if (g_gpu_problem.empty()) g_gpu_problem = CheckGpu();
  }
  LauncherLog(g_skip_gpu_check ? std::string("hardware check: skipped by the player") : g_gpu_problem.empty() ? std::string("hardware check: passed") : "hardware check: " + Narrow(g_gpu_problem));
  if (GpuBlocked()) {
    g_status = g_gpu_problem;
    g_status_color = kBad;
  }

  const bool asked = wcsstr(GetCommandLineW(), L"--play") != nullptr;
  const bool play_now = asked || g_auto_play;  // the row that starts the game straight away
  Updater::CleanUp(g_home);
  if (g_check_updates && !play_now && !wcsstr(GetCommandLineW(), L"--snapshot")) StartUpdateCheck();
  if (play_now) {
    const std::wstring stop = [] {
      if (!FoldersOk()) return std::wstring(L"Open DriveClub.exe once and choose the game folders.");
      if (!g_skip_gpu_check) {
        if (const std::wstring gpu = CheckGpu(); !gpu.empty()) return gpu;
      }
      if(!SaveSettings()) return g_settings_error;
      SetProgress(0, L"");
      std::wstring e = RunPrepare();
      return e.empty() ? StartGame() : e;
    }();
    if (stop.empty()) return 0;
    if (asked) {
      MessageBoxW(nullptr, stop.c_str(), L"DriveClub", MB_OK | MB_ICONERROR);
      return 1;
    }
    // The straight-away setting could not start the game: the window opens with the reason, so the
    // player is never left with a double click that appears to do nothing.
    g_status = stop;
    g_status_color = kBad;
  }

  GdiplusStartupInput gsi;
  ULONG_PTR token = 0;
  GdiplusStartup(&token, &gsi, nullptr);

  // --snapshot FILE: render the window to a PNG and exit (checks the layout without a screen).
  {
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    for (int i = 1; argv && i + 1 < argc; ++i) {
      if (wcscmp(argv[i], L"--snapshot") != 0) continue;
      HDC screen = GetDC(nullptr);
      g_dpi = g_scale = float(GetDeviceCaps(screen, LOGPIXELSX)) / 96.f;
      ReleaseDC(nullptr, screen);
      if (const char* sw = std::getenv("LAUNCHER_SNAPSHOT_W")) g_win_w = float(std::atoi(sw));
      if (const char* st = std::getenv("LAUNCHER_SNAPSHOT_TAB")) g_tab = std::clamp(std::atoi(st), 0, kTabCount - 1);
      if (const char* ss = std::getenv("LAUNCHER_SNAPSHOT_SUB")) g_subtab = std::clamp(std::atoi(ss), 0, kSubCount - 1);
      // LAUNCHER_SNAPSHOT_INFO = a row's name: its details panel
      if (const char* name = std::getenv("LAUNCHER_SNAPSHOT_INFO")) {
        for (size_t k = 0; k < g_c.size(); ++k) if (Narrow(g_c[k].label) == name) g_info_open = int(k);
      }
      // LAUNCHER_SNAPSHOT_SET = <row's name>=<choice number>: that choice instead of the saved one
      if (const char* set = std::getenv("LAUNCHER_SNAPSHOT_SET")) {
        const std::string s = set;
        const size_t eq = s.rfind('=');
        for (size_t k = 0; eq != std::string::npos && k < g_c.size(); ++k)
          if (Narrow(g_c[k].label) == s.substr(0, eq)) g_c[k].sel = std::atoi(s.c_str() + eq + 1);
      }
      Layout();
      const int w = int(g_win_w * g_scale), h = int(TotalHeight() * g_scale);
      {
      Bitmap bmp(w, h, PixelFormat32bppARGB);
      {
        Graphics gr(&bmp);
        HDC hdc = gr.GetHDC();
        Paint(hdc, w, h);
        gr.ReleaseHDC(hdc);
      }
      UINT count = 0, bytes = 0;
      GetImageEncodersSize(&count, &bytes);
      std::vector<char> raw(bytes);
      auto* encoders = reinterpret_cast<ImageCodecInfo*>(raw.data());
      GetImageEncoders(count, bytes, encoders);
      for (UINT k = 0; k < count; ++k) {
        if (wcscmp(encoders[k].MimeType, L"image/png") == 0) bmp.Save(argv[i + 1], &encoders[k].Clsid, nullptr);
      }
      }
      GdiplusShutdown(token);
      return 0;
    }
  }

  WNDCLASSW wc{};
  wc.lpfnWndProc = WndProc;
  wc.hInstance = inst;
  wc.lpszClassName = L"driveclub_launcher";
  wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
  wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
  RegisterClassW(&wc);

  HDC screen = GetDC(nullptr);
  g_dpi = g_scale = float(GetDeviceCaps(screen, LOGPIXELSX)) / 96.f;
  ReleaseDC(nullptr, screen);
  // The first window: the narrowest that shows the whole layout on this screen; wider windows hold
  // more columns and so are shorter. If none fits, the widest and the content scrolls.
  float start_w = kWinW;
  {
    RECT work{};
    if (SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0)) {
      const float avail_w = float(work.right - work.left), avail_h = float(work.bottom - work.top);
      const float candidates[3] = {kWinW, 1320.f, kMaxW};
      for (float cw : candidates) {
        if (cw * g_scale > avail_w) break;
        g_win_w = cw;
        g_win_h = 0;
        Layout();
        start_w = cw;
        if (TotalHeight() * g_scale + 48.f * g_scale <= avail_h) break;
      }
    }
  }
  g_win_w = start_w;
  Layout();
  const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_THICKFRAME;  // every edge and corner resizes
  RECT rc{0, 0, LONG(start_w * g_scale), LONG(TotalHeight() * g_scale)};
  AdjustWindowRect(&rc, style, FALSE);
  {
    // At most 85% of the work area high and 95% wide; the layout then shrinks to fit (see WM_SIZE).
    RECT work{};
    if (SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0)) {
      const LONG max_h = LONG((work.bottom - work.top) * 0.85), max_w = LONG((work.right - work.left) * 0.95);
      if (rc.bottom - rc.top > max_h) rc.bottom = rc.top + max_h;
      if (rc.right - rc.left > max_w) rc.right = rc.left + max_w;
    }
  }
  // Centred in the work area (the screen without the taskbar), never above its top edge.
  RECT area{0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)};
  SystemParametersInfoW(SPI_GETWORKAREA, 0, &area, 0);
  const int win_x = (std::max)(int(area.left), int(area.left) + (int(area.right - area.left) - int(rc.right - rc.left)) / 2);
  const int win_y = (std::max)(int(area.top), int(area.top) + (int(area.bottom - area.top) - int(rc.bottom - rc.top)) / 2);
  g_hwnd = CreateWindowExW(0, wc.lpszClassName, L"DriveClub - PC", style, win_x, win_y, rc.right - rc.left,
                           rc.bottom - rc.top, nullptr, nullptr, inst, nullptr);
  INITCOMMONCONTROLSEX common{sizeof(common), ICC_WIN95_CLASSES};
  InitCommonControlsEx(&common);
  g_motion_blur_tooltip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr,
      WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX, CW_USEDEFAULT, CW_USEDEFAULT,
      CW_USEDEFAULT, CW_USEDEFAULT, g_hwnd, nullptr, inst, nullptr);
  if (g_motion_blur_tooltip) {
    SendMessageW(g_motion_blur_tooltip, TTM_SETMAXTIPWIDTH, 0, 430);
    SendMessageW(g_motion_blur_tooltip, TTM_SETDELAYTIME, TTDT_INITIAL, 350);
    for (int choice = 0; choice < 2; ++choice) {
      TOOLINFOW tool{}; tool.cbSize = sizeof(tool); tool.uFlags = TTF_SUBCLASS;
      tool.hwnd = g_hwnd; tool.uId = choice + 1;
      tool.lpszText = const_cast<wchar_t*>(MotionBlurHint(choice));
      SendMessageW(g_motion_blur_tooltip, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&tool));
    }
  }
  ShowWindow(g_hwnd, show);
  MSG msg;
  while (GetMessageW(&msg, nullptr, 0, 0)) {
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
  }
  GdiplusShutdown(token);
  CoUninitialize();
  return int(msg.wParam);
}
