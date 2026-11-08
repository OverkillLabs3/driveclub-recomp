# Startup intro presentation pacing

The startup movie `dc_attract_video_30fps_11.v3_256.mp4` contains 30 frames per second. Presenting it at an unrelated cap such as 72 FPS can produce uneven repetitions and visibly distracting judder. This change temporarily limits presentation to 30 FPS while that specific movie is playing, then restores the configured launcher cap. A player's lower cap is never raised; an uncapped setting returns to uncapped afterwards. There is no hardcoded gameplay cap.

The video file, audio timestamps, decoder and game simulation clocks are unchanged. Only the exact startup movie basename is selected. Other movies, track flyovers, replays and driving retain their existing presentation policy. This targets the observed startup cadence problem; it does not claim to eliminate every source of video stalls or shader-compilation stutter.

The AvPlayer source owns a playback token. Successful video start/resume activates it; pause, stop, drained end-of-file and destruction release it. Multiple active players are counted so closing one cannot clear another's active token. The presentation thread recalculates its period and credit only on a cap change and rebases its next slot to avoid releasing a burst of queued flips. Neither launcher settings nor saved preferences are rewritten.

For comparison, set `BB_INTRO_PACING=0` in the game's environment before launch. The default is enabled. No SDL, force-feedback, collision-penalty or OpenXR changes are included.

## Verification

From an MSYS2 CLANG64 shell at the repository root:

```sh
clang++ -O2 -std=c++20 -Igpu/shim tools/test_intro_pacing.cpp -o out/test_intro_pacing.exe
out/test_intro_pacing.exe
bash build.sh
```

Keep assertions enabled for this test. It covers exact basename matching, configured caps 0/15/30/60/72/90/120, repeated start/stop, pause/resume, token destruction, overlapping players and concurrent token transitions. These are policy/ownership checks, not a decoder or real AvPlayer integration test.

Prior local driving builds were manually validated with presentation changing from 30 FPS in the intro to the player's configured 72 FPS afterwards. Logs recorded that transition. The original 60-FPS presentation experiment is superseded by this 30-FPS variant. Completion by naturally reaching EOF still needs to be distinguished from skipping the movie when reporting new manual validation.

No commercial movie, game files or private diagnostic dumps are included.
