#include <windows.h>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <string>

// Integration-test child only. No game, .NET, shell, network or input injection.
int wmain(int argc, wchar_t** argv) {
    const wchar_t* path = _wgetenv(L"DC_BOOTSTRAP_TEST_RECORD");
    if (!path) return 99;
    FILE* file = _wfopen(path, L"wb"); if (!file) return 98;
    auto value = [&](const std::wstring& s) {
        const std::uint32_t n = std::uint32_t(s.size());
        fwrite(&n, 4, 1, file); fwrite(s.data(), 2, n, file);
    };
    for (int i = 1; i < argc; ++i) value(argv[i]);
    wchar_t cwd[32768]; GetCurrentDirectoryW(32768, cwd); value(cwd);
    const auto root = _wgetenv(L"DOTNET_ROOT_X64"); value(root ? root : L"");
    fclose(file);
    return 17;
}
