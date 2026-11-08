# VR (experimental)

Plays DriveClub in an OpenXR headset. In the cockpit your head moves the camera (turning, leaning, looking around the
pillars), and the bonnet, bumper, dash and chase cameras follow your head too. The picture is real stereo, in one of
two modes (see *Stereo mode* below): **True stereo** draws both eyes in every frame, **AFR** draws alternate frames
from the left and the right eye. The HUD is a panel of its own, fixed ahead of the seat. Menus, loading screens and
the cameras that are not on the car are shown on a still screen in front of you.

It is **off by default**. When it is off, nothing VR-related is loaded and the game runs exactly as without it.

Tested only on: Quest 2 through Virtual Desktop (its OpenXR runtime, VDXR) and SteamVR, an RTX 4070 Ti Super,
DriveClub 1.28.

## Setup
1. **OpenXR loader.** `openxr_loader.dll` must be next to `DriveClubGame.exe` (in `runtime\`). The VR builds have it;
   otherwise it comes with the [OpenXR SDK](https://github.com/KhronosGroup/OpenXR-SDK/releases)
   (`native\x64\release\bin\openxr_loader.dll`) or MSYS2's `mingw-w64-clang-x86_64-openxr-sdk`
   (`libopenxr_loader.dll`, renamed).
2. **Put the headset on and start its PC link** (Virtual Desktop, SteamVR, Meta Link) **before** the game. A
   connected headset that is not worn holds the game's frames back (5 to 10 FPS). Close background downloads (game
   launchers, torrents), Wallpaper Engine and overlays: the game is limited by the CPU, and on the test PC they cost
   about 7% of the FPS.
3. **Launcher, VR tab**, card *VR headset (experimental)*:
   - **VR headset** on;
   - *Stereo mode*: **True stereo** or **AFR** (see below);
   - *Headset picture scale*: each eye is drawn at the size your headset's app asks for (Virtual Desktop's quality,
     SteamVR's resolution), in the shape of that headset's eye, so the headset gets it pixel for pixel. The first
     start with a new headset learns its eyes (`data\user\vr_eyes.ini`), the next ones draw the exact size.
     100% is what the headset gets. Above it the eyes are drawn bigger and brought down to
     the headset's size: cleaner edges and distant detail, for about a fifth more work at 110%, half more at 125%.
     Below it: more FPS, softer;
   - *Head smoothing in VR*: see *Picture* below.

While VR is on the launcher hides and sets what would break VR: *Output* and *Render resolution*, *Aspect ratio*,
*Dynamic resolution*, the *High-FPS camera fix*, *Motion blur*, *Field of view*, *Lock to horizon* and *Camera
shake*, and the Display tab's anti-aliasing (VR has its own, below). Your choices come back with VR off. *Frame rate*
reads **Follows the headset**: the game takes its limit from the headset's refresh rate (see below).

## Stereo mode
- **True stereo** (experimental): both eyes in every frame, drawn together. It is sharper and steadier than AFR, at a
  lower FPS. The first start compiles its own shaders, so the first minutes stutter more.
- **AFR**: one eye per frame, in turn. The higher FPS, but each eye gets half of it: you need about 90-100 FPS or more
  to be comfortable, and moving things can look doubled in turns whatever the FPS.

**Pick the headset's refresh rate a little below the FPS your PC holds**, in Virtual Desktop, Link or SteamVR (a
Quest 2 has 72, 80, 90 and 120 Hz). When the headset shows the same frame twice, things that move in the picture (the
road in a turn, other cars) look doubled. A headset rate above the FPS does no harm to the game, it only brings the
doubling back.

## Picture
The VR tab's card *VR picture and frame rate*, in both stereo modes:

- **Anti-aliasing in VR**:
  - *TAA*: the game's own. Fewer jagged edges, a softer picture.
  - *Off*: the sharpest picture, with jagged edges; a few more FPS.
  - *SMAA*: smooths the edges in every frame with hardly any blur, for 2 to 5 FPS. Thin detail far away still
    shimmers; a bigger *Headset picture scale* helps.
  - *DLSS (RTX)*: NVIDIA's, per eye (each eye its own history and motion), the steadiest picture far away. *DLSS
    quality in VR* picks how big it draws each eye: DLAA (every pixel) to Ultra Performance. A big *Headset picture
    scale* with Balanced or Performance is the sharpest. DLSS's sharpness is the Display tab's row.

  The FXAA pass stays a switch on the Detail tab and adds to any of them. FSR does not work per eye (it reads one eye
  or mixes them): VR never uses it.
- **Glow in VR**: how much of the game's glow (bloom) the headset gets. The game lays a blurred copy of the picture
  over it: on a screen it looks like haze and light, in the headset like a soft film over every edge. 100% is as the
  game draws it; lower gives clearer edges and deeper shadows; at 0% bright skies lose their halo. No cost in FPS.
- **Exposure in VR**: makes the whole picture brighter (+) or darker (-); in game **[** and **]** change it by a
  quarter stop. The game sets its exposure from how bright the whole picture is, like a camera: a shaded turn or
  the dark dash raised it, and the road flashed white when the turn opened up. In the headset, in the car, it now
  brightens by at most 3 stops a minute (more in a race with the time sped up), so turns and shade barely change it
  while a sunset is still followed; it darkens as fast as the game's, and comes back quickly after a short bright
  stretch. The first seconds in the car take the track's level at once, day or night, and
  it holds while you look down at the dash or up at the sky. `exposure_rate` in `vr.ini` sets the 3 (0: the
  game's speed). Lens dust is off in VR: it is a camera's lens, not an eye.
- **Foveated rendering**: the edges of each eye, which the lenses blur anyway, are drawn with less detail. Light,
  Medium (full detail within about 35 degrees of the middle) or Strong. It helps a few FPS when the graphics card is
  the limit.
- **Edge margin (frame generation)**: black edges when you turn your head come from generated frames. Each eye is
  drawn this many degrees wider, so frame generation on the PC (OpenXR Toolkit's) has the scene there. Frames made in
  the headset (Virtual Desktop's SSW, Meta's ASW) do not get it: without PC frame generation leave it Off.
- **Head smoothing in VR** (on the first card): steadies the camera against the tracking's small shakes while the head
  holds still. *Off*: the camera follows the headset exactly (for steady tracking). *Light* (the default): a still
  head stays steady, a moving one is followed at once. *Strong*: the steadiest, but leaning and moving the head lag a
  little behind (a slow lean about a sixth of a second).

The *Picture* sliders (brightness, contrast, gamma) and Page Up / Page Down while playing change the headset's
picture as they do the monitor's. The in-game exit confirmation (an overlay drawn on the monitor only) stays off
while VR is on.

## Recommended settings
What the test PC (RTX 4070 Ti Super, Quest 2 through Virtual Desktop) plays with, at about 55-60 FPS in races and the
headset at 72 Hz or a little below the FPS:

- *VR tab*: True stereo, Headset picture scale **110%**, Anti-aliasing in VR **DLSS** Quality (or **SMAA** for
  the sharpest picture without DLSS), Glow in VR **20%**, Exposure 0, Foveated rendering **Medium**, Edge margin Off,
  Head smoothing **Off** or Light.
- *Display*: Two-thread rendering as the launcher sets it (on, but **off on AMD cards**: slower there, and it lost the
  device in races). Anisotropic filtering Automatic, HDR off.
- *Detail*: Dynamic resolution off; the rest Game / off as you like.
- *Gameplay*: Mirror and reflection updates **Live**. *Static (faster)* is only for when the FPS is too low: about 10%
  more FPS, but the reflections and the rain on the glass freeze, and the windscreen gets a frozen bluish reflection
  of the dashboard that is very visible from the cockpit.

## Keys
The number row, on every keyboard (the keypad works too, for those who have one):

| Key | Keypad | |
|---|---|---|
| 0 | 5 | Recentre: where your head is and the direction you face become the seat (sit as you drive). It also happens by itself when you get into the car |
| 2 / 1 | 8 / 2 | Seat up / down (2 cm, with Ctrl 10 cm) |
| 5 / 6 | 4 / 6 | Seat left / right |
| 4 / 3 | 9 / 3 | Seat forward / back |
| [ / ] | | The picture darker / brighter (Exposure in VR, a quarter stop) |
| F12 | | The FPS on the monitor |

The seat starts at the game's own eye point for each car. The seat keys keep what you set by themselves, in
`data\user\vr_seat.ini` (with your saves, so it moves to a new install with `data\user`): set it once. Delete that
file to go back to the game's seat. The seat keys can be set in `vr.ini`: `key_recentre`, `key_up`, `key_down`,
`key_left`, `key_right`, `key_forward`, `key_back`, each one character (a letter, a digit, `[`, `;` ...), `F1` to
`F12`, or `pageup`, `pagedown`, `home`, `end`, `insert`, `delete`, `space`, `tab`.

The other keys of earlier builds (7, 8, 9 and the keys for comparing and saving pictures) are for development now:
they answer only with `dev_keys = 1` in `vr.ini`, so they never get in the way of the game.

## vr.ini
Optional, next to `DriveClubGame.exe` (`runtime\vr.ini`): `key = value` lines. A `vr.ini` without the launcher
switch also turns VR on (for running the game without the launcher). The launcher's VR tab wins over the lines it
has rows for (anti-aliasing, glow, exposure, smoothing, foveated rendering, edge margin). Defaults:

```
afr = 1            # with Stereo mode AFR: alternate frames; 0: one picture for both eyes (full frame rate, no depth)
ipd = 0.063        # metres between the eyes (the runtime's own with runtime_fov)
rigid = 1          # the camera fixed to the car body; 0: the game's head on springs (it sways in VR)
seat_x = 0         # the eyes relative to the driver's eyes the game sets for each car, metres:
                   # x left, y up, z forward (seat_y, seat_z); data\user\vr_seat.ini (the seat keys) wins
runtime_fov = 1    # each eye drawn with the field of view and eye distance the runtime gives it (drawn symmetric,
                   # the headset gets just its own part); 0: cockpit_vfov
resample = 1       # each eye brought to the runtime's exact size and angles (Lanczos), so it is shown pixel for pixel
frame_pose = 1     # each frame stated to the headset with the turn it was really drawn with; 0: the hook's
auto_recentre = 1  # recentre on getting into the car (race start, restart, another car); 0: only by the key
cockpit_vfov = 90  # the cockpit camera's vertical field of view in the headset, degrees
hud_layer = 1      # in the car the HUD is a panel of its own ahead of the seat; 0: drawn into the picture
hud_distance = 1.1 # the HUD panel: metres ahead, hud_width = 1.4 (metres, 16:9), hud_y = 0 (metres above the eyes)
screen_vfov = 40   # the still screen's height, degrees
window = 1         # the window (for watching and recording): one eye's frames at 16:9, the middle band of the
                   # eye's picture with the HUD and the menus stretched back to 16:9; 0: every frame as the headset
                   # gets it (both eyes in turn). window_eye = 0 the left eye, 1 the right
reproject = 0      # AFR only, experimental: 1 the other eye made from this frame's picture and depth, 2 both eyes
                   # from a frame drawn between them
turn_comp = 0      # AFR only: the older eye's picture turned by the car's turn since it was drawn
dev_keys = 0       # 1: the development keys (comparing and saving pictures, DLSS model, AFR modes)
audit = 0          # 1: the log names passes that draw one eye only or read the left eye for both (costs ~10 FPS)
key_recentre = 0   # and key_up = 2, key_down = 1, key_left = 5, key_right = 6, key_forward = 4, key_back = 3
```

## How it works
- A hook in the game's driver head camera update (1.28; the bytes are checked before patching, otherwise there is no
  head tracking and everything is a still screen) replaces the camera matrix the renderer draws from: the car body's
  turn and position plus the headset's pose, and in AFR every other frame the other eye.
- The bonnet, bumper, dash and chase cameras get the head where the scene takes its camera (Scene::SetCamera), and
  the game's culling gets the headset's field of view turned with the head, so what is behind the car is drawn. The
  way was found by [hookmanuk's VR fork](https://github.com/hookmanuk/driveclub-recomp).
- True stereo is Vulkan multiview, ported from the same fork: the main images get a second layer, and each draw goes
  to both eyes at once with its clip x moved by the eye's offset. The passes that build per-eye data (fog, shadows,
  the tiled lights, the glow, the game's motion vectors) run for each eye. DLSS runs twice a frame, on each eye's
  layers.
- Each eye is drawn with a symmetric projection that covers the runtime's angles, then resampled to exactly what the
  runtime asks for, so the headset shows it pixel for pixel.
- Each frame is matched with the hook call it was drawn from (the camera position in the G-buffer pass's constants),
  so it is stated to the headset at exactly the pose it was drawn from; the runtime turns it to the head's pose at
  display time.
- The game keeps its 16:9 aspect (the HUD's and the menus' canvas) and draws in the eye's shape: a hook where the
  scene takes its camera narrows the driver's head camera's projection to the render's shape.
- The HUD apart: at a frame's first HUD draw into the display buffer the scene is copied out and the buffer cleared,
  so the HUD draws alone (its alpha kept as coverage). In the car the eyes get the scene and the HUD is a quad layer;
  the window and the still screen get both put together again. The split point and the alpha fix follow
  [hookmanuk's VR fork](https://github.com/hookmanuk/driveclub-recomp).

## Known limits
- True stereo: a lower FPS than AFR. Far away the picture gets soft and thin detail shimmers (DLSS or a bigger
  *Headset picture scale* help). Menus stay flat.
- AFR: each eye gets half the game's frame rate, so the picture can shake a little, most of all near objects while
  turning. Below about 90 game FPS (45 per eye) it feels laggy when you move your head. The game's TAA blends each
  frame with the one before, which in AFR is the other eye: SMAA, Off or DLSS suit AFR better.
- Meta Link: the picture judders in True stereo (not solved). Use Virtual Desktop or SteamVR; ALVR goes through
  SteamVR too (not tested here).
- DriveClub Recomp is heavy on the CPU: a lower headset picture helps little when the CPU is the limit.
- No FSR and no frame generation from DLSS.
- The game's camera motion blur is off while VR is on (the eyes 6.3 cm apart look like motion to it).
- A few picture glitches remain (some screens before and after a race).
- Tested with one headset, two runtimes and one graphics card.
