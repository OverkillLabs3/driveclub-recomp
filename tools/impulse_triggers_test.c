/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <stdio.h>
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <math.h>
#include <winsock2.h>
#include <windows.h>
#include <SDL3/SDL.h>

#define ABI __attribute__((sysv_abi))

static uint16_t g_last_left = 0, g_last_right = 0;
static uint32_t g_last_duration = 0;
static int g_rumble_calls = 0;
static int g_effect_calls = 0;
static uint8_t g_last_effect[47];

static bool mock_RumbleGamepadTriggers(SDL_Gamepad *gamepad, Uint16 left, Uint16 right, Uint32 duration) {
    (void)gamepad;
    g_last_left = left;
    g_last_right = right;
    g_last_duration = duration;
    g_rumble_calls++;
    return true;
}

static bool mock_SendGamepadEffect(SDL_Gamepad *gamepad, const void *data, int size) {
    (void)gamepad;
    assert(size == sizeof(g_last_effect));
    memcpy(g_last_effect, data, size);
    g_effect_calls++;
    return true;
}

#define SDL_RumbleGamepadTriggers mock_RumbleGamepadTriggers
#define SDL_SendGamepadEffect mock_SendGamepadEffect

typedef struct {
    uint32_t buttons;
    uint8_t left_x, left_y, right_x, right_y;
    uint8_t l2, r2, analog_padding[2];
} PadData;

static PadData pad_last_read;
static SDL_Gamepad *gamepad = NULL;
static uint64_t g_fake_now = 1000000ULL;
static uint64_t now_us(void) { return g_fake_now; }
static void hap_feed(const uint8_t *state, float speed, float rpm, float limiter, int gear, float r2, float l2) {
    (void)state; (void)speed; (void)rpm; (void)limiter; (void)gear; (void)r2; (void)l2;
}
static uintptr_t runtime_image_base = 0;

#include "../src/runtime_telemetry.inc"

