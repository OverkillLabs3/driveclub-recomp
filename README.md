# DriveClub Recomp

**DriveClub for PS4 (version 1.28), running natively on Windows.**

<img width="1920" height="1080" alt="s009 20" src="https://github.com/user-attachments/assets/4c0fe155-eae9-4c78-aa24-4a5b154e2b9f" />

The source code is in this repository and the build is on the [Releases](../../releases) page.

## Planned for the next major release
- **DLSS/FSR Upscaling (not just native DLAA/FSR AA)**
- **DLSS/FSR Frame Generation, DLSS up to 4X, FSR up to 2X**
- **DLSS Neural Rendering (Standard and Optimised model support)**
- **Graphics improvements beyond what the PS4 offered**
- **Further performance and stability improvements**

## New in 0.2.0: DLSS and FSR anti-aliasing
The game's own anti-aliasing is replaced by a modern one, chosen in the launcher:
- **NVIDIA DLAA** (GeForce RTX) for the sharpest, steadiest picture: thin lines, wires, fences and foliage stay clean.
- **AMD FSR 3.1** on any graphics card, with no special hardware and nothing extra to install.

Both work at the native resolution and use the game's own camera motion. In the game the overlay shows what is active, **F9** switches the new anti-aliasing off and on to compare, and **F10** switches between FSR and DLSS on RTX cards.

## How to play
1. Download the latest `driveclub-recomp-<version>-win64.zip` from the [Releases](../../releases) page and extract it anywhere you can write to (not `Program Files`).
2. Have **your own** game dumps ready:
   - the **version 1.28 update** folder (it contains `eboot.bin`)
   - the **base game** folder (it contains `game000.dat`)
3. Double-click `DriveClub.exe`, press **Browse** next to each folder, check the settings and press **Play**.

Nothing to install. Your own files are never modified or copied, and the folders you chose are remembered.

Logs are in the `logs` folder.

## Highlights
- The game's own x86-64 code runs directly on your CPU, with no CPU emulation. The PS4 system libraries are reimplemented and the GPU commands are translated to Vulkan, similar to Wine and DXVK
- Playable from the menus through races, photo mode and saves
- About 60 FPS at 4K in the heaviest scenes on our test PC (RTX 5070 Ti, Ryzen 7 5800X3D), higher in lighter ones, with an unlocked frame rate option in the launcher that keeps the game at its normal speed
- Runs on NVIDIA, AMD and Intel graphics through Vulkan, with Resizable BAR used automatically where the driver allows it
- Keyboard racing layout and any gamepad (Xbox, PlayStation, Switch Pro, 8BitDo)
- Steering wheels with force feedback, pedals with clutch and an H-pattern shifter
- DLC content (add-on cars, bikes, tours and livery decals) can be unlocked from the launcher, built from your own game files. Cars you earn by playing still unlock through normal progression
- A launcher window that resizes from every edge and corner and stays sharp at any display scale
- A `mods` folder for loose-file mods: files in it replace the game's own files of the same path

## Controls
Any gamepad works. Keyboard, mouse and gamepad can be used at the same time.

| Action | Keyboard |
|---|---|
| Accelerate | W |
| Brake / reverse | S |
| Steer | A / D |
| Handbrake | Space |
| Shift down / up | Q / E |
| Change camera | C |
| Touchpad left / right half | R / T (R is Reset to Track) |
| Accept | Enter / Z |
| Back | Backspace |
| Pause | Esc / P |
| D-pad | Arrow keys |
| Look around | I J K L or the mouse |

F7 turns mouse look on or off. F9 and F10 are the anti-aliasing keys above. F12 hides and shows the FPS counter. The bindings are in `data\user\input_config\default.ini` and can be changed there.

### Steering wheels
Any wheel that Windows lists as a game controller works: the game sees it as a PS4 wheel, and its force feedback reaches the wheel the way it reached a Logitech wheel on the console. Pedals with a clutch and an H-pattern shifter (on the wheel or as a controller of its own, such as a Logitech Driving Force Shifter on a USB adapter) work too; choose a manual gearbox in the game for the shifter.

