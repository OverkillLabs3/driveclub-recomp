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
The dialog uses the same validated native regional predicate as the game's
input update and button help: Asian Circle-confirm layouts automatically use
Circle to select and Cross to go back. No host-language heuristic or manual
setting is involved. Controller remaps are then applied to the displayed prompts.

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

5. After leaving an event, cached Lua file wrappers can retain subscriptions to
   already destroyed file signals. Native teardown then recursively tries to
   initialize a missing bucket zero until its stack overflows. A validated stream
   destructor trampoline expires those dead subscriptions only after confirmed
   shutdown. The original destructor still closes live subscriptions, owned
   buffers and the stream itself. Normal gameplay takes the original cleanup path.

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

## Shutdown subscription regression

`tools/exit_shutdown_test.c` exercises the real wrapper against live, expired,
missing and already disconnected signal owners, checks that ordinary gameplay
leaves the connections alone, and verifies delegation to the native destructor.

```
clang -O1 -std=c11 -Wall -Wextra -Werror -ffunction-sections -fdata-sections -I. tools/exit_shutdown_test.c src/runtime_exit_state.c -Wl,--gc-sections -lwinpthread -o out/exit_shutdown_test.exe
out/exit_shutdown_test.exe
```

On Windows/DRIVECLUB 1.28, the 0.4.0 event -> pause -> Quit event -> Home ->
confirmed Exit path reproduced stack exhaustion at native +0xe7eb40, reached
from cached Lua stream teardown. The guarded path returned from game main with
status 0 and completed host GPU shutdown. No timeout termination is used.

## Debug-exception shutdown order

A separate user dump from the 0.4.0 preview showed main waiting for the swap
thread in `StopPresenting()`, while `bb:Present` spun in `BbCopy::WaitAsync()`.
The copy pool already had `stop=true`, no workers, no pending copies and zero
epochs: its static destructor had run before the GPU's older atexit callback.
`WaitAll()` requires one open epoch, so using that destroyed pool cannot finish.

The guest debug-exception handler now calls the idempotent `bbgpu_shutdown()`
before `exit(23)`, while host statics are still alive. This retains status 23 and
the existing fatal-exception behavior. It does not resume guest execution,
skip pending copies, detach threads or introduce a forced-exit timeout.

The subprocess regression uses the real handler and a C++ static-resource
fixture with production registration order (GPU atexit before pool creation):

```
clang -O1 -std=c11 -I. -c tools/exit_exception_order_test.c -o out/exit_exception_order_test.o
clang++ -O1 -std=c++20 tools/exit_exception_order_fixture.cpp out/exit_exception_order_test.o -lwinpthread -o out/exit_exception_order_test.exe
out/exit_exception_order_test.exe
```

Expected status: **23**, with `GPU_STOPPED_WITH_POOL_ALIVE` followed by
`POOL_DESTROYED_AFTER_GPU`. Restoring the old handler fails with status 91
(`pool destroyed before presentation stopped`). The fixture checks ordering;
the affected user has confirmed that the follow-up resolves the hang on their
graphics configuration.
