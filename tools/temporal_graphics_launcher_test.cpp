#define wWinMain temporal_graphics_unused_wWinMain
#include "../launcher/driveclub_launcher.cpp"
#undef wWinMain
#include <cassert>

int main() {
  wchar_t temp[MAX_PATH];
  assert(GetTempPathW(MAX_PATH, temp));
  g_home = std::wstring(temp) + L"driveclub-temporal-launcher-" + std::to_wstring(GetCurrentProcessId());
  g_data = g_home + L"\\data";
  g_runtime = g_home + L"\\runtime";
  MakeDirs(g_data + L"\\user");
  BuildModel();
  LoadSettings();
  g_graphics = CapabilitiesForGpu(L"NVIDIA GeForce RTX 5080");
  g_graphics.dlss_runtime = g_graphics.neural_runtime = true;
  assert(g_graphics.frame_multiplier_max == 6 && g_graphics.neural);
  assert(g_c[g_fgen_preset_row].sel == 1 && g_c[g_neural_preset_row].sel == 1);
  g_c[g_aa].sel = 2;
  g_c[g_fgen].sel = 2;
  g_c[g_fgen_x].sel = 4;
  g_c[g_sr_preset_row].sel = 4;
  g_c[g_reflex_row].sel = 2;
  g_c[g_neural_row].sel = 1;
  g_c[g_nr_intensity].sel = 4;
  g_c[g_nr_tone].sel = 3;
  g_c[g_nr_structure].sel = 1;
  SaveSettings();
  assert(Exists(SelectionPath()));
  LoadSettings();
  assert(g_c[g_fgen].sel == 2 && g_c[g_fgen_x].sel == 4);
  assert(g_c[g_sr_preset_row].sel == 4 && g_c[g_reflex_row].sel == 2);
  assert(g_c[g_fgen_preset_row].sel == 1 && g_c[g_neural_preset_row].sel == 1);
  assert(g_c[g_neural_row].sel == 1 && g_c[g_nr_intensity].sel == 4);
  assert(g_c[g_nr_tone].sel == 3 && g_c[g_nr_structure].sel == 1);
  g_graphics = CapabilitiesForGpu(L"NVIDIA GeForce RTX 4080");
  g_graphics.dlss_runtime = true;
  assert(!ControlReason(g_fgen_x, 4).empty());
  assert(NormalizeGraphics());
  assert(g_c[g_fgen_x].sel == 0 && g_c[g_neural_row].sel == 0);
  puts("PASS: temporal controls survive save/reload with a patch selection; defaults and GPU limits.");
}
