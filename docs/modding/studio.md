# Mod studio and the easy way to make mods

You do not have to write patches by hand.

## dcmod studio: a web page for modding

```bash
dcmod studio                 # prints an address like http://127.0.0.1:51234/?t=...
dcmod studio --mods my-mods  # where your mods are kept (default: dcmod-mods in the current folder)
```

Open the address in a browser. It only listens on your own computer and every request needs the random token in the address.

1. **Game files**: search all 8018 files, open one. Text and tables open in an editor; packs list their resources (XML and text resources can be edited); compiled Lua scripts show as an assembly listing.
2. Change the text, pick a mod, press **Save to mod**. The studio compares your text with the game's original and stores only the difference as patches in that mod. Saving the same file again replaces the earlier patches for it.
3. **Mods** tab: create a mod, edit its `mod.json`, tick mods and press **Check** or **Build**. Build writes the changed files to `_built` in the mods folder. The mod folders can also be imported on the launcher's Mods page.

## dcmod mod diff: from edited files to a mod

Edit the files in your favourite editor, then let the kit write the patches:

```bash
dcmod archive extract newdata/vehicles.csv gui/lams/english.csv -o edited
# edit the files under edited/
dcmod mod diff edited -o my-mod --id me.my-mod --name "My mod"
```

Tables become `csv.set`/`csv.add`/`csv.remove` patches, text becomes `text.replace` patches with enough surrounding lines to be unambiguous, and files that are not text are included whole. For a pack, extract it into a folder named like the pack (`dcmod rpk extract @global.rpk -o edited/global.rpk`), edit the resource files and run the same command; each changed resource becomes an `rpk.set`. The kit then builds the mod again on the game's files and fails if the result is not exactly your edited files.

## Finding things

```bash
dcmod archive grep topspeed "newdata/*"     # lines of text files containing a word
dcmod archive list -l "*mercedes*"
dcmod rpk list @global.rpk type:xml -l
```

## Textures

```bash
dcmod tex info @helmets.rpk
dcmod tex extract @helmets.rpk -o textures "helmetlivery_16*"    # DDS files
dcmod tex import @helmets.rpk helmetlivery_16.tif edited.dds -o helmets-new.rpk

# a texture whose largest levels are in a top mips pack (most car textures)
dcmod tex extract @vehicles/ferrari_458_italia.rpk -o textures "ferrari_458_logo_col*" --top @vehicles/ferrari_458_italia_top_mips.rpk
dcmod tex import @vehicles/ferrari_458_italia.rpk ferrari_458_logo_col.tif edited.dds -o car.rpk --top @vehicles/ferrari_458_italia_top_mips.rpk --top-out top.rpk
```

A PNG picture works as well as a DDS for textures stored as BC1 (DXT1), BC7 or plain 8 bit RGBA: the kit makes the mip levels and compresses them itself. The picture must have the texture's size (for example 512x512 for the helmet liveries), 8 or 16 bit, not interlaced. Other formats need a DDS.

In a mod, `{ "op": "tex.set", "file": "helmets.rpk", "resource": "helmetlivery_16.tif", "from": "textures/mine.png" }` replaces a texture. The DDS needs the texture's size and block format and all its mip levels; texconv, Compressonator, GIMP and Paint.NET write such files (BC1 for DXT1, BC7 for BC7). About 8000 of the textures in packs under 100 MB convert and tile back byte for byte. Textures whose largest levels are stored in the separate `*_topmips.rpk` packs (most car textures), volume and cube textures, and a few BC7 textures with an unexplained size (63) are reported and left alone.

Checked in the game: the Ferrari 458's `ferrari_458_license_plate.tif` (512x128, BC1) replaced with a solid magenta picture shows as a magenta number plate on the car in a race.

## Editor help

`tools/modkit/mod.schema.json` is a JSON Schema for `mod.json`. In Visual Studio Code, add `"$schema": "path/to/mod.schema.json"` to the file and you get completion and error checks while typing.
