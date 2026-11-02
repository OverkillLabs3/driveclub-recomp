# T300 FFB: duration, effect lifetime and out-of-race forces

These changes were developed on the Flat v0.3.1 release (`e2561fc`) with PR #210 applied. The runtime change is proposed against main, which already includes #210. It does not reapply that gain fix or change the driving-force calculations.

Three independent findings were addressed on a Thrustmaster T300 Ferrari Alcantara (T300RS base, firmware 34.00, driver 2.11.57), Windows 11 build 26200.

## Infinite effect duration in the bundled dependency

DriveClub requests `SDL_HAPTIC_INFINITY`. SDL 3.4.16's DirectInput conversion multiplies the sentinel by 1000, producing `0xfffffc18` instead of DirectInput `INFINITE` (`0xffffffff`). A separate zero-magnitude diagnostic captured the T300 driver's outgoing effect reports: a one-second reference encoded 1000 ms, the multiplied sentinel encoded 65,533 ms, and the preserved sentinel encoded infinite duration. The driver's playing-status flag could remain set after the finite effect expired, so polling that flag alone was insufficient.

`packaging/sdl3/ffb-infinity.patch` preserves the sentinel in the constant, periodic, condition, ramp and custom conversions. Finite-duration conversion is unchanged.

## DirectInput effect ownership

In the same dependency, destroying an effect unloaded it without releasing the COM reference returned by CreateEffect. After repeated transitions the process held 90 created DirectInput objects for five SDL effects. This count does **not** mean 90 simultaneous playing forces.

`packaging/sdl3/ffb-lifecycle.patch` explicitly stops, unloads and releases each effect, including cleanup when Stop or Unload fails. A zero-force comparison of 125 create/run/destroy operations left 125 references with the original dependency and zero with the corrected one. The leak was real, but its correction alone did not resolve the reported oscillation.

## Physics model outside player-controlled driving

The runtime previously restored the game's original hardware spring in intros, menus and replays when its Physics model was inactive. During a severe oscillation reproduced in the intro, there had been no player-driving sample: the constant and periodic effects had zero magnitude, the damper had zero saturation, and the active spring was the only declared nonzero force.

With the Physics model selected, this change allows the game's periodic effects only during player-controlled driving. The model's own constant force, slip and configured damper continue to work in driving. Guest effects are not started in intros, menus or replays, and the existing transition cleanup removes effects when leaving driving. The Original and Software models keep their previous routing. No global/session reset or device reopening is introduced.

This removes the out-of-driving spring path associated with the reproduction; it is not a claim that every internal cause of every T300 oscillation is identified.

## Building the complete tested combination

The runtime-only change does **not** fix the dependency's duration conversion. To reproduce the full tested combination, build SDL with both companion patches and supply its DLL when packaging.

From an MSYS2 CLANG64 shell, with an otherwise clean SDL 3.4.16 checkout:

```sh
SDL_SRC=/path/to/SDL
git -C "$SDL_SRC" checkout --detach fa2c02bb6e21974a89ea9824bc53c9932abe5f9c
git -C "$SDL_SRC" apply --check "$PWD/packaging/sdl3/ffb-infinity.patch" "$PWD/packaging/sdl3/ffb-lifecycle.patch"
git -C "$SDL_SRC" apply "$PWD/packaging/sdl3/ffb-infinity.patch" "$PWD/packaging/sdl3/ffb-lifecycle.patch"
cmake -S "$SDL_SRC" -B out/sdl-ffb -G Ninja -DCMAKE_BUILD_TYPE=Release -DSDL_SHARED=ON -DSDL_STATIC=OFF -DSDL_TESTS=OFF
cmake --build out/sdl-ffb --parallel 4
cp out/sdl-ffb/SDL3.dll out/SDL3.dll
bash packaging/package.sh dev-t300-ffb
```

The existing packaging script copies `out/SDL3.dll` over the stock collected DLL near the end. This override is required for the two dependency corrections. The patches are local dependency customizations, not an official SDL release; its license is included alongside them. They are not submitted to the SDL project by this proposal. Do not apply these patches blindly to a different SDL version.

## Validation and limits

- Seven offline tests execute the actual runtime effect loop against a fake haptic backend (`python tools/test_ffb_menu_routing.py`). No motor is opened. Set `CC` if a C compiler is not on PATH.
- Five effect types were checked at zero force for duration conversion. COM cleanup was also checked under simulated Stop/Unload failures and with the actual T300 driver at zero force.
- Flat manual driving passed the previous duration cutoff; repeated starts and a deliberate impact followed by restart no longer reproduced the abnormal forces in the tested build.
- Additional manual validation of the same FFB changes in the VR fork covered approximately twenty campaign events up to Rookie Challenge, with no recurrence reported by the hardware owner. **The longer campaign test was in VR, not twenty Flat races.** No OpenXR changes are included here.
- Tested driving settings: Physics gain 130, filter 10, minimum force 8, kerbs/road 40, slip/damper 0; wheel range 400 degrees, persistent Thrustmaster gain 75%. These values describe the test, not new defaults for everyone.
- Other wheel models have not been validated. A separate end-of-race freeze was observed and is not claimed fixed. Collision-penalty changes are not part of this proposal.

Implementation and diagnosis used Codex AI assistance. The hardware owner performed the real driving tests and reported the results. No commercial game data, saves, dumps or personal configuration files are included.
