# Continuous integration

ExyokiOffice CI is **manual only**. A full run regenerates and compiles the
whole OpenXML DOM (thousands of translation units), so nothing is wired to
`push` or `pull_request`; every workflow in `.github/workflows` uses
`workflow_dispatch` exclusively.

| Workflow | File | Purpose |
|---|---|---|
| `CI` | `.github/workflows/ci.yml` | Build + the whole CTest suite on MSVC, GCC and Clang, plus sanitizer builds. The test presets carry no label filter, so every registered test runs — including the `mcp` layer and the compatibility matrix. |
| `clang-format` | `.github/workflows/clang-format.yml` | Formats hand-written sources and opens a PR with the fixes. |
| `docs-pdf` | `.github/workflows/docs-pdf.yml` | Renders `docs/` into a single hyperlinked PDF manual with pandoc and uploads it as the `ExyokiOffice-manual-<version>` artifact. Chapter order and link preprocessing live in `docs/_pdf/`. |
| `doxygen-pdf` | `.github/workflows/doxygen-pdf.yml` | Renders the Doxygen API reference for the hand-written public headers (the generated DOM is excluded) into a PDF and uploads it as the `ExyokiOffice-api-reference-<version>` artifact. Configuration lives in `docs/_doxygen/Doxyfile`. |
| `create_install` | `.github/workflows/create_install.yml` | Builds, installs and zips a binary package for Windows and Linux, uploaded as the `ExyokiOffice-<version>-<os>-x64-<compiler>` artifact. The Linux job additionally packs the same binaries into a distroless container image. |

## Running the CI workflow

From the GitHub UI: **Actions → CI → Run workflow**, pick the branch, toggle
the jobs, then start the run.

From the command line:

```powershell
gh workflow run ci.yml --ref master
gh workflow run ci.yml --ref master -f run_linux_gcc=false -f build_type=RelWithDebInfo
gh run watch
```

### Inputs

| Input | Default | Meaning |
|---|---|---|
| `build_type` | `Debug` | Configuration for the non-sanitizer jobs (`Debug` or `RelWithDebInfo`). |
| `run_windows_msvc` | `true` | Windows, Ninja + MSVC, build and `ctest`. |
| `run_linux_gcc` | `true` | Ubuntu, Ninja + GCC, build and `ctest`. |
| `run_linux_clang` | `true` | Ubuntu, Ninja + Clang, build and `ctest`. |
| `run_linux_asan` | `true` | Ubuntu, Clang, ASan + UBSan, build and `ctest`. |
| `run_windows_asan` | `false` | Windows, MSVC AddressSanitizer, build and `ctest`. Opt-in; see the caveats below. |
| `run_mcp_python` | `true` | Ubuntu, Ninja + GCC. Builds only the three MCP servers and runs the `tests/mcp_python` suite through the `Mcp.Python` CTest entry. See below. |
| `warnings_as_errors` | `true` | Sets `EXYOKIOFFICE_WARNINGS_AS_ERRORS`. Turn it off to see the full warning list of a new toolchain instead of stopping at the first one. |
| `detect_leaks` | `false` | Enables LeakSanitizer during the ASan test run. Off by default so a run focuses on memory errors; turn it on for a leak audit. |

Each job selects its toolchain through the CMake presets in
`CMakePresets.json` — the workflow never hardcodes compiler paths or flags.
Windows jobs enter the developer environment through
`.github/workflows/vcvars.cmd`, which uses `vswhere` the same way
`WinBuild.ps1` does locally.

Windows jobs pass `-DEXYOKIOFFICE_BUILD_JOBS=1` because the Ninja presets
already schedule the parallelism; leaving MSVC's default `/MP` on would
oversubscribe the runner.

## The MCP Python SDK suite

