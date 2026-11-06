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

## Giving a car another model

`dcmod car swap DONOR VICTIM -o MODFOLDER` writes a mod that gives the car VICTIM the model and textures of the car DONOR: it copies the donor's pack and top mips pack over the victim's, and renames the packs the copy requires and the asset name in its resource names to the victim's. Everything else of the victim (table row, physics, sounds, cameras, dashboard) stays. In the running game, with `dcmod car swap fe_458 a45_amg`, starting a race with the Mercedes A45 AMG shows the Ferrari 458 Italia (a picture of the game window during the race shows its body and badge). The donor can also be a pack you changed yourself with `rpk` and `tex`.

## Adding a new car

**A variant works.** `dcmod car variant NAME NEWNAME -o MODFOLDER` writes a mod that adds a new row to `newdata/vehicles.csv` (its own name and unique number) and a copy of the car's record in `vehicledefs.vdef` under the new name, which the row uses as its `Vehicle Def`. The new car shares the model, textures, sounds and cameras of the original and has its own physics definition and table values. In the running game a car made this way is chosen by its name and races (checked with the benchmark mode, which starts a race with a car by its `Vehicle Name`), and the game's own vehicle list (`GameState.GetVehicles`, read by a mod script) holds the new car next to the original, with `class=3`.

`vehicledefs.vdef` is `EVOSLITLVDEF3-02`, a count (156) and records of 3616 bytes that start with a 32 byte name. The patch operations are

```json
{ "op": "vdef.clone", "file": "vehicledefs.vdef", "from": "Ferrari_458", "name": "my_458" }
{ "op": "vdef.set", "file": "vehicledefs.vdef", "name": "my_458", "offset": 3052, "expect": 1565.0, "value": 1200.0 }
```

`offset` is counted from the start of the record. The only field identified so far is the mass in kilograms at offset 3052 (the correlation with the table's `Weight_Kg` over 131 cars is 0.99; the Ferrari 458 has 1565). Other offsets correlate with power, torque and handling but are not identified: the tyre blocks start near offset 48 (four wheels, 208 bytes each) and the engine data is around 900 to 1400.

**A copy with its own model files does not load yet.** `dcmod car clone` copies the car's 29 asset-named files under a new Asset, lists them in `scripts/filemanifest.cfg` (the game's own list of its files), renames the packs the car's pack requires and the asset name inside its resource names, and copies the dashboard definition (`<asset>.def` in `global.rpk`; the game builds `<asset>.def.rpk` from it and the car's `<asset>_dash` texture). Each of these removed a failure: without the renamed names the game stopped with a null read while setting up texture streaming (guest offset `0x5feee0`); with them it no longer crashes, but the loading screen never finishes (the benchmark reports that the race did not start within 240 s). What the load waits for is not known yet. Until it is, a car with a new model is not possible; a variant of an existing model is.

## Dashboards (the instruments inside the car)

Each car's instrument cluster is an XML file (`assets/dashboards/<asset>.def`, stored as a binary resource in `global.rpk`, 125 of them) that draws pictures from the car's dashboard texture (`<asset>_dash.tif` in the car pack) and shows live values as numbers, needles, arcs and lights:

```bash
dcmod dash list                 # the dashboards
dcmod dash cat vw_golf_gti      # one dashboard's XML
dcmod dash check my.def         # elements, attributes and live values
dcmod dash binds                # the values a dashboard can show (vehicle.speed_kph, vehicle.revs, vehicle.gear, ...)
```

The elements are `canvas`, `font`, `image`, `maskedimage`, `fadeimage`, `number`, `gear`, `timer`, `needle`, `arc`, `box`, `line`, `slider`, `slidebox` and `visible` (shows its parent when a value is in a range). A dashboard is changed with `rpk.patch` on `global.rpk` and its resource name (for example `vw_golf_gti.def`), with the picture changed by `tex.set`. `dcmod dash check` reports elements, attributes and values it does not know; the 125 dashboards of the game were read to build its tables, and the dashboard of the Golf GTI passes it.
