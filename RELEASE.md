# Releasing ExyokiOffice

A checklist for cutting a new release. It is written so that a person or a
coding agent can follow it top to bottom without knowing the repository, and it
names every file a version number can hide in.

The guiding rule: **`VERSION.txt` in the repository root is the only functional
version literal.** The build, `Version.hpp`, the Windows DLL resource, the
installed CMake package, both documentation PDFs and the names of the release
archives derive from it. Everything
else listed below is prose that mentions a number, not a source of truth. See
[docs/ABI.md](docs/ABI.md) for the versioning policy itself.

Replace `X.Y.Z` with the new version and `A.B.C` with the previous one
throughout. Shell snippets are PowerShell unless marked otherwise; the
repository is developed on Windows.

## 1. Decide the number

Semantic versioning, as described in [docs/ABI.md](docs/ABI.md):

| Change | Bump |
| --- | --- |
| Wider redesign of the public API | major |
| ABI change, a documented source-breaking change, or new backward-compatible functionality | minor |
| Backward-compatible fixes that keep the ABI | patch |

The `0.x` series is over: from 1.0.0 on, only a patch release keeps the ABI. A
minor release may change the ABI — consumers recompile — and may even carry a
documented source-breaking change, most often a removal or a signature change
forced by a security or correctness fix, provided the break is spelled out in
the changelog; a major release is reserved for a wider redesign. See
[docs/ABI.md](docs/ABI.md) for the full policy. Read the `## [Unreleased]`
section of [CHANGELOG.md](CHANGELOG.md) to see what actually accumulated —
that, not a calendar, decides the number. A `### Removed` section is a minor
release when its break is documented there, and a major only when the API is
being redesigned more broadly.

## 2. Edit the version

### 2.1 `VERSION.txt` — always

One bare `MAJOR.MINOR.PATCH` line, nothing else. CMake validates the format at
configure time and fails the build on anything else, so a typo cannot slip
through.

### 2.2 The ABI version — nothing to edit

`ExyokiOfficeAbiVersion` in `CMakeLists.txt` is derived as
`MAJOR.MINOR` from the version you just set; it needs no hand editing at any
release level. It becomes `SOVERSION` on the shared library and `Version::Abi`
in the public header.

What does need a decision is whether the number you picked in step 1 is honest
about compatibility. Per [docs/ABI.md](docs/ABI.md), only a patch release
promises an unchanged ABI. So a change that alters a public type's layout, a
virtual table, an inline function's behavior or the exported symbol set cannot
ship as a patch release, even when it compiles against existing code — it needs
at least a minor bump, which is exactly what moves the soname.

### 2.3 Minimum-version snippets — major or minor releases

Consumers are shown `find_package(ExyokiOffice <major>.<minor> CONFIG
REQUIRED)`. The installed package config is written with
`COMPATIBILITY SameMinorVersion`, so a stale minimum in these snippets is not
cosmetic: **`tests/install` genuinely fails to configure** when its request no
longer matches the installed version.

- [tests/install/CMakeLists.txt](tests/install/CMakeLists.txt) — functional,
  the installed-package smoke test
- [vcpkg/test/CMakeLists.txt](vcpkg/test/CMakeLists.txt) — functional, the same
  test for the vcpkg package
- [README.md](README.md) — the consuming-the-package section
- [docs/introduction.md](docs/introduction.md) — the same snippet in the manual
- [llms-full.txt](llms-full.txt) — the same snippet in the AI documentation

Patch releases do not touch these: `1.0` still matches `1.0.4`.

### 2.4 Sample output and image tags — whenever the number changes

Markdown cannot interpolate the number, so every place a sample prints or pulls
the version is a manual step. None of these break a build; all of them tell a
reader to install the previous release.

- [docs/tools/exyoki.md](docs/tools/exyoki.md) — four `"toolVersion": "X.Y.Z"`
  samples, which `exyoki` prints from the library version
- [docs/tools/mcp-servers.md](docs/tools/mcp-servers.md) — three
  `exyoki-mcp-word X.Y.Z` start-up lines
