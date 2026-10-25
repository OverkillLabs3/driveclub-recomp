#define wWinMain motion_blur_unused_wWinMain
#include "../launcher/driveclub_launcher.cpp"
#undef wWinMain
#include <cassert>
int main() {
  wchar_t temp[MAX_PATH];
  assert(GetTempPathW(MAX_PATH, temp));
  g_home = std::wstring(temp) + L"driveclub-motion-blur-launcher-" +
           std::to_wstring(GetCurrentProcessId());
  g_data = g_home + L"\\data";
  g_runtime = g_home + L"\\runtime";
  MakeDirs(g_data + L"\\user");
  BuildModel();
  LoadSettings();
  assert(g_motion_blur >= 0 && g_c[g_motion_blur].sel == 0 &&
         g_c[g_motion_blur].def == 0);
  bool in_effects = false;
  for (const auto &card : g_cards)
    if (card.tab == kTabDetail && std::wstring(card.title) == L"EFFECTS")
      for (int id : card.ctls)
        in_effects |= id == g_motion_blur;
  assert(in_effects);
  for (int choice : {1, 0}) {
    g_c[g_motion_blur].sel = choice;
    SaveSettings();
    g_c[g_motion_blur].sel = 1 - choice;
    LoadSettings();
    assert(g_c[g_motion_blur].sel == choice);
    assert(std::wstring(MotionBlurReference()) ==
           (choice ? L"short" : L"long"));
    assert(std::wstring(MotionBlurHint(choice))
               .find(choice ? L"60 FPS" : L"30 FPS") != std::wstring::npos);
  }
  g_c[g_motion_blur].sel = 1;
  ResetDefaults();
  assert(g_c[g_motion_blur].sel == 0);
  g_c[g_aa].sel = 2;
  assert(BlurLocked() && !BlurRowLocked(g_c[g_motion_blur]));
  for (const auto &line : ReadLines(SelectionPath()))
    assert(line.find("@motion_blur") == std::string::npos);
  assert(std::wstring(MotionBlurHint(0)).find(L"Best for a cinematic look") !=
         std::wstring::npos);
  puts("PASS: Slow default/reset; Fast/Slow save/reload and runtime exposure "
       "mapping; independent of temporal AA lock; per-option hints; no XML "
       "pseudo-patch.");
}
