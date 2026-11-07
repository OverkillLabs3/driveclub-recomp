# Cameras

The cameras of every car are plain text files in the game's archive: `data/cameras/vehicle/<kind>_cameras/<car>.txt`, with the kinds `chase`, `bonnet`, `bumper`, `dashboard`, `driver_head`, `look_back` and `replay` (and shared files in `data/cameras/vehicle/include/`). A car file names its values and pulls the shared defaults in with `#include`, so it is short:

```
#include include/default_chase_camera_standard.txt

POSITION_OFFSET 0.0 1.265 3.849
TARGET_Z_OFFSET 1.325
ROOF_LINE_HEIGHT 1.0845
DELTA_DIST_FOR_LOOKBACK 0.4377
```

`POSITION_OFFSET` is the camera's place relative to the car (x sideways, y up, z behind, in metres). Read a file with `dcmod archive cat data/cameras/vehicle/chase_cameras/ferrari_458_italia.txt`, list them with `dcmod archive list "data/cameras/vehicle/chase_cameras/*"`.

## Change a camera

An ordinary `text.replace` patch does it (see [patches.md](patches.md)):

```json
{ "op": "text.replace", "file": "data/cameras/vehicle/chase_cameras/ferrari_458_italia.txt",
  "find": "POSITION_OFFSET 0.0 1.265 3.849", "replace": "POSITION_OFFSET 0.0 3.0 9.0" }
```

The example `tools/modkit/examples/far-camera` is this patch. Checked in the game (Ferrari 458 on Chile, Time Trial): without the mod the chase camera is close behind the car, with it the car is small in the picture, seen from higher and from farther behind; the log shows the runtime reading the changed file from the mods folder. A change to a shared file under `include/` changes every car that includes it. To change a group of cars with one patch use `each` (for example `"each": "data/cameras/vehicle/chase_cameras/*.txt"`, and a `regex` in `text.replace` for the value).

## What is not known

The meaning of every key of the shared files (field of view, smoothing, shake) was not worked out; read the shared files in `include/` for the names, and change one value at a time while watching the game.
