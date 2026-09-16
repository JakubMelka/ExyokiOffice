# exyokioffice-mcp

Three [Model Context Protocol](https://modelcontextprotocol.io) servers that
let an AI agent create, read and edit Word, Excel and PowerPoint documents
without Microsoft Office installed. Each server confines every path to one
folder of your choosing, validates every tool call against a published JSON
schema, and offers no way to run code.

The servers are native programs written in C++ on top of
[ExyokiOffice](https://github.com/JakubMelka/ExyokiOffice). This package is
the launcher: the wheel carries the executables for one platform, and the
commands below start them.

<!-- mcp-name: io.github.JakubMelka/exyoki-mcp-word -->
<!-- mcp-name: io.github.JakubMelka/exyoki-mcp-excel -->
<!-- mcp-name: io.github.JakubMelka/exyoki-mcp-power-point -->

## Use it with uvx

Nothing has to be installed. Register one server per document family with
`uvx` as the command; the first argument picks the family and `--workspace`
names the only folder the server may touch:

```jsonc
{
  "mcpServers": {
    "word": {
      "command": "uvx",
      "args": ["exyokioffice-mcp", "word", "--workspace", "C:/Users/me/Documents/agentwork"]
    },
    "excel": {
      "command": "uvx",
      "args": ["exyokioffice-mcp", "excel", "--workspace", "C:/Users/me/Documents/agentwork"]
    },
    "powerpoint": {
      "command": "uvx",
      "args": ["exyokioffice-mcp", "powerpoint", "--workspace", "C:/Users/me/Documents/agentwork"]
    }
  }
}
```

This is `.mcp.json` in a Claude Code project root. Claude Desktop uses the
same object in `claude_desktop_config.json`, VS Code `.vscode/mcp.json` with a
`servers` member, and Cursor `.cursor/mcp.json`.

## Use it installed

```bash
pip install exyokioffice-mcp
exyoki-mcp-word --workspace ./documents
exyoki-mcp-excel --workspace ./documents
exyoki-mcp-power-point --workspace ./documents
exyoki validate report.docx
```

`exyoki` is the command-line tool of the same library: it validates, inspects,
converts and diffs the documents the servers write.

## Options

Every server takes the same options; `--help` lists them. The ones that matter
when registering a server:

| Option | Meaning |
| --- | --- |
| `--workspace DIR` | The folder the server may read and write. Defaults to the working directory. |
| `--read-only` | Register only the tools that never modify a document or the file system. |
| `--print-tools` | Print the tool catalog as JSON and exit. |

## Platforms

Wheels are published for Windows x64 and Linux x64 (glibc 2.39 or newer). On
another platform `pip` reports that no matching distribution is found; the
[container image](https://github.com/JakubMelka/ExyokiOffice/blob/master/docs/tools/docker.md)
and the source build cover the rest.

## Documentation

- [MCP servers guide](https://github.com/JakubMelka/ExyokiOffice/blob/master/docs/tools/mcp-servers.md):
  registration, the workspace sandbox, worked sessions, the tool catalog and
  troubleshooting.
- [Packages](https://github.com/JakubMelka/ExyokiOffice/blob/master/docs/tools/mcp-packages.md):
  this wheel, the MCP bundles for Claude Desktop, the container image and the
  MCP Registry entries.

## License

MIT. The wheel carries the license of ExyokiOffice and the notices of the
third-party components compiled into the binaries under
`exyokioffice_mcp/licenses`.
