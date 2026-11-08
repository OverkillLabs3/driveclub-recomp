# Recipes

The recipes were run with `dcmod mod build` on version 1.28 data and the result inspected; the two car recipes were also run in the game.

## Change a car's stats

```json
{ "id": "me.fast-a45", "patches": [
  { "op": "csv.set", "file": "newdata/vehicles.csv", "key": "Vehicle Name",
    "match": "a45_amg", "set": { "TopSpeed": 321, "HorsePower_BHP": 999 } } ] }
```

Only those two cells change; the other 57 KB of the file is byte for byte as it was.

## Change a text in the game

```json
{ "op": "text.replace", "file": "gui/lams/english.csv", "find": "Solo Time Trial Challenge", "replace": "Solo Time Attack" }
```

## Change a graphics setting inside a pack

```json
{ "op": "rpk.patch", "file": "global.rpk", "resource": "viewportquality.xml", "ops": [
  { "op": "xml.set", "path": "ViewportQuality/Configuration[@name='default']/General",
    "attr": "lensdust", "value": "false" } ] }
```

The 21 MB pack is written again with the one attribute changed; every other resource is identical.

## A new car that shares an existing car's model

```bash
dcmod car variant fe_458 my_458 -o my-458
```

This writes a mod with a new row in `newdata/vehicles.csv` and its own copy of the car's physics record in `vehicledefs.vdef`. Then change what you want, for example the mass of the new record or its stats:

```json
{ "op": "vdef.set", "file": "vehicledefs.vdef", "name": "my_458", "offset": 3052, "expect": 1565.0, "value": 1200.0 },
{ "op": "csv.set", "file": "newdata/vehicles.csv", "key": "Vehicle Name", "match": "my_458", "set": { "TopSpeed": 300 } }
```

## Show one car's model on another car

```bash
dcmod car swap fe_458 a45_amg -o a45-as-458
```

Starting a race with the A45 then shows the 458's model. See [cars.md](cars.md).

## Add a line to a camera file

```json
{ "op": "text.append", "file": "data/cameras/vehicle/mercedes_a45_amg.txt", "text": "* a comment" }
```

## Two mods together

```bash
dcmod mod build mod-a mod-b -o built
```

If both set the same cell, the output has `conflict: newdata/vehicles.csv | a45_amg | TopSpeed: "321" from mod-a is replaced by "400" from mod-b`, and mod-b wins.

## Reshape a car or a track in Blender

```
dcmod mesh export @vehicles/ferrari_458_italia.rpk -o ferrari.glb
```

Open `ferrari.glb` in Blender (File > Import > glTF). Move vertices, add or remove triangles, keep every object's name, select the objects and export glTF binary with normals on (they are written into the game's vertex data), materials off, and texture coordinates on if you changed them or added vertices (Blender then splits vertices along UV seams, which is fine: the number of vertices may change). Then:

```
dcmod mesh import @vehicles/ferrari_458_italia.rpk edited.glb -o mymod/files/vehicles/ferrari_458_italia.rpk
```

The folder `mymod/files` is a mod (add a `mod.json` with `dcmod mod new`). With the Blender add-on in `tools/blender` the two steps are menu entries. A track pack works the same way (`@worlds/canada_road_circuit_02_n.rpk`); see [models.md](models.md) for what is and is not changed.

## Your own sound or music

```
dcmod audio info @audio/fmodstudio/gui.bank
dcmod audio wav @audio/fmodstudio/gui.bank countdown_expired_positive01_L -o original.wav     # hear what is there
ffmpeg -i mysound.mp3 -ar 48000 -sample_fmt s16 mysound.wav                                  # any audio to WAV
dcmod audio import @audio/fmodstudio/gui.bank countdown_expired_positive01_L mysound.wav -o mymod/files/audio/fmodstudio/gui.bank
```

The countdown sound now plays your sound (it can be shorter or longer). The same works for the race music in `music.bank` (`All Torque ...` and the other 51 tracks). `dcmod audio extract` and `replace` move the encoded streams between sounds without encoding. See [audio.md](audio.md).

## A mod with settings

A mod that lets the player choose how strong a change is:

```json
{
  "id": "me.power",
  "settings": { "power": { "label": "Top speed", "type": "number", "default": 320, "min": 200, "max": 450 } },
  "patches": [
    { "op": "csv.set", "file": "newdata/vehicles.csv", "key": "Vehicle Name", "match": "a45_amg",
      "set": { "TopSpeed": { "$setting": "power" } } }
  ]
}
```

Build it with the player's choice: `dcmod mod build me.power -o out --settings choices.json` where `choices.json` is `{ "me.power": { "power": 410 } }`. A script of the mod reads the value with `dcmod.setting("me.power", "power")`. See [mod-format.md](mod-format.md).

## Share a mod

`dcmod mod pack mymod -o mymod.zip` writes the folder as a ZIP. The ZIP holds only what is in the folder: patches, scripts and the files you added, none of the game's own files.
