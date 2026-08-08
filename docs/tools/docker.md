# The container image

The `create_install` workflow packs the Linux binaries into a container image
alongside the zip archive. It is the shortest way to run [`exyoki`](exyoki.md)
or to register the three [MCP servers](mcp-servers.md) with an AI client: no
extraction, no library paths, and no dependency on the glibc of the machine it
runs on.

The image is distroless. It carries the ExyokiOffice shared library, the four
installed programs, the third-party license notices, and a dispatcher that
turns `docker run` into one of the programs. There is no shell, no package
manager, no interpreter, and nothing else to execute.

## Contents

- [Getting the image](#getting-the-image)
- [What is inside](#what-is-inside)
- [Running the command-line tool](#running-the-command-line-tool)
- [Registering the MCP servers](#registering-the-mcp-servers)
- [The workspace and file ownership](#the-workspace-and-file-ownership)
- [Building the image yourself](#building-the-image-yourself)
- [Troubleshooting](#troubleshooting)

## Getting the image

The image is not pushed to a registry. It is uploaded by the workflow as the
`ExyokiOffice-<version>-docker-amd64` artifact, a gzipped `docker save`
tarball with a `.sha256` next to it. Download it from the run page and load it:

```bash
sha256sum -c ExyokiOffice-1.0.0-docker-amd64.tar.gz.sha256
docker load < ExyokiOffice-1.0.0-docker-amd64.tar.gz
# Loaded image: exyokioffice:1.0.0
```

Run it with no arguments and it tells you the rest:

```bash
docker run --rm exyokioffice:1.0.0
```

The download is around 27 MB and the image occupies roughly 127 MB once
unpacked, most of it the shared library. Only `linux/amd64` is built, matching
the x64-only zip archives.

## What is inside

```text
/opt/exyokioffice/
  exyoki-docker         the dispatcher; the image's entry point
  bin/                  exyoki, exyoki-mcp-word, exyoki-mcp-excel,
                        exyoki-mcp-power-point
  lib/                  libExyokiOffice.so and its soname links
  share/                LICENSE, THIRD-PARTY-LICENSES.md, BUILD-INFO.txt
  share/licenses/       the notice of every vendored third-party component
/work                   the working directory and the MCP workspace root
```

`BUILD-INFO.txt` records the version, the commit, the compiler and the glibc
the binaries were built against, exactly as it does in the zip archive.

Because there is no shell to ask the image what it is, the same identity is
also on the outside, as [OCI image
labels](https://github.com/opencontainers/image-spec/blob/main/annotations.md):

```bash
docker image inspect --format '{{json .Config.Labels}}' exyokioffice:1.0.0
docker image inspect \
  --format '{{index .Config.Labels "org.opencontainers.image.revision"}}' \
  exyokioffice:1.0.0
```

`title`, `description`, `licenses`, `vendor`, `authors`, `url`, `source`,
`documentation` and `base.name` are fixed in `docker/Dockerfile`; `version`,
`revision` and `created` describe the individual build and are passed by the
workflow. The workflow fails the build if any of the twelve is missing, so an
image that loads is an image that can be identified.

The public headers and the CMake package configuration are **not** in the
image: nothing inside it compiles against the library, and the headers are the
bulk of the archive. To build your own program against ExyokiOffice, use the
zip archive — see [Continuous integration](../ci.md#binary-packages).

The four programs are the same binaries the zip archive carries, built from the
same run. The library finds itself through the relative RPATH described there,
which is why `bin/` and `lib/` sit next to each other here too.

## Running the command-line tool

Name `exyoki` as the first argument; everything after it reaches the tool
unchanged. Mount the documents you want it to see at `/work`:

```bash
docker run --rm -v "$PWD:/work" exyokioffice:1.0.0 exyoki --help
docker run --rm -v "$PWD:/work" exyokioffice:1.0.0 exyoki validate report.docx
docker run --rm -v "$PWD:/work" exyokioffice:1.0.0 exyoki convert report.docx report.md
```

`/work` is the working directory, so relative paths mean what you expect.

## Registering the MCP servers

The servers speak JSON-RPC over standard input and output, so the container
needs `-i` and must not be given `-t`. Beyond that the entry is the ordinary
`command` plus `args` shape every client uses — `docker` is the command:

```jsonc
{
  "mcpServers": {
    "word": {
      "command": "docker",
      "args": ["run", "--rm", "-i",
               "-v", "/path/to/documents:/work",
               "exyokioffice:1.0.0", "word"]
    },
    "excel": {
      "command": "docker",
      "args": ["run", "--rm", "-i",
               "-v", "/path/to/documents:/work",
               "exyokioffice:1.0.0", "excel"]
    },
    "powerpoint": {
      "command": "docker",
      "args": ["run", "--rm", "-i",
               "-v", "/path/to/documents:/work",
               "exyokioffice:1.0.0", "powerpoint"]
    }
  }
}
```

This is `.mcp.json` in a Claude Code project root; Claude Desktop uses the same
object in `claude_desktop_config.json`, VS Code `.vscode/mcp.json` with a
`servers` member, and Cursor `.cursor/mcp.json`.

The short names `word`, `excel` and `powerpoint` are the dispatcher's; the
installed file names (`exyoki-mcp-word` and so on) work too, so a command line
copied from [MCP servers](mcp-servers.md) needs no translation.

Every option of those servers is passed through after the name, and every one
of them also reads an environment variable, which `-e` can set:

```bash
docker run --rm -i -v "$PWD:/work" exyokioffice:1.0.0 word --read-only
docker run --rm    -v "$PWD:/work" exyokioffice:1.0.0 word --print-tools
docker run --rm -i -e EXYOKI_MCP_LOG_LEVEL=debug \
  -v "$PWD:/work" exyokioffice:1.0.0 word
```

`--workspace` defaults to `/work` through `EXYOKI_MCP_WORKSPACE`, so a mount
there is all the sandbox configuration a normal setup needs. Everything the
[security model](mcp-servers.md#security-model) says about that sandbox still
applies inside the container, and the container is a second boundary
underneath it: a path that escapes the workspace check would still find only
the image.

## The workspace and file ownership

The image runs as uid 65532 and never as root. Files it writes into a bind
mount therefore belong to that uid, which is usually not what you want on your
own machine. Pass your own identity instead:

```bash
docker run --rm --user "$(id -u):$(id -g)" \
  -v "$PWD:/work" exyokioffice:1.0.0 exyoki convert report.docx report.md
```

With `--user`, the container writes as you and the results are yours. The same
flag belongs in the `args` of an MCP entry whose server saves documents.

Without any mount, `/work` is a writable directory inside the container and
everything saved there disappears with it.

## Building the image yourself

The recipe is [`docker/Dockerfile`](../../docker/Dockerfile). It assembles the
runtime image from an ordinary Linux install tree; it does not compile anything
except the dispatcher, so build the project first however you normally would:

```bash
cmake --preset linux-ninja-release -DEXYOKIOFFICE_BUILD_UNIT_TESTS=OFF
cmake --build --preset linux-ninja-release
cmake --install build/linux-ninja-release --prefix build/package/eo --strip
```

One constraint governs where that build may happen: **its glibc must be no
newer than the runtime base's.** The base is
`gcr.io/distroless/cc-debian13` (glibc 2.41, the libstdc++ of GCC 14), and the
workflow builds on Ubuntu 24.04 (glibc 2.39, GCC 13). Build on something newer
and the binaries will not start in the image — the error names the missing
`GLIBC_*` symbol versions. Building on something much older is not an option
either: the library uses `<format>`, so GCC 13 or newer is required.

`docker/Dockerfile` does not take the repository as its build context. Because
a distroless image has no shell, nothing can be arranged inside it with `RUN`,
so the workflow stages the finished layout in `rootfs/` next to the Dockerfile
and the image copies it in one piece. The `Stage the container root` step of
`create_install.yml` is the authoritative version of that staging.

## Troubleshooting

| Symptom | Cause | Fix |
|---|---|---|
| `docker exec` or `--entrypoint sh` fails | There is no shell in the image, by design | Inspect the files with `docker cp`, or `docker run --rm IMAGE exyoki parts <package>` for package contents |
| You do not know which build an image came from | The tag alone does not say | `docker image inspect --format '{{json .Config.Labels}}' IMAGE` — the labels carry the version, the commit and the build time |
| The client lists no tools | `-t` was passed, or `-i` was not | An MCP server needs standard input; `-i` and no `-t` |
| `path_outside_workspace` for a file you mounted | The mount is not at `/work`, or the path given is a host path | Mount at `/work` and pass paths relative to it |
| Saved documents belong to uid 65532 | The image runs unprivileged and the mount inherited that | Add `--user "$(id -u):$(id -g)"` |
| `no such file or directory` on start | An `amd64` image on a different architecture | Only `linux/amd64` is published; build from source elsewhere |
| Unknown command | The first argument is not one of the four programs | Run the image with no arguments for the list |

The MCP servers log to standard error, which docker shows as usual; standard
output carries the protocol and nothing else. Everything in
[MCP servers → Troubleshooting](mcp-servers.md#troubleshooting) applies
unchanged inside the container.
