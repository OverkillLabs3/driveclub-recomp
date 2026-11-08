# Shaders and graphics

The game's shaders are the console's GPU programs. The runtime translates each one to SPIR-V (the Vulkan shader format) the first time it is used and keeps the result in a cache. A mod can replace any of them with its own SPIR-V file.

## How a replacement is found

A file named like the translated shader in the folder `shaders` of a mod (or of the player's `data\user\shader\patch` folder) is used instead of the game's shader. The name is the stage, the shader's hash and a variant number: `fs_0x0000000063b26ca4_0.spv`. Stages are `fs` (pixel), `vs` (vertex), `cs` (compute), `gs`, `es`. The mod's folder comes first, in the order of the mods folders. A shader without a replacement file is untouched, and removing the file brings the original back on the next start.

In a launcher mod the files go in the mod's `files\shaders` folder (`dcmod shader install` does that).

## Getting the shaders

1. Stop the game and delete the folder `data\user\cache` (the cache holds the translated shaders, and shaders that come from it are not dumped).
2. Set the environment variable `DC_DUMP_SHADERS=1` and start the game; the first start is slow because every shader is translated. Drive a lap so that most shaders are used.
3. The SPIR-V files are in `data\user\shader\dumps`. About 1600 are written (about 670 pixel, 710 vertex, 220 compute). `dcmod shader list data\user\shader\dumps fs` lists them.

## Editing

```
dcmod shader dis fs_0x0000000063b26ca4_0.spv -o a.spvasm     (needs spirv-dis)
   ... edit a.spvasm in a text editor ...
dcmod shader as a.spvasm -o fs_0x0000000063b26ca4_0.spv      (needs spirv-as)
dcmod shader install fs_0x0000000063b26ca4_0.spv mymod
```

`spirv-dis` and `spirv-as` come with the Vulkan SDK and with SPIRV-Tools; they must be on PATH. Keep the interface of the shader as it is (the inputs, outputs and resources it declares): the game binds them by what the original used. Colour outputs are written one component at a time (`OpAccessChain %output_f32 %frag_color0 %u32_id_1`, then `OpStore`), so changing a colour means changing the value stored. Disassemble a few shaders of the same stage to learn the pattern before changing anything.

## Checked in the game

`tools/modkit/examples/shader-tint/tint.py` changes the 293 pixel shaders that write colour so that the green and blue outputs are 0. With the result in the mods folder the whole picture, the world and the instruments, is drawn in a single colour (blue on the capture of the race: the image is blue where it was not black); with the folder moved away the game looks as before (captures at 1902x1033, HDR off). Without the files the game looks as before.

## Limits

- A replacement is looked up with variant number 0 when the shader comes from the cache and with its own variant number when it is translated; a shader that has several variants (the same program compiled for different states) is replaced as variant 0 only. The dump names show which variants exist.
- Only the shader is replaced. Its resources (textures, buffers, constants) are what the game binds for the original.
- A shader that does not fit the pipeline (a different interface) makes the game fail to create the pipeline; the log says which. Remove the file to recover.
- The shaders are the game's own code; do not publish dumps or edited copies of them. A mod should carry only what you wrote, or a script like the example that makes the change from the player's dumps.
