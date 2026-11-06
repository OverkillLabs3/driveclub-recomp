# Modding DriveClub Recomp

The mod kit has three parts:

- **Mods as folders.** A mod is a folder with a `mod.json`. It can carry replacement files and a list of *patches*, which are small changes to the game's own files ("set this cell of the vehicle table", "change this attribute inside this pack's XML"). Patches are applied to the player's own copy of the game data, so a mod contains none of the game's data, and several mods can change the same file as long as they change different things.
- **`dcmod`, the command line kit.** It reads the game's archive, takes resource packs apart and puts them back together, reads the compiled Lua scripts, and builds mods.
- **The reference.** The pages listed below describe the formats and every patch operation.

| Page | Contents |
|---|---|
| [Getting started](getting-started.md) | Set up the kit and make a first mod |
| [Studio and the easy way](studio.md) | The web page for modding, making a mod from edited files, textures |
| [Mod format](mod-format.md) | `mod.json`, load order, requirements, conflicts |
| [Patch operations](patches.md) | Every operation with examples |
| [dcmod reference](dcmod.md) | All commands |
| [Game files](game-files.md) | What the archive holds and which files are worth changing |
| [File formats](formats.md) | Archive, resource packs, tables, XML, Lua |
| [Lua scripts](lua.md) | Mod scripts, logging, and the game's script layer: [the live API](lua-api.md) and [the names its scripts use](lua-names.md) |
| [Cars and dashboards](cars.md) | What a car is, what can be changed, the instrument displays |
| [Recipes](recipes.md) | Worked examples |

The older way still works: a folder or ZIP whose files sit at their game paths is a mod ([how files are found](../MODDING.md)).

## What can be changed today

| Area | How |
|---|---|
| Vehicle and track tables (`newdata/vehicles.csv`, `tracks.csv`), text and language files (`gui/lams`) | `csv.*`, `text.*` patches |
| Camera files, settings, XML data, UI layout text | `text.*`, `xml.*` patches |
| Graphics and level of detail settings inside `global.rpk` and other packs | `rpk.patch` with `xml.*` |
| Any resource inside a pack (add, replace, remove) | `rpk.*` patches |
| Compiled Lua scripts (rewards, game configuration, drift scoring, UI logic) | `lua.const` patches, or replace the script with Lua source |
| A new car as a copy of an existing one | `file.copy`, `csv.add`, `rpk.patch` |
| Anything else | replacement files, `bytes.*` patches |

## Limits

- Textures convert to and from DDS (`dcmod tex`), except those whose largest levels are in a separate top mips pack. Models have no converter yet; their resources can be extracted and replaced as raw payloads (`dcmod rpk`).
- Scripts that are compiled can be read as an assembly listing, not as Lua source. Replacing a script with new Lua source works because the game loads plain Lua text.
- Mods are not signed or checked. Only use mods from people you trust.
