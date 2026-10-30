#include "../../updater.cpp"
#include <cassert>
#include <filesystem>
#include <fstream>
#include <cstdio>
namespace fs = std::filesystem;
static void Write(const fs::path& p, const char* text) { fs::create_directories(p.parent_path()); std::ofstream(p) << text; }
static std::string Read(const fs::path& p) { std::ifstream f(p); return std::string(std::istreambuf_iterator<char>(f), {}); }
int wmain(int argc, wchar_t** argv) {
  assert(argc == 2);
  const auto base = fs::path(argv[1]) / (L"fallback-update-" + std::to_wstring(GetCurrentProcessId()));
  auto root = base/L"new", home = base/L"home";
  Write(root/L"DriveClub.exe", "new bootstrap"); Write(root/L"DriveClub.Native.exe", "new fallback");
  Write(home/L"DriveClub.exe", "old bootstrap"); Write(home/L"DriveClub.Native.exe", "old fallback");
  Write(root/L"runtime/DriveClubGame.exe", "new game");
  Write(home/L"launcher.ini", "retain settings"); Write(root/L"launcher.ini", "do not install");
  std::wstring error;
  assert(Updater::Apply(root.wstring(), home.wstring(), error, L"DriveClub.Native.exe"));
  assert(Read(home/L"DriveClub.exe") == "new bootstrap" && Read(home/L"DriveClub.Native.exe") == "new fallback");
  assert(Read(home/L"DriveClub.exe.old") == "old bootstrap" && Read(home/L"DriveClub.Native.exe.old") == "old fallback");
  assert(Read(home/L"launcher.ini") == "retain settings" && Read(home/L"runtime/DriveClubGame.exe") == "new game");
  // A release with only the original launcher can still replace the entry point.
  root = base/L"legacy"; Write(root/L"DriveClub.exe", "legacy native");
  assert(Updater::Apply(root.wstring(), home.wstring(), error, L"DriveClub.Native.exe"));
  assert(Read(home/L"DriveClub.exe") == "legacy native" && Read(home/L"DriveClub.Native.exe") == "new fallback");
  std::puts("PASS: fallback updater replaces bootstrap and native executable last, retains settings and accepts native-only packages");
}