`tests/mcp_python` drives the three server binaries the way a real host does:
over stdio, through the official [`mcp`](https://pypi.org/project/mcp/) Python
SDK, with nothing of ExyokiOffice linked into the client. It builds nothing, so
it is not a test layer but a single CTest entry, `Mcp.Python`, registered only
when `EXYOKIOFFICE_BUILD_MCP_PYTHON_TESTS` is on:

```powershell
python -m venv tests\mcp_python\.venv
tests\mcp_python\.venv\Scripts\python -m pip install -e tests\mcp_python
cmake --preset windows-vs -DEXYOKIOFFICE_BUILD_MCP_PYTHON_TESTS=ON
ctest --test-dir build\vs -C Debug -L mcp-python
```

The option is off by default because it is the one test that needs something
the C++ build cannot produce. Configuring with it on and no interpreter in
sight is a configure error naming the two commands above; a virtual environment
next to the suite is found automatically, and `EXYOKIOFFICE_MCP_PYTHON` points
the entry at any other interpreter. The entry passes the built binaries to the
suite through `EXYOKI_MCP_*_EXE`, so it tests the configuration that registered
it rather than whichever build directory the suite would have found on its own.

The `mcp-python` CI job is the only place the project installs anything from
PyPI. It uses the runner's own Python — the suite asks only for 3.10 or newer,
and pinning a version here would test one interpreter rather than the one a
contributor has — and installs into a virtual environment so the dependency
ranges stay in `tests/mcp_python/pyproject.toml`. It configures with
`EXYOKIOFFICE_BUILD_UNIT_TESTS=OFF` and builds the three server targets alone,
because compiling the doctest layers again is the other jobs' work.

The dependency belongs to that suite and to nothing else: it is not linked
into the library, the servers, or any package built from them.

## Sanitizer builds

`EXYOKIOFFICE_SANITIZER` (see `cmake/Sanitizers.cmake`) instruments the whole
build — generator, library, examples and tests — because sanitizer runtimes
require every linked object to agree. Accepted values:

| Value | GCC / Clang | MSVC and clang-cl |
|---|---|---|
| `none` (default) | — | — |
| `address` | `-fsanitize=address` | `/fsanitize=address` |
| `undefined` | `-fsanitize=undefined -fno-sanitize=vptr -fno-sanitize-recover=all` | not supported |
| `address+undefined` | both of the above | not supported |
| `thread` | `-fsanitize=thread` | not supported |

`vptr` is excluded from UBSan: it needs full RTTI visibility across the shared
library boundary. On MSVC the module strips `/RTC` (rejected together with
`/fsanitize=address`) and disables incremental linking.

Ready-made presets:

```powershell
cmake --preset linux-ninja-asan            # Clang, ASan + UBSan, RelWithDebInfo
cmake --preset windows-ninja-asan          # MSVC, ASan, Ninja
cmake --preset windows-vs-asan             # MSVC, ASan, Visual Studio, build/vs-asan
```

Sanitizer presets use their own binary directories, so instrumented and
uninstrumented objects never share a build tree.

Locally on Windows:

```powershell
.\WinBuild.ps1 -Sanitizer Address -Configuration Debug -Test
```

**Windows caveat.** MSVC links the AddressSanitizer runtime dynamically
(`clang_rt.asan_*_dynamic-x86_64.dll`), which lives in the MSVC `bin\Hostx64\x64`
directory. An instrumented binary therefore only starts from a Visual Studio
developer environment. `WinBuild.ps1` and the CI job both enter one; a plain
`powershell.exe` will fail with `STATUS_DLL_NOT_FOUND` (exit code
`-1073741515`). This is why `run_windows_asan` is opt-in and Linux carries the
default sanitizer coverage.

Runtime options used by the ASan job:

- build step: `ASAN_OPTIONS=detect_leaks=0` — the instrumented OpenXML
  generator runs as a build step and must not fail the compile over a
  short-lived allocation;
- test step: `detect_stack_use_after_return=1`, `strict_string_checks=1`,
  `UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1`, and LeakSanitizer
  suppressions from `.github/workflows/lsan.supp`.

Only add suppressions for leaks proven to be outside ExyokiOffice. Leaks in
ExyokiOffice code are bugs.

## Binary packages

`create_install` produces the archives that are attached to a GitHub release.
Each job builds the library, the `exyoki` tool and the three MCP servers
(`EXYOKIOFFICE_BUILD_MCP` is on by default and the packaging presets leave it
on), runs `cmake --install` into a staging prefix and zips that prefix:

```powershell
gh workflow run create_install.yml --ref master
gh workflow run create_install.yml --ref master -f run_linux=false -f package_suffix=rc1
```

| Input | Default | Meaning |
|---|---|---|
| `build_type` | `RelWithDebInfo` | Configuration the packaged binaries are built with (`RelWithDebInfo` or `Debug`). |
| `run_windows` | `true` | Windows x64 package, Ninja + MSVC. |
| `run_linux` | `true` | Linux x64 package, Ninja + GCC. |
| `package_suffix` | empty | Extra tag appended to the package name, for example `rc1`. |
| `verify_package` | `true` | Configures `tests/install` against the staged prefix, links and runs it, then runs `--version` on `exyoki` and all three MCP servers from the package. Also verifies the container image when one is built. |
| `build_docker` | `true` | Also pack the Linux binaries into a container image. Ignored when `run_linux` is off. |

The archive is named after the version in `VERSION.txt` and the platform it
was built for, for example `ExyokiOffice-1.0.0-windows-x64-msvc.zip` and
`ExyokiOffice-1.0.0-linux-x64-gcc.zip`. A `.sha256` checksum file is uploaded
next to it and the run summary repeats the digest.

Each archive holds a single root folder of the same name, so extracting it
never scatters files into the current directory:

```text
ExyokiOffice-1.0.0-windows-x64-msvc/
  BUILD-INFO.txt      version, platform, compiler, build type, commit, run URL
  bin/                exyoki, exyoki-mcp-word, exyoki-mcp-excel,
                      exyoki-mcp-power-point, plus ExyokiOffice.dll on Windows
  lib/                shared library or import library, and lib/cmake/ExyokiOffice
  include/            public headers
  share/doc/          LICENSE, THIRD-PARTY-LICENSES, README, CHANGELOG,
                      SECURITY and the manual pages
  share/doc/licenses/ the notice of every vendored third-party component that
                      is compiled into one of the shipped binaries
```

Unit tests and the examples are switched off for a packaging run: nothing
installs them, so building them would only lengthen the run. The separate
installed-package smoke project verifies that a consumer can find and link the
staged CMake package; the workflow then starts the installed CLI and all three
MCP binaries with `--version`. On Linux every installed binary carries a
loader-relative RPATH into the package's library directory, which is what makes
the extracted tree runnable from any location.

The archive is therefore the supported way to obtain the MCP servers without
building the project; see [MCP servers](tools/mcp-servers.md) for registering
them with a client.

### Why the Linux runner is pinned

`linux-package` says `runs-on: ubuntu-24.04`, not `ubuntu-latest`, because the
runner's glibc is the floor of everything the job produces. It has to be low
enough for the distributions the zip archive is meant to be extracted on, and
**no higher than the glibc of the container image's runtime base** — a binary
linked against a newer glibc than the base provides does not start there at
all, and the failure surfaces only when the image is run.

The pairing today is Ubuntu 24.04 (glibc 2.39, GCC 13) built against
`gcr.io/distroless/cc-debian13` (glibc 2.41, the libstdc++ of GCC 14). The
runner is the older of the two, which is the direction that works.

Going older on the runner is not free: the library uses `<format>`, which
libstdc++ only implements from GCC 13, so anything shipping GCC 12 or earlier —
Debian 12 and Ubuntu 22.04 among them — cannot compile the project at all. That
puts a hard bound under the runner label and, through it, under the glibc floor
of both artifacts.

`docker/Dockerfile` and the `runs-on:` label therefore have to be read
together, and both carry a comment saying so.

### The container image

With `build_docker` on, the Linux job packs the staged binaries into a
distroless image and uploads it as `ExyokiOffice-<version>-docker-amd64`, a
gzipped `docker save` tarball with a `.sha256` beside it. Nothing is pushed to
a registry; `docker load` is the whole installation.

Only the runtime half of the package travels: `bin/`, the shared library and
the license notices. The public headers are thousands of files and most of the
archive's bulk, and nothing in the image compiles against them.

Because a distroless image has no shell, the layout cannot be arranged inside
it with `RUN`. The `Stage the container root` step assembles the finished tree
first and the Dockerfile copies it in one piece; `bin/` and the library
directory stay siblings, which is the relation the binaries' built-in RPATH
already describes, so no path has to be patched. For the same reason the image
is verified by being used rather than inspected: the workflow prints the usage
page, runs `--version` on all four programs, then drives the Word server
through a `--replay` file that writes a document into a mounted directory and
validates it with the CLI from the same image.

[The container image](tools/docker.md) documents the result from a user's side.

## What is not covered yet

- **The document corpus is finite.** The 15 Office-saved fixtures are required,
  checksummed through `corpus/manifest.json`, opened by all three family
  editors, validated, checked by both content-model matchers, passed through an
  open-save-open graph/payload round trip, and serialized against the semantic
  JSON Schema. That catches regressions against real Office output, but it is
  not an exhaustive matrix of Office releases, third-party producers, locales,
  and feature combinations.
- **No fuzzing job.** The seven libFuzzer targets exist and are documented in
  [Fuzzing](fuzzing.md), but they run locally through `WinFuzz.ps1`; no
  workflow drives them. CI only replays the committed seed corpus and the
  crash artifacts, as part of `ExyokiOfficePackageTests`.
- **No independent validation oracle.** Compatibility is confirmed by the
  library's own validator only; nothing cross-checks a package against a
  second implementation.
- No "regenerate produces no diff" check and no coverage job.
