#define wWinMain camera_patch_unused_wWinMain
#include "../launcher/driveclub_launcher.cpp"
#undef wWinMain
#include <cassert>

int main() {
  wchar_t temp[MAX_PATH];
  assert(GetTempPathW(MAX_PATH, temp));
  g_home = std::wstring(temp) + L"driveclub-camera-patch-test-" + std::to_wstring(GetCurrentProcessId());
  g_data = g_home + L"\\data";
  g_runtime = g_home + L"\\runtime";
  MakeDirs(g_data + L"\\user");
  BuildModel();
  int row = -1;
  for (size_t i = 0; i < g_c.size(); ++i)
    if (g_c[i].patches[0] == "@camera_patch") row = int(i);
  assert(row >= 0 && g_c[row].def == 1 && g_c[row].sel == 1);
  LoadSettings();
  assert(g_c[row].sel == 1);
  assert(g_motion_blur >= 0 && g_c[g_motion_blur].sel == 0);
  int threads = -1;
  for (size_t i = 0; i < g_c.size(); ++i)
    if (g_c[i].patches[0] == "@twothread") threads = int(i);
  assert(threads >= 0);
  for (int selected : {1, 0}) {
    g_c[row].sel = selected;
    g_c[g_motion_blur].sel = 1 - selected;
    g_c[threads].sel = selected;
    SaveSettings();
    g_c[row].sel = 1 - selected;
    g_camera_patch = !selected;
    LoadSettings();
    assert(g_c[row].sel == selected);
    assert(g_c[g_motion_blur].sel == 1 - selected);
    assert(g_c[threads].sel == selected && g_two_thread == bool(selected));
    assert(std::wstring(MotionBlurReference()) == (selected ? L"long" : L"short"));
    bool saved_threads = false;
    for (const auto& line : ReadLines(IniPath())) {
      assert(line.rfind("two_thread=", 0) != 0);
      if (line == std::string("draw_threads=") + (selected ? "1" : "0")) saved_threads = true;
    }
    assert(saved_threads);
    SetCameraPatchEnvironment();
    wchar_t value[8] = {};
    assert(GetEnvironmentVariableW(L"BB_CAMERA_PATCH", value, 8) == 1);
    assert(value[0] == (selected ? L'1' : L'0'));
  }
  for (const auto& line : ReadLines(SelectionPath())) assert(line != "@camera_patch");
  ResetDefaults(); assert(g_c[row].sel == 1);
  puts("PASS: enabled by default and after reset, launcher choice survives save/reload, on-to-off environment is refreshed, Motion Blur and draw_threads preserved independently, no XML pseudo-patch emitted.");
}