- [docs/tools/docker.md](docs/tools/docker.md) — the pulled and loaded image
  tags and the `docker save` tarball name. The compressed and unpacked size
  figures name a release too; re-read them from the `create_install` summary in
  step 8 rather than carrying the old numbers forward.
- [README.md](README.md) — the two `docker pull` lines in the tooling section
- [docs/ci.md](docs/ci.md) — the sample archive names and the
  `publish_docker.yml` invocation

`vcpkg/test/vcpkg.json` also carries a `version`, but it is the version of the
consumer test project itself, not of the library. It happens to have started at
`1.0.0` and does not move with a release.

### 2.5 Prove nothing was missed

```powershell
rg -n "A\.B\.C" -g '!build/**' -g '!gen/build/**' -g '!data/**' -g '!CHANGELOG.md'
```

Only historical mentions may survive, and after step 3 the changelog is
expected to contain the old number. Anything else is a place this manual should
have listed — add it here when you find it.

## 3. Update the changelog

In [CHANGELOG.md](CHANGELOG.md):

1. Rename `## [Unreleased]` to `## [X.Y.Z] - YYYY-MM-DD`, using the actual
   release date in ISO form.
2. Add a fresh empty section on top:

   ```markdown
   ## [Unreleased]

   Nothing yet.
   ```

3. Drop any `Nothing yet.` placeholder from the section being released, and
   check that its entries describe user-visible changes rather than commits.

Entries are grouped under `Added`, `Changed`, `Deprecated`, `Removed`, `Fixed`
and `Security`. Deprecations should have been announced in an earlier release
before anything is listed under `Removed`.

## 4. Build, test and lint locally

A clean build compiles thousands of generated translation units; allow at least
ten minutes per configuration and do not start a second build over the same
build tree.

```powershell
.\WinBuild.ps1 -Clean -Configuration RelWithDebInfo -Test
.\WinBuild.ps1 -Configuration Debug -Test
.\WinLint.ps1 -Check
```

`WinBuild.ps1 -Test` runs the full CTest suite for that configuration. To run a
single layer while investigating a failure, use its label, for example
`ctest --preset debug -L word`.

Then confirm the version actually propagated:

```powershell
Select-String -Path build\vs\ExyokiOffice\Version.hpp -Pattern 'Major|Minor|Patch|Abi|String'
Select-String -Path build\vs\ExyokiOffice\ExyokiOffice.rc -Pattern 'FILEVERSION|ProductVersion'
```

`Version::String` must equal `VERSION.txt`, and `Version::Abi` must be its
`MAJOR.MINOR` prefix. `tests/unit/VersionTests.cpp` asserts both relationships
and derives its expectations from the version fields, so it needs no editing at
release time; `ctest --preset debug -L unit` runs it.

**After any build, run `git diff`.** `generate_openxml` writes into tracked
source directories, so regenerated files can appear without being asked for.
Regenerated output that is not part of the release belongs in its own commit,
not in the release commit.

## 5. Verify the installed package

This is the step that catches a stale `find_package` minimum, and it is not
covered by CI.

```powershell
$prefix = Join-Path (Get-Location) 'build\install'
.\WinBuild.ps1 -Configuration RelWithDebInfo -Install -InstallPrefix build\install
cmake -S tests\install -B build\install-smoke "-DCMAKE_PREFIX_PATH=$prefix"
cmake --build build\install-smoke --config RelWithDebInfo
# ExyokiOffice is a shared library; the smoke executable needs the DLL.
$env:PATH = "$prefix\bin;$env:PATH"
.\build\install-smoke\RelWithDebInfo\ExyokiOfficeInstallSmoke.exe
```

The executable prints the library version and returns non-zero if the runtime
and compile-time versions disagree. The printed number must be `X.Y.Z`.
`CMAKE_PREFIX_PATH` needs an absolute path, hence the `$prefix` variable, and
the whole `-D` argument is quoted: unquoted, PowerShell mangles the backslashes
on their way to CMake and the package is reported missing rather than stale.

Then check that the notices the licenses require a binary distribution to carry
are actually in it:

