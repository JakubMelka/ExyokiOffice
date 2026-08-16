# ExyokiOffice repository guide

## Project in one paragraph

ExyokiOffice is a C++20 shared library for creating, opening, editing, and
saving Microsoft Office Open XML packages (`.docx`, `.xlsx`, and `.pptx`). It
writes ZIP/XML packages directly; Microsoft Office and .NET are not build or
runtime dependencies. Hand-written high-level editors cover Word, Excel, and
PowerPoint, while generated typed OpenXML classes and a package layer provide
lower-level access. The repository also builds the `exyoki` command-line tool
and three MCP servers that expose the editors to AI agents.

## Orient yourself before reading code

Read the smallest relevant source of truth first:

1. `README.md` — project overview, three-format quickstarts, build and install.
2. `docs/README.md` — complete documentation index.
3. `docs/Compatibility.md` — supported formats, versions, and feature depth.
4. The task map below — primary API, guide, and runnable example.
5. The relevant public header under `include/ExyokiOffice/`.

Do not begin by enumerating `include/ExyokiOffice/DOM`, `sources/DOM`, or
`data`; these generated/imported trees are very large and usually not the
right implementation layer.

## Task map: goal to API to documentation to example

| Goal | Primary API or tool | Documentation | Runnable example |
| --- | --- | --- | --- |
| Create or edit Word documents | `Word::WordDocumentEditor` | `docs/Word.md`, `docs/word/` | `examples/ExampleWordEditor/main.cpp` |
| Modify an existing Word document | `WordDocumentEditor::Open`, body cursors | `docs/word/documents.md`, `docs/word/text.md` | `examples/ExampleWordEdit/main.cpp` |
| Create or edit Excel workbooks | `Excel::ExcelDocumentEditor` | `docs/Excel.md`, `docs/excel/` | `examples/ExampleExcelEditor/main.cpp` |
| Create or edit PowerPoint presentations | `PowerPoint::PowerPointDocumentEditor` | `docs/PowerPoint.md`, `docs/powerpoint/` | `examples/ExamplePowerPointEditor/main.cpp` |
| Work directly with typed OpenXML elements | `OpenXmlElement`, generated DOM types | `docs/introduction.md` | `examples/ExampleWord/main.cpp` |
| Open/save packages and manipulate parts or relationships | `OpenXmlPackage`, `OpenXmlPackagePart`, `Packaging::*Document` | `docs/introduction.md` | `examples/ExampleWord/main.cpp` |
| Inspect, validate, convert, diff, or query packages in C++ | `ExyokiOffice::Tools`, `ExyokiOffice::Xml` | `docs/tools/exyoki.md`, `docs/tools/conversion-formats.md` | `tools/exyoki/main.cpp` |
| Do the same from a shell | `exyoki` | `docs/tools/exyoki.md` | `tools/exyoki/main.cpp` |
| Expose documents to AI agents over MCP | `exyoki-mcp-word`, `exyoki-mcp-excel`, `exyoki-mcp-power-point` | `docs/tools/mcp-servers.md` | `tools/mcp/word/main.cpp` |
| Handle signatures or linked resources | `ExyokiOffice::Security` | `docs/Signatures.md`, `docs/ExternalResources.md` | focused unit tests under `tests/package/` |
| Design application concurrency | One document graph per worker, or one external mutex per graph | `docs/Threading.md` | locking and cancellation examples in the guide |
| Change generated OpenXML behavior | `gen/` and its metadata readers | `gen/README.md`, `data/README.md` | generator tests under `tests/generator/` |
| Build, test, lint, sanitize, fuzz, or measure coverage | CMake presets and Windows scripts | `README.md`, `docs/ci.md`, `docs/fuzzing.md`, `docs/coverage.md` | `WinBuild.ps1`, `WinLint.ps1`, `WinFuzz.ps1`, `WinCoverage.ps1` |

## Architecture and main API classes

The library has four layers. Prefer the highest layer that models the task:

