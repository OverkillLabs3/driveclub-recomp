# Tutorial: a new car with its own shape

This walks through making a copy of the Ferrari 458 Italia that is a car of its own in the game (own name, own model files), and changing its shape. Every command was run as written on the game's 1.28 files; the result was started in a race.

You need the kit built ([getting-started.md](getting-started.md)), the game folders given to it, and Blender (any version that imports glTF) for the shape.

## 1. Make the copy

```bash
dcmod car clone fe_458 my_car -o my_car
```

`fe_458` is the `Vehicle Name` of the Ferrari in `newdata/vehicles.csv`; `my_car` is the new car's name and the name of its files (`vehicles/my_car.rpk`, its cameras and so on). The command writes the mod folder `my_car` (a `mod.json` with about sixty patches; no game data). Check it and build it:

```bash
dcmod mod check my_car
dcmod mod build my_car -o built1
```

`built1` now holds the 29 copied files and the changed tables. Copy it into the game's `mods` folder to try the car as it is. Checked: the car loads and races (started with the benchmark mode, which picks a car by its `Vehicle Name`) and `GameState.GetVehicles` lists it. Not checked: how the car shows in the game's garage and unlock screens.

## 2. Look at the model

```bash
dcmod mesh info built1/vehicles/my_car.rpk | grep BODYShape
dcmod mesh export built1/vehicles/my_car.rpk -o my_car.glb "*_BODYShape"
```

The export has one object per part, named `ID_N_NAME`; keep the first two numbers when you rename or copy a part. See [models.md](models.md).

## 3. Change it

Open `my_car.glb` in Blender (File > Import > glTF). Move vertices, add or remove triangles, change the UV map. Export glTF binary (`.glb`) with normals on, materials off and texture coordinates on, over the same file name. See [recipes.md](recipes.md) for the Blender add-on, which does the import and export in one menu.

## 4. Put the result in a second mod

The clone mod builds `vehicles/my_car.rpk` with patches; a loose file in the same mod would be overwritten by them. A second mod that requires the first one holds the reshaped pack:

```bash
mkdir -p my_car_shape/files/vehicles
dcmod mesh import built1/vehicles/my_car.rpk my_car.glb -o my_car_shape/files/vehicles/my_car.rpk
```

and a `my_car_shape/mod.json`:

```json
{ "id": "my-car-shape", "name": "My car shape", "version": "1.0", "requires": ["my_car"] }
```

```bash
dcmod mod build my_car my_car_shape -o built2
dcmod mesh info built2/vehicles/my_car.rpk | grep BODYShape       # the new triangle counts
```

## 5. Share it

```bash
dcmod mod pack my_car -o my_car.zip
dcmod mod pack my_car_shape -o my_car_shape.zip
```

Players import both ZIPs on the launcher's Mods page and enable them. The first holds no game data (the launcher builds the copy from the player's own files); the second holds only the model you made.

## What else a car has

The copy shares the original's sounds, cameras and physics values (it has its own copy of the vehicle definition to change with `vdef.set`, see [cars.md](cars.md)). Textures are changed with `dcmod tex` and `tex.set` patches; the dashboard with the `dash` commands.
