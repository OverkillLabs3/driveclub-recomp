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

A frame that misses its slot shows in the frame log as a long interval. Its line in the frame trace says whether
the time went to read faults (the readback log then names the instruction and the writer), shader compiles,
evictions or the GPU.

## Seamless cubemap filtering

Native Vulkan cube-array sampling is enabled automatically for eligible read-only,
floating-point cubemap resources. This corrects reflection seams caused by filtering each
cube face independently as a 2D-array layer. No environment variable or launch option is
required.

The initial implementation handles normalized explicit-LOD samples (including level 0).
Storage, depth, integer, offset, gather, derivative and mixed/raw uses retain the array path,
as do samplers that disable cube wrapping. The GPU must support `imageCubeArray`; eligible
views must contain complete aligned cube faces. The shader/pipeline cache versions invalidate
older translated shaders and resource layouts, including the earlier opt-in variants.

### Regression checks (MSYS2 CLANG64, from the repository root)

Build the runtime normally with `bash build.sh`, then run:

```sh
clang++ -O2 -std=c++20 tools/native_cube_coords_test.cpp -o out/native_cube_coords_test.exe
out/native_cube_coords_test.exe
clang++ -O2 -std=c++20 -Igpu/third_party/sirit/include \
  -Igpu/third_party/sirit/externals/SPIRV-Headers/include \
  tools/native_cube_shader_test.cpp out/gpu/third_party/sirit/src/libsirit.a \
  -lfmt -o out/native_cube_shader_test.exe
out/native_cube_shader_test.exe out/native_cube_test.spv
spirv-val --target-env vulkan1.1 out/native_cube_test.spv
```

The coordinate test evaluates the production emitter helper against independent cube-face
bases at centers, edges, corners and multiple array indices (2,430 cases). The shader test
uses the same helper in a compute shader with both 2D-array and cube-array sampling, then
validates the generated SPIR-V. These checks require no game files or captured resources.
