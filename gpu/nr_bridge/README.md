# Neural rendering (experimental, off)

An optional pass that runs NVIDIA's DLSS Neural Rendering model on the picture. It is for people who want to
tinker and send improvements. It is off by default. The launcher exposes controls when compatible hardware,
the adapter and the model are available. The release includes the adapter but does not contain the model.

## What is in the source

- `gpu/shadps4/video_core/renderer_vulkan/vk_neural_rendering.{h,cpp}`: the renderer side. It prepares the
  picture (`host_shaders/neural_prepare.comp`), runs the model through the bridge, and puts the result back
  (`neural_resolve.comp`, `neural_ui.comp`). With the model's input scaled below 1 it works on fewer pixels,
  which is where the saving comes from.
- `gpu/shim/bbport_neural_settings.{h,cpp}`: the `nr_*` settings: up to four passes, each with a stage, style
  and intensity, the model preset, the input scale and the resolve method.
- `gpu/nr_bridge/`: the adapter that loads `nvngx_dlssnr.dll`. It is built on its own (MSVC, with the NVIDIA
  DLSS SDK headers), see `CMakeLists.txt`. The output has to be named `bbport_nr_nvngx.dll`; the model checks
  the name.

The hooks are a few lines each: the presenter (final frame, stage `Present`), the anti-aliasing pass (stages
`PreUpscale` and `PostUpscale`) and the instance and platform setup (Vulkan extensions). When `nr_enabled` is
0 every hook returns at once and no extension is requested.

## Turning it on

1. Put your own `nvngx_dlssnr.dll` next to `DriveClubGame.exe` in `runtime`, or point `BB_NR_RUNTIME` at its
   folder or DLL path. The adapter `bbport_nr_nvngx.dll` stays next to `DriveClubGame.exe`; source builds can
   build it separately with the provided CMake target.
2. Enable Neural Rendering in the launcher, or set `nr_enabled=1` in the settings file (or `BB_NEURAL=1`), then start the game. It must be set before the
   start, because the extensions are requested when the device is created.
3. **F7** switches it off and on, **F6** steps through the styles of the first pass.

It needs a GeForce RTX card and a recent driver. The log says why it did not start.

## Where to help

- Make it work with the HDR output (it runs before the HDR conversion today).
- A launcher card for it.
- Tests that run the model on a synthetic frame without the game.