```powershell
Get-ChildItem "$prefix\share\doc\ExyokiOffice\licenses"
```

Six files, one per vendored component compiled into a shipped binary. A new
dependency is not vendored until it is installed here and listed in
[THIRD-PARTY-LICENSES.md](THIRD-PARTY-LICENSES.md) and the third-party table in
[README.md](README.md).

## 6. Run the CI workflows

Every workflow that matters for a release is manual (`workflow_dispatch`) — only
the one-job `smoke.yml` runs on push, and it builds nothing a release ships.
Trigger the rest from the Actions tab or with `gh`:

```powershell
gh workflow run ci.yml -f build_type=RelWithDebInfo
gh workflow run clang-format.yml
```

`ci.yml` covers Windows MSVC, Linux GCC, Linux Clang and the sanitizer builds,
which is the project's Linux and Clang coverage — a local Linux build is not
required. `clang-format.yml` opens a pull request with formatting fixes if the
tree is not clean; merge it before tagging rather than tagging over it.

Optionally, for a major release, let the fuzzers run for a while:

```powershell
.\WinFuzz.ps1 -Target all -Seconds 600
```

## 7. Render the documentation PDFs

```powershell
gh workflow run docs-pdf.yml
gh workflow run doxygen-pdf.yml
```

Both read `VERSION.txt`, so the artifacts come out as
`ExyokiOffice-manual-X.Y.Z` and `ExyokiOffice-api-reference-X.Y.Z`, with
the version on the title page under the project logo. Trigger them **after**
`VERSION.txt` is committed and pushed — the workflows build from the branch,
not from your working tree. Download both artifacts and check the title pages
before attaching them to the release.

## 8. Build the binary packages

```powershell
gh workflow run create_install.yml
```

Like the PDFs, this builds from the branch, so trigger it **after**
`VERSION.txt` is committed and pushed. It produces
`ExyokiOffice-X.Y.Z-windows-x64-msvc.zip` and
`ExyokiOffice-X.Y.Z-linux-x64-gcc.zip`, each with a `.sha256` file beside it,
and — with `build_docker` left on — `ExyokiOffice-X.Y.Z-docker-amd64.tar.gz`
with its own digest. The run leaves every digest in its summary; keep them, the
release notes should quote them.

With `verify_package` left on, both jobs configure and run `tests/install`
against the staged package before zipping it, which is the same check as step 5
run on the release binaries, and the Linux job additionally drives the finished
container image through an MCP session. Download one archive anyway, extract it
and run `bin/exyoki --version`; do the same with the image:

```powershell
docker load -i ExyokiOffice-X.Y.Z-docker-amd64.tar.gz
docker run --rm exyokioffice:X.Y.Z
```

The packaging is only proven once the artifact that users will download has
been opened.

## 9. Commit, tag and publish

Stage deliberately. The repository root tends to collect untracked working
material — audit notes, sample documents produced while testing — and `git add
-A` would publish all of it in the release commit. Run `git status` first and
either delete what does not belong or move it out of the tree.

```powershell
git add -A
git commit -m "Release X.Y.Z"
git tag -a vX.Y.Z -m "ExyokiOffice X.Y.Z"
git push origin master --follow-tags
```

Tags are named `vX.Y.Z`. Then create the release, using the changelog section
as its body:

```powershell
gh release create vX.Y.Z --title "ExyokiOffice X.Y.Z" --notes-file <notes>
gh release upload vX.Y.Z ExyokiOffice-manual-X.Y.Z.pdf ExyokiOffice-api-reference-X.Y.Z.pdf
gh release upload vX.Y.Z ExyokiOffice-X.Y.Z-windows-x64-msvc.zip ExyokiOffice-X.Y.Z-windows-x64-msvc.zip.sha256
gh release upload vX.Y.Z ExyokiOffice-X.Y.Z-linux-x64-gcc.zip ExyokiOffice-X.Y.Z-linux-x64-gcc.zip.sha256
gh release upload vX.Y.Z ExyokiOffice-X.Y.Z-docker-amd64.tar.gz ExyokiOffice-X.Y.Z-docker-amd64.tar.gz.sha256
```

