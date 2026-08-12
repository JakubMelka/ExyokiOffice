# Changelog

All notable changes to ExyokiOffice are documented in this file. The project
uses [Semantic Versioning](https://semver.org/); the released version number
itself lives in `VERSION.txt` in the repository root.

Entries are grouped under `Added`, `Changed`, `Deprecated`, `Removed`, `Fixed`
and `Security`, and describe user-visible changes rather than commits.

## [Unreleased]

### Added

- Code coverage measurement: the `EXYOKIOFFICE_COVERAGE` build option, the
  `windows-ninja-clang-coverage` presets, and `WinCoverage.ps1`, which runs a
  chosen set of CTest labels against an instrumented clang-cl build and writes
  a summary table and an HTML report under `build/coverage`. Documented in
  [docs/coverage.md](docs/coverage.md). Nothing in CI runs it.
- MC/DC coverage as part of the same tooling: the `EXYOKIOFFICE_COVERAGE_MCDC`
  option, the `windows-ninja-clang-coverage-mcdc` presets, and the `-Mcdc`
  switch on `WinCoverage.ps1`, which builds a tree of its own and reports into
  `build/coverage-mcdc`.
- A seam-free coverage mode: `EXYOKIOFFICE_TEST_MONOLITH` builds
  `ExyokiOfficeMonolithTests`, one executable holding the library's sources
  and every test layer, and `WinCoverage.ps1 -Monolith` measures against it.
  One binary means no cross-module attribution loss, so inline code in
  headers is counted wherever a test instantiated it.

### Fixed

- The `ROUND` family survives every extreme digit count. A digit count whose
  scale factor overflows (`=ROUNDDOWN(2.9,400)`), and equally one where only
  the scaled value overflows (`=ROUNDDOWN(2.9,308)`), returns the value
  unchanged instead of `#NUM!`; a hugely negative digit count
  (`=ROUNDDOWN(2.9,-400)`, `=TRUNC(2.9,-400)`) rounds to 0 as Excel does,
  and `ROUNDUP` reports the unrepresentable magnitude as `#NUM!`.
- Aggregates report overflow as `#NUM!` instead of returning an infinity:
  `SUM`, `PRODUCT`, `AVERAGE`, `AVERAGEA`, `MEDIAN`, `SUMPRODUCT`, `SUMIF`,
  `AVERAGEIF`, `SUMIFS`, `AVERAGEIFS`, the `STDEV`/`VAR` family, and
  `DEGREES`.
- `LOG` with base 1 answers `#DIV/0!` - Excel's division by `ln(1) = 0` -
  instead of `#NUM!`.
- `SIN`, `COS`, and `TAN` refuse arguments of magnitude 2^27 and above with
  `#NUM!`, matching Excel's domain limit.
- `AVERAGEA` follows Excel's direct-argument coercion: numeric text passed
  directly contributes its value and unreadable direct text is `#VALUE!`,
  while text inside ranges and arrays still counts as zero.
- `AND`, `OR`, and `XOR` ignore text inside array constants the way they
  ignore text inside ranges instead of failing with `#VALUE!`.
- `RANDBETWEEN` with bounds beyond 2^53 no longer casts them into an integer
  distribution (undefined behavior); such ranges draw in the real domain.
- The DLL copies placed next to the test, tool, and example executables are now
  files that depend on the library rather than POST_BUILD steps of the copying
  targets. Under Ninja, a library change that left the export surface alone did
  not relink those targets, and an incremental test run then silently exercised
  the previous library. Each output directory gets one copy rule - shared
  directories no longer race - every executable depends on its directory's
  copy, so a target-scoped build still places the DLL, and the copied file
  itself is the tracked output, so a deleted copy is restored.

## [1.0.0] - 2026-08-08

First public release. Everything below is new, because there is no earlier
release to compare against; later versions will list only what changed.

ExyokiOffice is a C++20 shared library that creates, opens, edits and saves
Office Open XML packages — `.docx`, `.xlsx` and `.pptx` — by writing the
ZIP/XML package directly. Microsoft Office and .NET are needed neither to build
it nor to run it, and it pulls in no external dependency: every third-party
component it uses is vendored in the tree, and all of them are permissively
licensed — see [THIRD-PARTY-LICENSES.md](THIRD-PARTY-LICENSES.md). What each
format and feature is supported to do — create, edit or merely preserve — is
tabulated in
[docs/Compatibility.md](docs/Compatibility.md), which is authoritative; the
summary below says what exists, not how far it goes.

### Added

#### Word

- `Word::WordDocumentEditor` — documents and templates, snapshots and
  transactions, document properties and themes.
- Body cursors over paragraphs and runs: text, character and paragraph
  formatting, breaks, find and replace.
- Styles and numbering — `StyleManager`, latent styles, simple lists and
  multi-level numbering; headings.
- Tables with formatting, merged cells and nesting; inline and floating images
  with wrapping, positioning, cropping and alt text; external and internal
  hyperlinks with bookmark ranges.
- Fields and tables of contents, sections and page setup, headers and footers.
- Footnotes, endnotes, comments — including threaded replies and their
  resolution state — and structured document tags.
- Tracked revisions, document comparison, `InsertDocument` merging and mail
  merge; editing restrictions and password verifiers.

#### Excel

- `Excel::ExcelDocumentEditor` — workbooks, worksheets, copying sheets between
  workbooks, snapshots, properties, themes, VBA preservation, workbook and
  sheet protection.
- Cells and ranges: values, shared strings, structural row and column edits,
  merged cells, row and column dimensions and views.
- A formula engine that parses, evaluates and recalculates, with named ranges
  in workbook and worksheet scope.
- Styles and number formats through `StyleRepository`; worksheet tables and
  auto-filters; charts with cross-sheet data sources.
- Pivot tables — caches, records, definitions, aggregation and refresh — and
  slicers over both pivot tables and worksheet tables.
- Data validation and conditional formatting; hyperlinks, images and threaded
  comments; page setup, margins, print areas, headers and footers.

#### PowerPoint

- `PowerPoint::PowerPointDocumentEditor` — presentations, slide size, modify
  protection, slide management, `SlideBuilder`, sections and custom shows.
- The shape tree: geometry, transforms, fills, outlines and effects; text
  frames, runs and bullets.
- Embedded and linked pictures, audio and video, DrawingML tables, and charts
  with cached data and embedded workbooks.
- Masters, layouts, themes and placeholder inheritance; slide transitions,
  animation sequences with effects and triggers; speaker notes and slide
  comments.

#### Typed DOM and packaging

- `DocumentFormat::OpenXml::…` — generated element classes for
  WordprocessingML, SpreadsheetML and PresentationML, produced by
  `OpenXmlGenerator` from the JSON metadata under `data/` as part of the build.
- `Packaging::…` — OPC parts, relationships, content types and document
  lifecycle, with document properties resolved by namespace URI rather than by
  conventional prefix.
- `OpenXmlPackageLimits` bounds what a package may cost to open — entry count,
  per-entry size, compression ratio, running totals and XML nesting depth —
  and can be installed process-wide with
  `OpenXmlPackage::SetDefaultPackageLimits`.
- Digital signature inspection and verification, with all cryptography behind
  `ICryptoProvider`, and an external-resource policy that decides what a
  document may reach for.

#### Validation

- `OpenXmlPackageValidator` checks a package against the OPC rules, the schema,
  the schematron constraints and the DOM content models, reporting each
  diagnostic with the positional path of the element it is about.
- Content models are decided by an automaton compiled from the particle tree,
  which is linear in the number of children; the recursive matcher it replaced
  stays in the build as its oracle and as
  `--cross-check-content-model`/`CrossCheckContentModel`, a second opinion that
  reports a disagreement between the two as a defect in this library.
- The `corpus` test layer runs fifteen Word, Excel and PowerPoint packages
  saved by Microsoft Office through validation and an open-save-open matrix.
  They are the only inputs the library never produced, and they are what the
  validator's agreement with Office is measured against.

#### Tools and front ends

- `ExyokiOffice::Tools` — validation, inspection, `Stat`, `Diff`, `Detect`,
  `Redact`, `Unpack`/`Pack`, `Query`, `Extract`, text search and replace,
  Word splitting, template filling, workbook recalculation and document
  comparison. Every entry point has an overload taking a document already open
  in its family editor, so an application holding an editor never writes to a
  temporary file to call one.
- `ExyokiOffice::Xml` — namespace-precise XPath 1.0 over any part.
- Conversion between packages and an `exyokioffice-document` JSON envelope,
  plus CSV import and export for worksheets. The envelope has a published JSON
  Schema (draft 07,
  [docs/schemas/exyokioffice-document-v1.schema.json](docs/schemas/exyokioffice-document-v1.schema.json)),
  and `exyoki schema --check` validates a document against it.
- `exyoki`, the command line: inspect, validate, convert, unpack and repack,
  query, redact, compare, recalculate, fill, search and replace, read and write
  properties. Reports come in several `--format`s with a stable exit-code
  table, `exyoki commands` describes the whole interface as data, and
  `exyoki completions` generates a shell completion script from the live
  parser.
- Three Model Context Protocol servers — `exyoki-mcp-word`, `exyoki-mcp-excel`
  and `exyoki-mcp-power-point` — hand the editors to an AI agent as typed tools
  over stdio. Documents stay open between calls behind a `documentId`, every
  result uses one envelope with machine-readable error codes and repair hints,
  `batch` applies several edits as one transaction and `undo` steps back
  through them. Every path is confined to a `--workspace` root, `--read-only`
  publishes a mutating-free catalog, and there is no code-execution tool.

#### Building and consuming

- Windows (MSVC and `clang-cl`) and Linux (GCC and Clang), C++20, CMake 3.25 or
  newer, driven through the presets in `CMakePresets.json`.
- Installs as a CMake package: `find_package(ExyokiOffice 1.0 CONFIG REQUIRED)`
  and link `ExyokiOffice::ExyokiOffice`. `tests/install` is the smoke test that
  the installed package really configures, links and runs.
- A vcpkg port, `exyokioffice`, so the library can be installed with
  `vcpkg install exyokioffice` and consumed with the same `find_package` call an
  installed prefix uses. It has no dependencies to resolve — everything
  third-party is vendored — and it offers `tools` and `mcp` as optional
  features, neither on by default, so installing the library does not build the
  `exyoki` utility or the three MCP servers. A vcpkg port belongs to a vcpkg
  registry rather than to the project it packages, so the port sources are
  maintained in a clone of microsoft/vcpkg; what this repository carries is the
  consumer side of the package, in [vcpkg/](vcpkg/README.md): a standalone
  project that reaches only for the installed headers and the exported target,
  and the `Test-Port.ps1` script that installs the port and runs that project
  against it, on any triplet and either from the released tag, the tip of
  master, or the working tree.
- A container image, built by the `create_install` workflow next to the zip
  archives and uploaded as `ExyokiOffice-<version>-docker-amd64`, a gzipped
  `docker save` tarball. It is distroless — the shared library, `exyoki`, the
  three MCP servers, the third-party license notices, and nothing else, no
  shell and no package manager. Running it with no arguments prints how to use
  it; naming `exyoki`, `word`, `excel`, or `powerpoint` runs that program. The
  MCP servers can therefore be registered with `"command": "docker"` without
  installing anything. The image carries the standard OCI labels — version,
  revision, creation time, license, source and the rest — which is how it can be
  identified at all, having no shell to ask. A separate `publish_docker`
  workflow pushes the image a release carries to `ghcr.io`, so it can also be
  pulled instead of loaded from the tarball; it builds nothing and publishes
  exactly the artifact the release offers. See the [container
  image](docs/tools/docker.md) chapter.
- The build honors `BUILD_SHARED_LIBS`, so a static library is what the static
  triplet of a package manager installs, and `EXYOKIOFFICE_RUN_GENERATOR`, on by
  default, controls whether it reruns `OpenXmlGenerator`. The generated sources
  are committed, so turning that off builds from them as they are — which is
  what a packaging build needs, since it must not write into the source tree it
  was handed, and what makes cross compilation possible at all.
- The versioning and ABI policy is written down in [docs/ABI.md](docs/ABI.md):
  the ABI identity is `MAJOR.MINOR`, derived from `VERSION.txt`, and only a
  patch release promises an unchanged ABI.
- The manual under [docs/](docs/README.md) is also rendered as a PDF, alongside
  a Doxygen API reference, by the `docs-pdf` and `doxygen-pdf` workflows.

### Security

- The security policy is [SECURITY.md](SECURITY.md): vulnerabilities are
  reported privately through GitHub's *Report a vulnerability*, never as a
  public issue, and a confirmed critical report — exploitable memory corruption
  reachable by loading a document, or a signature verified over content it does
  not cover — is fixed within one week. It also states the security model that
  the manual only describes per subsystem, so it is clear which defects are
  vulnerabilities and which are the application's own configuration.
- Every front end opens packages under `OpenXmlPackageLimits::Recommended()`
  rather than the library's unlimited default: `exyoki`, the three MCP servers
  and every `ExyokiOffice::Tools` entry point. All three are routinely pointed
  at documents from outside the machine they run on, where a decompression bomb
  or deeply nested XML would otherwise exhaust memory or the stack.
  `--package-limits unlimited` restores the unbounded behaviour for forensic
  work on a damaged package, and warns at start-up when it does. The library
  itself still starts unlimited, so an embedding application chooses its own
  policy — see [SECURITY.md](SECURITY.md).
- `Tools::Unpack` and `Tools::Pack` treat archive entry names as untrusted:
  extraction refuses a `..` path component and escapes what its own predicate
  rejects, and a rename manifest cannot turn `exyoki pack` into a generator of
  traversal archives.
- Signature canonicalization stops at 512 levels of element nesting instead of
  recursing, and a JSON-RPC line longer than 16 MiB is answered with a parse
  error rather than read into memory unbounded.
- The library is fuzzed with libFuzzer under AddressSanitizer, and every input
  that ever crashed a target is kept under `tests/fuzz/crashes/` and replayed
  by the ordinary unit test build. See [docs/fuzzing.md](docs/fuzzing.md).

### Known limitations

- Saving a signed package breaks the digests over its *content* parts, because
  those are re-serialized from their trees rather than written as stored. This
  is what `SignatureSavePolicy` warns about; the signature part itself survives
  a save unchanged.
