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

Both use the game's own camera motion. In the game the overlay shows what is active, **F9** switches the new anti-aliasing off and on to compare, and **F10** switches between FSR and DLSS on RTX cards.

**Upscaling** (launcher, next to Anti-aliasing): the game draws at a lower resolution and DLSS or FSR rebuilds the picture at the full output size, after the game's own effects, so it stays sharp. At 5120x1440 on an RTX 4070 Ti the average went from 48 FPS (DLAA) to 80 (DLSS Quality), 88 (Balanced) and 104 (Performance); FSR 3.1 Quality gave 103 (DLSS's transformer model costs about 3 ms a frame there, for a cleaner picture). It works with every output and widescreen size; the render size follows from it. **Sharpness** sharpens the finished picture (AMD's RCAS, also after DLSS, which no longer sharpens by itself).

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
- Steering wheels with force feedback from the car's tyre physics, pedals with clutch, an H-pattern shifter and a USB handbrake
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

F5 and F6 narrow and widen the view. F7 turns mouse look on or off. F9 and F10 are the anti-aliasing keys above. F12 hides and shows the FPS counter. With HDR on, Page Up and Page Down make the picture brighter or darker. The bindings are in `data\user\input_config\default.ini` and can be changed there.

### Steering wheels
Any wheel that Windows lists as a game controller works: the game sees it as a PS4 wheel. Pedals with a clutch, an H-pattern shifter (on the wheel or as a controller of its own, such as a Logitech Driving Force Shifter on a USB adapter) and a handbrake with a USB of its own work too. Everything is set up on the launcher's **Driving** tab, under **Wheel and force feedback**.

**Setting up**
1. Connect the wheel, pedals, shifter and handbrake **before** starting the game.
2. Press **Wheel Setup** (or run `WheelSetup.exe` next to `DriveClub.exe`). Pick the wheel, and the pedals if they have a USB of their own. Every control is a row with its current mapping and a live view: click **Map** and turn the wheel, press the pedal, button or gear, or pull the handbrake; **×** clears a row. **Save** writes `data\user\input_config\default.ini`.
3. Set the **rotation** in the wheel's own software (Pit House, G HUB, ...) and enter the same number in Wheel Setup.
4. For the shifter choose a manual gearbox, and for a wheel turn on hardcore handling. The **Sim** driving preset on the **Assists and view** page does both and turns the assists off.

The **Handbrake** row listens on every controller, so a USB handbrake is found by pulling it; one that is a switch is mapped as a button. The game has no handbrake for wheels: pulled past 40%, the handbrake presses the circle button, which is the handbrake in a race.

Pedals on a USB of their own (`wheel_pedals` in `default.ini`) and wheels that Windows lists as two controllers of one name, as a Fanatec base is, are picked by Wheel Setup (for the second, `wheel_device = FANATEC Wheel @col01`: the HID collection, which stays the same from start to start).

**Force feedback**
- **Reworked Physics** (recommended): the force is worked out from the car's front tyres in the game's own simulation, the way a racing sim does it. The wheel gets heavier as the tyres load up in a corner, goes light when the front tyres start to slide and the car runs wide, and turns the right way by itself when the back of the car steps out. Kerbs, bumps and the road come through on top. It learns each car's tyres in the first corners.
- **Original Physics** (the default): the forces the PS4 game made for a Logitech wheel. Its settings are `wheel_ffb_gain` and `wheel_ffb_vibration` in `default.ini` (100 = as the game sends them, up to 2000; a few hundred is normal).

**Preset** sets the sliders for your kind of wheel: **Direct drive** (Fanatec DD, Moza, Simagic, Simucube, ...) or **Gear or belt** (most Logitech, Thrustmaster and Fanatec CSL wheels). Then adjust them to taste:

| Setting | Range | What it does |
|---|---|---|
| Gain | 0–300% | The overall strength. At 100% a hard corner uses about 60% of the wheel's force. |
| Filter | 0–100% | Smooths the force. 0 on a direct drive, a little on gear wheels. |
| Minimum force | 0–30% | Lifts the smallest forces past the dead spot that gear and belt wheels have around the centre. |
| Damper | 0–100% | Weight when you turn the wheel, strongest when the car stands still, as with real tyres. |
| Kerbs | 0–200% | The rumble of kerbs. |
| Road | 0–200% | Bumps and the road surface. |
| Slip | 0–200% | A buzz when the tyres slide, spin or lock. |
| Soft lock | Game, Off, 540°–1080° | Stops the wheel at the car's own steering lock (Game) or at a fixed angle. |
| Vibration wave | Sine, Square | Square feels sharper on gear and belt wheels. |
| Invert forces | | Turns the forces around if the wheel pulls the wrong way. |

Direct drive: gain 100, filter 0, minimum force 0, kerbs 50, road 50, slip 30, damper 10. Gear or belt: gain 130, filter 10, minimum force 8, kerbs 80, road 80, slip 50, damper 0.

The settings are kept in `default.ini` (`wheel_ffb_physics_*`), and the game picks up changes to it within a second, so `FFB Tuner.bat` can change them while you drive. If the wheel feels flat in hard corners, lower the gain: the log in `logs` says how often the force was at the wheel's maximum (`FFB clipping`). `wheel_ffb = off` turns force feedback off. `FfbTest.exe` plays single forces on the wheel without the game, to check the directions.

**Field of view**
The **Field of view** slider on the **Assists and view** page sets the view from 50% to 150% of the game's own, for every camera. While playing, **F5** narrows it and **F6** widens it in 5% steps; the new value shows at the top of the screen and is kept for the next start (`data\user\camera.ini`). At 100% the view is left as the game and the patches have it. Any other value counts from the game's own view and takes the place of the widescreen field-of-view fix (which is about 131%).

Tested with a Moza R3, a Fanatec 8 Nm direct drive base with ClubSport Pedals and a Fanatec handbrake plugged into the pedals, and a Logitech Driving Force Shifter. On some PCs Windows takes a long time to list game controllers; the game then starts later and pauses when a controller is plugged in while it runs.

## Settings
The window that opens when you start `DriveClub.exe` has every setting. Press **Play** to save them; they are kept for later starts.

**Performance preset:** Handheld, Low, Medium, High or Ultra pick the render and output sizes for your PC. You can still change each size afterwards.

**Frame rate:** 30, 60 (deltatime), 60 with a fixed tick, or **Unlocked (VRR)**. Unlocked runs up to 3 FPS below your display's refresh rate, the game keeps its normal speed, and variable refresh (G-Sync, FreeSync) is on: frames go out in step with the display, so a VRR display shows each one as it comes. For G-Sync, enable it for windowed and full screen mode in the NVIDIA Control Panel (the game's full screen is borderless). **Frame cap** (with Unlocked) sets the highest frame rate: a little below what your PC can hold (for example 90 when it runs at about 95-100) gives the steadiest motion.

