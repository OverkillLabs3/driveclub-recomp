# File formats

Everything here is read and written by the kit, and checked on the full game data: all 8018 archive names match their hashes, 3995 of the 4007 resource packs (all those under 600 MB) read and write back to identical bytes, and all 102 compiled Lua scripts do too.

## Archive (`game.ndx`, `gameNNN.dat`)

`game.ndx` starts with `DATX` and version 4300. It holds a filetime, two hash seeds, the compression id (7, LZ4), the chunk size (65536), a dictionary-packed table of file names, and one 32 byte record per file: volume and offset (`loc & 0xFFFF`, `loc >> 16`), size, name hash and MD5. The name hash is computed with 32 bit wrapping arithmetic from the upper-cased name with `\` as `/`.

Volumes start with `DATF`, then a `CHNK` table with the stored size of each chunk. A chunk of the full size is stored as it is, size 0 means zeros, anything else is a 4 byte size followed by an LZ4 block. A file's offset is a position in the volume's uncompressed data. The format was first described by the DriveClubFS project (MIT licence); the kit is an independent implementation.

## Resource pack (`.rpk`)

```
"EVOSLITL"  u32 18  "Resource PacK file"  u32 tocSize  u32 tocOffset(38)
table of contents (at 38):
  "EVOSLITL" u64 id u32 version, u32 0x11111111
  u32 resources, u32 total names, u32 total dependencies, u32 name pool size, u32 source pool size
  (version >= 0x40002) u32 alias count, u32 required pack count
  u64 root
  name pool, source path pool            NUL terminated strings
  per resource: u64 id, u32 size, u32 offset,
                u16 n + n * u64 dependencies, u16 n + n * u32 name offsets, u16 n + n * u32 source offsets
  aliases: u64 id + u32 name offset      required packs: u32 name offset
  u32 0x99999999
payloads, one after the other to the end of the file
```

A resource id has its type in the top 16 bits (5 pixel buffer, 38 binary, 39 XML, 43 level data, 71 top mips and so on; `dcmod rpk info` names them). The table is grouped by type while the payloads sit in another order; the kit keeps that order. Payloads start with `EVOSLITL`, the id and a version. XML and binary payloads continue with a u32 length and the content. Pixel buffers hold PS4 tiled texture data and the largest mip levels may be in a `top_mips` resource of another pack.

## Tables and text

`newdata/*.csv` are quoted CSV in Latin-1. `gui/lams/*.csv` are language files with a header of `#` comment lines, and values quoted with the control characters 2 and 3. The patch operations keep each file's line breaks and encoding.

## XML

Plain text XML with CRLF line breaks. Some files are not well formed; the game accepts them and so does the kit's editor.

## Lua

Scripts are compiled with the standard Lua 5.1 compiler for 64 bit machines (`ESC Lua`, version 0x51, 8 byte sizes, 4 byte instructions) and keep their line numbers and names. The kit reads and writes this format. The game also loads Lua source text.
