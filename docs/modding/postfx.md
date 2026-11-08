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
| `dcmod.postfx.scale(fraction, slot)` | The size a slot draws at, 0.05 to 1 of the picture (for chains that downsample, see the HDR stage). |

The slots run one after the other, slot 0 first, each reading the picture the one before made and each with its own 16 numbers: `dcmod.postfx.use("grade", 0)` then `dcmod.postfx.use("vignette", 1)` gives a colour grade with a vignette over it. A mod that wants a single effect never needs the slot.

A filter can also be chosen without a script: set `DC_POSTFX=<name>` in the environment, with the file in a mods folder.

The game checks the file about once a second while a filter is active and loads it again when it changed, so the effect can be tuned while the game runs (`dcmod mod watch` into the game's `mods` folder).

## The HDR stage: slots 3 to 8

Slots 0 to 2 work on the finished picture. Slots 3 to 8 (six of them) run earlier, on the linear HDR picture right after the upscaler and before the game's own bloom, tone mapping and HUD. That is where the graphics packs of other games do their work (exposure, grading in linear light, fog, ambient occlusion, depth of field), and it is the only stage that can read the scene's depth. Use the slot number in the calls:

```lua
dcmod.postfx.use("hdr_grade", 3)        -- first HDR filter
dcmod.postfx.param(0, 0.15, 3)          -- its first number
dcmod.postfx.use("hdr_depth_fx", 4)     -- second HDR filter, reads what slot 3 made
```

The shader has more to read here:

```glsl
layout(binding = 0) uniform sampler2D picture;       // linear HDR (RGBA16F), the upscaled picture
layout(binding = 1) uniform sampler2D linear_depth;  // distance from the camera in metres (R32F), the sky is 1e6
layout(binding = 2) uniform sampler2D motion;        // pixels, previous minus current (RG16F)
layout(binding = 3) uniform sampler2D gbuffer;       // RGBA8 g-buffer, the alpha holds the object number
layout(binding = 4) uniform sampler2D params;        // 4 by 1 texels (RGBA32F), see below
layout(binding = 5) uniform sampler2D scene;         // the HDR picture as it was before the first HDR slot
layout(binding = 6) uniform sampler2D history;       // what this slot drew the frame before
layout(binding = 7) uniform sampler2D lut;           // the filter's own picture: postfx/<name>.png beside the shader
layout(push_constant) uniform P { vec2 resolution; float time; float has_depth; vec4 values[4]; float has_history; float has_texture; vec2 texture_size; } p;
```

`lut` (binding 7) is the PNG that has the name of the shader (`postfx/<name>.png` next to `postfx/<name>.spv`, 8 bit RGB or RGBA, at most 2048 by 2048), sampled linearly and repeating; `has_texture` is 1 when there is one and `texture_size` its size in pixels. Use it for a colour lookup table, noise, a lens dirt picture or a gradient. It is loaded when the shader is, so restart the filter (switch it off and on) after changing the PNG. The example `tools/modkit/examples/lut-grade` grades the finished picture with a 32x32x32 table laid out as 32 squares side by side (1024 x 32); `make_lut.py` in it writes one, and a table exported from a photo editor in that layout works too. Checked in the game: the grade turns blues to a cooler teal in the shadows and warms the highlights, with no banding; with the amount at 0 the picture is unchanged.

`history` (binding 6) is this slot's own output of the frame before, in the same size and format (RGBA16F in the HDR stage), and `has_history` is 1 when it holds one (0 on the first frame, after a resize and after the shader changed; the picture stands in then). It allows effects over time: smoothing, trails, accumulation, and state kept between frames. The alpha channel of the HDR picture is free for such state (the game ignores it), for example `hdr-tonemap` keeps its current exposure in the alpha of every pixel and reads it back from `texelFetch(history, ivec2(0, 0), 0).a`, so its automatic exposure adapts over several frames (parameter 7 is the speed). Checked in the game: on a night start the picture brightens step by step over about a dozen seconds instead of jumping.

`has_depth` (the old `pad`) is 1 when bindings 1 to 4 hold the scene and 0 on the finished picture, where the picture stands in at each of them. The depth, motion and g-buffer images are the size of the game's render resolution, the picture is the output size; sample them with the same `uv`.

**View position from the depth.** `params` texel 0 is `a`, texel 1 is `b` (`texelFetch(params, ivec2(i, 0), 0)`):

```glsl
vec2 ray = vec2((a.x * uv.x + a.z) * b.z + b.x, (a.y * uv.y + a.w) * b.w + b.y);
float z = texture(linear_depth, uv).r;
vec3 view = vec3(z * ray, -z);           // right handed, forward is -z, units are metres
```

Normals follow from the cross product of the neighbours' view positions (use the texel size of the depth image, `1.0 / textureSize(linear_depth, 0)`, not the output's).

