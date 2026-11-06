# Native plugins

A plugin is a DLL that runs inside the game. It can read and write the game's memory and hook any instruction of the game's code, so it can change what no patch can: game logic, physics values in use, extra logging, a developer overlay.

**A plugin runs with all the rights of the game and of the player's account. Install only plugins whose source you have read or whose author you trust.** The runtime loads plugins only when the environment variable `DC_PLUGINS=1` is set; without it the `plugins` folders are ignored.

## Installing one

Put the DLL in the folder `plugins` of a mods folder (`mods\plugins\my.dll` next to `DriveClub.exe`) and start the game with `DC_PLUGINS=1`. The log (`logs\driveclub_*.log`) has a line `Plugin: my.dll loaded (init returned 0)` for each plugin, and its own lines start with `Plugin:`.

## Writing one

`tools/modkit/examples/plugin-hello` has the header `dc_plugin.h` and a complete example. A plugin exports one function:

```c
#include "dc_plugin.h"
__declspec(dllexport) int dc_plugin_init(const DcPluginApi *api) { ... return 0; }
```

The API gives `log(text)`, `read(addr, out, length)`, `write(addr, data, length)` and `hook(addr, fn, user)`. A hook runs `fn(regs, user)` each time any game thread reaches the instruction at `addr`; `regs` holds the registers (rax to r15, rsp, rip), changes to them are applied when the callback returns, and the game then goes on with the instruction (setting `rip` to another address continues there instead and skips the instruction). The callback runs on the game's thread, so keep it short and do not call into the game.

Build for 64 bit Windows with any C compiler; no runtime library is needed:

```
clang -shared -O2 -o hello.dll hello.c
```

## Addresses

A guest address is `0x800000000` plus the offset in the game's code and data image, the one a disassembler shows for the console executable (the file `eboot.elf` the launcher prepares, loaded at 0). A hook address must be the first byte of an instruction. The kit's `dcmod lua` and the Lua API list ([lua-api.md](lua-api.md)) are the easier way to change game logic; use a plugin when the logic is native code.

## Checked in the game

The example plugin was loaded in a race, hooked the start of the game's resource lookup (guest `0x800780f30`, which all game threads call, over 120000 times in about 30 seconds), logged its registers, and read the code bytes at the hook (`55 48 89 e5`, a function prologue). The game ran normally to the start of the race. Writing memory was checked too: a second plugin overwrote the first 8 bytes of a message in the game's constants (`Couldn't` became `PLUGGED!`), and the game's log line then read `PLUGGED! find ExciterDsp params for vehicle(ferrari_458)`; `write` makes read only memory writable for the copy. Changing registers in a hook uses the same breakpoint code but was not tried in the game.

## Limits

- At most 160 hooks in total (the runtime's breakpoints are shared with its own tracing).
- A hook stops the game thread for a moment each time it is reached: do not hook code that runs thousands of times per frame unless the callback is trivial.
- Plugins are only supported on Windows.
- A hook that is hit by two threads at the same instant may be missed by one of them while the first steps over the instruction.