**Resolution:** output from 720p to 8K, render size from 720p to 8K, and ultrawide sizes with a field-of-view fix. Sizes above 1080p use more memory; the needed memory patch is added for you.

**Mirror and reflection updates:** **Live** (default) draws the small scene of mirrors, rain and reflections every frame. **Static (faster)** draws it once and keeps that picture, which is much faster but stops reflections and rain on the glass from updating. The Handheld preset uses it.

**Anti-aliasing:** **Game** keeps the original. **FSR 3.1** works on any graphics card, **DLSS (RTX)** needs a GeForce RTX card; if it is not available the launcher falls back to FSR. Do not turn on the community patch "Disable motion blur and depth of field" with it: they do not work together and the game keeps its own anti-aliasing then.

**Upscaling:** with FSR 3.1 or DLSS: **DLAA** (**Native** with FSR) draws every pixel; **Quality**, **Balanced**, **Performance** and **Ultra Perf.** draw the game at 1/1.5, 1/1.7, 1/2 and 1/3 of the output size and rebuild the full picture, for more FPS each. The Render resolution choice does not apply then. With upscaling, F9 leaves FSR or DLSS on: the game draws at the smaller size, so there is no full-resolution picture to switch to. **Sharpness:** 0 to 100 (80 by default).

**HDR:** for HDR monitors and TVs; turn on "Use HDR" in Windows' display settings first. The game makes an SDR picture, so the port turns it into HDR10: the HUD and menus at a comfortable white, and the 3D scene's highlights (the sun, lights, reflections, glints) brighter, taken from the game's own scene before its tone mapping where the SDR picture runs out. White surfaces such as snow and painted lines stay white, not glaring. **HDR brightness** (100 to 600 nits, 350 by default; Page Up / Page Down while playing, kept for the next start) sets how bright white is; **HDR peak brightness** (400 to 2000 nits, 1000 by default) should match your screen's HDR peak. Frame generation is off with HDR.

**Anisotropic filtering:** keeps textures on roads and terrain sharp at a shallow angle. **Automatic** is 16x (4x on integrated graphics); you can also choose Off, 2x, 4x, 8x or 16x.

**Unlock DLC content:** off by default. Unlocks the add-on cars, bikes, tours and livery decals. The launcher reads the vehicle list, the tour list and the livery list from your own game archive and builds changed copies that the game uses. Your game folders are not modified.

**Gameplay:** player name, borderless fullscreen, FPS counter, motion blur and depth of field off, no reset-to-track timer, no track barriers, and photo mode options.

**Driving:** the field of view, the gearbox, brake assist, hardcore handling and wheel assist, and the steering wheel (Wheel Setup and force feedback, see [Steering wheels](#steering-wheels)). **Sim** turns the assists off, with a manual gearbox and hardcore handling; **Assisted** uses an automatic gearbox, brake assist and wheel assist; **Game Defaults** leaves them to the game's own menu. A choice made here wins over the game's menu; **In-game** hands that option back to the menu.

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

Also used: [SDL3](https://github.com/libsdl-org/SDL) and [FFmpeg](https://ffmpeg.org).

## License
GNU General Public License version 2 or later ([LICENSE](LICENSE)). Parts of the code derive from other GPL projects and keep their original notices in the source files. Third-party components keep their own licenses ([THIRD_PARTY_NOTICES.txt](packaging/release/THIRD_PARTY_NOTICES.txt)).

DriveClub is a trademark and copyright of its respective owners; this project is not affiliated with or endorsed by them.
