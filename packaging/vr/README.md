# VR (experimental)

Plays DriveClub in an OpenXR headset. In the cockpit view your head moves the camera (turning, leaning, looking
around the pillars), and the picture is real stereo: the game draws alternate frames from the left and the right
eye. Menus, loading screens and the other cameras are shown on a still screen in front of you.

It is **off by default**. When it is off, nothing VR-related is loaded and the game runs exactly as without it.

Tested only on: Quest 2 through Virtual Desktop (its OpenXR runtime, VDXR), an RTX 4070 Ti Super, DriveClub 1.28.

## Setup
1. **OpenXR loader.** Put `openxr_loader.dll` next to `DriveClubGame.exe` (in `runtime\`). It comes with the
   [OpenXR SDK](https://github.com/KhronosGroup/OpenXR-SDK/releases) (`native\x64\release\bin\openxr_loader.dll`)
   or MSYS2's `mingw-w64-clang-x86_64-openxr-sdk` (`libopenxr_loader.dll`, renamed).
2. **Launcher:**
   - *Gameplay* → **VR headset (experimental)** on;
   - *Graphics* → *Widescreen* → **VR Ultra** (2688x2880) or **VR High** (2496x2688): the picture in the shape of
     a headset eye (Virtual Desktop's Ultra and High sizes). Other sizes work, but waste pixels outside the eye's
     view;
   - *Frame rate* → **Unlocked**: with stereo each eye gets half the frame rate;
   - the field of view is set by VR itself while it is on (90° vertical in the cockpit, `cockpit_vfov`); the
     launcher's *Field of view* slider and F5/F6 apply again with VR off. Do not use camera mods that change the
     cockpit's focal length together with VR.
3. Put the headset on and start the headset's PC link (Virtual Desktop, SteamVR, Oculus Link) **before** the game.
   A connected headset that is not worn holds the game's frames back (5 to 10 FPS).

Hide the HUD in the game's options: it is laid out for 16:9 and the eye-shaped picture cuts its sides.

## Keys
| Key | |
|---|---|
| Keypad 5 | Recentre: the direction you face becomes straight ahead |
| Keypad 8 / 2 | Seat up / down (2 cm, with Ctrl 10 cm) |
| Keypad 4 / 6 | Seat left / right |
| Keypad 9 / 3 | Seat forward / back |

The seat keys print the values to the log (`VR: seat_x = ...`); copy them into `vr.ini` to keep them.

## vr.ini
Optional, next to `DriveClubGame.exe` (`runtime\vr.ini`): `key = value` lines. A `vr.ini` without the launcher
switch also turns VR on (for running the game without the launcher). Defaults:

```
afr = 1            # stereo by alternate frames; 0: one picture for both eyes (full frame rate, no depth)
ipd = 0.063        # metres between the eyes
rigid = 1          # the camera fixed to the car body; 0: the game's head on springs (it sways in VR)
seat_x = 0         # the eyes relative to the game's head point, metres: x left, y up, z forward (the
                   # game's point is above the driver's eyes; -1.15 fitted the cars tried)
seat_y = -1.15
seat_z = 0
smoothing = 1      # steadies a head that holds still; smooth_hz = 0.5, smooth_turn = 0.05, smooth_move = 0.1
cockpit_vfov = 90  # the cockpit camera's vertical field of view in the headset, degrees
screen_vfov = 40   # the still screen's height, degrees
```

## How it works
- A hook in the game's driver head camera update (1.28; the bytes are checked before patching, otherwise there is no
  head tracking and everything is a still screen) replaces the camera matrix the renderer draws from: the car body's
  turn and position plus the headset's pose, and every other frame the other eye.
- Each frame is matched with the hook call it was drawn from (the camera position in the G-buffer pass's constants),
  so it goes to its own eye and is stated to the headset at exactly the pose it was drawn from; the runtime turns it
  to the head's pose at display time.
- The headset gets the game's picture at the output resolution, not the window's.

## Known limits
- Each eye gets half the game's frame rate (about 40 in a race on the test PC). FSR, DLSS and frame generation would
  have to run per eye to work with alternate frames.
- The HUD and menus are not made for VR.
- Tested with one headset, runtime and graphics card.
