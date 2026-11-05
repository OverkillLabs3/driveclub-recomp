# dcmod reference

Every command prints its own help when run without arguments. The game folders come from `--update DIR --base DIR`, `DRIVECLUB_UPDATE_DIR` and `DRIVECLUB_BASE_DIR`, or `launcher.ini`.

## dcmod archive

The game's `game.ndx` and `gameNNN.dat` files.

| Command | Does |
|---|---|
| `info` | Entry count, chunk size, volumes used and missing. |
| `list [PATTERN...] [-l]` | File paths; `*` and `?` wildcards; `-l` adds size and volume. |
| `extract PATTERN... -o DIR [--verify]` | Writes files under `DIR` with their game paths. `--verify` checks the MD5 of the index. |
| `cat PATH` | One file to standard output. |
| `verify [--md5] [PATTERN...]` | Checks that every name matches its hash; with `--md5` reads and checks every file. |
| `grep TEXT [PATTERN...] [--limit N]` | Lines of the game's text files that contain TEXT, any case. |
| `names -o FILE` | Writes the list of all paths. |

## dcmod rpk

Resource packs. `PACK` is a file or `@path` for a pack inside the archive.

| Command | Does |
|---|---|
| `info PACK` | Table version, root resource, resources and bytes per type, required packs. |
| `list PACK [PATTERN...] [-l]` | Resources. Patterns match names (wildcards), or `type:xml`, or `id:HEX`. |
| `extract PACK -o DIR [PATTERN...]` | One file per resource and a `pack.json` that describes the pack. XML and binary resources are written as their plain content (`.xml`, `.bin`), others as whole payloads (`.res`). |
| `build DIR -o PACK` | Puts an extracted folder back together. A pack extracted completely and built again is byte for byte the original. Edit the files, or add entries to `pack.json`, before building. |
| `replace PACK TARGET FILE -o OUT` | Changes one resource. |
| `verify PACK... [--max-mb N]` | Reads each pack and writes it again; the bytes must match. `"@*.rpk"` checks every pack in the archive. |

## dcmod lua

Compiled Lua 5.1 scripts. `SCRIPT` is a file, a folder, or `@pattern` in the archive.

| Command | Does |
|---|---|
| `info SCRIPT...` | Size, functions, instructions. |
| `dis SCRIPT [-o FILE] [--no-lines]` | Assembly listing with constants, upvalue and local names where the script has them, and line numbers. |
| `survey SCRIPT... [-o FILE] [--min N]` | Markdown list of the globals, calls and table keys the scripts use. |
| `strings SCRIPT...` | Every string constant. |
| `verify SCRIPT...` | Read and write again; the bytes must match. |

## dcmod tex

Textures of packs as DDS. `info PACK`, `extract PACK -o DIR [PATTERN...]`, `import PACK TARGET FILE.dds -o OUT`, `verify PACK... [--max-mb N]`. See [studio.md](studio.md).

## dcmod studio

`dcmod studio [--mods DIR] [--port N]` starts the local web page described in [studio.md](studio.md).

## dcmod mod

| Command | Does |
|---|---|
| `new DIR [--id ID] [--name NAME]` | A mod folder with an example. |
| `diff EDITED -o MODFOLDER [--id ID] [--name NAME]` | Makes a mod from files you edited, and checks that building it gives those files. |
| `check DIR...` | Reads the mods, orders them, tries all patches on the game files, writes nothing. |
| `build DIR... -o OUT` | Applies the mods in the order given (after requirement and order rules) and writes the changed files and `dcmod-report.json` (which mod did what to which file). |

Exit status is 1 when a requirement is missing or mods conflict by declaration; value conflicts and warnings are printed and do not change it.
