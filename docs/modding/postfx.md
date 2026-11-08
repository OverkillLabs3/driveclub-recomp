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
| `dcmod.postfx.use(name [, slot])` | Switches the filter `postfx/<name>.spv` on in a slot (0 to 2 on the finished picture, 3 to 8 on the HDR picture, 0 by default). `"off"` or `nil` switches the slot off. |
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

**Following the sun.** `dcmod.sun()` ([lua.md](lua.md)) says where the sun is in the picture and how bright it is, so a script can hand that to a filter as parameters. The example `tools/modkit/examples/hdr-sunrays` does this: light shafts (a radial blur of the open sky, found from the depth, towards the sun) and lens ghosts, strength following the sun's brightness and switched off when the sun is behind the camera or below the horizon. Checked in the game with the sun's position forced to a point in the sky ahead (the real sun was never in frame in my test runs, so the sun's own screen position is only checked by the log): golden shafts fall through the palms and along the fence and the road, with a warm haze round the sun. The first version veiled the car and drew faint vertical stripes; the shafts now fade on things closer than about 45 m (the car and the road under it stay clean) and every pixel starts its march at a different place along its ray. With the real sun the filter stays off whenever the sun is behind the camera or under the horizon.

**Reflections and a wet road.** `tools/modkit/examples/hdr-wet-road` makes the asphalt damp everywhere (darker, glossy, a soft blurred reflection) with puddles where a noise field fixed in the world says so (darker still, nearly a mirror, a sharp reflection of the palms, the pylon and the sky). The ground is found from the normals that the depth gives, turned into the world with the camera's axes, and limited to dark, grey, cool pixels so that sand and gravel stay dry (the red channel is `.b` at this stage); the puddle field is world position noise, so puddles stay where they are while the car drives past; the reflected ray is marched through the depth buffer (36 steps, growing) and a Fresnel weight blends the hit in, with a blur that grows with the roughness and with how far the ray went; rays that leave the picture use the sky picture where the ray points. The script passes the camera's right and forward axes and position and four numbers at its top: `WETNESS` (how much of the road is puddle), `STRENGTH`, `DARKEN`, `DAMP_BLUR`. Checked in the game on Chile at 15:00: puddles with clear palm and pylon reflections that stay put, glossy damp asphalt between them, sand dry. The first version (a uniform sheen) did not read as wet and the second one wetted the sand as well. Known flaws: the reflection of tall things stretches into vertical streaks (it is what a screen space reflection of a vertical object does; it looks like the real thing at a distance and artificial close up), a kerb right next to a puddle can pick up a little sheen, and it costs about 36 depth lookups on every road pixel (25 ms a frame here against 22 without it).

**Lens effects.** `tools/modkit/examples/lens-filter` works on the finished picture (slot 0, HUD included): chromatic aberration that grows towards the edges, barrel distortion, a sharpen and film grain, with the four numbers as parameters. Checked in the game.

**Occlusion and bounce light over time.** `tools/modkit/examples/hdr-ssgi` marches six rays per pixel over the hemisphere above the surface (the normals come from the depth), darkens pixels that rays hit and takes a little of the colour of what they hit. The occlusion is smoothed over frames with the history binding: the running value sits in the alpha of the picture and is read back from where the pixel was a frame ago (the motion vectors say where), clamped to stay near what the current frame sees so that moving things leave no ghost (without the clamp the first version left dark car-shaped smears on the road). Checked in the game: contact darkening under the car, at the base of palms, rocks and grass and along kerbs, no ghosting. The first version was too faint to see; this one has eight rays of five steps, a curve that darkens corners harder, an interleaved gradient noise (the earlier hash showed a checker on flat ground), a history clamp of 0.1 and a fade-out beyond 55 to 140 m where the depth is too coarse. At 130 mph a few blocky smears first showed next to the road edge, where fast motion vectors send the history lookup to the wrong place; the old value is now trusted less the faster the pixel moves (not at all above 24 pixels a frame), and the smears are gone in the same spot at 136 mph.

**Sky with clouds.** `tools/modkit/examples/hdr-sky` paints the sky where the depth says sky, with a physically based atmosphere instead of a gradient: single scattering marched from the camera through Rayleigh (blue sky, red sunsets) and Mie (haze, the glow round the sun) layers, each sample lit through the atmosphere towards the sun, so the colours come out of the physics: muted deep blue overhead and paler towards the horizon at noon, an orange glow low on the sun's side with a dark teal sky above at dusk, dim and cool at night. The sun disc is attenuated by the same air. A cloud layer (five octaves of noise on a plane overhead, lit from the side the sun is on and tinted by the atmosphere's attenuation, so the clouds go orange at sunset) drifts with the wind; `coverage` at the top of the script sets it, 0 clear to 1 overcast. A filter only sees view-space vectors, so the script passes the sun, the world's up and the camera's axes seen from above. Checked in the game on Chile at 13:00 and 18:00. The first version (a two colour gradient) looked like a flat cartoon blue and was replaced. Known flaws: the sky is on the dark side at noon (raise `SUN_I` in the shader), the clouds are faint, and the thin crowns of palms close to the camera still show a faint light fringe against the dusk sky. The game's picture holds a blend of the leaf and its own bright sky at every edge, and the depth calls such pixels sky. Pixels next to geometry now take the colour of that geometry and mix the new sky in by how much sky is round them (the first fix, keeping the game's pixel, drew a white outline along every ridge and was dropped); the ridges are clean, the closest palm crowns are not.

