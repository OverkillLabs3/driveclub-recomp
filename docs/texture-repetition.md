# Reduce texture repetition

Some native textures form visible repeating rows across large surfaces.
**Reduce texture repetition** breaks up compatible road, ground and wall layers,
plus eligible textured plastic, leather and fabric grain in car interiors. It changes sampling,
without replacing texture assets.

## The setting

Find **Reduce texture repetition** in the launcher's **Scene detail** card.
The choice takes effect on the next game start.

| Performance preset | Default |
| --- | --- |
| Custom, Handheld, Low, Medium, High, Ultra | On |
| PS4 | Off: original sampling |

The saved manual choice is retained until another preset or Reset is selected.
Existing PS4 profiles without the setting keep it off. Other legacy profiles
inherit the PC default. `texture_repetition=0/1` is stored in launcher and runtime
configuration; `BB_TEXTURE_REPETITION=0/1` overrides it at startup.

## Material scope

Selection uses native material-family provenance and shader dataflow. It does
not inspect mesh shape: an eligible family is a conservative proxy for extended
surfaces with visible tiling, not proof that every surface is flat. Unknown or
mixed ownership is excluded. There is no per-texture-ID whitelist.

The world cohort admits road, ground, roadside verge, concrete, pavement, brick,
roof, tile, tarmac and wall families, plus native terrain, landscape and scree
families. Static cliff/mountain, snow/ice, salt-flat, path and field substrates
also qualify. These terrain families cover more than road-adjacent surfaces.
Vegetation, trees, bark, foliage blends, animated or deformed object families,
glass, effects and authored markings are excluded before the positive rules.
Ambiguous basic/rock/stone object families, mixed riverbed/water families and
intentional rumblestrip markings remain outside this cohort.

The separate interior cohort covers `leather`, `leather_glow`, `leather_stitched`,
`fuzzy_felt` and `fuzzy_felt_stitched` families. Only qualifying grain normal/detail
reads and a matching scalar grain/AO layer change. Albedo, stitches, alpha control
data and other layers retain their original samples. Some textured plastic uses
the same native grain family as leather, including materials on interior-shell
meshes. Selection follows the grain layer, not a plastic display name. Glass,
paint, logos, seatbelts, perforated or embossed patterns, generic plastic/carbon
families and rain/weather families are excluded. This does not enable every
dashboard, pillar or upholstery material.

All source families of every known owner of a program must qualify for the same
cohort. Programs shared with an excluded family keep original sampling.

## Layer checks

Both cohorts require fragment shaders writing deferred color, normal and material
targets; discard/cutout shaders are skipped. Samples need mipmaps, normalized
float 2D/2D-array sampling and Repeat/Mirror on both axes. UVs must contain uniform
scaling of interpolated attributes. Unsupported or ambiguous UVs remain original.

World samples must pair sRGB color RGB with BC5 normal RGB on structurally
matching UVs. Interior grain normals must use RG without B/alpha dependence in
the normal output. BC1/BC7 fabric additionally requires a confirmed signed RG
normal decode and no raw RGB/albedo output path. BC4 UNORM scalar grain/AO is
admitted only when used through R alone and paired with an accepted grain normal
on the same UV. Unscaled stitch reads and nonlinear varying products are skipped.

Explicit LOD, fetch/gather, depth/comparison, integer, cube/3D, single-mip and
instruction-offset sampling remain original. Sample alpha is always preserved.
Unique artwork baked into an otherwise eligible layer remains a regression risk;
material provenance cannot identify every authored detail inside an image.

## Sampling and cost

The shader retains its original sample and blends translated texture phases from
a deterministic sparse point catalogue. Smooth, local weights avoid a fixed
four-corner blending grid. All coefficients are nonnegative and sum to one;
there is no global-mip mean or contrast gain. Matching layers share phase offsets
and weights, including mixed Repeat/Mirror pairs. No UV rotation or density
scaling is introduced. Alpha retains its original sample.

The blend starts at about 512 screen pixels per repeat and reaches full strength
at about 32. Close detail retains the original result. When the blend is zero,
a shader branch skips the catalogue and additional reads. Gradients are calculated
before all new branches and loops and reused for every translated sample.

At positive blend, the catalogue checks 16 neighbouring cells and adds about
3.14 texture reads on average, with a data-dependent count. These checks also
cost arithmetic and branches; fewer reads alone do not establish faster shaders.
Blending can soften fine grain, and some original repetition remains. The feature
reduces visible repetition rather than eliminating every repeated motif.

A local Fraser Valley comparison measured 129.98 FPS with the effect off and
107.26 FPS with it on (about 17.5% lower). Both used 45 measured seconds after
15 seconds of warmup, 4K output with DLSS Performance and a 237 FPS cap. These
were single runs with separate cold caches; the release logs cannot establish
that shader compilation had completely stopped. This is a substantial observed
cost, not a cross-GPU or fully warmed performance guarantee.

The setting is frozen at startup and participates in shader-cache identity after
configuration loading. Relevant image formats, number formats, mip-chain and
Repeat/Mirror state participate in permutation compatibility. Changing the
requested setting does not mutate compiled pipelines; restart is required.

## Reproducing the registries

Supply the original extracted packages:

```sh
python tools/generate_world_repeat_registry.py global.rpk vehicleglobal.rpk
python tools/generate_interior_repeat_registry.py global.rpk vehicleglobal.rpk
python tools/test_world_repeat_registry.py
python tools/test_interior_repeat_registry.py
```

The tested 1.28 packages yield 617 world and 44 interior pixel-program hashes,
with no overlap. These are candidates; the layer checks can reject them at
compilation. The registries contain no texture IDs, image data or shader
instructions. Generators print package SHA256 hashes and derive ownership again.

## Validation and limits

- Thirteen world and five interior generator tests cover family exclusions, every-owner
  and every-source closure, renamed display names and deterministic output.
- Thirty-three production C++ selector cases cover qualifying layers, stitch/mask
  exclusions, decoded fabric normals, raw albedo rejection, UVs and sampler guards.
- Five reconstructed native interior graphs select the expected grain layers and
  exclude stitches; an unsupported felt permutation selects nothing. These use
  audited formats with representative descriptors, not current live GPU bindings.
- Reconstructed Canadian and Japanese terrain graphs select matching diffuse/normal
  layers while leaving global, packed and scalar control layers original. These
  also use representative descriptors; no broader UV or single-target shader
  fallback is introduced.
- Launcher and fresh-process runtime tests cover presets, legacy migration,
  opt-out, save/reload, environment precedence and the startup/restart boundary.
- Thirty-two actual-helper Sirit SPIR-V variants pass `spirv-val`, covering
  two SPIR-V versions, 2D/array inputs, both gradient paths, all Repeat/Mirror
  combinations and two sequential expansions. Control-flow/dataflow checks
  verify supported cells, guarded reads, normalization, empty/zero fallbacks,
  original alpha and derivative placement. Six deliberately broken variants
  are rejected. See [the portable test instructions](../tools/repeat_sampling_test.md).
- Changed C++ integration units compile against existing local dependencies.

These checks do not establish final visual quality, cross-GPU performance or a
clean release build. Visual checks of grain retention, motion, streaming and
normal-map lighting remain separate from the structural shader test. No
frame-rate gain is claimed.