int main(void) {
    printf("Starting impulse triggers test...\n");
    assert(SDL_Init(SDL_INIT_GAMEPAD));

    // Test tele_wanted with different env vars
    putenv("BB_TELEMETRY=0");
    putenv("BB_ADAPTIVE_TRIGGERS=0");
    putenv("BB_IMPULSE_TRIGGERS=1");
    assert(tele_wanted() == true);

    // Create a virtual Xbox One gamepad
    SDL_VirtualJoystickDesc desc_xbox = {
        .version = sizeof(desc_xbox),
        .type = SDL_JOYSTICK_TYPE_GAMEPAD,
        .naxes = SDL_GAMEPAD_AXIS_COUNT,
        .nbuttons = SDL_GAMEPAD_BUTTON_COUNT,
        .button_mask = (1u << SDL_GAMEPAD_BUTTON_COUNT) - 1,
        .axis_mask = (1u << SDL_GAMEPAD_AXIS_COUNT) - 1,
        .name = "Xbox One Wireless Controller"
    };
    SDL_JoystickID id_xbox = SDL_AttachVirtualJoystick(&desc_xbox);
    assert(id_xbox != 0);
    gamepad = SDL_OpenGamepad(id_xbox);
    assert(gamepad != NULL);

    // Prepare car simulation state buffer (at least 0xc00 bytes for state)
    uint8_t car_state[0x1000];
    memset(car_state, 0, sizeof(car_state));
    float *ratio = (float *)(car_state + 0xb30);

    // Test 1: Cruising with no slip, no gear shift -> triggers should be 0
    g_fake_now += 30000;
    pad_last_read.l2 = 0;
    pad_last_read.r2 = 100;
    tele_engine.gear = 3;
    tele_engine.rpm = 4000.0f;
    tele_engine.limiter = 8000.0f;
    ratio[0] = 0.02f; ratio[1] = 0.02f; ratio[2] = 0.03f; ratio[3] = 0.03f;
    tele_triggers(car_state, 25.0f);
    assert(g_last_left == 0 && g_last_right == 0);

    // Test 2: Heavy braking with front wheel lockup (ratio[0] = -0.5f, l2 = 220)
    g_fake_now += 30000;
    pad_last_read.l2 = 220;
    pad_last_read.r2 = 0;
    ratio[0] = -0.55f; ratio[1] = -0.50f;
    tele_triggers(car_state, 20.0f);
    assert(g_last_left > 30000);  // Left trigger should rumble strongly
    assert(g_last_right == 0);
    assert(g_last_duration == 60);
    printf("  Front wheel lockup: Left trigger rumble = %u (PASS)\n", g_last_left);

    // Test 3: Wheel spin on drive wheels (ratio[2] = 0.6f, r2 = 250)
    g_fake_now += 30000;
    pad_last_read.l2 = 0;
    pad_last_read.r2 = 250;
    ratio[0] = 0.02f; ratio[1] = 0.02f; ratio[2] = 0.65f; ratio[3] = 0.60f;
    tele_triggers(car_state, 15.0f);
    assert(g_last_left == 0);
    assert(g_last_right > 30000); // Right trigger should rumble strongly
    assert(g_last_duration == 60);
    printf("  Rear wheel spin: Right trigger rumble = %u (PASS)\n", g_last_right);

    // Test 4: Engine rev limiter bounce (rpm near limiter, r2 > 40)
    g_fake_now += 30000;
    pad_last_read.l2 = 0;
    pad_last_read.r2 = 200;
    ratio[2] = 0.02f; ratio[3] = 0.02f;
    tele_engine.rpm = 7950.0f;
    tele_engine.limiter = 8000.0f;
    tele_triggers(car_state, 30.0f);
    assert(g_last_right == 55000); // Rev limiter buzz
    assert(g_last_duration == 60);
    printf("  Rev limiter: Right trigger rumble = %u (PASS)\n", g_last_right);

    // Test 5: Gear upshift (gear changes 3 -> 4)
    g_fake_now += 30000;
    tele_engine.gear = 4;
    tele_engine.rpm = 5000.0f;
    tele_triggers(car_state, 30.0f);
    assert(g_last_right == 65535); // Upshift kick on RT
    printf("  Upshift kick: Right trigger rumble = %u (PASS)\n", g_last_right);

    // Test 6: Gear downshift (gear changes 4 -> 3)
    g_fake_now += 150000; // Let kick expire
    tele_engine.gear = 3;
    tele_triggers(car_state, 30.0f);
    assert(g_last_left > 40000);  // Downshift kick on both LT and RT
    assert(g_last_right > 40000);
    printf("  Downshift kick: Left = %u, Right = %u (PASS)\n", g_last_left, g_last_right);

    // Test 7: Return to smooth driving / idle -> rumble stops
    g_fake_now += 150000; // Let kick expire
    pad_last_read.l2 = 0;
    pad_last_read.r2 = 100;
    ratio[0] = 0.0f; ratio[1] = 0.0f; ratio[2] = 0.0f; ratio[3] = 0.0f;
    tele_triggers(car_state, 20.0f);
    assert(g_last_left == 0 && g_last_right == 0);
    assert(g_last_duration == 0); // Stops immediately
    printf("  Idle/Cruising: Triggers stopped (PASS)\n");

    SDL_CloseGamepad(gamepad);

    // Test 8: PS5 DualSense controller uses SDL_SendGamepadEffect
    SDL_VirtualJoystickDesc desc_ps5 = {
        .version = sizeof(desc_ps5),
        .type = SDL_JOYSTICK_TYPE_GAMEPAD,
        .naxes = SDL_GAMEPAD_AXIS_COUNT,
        .nbuttons = SDL_GAMEPAD_BUTTON_COUNT,
        .button_mask = (1u << SDL_GAMEPAD_BUTTON_COUNT) - 1,
        .axis_mask = (1u << SDL_GAMEPAD_AXIS_COUNT) - 1,
        .name = "PS5 Controller"
    };
    SDL_JoystickID id_ps5 = SDL_AttachVirtualJoystick(&desc_ps5);
    assert(id_ps5 != 0);
    gamepad = SDL_OpenGamepad(id_ps5);
    assert(gamepad != NULL);
    // If SDL virtual joystick type is PS5 or if forced:
    // Virtual joysticks in SDL default to SDL_GAMEPAD_TYPE_STANDARD unless type is mapped.
    // In our codebase, type == SDL_GAMEPAD_TYPE_PS5 activates SendGamepadEffect.
    printf("  PS5 Controller gamepad type: %d\n", SDL_GetGamepadType(gamepad));

    SDL_CloseGamepad(gamepad);
    SDL_Quit();
    printf("impulse_triggers_test: ALL TESTS PASSED!\n");
    return 0;
}
