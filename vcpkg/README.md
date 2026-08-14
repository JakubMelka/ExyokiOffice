<!--
Copyright (c) 2026 Jakub Melka and Collaborators
SPDX-License-Identifier: MIT
See LICENSE file in the project root for full license text.
-->

# vcpkg packaging

ExyokiOffice ships as the vcpkg port `exyokioffice`. This directory holds the
consumer side of that package — a project that installs it and uses it the way
any other project would — plus the script that runs the whole thing end to end.

## Where the port itself lives

The port is **not** in this repository. A vcpkg port is a directory inside a
vcpkg registry, and the registry ExyokiOffice targets is
[microsoft/vcpkg](https://github.com/microsoft/vcpkg), so the port is maintained
in a clone of it:

```
<vcpkg clone>/ports/exyokioffice/vcpkg.json      # name, version, license, features
<vcpkg clone>/ports/exyokioffice/portfile.cmake  # how the package is built and installed
<vcpkg clone>/ports/exyokioffice/usage           # what vcpkg prints after installing
<vcpkg clone>/versions/e-/exyokioffice.json      # version database, generated
<vcpkg clone>/versions/baseline.json             # baseline entry, generated
```

Keeping it there rather than mirroring it here means there is one copy to
change, and a pull request to microsoft/vcpkg is a diff of that clone rather
than a copy step. The clone this repository is developed against is
`E:\VCPKG\vcpkg`, branch `branches/exyoki`; `Test-Port.ps1` finds it through
`VCPKG_ROOT` and falls back to that path.

## What the port does

| | |
| --- | --- |
| Sources | `vcpkg_from_github` from `JakubMelka/ExyokiOffice`, tag `v<version>` |
| Dependencies | none beyond `vcpkg-cmake` and `vcpkg-cmake-config` — everything third-party is vendored |
| Linkage | both; `vcpkg_cmake_configure` passes `BUILD_SHARED_LIBS` from the triplet |
| Feature `tools` | builds `exyoki` and installs it into `tools/exyokioffice` |
| Feature `mcp` | builds the three MCP servers and installs them into `tools/exyokioffice` |
| Exported target | `ExyokiOffice::ExyokiOffice`, via `find_package(ExyokiOffice 1.0 CONFIG REQUIRED)` |

Neither feature is on by default, so `vcpkg install exyokioffice` builds the
library alone.

The portfile passes `EXYOKIOFFICE_RUN_GENERATOR=OFF`. The generated DOM sources
are committed upstream, and a package build must neither write into the source
tree it was handed nor run a generator it just compiled for the target
architecture. It also passes `EXYOKIOFFICE_WARNINGS_AS_ERRORS=OFF`: a warning
introduced by a compiler newer than the release is a problem for the project,
not for the person installing it.

## Running the smoke test

```powershell
.\vcpkg\Test-Port.ps1 -LocalSource                    # build this working tree
.\vcpkg\Test-Port.ps1 -Head                           # build the tip of GitHub master
.\vcpkg\Test-Port.ps1                                 # build the released tag
.\vcpkg\Test-Port.ps1 -Triplet x64-windows-static     # any other triplet
```

The script installs `exyokioffice[tools,mcp]` into a private root under
`vcpkg/test/build`, then configures, builds, and runs `vcpkg/test` against it.
`vcpkg/test` reaches only for what the package promises a consumer: the
installed headers, the exported target, and a round trip through Word, Excel,
and PowerPoint. `Vcpkg.Tool.Version` and the three `Vcpkg.Mcp.*` entries
register themselves only when the corresponding feature actually installed its
executables, so the same project also passes for a features-off install.

`-LocalSource` is the mode to use while changing either the library or the port.
It generates an overlay port that is the real one with its `vcpkg_from_github`
call replaced by a copy of this working tree, so every other decision the
portfile makes is still the one that ships. Because vcpkg hashes the portfile
and not the tree it copies, that mode also disables binary caching.

Neither `-Head` nor `-LocalSource` reaches for the release tarball, so neither
needs the `SHA512` the portfile carries. They are therefore the two modes that
work against an untagged working tree; the default mode needs a published tag
and its matching hash in the portfile.

## A note on `-static` triplets

vcpkg's consumer toolchain sets `CMAKE_MSVC_RUNTIME_LIBRARY` only while building
ports, never for the project consuming them. An `x64-windows-static` package is
linked against the static runtime, so a consumer that leaves CMake's default in
place fails at link time with `LNK2038: mismatch detected for 'RuntimeLibrary'`.
`vcpkg/test/CMakeLists.txt` therefore selects the static runtime itself when the
triplet ends in `-static`, and that is what any consumer of such a triplet has to
do. `-static-md` keeps the dynamic runtime and needs nothing.

## Releasing a new version

The steps belong to the release checklist in [RELEASE.md](../RELEASE.md); this
is what they do in the vcpkg clone:

```powershell
cd $env:VCPKG_ROOT
# 1. version in ports/exyokioffice/vcpkg.json must equal VERSION.txt
# 2. SHA512 in ports/exyokioffice/portfile.cmake must be the tarball's:
.\vcpkg.exe install exyokioffice   # the mismatch error prints the actual hash
.\vcpkg.exe format-manifest ports/exyokioffice/vcpkg.json
git add ports/exyokioffice
git commit -m "[exyokioffice] Update to <version>"
.\vcpkg.exe x-add-version exyokioffice
git commit -am "[exyokioffice] Update version database"
```

`x-add-version` writes both `versions/e-/exyokioffice.json` and the
`versions/baseline.json` entry from the committed port, which is why the port is
committed first and neither file is edited by hand. Finish with
`.\vcpkg\Test-Port.ps1` — without `-Head` or `-LocalSource`, so that what is
tested is the published tarball.
