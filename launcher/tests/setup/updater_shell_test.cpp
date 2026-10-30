#include "../../updater.cpp"
#include <cassert>
#include <filesystem>
#include <fstream>
#include <cstdio>
namespace fs = std::filesystem;
static void Write(const fs::path& p, const std::string& value) { fs::create_directories(p.parent_path()); std::ofstream(p, std::ios::binary) << value; }
static std::string Read(const fs::path& p) { std::ifstream f(p, std::ios::binary); return {std::istreambuf_iterator<char>(f), {}}; }
static PROCESS_INFORMATION Child(const fs::path& exe, const std::wstring& args) {
  auto command = L"\"" + exe.wstring() + L"\" " + args;
  STARTUPINFOW si{}; si.cb = sizeof(si); PROCESS_INFORMATION pi{};
  assert(CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, exe.parent_path().c_str(), &si, &pi));
  CloseHandle(pi.hThread); return pi;
}
int wmain(int argc, wchar_t** argv) {
  int deferred = Updater::RunDeferred(argc, argv); if (deferred >= 0) return deferred;
  wchar_t image[32768]; GetModuleFileNameW(nullptr, image, 32768); fs::path self(image);
  if (argc == 4 && wcscmp(argv[1], L"--shell") == 0) {
    HANDLE locked = CreateFileW((self.parent_path()/L"DriveClub.Launcher.dll").c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    assert(locked != INVALID_HANDLE_VALUE);
    HANDLE ready = OpenEventW(EVENT_MODIFY_STATE, FALSE, argv[2]);
    HANDLE release = OpenEventW(SYNCHRONIZE, FALSE, argv[3]);
    assert(ready && release); SetEvent(ready); WaitForSingleObject(release, 30000);
    CloseHandle(locked); CloseHandle(ready); CloseHandle(release); return 0;
  }
  if (self.filename() == L"DriveClub.exe") { Write(self.parent_path()/L"started.txt", "new launcher started"); return 0; }
  assert(argc == 2);
  fs::path base = fs::path(argv[1])/(L"wpf-update-" + std::to_wstring(GetCurrentProcessId()));
  auto home = base/L"home", root = home/L"update_download/new/driveclub-recomp-9.9.9-win64";
  fs::create_directories(root); fs::create_directories(home);
  fs::copy_file(self, home/L"DriveClub.Launcher.exe"); fs::copy_file(self, root/L"DriveClub.exe");
  Write(home/L"DriveClub.exe", "old bootstrap");
  Write(home/L"DriveClub.Launcher.dll", "old WPF library"); Write(root/L"DriveClub.Launcher.dll", "new WPF library");
  for (const auto& relative : {L"launcher.ini", L"data/user/save.bin", L"mods/player.bin", L"logs/player.log"}) {
    Write(home/relative, "player-owned"); Write(root/relative, "must not replace");
  }
  const auto readyName=L"Local\\DriveClub.UpdateReady."+std::to_wstring(GetCurrentProcessId());
  const auto releaseName=L"Local\\DriveClub.UpdateRelease."+std::to_wstring(GetCurrentProcessId());
  HANDLE ready=CreateEventW(nullptr,TRUE,FALSE,readyName.c_str()), release=CreateEventW(nullptr,TRUE,FALSE,releaseName.c_str());
  auto shell=Child(home/L"DriveClub.Launcher.exe",L"--shell "+readyName+L" "+releaseName);
  assert(WaitForSingleObject(ready,10000)==WAIT_OBJECT_0);
  std::wstring error;
  assert(!Updater::Defer(root.wstring(),home.wstring(),GetCurrentProcessId(),error));
  assert(Read(home/L"DriveClub.Launcher.dll")=="old WPF library");
  assert(Updater::Defer(root.wstring(),home.wstring(),shell.dwProcessId,error));
  Sleep(250);
  assert(Read(home/L"DriveClub.Launcher.dll")=="old WPF library" && !fs::exists(home/L"started.txt"));
  SetEvent(release); assert(WaitForSingleObject(shell.hProcess,10000)==WAIT_OBJECT_0); CloseHandle(shell.hProcess);
  for(int i=0;i<200 && !fs::exists(home/L"started.txt");++i)Sleep(50);
  assert(fs::exists(home/L"started.txt"));
  assert(Read(home/L"DriveClub.Launcher.dll")=="new WPF library");
  for(const auto& relative:{L"launcher.ini",L"data/user/save.bin",L"mods/player.bin",L"logs/player.log"}) assert(Read(home/relative)=="player-owned");
  assert(Read(home/L"DriveClub.exe.old")=="old bootstrap");
  Write(home/L"update_download/do-not-delete.txt","unrelated");
  Updater::CleanUp(home.wstring());assert(Read(home/L"update_download/do-not-delete.txt")=="unrelated");
  CloseHandle(ready);CloseHandle(release);
  std::puts("PASS: real helper waits for mapped WPF DLLs, rejects unrelated PIDs, installs/restarts and preserves settings, saves, mods and unrelated staging files");
}
