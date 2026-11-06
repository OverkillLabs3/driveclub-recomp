# Mod format

A mod is a folder:

```
my-mod/
  mod.json          the manifest (optional for a mod that is only files)
  files/            files that replace or add game files, at their game paths
  anything else     files that patches refer to with "from"
```

The loose files come from the folder named by `"files"` in `mod.json`; without that, from a folder called `files`, `mods` or `app0`; without any of those, and when the mod has no patches, from the mod folder itself (the old layout; `mod.json`, README, LICENSE, CHANGELOG and CREDITS files are skipped).

## mod.json

```json
{
  "id": "author.my-mod",
  "name": "My mod",
  "version": "1.2.0",
  "author": "Name",
  "description": "What it changes.",
  "game": "1.28",
  "requires": [ { "id": "author.library", "version": ">=1.0" }, "other.mod" ],
  "conflicts": [ "author.other-version" ],
  "after": [ "author.some-mod" ],
  "before": [ "author.another-mod" ],
  "patches": [ ]
}
```

| Field | Meaning |
|---|---|
| `id` | Unique name: `a-z`, `0-9`, `.`, `-`, `_`. Defaults to the folder name. |
| `name`, `version`, `author`, `description`, `game` | Shown to players. `version` is dotted numbers. |
| `requires` | Mods that must be installed. An entry is an id, or `{ "id", "version", "optional" }`. A version is `1.2`, `=1.2`, `>=1.2`, `>1.2`, `<=1.2` or `<1.2`. A required mod loads first. |
| `conflicts` | Mods that cannot be used together with this one. |
| `after`, `before` | Load order rules that do not need the other mod to be present. |
| `patches` | The [patch operations](patches.md), applied in order. |
| `files` | Folder with the loose files, relative to the mod. |
| `scripts` | Lua files (game paths) that run at start-up; see [lua.md](lua.md). |
| `settings` | Choices the player can change, see below. |

## Settings

A mod can offer settings. Each has a name and a definition:

```json
"settings": {
  "power":    { "label": "Top speed", "type": "number", "default": 300, "min": 100, "max": 500 },
  "greeting": { "label": "Greeting", "type": "string", "default": "hello" },
  "loud":     { "type": "bool", "default": false },
  "mode":     { "type": "choice", "default": "a", "choices": ["a", "b"] }
}
```

`type` is `number` (with optional `min` and `max`), `bool`, `string` or `choice` (one of `choices`); `default` is required. The player's choices are a JSON file `{ "mod.id": { "power": 410 } }` given to `dcmod mod check` or `dcmod mod build` with `--settings FILE`; a value of the wrong type, outside its range, or for a name the mod does not have is an error.

- In a patch, `{ "$setting": "power" }` in place of a value stands for the setting's value: `"set": { "TopSpeed": { "$setting": "power" } }`.
- In a Lua script, `dcmod.setting("mod.id", "power", fallback)` returns it; `dcmod.settings["mod.id"]` is the whole table.

The launcher does not show settings yet; until it does, a settings file is passed by hand.

## Load order and what wins

The player's list order is the starting point. The kit moves a mod only as far as `requires`, `after` and `before` demand, and fails if the rules go around in a circle.

For each mod in turn: its loose files are written first, then its patches are applied to the result of everything before it. So

- a later mod's file replaces an earlier mod's file at the same path;
- patches see the files as earlier mods left them, and a patch whose target no longer exists fails with a message instead of being skipped silently (`"optional": true` on a patch turns the failure into a warning);
- two mods that set the same table cell, XML attribute or resource to different values are reported as a `conflict:`; the later one wins.

## What a mod must not contain

Do not include the game's own files, dumps or saves. Patches exist so that a mod only carries what it changes.

## In the launcher

The **Mods** page imports a ZIP or folder. A mod with a non-empty `patches` list keeps its whole folder in the library; only the files in its `files` folder count as loose game files. When any enabled mod has patches, enabling, disabling or moving a mod makes the launcher run `dcmod.exe` (next to the launcher) on all enabled mods and put the result in `mods`. The mod at the top of the list wins, as with plain file mods. Problems (a missing requirement, a patch that no longer fits the game files) are shown as the reason enabling failed, and `data\mod-manager\last-build.txt` keeps the output of the last run, including `conflict:` lines.
