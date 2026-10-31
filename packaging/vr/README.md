# VR (experimental)

Plays DriveClub in an OpenXR headset. In the cockpit your head moves the camera (turning, leaning, looking around the
pillars), and the bonnet, bumper, dash and chase cameras follow your head too. The picture is real stereo, in one of
two modes (see *Stereo mode* below): **True stereo** draws both eyes in every frame, **AFR** draws alternate frames
from the left and the right eye. The HUD is a panel of its own, fixed ahead of the seat. Menus, loading screens and
the cameras that are not on the car are shown on a still screen in front of you.

It is **off by default**. When it is off, nothing VR-related is loaded and the game runs exactly as without it.

Tested only on: Quest 2 through Virtual Desktop (its OpenXR runtime, VDXR), an RTX 4070 Ti Super, DriveClub 1.28.

## Setup
1. **OpenXR loader.** Put `openxr_loader.dll` next to `DriveClubGame.exe` (in `runtime\`). It comes with the
   [OpenXR SDK](https://github.com/KhronosGroup/OpenXR-SDK/releases) (`native\x64\release\bin\openxr_loader.dll`)
   or MSYS2's `mingw-w64-clang-x86_64-openxr-sdk` (`libopenxr_loader.dll`, renamed).
2. **Launcher** (the card *VR headset (experimental)* is on the *Display* page of the new launcher, the *Graphics* tab
   of the classic one):
   - **VR headset** on;
   - *Stereo mode*: **True stereo** or **AFR** (see below);
   - *Headset picture (pixels per eye)*: the size the game draws each eye at, in the shape
     of a headset eye (the menus and the HUD keep their 16:9 shape). 1856x1984: the Quest 2 panel, light;
     **2496x2688** and 2688x2880: Virtual Desktop's High and Ultra on a Quest 2; 3200x3440 and 3584x3840: Quest 3,
     Pimax and other sharp headsets. Pick about what your streaming app sends: more only costs frames. Leave
     *Output*, *Render resolution* and *Widescreen* on Default and Off;
   - *Frame rate* reads **Follows the headset** while VR is on: the game takes its limit from the headset's refresh
     rate, and your monitor does not hold it back. Pick the rate in Virtual Desktop, Link or SteamVR (see below). The
     usual *Frame rate* and *Frame cap* rows come back, with their choices, when VR is off;
   - the field of view is set by VR itself while it is on (90° vertical in the cockpit, `cockpit_vfov`); the
     launcher's *Field of view* slider and F5/F6 apply again with VR off. Do not use camera mods that change the
     cockpit's focal length together with VR.
3. Put the headset on and start the headset's PC link (Virtual Desktop, SteamVR, Oculus Link) **before** the game.
   A connected headset that is not worn holds the game's frames back (5 to 10 FPS).
   Close background downloads (game launchers, torrents), Wallpaper Engine and overlays: the game is limited by the
   CPU, and on the test PC they cost about 7% of the FPS.

## Stereo mode
- **True stereo** (experimental): both eyes in every frame, drawn together. It is sharper and steadier than AFR, at a
  lower FPS: about 65-80 in races at 2688x2880 on the test PC, where AFR gets about 115. The first start compiles its
  own shaders (they are kept apart from AFR's), so the first minutes stutter more.
- **AFR**: one eye per frame, in turn. The higher FPS, but each eye gets half of it: you need about 90-100 FPS or more
  to be comfortable, and moving things can look doubled in turns whatever the FPS.

**Pick the headset's refresh rate a little below the FPS your PC holds.** When the headset shows the same frame twice,
things that move in the picture (the road in a turn, other cars) look doubled, and in True stereo that is the only
reason for it. For True stereo on the test PC that means 72 Hz; a Quest 2 has 72, 80, 90 and 120 Hz (Virtual Desktop
offers 60 too, but there the screen flickers, most of all in the menus). In AFR 90 or 120
Hz suits. A headset rate above the FPS does no harm to the game, it only brings the doubling back.

*Driving* → *Camera shake* has no visible effect in the headset (checked at 100%): the view does not shake. *Lock to
horizon* does not change the cockpit while VR is on (the headset holds the view there); with *All views* the other cameras still keep level.

The *Picture* sliders (brightness, contrast, gamma) and Page Up / Page Down while playing change the headset's
picture as they do the monitor's. The in-game exit confirmation (an overlay drawn on the monitor only) stays off
while VR is on.

## Recommended settings
What the test PC (RTX 4070 Ti Super) plays with:

- *Display*: Frame rate **Follows the headset** (in AFR the test PC holds about 90 at 3200x3440 and 110-120 at 2688x2880 on a Quest 2 at 120 Hz). Output and Render resolution **Default**,
  Widescreen **Off**, Two-thread rendering on. Anti-aliasing **Game** (or DLSS, see below), anisotropic filtering
  Automatic, HDR off. *VR headset*: on, Headset picture **2496x2688** (2688x2880 for a faster PC; 3200x3440 and up for Quest 3 or Pimax).
- *DLSS (experimental in VR)*: Anti-aliasing **DLSS (RTX)** with an *Upscaling* level works per eye in both stereo
  modes (each eye its own history and motion). It is smoother and cleaner than no anti-aliasing, but softer, with some
  ghosting (most of all the windscreen's HUD reflection), and gives no frames where the CPU is the limit. Key 7 tries
  DLSS's models in a race. FSR does not work per eye (it reads one eye or mixes them): do not use it with VR.
- *Detail*: everything on Game / off, except Lens dust, Heat haze and the **FXAA pass** on. **Motion blur off**:
  the alternate frames come from eyes 6.3 cm apart, which the game's blur takes for motion.
- *Gameplay*: Mirror and reflection updates **Live**; Patches: **Disable motion blur and depth of field** on.
  *Static (faster)* is only for when the FPS is too low: about 11% more FPS in a CPU-limited benchmark (125.7 → 139.3
  on the test PC), but the reflections and the rain on the glass freeze, and the windscreen gets a frozen bluish
  reflection of the dashboard that is very visible from the cockpit.

## Keys
The number row, on every keyboard (the keypad works too, for those who have one):

| Key | Keypad | |
|---|---|---|
| 0 | 5 | Recentre: where your head is and the direction you face become the seat (sit as you drive). It also happens by itself when you get into the car |
| 2 / 1 | 8 / 2 | Seat up / down (2 cm, with Ctrl 10 cm) |
| 5 / 6 | 4 / 6 | Seat left / right |
| 4 / 3 | 9 / 3 | Seat forward / back |
| 8 | | AFR, experimental: eyes from depth, off → 1 → 2 → off (see below). True stereo: the eye offset off / on, to compare |
| 9 | | AFR, experimental: car turn compensation on / off (see below). True stereo: the window shows the other eye |
| 7 | | With DLSS: its model, auto → K → L → M → auto (see the log, `VR: DLSS model ...`) |

The seat starts at the game's own eye point for each car. The seat keys keep what you set by themselves, in
`data\user\vr_seat.ini` (with your saves, so it moves to a new install with `data\user`): set it once. Delete that
file to go back to the game's seat. Other keys can
be set in `vr.ini`: `key_recentre`, `key_up`, `key_down`, `key_left`, `key_right`, `key_forward`, `key_back`, each one
character (a letter, a digit, `[`, `;` ...), `F1` to `F12`, or `pageup`, `pagedown`, `home`, `end`, `insert`,
`delete`, `space`, `tab`.

## vr.ini
Optional, next to `DriveClubGame.exe` (`runtime\vr.ini`): `key = value` lines. A `vr.ini` without the launcher
switch also turns VR on (for running the game without the launcher). Defaults:

```
afr = 1            # with Stereo mode AFR: alternate frames; 0: one picture for both eyes (full frame rate, no depth)
ipd = 0.063        # metres between the eyes
rigid = 1          # the camera fixed to the car body; 0: the game's head on springs (it sways in VR)
seat_x = 0         # the eyes relative to the driver's eyes the game sets for each car, metres:
                   # x left, y up, z forward (seat_y, seat_z); data\user\vr_seat.ini (the seat keys) wins
runtime_fov = 1    # each eye drawn with the field of view and eye distance the runtime gives it (drawn symmetric,
                   # the headset gets just its own part); 0: cockpit_vfov
dlss_model = auto  # with DLSS: auto, k, l or m (key 7 cycles it in a race)
smoothing = 1      # steadies a head that holds still; smooth_hz = 0.5, smooth_turn = 0.05, smooth_move = 0.1
frame_pose = 1     # each frame stated to the headset with the turn it was really drawn with; 0: the hook's
auto_recentre = 1  # recentre on getting into the car (race start, restart, another car); 0: only by the key
cockpit_vfov = 90  # the cockpit camera's vertical field of view in the headset, degrees
hud_layer = 1      # in the car the HUD is a panel of its own ahead of the seat; 0: drawn into the picture
hud_distance = 1.1 # the HUD panel: metres ahead, hud_width = 1.4 (metres, 16:9), hud_y = 0 (metres above the eyes)
screen_vfov = 40   # the still screen's height, degrees
reproject = 0      # AFR only, experimental, smoother turns: 1 the other eye made each frame from this frame's picture and
                   # depth (glass, reflections and edges flicker a little); 2 both eyes made from a frame drawn
                   # between them (no flicker, softer edges, glass at a wrong depth). key_reproject = 8 cycles it
turn_comp = 0      # AFR only. 1: the older eye's picture turned by the car's turn since it was drawn (steadier world in
                   # turns, the cockpit moves in that eye instead); 0.5 half. key_turn = 9 switches it while driving
window = 1         # the window (for watching and recording): one eye's frames at 16:9, the middle band of the
                   # eye's picture with the HUD and the menus stretched back to 16:9; 0: every frame as the headset
                   # gets it (both eyes in turn). window_eye = 0 the left eye, 1 the right
key_recentre = 0   # and key_up = 2, key_down = 1, key_left = 5, key_right = 6, key_forward = 4, key_back = 3
```

## How it works
- A hook in the game's driver head camera update (1.28; the bytes are checked before patching, otherwise there is no
  head tracking and everything is a still screen) replaces the camera matrix the renderer draws from: the car body's
  turn and position plus the headset's pose, and every other frame the other eye.
- The bonnet, bumper, dash and chase cameras get the head where the scene takes its camera (Scene::SetCamera), and
  the game's culling gets the headset's field of view turned with the head, so what is behind the car is drawn. The
  way was found by [hookmanuk's VR fork](https://github.com/hookmanuk/driveclub-recomp).
- True stereo is Vulkan multiview, ported from the same fork: the main images get a second layer, and each draw goes
  to both eyes at once with its clip x moved by the eye's offset. DLSS runs twice a frame, on each eye's layers.
- Each frame is matched with the hook call it was drawn from (the camera position in the G-buffer pass's constants),
  so it goes to its own eye and is stated to the headset at exactly the pose it was drawn from; the runtime turns it
  to the head's pose at display time.
- The headset gets the game's picture at the output resolution, not the window's.
- With the "+ HUD" sizes the game keeps its 16:9 aspect (the HUD's and the menus' canvas) and draws in the eye's
  shape: a hook where the scene takes its camera (Scene::SetCamera) narrows the driver's head camera's projection to
  the render's shape; everything else is squeezed and shown at 16:9 again.
- The HUD apart: at a frame's first HUD draw into the display buffer the scene is copied out and the buffer cleared,
  so the HUD draws alone (its alpha kept as coverage). In the car the eyes get the scene and the HUD is a quad layer;
  the window and the still screen get both put together again. The split point and the alpha fix follow
  [hookmanuk's VR fork](https://github.com/hookmanuk/driveclub-recomp).

## Known limits
- AFR: each eye gets half the game's frame rate (about 55-60 in a race on the test PC), so the picture can shake a
  little, most of all near objects while turning. Below about 90 game FPS (45 per eye) it feels laggy when you move
  your head. Without DLSS it can look grainy: the game's own temporal AA mixes the two eyes, so its FXAA pass (Detail
  tab) is the only other anti-aliasing that helps.
- True stereo: a lower FPS than AFR, and DLSS has no vectors of the other cars' own motion, so distant cars can swim a
  little inside their outline. Menus stay flat.
- DriveClub Recomp is heavy on the CPU: a lower headset picture helps little when the CPU is the limit.
- DLSS per eye is experimental: softer, some ghosting, and no FPS gain where the CPU is the limit. No FSR, no frame
  generation.
- The game's camera motion blur is off while VR is on (the eyes 6.3 cm apart look like motion to it).
- A few picture glitches remain (some screens before and after a race).
- Tested with one headset, runtime and graphics card.