1. Connect the wheel, pedals and shifter **before** starting the game.
2. Run `WheelSetup.exe` next to `DriveClub.exe`. Pick the wheel, and the pedals if they have a USB of their own. Every control is a row with its current mapping and a live view: click **Map** and turn the wheel, press the pedal, button or gear; **×** clears a row. **Save** writes `default.ini`.
3. Set the **rotation** in the wheel's own software (Pit House, G HUB, ...) and enter the same number in `WheelSetup.exe`.

Pedals on a USB of their own (`wheel_pedals` in `default.ini`) and wheels that Windows lists as two controllers of one name, as a Fanatec base is, are picked by `WheelSetup.exe` (for the second, `wheel_device = FANATEC Wheel @col01`: the HID collection, which stays the same from start to start).

The force feedback follows the game's own setting. In `default.ini`, `wheel_ffb_gain` and `wheel_ffb_vibration` make the forces and the road vibrations stronger or weaker (100 = as the game sends them, up to 2000; the steering forces are light on most wheels, so a few hundred is normal), `wheel_ffb_invert = true` turns them around if the wheel pulls the wrong way, and `wheel_ffb = off` turns force feedback off. `FfbTest.exe` plays single forces on the wheel without the game, to check the directions.

Tested with a Moza R3 and a Logitech Driving Force Shifter. On some PCs Windows takes a long time to list game controllers; the game then starts later and pauses when a controller is plugged in while it runs.

## Settings
The window that opens when you start `DriveClub.exe` has every setting. Press **Play** to save them; they are kept for later starts.

**Performance preset:** Handheld, Low, Medium, High or Ultra pick the render and output sizes for your PC. You can still change each size afterwards.

**Frame rate:** 30, 60 (deltatime), 60 with a fixed tick, or **Unlocked**. Unlocked has no frame cap and the game keeps its normal speed.

**Resolution:** output from 720p to 8K, render size from 720p to 8K, and ultrawide sizes with a field-of-view fix. Sizes above 1080p use more memory; the needed memory patch is added for you.

**Mirror and reflection updates:** **Live** (default) draws the small scene of mirrors, rain and reflections every frame. **Static (faster)** draws it once and keeps that picture, which is much faster but stops reflections and rain on the glass from updating. The Handheld preset uses it.

**Anti-aliasing:** **Game** keeps the original. **FSR 3.1** works on any graphics card, **DLAA (RTX)** needs a GeForce RTX card; if it is not available the launcher falls back to FSR. Do not turn on the community patch "Disable motion blur and depth of field" with it: they do not work together and the game keeps its own anti-aliasing then.

**Anisotropic filtering:** keeps textures on roads and terrain sharp at a shallow angle. **Automatic** is 16x (4x on integrated graphics); you can also choose Off, 2x, 4x, 8x or 16x.

**Unlock DLC content:** off by default. Unlocks the add-on cars, bikes, tours and livery decals. The launcher reads the vehicle list, the tour list and the livery list from your own game archive and builds changed copies that the game uses. Your game folders are not modified.

**Gameplay:** player name, borderless fullscreen, FPS counter, motion blur and depth of field off, no reset-to-track timer, no track barriers, and photo mode options.

**Skip the hardware check:** the launcher checks that your processor and graphics card can run the game (a processor with AVX, BMI1, LZCNT and MOVBE, and Vulkan 1.3). If it ever refuses hardware that works, turn this on to start anyway.

## Benchmark
The **Benchmark** card in the launcher measures how the game runs with your settings. Choose the track, car, time of day, weather, rain and length, then press **Run benchmark**. The game starts straight into the event, the AI drives, and the game closes by itself when it is done. The launcher then shows the average frame rate and the **1% and 0.1% lows** (the average of the slowest 1% and 0.1% of frames). The result is also in `data\user\benchmark\result.txt` and `logs\launcher.log`. Under the result, the card says what the game actually ran with (render and output resolution, frame limit, anti-aliasing); point at it to see every setting.

