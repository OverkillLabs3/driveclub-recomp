// Test-only joystick provider. Never copy this DLL into a real game package.
#include <cstdint>
#define API extern "C" __declspec(dllexport)
API bool SDL_InitSubSystem(uint32_t) { return true; }
API uint32_t SDL_WasInit(uint32_t flags) { return flags; }
API uint32_t* SDL_GetJoysticks(int* count) { static uint32_t id=1;*count=1;return &id; }
API void* SDL_OpenJoystick(uint32_t) { return reinterpret_cast<void*>(1); }
API void SDL_CloseJoystick(void*) {}
API const char* SDL_GetJoystickName(void*) { return "FixtureWheel"; }
API int SDL_GetNumJoystickButtons(void*) { return 8; }
API bool SDL_GetJoystickButton(void*,int) { return false; }
API void SDL_UpdateJoysticks() {}
API void SDL_free(void*) {}
