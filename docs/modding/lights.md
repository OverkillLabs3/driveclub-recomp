# Car lights

Every car pack (`vehicles/<asset>.rpk`) has a `lights` resource: the car's own light sources, such as the headlights, the brake lights and the indicators. The kit reads and writes it, so the colour and brightness of a car's lights can be changed without touching the model.

```bash
dcmod car lights fe_458          # a table of the lights of a car (NAME is its Vehicle Name)
dcmod car lights fe_458 --json   # the same as JSON
dcmod car lights ALL             # checks all 137 cars: every table must read and write back to the same bytes
```

The output of `dcmod car lights fe_458`:

```
index type color                    intensity node               f6 .. f14
3     3    0.895   0.98    1       76        0005f3aa5a8b1f90   0 0 0 2e+04 1 0.7854 0.3491 0 1
```

## Change a light

`light.set` is a patch in `mod.json`. `match` picks the lights and `set` lists the fields to change:

```json
{
  "id": "me.blue-headlights",
  "name": "Blue headlights",
  "version": "1.0",
  "patches": [
    { "op": "light.set", "file": "vehicles/ferrari_458_italia.rpk",
      "match": { "index": [3, 4] },
      "set": { "color": [0, 0.2, 1], "intensity": 1500 } }
  ]
}
```

`match` takes `"index"` (a number or a list), `"type"`, `"node"` (the identifier of the node the light sits on), or `"all": true`. When several keys are given, a light has to fit all of them. A patch that matches no light fails and says how many lights the pack has.

Fields of `set`:

| Field | Meaning |
|---|---|
| `color` | `[r, g, b]`, linear. |
| `intensity` | Brightness. The 458's headlights are 76; 1500 lights the whole road ahead. |
| `node` | Identifier of the node the light is attached to (hex), `""` for none. |
| `type` | The light's type number (see below). |
| `flags` | Two more bytes, `[a, b]`. |
| `f6` .. `f14` | Nine more numbers whose meaning is not decoded yet. |

## What was checked

- All 137 cars' tables have the same layout (records of 63 bytes after a 24 byte header, a 16 byte tail) and are written back byte for byte.
- In the game, at night on Chile, with the Ferrari 458: `color [0, 0.2, 1]` and `intensity 1500` on lights 3 and 4 turned both headlight beams blue and much brighter, and lit the road, the rocks and the banner far ahead. The brake lights (type 1) did not change.
- Changing `f10`, `f11` and `f12` of the headlights (to 30, 1.4 and 0.9) made no difference that could be seen in one frame: they are probably the size and the cone angles of the beam, but that is a guess.

## Types seen on the 458

| Type | Lights | Notes |
|---|---|---|
| 3 | 3, 4 | The headlight beams (the only ones with a `node`). Checked in the game. |
| 1 | 1, 2, 5 to 9 | Red and white lamps at the back and sides, probably the brake, tail and reverse lights. Not checked one by one. |
| 9 | 0 | One light with a very wide cone. Not checked. |
| 6 | 10 | One light with no range. Not checked. |

Other cars use the same types; use `dcmod car lights NAME` to see them, then change one light at a time and look at the result in the game.

# The glow of the lamps

A second resource of the car pack, `vehicle_custom_data`, has the named glow settings of the lamps and of the dashboard backlight: `HeadLight`, `SideLight`, `TailLight`, `BrakeLight`, `DashDisplayBacklight1` and `DashDisplayBacklight2` (all 137 cars have these six; more numbers follow them in tables that the kit keeps as they are).

```bash
dcmod car custom fe_458          # the entries of a car
dcmod car custom fe_458 --json
dcmod car custom ALL             # checks all cars
```

```
name                     color                    intensity f4 .. f7
HeadLight                0.893   0.935   1       184       1 1 1 100
TailLight                1       0.00137 0.000299 8         1 1 1 100
BrakeLight               1       0.00137 0.000299 100       1 1 1 100
```

`custom.set` changes them, with `match` `{"name": "TailLight"}`, a list of names or `{"all": true}`, and `set` with `color` `[r, g, b]`, `intensity` and `f4` to `f7`:

```json
{ "op": "custom.set", "file": "vehicles/ferrari_458_italia.rpk",
  "match": { "name": ["TailLight", "BrakeLight"] },
  "set": { "color": [0, 1, 0], "intensity": 60 } }
```

Checked in the game (Ferrari 458 at night): `TailLight` colour `[0, 1, 0]`, intensity 60, and `BrakeLight` colour `[0, 1, 0]`, intensity 300, made both rear lamps glow bright green with a halo around them. The entries are the glow of the lamp meshes, while the table above (`dcmod car lights`) holds the light sources that light the road. A car's headlights change in both places: the lamp glow here and the beam there.