**Channel order at the HDR stage.** The picture of slots 3 to 8 is in the game's own channel order: blue is in `.r` and red is in `.b` (green is where it should be). Reading and writing the picture unchanged works whatever the order; colours you make up in the shader (tints, a sky, a fog colour, luminance weights) must be written the other way round, for example `vec3 tint = vec3(1.0, 0.82, 0.55).bgr` and luminance `dot(c, vec3(0.0722, 0.7152, 0.2126))`. Checked in the game: a constant of (0.10, 0.27, 0.75) written to the sky came out orange, and swapping the picture itself with `.bgr` gave blue shrubs and orange mountains. Slots 0 to 2 (the finished picture) are not affected. All the HDR examples do this.

**How bright the scene is at the HDR stage.** The values here are scene-referred but exposed already: measured with a filter that paints pixels above a threshold, the brightest parts of a sunny scene (sky near the horizon, white kerbs) sit between about 0.45 and 0.98, nothing is above 1, and the game's colour lookup table does the rest afterwards. A tone mapping curve therefore wants about one stop of exposure first. The example `tools/modkit/examples/hdr-tonemap` has Reinhard, ACES, Hable and a soft shoulder with exposure in stops (F9 steps through the curves, PageUp and PageDown change the exposure) and a view that paints what is brighter than a threshold in magenta. Checked in the game: ACES with +1 stop gives a clearly punchier, brighter picture than the original. It also has an automatic exposure (parameters 5 and 6): the shader takes the geometric mean of the brightness at 40 points of the picture and moves the exposure towards a target, up to 2.5 stops either way. At night on Chile (03:00) the road and the horizon become readable with it on and stay almost black with it off. It adapts over time through the history binding (see above).

**Following the sun.** `dcmod.sun()` ([lua.md](lua.md)) says where the sun is in the picture and how bright it is, so a script can hand that to a filter as parameters. The example `tools/modkit/examples/hdr-sunrays` does this: light shafts (a radial blur of the open sky, found from the depth, towards the sun) and lens ghosts, strength following the sun's brightness and switched off when the sun is behind the camera or below the horizon. Checked in the game with the sun's position forced to a fixed point (shafts radiate from it); with the real sun the filter stays off whenever the sun is behind the camera, as it was in the frames taken.

