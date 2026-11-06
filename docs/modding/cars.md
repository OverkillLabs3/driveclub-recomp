# Cars and dashboards

## What a car is

A car is one row of `newdata/vehicles.csv` (137 rows) and the files named after the row's `Asset` column:

```bash
dcmod car files fe_458        # the row and the 29 files that belong to the Ferrari 458 Italia
```

For `ferrari_458_italia` they are `vehicles/ferrari_458_italia.rpk` (model, materials and textures, 530 resources), `vehicles/ferrari_458_italia_top_mips.rpk` (the largest texture levels), `data/nexus/skel/wiper/...rpk`, `data/nexus/statemachine/vehicles/...rpk`, `data/nexus/merged/...rpk`, and about twenty camera files under `data/cameras/vehicle/` (chase, bonnet, bumper, dashboard, driver head, look back and replay cameras). Other columns of the row name shared data: `Vehicle Def` (an entry in `vehicledefs.vdef`), `Audio Resource`, `AI Audio Resource`, `Animation`.

## What can be changed

| Change | How |
|---|---|
| Stats, unlock level, names, class | `csv.set` on `newdata/vehicles.csv` |
| Cameras | `text.*` patches on the camera files |
| A car's paint, decals, interior textures | `tex.set` with a PNG or DDS on the car's pack (textures whose largest levels are in the `_top_mips` pack cannot be changed yet) |
| Anything inside the pack | `rpk.set`, `rpk.patch`, `rpk.add`, `rpk.remove` |
| The in-car instruments | see below |

Changing a car **in place** (its table row, its pack, its cameras) is the way that works.

## Adding a new car

`dcmod car clone NAME NEWNAME -o MODFOLDER` writes a mod that adds a table row and copies all the asset-named files under a new Asset with `file.copy` patches (the mod holds none of the game's data). The game opens the copied pack when the new car is chosen, but it then stops with a null read in the car loading code. The cause is not known yet: some lookup is keyed by something other than the file names. Copying the files is therefore not enough to add a car, and the command is a starting point for finding what else is needed, not a working feature. A new car is not possible until that is understood.

## Dashboards (the instruments inside the car)

Each car's instrument cluster is an XML file (`assets/dashboards/<asset>.def`, stored as a binary resource in `global.rpk`, 125 of them) that draws pictures from the car's dashboard texture (`<asset>_dash.tif` in the car pack) and shows live values as numbers, needles, arcs and lights:

```bash
dcmod dash list                 # the dashboards
dcmod dash cat vw_golf_gti      # one dashboard's XML
dcmod dash check my.def         # elements, attributes and live values
dcmod dash binds                # the values a dashboard can show (vehicle.speed_kph, vehicle.revs, vehicle.gear, ...)
```

The elements are `canvas`, `font`, `image`, `maskedimage`, `fadeimage`, `number`, `gear`, `timer`, `needle`, `arc`, `box`, `line`, `slider`, `slidebox` and `visible` (shows its parent when a value is in a range). A dashboard is changed with `rpk.patch` on `global.rpk` and its resource name (for example `vw_golf_gti.def`), with the picture changed by `tex.set`. `dcmod dash check` reports elements, attributes and values it does not know; the 125 dashboards of the game were read to build its tables, and the dashboard of the Golf GTI passes it.
