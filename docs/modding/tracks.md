# Tracks

## What a track is

A track is one row of `newdata/tracks.csv` (the `Track Name` and `RPKName` columns hold the same text, for example `canada_road_circuit_02_n` for Fraser Valley) and three files named after it:

```bash
dcmod track files canada_road_circuit_02_n
```

| File | Content |
|---|---|
| `worlds/<name>.rpk` | the world: terrain, scenery, road, about 160 MB, 2641 sub meshes for Fraser Valley |
| `worlds/<name>_patch.rpk` | changes to the world for the game's updates (20 MB) |
| `data/leveldata/<name>.rpk` | level data: the route, triggers, AI data |

The world and patch packs require `worlds/<country>_common.rpk`, and the level data and the patch require the world pack by its name.

## What can be changed

- **The geometry of an existing track.** Export the world pack with `dcmod mesh` and import it again; a loose file `worlds/<name>.rpk` in a mod replaces the world ([models.md](models.md)). Checked in the game: every vertex of Fraser Valley's scenery raised by 60 m, and the scenery floats in the sky. Positions and the number of triangles of a part can change as long as the part stays within the vertex buffer it lives in; the buffers of the world packs are full (65535 vertices), so a part that needs more vertices than it has cannot be extended yet.
- **The table row** (name shown, time of day, weather, route type, chunk) with `csv.set` on `newdata/tracks.csv`.
- **Textures and shaders** like anywhere else ([studio.md](studio.md), [shaders.md](shaders.md)).

## A track under a new name

```bash
dcmod track clone canada_road_circuit_02_n canada_road_circuit_09_n -o mytrack --title "My Valley"
dcmod mod build mytrack -o "<game folder>\mods"
```

`track clone` writes a mod that copies the track as a new one: a table row (new `Track Name` and `RPKName`, the next `UniqueID`, the title), copies of the three files, and everything the game keys by the track's name. NEWNAME must be as long as the track name and differ from it in the circuit number only (`_02_` to `_09_`): names are stored inside packs that keep their size. The copy has the world, the route and the interface pictures of the original, so it is a second Fraser Valley until its packs are changed; the geometry can then be edited with `dcmod mesh` ([models.md](models.md)).

What the mod does, which is what a copy needs (each was found by running a copy and reading what the game could not find; `track clone` writes all of it):

1. `csv.add` the row of `newdata/tracks.csv`, `file.copy` of the three packs and `manifest.add` of their paths.
2. `rpk.rename` in the copies: the packs' `requires` (the level data and the patch require the world pack by name) and the **names** of resources. The game finds the world and the level data by name (`canada_road_circuit_02_n` is the name of the world resource, `..._roads`, `..._spawnpoints` and so on those of the level data), so the names must carry the new circuit.
3. `rpk.replace` in the world and patch world: the layer names (`Cana_C02_Tier1` becomes `Cana_C09_Tier1`), the text of the track name in the world and its scene objects, and, with `"hash": "crc32"`, the **CRC32 of the names** that the world stores (a map from the CRC32 of a name to the scene object; without it the world asks for `canada_road_circuit_02_n` and the game faults while it reads the map).
4. `rpk.copy` of every picture in `newui/art/*.rpk` named after the track (track info background and logos, track map and its variants, loading screen), and `text.insert` of the track's lines in `newui/control/pre_loaded_textures.ctl`, in `newdata/rank/elite.csv` and in the 22 language tables `gui/lams/*.csv` (each field is wrapped in the characters 2 and 3 there).
5. `lua.alias` on the scripts that have a table by track name (`weatherpresetlookup.lua.bin`: where it sets `table["canada_road_circuit_02_n"]` it also sets the new name). Without it the loading page's Lua script fails and the game asserts `Lua state machine failed to shutdown`.

Checked in the game (hidden 1920x1080 runs): `canada_road_circuit_09_n` (a copy of Fraser Valley) and `uk_road_circuit_08_r` (a copy of a UK circuit) load and race, with the track name of the copy in the benchmark's log. The copy was not driven through the menus, and the pictures and lap data of the copy are the original's.

Not covered: other kinds of tracks than the ones named `<country>_<kind>_<NN>_<variant>` with a circuit number, tracks whose scripts or tables are keyed in a way that was not met (`track clone` says when a script or table does not hold the name), the tour (`data/databases/dc.tour` lists events by track), and a change of the world's name length.

How it was found, for the next track that fails: a plugin hook on the game's lookup function logs what it cannot find; the guest address of a fault gives the function (the kit's research folder has the Ghidra scripts); the fault `read access to 0000000000000018 at guest offset 0x780f7e` was the world not found by name, `0x69552e` was the map keyed by the CRC32 of a track name, and `0x662260` is the game's own assertion (a `INT 0x41` that prints a message with the call stack; a plugin hook there can read the message from the caller's stack at `rsp + 8 + 0x30`).
