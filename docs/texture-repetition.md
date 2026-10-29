# Reduce texture repetition

Some native road and ground textures form visible diagonal rows when their pattern
repeats across a large surface. **Reduce texture repetition** is an optional visual
enhancement that breaks up this repetition on compatible world-material layers.
It does not replace the game's texture assets.

## The setting

Find **Reduce texture repetition** in the launcher's **Scene detail** card.
The choice takes effect on the next game start.

| Performance preset | Default |
| --- | --- |
| Custom, Handheld, Low, Medium, High, Ultra | On |
| PS4 | Off: original sampling |

The saved manual choice is retained until another preset or Reset is selected.
Existing PS4 profiles without the new setting keep it off. Other legacy profiles
inherit the PC default. The setting is stored as `texture_repetition=0/1` in both
launcher and runtime configuration; `BB_TEXTURE_REPETITION=0/1` is a startup override.

This costs extra GPU texture reads. No performance measurements across GPUs or
presets are available yet, including Handheld. The user can disable it separately.

## Which layers qualify

The renderer combines native material ownership with shader dataflow. It does
not enumerate textures or rely on a manually maintained list of material names.

1. All known owners of the pixel program must belong to the native
   `assets/noodle/materials/atg1/` group. Programs shared with vehicle or character
   materials are excluded.
2. The shader must write the deferred color, normal and material targets. Cutout
   shaders with discard are skipped.
3. A qualifying color sample must be sRGB and feed color RGB. A qualifying normal
   sample must be BC5 and feed normal RGB. Alpha-only dependencies do not qualify.
4. The pair must use structurally matching UV expressions containing scaling of
   interpolated attributes by a uniform. Both axes must use Repeat/Mirror, with a
   mip chain and normalized float 2D/2D-array sampling.
5. Only the confirmed color/normal reads change. Alpha, blend masks, material
   parameter maps and all other sampling instructions retain their original data.

This includes eligible terrain, road, verge, concrete, wall, rock and bark layers.
It does **not** imply that every material in those categories qualifies. Grass and
snow coverage has not been visually verified. Individual cutout grass blades and
leaves are excluded by the discard rule.

Explicit LOD, fetch/gather, depth/comparison, integer, cube/3D, single-mip and
instruction-offset sampling are excluded. Nonlinear, texture-driven or ambiguous
UV expressions remain native. So do unsupported normal formats.

This is a conservative heuristic rather than an engine-provided semantic tag.
It cannot identify a unique logo or marking baked into an otherwise qualifying
repeating albedo. Such authored details remain a review/regression risk.

## How sampling changes

The shader keeps its original sample, then samples four deterministic translated
UV phases using the original gradients. Paired color/normal samples share phase
offsets and blend weights. No UV rotation is applied to tangent-space normals.

The blend with the original sample increases with the UV footprint: it starts at
about 512 screen pixels per repeat and reaches full strength at about 32. Close
detail therefore retains the original result. Sharpened blend weights and an
approximate variance correction around the coarsest mip reduce contrast loss;
they are not a guarantee of lossless local contrast.

Each selected instruction adds four gradient samples plus a coarsest-mip read.
The extra reads are currently emitted even where the blend retains the close
original result. This is the main performance cost requiring profiling.

The setting is frozen at startup. Enabled and disabled modes use separate shader
cache versions, and mip-chain, BC5 and Repeat/Mirror specialization state is
included in permutation compatibility. Changing the requested setting does not
mutate already compiled pipelines; restart is required.

UV-expression analysis rejects unsupported operations early and bounds structural
key size to avoid expanding a shared SSA graph indefinitely.

## Reproducing the ownership registry

Supply the original packages from a legally owned game extraction:

```sh
python tools/generate_world_repeat_registry.py global.rpk vehicleglobal.rpk
python tools/test_world_repeat_registry.py
```

The checked-in registry contains 1,604 sorted world-owned pixel-program hashes
from the tested 1.28 packages. It contains no texture IDs, texture data or shader
instructions. Unrecognized programs keep the original sampling. The generator
prints input SHA256 hashes and derives ownership again rather than editing a list.

## Validation and limits

- Launcher regression: all PC/PS4 presets, legacy-profile migration, explicit
  opt-out, save/reload, reset, environment precedence and absence of pseudo-patches.
- Runtime settings: default, saved ON/OFF, both environment override directions,
  persistence and the immutable startup/restart boundary.
- Production C++ selector: 15 positive/negative checks, including alpha-only masks,
  mismatched/unique UVs, clamp, discard, unsupported formats, stage and mip gates,
  and bounded shared-DAG expansion.
- Registry tests: renamed materials, shared world/vehicle programs, missing
  provenance, non-pixel stages and deterministic output.
- Local native-road selector check: reconstructed 919-instruction captured graph
  selected only images 6/7/8/9 (two color/normal pairs), leaving slots 0-5 unchanged.
- Selective-sampling runtime: 9 affected SPIR-V modules passed `spirv-val` in an
  ordinary game launch. Those runtime checks precede the public
  launcher integration. The publication source was compiled and privately linked
  against existing local dependency/runtime objects; this is not a clean release
  build or a cross-GPU performance result.

Motion, streaming, UV boundaries, unusual authored surfaces and other tracks still
need wider visual regression testing. The separate core fixes for native render
feedback timing and warm-cache vertex fetch are tracked by PRs #280 and #281;
this graphics feature does not include or replace them.
