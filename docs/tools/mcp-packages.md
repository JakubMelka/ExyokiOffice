# Packages for the MCP servers

The three MCP servers are native programs, and [MCP servers](mcp-servers.md)
registers them from an extracted release archive. This chapter covers the
other ways to obtain them, each aimed at a different host:

| Form | For | Install | Built by |
| --- | --- | --- | --- |
| [`exyokioffice-mcp` on PyPI](#the-python-package) | Hosts configured with a `command`, where `uvx` or `pip` is already the habit | Nothing: `uvx exyokioffice-mcp word` fetches it | `create_install`, one wheel per platform |
| [MCP bundles](#the-mcp-bundles) (`.mcpb`) | Claude Desktop | Open the file; Claude Desktop asks for the document folder | `create_install`, one bundle per server and platform |
| [The container image](docker.md) | Anything with Docker, and the only form with a second boundary around the workspace | `docker pull` | `create_install`, pushed by `publish_docker` |
| [MCP Registry entries](#the-mcp-registry-entries) | Hosts that browse the registry | The host installs one of the forms above | `create_install`, published with `mcp-publisher` |

All four carry the same binaries from the same workflow run, so a server
answers the same way whichever form started it, and the
[tool catalog](mcp-servers.md#tool-catalog) is the one catalog.
`packaging/package.py` produces the first, second and fourth from the same
install tree the release archive is zipped from; [Building the packages
yourself](#building-the-packages-yourself) shows how.

## The Python package

`exyokioffice-mcp` is a launcher: the wheel carries `exyoki`, the three servers
and the shared library for one platform under `exyokioffice_mcp/bin`, and its
console scripts start them. It imports nothing at run time and the servers
contain no Python; on Linux the launcher replaces itself with the server, on
Windows the server runs as a child that inherits the host's pipes.

With `uvx`, nothing is installed and the registration is one entry per family:

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

`exyokioffice-mcp` is the command `uvx` runs when given the package name. Its
first argument picks the program — `word`, `excel`, `powerpoint` or `exyoki` —
the same words the container image's dispatcher takes, and everything after it
goes to that program unchanged. Pin a release with `exyokioffice-mcp==1.1.0`
in place of the bare name.

Installed with `pip`, the programs are also on `PATH` under their own names:

```bash
pip install exyokioffice-mcp
exyoki-mcp-word --workspace ./documents
exyoki-mcp-power-point --print-tools
exyoki validate report.docx
```

`exyoki` is the [command-line tool](exyoki.md) of the same library, so a
document a server wrote can be validated or converted without a second
installation. An environment that already has the native `exyoki` on `PATH`
gets a second one from the wheel; whichever comes first in `PATH` wins, and
both print their version with `--version`.

Wheels exist for Windows x64 (`win_amd64`) and Linux x64 (`manylinux_2_39`,
the glibc of the runner that builds the release). On any other platform `pip`
reports that no matching distribution was found; use the container image or
build from source. The wheel carries the ExyokiOffice license and the notices
of every vendored component under `exyokioffice_mcp/licenses`.

## The MCP bundles

An MCP bundle is a zip file with a `manifest.json` that Claude Desktop reads
to install a server with no configuration file edited by hand. One bundle
holds one server for one platform, so a release has six:

```text
exyoki-mcp-word-1.1.0-win32-x64.mcpb
exyoki-mcp-excel-1.1.0-win32-x64.mcpb
exyoki-mcp-power-point-1.1.0-win32-x64.mcpb
exyoki-mcp-word-1.1.0-linux-x64.mcpb
exyoki-mcp-excel-1.1.0-linux-x64.mcpb
exyoki-mcp-power-point-1.1.0-linux-x64.mcpb
```

Each carries the server, the shared library, the license notices and a
manifest that names every tool with its description. Open the file in Claude
Desktop, or drop it on the Extensions page of its settings; the installer
shows the tool list and asks for one setting, the **document folder**, which
becomes the server's `--workspace`. That folder is the only place the server
can read or write; the [security model](mcp-servers.md#security-model)
describes what that means for paths in tool calls.

The manifest is `manifest_version` 0.3 with `server.type` `binary`, an
`entry_point` under `bin/`, and `compatibility.platforms` naming the one
platform the bundle is built for. There is no macOS bundle, because the
release workflow builds no macOS binaries.

Every bundle has a `.sha256` file beside it on the release page and the same
digest inside the [registry entry](#the-mcp-registry-entries); Claude Desktop
does not check it, but a host that installs from the registry must.

## The MCP registry entries

The [MCP Registry](https://registry.modelcontextprotocol.io) is a catalog of
`server.json` files, one per server, telling a host where a server's packages
are and how to start them. ExyokiOffice has three entries:

```text
io.github.JakubMelka/exyoki-mcp-word
io.github.JakubMelka/exyoki-mcp-excel
io.github.JakubMelka/exyoki-mcp-power-point
```

Each entry lists the same server in every form the release ships:

- **`pypi`**: `exyokioffice-mcp`, run with `uvx`, with the family as a fixed
  positional argument and `--workspace` as the one value the host has to ask
  the user for.
- **`oci`**: `ghcr.io/jakubmelka/exyokioffice:<version>-<family>`, run with
  `docker`, mounting the workspace on `/work` and closing the network. The
  per-family tags exist for the registry alone: it proves ownership of an
  image by an `io.modelcontextprotocol.server.name` label, one label holds one
  name, and the release image serves three servers. `publish_docker` derives
  the three tags from the release image by adding that label and nothing
  else, so they share every layer with it.
- **`mcpb`**: the two bundles from the GitHub release, each with its
  `fileSha256`.

The entries are generated, not hand-written: `create_install` renders them
from `packaging/servers.json`, `VERSION.txt` and the finished bundles into the
`ExyokiOffice-<version>-mcp-registry` artifact, one `server.json` per server
directory. Publishing is a release step, done after the bundles are attached
to the release and the wheel and image are public, because the registry
fetches all three to verify them:

```powershell
mcp-publisher login github
cd exyoki-mcp-word; mcp-publisher publish; cd ..
cd exyoki-mcp-excel; mcp-publisher publish; cd ..
cd exyoki-mcp-power-point; mcp-publisher publish; cd ..
```

`mcp-publisher` is the registry's own command; the
[registry quickstart](https://github.com/modelcontextprotocol/registry/blob/main/docs/modelcontextprotocol-io/quickstart.mdx)
says where to get it. The namespace `io.github.JakubMelka` is granted by
logging in as that GitHub account and is matched with its exact case. The
wheel's README carries an `mcp-name:` line per server, which is how the
registry ties the PyPI package to the three names.

## Building the packages yourself

`packaging/package.py` needs only Python 3.9 and an install tree — the
directory `cmake --install` writes, which is also what the release archives
contain once extracted:

```powershell
cmake --install build\ninja-release --prefix build\install
python packaging\package.py mcpb  --install-dir build\install --output build\packages
python packaging\package.py wheel --install-dir build\install --output build\packages
python packaging\package.py server-json --output build\packages\registry (Get-Item build\packages\*.mcpb)
```

`mcpb` writes the bundles for the platform the tree was built on, with a
`.sha256` beside each; `--server word` limits it to one server and `--icon`
overrides or, given an empty string, drops the icon. `wheel` additionally
needs [hatchling](https://hatch.pypa.io) importable, or `python -m build`
installed, and derives the platform tag from the tree and, on Linux, from the
glibc of the machine running it, which has to be the machine that built the
binaries; `--wheel-tag` overrides it. `server-json` takes the finished bundles,
because their digests and file names go into the manifests, and `--release-tag`
names the GitHub release they will be attached to when it is not `v<version>`.
`manifest --server word` prints a bundle manifest without building anything,
and `names` lists the servers with their registry names for shell scripts.

Everything the outputs say about a server comes from three places:
`packaging/servers.json` (names, descriptions, keywords, the image and package
names, the registry namespace), the tool catalogs under `docs/schemas/` (the
tool lists in the bundle manifests) and `VERSION.txt`. Changing a description
means changing `servers.json`; a tool added to a server reaches its bundle
manifest through the regenerated catalog.

`packaging/tests/test_package.py` runs the script over a fabricated install
tree and is the `Packaging.Python` CTest entry (`ctest -L packaging`),
registered whenever CMake finds a Python interpreter. Its wheel test skips
itself when hatchling is not importable.

In `create_install`, both jobs run the same three commands over the tree they
have just verified and zipped, and upload
`ExyokiOffice-<version>-mcp-<os>-x64` with the wheel and the bundles. A final
job gathers the bundles of both and renders the registry manifests; see
[Continuous integration](../ci.md#mcp-packages). [RELEASE.md](../../RELEASE.md)
lists where each artifact goes.
