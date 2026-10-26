#include "../launcher/gpu_check.cpp"
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
  std::filesystem::remove(external / L"nvngx_dlssnr.dll");
  std::filesystem::remove(runtime);
  std::filesystem::remove(external);
  puts("PASS: NR model directory, DLL path, local fallback, and missing bridge.");
}