**Lens dirt.** `tools/modkit/examples/hdr-lens-dirt` uses the filter's picture (binding 7, `make_dirt.py` writes a grey PNG of smudges, specks and hairs): a wide soft measure of what is brighter than a threshold lights the dirt, so it stays invisible in shade and glows when the camera looks towards the sun. Checked in the game: with the sun behind the camera only a few faint specks show; looking towards the low sun the sky fills with glowing specks and haze. The first picture (round specks) read as water droplets, so `make_dirt.py` now makes grime: cloudy patches, wipe streaks, a few fingerprint rings, light dust and fine scratches; in the game that gives faint smears and a few glints and is subtle by default. Replace the PNG with your own to change it.

**Stacking the examples.** All six HDR slots and the finished-picture slot can run at once, and the effects combine without artifacts. Every example script has its slot number as the last argument of its `dcmod.postfx` calls; to run several together give each its own slot and put what the others should see first. A set that worked well on Chile at 17:00: slot 3 `hdr-sky`, 4 `hdr-wet-road`, 5 `hdr-ssgi`, 6 `hdr-fog-volume` (with `DENSITY` at 0.006: its default is for a thick fog on its own), 7 `hdr-sunrays`, 8 `hdr-tonemap` (ACES, +0.3 stops, automatic exposure) and `lut-grade` on the finished picture. In the game that gave a deep, hazy, believable evening: soft sun lit clouds, puddles that reflect the palms, contact shading under the car and a thin haze that swallows the hills. It is not cheap: that run took 51 to 57 ms a frame here (18 to 19 frames a second) against 22 to 27 ms with no filter, the cost being the four effects that march the depth buffer (wet road, occlusion, fog, shafts). Each default is tuned alone, so stacking wants the fog lowered, as above, and one or two of the heavy effects left out on a slower card. `hdr-tonemap` also draws its curve and exposure as text in the corner; delete the `dcmod.ui.text` line in its script to hide that.

**Thick volumetric fog.** `tools/modkit/examples/hdr-fog-volume` is the fog example. Every view ray is marched (22 steps, finer near the camera) through a density field in the WORLD: ground fog that thins with height, shaped by two layers of drifting 3D noise (large patches, billows inside them). At each step the sun is tested through the fog itself (three taps towards the sun give the optical depth, so the inside and far side of a bank are dark and cool and its sunward edge glows) and against whatever stands in the picture between that point and the sun (palms, pylons and grandstands cut shafts into it); the ambient colour is the average of the open sky across the top of the picture; a two lobe phase function adds the glow towards the sun; and what is behind the fog is dimmed by the transmittance, so distant things disappear into it. To work in the world the shader needs the camera's place and axes and the sun's direction every frame: `main.lua` passes them (right, forward, position, sun, then density, falloff, ground height and a thickness multiplier, all 16 parameters), and the ground level is where the car's wheels are. Tune `DENSITY` (0.006 mist, 0.012 default, 0.03 and up very thick) and `FALLOFF` (0.03 a tall bank, 0.1 a thin carpet) at the top of the script. Checked in the game on Chile at 16:00: the first version was a blown out white wash (the sun term was ten times too strong and the noise had too little contrast); now the fog sits in patches, the hills and palms fade into it by distance, the lit side glows warm and the sky colour tints it. Cost: the first version ran at 46 to 56 ms a frame here, the tuned one at 34 to 42 ms (against 22 to 27 ms without it) after four changes with no visible difference: rays going up stop at the height where the fog is too thin to see, the sun taps use a one-lookup version of the field, the sky average takes four lookups instead of ten, and 18 steps instead of 26. A later pass (14 steps, no fine noise layer where the large patches say the air is clear) measured 37 ms a frame with the sun in view, about the same as before: the fog is still the most expensive of the examples and I found no cheap way to halve it (a filter slot that draws at a lower size also scales the whole picture down, so a half resolution fog is not possible with one slot). With the real sun ahead the fog glows white-gold round the sun and the hills fade into it, which is the look it was built for. Known limits: the patchiness is subtle on the road itself (it shows most in the distance), it shades with only what the picture shows, and the fog is cut off at 260 m.

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
