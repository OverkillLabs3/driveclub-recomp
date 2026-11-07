# Modding DriveClub Recomp

The mod kit has three parts:

- **Mods as folders.** A mod is a folder with a `mod.json`. It can carry replacement files and a list of *patches*, which are small changes to the game's own files ("set this cell of the vehicle table", "change this attribute inside this pack's XML"). Patches are applied to the player's own copy of the game data, so a mod contains none of the game's data, and several mods can change the same file as long as they change different things.
- **`dcmod`, the command line kit.** It reads the game's archive, takes resource packs apart and puts them back together, reads the compiled Lua scripts, and builds mods.
- **The reference.** The pages listed below describe the formats and every patch operation.

| Page | Contents |
|---|---|
| [Getting started](getting-started.md) | Set up the kit and make a first mod |
| [Studio and the easy way](studio.md) | The web page for modding, making a mod from edited files, textures |
| [Working with an AI agent](agent.md) | `dcmod mcp`: let an agent browse the game files and make mods |
| [Mod format](mod-format.md) | `mod.json`, load order, requirements, conflicts |
| [Patch operations](patches.md) | Every operation with examples |
| [dcmod reference](dcmod.md) | All commands |
| [Game files](game-files.md) | What the archive holds and which files are worth changing |
| [File formats](formats.md) | Archive, resource packs, tables, XML, Lua |
| [Lua scripts](lua.md) | Mod scripts, logging, and the game's script layer: [the live API](lua-api.md) and [the names its scripts use](lua-names.md) |
| [Cars and dashboards](cars.md) | What a car is, what can be changed, the instrument displays |
| [Cameras](cameras.md) | The camera text files of every car, changing a view |
| [Models and Blender](models.md) | Export model geometry to glTF, edit, import back |
| [Music and sound](audio.md) | FMOD banks: hear, extract, replace and encode your own sounds and music |
| [Native plugins](plugins.md) | DLLs that hook the game's code (opt-in, for code you trust) |
| [Post-processing filters](postfx.md) | A shader of your own over the game's picture, switched and tuned by a script |
| [Car lights](lights.md) | Read and change the colour and brightness of a car's lights |
| [Tracks](tracks.md) | what a track is, what can be changed, a copy of a track under a new name |
| [Tutorial: a new car](tutorial-new-car.md) | clone a car, reshape it in Blender, share it as two mods |
| [Shaders and graphics](shaders.md) | dump the game's shaders, edit the SPIR-V, replace them in a mod |
| [Recipes](recipes.md) | Worked examples |

The older way still works: a folder or ZIP whose files sit at their game paths is a mod ([how files are found](../MODDING.md)).

## What can be changed today

| Area | How |
|---|---|
| Vehicle and track tables (`newdata/vehicles.csv`, `tracks.csv`), text and language files (`gui/lams`) | `csv.*`, `text.*` patches |
| Camera files, settings, XML data, UI layout text | `text.*`, `xml.*` patches |
| Graphics and level of detail settings inside `global.rpk` and other packs | `rpk.patch` with `xml.*` |
| Any resource inside a pack (add, replace, remove) | `rpk.*` patches |
| Compiled Lua scripts (rewards, game configuration, drift scoring, UI logic) | `lua.const` and `lua.alias` patches, or replace the script with Lua source |
| A car with another car's model, or a new row with its own physics | `dcmod car swap`, `dcmod car variant` ([cars.md](cars.md)) |
| Car and model geometry (positions, triangles, skin weights, materials of a part) | `dcmod mesh` and Blender ([models.md](models.md)) |
| A track under a new name | `dcmod track clone` ([tracks.md](tracks.md)) |
| A shader over the whole picture (grading, vignette, grain), scripted | `dcmod.postfx`, `postfx/<name>.spv` ([postfx.md](postfx.md)) |
| The time of day and the weather of a race, scripted | `dcmod.time`, `dcmod.set_time`, `dcmod.race_weather` ([lua.md](lua.md)) |
| A different chase, bonnet or dashboard camera | a `text.replace` patch on the camera file ([cameras.md](cameras.md)) |
| One change for a whole group of cars, tracks or files | `"each"` in a patch ([patches.md](patches.md)) |
| An app with buttons, switches and sliders over the game | `dcmod.gui.app` ([lua.md](lua.md)) |
| Keys and mouse for your own controls | `dcmod.input`, `dcmod.key` ([lua.md](lua.md)) |
| Live car data and your own HUD in the game | `dcmod.car()`, `dcmod.ui` ([lua.md](lua.md)) |
| The lights of a car (headlights, brake lights) | `dcmod car lights`, `light.set` patch ([lights.md](lights.md)) |
| Music and sound effects (FMOD banks), your own WAV files, which stream an event plays, loops | `dcmod audio import`, `audio set` ([audio.md](audio.md)) |
| Shaders (the look of surfaces, lighting, post effects) | `dcmod shader` and `shaders\` in a mod ([shaders.md](shaders.md)) |
| Anything else | replacement files, `bytes.*` patches |

## Limits

- Textures convert to and from DDS and PNG (`dcmod tex`, also those whose largest levels are in a separate top mips pack, with `--top` and `"top"`). Models convert to glTF and back with any number of vertices and triangles (`dcmod mesh`, with normals); a car's model pack can be given to another car (`dcmod car swap`) and its resources extracted and replaced as raw payloads (`dcmod rpk`). A car under a new name with its own model files loads and races (`dcmod car clone`, [cars.md](cars.md)), and so does a track under a new name (`dcmod track clone`, [tracks.md](tracks.md)). Not possible yet: a part that the car does not have (new nodes), the volume and pitch of a sound, an overlay that shows what a mod changed ([models.md](models.md), [audio.md](audio.md)).
- Compiled game scripts can be read as an assembly listing, not as Lua source. A replacement script has to be compiled (`dcmod lua compile`, or list it under `scripts` in `mod.json` and the build compiles it): the game cannot run Lua it compiles from text itself.
- Mods are not signed or checked. Only use mods from people you trust.
