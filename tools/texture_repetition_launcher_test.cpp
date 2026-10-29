#define wWinMain texture_repetition_unused_wWinMain
#include "../launcher/driveclub_launcher.cpp"
#undef wWinMain
#include <cassert>

int main() {
  wchar_t temp[MAX_PATH];
  assert(GetTempPathW(MAX_PATH, temp));
  g_home = std::wstring(temp) + L"driveclub-texture-repeat-test-" + std::to_wstring(GetCurrentProcessId());
  g_data = g_home + L"\\data";
  g_runtime = g_home + L"\\runtime";
  MakeDirs(g_data + L"\\user");
  BuildModel();
  assert(g_texture_repetition >= 0 && g_c[g_texture_repetition].def == 1);
  LoadSettings();
  assert(g_c[g_texture_repetition].sel == 1);
  for (int preset = 0; preset <= 6; ++preset) {
    g_c[g_texture_repetition].sel = preset == 6 ? 1 : 0;
    ApplyPreset(preset);
    assert(g_c[g_texture_repetition].sel == (preset == 6 ? 0 : 1));
  }
  // An explicit opt-out survives saving in a PC profile, independently of the
  // PS4 default, and goes into both the launcher and the runtime settings files.
  ApplyPreset(4);
  for (int enabled : {0, 1, 0}) {
    g_c[g_texture_repetition].sel = enabled;
    if (!SaveSettings()) { const auto error=GetLastError(); std::fprintf(stderr, "SaveSettings: %s (Win32 %lu)\n", Narrow(g_settings_error).c_str(),error); return 2; }
    g_c[g_texture_repetition].sel = 1 - enabled;
    LoadSettings();
    assert(g_c[g_texture_repetition].sel == enabled);
    bool saved = false;
    for (const auto& line : ReadLines(g_data + L"\\user\\driveclub.ini"))
      if (line == std::string("texture_repetition=") + (enabled ? "1" : "0")) saved = true;
    assert(saved);
    SetTextureRepetitionEnvironment();
    wchar_t value[8] = {};
    assert(GetEnvironmentVariableW(L"BB_TEXTURE_REPETITION", value, 8) == 1);
    assert(value[0] == (enabled ? L'1' : L'0'));
  }
  // The environment exported by a previous Play must not pin the next Play.
  SetEnvironmentVariableW(L"BB_TEXTURE_REPETITION", L"0");
  g_user_env.clear();
  SnapshotUserEnv();
  g_c[g_texture_repetition].sel = 1;
  SetTextureRepetitionEnvironment();
  wchar_t forced[8] = {};
  GetEnvironmentVariableW(L"BB_TEXTURE_REPETITION", forced, 8);
  assert(forced[0] == L'0');
  g_user_env.clear();
  SetEnvironmentVariableW(L"BB_TEXTURE_REPETITION", nullptr);

  // Existing PS4 profiles predate the key. Migrate them without enabling an
  // enhancement; legacy PC profiles receive the new default.
  for (int preset : {6, 4}) {
    ApplyPreset(preset);
    if (!SaveSettings()) { const auto error=GetLastError(); std::fprintf(stderr, "SaveSettings: %s (Win32 %lu)\n", Narrow(g_settings_error).c_str(),error); return 2; }
    std::string legacy;
    for (const auto& line : ReadLines(IniPath()))
      if (line.rfind("texture_repetition=", 0) != 0) legacy += line + "\n";
    assert(SaveTextAtomically(IniPath(), legacy));
    g_c[g_texture_repetition].sel = 1;
    LoadSettings();
    assert(g_c[g_texture_repetition].sel == (preset == 6 ? 0 : 1));
  }
  ApplyPreset(6);
  assert(IsPs4Profile());
  g_c[g_texture_repetition].sel = 1;
  assert(!IsPs4Profile());
  ResetDefaults();
  assert(g_c[g_texture_repetition].sel == 1);
  for (const auto& line : ReadLines(SelectionPath()))
    assert(line.find("@texture_repetition") == std::string::npos);
  puts("PASS: PC/PS4 presets, legacy migration, explicit opt-out, save/reload, runtime config, environment precedence, reset; no XML pseudo-patch.");
}
