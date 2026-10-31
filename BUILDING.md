# Building

The game runtime and the launcher are built on Windows with the MSYS2 CLANG64 toolchain. Everything else
the code needs is in this repository (`gpu/third_party/`, `third_party/`).

## Toolchain

Install MSYS2 and, from the CLANG64 shell, the build packages once:

    pacman -S --needed mingw-w64-clang-x86_64-{clang,cmake,ninja,pkgconf,vulkan-headers,vulkan-loader,\
    vulkan-memory-allocator,fmt,boost,robin-map,xxhash,sdl3,zydis,ffmpeg,protobuf,nlohmann-json,\
    openssl,zlib,miniupnpc}

## Game runtime

    bash build.sh

This produces `out/DriveClubGame.exe` without diagnostic capture/profiling code.
For a separate diagnostic executable, use `BB_DIAGNOSTICS=ON bash build.sh`; see
[Runtime diagnostics builds](docs/runtime-diagnostics.md).

## Release

    bash packaging/package.sh 1.0

This builds the runtime and the launcher, copies the runtime libraries next to them and writes
`release/driveclub-recomp-1.0-win64.zip`. The zip needs nothing installed on the player's machine.

## Linux (x86_64)

The Linux support comes from the upstream ARM Linux branch
([seregasmith/ARM_linux_driveclub-recomp](https://github.com/seregasmith/ARM_linux_driveclub-recomp),
branch `arm_linux`), which runs the game natively on ARM64 through FEXCore. On x86_64 the game's
code runs on the host CPU: `src/guest_cpu.h` resolves to plain calls in `src/runtime_cpu.c`, and
no FEX library is built. CMake builds `libbbgpu.so`; the loader/HLE runtime is compiled from
`src/` (`src/compat_win.c` is Windows-only). The Win32/WPF launcher is not part of the Linux
build; `driveclub-prepare` (from `tools/prepare_cli.cpp`, using the portable
`launcher/prepare.cpp`) prepares the game dumps, and the AppImage's `AppRun` starts the runtime
with the prepared files.

### Dependencies

Ubuntu 24.04 or newer:

    sudo apt install build-essential clang cmake ninja-build pkg-config curl ca-certificates \
      libvulkan-dev libfmt-dev libboost-dev libxxhash-dev libzydis-dev \
      libprotobuf-dev protobuf-compiler nlohmann-json3-dev \
      libssl-dev zlib1g-dev libpng-dev libminiupnpc-dev \
      glslang-tools libx11-dev libxext-dev libxrandr-dev libxi-dev \
      libxcursor-dev libxfixes-dev libxinerama-dev libxss-dev libxtst-dev \
      libasound2-dev libpulse-dev libpipewire-0.3-dev \
      libxkbcommon-dev libudev-dev libdrm-dev \
      nasm squashfs-tools

Ubuntu 24.04 has no `libsdl3-dev` or `libtsl-robin-map-dev`, and its Vulkan headers are older
than the video core (`VK_KHR_maintenance8`, `VK_EXT_robustness2`). The AppImage dependency
script builds SDL3 (with ALSA/PulseAudio/PipeWire) and a minimal FFmpeg, and installs the
Vulkan headers, robin-map and the Vulkan Memory Allocator header under `out/deps`:

    bash packaging/appimage/fetch-deps.sh

On a distribution that packages SDL3, FFmpeg, the newer Vulkan headers, robin-map and the
Vulkan Memory Allocator header, that step can be skipped; `build-linux.sh` and CMake pick up
whatever `pkg-config` and the CMake package registry find.

### Runtime

    bash build-linux.sh

This produces `out/driveclub-probe`, `out/gpu/libbbgpu.so`, `out/driveclub-prepare` and
`out/driveclub-launcher`. The launcher is the native Linux equivalent of the Windows launcher
(SDL3 + the ImGui already in `gpu/third_party`): it picks the game folders, edits the recomp
settings and patches, prepares the game and starts it. For the AppImage it is the default
entry point; run it directly with `out/driveclub-launcher`, or prepare the dumps headlessly
and start the runtime from a directory that holds `trophies.tsv` (the runtime reads it from
the working directory):

    mkdir -p data
    cp packaging/release/trophies.tsv .
    out/driveclub-prepare --update /path/to/update --base /path/to/base \
      --work data --patches patches/DriveClub.xml
    out/driveclub-probe data/prep/boot-linked.bin --content-profile data/prep/content.bin \
      --patches data/prep/patches.bin --app0 /path/to/update --app0-base /path/to/base \
      --user data/user --mods data/mods --stub-unresolved --timeout 0

The launcher writes the runtime's `BB_*` environment variables (and `camera.ini`,
`picture.ini`, `driveclub.ini`, `patches_enabled.txt`) from its settings and saves them in
`DRIVECLUB_DATA_DIR` (the AppImage uses `data/` beside itself when that folder is writable,
otherwise `~/.local/share/driveclub-recomp`); they are the same options documented in the
README.
`BB_RESTART_COMMAND` lets the in-game settings restart the runtime (the AppImage sets it to
the AppImage itself with `--play`).

### AppImage

    bash packaging/appimage/build-appimage.sh 0.4.0

This builds the runtime and the native Linux launcher (unless `SKIP_BUILD=1`), stages an
AppDir and writes `release/driveclub-recomp-0.4.0-x86_64.AppImage` and its `.sha256`. Running
the AppImage without arguments opens the launcher; `--play` skips it. The launcher keeps its
data in `data/` beside the AppImage when that folder is writable, like the Windows build, and
otherwise in `~/.local/share/driveclub-recomp`. DATA FOLDER > Change... selects another
folder (moving or copying the existing data); the choice is saved in `data-path` beside the
AppImage (or in the XDG config directory for a read-only install). `DRIVECLUB_DATA_DIR`
overrides it; `DRIVECLUB_PORTABLE=1` or a `portable` marker selects `data/` when no folder has
been chosen. It needs
`squashfs-tools` and network access once for the AppImage type-2 runtime (kept in
`out/appimage/`). Useful variables:
`APPIMAGE_COMP` (default `zstd`), `APPIMAGE_ZSTD_LEVEL` (15), `APPIMAGE_BLOCK` (128K) and
`APPIMAGE_RUNTIME` for an already downloaded runtime.

The package bundles SDL3, the minimal FFmpeg, fmt, protobuf, zydis, libpng, the launcher's
ImGui code, its bundled interface font and the C++ runtime. glibc, the Vulkan
loader/ICD/driver, the X11 stack (through XWayland on Wayland desktops), the font stack and the
audio backends stay the host's (bundling them breaks the driver and the desktop). It is built on
Ubuntu 24.04, so it needs glibc 2.39 or newer on the player's machine.

`.github/workflows/appimage.yml` builds the AppImage on `ubuntu-24.04` for tag pushes
(`v*`), manual runs and pull requests that touch the pipeline, extracts the built AppImage and
smoke-tests it (`--version`, `--selftest`, `--help`), failing on unresolved shared libraries,
and uploads it as an artifact.

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

## Faster release build (profile-guided)

The player release is built with link-time optimization and a recorded profile (`pgo/default.profdata`): `packaging/package.sh` does this by itself when the profile is there. To build it by hand: `BB_PGO=use BB_LTO=ON bash build.sh`. The default `bash build.sh` stays the quick build.

To record a new profile after the code has changed a lot: build with `BB_PGO=generate BB_LTO=ON`, run the game's benchmark and a race or two with your usual settings (the profile is written every 30 seconds into `pgo/`), then merge the `.profraw` files with `llvm-profdata merge -output=pgo/default.profdata pgo/*.profraw`.
