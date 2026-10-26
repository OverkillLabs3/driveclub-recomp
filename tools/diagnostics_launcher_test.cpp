#define wWinMain diagnostics_unused_wWinMain
#include "../launcher/driveclub_launcher.cpp"
#undef wWinMain
#include <cassert>
int main() {
    g_home = L"diagnostics-routing-test";
    g_runtime = g_home + L"\\runtime";
    SetEnvironmentVariableW(L"BB_DIAGNOSTIC_RUNTIME", nullptr);
    const auto ordinary = GameExe();
    SetEnvironmentVariableW(L"BB_DIAGNOSTIC_RUNTIME", L"DriveClubGame.diagnostics.exe");
    assert(GameExe() == g_runtime + L"\\DriveClubGame.diagnostics.exe");
    for (const wchar_t* invalid : {L"..\\evil.exe", L"C:\\evil.exe", L"unknown.exe"}) {
        SetEnvironmentVariableW(L"BB_DIAGNOSTIC_RUNTIME", invalid);
        assert(GameExe() == ordinary);
    }
    std::wstring too_long(100, L'x');
    SetEnvironmentVariableW(L"BB_DIAGNOSTIC_RUNTIME", too_long.c_str());
    assert(GameExe() == ordinary);
    SetEnvironmentVariableW(L"BB_DIAGNOSTIC_RUNTIME", nullptr);
    assert(GameExe() == ordinary);
    puts("PASS: explicit sibling diagnostic executable only; invalid paths and oversized values fall back; clearing selection restores ordinary routing.");
}