**Reflections.** The example `tools/modkit/examples/hdr-wet-road` finds flat ground from the normals the depth gives (using the world's up in view space, which the script computes from `dcmod.camera()`), keeps to dark grey ground (asphalt), marches the reflected ray through the depth buffer, blends the picture found there in with a Fresnel weight and darkens the road. Rays that leave the picture use the sky above. The reflections are blurred more the further the reflected thing is (parameter 6 is the roughness, 0 gives a mirror) and strongest at grazing angles, so the road looks damp and glossy rather than flooded. Checked in the game; it costs about 40 depth lookups and 9 picture lookups a pixel on the road.

**Lens effects.** `tools/modkit/examples/lens-filter` works on the finished picture (slot 0, HUD included): chromatic aberration that grows towards the edges, barrel distortion, a sharpen and film grain, with the four numbers as parameters. Checked in the game.

**Height fog with scattering.** `tools/modkit/examples/hdr-height-fog` works the fog out exactly along each pixel's ray: the density falls off with the height above the ground (the script takes the car's height as the ground and the camera's height from `dcmod.camera()`), the optical depth is the integral of that density over the distance the depth gives, and the sun lights the haze (a glow round the sun and a tint towards it, from `dcmod.sun()`). Checked in the game at 17:00 on Chile: a soft haze that brightens towards the low sun. The 08:00 frame did not come out (the run was still in the menu).

**Occlusion and bounce light over time.** `tools/modkit/examples/hdr-ssgi` marches six rays per pixel over the hemisphere above the surface (the normals come from the depth), darkens pixels that rays hit and takes a little of the colour of what they hit. The occlusion is smoothed over frames with the history binding: the running value sits in the alpha of the picture and is read back from where the pixel was a frame ago (the motion vectors say where), clamped to stay near what the current frame sees so that moving things leave no ghost (without the clamp the first version left dark car-shaped smears on the road). Checked in the game: contact darkening at the base of palms, rocks and grass and under the car, no ghosting; the effect is deliberately mild.

**Sky with clouds.** `tools/modkit/examples/hdr-sky` paints the sky where the depth says sky: a gradient from the horizon to the zenith that follows the sun's height (blue by day, orange at a low sun, dark at night), a sun disc and glow in the real sun's place, and a drifting cloud layer (five octaves of noise projected on a plane overhead, lit from the side the sun is on, thinning into the haze at the horizon). Cloud cover is the variable `coverage` in the script (0 clear to 1 overcast). Because a filter only sees view-space vectors, the script also passes the camera's right, forward and up axes seen from above so the shader can turn a view ray into a direction over the ground and the clouds stay put when the camera turns. Checked in the game: the first version showed hard-edged, blocky cloud shapes (a `sin` based hash loses precision at the large plane coordinates near the horizon); with an integer style hash and a gentler perspective they are soft streaks.

**Lens dirt.** `tools/modkit/examples/hdr-lens-dirt` uses the filter's picture (binding 7, `make_dirt.py` writes a grey PNG of smudges, specks and hairs): a wide soft measure of what is brighter than a threshold lights the dirt, so it stays invisible in shade and glows when the camera looks towards the sun. Checked in the game: with the sun behind the camera only a few faint specks show; looking towards the low sun the sky fills with glowing specks and haze. The specks come out round, so it reads more like water droplets than hair and smears; replace the PNG with your own to change that.

**Volumetric sunlight.** `tools/modkit/examples/hdr-volumetric` marches every view ray in 22 steps (denser near the camera), tests each point for the sun with a short march towards the sun through the depth buffer, and adds the light that gets through, weighted by a Henyey-Greenstein phase function that peaks when looking at the sun; what it adds is white-yellow haze that palms, pylons and buildings cut into. The numbers are the scattering per metre, the strength, how far along the ray light is gathered and how tightly it scatters towards the sun. Checked in the game with the sun's direction forced to a point ahead of the camera (in the real races the sun was behind the camera in every frame I took, so I could not see the real thing): at a strength of 1.2 the whole picture was blown out to white, at 0.07 a bluish haze with lit gaps between the palms and dark silhouettes on the other side. Strength is the number to tune first, it is very sensitive. It is a cheap approximation: only what the depth buffer shows can block the sun, so shafts through things that are off screen are missing.

**Stacking the examples.** All six HDR slots and the finished-picture slot can run at once (checked in the game: sky with clouds, sun shafts, occlusion, height fog, ACES tone curve and lens dirt on separate slots, with the colour table on top, on Chile at 16:00): the effects combine without artifacts, and the sky, haze and dirt read as one look. Two things to know. The defaults of the examples are each tuned alone, so together they are too bright and hazy (turn the fog density, the sun shafts and the dirt down); and the cost adds up: that run held 13 to 20 frames a second where the same scene with no filter held 40 to 45 at this resolution on the test machine, mostly the three effects that march the depth buffer (occlusion, fog, volumetric). Slots are drawn in order, so put what the others should see (the sky) first and what works on the final look (tone curve, dirt) last.

**Thick volumetric fog.** `tools/modkit/examples/hdr-fog-volume` is the one to use for fog; `hdr-height-fog` and `hdr-volumetric` are simpler and cheaper but flat. Every view ray is marched (22 steps, finer near the camera) through a density field in the WORLD: ground fog that thins with height, shaped by two layers of drifting 3D noise (large patches, billows inside them). At each step the sun is tested through the fog itself (three taps towards the sun give the optical depth, so the inside and far side of a bank are dark and cool and its sunward edge glows) and against whatever stands in the picture between that point and the sun (palms, pylons and grandstands cut shafts into it); the ambient colour is the average of the open sky across the top of the picture; a two lobe phase function adds the glow towards the sun; and what is behind the fog is dimmed by the transmittance, so distant things disappear into it. To work in the world the shader needs the camera's place and axes and the sun's direction every frame: `main.lua` passes them (right, forward, position, sun, then density, falloff, ground height and a thickness multiplier, all 16 parameters), and the ground level is where the car's wheels are. Tune `DENSITY` (0.006 mist, 0.012 default, 0.03 and up very thick) and `FALLOFF` (0.03 a tall bank, 0.1 a thin carpet) at the top of the script. Checked in the game on Chile at 16:00: the first version was a blown out white wash (the sun term was ten times too strong and the noise had too little contrast); now the fog sits in patches, the hills and palms fade into it by distance, the lit side glows warm and the sky colour tints it. Cost: the first version ran at 46 to 56 ms a frame here, the tuned one at 34 to 42 ms (against 22 to 27 ms without it) after four changes with no visible difference: rays going up stop at the height where the fog is too thin to see, the sun taps use a one-lookup version of the field, the sky average takes four lookups instead of ten, and 18 steps instead of 26. Known limits: the patchiness is subtle on the road itself (it shows most in the distance), it shades with only what the picture shows, and the fog is cut off at 260 m.

**Chains with other sizes.** `binding 0` is what the slot before drew (the scene for the first one). `dcmod.postfx.scale(fraction, slot)` sets the size a slot draws at (0.05 to 1 of the picture, 1 by default): the next slot reads the smaller image, and `scene` (binding 5) is still the full-size picture, so a chain can downsample, blur, upsample and put the result back on the scene, the way a bloom does. If the last slot draws smaller than the picture it is scaled back up (linear). The example `tools/modkit/examples/hdr-bloom` is a bloom with an anamorphic glare made of five slots of one shader (bright pass at half size, two blurs at a quarter and an eighth, a tent upsample, a composite at full size).

The example `tools/modkit/examples/hdr-grade` is a cinematic grade in linear light (exposure in stops, contrast around middle grey, saturation, split toning, vignette, grain). `tools/modkit/examples/hdr-depth-fx` has distance fog that takes the colour of the sky, a depth of field that focuses on the car, and ambient occlusion from the depth, plus two views for checking (param 3 set to 1 shows the distance, 2 shows the normals).

Checked in the game (Ferrari 458 on Chile, DLSS Performance at 4K): the grade made the reds richer, the horizon warmer and the shadows teal with the HUD untouched; the distance view showed near ground dark and far hills fading to white; the normals view showed the road flat and up-facing and the car and palms in detail; the depth effects gave soft contact shading, a blur that grows away from the car, and haze that matched the sky in the afternoon and in the evening, at 83 to 103 frames a second (the grade alone cost about 5 ms at 4K).

The bloom chain was checked with exaggerated settings first (threshold 0.25, bloom 1.6, glare 1.2): the grandstand roof and the haze glowed and a soft streak came off the lights, at 82 frames a second; the example's own defaults are milder. Limits of the HDR stage: it lives in the game's anti-aliasing pass, so it only runs while DLSS or FSR anti-aliasing is on (the same condition as the motion vectors); up to six filters; the depth is the game's own (the transparent things, glass and smoke, are not in it); there is no previous-frame input yet.

## Example

`tools/modkit/examples/vignette-filter` has the shader (`vignette.frag`), the compiled `files/postfx/vignette.spv` and the script. Checked in the game (Ferrari 458 on Chile, DLSS Performance): with the mod the corners of the picture are dark and the colours warmer, at 96 frames a second.

## What was checked

In the game (Ferrari 458 on Chile): the filter switches on from a script (dark corners, warm tint) with DLSS Performance and with no upscaler at 4K native; changing a parameter while racing changes the picture; `dcmod.postfx.use("off")` restores the plain picture; replacing the `.spv` file during the race loaded the new shader within a few seconds (a red-only filter, HUD included); a missing name, a text file and random bytes each give a warning in the log and leave the picture alone, and switching back to a good filter works. Two filters in a chain (the vignette in slot 0, a red-only filter in slot 1) gave the dark corners and the red-only picture together. Not checked: frame generation, HDR (skipped by design), Linux.

## Limits

- Slots 0 to 2 on the finished picture read only that picture (no depth, motion or previous frame); there is no previous-frame input yet in the HDR stage either.
- The picture is the one the game made, with its HUD: a filter cannot leave the HUD out.
- Skipped with HDR output.
- The file is looked up in the mods folders by name: names use letters, digits, `_` and `-`.
