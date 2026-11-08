# Hot Hatch (example mod)

A small mod that shows the kinds of change most mods use: table cells, text, XML inside a resource pack, and an added file (from the `files` folder). Read `mod.json`, which has a comment on each patch.

Try it (from the repository root, with the game folders set up as in docs/modding/getting-started.md):

```bash
dcmod mod check tools/modkit/examples/hot-hatch
dcmod mod build tools/modkit/examples/hot-hatch -o built
```

`built` then holds `newdata/vehicles.csv`, `gui/lams/english.csv`, `global.rpk` and `data/mods/hot-hatch-readme.txt`, plus `dcmod-report.json` (which mod changed what; do not copy that one). Copy the rest into the game's `mods` folder, or import this folder on the launcher's Mods page and enable it.

To make your own, copy this folder, change `id` and `name`, and replace the patches. See [docs/modding](../../../../docs/modding/README.md) for every operation.
