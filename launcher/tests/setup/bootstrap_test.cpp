#include "../../setup/bootstrap.h"
#include <shellapi.h>
#include <cassert>
#include <filesystem>
#include <cstdio>
#include <cstring>

namespace fs = std::filesystem;
static void Pe(const fs::path& path, std::uint16_t machine = 0x8664) {
    fs::create_directories(path.parent_path());
    std::vector<char> data(160);
    const std::uint16_t mz = 0x5a4d; const std::uint32_t offset = 128, signature = 0x4550;
    std::memcpy(data.data(), &mz, 2); std::memcpy(data.data() + 60, &offset, 4);
    std::memcpy(data.data() + 128, &signature, 4); std::memcpy(data.data() + 132, &machine, 2);
    std::ofstream file(path, std::ios::binary); file.write(data.data(), data.size());
}
int wmain(int argc, wchar_t** argv) {
    assert(argc == 2);
    const auto root = fs::path(argv[1]) / (L"native-bootstrap-" + std::to_wstring(GetCurrentProcessId()));
    const auto app = root / L"app", runtime = root / L"runtime";
    fs::create_directories(app);
    const std::vector<std::wstring> roots{runtime.wstring()};
    unsigned checks = 0;
    auto check = [&](bool ok) { assert(ok); ++checks; };
    check(!Bootstrap::Runtime(app.wstring(), roots).available);
    Pe(runtime / L"shared/Microsoft.NETCore.App/8.0.31/coreclr.dll");
    check(!Bootstrap::Runtime(app.wstring(), roots).available);
    Pe(runtime / L"shared/Microsoft.WindowsDesktop.App/8.0.31/PresentationNative_cor3.dll", 0x014c);
    check(!Bootstrap::Runtime(app.wstring(), roots).available);
    Pe(runtime / L"shared/Microsoft.WindowsDesktop.App/8.0.31/PresentationNative_cor3.dll");
    check(Bootstrap::Runtime(app.wstring(), roots).available);
    Pe(runtime / L"shared/Microsoft.WindowsDesktop.App/8.0.32/PresentationNative_cor3.dll");
    check(!Bootstrap::Runtime(app.wstring(), roots).available);
    Pe(runtime / L"shared/Microsoft.NETCore.App/8.0.32/coreclr.dll");
    check(Bootstrap::Runtime(app.wstring(), roots).available);
    check(Bootstrap::Runtime(app.wstring(), {L"Z:\\absent",runtime.wstring()}).root == runtime.wstring());
    for (const wchar_t* version : {L"8.0.", L"8.0.31-preview", L"9.0.1", L"8.0.-1", L"8.0.999999999999999999"}) {
        unsigned patch = 0; check(!Bootstrap::StableEight(version, patch));
    }
    Pe(app / L"coreclr.dll"); Pe(app / L"PresentationNative_cor3.dll");
    { std::ofstream file(app / L"DriveClub.Launcher.runtimeconfig.json");
      file << "{\"runtimeOptions\":{\"includedFrameworks\":[{\"name\":\"Microsoft.WindowsDesktop.App\",\"version\":\"8.0.31\"}]}}"; }
    check(!Bootstrap::Runtime(app.wstring(), {}).bundled);
    Pe(app / L"hostfxr.dll"); Pe(app / L"hostpolicy.dll");
    check(Bootstrap::Runtime(app.wstring(), {}).bundled);
    { std::ofstream file(app / L"DriveClub.Launcher.runtimeconfig.json"); file << "{\"runtimeOptions\":{\"frameworks\":[]}}"; }
    check(!Bootstrap::Runtime(app.wstring(), {}).available);
    check(std::wstring(Bootstrap::Target(true, false)) == L"DriveClub.Native.exe");
    check(std::wstring(Bootstrap::Target(false, true)) == L"DriveClub.Native.exe");
    check(std::wstring(Bootstrap::Target(false, false)) == L"DriveClub.Launcher.exe");
    const std::vector<std::wstring> arguments{L"", L"C:\\space folder\\кадр.png", L"a\"b", L"ends with slash\\", L"two\\\\\"quotes", L"$(literal); & text"};
    std::wstring command = Bootstrap::Quote(L"fixture.exe");
    for (const auto& arg : arguments) command += L" " + Bootstrap::Quote(arg);
    int count = 0; auto parsed = CommandLineToArgvW(command.c_str(), &count);
    check(parsed && count == int(arguments.size() + 1));
    for (size_t i = 0; i < arguments.size(); ++i) check(arguments[i] == parsed[i + 1]);
    LocalFree(parsed);
    std::vector<std::wstring> unique;
    Bootstrap::AddRoot(unique, L"C:\\dotnet\\"); Bootstrap::AddRoot(unique, L"c:\\DOTNET");
    check(unique.size() == 1);
    std::printf("PASS: %u native bootstrap checks (runtime, architecture, Wine routing, offline runtime, argument quoting)\n", checks);
}
