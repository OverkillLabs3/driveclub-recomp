# Recipes

Each recipe was run with `dcmod mod build` on version 1.28 data and the result inspected.

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

## Add a car row that copies another

```json
{ "op": "csv.add", "file": "newdata/vehicles.csv", "key": "Vehicle Name",
  "like": "a45_amg", "after": "a45_amg", "row": { "Vehicle Name": "my_car", "Asset": "my_car" } },
{ "op": "file.copy", "from": "vehicles/mercedes_a45_amg.rpk", "file": "vehicles/my_car.rpk" }
```

This gives a new row and a pack to start from; changing the pack's contents is done with `rpk.set` and `rpk.patch`.

## Add a line to a camera file

```json
{ "op": "text.append", "file": "data/cameras/vehicle/mercedes_a45_amg.txt", "text": "* a comment" }
```

## Two mods together

```bash
dcmod mod build mod-a mod-b -o built
```

If both set the same cell, the output has `conflict: newdata/vehicles.csv | a45_amg | TopSpeed: "321" from mod-a is replaced by "400" from mod-b`, and mod-b wins.
