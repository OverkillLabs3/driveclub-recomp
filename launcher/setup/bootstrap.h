#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <algorithm>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace Bootstrap {
inline std::wstring Join(const std::wstring& root, const std::wstring& name) {
    return root + (root.empty() || root.back() == L'\\' || root.back() == L'/' ? L"" : L"\\") + name;
}
inline bool File(const std::wstring& path) {
    const DWORD a = GetFileAttributesW(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}
inline std::wstring Environment(const wchar_t* name) {
    const DWORD n = GetEnvironmentVariableW(name, nullptr, 0);
    if (!n) return {};
    std::wstring value(n, L'\0');
    const DWORD read = GetEnvironmentVariableW(name, value.data(), n);
    if (!read || read >= n) return {};
    value.resize(read);
    return value;
}
inline bool X64Image(const std::wstring& path) {
    std::ifstream file(path.c_str(), std::ios::binary | std::ios::ate);
    if (!file) return false;
    const auto size = file.tellg();
    if (size < 64) return false;
    file.seekg(0);
    std::uint16_t magic = 0, machine = 0;
    std::uint32_t offset = 0, signature = 0;
    file.read(reinterpret_cast<char*>(&magic), 2);
    file.seekg(60); file.read(reinterpret_cast<char*>(&offset), 4);
    if (magic != 0x5a4d || offset < 64 || std::uint64_t(offset) + 6 > std::uint64_t(size)) return false;
    file.seekg(offset); file.read(reinterpret_cast<char*>(&signature), 4);
    file.read(reinterpret_cast<char*>(&machine), 2);
    return bool(file) && signature == 0x4550 && machine == 0x8664;
}
inline bool StableEight(const std::wstring& version, unsigned& patch) {
    if (version.size() <= 4 || version.compare(0, 4, L"8.0.") != 0) return false;
    patch = 0;
    for (size_t i = 4; i < version.size(); ++i) {
        if (version[i] < L'0' || version[i] > L'9' || patch > 1000000) return false;
        patch = patch * 10 + unsigned(version[i] - L'0');
    }
    return true;
}
inline int BestPatch(const std::wstring& directory, const wchar_t* native_file) {
    WIN32_FIND_DATAW found{};
    const HANDLE h = FindFirstFileW(Join(directory, L"*").c_str(), &found);
    if (h == INVALID_HANDLE_VALUE) return -1;
    int best = -1;
    do {
        unsigned patch = 0;
        if ((found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && StableEight(found.cFileName, patch) &&
            X64Image(Join(Join(directory, found.cFileName), native_file))) best = std::max(best, int(patch));
    } while (FindNextFileW(h, &found));
    FindClose(h);
    return best;
}
inline void AddRoot(std::vector<std::wstring>& roots, std::wstring value) {
    while (!value.empty() && (value.back() == L'\\' || value.back() == L'/')) value.pop_back();
    if (!value.empty() && std::none_of(roots.begin(), roots.end(), [&](const auto& r) {
        return _wcsicmp(r.c_str(), value.c_str()) == 0;
    })) roots.push_back(std::move(value));
}
inline std::vector<std::wstring> RuntimeRoots() {
    std::vector<std::wstring> roots;
    AddRoot(roots, Environment(L"DOTNET_ROOT_X64"));
    AddRoot(roots, Environment(L"DOTNET_ROOT"));
    for (HKEY hive : {HKEY_LOCAL_MACHINE, HKEY_CURRENT_USER}) {
        HKEY key{};
        if (RegOpenKeyExW(hive, L"SOFTWARE\\dotnet\\Setup\\InstalledVersions\\x64", 0,
                          KEY_READ | KEY_WOW64_64KEY, &key) != ERROR_SUCCESS) continue;
        wchar_t value[32768]{}; DWORD bytes = sizeof(value), type = 0;
        const auto status = RegQueryValueExW(key, L"InstallLocation", nullptr, &type,
                                             reinterpret_cast<BYTE*>(value), &bytes);
        RegCloseKey(key);
        if (status == ERROR_SUCCESS && type == REG_SZ && bytes >= sizeof(wchar_t) && bytes <= sizeof(value) &&
            value[bytes / sizeof(wchar_t) - 1] == 0) AddRoot(roots, value);
    }
    for (const wchar_t* name : {L"ProgramW6432", L"ProgramFiles"}) {
        const auto base = Environment(name);
        if (!base.empty()) AddRoot(roots, Join(base, L"dotnet"));
    }
    return roots;
}
struct RuntimeState { bool available = false, bundled = false; std::wstring root; };
inline RuntimeState Runtime(const std::wstring& directory, const std::vector<std::wstring>& roots) {
    std::ifstream config(Join(directory, L"DriveClub.Launcher.runtimeconfig.json").c_str(), std::ios::binary);
    const std::string text((std::istreambuf_iterator<char>(config)), {});
    if (text.find("\"includedFrameworks\"") != std::string::npos &&
        text.find("Microsoft.WindowsDesktop.App") != std::string::npos &&
        X64Image(Join(directory, L"coreclr.dll")) && X64Image(Join(directory, L"PresentationNative_cor3.dll")) &&
        X64Image(Join(directory, L"hostfxr.dll")) && X64Image(Join(directory, L"hostpolicy.dll"))) {
        return {true, true, directory};
    }
    for (const auto& root : roots) {
        const int core = BestPatch(Join(root, L"shared\\Microsoft.NETCore.App"), L"coreclr.dll");
        const int desktop = BestPatch(Join(root, L"shared\\Microsoft.WindowsDesktop.App"), L"PresentationNative_cor3.dll");
        if (desktop >= 0 && core >= desktop) return {true, false, root};
    }
    return {};
}
inline bool Wine() {
    const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    return ntdll && GetProcAddress(ntdll, "wine_get_version") != nullptr;
}
inline const wchar_t* Target(bool wine, bool native) {
    return wine || native ? L"DriveClub.Native.exe" : L"DriveClub.Launcher.exe";
}
// Quote for the Windows argv parser, including empty arguments, embedded quotes
// and trailing backslashes. No shell or command interpreter is involved.
inline std::wstring Quote(const std::wstring& value) {
    std::wstring out = L"\""; size_t slashes = 0;
    for (wchar_t c : value) {
        if (c == L'\\') { ++slashes; continue; }
        out.append(c == L'"' ? slashes * 2 + 1 : slashes, L'\\');
        out.push_back(c); slashes = 0;
    }
    out.append(slashes * 2, L'\\'); out.push_back(L'"');
    return out;
}
inline int Start(const std::wstring& exe, const std::vector<std::wstring>& args,
                 const std::wstring& directory, bool hidden = false) {
    std::wstring command = Quote(exe);
    for (const auto& arg : args) command += L" " + Quote(arg);
    if (command.size() >= 32767) return -1;
    STARTUPINFOW si{}; si.cb = sizeof(si); si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = hidden ? SW_HIDE : SW_SHOWNORMAL;
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_UNICODE_ENVIRONMENT,
                         nullptr, directory.c_str(), &si, &pi)) return -1;
    CloseHandle(pi.hThread);
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1; GetExitCodeProcess(pi.hProcess, &code); CloseHandle(pi.hProcess);
    return int(code);
}
}
