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
- [Closing the network](#closing-the-network)
- [Building the image yourself](#building-the-image-yourself)
- [Troubleshooting](#troubleshooting)

## Getting the image

The image lives in the GitHub Container Registry as
`ghcr.io/jakubmelka/exyokioffice`, listed under this repository's [package
page](https://github.com/JakubMelka/ExyokiOffice/pkgs/container/exyokioffice):

```bash
docker pull ghcr.io/jakubmelka/exyokioffice:1.1.0
docker pull ghcr.io/jakubmelka/exyokioffice:latest
```

Two tags point at every release. `1.1.0` is that release and nothing else, and
it never moves; `latest` follows the newest published release, so it means a
different image after each one. Pin the version in anything reproducible — a
`docker compose` file, an MCP client configuration, a CI job — and keep
`latest` for trying the project out.

The same image is also attached to every [GitHub
release](https://github.com/JakubMelka/ExyokiOffice/releases) as the
`ExyokiOffice-<version>-docker-amd64` asset, a gzipped `docker save` tarball
with a `.sha256` next to it, and to the page of the `create_install` run that
produced it. It is the same image, not a second build — the registry copy is
pushed from that very tarball — so use it when a machine cannot reach the
registry, or to keep an archived copy of a version:

```bash
sha256sum -c ExyokiOffice-1.1.0-docker-amd64.tar.gz.sha256
docker load < ExyokiOffice-1.1.0-docker-amd64.tar.gz
# Loaded image: exyokioffice:1.1.0
```

A loaded image is named `exyokioffice:1.1.0`, without the registry prefix. The
examples below use the registry name; substitute whichever you have.

Run it with no arguments and it tells you the rest:

```bash
docker run --rm ghcr.io/jakubmelka/exyokioffice:1.1.0
```

Only `linux/amd64` is built, matching the x64-only zip archives.

### What the size figures mean

The download is a fraction of what it unpacks to: almost all of the image is
the shared library, and it compresses well. For 1.0.0 that is about 28 MB of
compressed layers against roughly 96 MB unpacked, and the summary of the run
that produced the image reports both.

Locally, neither number is what the tooling shows by default:

```bash
docker image inspect --format '{{.Size}}' ghcr.io/jakubmelka/exyokioffice:1.1.0
```

sums the layer sizes as the descriptors you happen to have record them — the
compressed sizes for an image that was pulled, the uncompressed ones for an
image that was loaded from a `docker save` tarball. The same image therefore
reports about 28 MB after a pull and about 96 MB after a load.

The `docker images` column is a third thing again: with the containerd image
store it is disk usage. A pulled image occupies roughly its unpacked size,
while a loaded one occupies about twice that, because `docker save` carries the
layers uncompressed and loading them keeps that copy alongside the unpacked
one. Nothing is wrong with either; they are two local spellings of one image,
and `docker image inspect --format '{{.Id}}'` differing between them is the
same accounting artefact. What identifies the image across both is its layer
set and its labels.

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
IMAGE=ghcr.io/jakubmelka/exyokioffice:1.1.0
docker image inspect --format '{{json .Config.Labels}}' "$IMAGE"
docker image inspect \
  --format '{{index .Config.Labels "org.opencontainers.image.revision"}}' \
  "$IMAGE"
```

`title`, `description`, `licenses`, `vendor`, `authors`, `url`, `source`,
`documentation` and `base.name` are fixed in `docker/Dockerfile`; `version`,
`revision` and `created` describe the individual build and are passed by the
workflow. The workflow fails the build if any of the twelve is missing, so an
image that loads is an image that can be identified.

### What `licenses` covers, and what it does not

The `org.opencontainers.image.licenses` label reads `MIT`, and that is the
license of what this repository puts into the image: the four programs, the
shared library and the dispatcher. Their notices, and those of every vendored
component compiled into them, are the files under `share/` listed above.

The image is not only those files. Under them sits
`gcr.io/distroless/cc-debian13`, which contributes glibc, libstdc++ and a small
set of Debian base packages; those carry their own terms — the LGPL and the GPL
with the GCC runtime exception among them — and are not covered by the label or
by anything under `share/licenses/`. The `base.name` label names that image so
the boundary is checkable from outside.

This matters if you redistribute the image rather than only run it: the
obligations that come with the base are the base's, and the place to satisfy
them is [the distroless project](https://github.com/GoogleContainerTools/distroless)
and Debian's sources for the packages it installs. Running the image, or
building your own from `docker/Dockerfile`, needs nothing of the sort.

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
IMAGE=ghcr.io/jakubmelka/exyokioffice:1.1.0
docker run --rm --network none -v "$PWD:/work" "$IMAGE" exyoki --help
docker run --rm --network none -v "$PWD:/work" "$IMAGE" exyoki validate report.docx
docker run --rm --network none --user "$(id -u):$(id -g)" -v "$PWD:/work" "$IMAGE" \
  exyoki convert report.docx report.md
```

`/work` is the working directory, so relative paths mean what you expect. The
two read-only commands need nothing else; the one that *writes* `report.md`
carries `--user`, because without it the container writes as uid 65532 and a
directory of your own with the usual `755` permissions refuses it. See
[the workspace and file ownership](#the-workspace-and-file-ownership).
`--network none` is explained under [Closing the
network](#closing-the-network); nothing in the image ever reaches out, so it
costs nothing and saves both a risk and a measurable part of the start-up.

## Registering the MCP servers

The servers speak JSON-RPC over standard input and output, so the container
needs `-i` and must not be given `-t`. Beyond that the entry is the ordinary
`command` plus `args` shape every client uses — `docker` is the command:

```jsonc
{
  "mcpServers": {
    "word": {
      "command": "docker",
      "args": ["run", "--rm", "-i", "--network", "none",
               "-v", "/path/to/documents:/work",
               "ghcr.io/jakubmelka/exyokioffice:1.1.0", "word"]
    },
    "excel": {
      "command": "docker",
      "args": ["run", "--rm", "-i", "--network", "none",
               "-v", "/path/to/documents:/work",
               "ghcr.io/jakubmelka/exyokioffice:1.1.0", "excel"]
    },
    "powerpoint": {
      "command": "docker",
      "args": ["run", "--rm", "-i", "--network", "none",
               "-v", "/path/to/documents:/work",
               "ghcr.io/jakubmelka/exyokioffice:1.1.0", "powerpoint"]
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
docker run --rm -i -v "$PWD:/work" "$IMAGE" word --read-only
docker run --rm    -v "$PWD:/work" "$IMAGE" word --print-tools
docker run --rm -i -e EXYOKI_MCP_LOG_LEVEL=debug \
  -v "$PWD:/work" "$IMAGE" word
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
  -v "$PWD:/work" "$IMAGE" exyoki convert report.docx report.md
```

With `--user`, the container writes as you and the results are yours. The same
flag belongs in the `args` of an MCP entry whose server saves documents.

Without any mount, `/work` is a writable directory inside the container and
everything saved there disappears with it.

## Closing the network

Nothing in this image ever opens a socket. The library has no HTTP client at
all — a document's external references are resolved by a resolver the embedding
application supplies, as [External resources](../ExternalResources.md)
describes, and neither `exyoki` nor the MCP servers supply one; `exyoki
external` reports those references without following them. The MCP protocol
itself runs over standard input and output. So the container can be run with no
network at all:

```bash
docker run --rm --network none -v "$PWD:/work" "$IMAGE" exyoki validate report.docx
```

`--network none` gives the container a network namespace containing only
loopback: no veth pair, no bridge, no address, no NAT rules, no published
ports. Nothing can reach out and nothing can reach in.

Two things come of it. It is a real boundary — the sandbox the [security
model](mcp-servers.md#security-model) describes bounds which files a server may
touch, and this bounds where anything inside could send them, whatever it is.
And it is faster: setting that networking up and tearing it down again is a
measurable part of what starting a container costs, and for a program that
never uses it, all of it is waste.

It is the caller's flag, not a property of the image — a Dockerfile cannot
demand it, so every command line and every client entry has to carry it. In
Compose it is `network_mode: "none"`:

```yaml
services:
  exyoki:
    image: ghcr.io/jakubmelka/exyokioffice:1.1.0
    network_mode: "none"
    volumes:
      - ./documents:/work
```

Kubernetes has no equivalent field; there the same thing is a NetworkPolicy
that denies the pod all ingress and egress.

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
| Every `docker run` takes noticeably longer than the work it does | Creating, networking and tearing down a container costs far more than starting these programs, and it is charged once per `docker run` | Add `--network none`, and for a batch run one container over the whole batch — or use the zip archive, where starting the program is all there is |

The MCP servers log to standard error, which docker shows as usual; standard
output carries the protocol and nothing else. Everything in
[MCP servers → Troubleshooting](mcp-servers.md#troubleshooting) applies
unchanged inside the container.
