// One-shot Windows launcher handoff. No captures, polling, or diagnostic build dependency.
#pragma once
#ifdef _WIN32
#include <windows.h>
#include <atomic>
#include <cstdio>
#include <cwchar>

namespace BbLauncherReady {
inline constexpr char Marker[] = "LauncherReady: first game frame submitted";
inline constexpr wchar_t Prefix[] = L"Local\\DriveClub.FirstFrame.";
inline bool ValidName(const wchar_t* name) {
    constexpr size_t prefix = sizeof(Prefix) / sizeof(wchar_t) - 1;
    if (std::wcslen(name) != prefix + 32 || std::wcsncmp(name, Prefix, prefix) != 0) return false;
    for (size_t i = prefix; i < prefix + 32; ++i)
        if (!((name[i] >= L'0' && name[i] <= L'9') || (name[i] >= L'a' && name[i] <= L'f') ||
              (name[i] >= L'A' && name[i] <= L'F'))) return false;
    return true;
}
class OneShot {
    std::atomic<bool> attempted{false};
public:
    void Presented(bool game_frame, bool reused) {
        if (!game_frame || reused || attempted.load(std::memory_order_relaxed) ||
            attempted.exchange(true, std::memory_order_relaxed)) return;
        wchar_t name[128]{};
        const DWORD count = GetEnvironmentVariableW(L"BB_LAUNCH_READY_EVENT", name, 128);
        if (!count || count >= 128 || !ValidName(name)) return;
        HANDLE event = OpenEventW(EVENT_MODIFY_STATE, FALSE, name);
        if (!event) return;
        if (SetEvent(event)) std::puts(Marker);
        CloseHandle(event);
    }
};
inline void GameFramePresented(bool game_frame, bool reused) {
    static OneShot state;
    state.Presented(game_frame, reused);
}
}
#else
namespace BbLauncherReady { inline void GameFramePresented(bool, bool) {} }
#endif
