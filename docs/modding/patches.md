# Patch operations

A patch is an object in the `patches` list of `mod.json`. Every patch has `"op"`. Most have `"file"`, the game path of the file to change (any case, `/` or `\`). `"comment"` is ignored. `"optional": true` turns a failure of that patch into a warning. A field that an operation does not know is an error, so typos are found.

Text in patches is UTF-8. Line breaks are matched whatever the file uses (`\n` in the patch matches a CRLF file) and the file's own line breaks and Latin-1 encoding are kept.

## Whole files

| Operation | Fields |
|---|---|
| `file.set` | `file`, and `from` (a file in the mod) or `text`. Creates or replaces the file. |
| `file.copy` | `from` (a game path, or a file an earlier patch made), `file`. |

## Text

`text.replace`: `file`, `find` (or `regex`, ECMAScript), `replace`. The text must occur exactly once; otherwise give `"occurrence": N` (from 1) or `"all": true`. A regular expression may use `$1` in `replace`.

`text.insert`: `file`, `after` or `before` (a text, or a regular expression with `"regex": true`), `text`, plus `occurrence` or `all`.

`text.append`, `text.prepend`: `file`, and `text` or `from`.

```json
{ "op": "text.replace", "file": "gui/lams/english.csv", "find": "Solo Time Trial Challenge", "replace": "Solo Time Attack" }
```

## Tables (CSV)

The table's first row names the columns. Rows are found by the value of a key column (`"key"`, default the first column). Only the cells that change are rewritten; quotes, order and line breaks of everything else stay.

`csv.set`: `file`, `key`, `match` (a value, a list of values, or `"*"`), optional `where` (more column conditions), `set` (column: value). Values may be text, numbers or true/false. Fails if no row matches or a column is unknown.

`csv.add`: `file`, `key`, `row` (column: value), optional `like` (key value of a row to copy the other cells from), `after` (key value to insert after; default the end), `replace` (replace an existing row with the same key).

`csv.remove`: `file`, `key`, `match`, optional `where`.

```json
{ "op": "csv.set", "file": "newdata/vehicles.csv", "key": "Vehicle Name",
  "match": "a45_amg", "set": { "TopSpeed": 320, "HorsePower_BHP": 500 } }
```

## XML

`path` selects elements: names separated by `/` starting at the root element, `*` for any name, `[@attr='value']` or `[@attr]` to filter, `[n]` for the nth match under each parent, and a leading `//` to search the whole document. A path that matches several elements needs `"all": true`. The text of the file outside the edited spot is untouched, comments included. Files that are not quite well formed (the game's own `viewportquality.xml` is missing a closing tag) are accepted.

`xml.set`: `file`, `path`, and exactly one of `attr` + `value` (sets or adds the attribute), `text` (the element's text), `remove_attr`. `"raw": true` writes the value without escaping `& < "`.

`xml.remove`: `file`, `path`.

`xml.insert`: `file`, `path`, `xml` (a fragment), `where`: `append` (default), `prepend`, `before` or `after`.

```json
{ "op": "xml.set", "file": "scripts/adverts.xml", "path": "Adverts/Advert[1]", "attr": "width", "value": "256" }
```

## JSON

`json.merge`: `file`, `value`. A merge patch (RFC 7386): objects merge, `null` deletes a key, anything else replaces. The file is written with two-space indentation.

## Bytes

`bytes.patch`: `file`, `offset` (number, or text such as `"0x1f0"`), `write` (hex), optional `expect` (hex that must be there now; the patch fails if not, which protects against other game versions).

`bytes.replace`: `file`, `find` (hex), `replace` (hex, any length), plus `occurrence` or `all`.

## Compiled Lua

`lua.const`: `file`, `find`, `replace` (both a number, text, true/false or null, of the same kind), optional `function` (such as `"main/3"`, the fourth function inside the main chunk; default the whole script), `expect` (how many constants must change; default 1, 0 for any number). It changes the constant tables of the compiled script, so numbers and strings in game logic can be changed without Lua source. See [Lua scripts](lua.md).

## Resource packs

For these, `file` is a pack such as `global.rpk`, and `resource` is a name from `dcmod rpk list`, or a 16 digit hex identifier. A name that is a path also matches by its last part.

`rpk.set`: `file`, `resource`, `from` or `text`. Replaces a resource. For XML and binary resources the content is the XML text or the bytes; for other types it is the whole payload as `dcmod rpk extract` writes it.

`rpk.patch`: `file`, `resource`, `ops`. Applies content operations (`text.*`, `csv.*`, `xml.*`, `json.merge`, `bytes.*`, `lua.const`, without `file`) to an XML or binary resource.

`rpk.add`: `file`, `type` (`xml`, `bin`, or another name from `dcmod rpk info`, or `type_N`), `id` (16 hex digits; the first four are the type number, `0027` for xml), `name`, `from` or `text`, optional `sources`, `deps`, `version`.

`tex.set`: `file`, `resource`, `from` (a DDS file). Replaces a texture; see [studio.md](studio.md) for the DDS requirements.

`rpk.remove`: `file`, `resource`. Dependencies on it are dropped from the other resources of the pack.

```json
{ "op": "rpk.patch", "file": "global.rpk", "resource": "viewportquality.xml",
  "ops": [ { "op": "xml.set", "path": "ViewportQuality/Configuration[@name='default']/General",
             "attr": "lensdust", "value": "false" } ] }
```

A pack that is changed is written again with its resources in their original order, so a pack that mods leave alone is never touched.

## Other game files

`vdef.clone`: `file` (`vehicledefs.vdef`), `from` (a record name), `name` (the new record's name). Adds a copy of a vehicle definition.

`vdef.set`: `file`, `name` (record), `offset` (from the start of the record), `value`, optional `type` (`float`, default, or `u32`) and `expect` (the value that must be there now). See [cars.md](cars.md).

`manifest.add`: `file` (`scripts/filemanifest.cfg`), `paths` (game paths to list). The game's list of its own files; new files that the game should find by itself have to be in it.

`rpk.rename`: `file` (a pack), `find`, `replace`, optional `scope` (`required`, default, `names`, `sources` or `all`). Changes text in the names a pack lists, for example the packs it requires.

`rpk.copy`: `file` (a pack), `resource` (an existing resource), `name` (the copy's name). Adds a copy of the resource under a new name and a new identifier.

`archive.register`: `paths` (files that mods add). Lists the files in the game's own archive index by writing a new `game.ndx` and an extra volume. When the paths are files the archive already has, the index is kept as it is, only those records point to a new volume (the game starts and runs with such an index). Paths the archive does not have yet are added to the index with their names; the game does not start properly with that yet, so no shipped command uses it. A loose file in the mods folder already replaces an archive file, so this is only needed to test how the game reads the archive.
