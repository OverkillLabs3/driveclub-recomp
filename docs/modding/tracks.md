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

## What does not work

**A track under a new name does not load.** Copying the three files under a new name (with the names inside the packs and the packs' `requires` changed, and the track's pictures in `newui/art` copied under the new name) makes the game fault while it loads the world. A row that only adds a new `Track Name` with the old `RPKName` also faults, so the game keys more than the files on the track name. A scan of the game's files for the track name (`dcmod archive grep NAME --binary`) finds, besides the three track files, the table row, the localisation text, the pictures in `newui/art` (menu and map pictures by name) and two more places: `scripts/lua/libraries/weatherpresetlookup.lua.bin`, a Lua table that maps each track name to its weather presets (the row-only copy still faulted with that table changed to the new name, so it is not the only cause), and `data/databases/dc.tour` (the events). The fault is a null pointer read right after a lookup of a resource by its identifier in the loaded packs (guest function at offset `0x780f7e`, a cache of 16 entries over the packs): the game asks for a resource that is not there. Identifiers of resources are 48 bit hashes of their names (type in the top 16 bits) and the hash function is not known to the kit, so a pack's resources cannot be renamed consistently yet; a copy keeps the old identifiers under new names. Adding a key to a compiled Lua table is also not possible with the kit's patches yet (`lua.const` changes constants, it cannot add entries). Until that is solved, a new track is made by replacing the world of a track you do not need.
