#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#include "bootstrap.h"
#include <shellapi.h>
#include <string>

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    wchar_t file[32768];
    DWORD size = GetModuleFileNameW(nullptr, file, 32768);
    if (!size || size >= 32768) return 1;
    std::wstring root(file, size);
    root.resize(root.find_last_of(L"\\/"));
    bool check = false, native = false;
    std::vector<std::wstring> arguments;
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) return 1;
    for (int i = 1; i < argc; ++i) {
        const std::wstring arg = argv[i];
        if (arg == L"--check-only") check = true;
        else if (arg == L"--native") native = true;
        else arguments.push_back(arg);
    }
    LocalFree(argv);
    const bool fallback = Bootstrap::Wine() || native;
    const auto target = Bootstrap::Join(root, Bootstrap::Target(fallback, false));
    auto error = [&](const wchar_t* message) {
        if (!check) MessageBoxW(nullptr, message, L"DRIVECLUB", MB_OK | MB_ICONERROR);
        return 1;
    };
    if (!Bootstrap::File(target)) return error(L"Extract the complete launcher package. A launcher program is missing.");
    // Wine never enters the .NET/PowerShell path. --native also permits an
    // explicit fallback on Windows without requiring .NET.
    if (fallback) return check ? 0 : Bootstrap::Start(target, arguments, root);
    auto state = Bootstrap::Runtime(root, Bootstrap::RuntimeRoots());
    if (check) return state.available ? 0 : 2;
    if (!state.available) {
        const auto script = Bootstrap::Join(root, L"Install-Launcher.ps1");
        if (!Bootstrap::File(script)) return error(L"Install-Launcher.ps1 is missing. Extract the complete package, or use DriveClub.Native.exe.");
        wchar_t system[32768];
        if (!GetSystemDirectoryW(system, 32768)) return error(L"Windows PowerShell could not be located. You can use DriveClub.Native.exe.");
        const auto powershell = Bootstrap::Join(system, L"WindowsPowerShell\\v1.0\\powershell.exe");
        const int result = Bootstrap::Start(powershell,
            {L"-NoProfile", L"-ExecutionPolicy", L"Bypass", L"-WindowStyle", L"Hidden", L"-File", script, L"-InstallOnly"}, root, true);
        if (result < 0) return error(L"Windows PowerShell could not be started. You can use DriveClub.Native.exe.");
        if (result != 0) return result; // cancelled elevation, failure, or restart requested
        state = Bootstrap::Runtime(root, Bootstrap::RuntimeRoots());
        if (!state.available) return 0; // permission declined; do not launch/install anything else
    }
    if (!state.bundled) {
        SetEnvironmentVariableW(L"DOTNET_ROOT_X64", state.root.c_str());
        SetEnvironmentVariableW(L"DOTNET_ROOT", state.root.c_str());
    }
    const int result = Bootstrap::Start(target, arguments, root);
    return result < 0 ? error(L"The launcher could not be started. You can use DriveClub.Native.exe.") : result;
}
