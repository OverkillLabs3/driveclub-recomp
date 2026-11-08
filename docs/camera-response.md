# Camera and HUD response at variable frame rates

The existing **Camera shake workaround** setting selects this implementation.
`BB_CAMERA_PATCH=0` leaves the camera hooks disabled; `BB_CAMERA_PATCH=1` enables
the complete response graph. No additional component settings are exposed.
The launcher keeps its existing presets and saved setting.

The implementation targets DRIVECLUB 1.28 on Windows x86-64. Other platform
builds retain the existing no-op camera installer. Native hook signatures and
copied instruction spans are checked against the supported image.

## Response model

The authored 30 Hz response is the reference. Spring settings, G-force gains,
native filters, clamps, and random impulse magnitudes are retained. The code
does not replace these with hand-tuned strengths or change vehicle physics.

The native integer clock supplies the shared 33,333 microsecond phase. Private
camera state advances on that phase while the game continues to render at its
normal rate. Rendering between observed inputs uses explicit sampling rules;
it cannot recover motion that the engine never observed.

* Procedural camera noise uses rigid rotation interpolation and lifecycle
  resets, avoiding the scale/shear introduced by blending matrix elements.
* Chase springs reconstruct their inputs from observed velocity and orientation
  and apply the original authored recurrence at the native phase.
* Replay pose queries and their velocity origin use a paired query time. The
  physical render sample and private canonical camera sample remain separate.
* Head/torso response publishes its coupled state at three original consumption
  boundaries under the same object/clock/binding lease. Native 30 Hz calls use
  the original path without publishing a replacement state.
* Paused replay continuation uses the selected native clock and a validated
  retained velocity origin. Seeks, reset events, and object reuse invalidate
  stale bindings rather than borrowing another camera's history.

Original instruction spans needed by the private response are copied from the
loaded game image at startup, validated, and relocated in private executable
storage. Game assets and prerecorded instruction-body fixtures are not included.
The runtime writes no diagnostic tapes, screenshots, or trace files.

## Validation

Run the asset-free core tests with Clang and Python:

```
python tests/camera_response/run.py --cc clang --output build/camera-tests
```

They exercise integer-clock cadence and jitter, paired-query overflow and
failure clearing, publication write boundaries and stage order, and replay
binding reset/reuse. They do not execute the game hooks or establish visual
equivalence by themselves.

The accepted implementation was also measured in game across all six gameplay
views in the race and in replay, including replay stop and LT/LT/RT seeks.
Native-30 operator/state comparisons, actual VRR canonical updates, paused
continuation, publication ordering, rigid camera matrices, and a separate
shadow reproduction scenario were checked. Local capture and analysis tools
remain outside this source change.

Comparison video: https://youtu.be/RuXUVNLuZGQ

The middle column is the native-30 recomp reference, not a PS4 hardware capture.
The video uses independent AI runs synchronized at camera changes; trajectories
and seek destinations can differ. The 4K export was assembled from 1080p
recordings.

## Limits

This remains a toggleable compatibility workaround. It does not claim pixel
identity on real PS4 hardware, coverage of every cinematic replay camera or
vehicle, or support for unobserved multi-tick callbacks below the engine's
current timestep clamp. Missing or incompatible source state uses the existing
native/failure path rather than inventing historical samples. The separate 4K
car-material flicker is outside this change.
