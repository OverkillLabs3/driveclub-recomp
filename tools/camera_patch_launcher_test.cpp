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
  assert(row >= 0 && g_c[row].def == 0 && g_c[row].sel == 0);
  LoadSettings();
  assert(g_c[row].sel == 0);
  for (int selected : {1, 0}) {
    g_c[row].sel = selected;
    SaveSettings();
    g_c[row].sel = 1 - selected;
    g_camera_patch = !selected;
    LoadSettings();
    assert(g_c[row].sel == selected);
    SetCameraPatchEnvironment();
    wchar_t value[8] = {};
    assert(GetEnvironmentVariableW(L"BB_CAMERA_PATCH", value, 8) == 1);
    assert(value[0] == (selected ? L'1' : L'0'));
  }
  for (const auto& line : ReadLines(SelectionPath())) assert(line != "@camera_patch");
  puts("PASS: disabled by default, launcher choice survives save/reload, on-to-off environment is refreshed, no XML pseudo-patch emitted.");
}
