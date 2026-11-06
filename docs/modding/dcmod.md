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
| `grep TEXT [PATTERN...] [--limit N] [--binary]` | Lines of the game's text files that contain TEXT, any case. With `--binary`: the files (packs and compiled scripts too, up to 64 MB) that hold TEXT, with the number of times. |
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
| `compile SOURCE.lua [-o OUT]` | Compiles Lua source to the bytecode the game runs (the game cannot run chunks it compiles from text). |
| `verify SCRIPT...` | Read and write again; the bytes must match. |

## dcmod car

| Command | Does |
|---|---|
| `list` | Every car: Vehicle Name, Asset (the file name stem) and class. |
| `files NAME` | The table row of a car (NAME is its Vehicle Name) and every file named after its Asset. |
| `swap DONOR VICTIM -o MOD` | A mod that gives the car VICTIM the model and textures of DONOR. |
| `variant NAME NEWNAME -o MOD` | A mod that adds NEWNAME with its own physics record, sharing NAME's model. |
| `clone NAME NEWNAME -o MOD [--asset A]` | Copies all of a car's files under a new Asset and renames the names inside the copied packs. The copy loads and races, with its own model files. |

See [cars.md](cars.md).

## dcmod dash

`list`, `cat NAME`, `check FILE|NAME` and `binds`: the in-car instrument definitions. See [cars.md](cars.md).

## dcmod tex

Textures of packs as DDS. `info PACK`, `extract PACK -o DIR [PATTERN...]`, `import PACK TARGET FILE.dds -o OUT`, `verify PACK... [--max-mb N]`. See [studio.md](studio.md).

## dcmod audio

FMOD banks. `info BANK`, `extract BANK -o DIR [NAME]`, `wav BANK NAME -o FILE.wav` (decode), `import BANK NAME FILE.wav -o OUT` (encode to ATRAC9 and rebuild the bank), `replace BANK NAME FILE -o OUT`. See [audio.md](audio.md).

## dcmod mesh

Model geometry as glTF. `info PACK`, `export PACK -o FILE.glb [NAME...]`, `import PACK FILE.glb -o OUT.rpk`. See [models.md](models.md).

## dcmod shader

Shader replacement. `list DUMPDIR [STAGE]`, `dis FILE.spv [-o OUT]`, `as FILE.spvasm -o OUT.spv`, `install FILE.spv MODFOLDER` (the last two and `dis` need `spirv-dis` and `spirv-as` on PATH). See [shaders.md](shaders.md).

## dcmod track

`list` shows every track (Track Name and the name in menus); `files NAME` lists a track's table row and the files named after it. See [tracks.md](tracks.md).

## dcmod mcp

`dcmod mcp [--mods DIR] [--update DIR --base DIR]` starts an MCP server for AI agents; see [agent.md](agent.md).

## dcmod studio

`dcmod studio [--mods DIR] [--port N]` starts the local web page described in [studio.md](studio.md).

## dcmod mod

| Command | Does |
|---|---|
| `new DIR [--id ID] [--name NAME]` | A mod folder with an example. |
| `pack DIR -o FILE.zip` | Writes the mod folder as a ZIP to share (files stored, nothing from the game added). |
| `diff EDITED -o MODFOLDER [--id ID] [--name NAME]` | Makes a mod from files you edited, and checks that building it gives those files. |
| `check DIR... [--settings FILE]` | Reads the mods, orders them, tries all patches on the game files, writes nothing. |
| `build DIR... -o OUT` | Applies the mods in the order given (after requirement and order rules; `--settings FILE` gives the player's choices for mod settings, see [mod-format.md](mod-format.md)) and writes the changed files and `dcmod-report.json` (which mod did what to which file). |

Exit status is 1 when a requirement is missing or mods conflict by declaration; value conflicts and warnings are printed and do not change it.
