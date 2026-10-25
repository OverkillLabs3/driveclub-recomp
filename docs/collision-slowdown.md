# Optional collision slowdown removal

The Gameplay → Patches option **Disable collision slowdown penalties** is off by default. It skips the additional wall-impact slowdown request and the car-contact penalty timer in DriveClub 1.28. The existing corner-cut penalty path and the physical collision engine are left in place. No game dump files are changed.

## Player choice and reverting

This is an optional preference for players who enjoy more relaxed driving and find the additional contact slowdown frustrating. It does not change the intended default balance: leaving the checkbox unchecked keeps the original behavior.

1. Close the running game and open the Windows launcher (`DriveClub.exe`).
2. Open **Gameplay → Patches** and check **Disable collision slowdown penalties**.
3. Press **Play** to apply it to the next game process.

To restore the original behavior, close the game, uncheck the same option, and launch again. This is a launcher option, not a new switch in the PS4 game's own settings menu. It does not take effect midway through an existing race.

Cars still physically hit walls, trackside objects and other cars, and can lose speed from the impact itself. This option removes the extra gameplay slowdown, not collision physics. Fame deductions, scoring rules and some collision notifications may still occur. Corner-cut penalties remain enabled. It is not a blanket removal of every penalty.

## Patch boundaries

The launcher uses the existing community-patch compiler. Each line contains the full original setter's FNV-1a hash; a mismatch at either site rejects the whole option. Addresses in the XML include the patch format's `0x400000` base.

| Guest offset | Purpose | Guarded span |
| --- | --- | --- |
| `+0x644a20` | Arms the extra wall-impact slowdown flag at component `+0x0c` and stores its input at `+0x10` | 10 bytes |
| `+0x644a70` | Starts the car-contact penalty at component `+0x24/+0x28`, with a three-second timer and `+0x3f` notification flag | 54 bytes |

Both patch values preserve every original byte except the function-entry byte, which becomes `ret`. This is a void setter in each case. The independent corner-cut setter at `+0x644a30` is not patched, nor is the wall/contact event dispatch or the Havok collision response. Existing scoring rules and penalties for cutting corners are outside this option's scope.

The wall setter is reached through `+0x640ba0` from the contact-event handler at `+0x4c17a0`. The car-contact setter is reached through `+0x640c00` from both car branches in `+0x4c19f0`. The corner-cut setter is reached through `+0x640bd0` from the off-road processing at `+0xc4b687`.

## Verification

Build the isolated test with MSYS2 CLANG64 from the repository root:

```sh
clang++ -O2 -std=c++20 -static -o out/collision_penalty_test.exe tools/collision_penalty_test.cpp
out/collision_penalty_test.exe '/path/to/your/1.28/eboot.elf' patches/DriveClub.xml
```

The test reads the original instructions from the player's own extracted ELF; no game image is included. It uses the actual XML parser and patch compiler, verifies off/on behavior and atomic rejection of a mismatch at either site, and executes only the three small isolated setters against private test memory. It checks the original wall request, the original three-second contact timer, the absence of both requests after patching, preservation of the corner-cut timer, and that only two image bytes differ. It never initializes SDL or any force-feedback device.

Also build the launcher using the command in `packaging/package.sh`. In a game smoke test, use `BB_FFB_DRY=1` for unattended runs and confirm the option is present in the settings log and the two guarded writes are included in `data/prep/patches.bin`. A player should still confirm the driving feel after wall contact; the isolated test verifies the setters and patch boundaries, not every track or race mode.

## Separation from the FFB correction

This change has no SDL or force-feedback code. It can be proposed independently of the T300 infinite-duration dependency fix. On the local PR210 installation, the corrected SDL DLL is retained while this option is enabled.

Development and diagnosis used Codex AI assistance. The hardware owner performed manual driving tests. This is a preference proposal for maintainer review, not a claim that the original game balance is a bug.