1. High-level editors under `include/ExyokiOffice/{Word,Excel,PowerPoint}`.
2. Generated typed DOM under `include/ExyokiOffice/DOM`.
3. OPC packaging under `include/ExyokiOffice/Packaging` and the package base
   headers directly under `include/ExyokiOffice`.

Above these sits the tooling and front-end layer: `ExyokiOffice::Tools` and
`ExyokiOffice::Xml` in the shared library, the `exyoki` command line in
`tools/exyoki/`, and the three MCP servers in `tools/mcp/`. A front end never
implements document behavior of its own — it adapts the layers below, so a
missing capability is fixed in the library and surfaced here.

The six main document types are:

- `Word::WordDocumentEditor` in `Word/WordDocument.hpp`: user-facing Word
  authoring through body cursors, paragraphs, runs, tables, images, styles,
  fields, sections, comments, revisions, and related features.
- `Excel::ExcelDocumentEditor` in `Excel/ExcelDocument.hpp`: workbook and
  worksheet editing, cells, ranges, formulas, styles, tables, charts, pivots,
  slicers, validation, and layout.
- `PowerPoint::PowerPointDocumentEditor` in
  `PowerPoint/PowerPointDocument.hpp`: slides, masters, layouts, shapes, text,
  media, tables, charts, transitions, animations, notes, and comments.
- `Packaging::WordDocument`, `Packaging::ExcelDocument`, and
  `Packaging::PowerPointDocument`: lower-level package lifecycle and parts.
  Each is re-exported as an alias in its format namespace.

Editors own a `std::shared_ptr` to their document and expose `GetDocument()` so
lower layers remain reachable. Packages are held in memory and saved explicitly.

Additional public subsystems:

- `ExyokiOffice::Tools`: validation, inspection, archiving, Flat OPC,
  conversion, diffing, media export, text extraction, and editing utilities.
- `ExyokiOffice::Xml`: namespace-aware XPath queries over package parts.
- `ExyokiOffice::Security`: signatures and policy-controlled external
  resources. External access is disabled unless the application installs a
  resolver and policy.

## API conventions that affect implementation

- Opening loads a package into memory; changes reach disk only through an
  explicit save. File saving is atomic by default.
- Factories such as `CreateNew`, `Open`, and many `Add...` operations return a
  null smart pointer on failure. Check results before dereferencing.
- Operations with useful failure detail return structured result objects;
  check `Succeeded()` or boolean conversion. Validation should reject input
  before mutating the document.
- Lengths use `MeasuringUnits`; angles use `MeasuringAngle`; colors use
  `ExyokiOffice::Color` or the format-specific color wrapper.
- Public scalar declarations use aliases from `StandardTypes.hpp`.
- Preserve unsupported content when editing whenever the existing API does so;
  do not silently discard package parts or markup outside a high-level model.
- Address XML by namespace URI and local name, never by a literal prefix.
  `w:`, `cp:`, `dc:` and friends are conventions of the producer, not part of
  the format: a document may bind the same namespace to any prefix and stays
  valid. Matching on the prefixed name reads nothing from such a file and can
  write a malformed one. Use `Xml::NamespaceResolver` (and
  `Xml::CorePropertiesXml` for `docProps/core.xml`). The `xml:` prefix is the
  one exception - it is bound by the XML specification itself.
- `.docx`, `.xlsx`, and `.pptx` are OPC ZIP packages. A package feature often
  requires coordinated content-type, part, relationship, and XML changes.

## Generated code and metadata boundaries

Do not enumerate or read these trees wholesale:

- `include/ExyokiOffice/DOM/**`
- `sources/DOM/**`
- `data/**`

Generated packaging files include:

- `include/ExyokiOffice/Packaging/GeneratedParts.hpp`
- `sources/Packaging/GeneratedParts.cpp`
- `sources/Packaging/OpenXmlPackageFactory.cpp`

Files with an `<auto-generated>` banner must not be edited by hand. Fix the
generator under `gen/` and regenerate. The JSON under `data/` is largely an
imported Open-XML-SDK metadata snapshot; avoid direct edits unless the task is
specifically about schema metadata and its provenance is understood.

