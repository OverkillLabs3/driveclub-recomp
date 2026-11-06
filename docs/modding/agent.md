# Working with an AI agent (MCP)

`dcmod mcp` is a [Model Context Protocol](https://modelcontextprotocol.io) server on standard input and output. An agent that speaks MCP (Claude Code, Claude Desktop, Cursor and others) can look through the game's files, write and test patches, and build mods with the same commands a person uses.

## Set it up

Give the server your game folders (the same ones as `launcher.ini`) and a folder for your mods.

Claude Code:

```bash
claude mcp add dcmod -- dcmod.exe mcp --update "D:\Game\Update" --base "D:\Game\Base" --mods "D:\my-mods"
```

Any client that reads a JSON file (`.mcp.json`, `claude_desktop_config.json`, Cursor's `mcp.json`):

```json
{
  "mcpServers": {
    "dcmod": {
      "command": "D:\path\to\dcmod.exe",
      "args": ["mcp", "--update", "D:\Game\Update", "--base", "D:\Game\Base", "--mods", "D:\my-mods"]
    }
  }
}
```

Without `--update` and `--base` the server uses the environment variables `DRIVECLUB_UPDATE_DIR` and `DRIVECLUB_BASE_DIR`, or `launcher.ini` in its working folder.

## Tools

| Tool | Does |
|---|---|
| `game` | Runs a `dcmod` command: `command` is `archive`, `rpk`, `lua`, `tex`, `audio`, `mesh`, `dash`, `car` or `mod`, `args` are its arguments. For example `archive` with `["grep", "topspeed", "newdata/*"]` or `rpk` with `["list", "@global.rpk", "type:xml", "-l"]`. With no `args` a command prints its help. |
| `mod_list`, `mod_read`, `mod_write` | The mods in the workspace and their `mod.json`. |
| `mod_add_patch` | Adds one patch to a mod and checks that the mod still applies to the game's files; a patch that does not apply is not kept, and the message says why. |
| `mod_write_file` | Writes a file inside a mod folder (a script, or a file a patch reads with `from`). |
| `mod_check`, `mod_build` | Applies mods in order without writing, or writes the changed files to `_built` in the workspace. |

The agent can write only inside the workspace: output files (`-o`) elsewhere are refused, and a mod file path cannot leave its mod folder. The server does not start the game and does not change the game's files.

## A good first request

"Make a mod that raises the top speed of the A45 AMG to 333, show me which cell you changed, and build it." The agent finds the row with `archive grep`, adds a `csv.set` patch with `mod_add_patch`, and builds with `mod_build`. The patch reference is [patches.md](patches.md); an agent can read it from the repository.
