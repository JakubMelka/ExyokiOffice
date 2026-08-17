# Changelog

All notable changes to ExyokiOffice are documented in this file. The project
uses [Semantic Versioning](https://semver.org/); the released version number
itself lives in `VERSION.txt` in the repository root.

Entries are grouped under `Added`, `Changed`, `Deprecated`, `Removed`, `Fixed`
and `Security`, and describe user-visible changes rather than commits. One
entry is one sentence; the reasoning behind a change belongs in its commit and
in [docs/](docs/README.md).

## [Unreleased]

### Added

- Code coverage: the `EXYOKIOFFICE_COVERAGE` option, the
  `windows-ninja-clang-coverage` presets and `WinCoverage.ps1`, which reports
  into `build/coverage`. See [docs/coverage.md](docs/coverage.md). Not run in CI.
- MC/DC coverage: `EXYOKIOFFICE_COVERAGE_MCDC`, the
  `windows-ninja-clang-coverage-mcdc` presets and `WinCoverage.ps1 -Mcdc`.
- `EXYOKIOFFICE_TEST_MONOLITH` builds `ExyokiOfficeMonolithTests`, a single
  executable measured by `WinCoverage.ps1 -Monolith` with no cross-module
  attribution loss.
- Every `Open` overload of the three document families and their editors takes
  an optional `Packaging::OpenError*`, which distinguishes a missing file, an
  unopenable one, an unreadable package, an exceeded limit, a strict-validation
  failure, a cancellation, and a document of the wrong family.
- `Security::ICryptoProvider::VerifyDataWithChain` receives the whole embedded
  certificate chain; its default implementation forwards to `VerifyData`.
- `Tools::ToFlatOpcOptions::Limits`, so `Tools::ConvertToFlatOpc` and
  `exyoki flatopc` read an archive under the library's ZIP ceilings.
- `Tools::RedactResult::PartsRemoved`, reported as `partsRemoved` by
  `exyoki redact` and the MCP tool.
- `Security::SignatureResult::UncoveredParts` lists the package parts a
  signature says nothing about.
- `Security::VerifySignaturesOptions::AllowSha1`, defaulting to false; SHA-1
  digests and RSA-SHA1 signature values are otherwise reported as invalid.
- `ExyokiOffice::RegexPattern`, an expression plus its options.
- Image detection covers TIFF, EMF and placeable WMF, and reads JPEG resolution
  from Exif as well as JFIF.
- `AttachOwningPart` / `OwningPart` on `Word::Paragraph`, `Hyperlink`, `Table`,
  `ContentControl`, `Note`, `Comment` and `HeaderFooterContent`.
- `CODE_OF_CONDUCT.md`, issue and pull request templates, and a `Smoke` workflow
  that builds and tests on every push and pull request.

### Security

- Packages start with `OpenXmlPackageLimits::Recommended()` instead of no limits
  at all; `Unlimited()` restores the previous behaviour.
- `Tools::ConvertToFlatOpc` enforces those limits too, instead of allocating the
  declared uncompressed size of every entry.
- The regex subject limit is enforced in `Tools::SearchDocumentText` and
  `ReplaceDocumentText`, not only in the Word paragraph API.
- Regular expressions run over at most 32768 bytes
  (`RegexPattern::MaximumSubjectLength`).
- An unreadable archive entry is reported as
  `ValidationErrorId::OpcEntryUnreadable` instead of being skipped in silence;
  the load still succeeds.
- `[Content_Types].xml` is matched case-sensitively, so a package cannot carry
  two of them with different meanings.
- An MCP message nesting arrays or objects deeper than 128 levels is refused
  before the JSON is parsed.
- Word identifier allocation saturates instead of reaching signed overflow.
- An RSA-SHA1 signature value is refused even with no crypto provider present.
- Parsing an EMF picture frame no longer subtracts two untrusted `Int32` fields
  into an `Int32`.
- Signature verification reads the covered parts from a `Manifest` inside a
  `dsig:Object` whose digest has verified; a signature covering no part or
  relationship set, and a signature part repeating an element `Id`, are reported
  `Invalid` with `SignatureMalformed`. See
  [docs/Signatures.md](docs/Signatures.md).
- Formula expressions nested deeper than 128 levels are rejected with a
  diagnostic, and the expression tree is released and walked iteratively.
- Deeply nested XML no longer reaches the stack: DOM validation,
  `Xml::InnerText`, `XmlQuery` descendant walks, deep-copy namespace collection
  and the package limit check keep their own stack, and markup compatibility
  processing refuses to descend past 512 levels with the new
  `ValidationErrorId::NestingTooDeep`.
- `Tools::RedactDocument` scrubs what it claimed to: tracked changes are
  accepted in every story part, a deleted paragraph mark merges its paragraph,
  deleted rows go with their cells, `w:rPrChange` records are dropped, a
  character style that hides text counts as hidden, `w:vanish` and
  `w:specVanish` are read as on/off values, `xl/persons` and legacy
  `ppt/comments/*` go with the comments they belong to, and the metadata pass
  clears descriptive properties, last-printed time, attached template name,
  `w:rsid*`, `customXml` and `docProps/thumbnail`. What it still does not reach
  is stated in the API and in [docs/tools/exyoki.md](docs/tools/exyoki.md).
- `PowerPointDocument::Open` applies `OpcValidationMode` and
  `MaxCharactersInPart`, which it used to ignore.
- A ZIP entry with an empty name no longer causes a one-byte heap under-read.
- The largest accepted `spinCount` dropped from ten million to one million.

### Removed

- **Source-incompatible with 1.0.0.** The `std::regex` overloads of
  `Word::Paragraph::FindAllRegex` and `ReplaceAllRegex` are gone; replace
  `paragraph->FindAllRegex(std::regex(text, std::regex::icase))` with
  `paragraph->FindAllRegex(RegexPattern{text, true})`, or with
  `RegexPattern::Literal(text)` where the needle is not an expression.
  [docs/ABI.md](docs/ABI.md) reserves a source break for a security fix that
  cannot be made otherwise, which this is. **The next release has to be 2.0.0.**

### Changed

- `Word::Paragraph::FindAllRegex` and `ReplaceAllRegex` take a `RegexPattern`
  instead of a compiled `std::regex`, and `<regex>` is gone from the public
  headers; `PowerPointDocument.hpp` no longer includes `Presentation.hpp`.
- A paragraph's text is one thing throughout the Word API: `Runs`, `PlainText`,
  `Find`, `GetText`, `ReplaceText` and the regex overloads all read the runs in
  document order, including those inside hyperlinks, tracked insertions, content
  controls, smart tags and simple fields, and exclude text a reader does not see.
- A replacement rewrites the range it was given and nothing else, keeping page
  breaks and non-breaking hyphens elsewhere in the run as elements.
- Text written through the Word API carries `xml:space="preserve"` by default;
  passing `false` writes it without.
- `EXYOKIOFFICE_RUN_GENERATOR` defaults to on only for a developer build of this
  repository, since the generator writes into the source tree.
- `EXYOKIOFFICE_WARNINGS_AS_ERRORS` defaults to `OFF`; CI and `WinBuild.ps1`
  pass `ON` explicitly.
- Configuring stops with a plain message below GCC 13, Clang 17 or MSVC 19.30,
  and reports macOS as untested; README.md carries the platform table.
- `SignatureResult::IsValid()` is documented as meaning cryptographic
  consistency with the certificate the signature carries, not trust in it.
- A fuzz build compiles with `MINIZ_DISABLE_ZIP_READER_CRC32_CHECKS` so the
  raw-package target reaches the loader. See [docs/fuzzing.md](docs/fuzzing.md).

### Fixed

- Flat OPC conversion tests path traversal one component at a time, so a part
  named `notes..xml` is no longer dropped.
- Relationships created outside the main document part are recorded in that
  part; a paragraph with no part returns nullptr from `AddHyperlink`.
- An inline content control inherits the part its paragraph lives in.
- `Paragraphs()` and `Tables()` reach into a block-level structured document tag.
- A drawing identifier is allocated across every story of a Word document.
- A picture added without an explicit size is scaled to the text width.
- PowerPoint text extraction descends into group shapes and reads tables.
- A leading empty cell keeps its column when a PowerPoint table is extracted.
- Searching a Word document compiles the expression once instead of once per
  paragraph.
- A path outside ASCII opens on Windows; the bundled ZIP layer is handed UTF-8
  rather than the active code page.
- Numbers are read and written locale-independently through
  `std::from_chars`/`std::to_chars`, and the number-format renderer no longer
  truncates at a fixed 64-byte buffer.
- A number converted to text is spelled as a spreadsheet spells it: fifteen
  significant digits, uppercase `E` with a signed two-digit exponent, and
  positional notation between 1E-04 and 1E+21; comparisons use the same
  precision, so `=0.1+0.2=0.3` is TRUE.
- `ExcelCellValue::Number` maps an infinity or a NaN to `#NUM!` instead of
  writing `inf` or `nan` into `<v>`.
- The 1900 date system keeps its imaginary leap day: `DATE(1900,2,29)`,
  `DATE(1900,1,60)` and `DATE(1900,3,0)` are all serial 60, `DAY(60)` is 29, and
  serial 0 is 0 January 1900.
- Formula function fixes:
  - the `ROUND` family survives every extreme digit count, returning the value
    unchanged on overflow, rounding to 0 for hugely negative counts, and
    reporting an unrepresentable `ROUNDUP` magnitude as `#NUM!`;
  - `SUM`, `PRODUCT`, `AVERAGE`, `AVERAGEA`, `MEDIAN`, `SUMPRODUCT`, `SUMIF`,
    `AVERAGEIF`, `SUMIFS`, `AVERAGEIFS`, the `STDEV`/`VAR` family and `DEGREES`
    report overflow as `#NUM!` instead of returning an infinity;
  - `LOG` with base 1 answers `#DIV/0!`;
  - `SIN`, `COS` and `TAN` refuse arguments of magnitude 2^27 and above;
  - `AVERAGEA` follows Excel's direct-argument coercion;
  - `AND`, `OR` and `XOR` ignore text inside array constants;
  - `RANDBETWEEN` draws in the real domain for bounds beyond 2^53.
- The formula evaluator's depth counter is balanced by a scope guard.
- `Tools::Extract` and `Tools::RedactDocument` are `[[nodiscard]]`.
- The built-in SHA implementation pads in place instead of copying the whole
  message, halving the peak memory of digesting a large part.
- A static build is compiled position-independent.
- A fuzz build links again: CMake skips the examples whenever
  `EXYOKIOFFICE_BUILD_FUZZERS` is on.
- `EXYOKIOFFICE_RUN_GENERATOR` defaults to off when cross compiling.
- CMake prints a count instead of several hundred generated file paths.
- The DLL copies next to the test, tool and example executables are tracked
  files those targets depend on rather than POST_BUILD steps, so an incremental
  Ninja run no longer exercises the previous library.

## [1.0.0] - 2026-08-08

First public release. Everything below is new, because there is no earlier
release to compare against; later versions will list only what changed.

ExyokiOffice is a C++20 shared library that creates, opens, edits and saves
Office Open XML packages - `.docx`, `.xlsx` and `.pptx` - by writing the
ZIP/XML package directly. It needs neither Microsoft Office nor .NET, and
vendors every third-party component it uses; see
[THIRD-PARTY-LICENSES.md](THIRD-PARTY-LICENSES.md). What each format and
feature is supported to do is tabulated in
[docs/Compatibility.md](docs/Compatibility.md), which is authoritative; the
summary below says what exists, not how far it goes.

### Added

#### Word

- `Word::WordDocumentEditor`: documents and templates, snapshots and
  transactions, document properties and themes.
- Body cursors over paragraphs and runs: text, character and paragraph
  formatting, breaks, find and replace.
- Styles and numbering: `StyleManager`, latent styles, simple lists,
  multi-level numbering and headings.
- Tables with formatting, merged cells and nesting.
- Inline and floating images with wrapping, positioning, cropping and alt text.
- External and internal hyperlinks with bookmark ranges.
- Fields and tables of contents, sections and page setup, headers and footers.
- Footnotes, endnotes, comments with threaded replies and resolution state, and
  structured document tags.
- Tracked revisions, document comparison, `InsertDocument` merging and mail
  merge; editing restrictions and password verifiers.

#### Excel

- `Excel::ExcelDocumentEditor`: workbooks, worksheets, copying sheets between
  workbooks, snapshots, properties, themes, VBA preservation, workbook and
  sheet protection.
- Cells and ranges: values, shared strings, structural row and column edits,
  merged cells, row and column dimensions and views.
- A formula engine that parses, evaluates and recalculates, with named ranges in
  workbook and worksheet scope.
- Styles and number formats through `StyleRepository`; worksheet tables and
  auto-filters; charts with cross-sheet data sources.
- Pivot tables with caches, records, definitions, aggregation and refresh, and
  slicers over both pivot tables and worksheet tables.
- Data validation and conditional formatting; hyperlinks, images and threaded
  comments; page setup, margins, print areas, headers and footers.

#### PowerPoint

- `PowerPoint::PowerPointDocumentEditor`: presentations, slide size, modify
  protection, slide management, `SlideBuilder`, sections and custom shows.
- The shape tree: geometry, transforms, fills, outlines and effects; text
  frames, runs and bullets.
- Embedded and linked pictures, audio and video, DrawingML tables, and charts
  with cached data and embedded workbooks.
- Masters, layouts, themes and placeholder inheritance; slide transitions,
  animation sequences with effects and triggers; speaker notes and slide
  comments.

#### Typed DOM and packaging

- `DocumentFormat::OpenXml::…`, generated element classes for WordprocessingML,
  SpreadsheetML and PresentationML, produced by `OpenXmlGenerator` from the JSON
  metadata under `data/` as part of the build.
- `Packaging::…`, OPC parts, relationships, content types and document
  lifecycle, with document properties resolved by namespace URI.
- `OpenXmlPackageLimits` bounds what a package may cost to open and can be
  installed process-wide with `OpenXmlPackage::SetDefaultPackageLimits`.
- Digital signature inspection and verification with all cryptography behind
  `ICryptoProvider`, and an external-resource policy.

#### Validation

- `OpenXmlPackageValidator` checks a package against the OPC rules, the schema,
  the schematron constraints and the DOM content models, reporting each
  diagnostic with the positional path of the element it is about.
- Content models are decided by an automaton compiled from the particle tree;
  the recursive matcher it replaced stays as its oracle behind
  `--cross-check-content-model` / `CrossCheckContentModel`.
- The `corpus` test layer runs fifteen packages saved by Microsoft Office
  through validation and an open-save-open matrix.

#### Tools and front ends

- `ExyokiOffice::Tools`: validation, inspection, `Stat`, `Diff`, `Detect`,
  `Redact`, `Unpack`/`Pack`, `Query`, `Extract`, text search and replace, Word
  splitting, template filling, workbook recalculation and document comparison.
  Every entry point also has an overload taking an already open editor.
- `ExyokiOffice::Xml`, namespace-precise XPath 1.0 over any part.
- Conversion between packages and an `exyokioffice-document` JSON envelope, plus
  CSV import and export for worksheets. The envelope has a published JSON Schema
  ([docs/schemas/exyokioffice-document-v1.schema.json](docs/schemas/exyokioffice-document-v1.schema.json))
  that `exyoki schema --check` validates against.
- `exyoki`, the command line: inspect, validate, convert, unpack and repack,
  query, redact, compare, recalculate, fill, search and replace, read and write
  properties, with several `--format`s and a stable exit-code table.
  `exyoki commands` describes the interface as data and `exyoki completions`
  generates a shell completion script.
- Three Model Context Protocol servers, `exyoki-mcp-word`, `exyoki-mcp-excel`
  and `exyoki-mcp-power-point`, which keep documents open behind a `documentId`,
  answer in one envelope with machine-readable error codes, support `batch` and
  `undo`, confine every path to a `--workspace` root, offer `--read-only`, and
  expose no code-execution tool.

#### Building and consuming

- Windows (MSVC and `clang-cl`) and Linux (GCC and Clang), C++20, CMake 3.25 or
  newer, driven through the presets in `CMakePresets.json`.
- Installs as a CMake package: `find_package(ExyokiOffice 1.0 CONFIG REQUIRED)`
  and link `ExyokiOffice::ExyokiOffice`; `tests/install` is the smoke test.
- A vcpkg port, `exyokioffice`, with optional `tools` and `mcp` features,
  neither on by default. The port sources live in a clone of microsoft/vcpkg;
  this repository carries the consumer side in [vcpkg/](vcpkg/README.md) and the
  `Test-Port.ps1` script that installs the port and runs a project against it.
- A distroless container image, built by the `create_install` workflow as
  `ExyokiOffice-<version>-docker-amd64` and pushed to `ghcr.io` by
  `publish_docker`, carrying the library, `exyoki`, the three MCP servers and
  the license notices. See [docs/tools/docker.md](docs/tools/docker.md).
- The build honors `BUILD_SHARED_LIBS` and `EXYOKIOFFICE_RUN_GENERATOR`; the
  generated sources are committed, so the generator can be turned off for
  packaging builds and cross compilation.
- The versioning and ABI policy is [docs/ABI.md](docs/ABI.md): the ABI identity
  is `MAJOR.MINOR` and only a patch release promises an unchanged ABI.
- The manual under [docs/](docs/README.md) is rendered as a PDF alongside a
  Doxygen API reference by the `docs-pdf` and `doxygen-pdf` workflows.

### Security

- The security policy is [SECURITY.md](SECURITY.md): vulnerabilities are
  reported privately through GitHub's *Report a vulnerability*, and a confirmed
  critical report is fixed within one week. It also states the security model.
- Every front end opens packages under `OpenXmlPackageLimits::Recommended()`
  rather than the library's unlimited default; `--package-limits unlimited`
  restores the unbounded behaviour and warns at start-up.
- `Tools::Unpack` and `Tools::Pack` treat archive entry names as untrusted, and
  a rename manifest cannot turn `exyoki pack` into a generator of traversal
  archives.
- Signature canonicalization stops at 512 levels of element nesting, and a
  JSON-RPC line longer than 16 MiB is answered with a parse error.
- The library is fuzzed with libFuzzer under AddressSanitizer, and every input
  that ever crashed a target is replayed by the unit test build. See
  [docs/fuzzing.md](docs/fuzzing.md).

### Known limitations

- Saving a signed package breaks the digests over its *content* parts, because
  those are re-serialized from their trees rather than written as stored;
  `SignatureSavePolicy` warns about it. The signature part itself survives.