Two overlays are owned by this repository and must survive metadata imports:

- `data/exyokioffice_part_paths.json`
- `data/exyokioffice_ambiguous_elements.json`

Locate a generated type with a targeted search, for example:

```powershell
rg -n "class Paragraph" include/ExyokiOffice/DOM/DocumentFormat/OpenXml/Wordprocessing.hpp
rg -l "SomeOpenXmlType" include/ExyokiOffice/DOM sources/DOM
```

The `generate_openxml` target writes generated files into tracked source
directories. Always inspect `git diff` after a build or regeneration and do not
mix unrelated generated churn into a change. Generator output must be
deterministic across MSVC, GCC, and Clang; never let unordered iteration or an
unstable tie decide emitted output.

Every generated type that is part of the public API must reach a consumer of
the shared library: element and part classes carry `EXYOKIOFFICE_EXPORT` on
the class, and an enum class carries it on `GetMetaEnum()` alone, because a
dllexport class over the non-exported `OpenXmlEnum` base raises C4275 in the
warnings-as-errors build. An unexported out-of-line member compiles and only
fails when something outside `sources/` links it, so
`tests/unit/GeneratedEnumMetadataTests.cpp` exercises that path deliberately.

An ordinary CMake build is the preferred way to regenerate, because the root
build runs `generate_openxml` before compiling the library. The generator can
also be invoked manually; it is built inside the preset's binary directory, so
the path follows the preset and, for multi-config generators, the configuration:

```powershell
build\vs\gen\Debug\OpenXmlGenerator.exe --data data --out-include include --out-source sources
build\ninja-clang-debug\gen\OpenXmlGenerator.exe --data data --out-include include --out-source sources
```

## Repository map and change boundaries

- `include/ExyokiOffice/`: stable public declarations.
- `sources/`: hand-written implementations.
- `include/ExyokiOffice/DOM/`, `sources/DOM/`: generated DOM.
- `gen/`: native OpenXML source generator.
- `data/`: imported schema/part metadata plus repository overlays.
- `tools/exyoki/`: CLI adapter over the public Tools module.
- `tools/mcp/`: the three Model Context Protocol servers and the static
  `exyoki-mcp-core` they share (protocol, sessions, workspace sandbox, tools).
- `docs/schemas/`: published machine-readable contracts. Generated, not
  hand-edited: `exyoki schema --output docs/schemas/<name>` regenerates the
  document-model schema, and `ToolsDocumentModelSchemaTests` fails when the
  checked-in file no longer matches what the library builds. The
  `mcp-*-tools.json` catalogs come from `<server> --print-tools`, and the
  `Mcp.Catalog.*` CTest entries fail on drift.
- `examples/`: runnable, CTest-registered examples.
- `tests/`: doctest test layers — `unit/`, `dom/`, `package/`, `word/`,
  `spreadsheet/`, `presentation/`, `tools/`, `cli/` (the `exyoki` command line,
  linking `exyoki_core` so the commands run in process), `mcp/`, `generator/`,
  `compat/` (one test per row of `docs/Compatibility.md`), and `corpus/`,
  with shared helpers in `support/`
  and a standalone installed-package smoke project in `install/`.
  Each layer registers CTest entries with labels; `ctest -L word` and
  `ctest -L excel-pivot` run one family or one area. The entries are
  independent processes and the suite is meant to be run with
  `ctest --parallel`, which the test presets and `WinBuild.ps1` already do.
- `corpus/`: real Word, Excel and PowerPoint packages saved by Microsoft
  Office, described by `corpus/manifest.json` and driven by the `tests/corpus`
  layer. These are the only fixtures the library did not produce itself, which
  makes them the evidence that a validator rule or a writer is wrong rather
  than merely self-consistent. See `corpus/README.md` before adding one. The
  manifest is read at configure time as well: the validation, content-model and
  round-trip sweeps are registered as one CTest entry per document, so adding a
  described fixture adds its entries with no CMake change.
