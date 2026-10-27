# Optional one-second collision penalties

The Windows launcher option **One-second collision penalties** under Gameplay → Patches is **off by default**. Enabling it keeps the native collision response while shortening the additional speed/acceleration penalty to one second after the latest trigger. Repeated contacts can restart that second. This is a duration change, not a one-second input delay.

Close the game, enable the option and launch again. To restore the original policy, close the game, uncheck it and relaunch. The former **Disable collision slowdown penalties** selection no longer applies: enable the renamed option explicitly if upgrading from that proposal.

Physical impacts, native initial wall-impact slowing, collision event dispatch, scoring paths and contact friction remain. Corner-cut penalties are unchanged. Preserving the event producer does not guarantee a message or points deduction on every contact: the native game decides those outcomes.

## Patch boundaries

The existing community-patch compiler checks all four OriginalHash guards against the player's original DriveClub 1.28 ELF. If any guard differs, the entire option is rejected. No game archive is modified. XML addresses include the format's 0x400000 base.

| Guest offset | Guarded span | Enabled behavior |
| --- | --- | --- |
| +0x644a20 | 10 bytes | Original wall-request setter, including its event/physical request, retained |
| +0x644a30 | 54 bytes | World-contact setter retained; timer at component +0x20 becomes 1 s |
| +0x644a70 | 54 bytes | Car-contact setter retained; timer at component +0x28 becomes 1 s |
| +0x6446a6 | 20 bytes | Wall tail at component +0x18 becomes 1 s after the native initial impact calculation |

The wall setter write is intentionally identical to the original instructions: it explicitly restores and guards the native entry used in the validated variant. Neither setter returns early. The world/car writes change only their timer immediate. The wall-tail assignment replaces the native variable duration, which can reach several seconds, without changing the preceding impact calculation or subsequent update logic.

Correction to the earlier proposal's documentation: +0x644a30 is the world-contact setter, not CornerCut. The separate CornerCut setter is +0x644ad0 and its timers at +0x30/+0x34 are unchanged.

## Verification

Using MSYS2 CLANG64 from the repository root:

```sh
mkdir -p out
clang++ -O2 -std=c++20 -static -o out/collision_penalty_test.exe tools/collision_penalty_test.cpp
clang -O2 -std=c11 -static -o out/collision_response_test.exe tools/collision_response_test.c
out/collision_penalty_test.exe '/path/to/your/1.28/eboot.elf' patches/DriveClub.xml out/collision-only.bin
out/collision_response_test.exe '/path/to/your/1.28/eboot.elf' out/collision-only.bin
```

The first test uses the actual launcher XML parser/compiler: four enabled writes, zero disabled writes and atomic rejection of a mismatch at each site. An optional pair of arguments, selection-file and validated-patches.bin, compares the complete compilation against a locally validated production patch. These private files are not distributed.

The second test executes the native setters, updater and collision event producer from the player's own ELF in private memory. It verifies initial wall/world/car slowing, expiration of speed scaling and acceleration restriction after one second, retained car/world events, and a CornerCut penalty continuing beyond one second. The updater/event producer signatures are checked before execution. Native physics wake/velocity backends are mocked, powf is bridged to the host, and the event consumer records output instead of executing the score or HUD backend. It starts no game, SDL or force-feedback device. It does not measure actual Fame totals or HUD rendering.

The launcher was compiled and the same four writes were compared byte-for-byte against the local player-validated variant. The hardware owner accepted the driving feel in a manual test. This is not exhaustive validation across tracks, cars or race modes.

## Scope

No SDL, FFB, intro pacing or friction changes are included. This remains independent of the T300 fixes in PR #235. Development used Codex AI assistance and hands-on driving tests by the hardware owner. No commercial ELF, save or diagnostic memory dump is distributed.
