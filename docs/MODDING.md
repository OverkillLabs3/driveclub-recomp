# Making mods

A mod is a set of files that replace the game's own files. When the game opens a file by name, DriveClub Recomp looks in the `mods` folder first, and uses your file if it is there. The game's folders are never changed, and removing a mod puts the original back.

This page says what is known to work. What is only reported by players is marked.

## How it works

The game keeps its data in archives (`game.ndx` and `game000.dat`, `game001.dat` and so on) but asks for each file by a path such as `/app0/data/leveldata/canada_road_circuit_02_n.rpk`. The runtime turns that into a folder lookup: for a path `/app0/<path>` it looks for `<path>` in each mods folder, then in your update folder, then in your base game folder, and only then does the game read the file from its archive. The log shows the names the game asks for.

So the path of your file under `mods` is the path of the file inside the game's archive. A file at `mods\newdata\vehicles.csv` replaces `newdata/vehicles.csv`. The whole file is replaced, so your copy must keep the format the game expects.

Proven by the launcher itself, which uses the same route for its features: the vehicle, tour and livery lists ("Unlock DLC content"), `global.rpk` (scene detail and car level of detail), and the intro video ("Skip the intro video").

## Where mods go

`mods` is the folder next to `DriveClub.exe`. Put files there by hand (both launchers), or use the **Mods** page of the new launcher:

- **Import ZIP** or **Import folder** adds a mod to the library. An imported mod starts disabled.
- The checkbox turns a mod on or off. Turning it on copies its files into `mods`; turning it off removes them.
- **Move up** and **Move down** set the order. When two enabled mods have a file with the same path, the one higher in the list wins. Selecting a mod shows how many files it shares with other enabled mods.
- **Open mods folder** opens `mods`.

The manager keeps its library in `data\mod-manager`. It refuses to overwrite a file in `mods` that it did not put there, so move your hand-placed files out of the way first.

## What a mod looks like

A ZIP or a folder with the files at the paths they have in the game:

```
my-mod.zip
  mod.json                  (optional)
  data/cameras/vehicle/ferrari_458_italia.txt
  newdata/vehicles.csv
```

The files may also sit inside one folder called `files`, `mods` or `app0`. One enclosing folder around the whole mod is fine too. `mod.json` is optional and only names the mod in the list:

```json
{ "name": "My mod", "version": "1.0", "author": "Your name", "description": "What it changes." }
```

The importer requires at least one file, refuses two files with the same path, and refuses links inside a ZIP.

## What can be changed

All of these paths were checked against a version 1.28 archive (CUSA00093). "Text" means plain text you can edit in any editor. The log lists the first 300 files the game opens as `Runtime: open(/app0/...)`, which is a good way to see the names it asks for.