- `tests/fuzz/`: fuzz targets, curated seeds, and committed crash regressions.
- `vcpkg/`: the consumer side of the vcpkg package — `test/`, a standalone
  project that installs the port and uses it through `find_package` only, and
  `Test-Port.ps1`, which drives that end to end. The port itself is not here: a
  vcpkg port is a directory in a vcpkg registry, so it is maintained in a clone
  of microsoft/vcpkg. `vcpkg/README.md` says where, what it configures, and how
  a release updates it.
- `docker/`: the container image the `create_install` workflow builds —
  `Dockerfile` (the distroless runtime image) and `entrypoint.c` (the dispatcher
  that prints the usage page or runs one of the four programs). `Dockerfile` is
  not built against the repository: the workflow stages a `rootfs/` tree next to
  it, because a distroless image has no shell to arrange anything with. Its
  `FROM` line and the `runs-on:` label of `linux-package` are one decision —
  the build's glibc must be no newer than the runtime base's. See
  `docs/tools/docker.md`.
- `sources/pugixml/`, `sources/zip/`, `3rdparty/`: vendored dependencies.
- `VERSION.txt`: the release number, stored once. The build, the generated
  `Version.hpp`, the Windows resource and both documentation PDFs read it; see
  `docs/ABI.md` for the policy and `RELEASE.md` for the release procedure.
  Never hard-code a version anywhere else.
- `.gitattributes`, `.editorconfig`: line endings, encoding and editor defaults.
  `.gitattributes` decides what Git stores and checks out, `.editorconfig` what
  an editor writes; the two must stay consistent. See "C++ style" below.

Keep vendored code unchanged unless a task explicitly targets it. Put public
API in `include/ExyokiOffice/` and implementation in `sources/`. Prefer a
focused private helper class with static methods for file-local helpers; do not
add an anonymous namespace merely to hold helper functions. The rule is about
functions, so two shapes stay as they are: a block that holds only constants,
and one that holds only a class - the class is already the pattern, and
wrapping it changes nothing. Keep each helper class focused: when one grows
past a few dozen members, split it by subject rather than letting it collect
everything the file needs.

## Build, test, and install

Use `CMakePresets.json`; do not configure into an ad-hoc build directory. The
configure presets are:

- `windows-vs`: Visual Studio 2026 (v18), multi-config.
- `windows-ninja-debug` / `windows-ninja-release`: Ninja, MSVC.
- `windows-ninja-clang-debug` / `windows-ninja-clang-release`: Ninja,
  `clang-cl`.
- `linux-ninja-debug` / `linux-ninja-release`: Ninja on Linux (hidden on
  Windows).
- `linux-ninja-clang-debug` / `linux-ninja-clang-release`: Ninja + Clang on
  Linux.
- `windows-vs-asan`, `windows-ninja-asan`, `linux-ninja-asan`: sanitizer
  builds with their own binary directories.
- `windows-ninja-clang-fuzz`, `windows-ninja-fuzz`: libFuzzer builds.

Build and test presets share the configure preset's name (`debug`, `release`,
`ninja-debug`, `ninja-release`, `ninja-clang-debug`, `ninja-clang-release`,
`asan-debug`, `asan-release`, `ninja-asan`, `ninja-clang-fuzz`, `ninja-fuzz`,
`linux-ninja-*`). All presets except the fuzz ones inherit
`EXYOKIOFFICE_BUILD_EXAMPLE_WORD`, `EXYOKIOFFICE_BUILD_UNIT_TESTS`, and
`EXYOKIOFFICE_BUILD_TOOL` set to `ON`, so the examples, every doctest test
layer, and `exyoki` are built and registered with CTest without extra cache
flags. The fuzz presets deliberately turn the examples, tests, and tool off and
enable `EXYOKIOFFICE_BUILD_FUZZERS` instead.

On Windows, use the repository script. It discovers the newest Visual Studio
with `vswhere`, enters its developer environment, and uses the `windows-vs`
preset:

