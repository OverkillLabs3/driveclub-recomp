# In-game exit confirmation

The dialog is rendered by the existing ImGui/Vulkan overlay in the game
swapchain. It has no separate desktop window. The current design intentionally
has no logo: a centered question, Cancel/Exit buttons, and device prompts.

## Root-menu detection (DRIVECLUB 1.28)

`runtime_exit.c` checks game UI data, not the host overlay's `menu_open` and not
the absence of a race. A dialog can open only when:

- Selector is active;
- its foreground stack has exactly one frame, whose registry name is
  `x_main.frontend`;
- overlay and sit-on-top stacks are empty;
- the game state identifies `frontendflow/Front` or `frontendflow/NoDiorama`;
- the ImGui renderer is initialized and the game is not displaying an IME dialog.

The bindings and instruction bytes are validated before using revision-specific
offsets. Unsupported layouts fail closed. Selector is at image+0x189f8c8;
its three stacks are at +0x240/+0x248/+0x250. Stack frames are 0xb0 bytes;
the frame's +8 points to the registered page. The game's own Selector.PushPage
binding uses that registry and the name at node+0x38. GameState.GetCurrent is
used for the additional state check. No proprietary executable or Lua data is
included in this checkout.

## Input

The pure state machine is in `runtime_exit_state.c`. The normal pad sampler
continues sampling while captured so releases can be observed. Actions use the
existing logical Cross/Circle/d-pad outputs after user bindings have been applied.
SDL physical buttons are used only to identify the active device and its prompts.

Back/Esc must be released before another opening or cancellation. Returning from
a submenu while holding Back cannot open the dialog. Sticks have separate engage
and neutral thresholds; held buttons do not repeat. After cancellation the game
receives neutral input until held controls are released. Mouse bindings cannot
also activate a logical Confirm underneath a modal pointer click. Device changes
change prompts without resetting the selected button.

The window thread owns relative-mouse mode and cursor visibility. The mouse-stick
sampler requests its normal mode; a modal temporarily overrides it and restores
the requested mode after cancellation and release.

## Normal shutdown and prerequisites

Confirmed Exit waits for writable save-data mounts to finish, then calls the
game's actual `GameState.Quit` binding. The game stops its systems and returns
from its main function; guest atexit handlers run through the existing runtime.
`GameState.EndGame` is a gameplay event and does not exit the application.

This exposed previously unused cleanup paths, fixed here:

1. The WebAPI_0 offline initialization retry at +0x7d6e70 did not observe its
   owner's stop flag (+0x21), so the main thread could never join it. A validated
   startup hook preserves cleanup/retry behavior during play and exits that
   retry when the existing stop request is set.
2. Orbis `catchReturnFromMain` raises a debug exception even for status 0. The PC
   implementation allows the following CRT instruction to call guest `exit`
   with the original status.
3. Host presentation threads must stop before their presenter/port owners are
   destroyed. GPU/window/kernel-service threads are explicitly joined, and
   rasterizer callbacks are removed before their owners disappear.
4. ImGui Vulkan resources and DLSS/NGX state must be released before destroying
   their Vulkan device. Later process-exit cleanup is idempotent.

The confirmation path uses ordinary `exit` after this cleanup. The existing
window-close/emergency-exit paths are separate and are not used by the dialog.

## Regression checks

From the repository root, with the MSYS2 CLANG64 compiler:

```
clang -std=c11 -Wall -Wextra -Werror -I. tools/exit_dialog_test.c src/runtime_exit_state.c -o out/exit_dialog_test.exe
out/exit_dialog_test.exe
```

These exercise root-only opening, submenu return with held Back, opening-edge
suppression, default Cancel, held Confirm after cancellation, short keyboard
pulses, stick drift/hysteresis, navigation, pointer activation, and scene changes.

The Windows SDL sampler test also exercises a real in-process virtual controller
through `runtime_pad.c`, including swapped Confirm/Back assignments and stick axes:

```
clang -O1 -std=c11 -ffunction-sections -fdata-sections -I. tools/exit_pad_test.c src/runtime_exit_state.c -lSDL3 -lwinpthread -Wl,--gc-sections -o out/exit_pad_test.exe
out/exit_pad_test.exe
```

The test's virtual device exists only inside that process and does not send
controller input to Windows or other applications.

For game integration, use a separate copy of the user directory with `--user`.
The runtime's existing `BB_PAD_FILE` interface can hold/release logical controls
without desktop input injection. Check both windowed and fullscreen rendering,
mouse capture/restoration, remapped controller buttons, device changes, and a
complete exit with status 0. Preserve the original save/config files; never use
the user's active save directory as an automated test fixture.

Verified on Windows with the 1.28 prepared image: windowed and fullscreen
dialog, Esc/Enter, mouse cancellation and release, submenu return with held
Back, held Confirm after cancellation, stick drift, and complete exits with
status 0 both without AA and with DLAA. All 191 original save files matched
their pre-test SHA-256 hashes. Physical PlayStation controllers and Linux have
not been exercised; controller routing was covered by the SDL virtual test.
