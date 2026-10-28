#include <windows.h>
#include <vector>
#include <utility>
static std::vector<std::pair<const wchar_t*, DWORD>> fixture_displays;
static BOOL WINAPI FixtureEnumDisplayDevicesW(LPCWSTR, DWORD index, PDISPLAY_DEVICEW display, DWORD) {
  if (index >= fixture_displays.size()) return FALSE;
  wcsncpy_s(display->DeviceString, fixture_displays[index].first, _TRUNCATE);
  display->StateFlags = fixture_displays[index].second;
  return TRUE;
}
#define EnumDisplayDevicesW FixtureEnumDisplayDevicesW
#include "../launcher/gpu_check.cpp"
#undef EnumDisplayDevicesW
#include <cassert>
#include <fstream>

int main() {
  const auto home = std::filesystem::path(ExeDir());
  const auto runtime = home / L"runtime";
  const auto external = home / L"external-model";
  // Run this fixture from its own output directory, never beside an installed launcher.
  if (std::filesystem::exists(runtime) || std::filesystem::exists(external)) {
    fputs("Run the fixture from an isolated output directory.\n", stderr);
    return 1;
  }
  std::filesystem::create_directory(runtime);
  std::filesystem::create_directory(external);
  std::ofstream(runtime / L"bbport_nr_nvngx.dll").put('\0');
  std::ofstream(external / L"nvngx_dlssnr.dll").put('\0');
  std::ofstream(runtime / L"bbport_dlss.dll").put('\0');
  std::ofstream(runtime / L"nvngx_dlss.dll").put('\0');
  std::ofstream(runtime / L"nvngx_dlssg.dll").put('\0');
  _wputenv_s(L"BB_GPU_ID", L"");
  fixture_displays = {{L"Intel(R) Graphics", DISPLAY_DEVICE_ATTACHED_TO_DESKTOP}, {L"NVIDIA GeForce RTX 5080", 0}};
  assert(!DisplayGraphicsCapabilities().checked && DisplayGraphicsCapabilities().dlss_runtime);
  fixture_displays = {{L"NVIDIA GeForce RTX 5080", DISPLAY_DEVICE_ATTACHED_TO_DESKTOP}};
  assert(DisplayGraphicsCapabilities().checked && DisplayGraphicsCapabilities().frame_multiplier_max == 6);
  _wputenv_s(L"BB_GPU_ID", L"0");
  assert(!DisplayGraphicsCapabilities().checked);
  _wputenv_s(L"BB_GPU_ID", L"");
  Candidate card{.name = L"NVIDIA GeForce RTX 5080"};
  _wputenv_s(L"BB_NR_RUNTIME", external.c_str());
  assert(FeatureSupport(card).neural_runtime && FeatureSupport(card).neural);
  const auto model = external / L"nvngx_dlssnr.dll";
  _wputenv_s(L"BB_NR_RUNTIME", model.c_str());
  assert(FeatureSupport(card).neural_runtime);
  std::filesystem::remove(runtime / L"bbport_nr_nvngx.dll");
  assert(!FeatureSupport(card).neural_runtime);
  std::ofstream(runtime / L"bbport_nr_nvngx.dll").put('\0');
  _wputenv_s(L"BB_NR_RUNTIME", L"");
  assert(!FeatureSupport(card).neural_runtime);
  std::ofstream(runtime / L"nvngx_dlssnr.dll").put('\0');
  assert(FeatureSupport(card).neural_runtime);
  std::filesystem::remove(runtime / L"nvngx_dlssnr.dll");
  std::filesystem::remove(runtime / L"bbport_nr_nvngx.dll");
  std::filesystem::remove(runtime / L"bbport_dlss.dll");
  std::filesystem::remove(runtime / L"nvngx_dlss.dll");
  std::filesystem::remove(runtime / L"nvngx_dlssg.dll");
  std::filesystem::remove(external / L"nvngx_dlssnr.dll");
  std::filesystem::remove(runtime);
  std::filesystem::remove(external);
  puts("PASS: NR runtime paths, detached adapter ambiguity, single GPU and explicit device override.");
}
