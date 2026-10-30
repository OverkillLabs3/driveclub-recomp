#include "../gpu/shim/bbport_launch_ready.h"
#include <cassert>
#include <cstdio>

int main() {
    using namespace BbLauncherReady;
    const wchar_t* valid = L"Local\\DriveClub.FirstFrame.0123456789abcdef0123456789ABCDEF";
    assert(ValidName(valid));
    assert(!ValidName(L"Global\\DriveClub.FirstFrame.0123456789abcdef0123456789abcdef"));
    assert(!ValidName(L"Local\\DriveClub.FirstFrame.not-a-guid"));
    assert(!ValidName(L"Local\\DriveClub.FirstFrame.0123456789abcdef0123456789abcdeg"));
    HANDLE event = CreateEventW(nullptr, TRUE, FALSE, valid); assert(event);
    SetEnvironmentVariableW(L"BB_LAUNCH_READY_EVENT", valid);
    DWORD before=0,after=0;GetProcessHandleCount(GetCurrentProcess(),&before);
    OneShot state;
    state.Presented(false,false); assert(WaitForSingleObject(event,0)==WAIT_TIMEOUT);
    state.Presented(true,true); assert(WaitForSingleObject(event,0)==WAIT_TIMEOUT);
    state.Presented(true,false); assert(WaitForSingleObject(event,0)==WAIT_OBJECT_0);
    ResetEvent(event);
    for(int i=0;i<10000;i++)state.Presented(true,false);
    assert(WaitForSingleObject(event,0)==WAIT_TIMEOUT);
    GetProcessHandleCount(GetCurrentProcess(),&after);assert(before==after);
    OneShot absent;
    SetEnvironmentVariableW(L"BB_LAUNCH_READY_EVENT",nullptr);
    absent.Presented(true,false);assert(WaitForSingleObject(event,0)==WAIT_TIMEOUT);
    OneShot invalid;
    SetEnvironmentVariableW(L"BB_LAUNCH_READY_EVENT",L"Global\\unrelated");
    invalid.Presented(true,false);assert(WaitForSingleObject(event,0)==WAIT_TIMEOUT);
    SetEnvironmentVariableW(L"BB_LAUNCH_READY_EVENT",nullptr);
    CloseHandle(event);
    std::puts("PASS: real/reused/generated frame gating, one-shot signal, event validation, absent/invalid opt-in, no handle leak.");
}