```powershell
.\WinBuild.ps1 -QuickTest
.\WinBuild.ps1 -Test
.\WinBuild.ps1 -Clean -Test
.\WinBuild.ps1 -Install
.\WinBuild.ps1 -Install -InstallPrefix C:\ExyokiOffice
.\WinBuild.ps1 -Configuration Debug -Test
```

`-QuickTest` skips the areas labelled `slow` — the five that read every package
under `corpus/` or compile every content model in the schema import. They are
most of the running time and everything else is a few seconds, so `-QuickTest`
is the edit-test loop and `-Test` is what a change has to pass before it is
reported as done.

`-Configuration` defaults to `RelWithDebInfo`, which runs the suite in about two
minutes against eleven in `Debug`. Reach for `Debug` when you want MSVC's
iterator debugging or a debugger view that has not been optimized; there are no
runtime assertions in the hand-written sources, so nothing else changes.

`-Sanitizer Address` selects a separate ASan build tree. `-Jobs` controls both
CMake parallelism and MSVC `/MP`; the script also sets MSBuild's
`MultiToolTask`, `EnforceProcessCountAcrossBuilds`, and `CL_MPCount` so the two
limits do not multiply. Clean builds of generated translation units are slow
and memory-heavy; allow at least ten minutes and throttle jobs on
memory-limited machines. MSVC builds use `/bigobj` for the same reason.

Sanitizers are driven by `EXYOKIOFFICE_SANITIZER` (`cmake/Sanitizers.cmake`;
values `none`, `address`, `undefined`, `address+undefined`, `thread`, with MSVC
supporting `address` only). The flags apply to the whole build, so a sanitizer
configuration always uses its own binary directory, and instrumented Windows
binaries only run from a Visual Studio developer environment because the ASan
runtime DLL lives in the MSVC `bin` directory.

The equivalent commands, once the Visual Studio developer environment is
initialized, are:

```powershell
$jobs = [Environment]::ProcessorCount
$env:UseMultiToolTask = 'true'
$env:EnforceProcessCountAcrossBuilds = 'true'
$env:CL_MPCount = $jobs
cmake --preset windows-vs -DEXYOKIOFFICE_BUILD_JOBS=$jobs
cmake --build --preset debug --parallel $jobs
ctest --preset debug
```

If `cmake` does not resolve on `PATH`, run from a shell that initializes it
(a Visual Studio developer shell) rather than hardcoding an install path.

On Linux:

```bash
cmake --preset linux-ninja-debug
cmake --build --preset linux-ninja-debug
ctest --preset linux-ninja-debug
```

When a restricted Windows sandbox reports `MSB4018`, `FileTracker`, and
`E_ACCESSDENIED` during compiler detection, rerun the same build with sandbox
escalation. Do not switch generators or override `CMAKE_CXX_COMPILER`.

At minimum, build affected targets and run relevant unit tests via
`ctest --preset <name>`. For document behavior, add or run a focused round-trip
reproduction and verify that the resulting package still opens. Do not commit
build products, generated example documents, or IDE-local files.

CI is manual apart from one job. `smoke.yml` — a single Linux GCC release build
and the test suite — runs on push to `master` and on pull requests; every other
workflow in `.github/workflows` is `workflow_dispatch` and has to be started by
hand. `ci.yml` builds and tests on MSVC, GCC, Clang, and ASan/UBSan; see
`docs/ci.md`. Do not give any of the others an automatic trigger: a full run
regenerates and compiles the whole DOM on three platforms, which is precisely
what the one smoke job exists to keep off every push.

## Linting and fuzzing

```powershell
.\WinLint.ps1 -Check
.\WinLint.ps1 -Changed -Tool Format
.\WinFuzz.ps1 -Target flatopc -Seconds 300
.\WinFuzz.ps1 -Replay
```

