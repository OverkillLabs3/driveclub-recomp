# Game files

The archive of version 1.28 holds 8018 files (46 GB) in 86 volumes. `dcmod archive list` shows all of them. The main areas:

| Path | Files | What |
|---|---:|---|
| `vehicles/` | 274 | One resource pack per car: model, textures, materials. |
| `worlds/` | 200 | Shared world packs per country, including the large `*_topmips.rpk` packs that hold the highest texture detail. |
| `data/leveldata/` | 121 | One pack per track and direction, and `globaldata.rpk`. |
| `data/cameras/vehicle/` | about 1900 | Camera definitions per car (`chase_cameras`, `bonnet_cameras`, `bumper_cameras`, `dashboard_cameras`, `driver_head_cameras`, `look_back_cameras`), plain text. |
| `data/audio/vehicles/` | 234 | Vehicle audio data. `audio/fmodstudio` holds the FMOD banks. |
| `data/nexus/` | about 500 | Animation, skeleton and state machine data for drivers, riders, wipers and crowds. |
| `data/timetrial/` | 128 | Time trial data. |
| `newdata/` | 3 + `rank/` | `vehicles.csv` (137 cars) and `tracks.csv` as text tables. |
| `newui/` | about 650 | The menu system: `panels` and `panels/templates` (layout text), `control`, `pages`, and `art` (picture packs). |
| `liveryeditor/` | about 2300 | Stickers, designs, club symbols, masks, paints and templates of the livery editor. |
| `gui/lams/` | 22 | Text of every language, one CSV per language. |
| `scripts/` | 109 | Boot scripts, the Lua libraries (`scripts/lua`) and the automatic test scripts; see [Lua scripts](lua.md). |
| `ghosts/` | 295 | Ghost data. |

Top level: `global.rpk` (settings, shaders, shared textures), `vehicleglobal.rpk`, `vehicledefs.vdef`, and the `override-N.tss` settings files.

## Good first targets

- `newdata/vehicles.csv`: stats, unlock levels, the asset name that links a row to `vehicles/<asset>.rpk`, and the camera and audio resources.
- `gui/lams/<language>.csv`: every text in the game.
- `global.rpk`: the XML resources `viewportquality.xml` (graphics quality, shadows, level of detail) and `vehiclelodgroupdata.xml`. `dcmod rpk list @global.rpk type:xml -l` lists them.
- Camera files: for example `data/cameras/vehicle/mercedes_a45_amg.txt` pulls shared definitions in with `#include`.
- The Lua libraries for rewards, drift, game configuration and the HUD.

The paths the game really opens can be seen in the log (`Runtime: open(/app0/...)`).
