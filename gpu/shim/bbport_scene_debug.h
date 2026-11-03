#pragma once
#include "bbport_frame_stats.h"
#include "bbport_scene_clock.h"
#include "imgui.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

extern "C" uintptr_t runtime_image_base;
extern "C" uint64_t runtime_image_size;
extern "C" int runtime_scene_debug_time(float *, float *);

namespace BbOverlay::SceneDebug {
inline std::atomic<bool> expanded{false};
inline bool Enabled() {
  static const bool value = [] {
    const char *v = std::getenv("BB_SCENE_OVERLAY");
    return !v || std::atoi(v) != 0;
  }();
  return value;
}
inline std::array<float, 360> frame_times{};
inline unsigned frame_next = 0, frame_count = 0;
inline FrameTimingStats frame_stats;
inline void NoteFrame(float ms, std::chrono::steady_clock::time_point now) {
  if (!(ms > 0) || !std::isfinite(ms))
    return;
  frame_stats.Add(now, ms);
  frame_times[frame_next] = ms;
  frame_next = (frame_next + 1) % frame_times.size();
  frame_count = std::min(frame_count + 1, unsigned(frame_times.size()));
}
inline void DrawFrameTimes(float scale) {
  if (!frame_count)
    return;
  float peak = 0;
  for (unsigned i = 0; i < frame_count; ++i)
    peak = std::max(peak, frame_times[i]);
  const float latest =
      frame_times[(frame_next + frame_times.size() - 1) % frame_times.size()];
  static float shown_latest = 0, shown_peak = 0;
  static auto last_label = std::chrono::steady_clock::time_point{};
  const auto label_now = std::chrono::steady_clock::now();
  if (label_now - last_label >= std::chrono::milliseconds(100)) {
    last_label = label_now;
    shown_latest = latest;
    shown_peak = peak;
  }
  ImGui::Text("Frame time: %.1f ms", shown_latest);
  ImGui::SameLine(210.0f * scale);
  ImGui::Text("max %.1f ms", shown_peak);
  const float ceiling = std::max(50.0f, std::ceil(peak / 16.6667f) * 16.6667f);
  const ImVec2 pos = ImGui::GetCursorScreenPos();
  const ImVec2 size(420.0f * scale, 64.0f * scale);
  const ImVec4 text_color = ImGui::GetStyleColorVec4(ImGuiCol_Text);
  ImGui::PushStyleColor(ImGuiCol_PlotLines, text_color);
  ImGui::PushStyleColor(ImGuiCol_PlotLinesHovered, text_color);
  ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0, 0, 0, 0));
  ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(0, 0, 0, 0));
  ImGui::PlotLines("##frame_times", frame_times.data(), int(frame_count),
                   frame_count == frame_times.size() ? int(frame_next) : 0,
                   nullptr, 0, ceiling, size);
  ImGui::PopStyleColor(4);
  auto *draw = ImGui::GetWindowDrawList();
  for (float line : {16.6667f, 33.3333f}) {
    const float y = pos.y + size.y * (1.0f - line / ceiling);
    draw->AddLine(ImVec2(pos.x, y), ImVec2(pos.x + size.x, y),
                  IM_COL32(180, 180, 180, 70));
  }
}
inline bool Read(uintptr_t address, void *output, size_t size) {
#ifdef _WIN32
  SIZE_T got = 0;
  return address >= 0x10000 &&
         ReadProcessMemory(GetCurrentProcess(),
                           reinterpret_cast<const void *>(address), output,
                           size, &got) &&
         got == size;
#else
  return false;
#endif
}
inline uintptr_t Pointer(uintptr_t address) {
  uintptr_t value = 0;
  Read(address, &value, sizeof(value));
  return value;
}
inline std::string String(uintptr_t object) {
  std::array<unsigned char, 40> raw{};
  if (!Read(object, raw.data(), raw.size()))
    return {};
  uint64_t length = 0, capacity = 0;
  std::memcpy(&length, raw.data() + 24, 8);
  std::memcpy(&capacity, raw.data() + 32, 8);
  if (!length || length > 120 || capacity < length || capacity > 1048576)
    return {};
  uintptr_t address = object + 8;
  if (capacity > 15)
    std::memcpy(&address, raw.data() + 8, 8);
  std::string result(size_t(length), '\0');
  if (!Read(address, result.data(), result.size()))
    return {};
  for (unsigned char c : result)
    if (c < 32 || c > 126)
      return {};
  return result;
}
struct State {
  uintptr_t config = 0, active = 0;
  std::string track, clouds, precipitation;
  float start = -1, compression = -1, rain = -1;
};
inline State state;
inline std::string TrackName(const std::string &id) {
  static const auto names = [] {
    std::vector<std::pair<std::string, std::string>> result;
#ifdef _WIN32
    const wchar_t *path = _wgetenv(L"BB_SCENE_TRACK_NAMES");
    FILE *file = path ? _wfopen(path, L"rb") : nullptr;
#else
    const char *path = std::getenv("BB_SCENE_TRACK_NAMES");
    FILE *file = path ? std::fopen(path, "r") : nullptr;
#endif
    if (!file)
      return result;
    char line[512];
    while (std::fgets(line, sizeof(line), file)) {
      char *tab = std::strchr(line, '\t');
      if (!tab)
        continue;
      *tab++ = 0;
      tab[std::strcspn(tab, "\r\n")] = 0;
      result.emplace_back(line, tab);
    }
    std::fclose(file);
    return result;
  }();
  for (const auto &[key, name] : names)
    if (key == id)
      return name;
  return id;
}
inline void Update() {
  static auto last_update = std::chrono::steady_clock::time_point{};
  const auto update_now = std::chrono::steady_clock::now();
  if (update_now - last_update < std::chrono::milliseconds(100)) return;
  last_update = update_now;
  State next;
  if (runtime_image_base && runtime_image_size > 0x1911108) {
    const auto global = Pointer(runtime_image_base + 0x1911100);
    const auto services = Pointer(global + 0x970);
    const auto base = Pointer(services + 0xf8);
    unsigned char primary = 1;
    if (base && Read(base + 0x780, &primary, 1)) {
      next.config = base + (primary ? 8 : 0x3b8);
      // GameConfig::GetTrack (0x4f70): text at +0xf0, string object at +0xe8.
      next.track = String(next.config + 0xe8);
      next.clouds = String(next.config + 0x208);
      next.precipitation = String(next.config + 0x230);
      Read(next.config + 0x2d4, &next.start, 4);
      Read(next.config + 0x2d8, &next.compression, 4);
    }
    const auto world = Pointer(global + 0x968);
    next.active = Pointer(world + 0xf8);
    const auto active_track = String(next.active + 0xf0);
    if (!active_track.empty() && active_track != "unk")
      next.track = active_track;
    const auto active_clouds = String(next.active + 0x210);
    if (!active_clouds.empty())
      next.clouds = active_clouds;
    const auto precipitation = Pointer(services + 0x258);
    Read(precipitation + 0x98, &next.rain, 4);
    if (!std::isfinite(next.rain) || next.rain < 0 || next.rain > 1)
      next.rain = -1;
  }
  state = std::move(next);
}
inline void Draw() {
  Update();
  if (state.track.empty()) {
    ImGui::TextDisabled("Scene: waiting for race data");
    return;
  }
  ImGui::Separator();
  ImGui::Text("Track: %s", TrackName(state.track).c_str());
  static float hour = -1, compression = -1;
  static bool live = false;
  static auto last_clock = std::chrono::steady_clock::time_point{};
  const auto clock_now = std::chrono::steady_clock::now();
  if (clock_now - last_clock >= std::chrono::milliseconds(100)) {
    last_clock = clock_now;
    live = runtime_scene_debug_time(&hour, &compression) &&
           std::isfinite(hour) && hour >= 0 && hour < 100000;
  }
  if (live) {
    const int seconds = int(std::fmod(hour, 24.0f) * 3600.0f);
    ImGui::Text("Time of Day: %02d:%02d:%02d", seconds / 3600,
                seconds / 60 % 60, seconds % 60);
  } else if (std::isfinite(state.start) && state.start >= 0 &&
             state.start < 24) {
    const int minutes = int(state.start * 60);
    ImGui::Text("Time of Day: --  (start %02d:%02d)", minutes / 60,
                minutes % 60);
  } else
    ImGui::Text("Time of Day: --");
  const float factor = live ? compression : state.compression;
  char compression_text[64];
  bbport_scene_compression_text(compression_text, sizeof compression_text, factor, live);
  ImGui::TextUnformatted(compression_text);
  ImGui::Text("Cloud Cover: %s",
              state.clouds.empty() ? "--" : state.clouds.c_str());
  if (state.rain >= 0)
    ImGui::Text("Precipitation: %.1f%%  (%s)", state.rain * 100,
                state.precipitation.c_str());
  else
    ImGui::Text("Precipitation: --  (%s)", state.precipitation.c_str());
}
} // namespace BbOverlay::SceneDebug