`WinLint.ps1` discovers Visual Studio the same way `WinBuild.ps1` does and uses
the LLVM tools bundled there (`VC\Tools\Llvm\x64\bin`), so no separate clang
installation is needed. `-Tool Format|Tidy|All` selects the tools. Its file set
is `git ls-files` minus the generated and vendored trees — the same exclusions
as `.github/workflows/clang-format.yml` — minus any file carrying the
`<auto-generated>` banner; the nested `.clang-tidy` files disable checks in
those trees, but clang-format has no per-directory opt-out, so the path filter
is still required. clang-tidy needs a compilation database, so the script
configures `windows-ninja-clang-debug` into a dedicated `build/tidy` directory
and builds only `generate_openxml`, leaving existing build trees untouched;
`-SkipConfigure` reuses that directory. Full clang-tidy can take more than ten
minutes.

`WinFuzz.ps1` keeps its growing working corpus under
`build/<preset>/fuzz-corpus`; `-Minimize` deliberately replaces the committed
seed subset under `tests/fuzz/corpus`. Crash artifacts belong under
`tests/fuzz/crashes` and become ordinary regression-test inputs.

## Coverage

```powershell
.\WinCoverage.ps1
.\WinCoverage.ps1 -Label word, spreadsheet -LabelExclude slow
.\WinCoverage.ps1 -AllTests
.\WinCoverage.ps1 -AllTests -Mcdc
.\WinCoverage.ps1 -Monolith
```

`WinCoverage.ps1` measures which lines and regions of the library a CTest
selection reaches, using LLVM source-based coverage through the clang-cl
toolchain bundled with Visual Studio. It builds the
`windows-ninja-clang-coverage` preset into its own directory, runs the selected
labels with the raw profiles redirected under `build/coverage`, and writes a
summary table and an HTML report there. `-Mcdc` adds MC/DC in a second tree;
`-Monolith` measures against one executable holding the library and every test
layer, the only mode that attributes header code instantiated by tests.
Nothing in CI runs it. See `docs/coverage.md` for what each report includes,
why the coverage build keeps the library shared, and the OpenCppCoverage
alternative for an MSVC build.

## Documentation responsibilities

- `README.md` is the concise project and consumer entry point.
- `docs/README.md` indexes the complete user manual.
- `AGENTS.md` is the source-code navigation and agent workflow guide.
- `CLAUDE.md` only adds Claude Code harness specifics and points here; keep
  project knowledge in this file rather than duplicating it there.
- `CONTRIBUTING.md` is the contributor workflow and review checklist.
- `llms.txt` is the compact AI documentation index; `llms-full.txt` is the
  self-contained AI context.
- `MARKETING.md` is the plan for making the library known; it carries no
  project knowledge a code change would need to keep in step.
- `docs/Compatibility.md` is authoritative for feature support.
- `docs/Threading.md` defines the runtime concurrency contract.

When adding a documentation chapter, link it from `docs/README.md` and add it
to `docs/_pdf/chapters.txt`. Keep examples compilable, use relative file links,
and update compatibility claims in the same change as the implementation.

## C++ style

Preserve the C++20 baseline and follow `.clang-format`. Write normally expanded
C++; never compress several statements or complete functions onto one line.
Use braces for every `if`, `else`, `for`, `while`, and similar control-flow body.

Text files are LF on every platform, including Windows, and UTF-8 without a BOM.
`.gitattributes` enforces this independently of any global `core.autocrlf`, so
never rewrite a file's line endings as part of an unrelated change: it turns a
small diff into a whole-file rewrite and buries the actual edit. When writing
files, emit `\n`, not `\r\n` — this is what the DOM generator already does.

Two exemptions matter when touching those trees. Batch files (`*.cmd`, `*.bat`)
keep CRLF. Everything under `tests/fuzz/corpus/` and `tests/fuzz/crashes/` is
marked `-text` and must stay byte-exact — do not trim trailing whitespace or add
a final newline there, and never stage a content change to those paths that came
from a renormalization rather than from a deliberate new seed.

The MSVC build does not pass `/utf-8`. Without a byte order mark MSVC therefore
reads sources in the system ANSI code page, so a narrow string literal holding
non-ASCII text compiles to locale-dependent bytes rather than to UTF-8. Keep
literals ASCII, or escape the bytes explicitly; do not add a BOM to compensate.
