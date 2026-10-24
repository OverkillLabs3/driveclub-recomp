# Building

The game runtime and the launcher are built on Windows with the MSYS2 CLANG64 toolchain. Everything else
the code needs is in this repository (`gpu/third_party/`, `third_party/`).

## Toolchain

Install MSYS2 and, from the CLANG64 shell, the build packages once:

    pacman -S --needed mingw-w64-clang-x86_64-{clang,cmake,ninja,pkgconf,vulkan-headers,vulkan-loader,\
    vulkan-memory-allocator,fmt,boost,tsl-robin-map,xxhash,sdl3,zydis,ffmpeg,protobuf,nlohmann-json,\
    openssl,zlib,miniupnpc}

## Game runtime

    bash build.sh

This produces `out/DriveClubGame.exe`.

## Release

    bash packaging/package.sh 1.0

This builds the runtime and the launcher, copies the runtime libraries next to them and writes
`release/driveclub-recomp-1.0-win64.zip`. The zip needs nothing installed on the player's machine.

## Finding a stall

These switches show where the frame time goes. They are off unless set, and cost nothing when off. Set them
in the environment of `DriveClub.exe --play`; with `BB_BENCHMARK` (see the README) the race is the same every
time.

- `BB_FRAME_STATS=1`: every 5 seconds the log gets the flip rate and frame-time spread, where the GPU command
  thread waited, the guest reads of GPU-written data (offloaded, early copies made and used), the faults
  emulated and declined, and the label waits.
- `BB_FRAME_LOG=<file>`: one line per flip, the wall-clock time and the interval since the previous flip (ms).
  For the 1% and 0.1% lows and the spikes.
- `BB_FRAME_TRACE=<file>`: one line per flip with where that frame's time went. The columns are named in the
  first line: the PrepareFrame and frames-ahead waits, GPU thread idle time, the submission lock, read faults
  (count, total, the worst thread), write faults, the present, shader compiles, texture evictions, uploads and
  draws.
- `BB_READBACK_LOG=<file>`: one line per guest read that downloads GPU-written data: the faulting instruction
  and address, how long the read waited for the GPU thread, for the submission that wrote the data and for
  the copy, and the shader or copy that wrote it.
- `BB_PHASES=1` (with `BB_FRAME_STATS`): adds which part of a draw the GPU command thread and the draw
  recording thread spend their time in, and how the GPU command thread's time splits over the PM4 opcodes.
  This adds a clock read per part per draw, so it lowers the frame rate; `BB_PHASE_ONE=<n>` times only one of
  them (`0` the recording phases, `1`-`8` the single parts listed in `gpu/shim/bbport_toggles.h`), which keeps
  the number for that part accurate.
- `BB_PREP_WHY=1`: for the first few draws, why a draw-preparation worker found no matching shader permutation.
- `BB_PREP_ADAPT=0`: turns off the self-test that sets the speculative draw preparation aside when its results
  are not usable (see `gpu/shadps4/video_core/renderer_vulkan/vk_draw_prep.h`).

A frame that misses its slot shows in the frame log as a long interval. Its line in the frame trace says whether
the time went to read faults (the readback log then names the instruction and the writer), shader compiles,
evictions or the GPU.
