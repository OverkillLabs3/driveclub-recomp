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