| Path | Format | What it holds |
|---|---|---|
| `newdata/vehicles.csv` | Text (CSV) | The vehicle list, 137 rows. Columns include `Vehicle Name`, `Class`, `Asset`, `Vehicle Def`, `Audio Resource`, `Acceleration`, `TopSpeed`, `HorsePower_BHP`, `Torque_Nm`, `Weight_Kg`, `PlayerUnlockLevel` and the unlock and store fields. |
| `data/databases/dc.tour` | Binary | The tours (about 420 KB). |
| `liveryeditor/assets/assets.txt` | Text | The livery editor's asset list. |
| `data/cameras/vehicle/<asset>.txt` | Text | The cameras of a car, which `#include` shared files such as `include/common_cameras.txt`. |
| `data/cameras/vehicle/driver_head_cameras/<asset>.txt` and `.../include/driver_head_camera_params.txt` | Text | The driver's head camera: rotation offset, focal length, look-at limits and neck movement limits, with `*` comments. |
| `data/leveldata/<track>.rpk` | Resource pack | One pack per track and direction, for example `canada_road_circuit_02_n.rpk` (7.6 MB) and `chile_race_track_v1.rpk`. The launcher writes `data\user\scene_tracks.tsv`, which lists every track name next to its display name. |
| `data/leveldata/globaldata.rpk` | Resource pack | Data shared by all tracks (10 MB). |
| `vehicles/<asset>.rpk` | Resource pack | A car's model and textures, for example `vehicles/mercedes_a45_amg.rpk` (21 MB). `<asset>` is the `Asset` column of `vehicles.csv`. |
| `vehicledefs.vdef` | Binary (`EVOSLITLVDEF`) | The vehicle definitions, named by the `Vehicle Def` column. |
| `vehicleglobal.rpk`, `global.rpk` | Resource pack | Textures and shared data for cars and for the game; `global.rpk` also holds the graphics quality and car level of detail settings as XML text. |
| `newui/art/*.rpk` | Resource pack | Menu art such as `vehicles.rpk` (96 MB) and the `tour_tiles*.rpk` pictures. |
| `newui/art/dc_attract_video_30fps_11.v3_256.mp4` | Video | The intro video. |
| `override-0.tss` to `override-6.tss` | Text | Settings the game would load from its server, one `key value` per line after a `<< Config >>` line. See [online play](online/README.md). With Online play on, the launcher writes `override-0.tss` itself at every start, so do not rely on your own copy then. |

Any file the game opens by name can be replaced the same way; the table lists the ones that exist in the archive and are known to be opened.

## Resource packs

`.rpk` files are the game's resource packs. They start with `EVOSLITL` and the text `Resource PacK file`. A pack lists every file it holds with its size, so you cannot change a file inside a pack to another length with a text editor. Two ways around that:

- Replace the whole pack with a pack you built.
- Edit text inside a pack without changing its length: pad with spaces and remove comments. This is how the launcher edits the graphics settings in `global.rpk`.

This project does not include tools to unpack or build packs. Tools published by others (not part of this project; check their licences and limits):

- [DriveClubFS](https://github.com/Nenkai/DriveClubFS) by Nenkai unpacks the `.ndx` and `.dat` archives and extracts textures, binary resources and XML from `.rpk` files. Its README lists versions 1.00 and 1.28 as tested. It is an unpacker and has no function to rebuild an archive or a pack.
- [010GameTemplates](https://github.com/Nenkai/010GameTemplates/tree/main/Evolution%20Studios/DriveClub) has 010 Editor templates for the archive, index, tour and save formats.
- [io_import_driveclub](https://github.com/oxmaulmike2581/io_import_driveclub) imports car packs into Blender 4.1 or newer (partial material support, no skeletal animation).
- The ResHax thread on [DriveClub models](https://reshax.com/topic/719-driveclub-ps4/) collects notes on the mesh, vertex buffer and texture layout of the packs.

## Custom tracks

A track is the pack `data/leveldata/<track>.rpk`, and the game asks for it by that path, so a pack placed in `mods` at the same path is used instead of the original. Players have reported putting their own tracks in this way and getting them to load. How to build a track pack is not publicly documented, so this page does not cover it.

## Which file wins

For each file the game opens, the first of these that has it is used:

1. Your `mods` folder.
2. The files the launcher builds itself (unlock, scene detail, car switching, skip intro).
3. Your update folder, then your base game folder.
4. The game's archive.

## Checking that it works

Start the game and open the newest `logs\driveclub_*.log`. For the first files a mod replaces, there are lines like:

```
Runtime: replaced by mods folder: /app0/newdata/vehicles.csv
```

If your file is not listed, the path is wrong (check the folder names and the slashes) or the game did not open that file in that session. If the game does not start or behaves wrongly, disable the mod or move its files out of `mods`; that always gives the original game back.

## Sharing a mod

Share only files you made or changed. Do not upload the game's original files, game dumps or saves. Say which game version the mod was made for (the version 1.28 update). The mod manager loads local files only, it is not a download site, and mods are not checked or signed, so only use mods from people you trust.
