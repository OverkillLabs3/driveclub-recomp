# Getting started

## Get the kit

In the download, `dcmod.exe` is next to `DriveClub.exe` and the `modkit` folder holds these pages, the examples (`modkit/examples`), the mod schema and the Blender add-on. Open a terminal in the game folder and run `dcmod` without arguments for the list of commands.

To build it yourself (for example from a checkout, or on Linux) the kit is one C++17 program with no dependencies. On Windows use the MSYS2 CLANG64 shell described in [BUILDING.md](../../BUILDING.md); on Linux any C++17 compiler works.

```bash
bash tools/modkit/build.sh        # writes out/dcmod.exe (out/dcmod on Linux)
bash tools/modkit/test.sh         # self-test on made-up data
bash tools/modkit/tests/realdata.sh   # round trips on your own game files (optional)
```

## Tell it where the game is

The kit reads the game's files from your own copy. Give the update folder and the base game folder (the update first) in one of three ways:

```bash
dcmod archive info --update "D:\Game\Update" --base "D:\Game\Base"
# or set DRIVECLUB_UPDATE_DIR and DRIVECLUB_BASE_DIR
# or run it next to launcher.ini, which has update= and base= lines
```

`dcmod archive info` should report 8018 entries and no missing volumes.

## Look around

```bash
dcmod archive list -l "newdata/*"                 # files with sizes
dcmod archive extract newdata/vehicles.csv -o work
dcmod rpk list @global.rpk type:xml -l            # resources inside a pack in the archive
dcmod lua dis @scripts/bootscripts/defaultbootscript.lua.bin
```

## A first mod

```bash
dcmod mod new my-mod --name "My mod"
```

This creates `my-mod/mod.json` with one example patch that changes the top speed of a car in the vehicle table. Edit it. The vehicle names are the `Vehicle Name` column of `newdata/vehicles.csv`; list them with:

```bash
dcmod archive cat newdata/vehicles.csv
```

Check the mod against the game files without writing anything:

```bash
dcmod mod check my-mod
```

Build it. The output folder holds only the files the mod changed:

```bash
dcmod mod build my-mod -o built
```

For a quick edit loop, `dcmod mod watch my-mod -o "<game folder>\mods"` builds into the game's mods folder and builds again every time a file of the mod changes; the game looks for a mod's file each time it opens the game file of that path, so a change shows up the next time the game opens that file, without a restart. Checked: a changed `vehicles/ferrari_458_italia.rpk` copied into the mods folder 12 seconds after the game started (at the menu, before the race loaded) was the car that raced. In practice: car, track and texture packs are opened when a race loads, so leave the game running at the menu, rebuild, and start the race again. Files the game reads once at start (the tables and Lua scripts, the sound banks that are loaded at start) need a restart; which sound banks reload was not checked. There is no overlay that shows what a mod changed; the game's log (`logs/driveclub_*.log`) lists the first files it took from the mods folder (`Runtime: replaced by mods folder: ...`), and plugins (`plugins.md`) can read and change memory while it runs.

Copy the contents of `built` into the game's `mods` folder (next to `DriveClub.exe`). The log (`logs\driveclub_*.log`) then shows `Runtime: replaced by mods folder: ...` for each file the game opened. Several mods are built in one go, in the order given, with `dcmod mod build first-mod second-mod -o built`; later mods win where they change the same thing, and the kit prints a `conflict:` line for each such case.
