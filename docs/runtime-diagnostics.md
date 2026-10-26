# Runtime diagnostics builds

Normal builds exclude capture-trigger polling, shader/buffer dumps, GPU profiling, validation/markers, verbose trace/debug logging and diagnostic-only hot-path timing/counters. Setting their old environment flags does not enable them in a normal executable. Error reports and functional frame/display tracking, mirror feedback, rendering synchronization, the performance overlay and benchmark support remain available.

The opt-in shader quarantine remains functional in ordinary builds: `BB_QUARANTINE=1` retains recent draw/dispatch history, so a device loss can save new suspect compute shaders and load them on the next run. This startup option is cached once rather than querying the environment per command. With quarantine off, only diagnostic builds collect that history.

With MSYS2 CLANG64, build the ordinary runtime as usual:

```sh
./build.sh
```

Explicitly build diagnostics into a separate cache and executable:

```sh
BB_DIAGNOSTICS=ON ./build.sh
```

This writes `out/DriveClubGame.diagnostics.exe` using `out/gpu-diagnostics`; it does not replace `out/DriveClubGame.exe` or reconfigure its normal cache. With CMake directly, use `-DBB_DIAGNOSTICS=ON` in a separate build directory; the default is `OFF`.

For a packaged Windows build, put `DriveClubGame.diagnostics.exe` beside the ordinary exe in `runtime/`, so both use the same adjacent DLLs and upscaling providers. Start the launcher from a process-local diagnostic shell or BAT, for example:

```bat
@echo off
setlocal
set "BB_DIAGNOSTIC_RUNTIME=DriveClubGame.diagnostics.exe"
set "BB_CAPTURE_TRIGGER=D:\captures\capture_now"
set "BB_CAPTURE_DIR=D:\captures"
start "" "DriveClub.exe"
endlocal
```

Create the capture directory first. Creating the trigger file records the next complete frame, as before. Other diagnostic flags retain their existing behavior in this build. The launcher accepts only the fixed sibling filename; it does not copy it over the normal exe. Starting `DriveClub.exe` normally uses the ordinary runtime again. Diagnostic runs can add substantial overhead and should not be used for performance comparisons.

## Regression checks

Configure both variants with `-DBB_BUILD_DIAGNOSTIC_TESTS=ON`, build the `bb_diagnostics_gate_test` target, and run it from each build directory. The test poisons diagnostic environment flags, checks the actual capture implementation and verifies that frame counts, display-buffer registration beyond the fast-list capacity and mirror feedback still work. Assertions remain enabled for this test in release builds.

The `bb_quarantine_test` target also checks both `off` and `on` modes in separate processes. Run `bb_quarantine_test off` and `bb_quarantine_test on` in each build directory. These tests use their own temporary file and exercise disabled recording, ring wraparound, preserving existing/manual entries, saving new dispatch hashes, deduplication, and loading the saved quarantine. They do not require a GPU crash.

The launcher routing regression can be built in MSYS2 CLANG64:

```sh
source ./msys2-env.sh
clang++ -O1 -std=c++20 -include cstdlib -static -o out/diagnostics_launcher_test.exe tools/diagnostics_launcher_test.cpp launcher/prepare.cpp launcher/gpu_check.cpp launcher/datx.cpp launcher/unlock.cpp launcher/graphics.cpp launcher/carswitch.cpp launcher/updater.cpp -lgdiplus -lgdi32 -luser32 -lshell32 -lole32 -ldwmapi -lwinhttp -lbcrypt -lcomctl32
./out/diagnostics_launcher_test.exe
```