Without the window: set `BB_BENCHMARK` and start `DriveClub.exe --play`. `BB_BENCHMARK=1` uses the defaults (Fraser Valley, Ferrari 458 Italia, noon, clear, 60 s); keys can be given as `BB_BENCHMARK=track=chile_race_track_v1,car=fe_f12,tod=18`. The game exits when done (exit code 0, or 3 if the race did not start) and prints one line to its log:
```
Benchmark: result mode=TimeTrial track=canada_road_circuit_02_n car=fe_458 tod=12 weather=Clear rain=dry drive=ai seconds=60.0 frames=2639 avg_fps=43.97 low1_fps=26.94 low01_fps=14.14 p99_ms=32.65 max_ms=103.85 load_s=21.9
```
A second line, `Benchmark: ran render=... output=... fps_limit=... aa=...`, lists the settings the game ran with, read by the game itself (the render and output sizes from its own memory). The keys and their values are listed at the top of [src/runtime_benchmark.c](src/runtime_benchmark.c).

## Known issues
- **Some reflections are too bright**, for example rain drops on the side window and the paint on cars on the stage select screen.
- **Anti-aliasing:** fast cars can leave a faint trail, because cars have no motion data of their own yet.
- **Static (faster)** stops reflections and rain on the glass from updating.
- **The processor is the limit.** Heavy scenes (many cars, trees and rain) drop frames first on slower CPUs.
- **First play builds shaders:** short stutters in the first minutes until they are cached. This gets better on later runs.
- **Online features do not work.**
- Tested on an NVIDIA graphics card only. AMD and Intel are untested; reports from AMD and Intel players are welcome.

## Notes
- You must supply your own, legally obtained dump of the game. No game files are included.
- **Tested with:** DriveClub, title ID `CUSA00093`, version 1.28. The European release (`CUSA00003`) version 1.28 is accepted too and has been played less. Other versions and repacked dumps have not been tested and may not work.
- SmartScreen may warn (*More info*, then *Run anyway*). Verify with the checksum.
- Requires Windows 10/11 64-bit, a processor from Intel Haswell (2013) or AMD Excavator or Ryzen onward, and a graphics card with a Vulkan 1.3 driver. About 40 GB of free space is needed for the game dumps.

## Building from source
See [BUILDING.md](BUILDING.md).

## Reporting problems
Open an [issue](../../issues/new/choose) and fill in the form. **Attach the log files**: without them an issue usually cannot be solved. In the folder next to `DriveClub.exe`, open `logs` and drag these into the issue:
- the newest `driveclub_<date>_<time>.log` (one per start: take the one from the run that went wrong). It starts with your Windows, processor, graphics card, driver and launcher choices, and has a status line every 30 seconds, so nothing else is needed from you.
- `launcher.log`: what the launcher saw. It exists even when the game never started, for example when the launcher shows an error.
- any `crash_*.dmp`: a small file written when the game crashes.

Also say what you were doing when it happened (menu, loading, which event and car).

## Credits
The community patches for DriveClub are by Kravickas, TonySSS, ZipperedRain, kalaposfos, squidbus, illusion and u/Charlie_Muggins.

This project is built on other projects and says so where it uses their code. [NOTICE](NOTICE) names every upstream project, what was taken from it and how to verify that. In short:

- [bloodborne_pc](https://github.com/Supermedo/bloodborne_pc) by Supermedo (GPL-2.0-or-later): the native runtime in `src/`, the graphics shim in `gpu/shim/` and parts of the vendored game runtime in `gpu/shadps4/`.
- [bloodborne_pc](https://github.com/deadinside28/bloodborne_pc) by deadinside28 (GPL-2.0-or-later): the original native Linux port, the source of the `bbport:` changes in the vendored renderer.
- [shadPS4](https://github.com/shadps4-emu/shadPS4) (GPL-2.0-or-later): the video core and shader translator in `gpu/shadps4/`.
- [shadp2p](https://github.com/Wozzardman/shadp2p) and [BloodBorneButPc](https://github.com/Awakenchan/BloodBorneButPc) (GPL-2.0-or-later): the shadNet client and the network and NP libraries.

Also used: [SDL3](https://github.com/libsdl-org/SDL) and [FFmpeg](https://ffmpeg.org).

## License
GNU General Public License version 2 or later ([LICENSE](LICENSE)). Parts of the code derive from other GPL projects; the files that were changed for this project carry a notice at the top naming the project they are based on, and [NOTICE](NOTICE) lists all of them. Third-party components keep their own licenses ([THIRD_PARTY_NOTICES.txt](packaging/release/THIRD_PARTY_NOTICES.txt)).

DriveClub is a trademark and copyright of its respective owners; this project is not affiliated with or endorsed by them.
