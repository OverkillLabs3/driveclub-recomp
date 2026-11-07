# Post-processing filters

A mod can put its own shader over the game's picture, the way colour grading, vignettes, film grain or a different look are made in other games. The filter is a SPIR-V fragment shader in the mod, `postfx/<name>.spv`. A Lua script switches it on and gives it numbers.

The filter sees the finished picture of the game (the 3D scene, after the upscaler, with the game's own HUD in it) and runs before the picture settings of the launcher (gamma, contrast, brightness). The mod's overlay text (`dcmod.ui`) is drawn after it, so it is not filtered. A filter works in the SDR picture; with HDR output on, filters are skipped.

## The shader

```glsl
#version 450
layout(location = 0) in vec2 uv;                       // 0..1 across the picture
layout(location = 0) out vec4 color;
layout(binding = 0) uniform sampler2D picture;         // the game's picture
layout(push_constant) uniform P {
    vec2 resolution;      // size of the picture in pixels
    float time;           // seconds since the game started
    float pad;
    vec4 values[4];       // the 16 numbers of dcmod.postfx.param(0..15): values[0].x is 0, values[0].y is 1 ...
} p;

void main() {
    color = vec4(texture(picture, uv).rgb, 1.0);
}
```

Compile it with any GLSL compiler for Vulkan, for example `glslangValidator -V filter.frag -o files/postfx/filter.spv` (it comes with the Vulkan SDK and with MSYS2's `mingw-w64-clang-x86_64-glslang`). The interface above is fixed: the picture at binding 0, the push constants as shown, one output at location 0. A shader with another interface is refused and the game log says so (`Post filter ...`).

## The script

```lua
dcmod.on_ready(function()
  dcmod.postfx.use("vignette")     -- postfx/vignette.spv of this mod (or of any mods folder)
  dcmod.postfx.param(0, 0.8)       -- values[0].x
  dcmod.postfx.param(1, 0.35)      -- values[0].y
end)

-- parameters can change while playing, for example with the speed:
dcmod.every(0.1, function()
  local car = dcmod.car()
  if car then dcmod.postfx.param(0, math.min(1, car.kmh / 300)) end
end)
```

| Call | Does |
|---|---|
| `dcmod.postfx.use(name [, slot])` | Switches the filter `postfx/<name>.spv` on in a slot (0 to 2, 0 by default). `"off"` or `nil` switches the slot off. |
| `dcmod.postfx.param(i, value [, slot])` | Sets number `i` (0 to 15) of that slot's filter. Values are kept to a thousandth. |

The slots run one after the other, slot 0 first, each reading the picture the one before made and each with its own 16 numbers: `dcmod.postfx.use("grade", 0)` then `dcmod.postfx.use("vignette", 1)` gives a colour grade with a vignette over it. A mod that wants a single effect never needs the slot.

A filter can also be chosen without a script: set `DC_POSTFX=<name>` in the environment, with the file in a mods folder.

The game checks the file about once a second while a filter is active and loads it again when it changed, so the effect can be tuned while the game runs (`dcmod mod watch` into the game's `mods` folder).

## Example

`tools/modkit/examples/vignette-filter` has the shader (`vignette.frag`), the compiled `files/postfx/vignette.spv` and the script. Checked in the game (Ferrari 458 on Chile, DLSS Performance): with the mod the corners of the picture are dark and the colours warmer, at 96 frames a second.

## What was checked

In the game (Ferrari 458 on Chile): the filter switches on from a script (dark corners, warm tint) with DLSS Performance and with no upscaler at 4K native; changing a parameter while racing changes the picture; `dcmod.postfx.use("off")` restores the plain picture; replacing the `.spv` file during the race loaded the new shader within a few seconds (a red-only filter, HUD included); a missing name, a text file and random bytes each give a warning in the log and leave the picture alone, and switching back to a good filter works. Two filters in a chain (the vignette in slot 0, a red-only filter in slot 1) gave the dark corners and the red-only picture together. Not checked: frame generation, HDR (skipped by design), Linux.

## Limits

- Three filters in a chain, one pass each, reading only the picture before them (no depth, no motion vectors, no previous frame yet).
- The picture is the one the game made, with its HUD: a filter cannot leave the HUD out.
- Skipped with HDR output.
- The file is looked up in the mods folders by name: names use letters, digits, `_` and `-`.
