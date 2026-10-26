# High-FPS camera workaround (experimental)

This is an optional workaround requiring review, not a complete accuracy fix. Camera behavior remains incorrect in some situations and is not identical to the original 30 FPS behavior.

Enable **Gameplay → Patches → High-FPS camera workaround (experimental)** in the Windows launcher. It is enabled by default, is saved in `launcher.ini` as `camera_patch=0/1`, and applies on the next game launch. Turning it off installs none of this bundle's runtime hooks. For direct runtime launches the equivalent opt-in is `BB_CAMERA_PATCH=1`; an unset value or `0` leaves it off.

## Bundle

- Run continuous procedural camera noise at its 30 Hz reference cadence and interpolate the effect matrix between ticks. The main camera continues updating every rendered frame; unrelated event impulses retain their native route.
- Observe direct vehicle point velocities over 1/30 s for camera acceleration. Other camera target types retain the tested interval-mean observation path.
- Normalize the replay controller's linear and angular correction gains for shorter updates, retaining the original correction limits and teleport handling downstream. This changes replay feedback, not the engine's global physics tick.
- Feed the recorded pre-feedback velocity derivative to the replay head/HUD observers and the driver's GForce animation input, avoiding a derivative of the resimulation correction itself. Keep the native animation filter/jolts and restore the private raw velocity history.
- For the two measured continuous chase-camera springs only, blend each native position/velocity integration result 50% toward the fractional native-30-FPS response. Authored damping, stiffness, force and limits remain unchanged. A 50% per-update blend is not an exact midpoint between two independently evolved complete camera trajectories.

All code hooks check their expected DriveClub 1.28 instruction signatures before installation. Unsupported individual hooks are skipped and logged. This does not guarantee all-or-nothing installation when only part of an image matches; that policy needs review. Non-Windows builds have a no-op installer. No capture files, GPU readbacks, or periodic diagnostic reporting are included.

## Remaining issues and review requested

- Gameplay, replay and cockpit behavior is improved in the supplied clips, but residual shake and incorrect camera motion remain. Do not treat the clips as evidence of full 30 FPS parity across every camera.
- Review the replay correction normalization, observation timing/cache lifetime and pause/seek/camera-switch behavior.
- Review the scope and justification of the 50% chase spring blend; it is an experimental compromise rather than a reference-accurate setting.
- Review the x86-64 gateways, register preservation, signature coverage and partial-install policy. More in-game coverage is needed across replay camera groups, frame pacing, cars and tracks.

## Validation

The Windows runtime and launcher were compiled. Local build-only standard-header compatibility flags for existing upstream code were excluded from the changes. Standalone native gateway tests cover procedural cadence, point/interval observation, the spring blend, replay/head/HUD/GForce bridges, native-30/stale-cache fallbacks and unchanged physics buffers in the observation paths. The optional installer test uses a locally flattened, unpatched 1.28 image and never executes its guest code. No game image is included. The launcher regression also checks that the camera switch, Motion Blur Slow/Fast choice and current `draw_threads` setting survive save/reload independently.

From the repository root in MSYS2 CLANG64:

```sh
for name in motion spring replay; do
  clang -O2 -Wall -Wextra -Werror -Wno-unused-function tools/camera_patch_${name}_test.c -o out/camera_patch_${name}_test.exe -lm
  ./out/camera_patch_${name}_test.exe
done
# Optional: locally supplied unpatched 1.28 flat image.
clang -O2 tools/camera_patch_install_test.c -o out/camera_patch_install_test.exe -lm
./out/camera_patch_install_test.exe /path/to/guest-flat.bin
# Actual launcher setting persistence/environment test, with temporary settings only.
clang++ -O1 -std=c++20 -include cstdlib -static -o out/camera_patch_launcher_test.exe tools/camera_patch_launcher_test.cpp launcher/prepare.cpp launcher/gpu_check.cpp launcher/datx.cpp launcher/unlock.cpp launcher/graphics.cpp launcher/carswitch.cpp launcher/updater.cpp -lgdiplus -lgdi32 -luser32 -lshell32 -lole32 -ldwmapi -lwinhttp -lbcrypt -lcomctl32
./out/camera_patch_launcher_test.exe
```