Then push the container image to the GitHub Container Registry, where it shows
up under the repository's Packages:

```powershell
gh workflow run publish_docker.yml -f release=vX.Y.Z
```

This comes after the release is published, not before: the workflow reads the
image out of the release itself — it downloads the `-docker-amd64` asset,
checks its digest and pushes exactly that — and the workflow token does not see
a draft. It builds nothing, so it takes a minute. See
[docs/ci.md](docs/ci.md#publishing-the-image-to-the-registry).

The very first run creates the package as **private**. Open Packages →
`exyokioffice` → Package settings and change the visibility to public; every
later release inherits that setting.

## 10. Update the vcpkg port

This step comes after the tag, because the port names the release tarball and
verifies its hash — neither exists before step 9. The port lives in a clone of
microsoft/vcpkg, not in this repository; [vcpkg/README.md](vcpkg/README.md)
says where and why.

```powershell
cd $env:VCPKG_ROOT
# version in ports/exyokioffice/vcpkg.json must equal VERSION.txt
# SHA512 in ports/exyokioffice/portfile.cmake must be the new tarball's:
.\vcpkg.exe install exyokioffice     # the mismatch error prints the actual hash
.\vcpkg.exe format-manifest ports/exyokioffice/vcpkg.json
git add ports/exyokioffice
git commit -m "[exyokioffice] Update to X.Y.Z"
.\vcpkg.exe x-add-version exyokioffice
git commit -am "[exyokioffice] Update version database"
```

`x-add-version` generates both `versions/e-/exyokioffice.json` and the
`versions/baseline.json` entry from the committed port, so the port is committed
first and neither file is written by hand. Then prove the published package
works from the outside:

```powershell
.\vcpkg\Test-Port.ps1
```

No `-Head` and no `-LocalSource`: what is being tested is the tarball a consumer
will download.

## 11. After the release

- Confirm `## [Unreleased]` is back at the top of the changelog and empty.
- If any step above turned out to be wrong or incomplete, fix this file in the
  same breath. A release manual is only trustworthy if it is corrected the
  moment it misleads someone.

## Quick checklist

```text
[ ] VERSION.txt                      bumped to X.Y.Z
[ ] version level honest about ABI   only a patch release may keep the soname
[ ] tests/install/CMakeLists.txt     find_package minimum (major/minor)
[ ] vcpkg/test/CMakeLists.txt        find_package minimum (major/minor)
[ ] README.md                        find_package minimum (major/minor)
[ ] docs/introduction.md             find_package minimum (major/minor)
[ ] llms-full.txt                    find_package minimum (major/minor)
[ ] docs/tools/exyoki.md             four toolVersion samples
[ ] docs/tools/mcp-servers.md        three server start-up lines
[ ] docs/tools/docker.md             image tags, tarball name, size figures
[ ] README.md + docs/ci.md           docker pull tags, sample archive names
[ ] CHANGELOG.md                     Unreleased -> [X.Y.Z] - date, new empty Unreleased
[ ] rg for the previous version      no unexpected survivors
[ ] WinBuild.ps1 -Clean -Test        RelWithDebInfo and Debug both green
[ ] WinLint.ps1 -Check               clean
[ ] git diff after the build         no unintended generated churn
[ ] git status before the commit     nothing untracked that should not ship
[ ] Version.hpp / .rc                carry X.Y.Z, Version::Abi is X.Y
[ ] install smoke test               configures, links, prints X.Y.Z
[ ] share/doc/.../licenses           six third-party notices present
[ ] ci.yml + clang-format.yml        green
[ ] docs-pdf + doxygen-pdf           artifacts named X.Y.Z, logo and version on title page
[ ] create_install                   windows and linux zips plus the docker image,
                                     all verified, digests kept
[ ] tag vX.Y.Z + GitHub release      PDFs and both zips attached
[ ] publish_docker                   image pushed to ghcr.io, package public
[ ] vcpkg port                       version, SHA512, x-add-version committed
[ ] vcpkg\Test-Port.ps1              green against the published tarball
```
