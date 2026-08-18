# Versioning and ABI policy

ExyokiOffice uses Semantic Versioning (`MAJOR.MINOR.PATCH`).

## Where the version lives

The release number is stored once, in `VERSION.txt` in the repository root, and
everything else derives from it:

| Consumer | How it reads the number |
|---|---|
| CMake project and package config | `CMakeLists.txt` reads `VERSION.txt` into `ExyokiOfficeVersion` and passes it to `project()` |
| `ExyokiOffice::Version` and `GetVersion()` | generated from `cmake/Version.hpp.in` at configure time |
| Windows DLL resource | generated from `cmake/VersionInfo.rc.in` at configure time |
| `exyoki` JSON reports (`toolVersion`) | calls `GetVersion()` |
| PDF manual and API reference | the `docs-pdf` and `doxygen-pdf` workflows read `VERSION.txt` and put it in the file name and on the title page |
| Binary release archives | the `create_install` workflow reads `VERSION.txt` and puts it in the archive name and in its `BUILD-INFO.txt` |

`VERSION.txt` holds a bare `MAJOR.MINOR.PATCH` line; CMake rejects anything
else at configure time rather than propagating a malformed number.

Bumping a release therefore means editing `VERSION.txt` and nothing else — the
prose that quotes a version number (changelog, `find_package` snippets, sample
tool output) is listed in the release manual, [`RELEASE.md`](../RELEASE.md),
which walks through the whole procedure.

## What each release level promises

| Release | Source compatibility | Binary compatibility (ABI) |
|---|---|---|
| Patch (`1.2.3` → `1.2.4`) | preserved | **preserved** — drop in the new library and relink nothing |
| Minor (`1.2.x` → `1.3.0`) | preserved for existing code, apart from a documented break | **may change** — recompilation required |
| Major (`1.x.y` → `2.0.0`) | may break | not preserved |

Only a patch release guarantees an unchanged ABI. A minor release is allowed to
change the ABI — layouts, virtual tables, inline behavior, and the set of
exported symbols, up to removing or replacing an exported function — so
consumers must rebuild against it.

Source compatibility is normally preserved across a minor release. A minor may
nonetheless carry a documented source-breaking change — most often a removal or
a signature change forced by a security or correctness fix — without a major
bump; a major release is reserved for a wider redesign. Every such break is
called out in the changelog.

## The ABI identity

Because compatibility ends at the minor version, the ABI identity is exactly
`MAJOR.MINOR`. It is derived from `VERSION.txt` rather than maintained as a
separate number — a hand-kept counter could only drift from the policy it is
supposed to encode:

- `Version::Abi` and `GetAbiVersion()` report it as a string, for example
  `"1.0"`. A matching pair is what this project promises; see the toolchain
  condition below for what it does not cover.
- The installed shared library carries it as `SOVERSION`, so a minor release
  produces a new soname on platforms with versioned library names.
- The generated package config uses `COMPATIBILITY SameMinorVersion`, so
  `find_package(ExyokiOffice 1.0 CONFIG REQUIRED)` accepts every `1.0.z` and
  rejects `1.1.0`, matching the guarantee above.

Consumers should link the exported `ExyokiOffice::ExyokiOffice` CMake target
instead of depending on a library filename.

## The condition the version number cannot express

The table above is a promise about *this project's* changes. It is not a
promise that any two builds carrying the same `MAJOR.MINOR` can be mixed,
because the public API passes standard-library types — `std::string`,
`std::vector`, `std::optional`, `std::filesystem::path` — by value across the
library boundary. Their layout belongs to the toolchain, not to ExyokiOffice,
so the library and everything linking it must additionally agree on:

- the compiler and its standard library (MSVC STL, libstdc++, libc++ are not
  interchangeable, and neither are major versions of one of them);
- the C++ standard the two were compiled with;
- on MSVC, the runtime library and `_ITERATOR_DEBUG_LEVEL`, which is why a
  Debug build of the library cannot serve a Release consumer;
- on libstdc++, `_GLIBCXX_USE_CXX11_ABI`.

A mismatch here is not caught by the version check. It shows up as a link
error in the lucky case and as heap corruption in the unlucky one. Installing
the library through a package manager that builds it with the same toolchain
as the consuming project — vcpkg, or a distribution package — makes the
condition hold by construction.

Public API consists of installed headers under `include/ExyokiOffice`.
Generated DOM headers are public API but may grow as schema coverage improves.
Deprecations are announced in the changelog before removal whenever practical.
