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

- Every `Open` overload of the three document families and their editors takes
  an optional `Packaging::OpenError*` as its last argument. Passing one turns
  "returned nullptr" into a reason: a missing file, a file that exists but
  cannot be opened, an unreadable package (which is also what an encrypted
  document looks like), an exceeded limit with the loader's diagnostics
  attached, a strict-validation failure, a cancellation - including one that
  arrives while the loader is already inside the package, which used to be
  reported as a corrupt file - or a document of another family handed to the
  wrong API. All three families answer the last one: a package with no main
  document part is not a Word document and one with no workbook part is not an
  Excel document, however readable the package is. Passing nothing behaves
  exactly as before.
- `Security::ICryptoProvider::VerifyDataWithChain` receives the whole
  certificate chain embedded in a signature rather than the leaf alone, which is
  what a provider building a path to its own trust anchors needs. It has a
  default implementation that forwards the first certificate to `VerifyData`, so
  existing providers keep working unchanged.
- `Tools::ToFlatOpcOptions::Limits`, so `Tools::ConvertToFlatOpc` and
  `exyoki flatopc` read an archive under the same ZIP ceilings as the rest of
  the library instead of allocating whatever size the ZIP directory declares.
- `Tools::RedactResult::PartsRemoved` counts whole parts detached for what they
  carried, and `exyoki redact` and the MCP tool report it as `partsRemoved`.
- `Security::SignatureResult::UncoveredParts` lists the package parts a
  signature says nothing about. An Open XML signature covers the parts its
  manifest names and nothing announces the ones it does not, so a part added
  after signing leaves every digest matching and `IsValid()` true. The
  relationship transform makes that reachable in practice: a signature that
  selects relationships by `SourceId` keeps its digest when a relationship with
  a new identifier appears beside them.
- `Security::VerifySignaturesOptions::AllowSha1`, defaulting to false. A SHA-1
  digest or an RSA-SHA1 signature value is now reported as invalid with an
  explanation instead of being computed, because a signature over a
  collision-broken digest is a signature two different documents share. Archived
  packages that carry one can still be verified by turning the option on.
- `ExyokiOffice::RegexPattern`, an expression plus its options, which is what
  the regular-expression API takes now.
- Image detection covers TIFF, EMF, and placeable WMF, and reads a JPEG's
  resolution from Exif as well as from JFIF.
- `Word::Paragraph::AttachOwningPart` / `OwningPart`, and the same pair on
  `Hyperlink`, `Table`, `ContentControl`, `Note`, `Comment`, and
  `HeaderFooterContent`: the part a piece of content lives in, which is where
  its relationships belong.
- Community files a contributor looks for: `CODE_OF_CONDUCT.md`, issue and pull
  request templates, and a `Smoke` workflow that builds and tests on every push
  and pull request. The full matrix stays in the manually started `CI` workflow.

### Security

- The regular-expression subject limit is enforced in the document text tools as
  well, not only in the Word paragraph API. `Tools::SearchDocumentText` and
  `ReplaceDocumentText` handed Excel cells and PowerPoint shapes, table cells and
  notes pages straight to the regex engine, so a document that could not crash
  the process through one frontend could still crash it through the other. Both
  now go through the single matcher that applies
  `RegexPattern::MaximumSubjectLength`.
- An RSA-SHA1 signature value is refused with no crypto provider present. The
  refusal was decided after the provider check, so the callers that verify
  without one - `exyoki signatures`, the MCP servers - reported a
  collision-broken signature as merely unchecked. Which algorithm a signature
  claims is written in the document and needs no key to read.
- Parsing an EMF picture frame no longer subtracts two untrusted `Int32` fields
  into an `Int32`. `right > left` does not make the difference representable, so
  a metafile stating the extremes of the range reached signed overflow -
  undefined behavior - before any dimension was computed.
- Package signature verification no longer accepts a signature whose manifest
  has been moved out of the object it was signed in. The parts a signature
  covers are now read from the `Manifest` inside a `dsig:Object` whose digest
  has verified, rather than from any object of the signature part. Wrapping the
  signed object in another one leaves both the same-document digest and the
  signature value intact, so a package edited after that treatment used to
  verify as unchanged content. Two rules follow the same principle. A signature
  whose verified references name no part and no relationship set of the package
  covers no content and is reported `Invalid` with `SignatureMalformed` instead
  of valid - whether because no manifest was reached at all, or because the
  manifest's entries are bare-name references that digest elements of the
  signature XML itself. And a signature part that repeats an element `Id`,
  including one repeated from the root `Signature` element, is refused, because
  which element a `#fragment` resolves to would otherwise decide what was
  checked. See [docs/Signatures.md](docs/Signatures.md).
- Formula text is bounded: expressions nested deeper than 128 levels are
  rejected with a diagnostic rather than descending one stack frame per level.
  Every `f` element of a loaded workbook reaches the parser, so a couple of
  kilobytes of `((((...))))` used to end the process in a stack overflow.
  Excel's own limit is 64 nested function levels, so nothing it can store is
  affected. The parsed expression tree is also released iteratively, since a
  chain such as `A1+A2+...` is as deep as the formula is long while nesting
  nothing, and both walks over it in the formula engine - validation and
  precedent collection - use an explicit stack for the same reason.
- Deeply nested XML no longer reaches the stack through a walk over a document
  tree. DOM validation, `Xml::InnerText`, `XmlQuery` descendant walks, the
  namespace collection behind a deep copy, and the package limit check itself
  all keep their own stack; markup compatibility processing, which rewrites the
  tree as it descends and cannot become a loop, refuses to descend past 512
  levels and reports the new `ValidationErrorId::NestingTooDeep`. The limit
  check mattered most: a caller that set a node ceiling but left the depth
  ceiling at "no limit" had the check itself overflowed by the document it was
  meant to contain. That check now also walks the document through its parent
  and sibling links rather than queueing the children it has yet to visit, so
  an element with millions of children no longer costs an allocation per child
  before the node ceiling rejects the document.

- Packages start with the recommended ZIP/XML limits instead of no limits at
  all. Every ceiling used to default to zero, meaning unlimited, so an
  application that opened an upload without reading the documentation had no
  defence against a decompression bomb or deeply nested XML. `Recommended()` is
  wide enough that no ordinary Office document is rejected; a caller who wants
  none says `OpenXmlPackageLimits::Unlimited()`, and one who knows its own
  documents should tighten it further.
- `Tools::ConvertToFlatOpc` enforces those limits too. It reads the archive
  itself rather than through the OPC loader, so it used to allocate the declared
  uncompressed size of every entry - the one entry point whose whole job is
  reading a file the caller did not make, with the guard the rest of the library
  applies missing.
- `Tools::RedactDocument` scrubs what it always claimed to. Tracked changes are
  accepted in every story part rather than in the body alone, so a deletion in a
  header no longer survives publication; a deleted paragraph mark merges its
  paragraph with the next instead of losing only the marker, which was a reject
  dressed as an accept; deleted table rows go with their cells; the
  `w:rPrChange` family of records of former formatting is dropped; hidden text
  is recognized when a character style hides it and not only when the run does;
  Excel's `xl/persons` registry and PowerPoint's legacy `ppt/comments/*` parts
  are removed with the comments they belong to; and the metadata pass now clears
  the descriptive properties, the last-printed time, the attached template name,
  the `w:rsid*` editing-session identifiers, the `customXml` store, and
  `docProps/thumbnail` - a rendering of the first page made before any of this
  ran. Accepting a revision also drops the property-level markers that record
  one without wrapping anything, such as `w:trPr/w:ins` on an inserted row, each
  of which carries an author and a date; revisions inside comments that were
  kept are accepted too. Hidden-text removal reads `w:vanish` and `w:specVanish`
  as the on/off values they are, so a run that switches the hiding off with
  `w:val="false"` is visible and stays - taking the element's presence for the
  answer deleted text the document displays. The registries that name comment
  authors are removed with the comments rather than with the metadata: deleting
  them from under comments that were kept left every author reference dangling.
  What it still does not reach is stated in the API and in
  [docs/tools/exyoki.md](docs/tools/exyoki.md) rather than left to be assumed.
- `PowerPointDocument::Open` applies the open settings the API documents.
  `OpcValidationMode` and `MaxCharactersInPart` were read for Word and Excel and
  silently ignored for PowerPoint, so a caller opening an untrusted `.pptx`
  under strict validation and a size budget got neither.
- A ZIP entry with an empty name no longer causes a one-byte heap under-read
  while the archive is being listed. The bundled `zip_entry_isdir` read the last
  character of a name that had none; the name is now checked first, and the
  vendored function guards the length itself.
- The largest `spinCount` accepted from a document dropped from ten million to
  one million, ten times what Word and PowerPoint write. The value comes out of
  the file and the work is linear in it, with nothing to parallelize, so a
  crafted document could spend a minute of SHA-512 per `Unprotect` call for an
  element that only tells a consumer which password to accept.
- Regular expressions run over at most 32768 bytes of text
  (`RegexPattern::MaximumSubjectLength`). Backtracking engines recurse per input
  character, and the Microsoft implementation exhausts the stack on a long
  enough subject - which is not an exception a caller can catch, and is the one
  failure a library behind an MCP server must not be able to reach.

### Removed

- **Source-incompatible with 1.0.0.** The `std::regex` overloads of
  `Word::Paragraph::FindAllRegex` and `ReplaceAllRegex` are gone; the
  `RegexPattern` overloads below replace them. Code written against 1.0.0 has to
  change `paragraph->FindAllRegex(std::regex(text, std::regex::icase))` into
  `paragraph->FindAllRegex(RegexPattern{text, true})`, or
  `RegexPattern::Literal(text)` where the needle is not an expression.

  [docs/ABI.md](docs/ABI.md) reserves a source break for a major release or for
  a security fix that cannot be made otherwise, and this is the latter: the
  subject-length limit that keeps a crafted document from exhausting the stack
  belongs to the pattern, and a signature taking an already compiled
  `std::regex` both bypasses it and puts the standard library's regex
  implementation back into this library's ABI. Keeping the old overloads as
  deprecated adapters would mean keeping `<regex>` in the public headers, which
  is the thing being removed. **The next release therefore has to be 2.0.0.**

### Changed

- `Word::Paragraph::FindAllRegex` and `ReplaceAllRegex` take an
  `ExyokiOffice::RegexPattern` - the expression and its options - instead of a
  compiled `std::regex`, and `<regex>` is gone from the public headers. A
  compiled `std::regex` in a signature makes the standard library's regex
  implementation part of this library's ABI, and it cost every translation unit
  that included a Word header the whole of `<regex>` whether it searched
  anything or not. `PowerPointDocument.hpp` no longer includes the 213 KB
  `Presentation.hpp` for five types it only holds behind a `shared_ptr`.
- `EXYOKIOFFICE_RUN_GENERATOR` defaults to on only for a developer build of this
  repository - top-level project, writable source tree - and to off otherwise.
  The generator writes into the *source* tree, so the previous unconditional
  default failed a read-only checkout (Nix, distro packaging) and left
  regenerated files in a clone the user only meant to compile. Setting the
  option explicitly still wins.
- `EXYOKIOFFICE_WARNINGS_AS_ERRORS` now defaults to `OFF`. Every compiler
  generation invents diagnostics, so the previous default turned an unseen
  warning on a newer toolchain into a failed build for anyone who merely wanted
  to build the library. CI and `WinBuild.ps1` pass `ON` explicitly, so the
  builds that police warnings still do.
- Configuring stops with a plain message when the compiler is older than
  GCC 13, Clang 17, or MSVC 19.30, instead of failing hundreds of translation
  units later inside `<format>` or `<charconv>`. macOS, which no preset and no
  CI job covers, is reported as untested at configure time. README.md now
  carries the platform table and a note on how much memory a full build wants
  per job.

- A paragraph's text is one thing throughout the Word API. `Paragraph::Runs`,
  `PlainText`, `Find`, `GetText`, `ReplaceText` and the regular-expression
  overloads all read the runs of the paragraph in document order, including
  those inside hyperlinks, tracked insertions, content controls, smart tags and
  simple fields, with a `w:tab` counting as a tab and a `w:br` as a newline.
  They used to disagree: `PlainText` walked descendants while the others saw
  direct children only, so a search missed a word inside a hyperlink that
  `PlainText` had just reported, and an offset from one addressed different
  characters in the other. Text a reader does not see - tracked deletions, field
  instructions, and the separate content of an anchored text box - is not part
  of it, and a replacement is written back with tabs and breaks as elements
  rather than as characters inside `w:t`. A replacement rewrites the range it
  was given and nothing else: a page break or a non-breaking hyphen elsewhere in
  the same run keeps its element rather than being flattened into a plain break
  or a plain hyphen, so replacing a word no longer moves where the page ends.
- Text written through the Word API carries `xml:space="preserve"` by default.
  `AddParagraph("Hello ")` followed by `AddText("world")` produced `Helloworld`,
  because without the attribute an XML consumer may collapse the whitespace.
  Every text-taking overload defaults the same way; passing `false` still writes
  the text without it.
- `SignatureResult::IsValid()` is documented for what it is: the signature is
  cryptographically consistent with the certificate the signature itself
  carries. Establishing whether that certificate may be believed - chain
  building, revocation, policy - belongs to the crypto provider and the
  application, and the interface now says so where an implementer will read it.

### Fixed

- `DATE()` reaches the 1900 date system's imaginary 29 February however it is
  spelled. The day is now counted in serials rather than on the calendar, so
  `DATE(1900,1,60)` and `DATE(1900,3,0)` answer 60 like `DATE(1900,2,29)`; both
  used to step over the phantom day and answer 61 and 59. Dates away from 1900
  are unchanged.
- Searching a Word document compiles the expression once instead of once per
  paragraph. `Tools::WordTextTools` held the pattern rather than a compiled
  expression, so a document with N paragraphs built the same automaton N+1
  times - and building it is the expensive half of a short match.
- A leading empty cell keeps its column when a PowerPoint table is extracted.
  The tab separator was emitted only once something had been written, so the row
  `{"", "Q2"}` came out as `Q2`, indistinguishable from a one-column table.
- An inline content control inherits the part its paragraph lives in. One added
  to a comment reported the main document part as its owner and one in a header
  reported nothing at all, which is where a hyperlink added inside it would have
  written its relationship.
- `EXYOKIOFFICE_RUN_GENERATOR` defaults to off when cross compiling, which its
  documentation already claimed. The condition tested only for a top-level
  project and a writable source tree, so an ordinary cross build built the
  generator for the target and then tried to run it on the host.
- A path outside ASCII opens on Windows. The bundled ZIP layer decodes the file
  name it is handed as UTF-8, while `std::filesystem::path::string()` produces
  the active code page, so `Příloha.docx` reached the file API as mojibake and
  the package "did not exist" for exactly the users whose language needs the
  characters.
- `ExcelCellValue::Number` maps an infinity or a NaN to `#NUM!` instead of
  writing `inf` or `nan` into `<v>`, which produced a workbook Excel offered to
  repair by dropping the sheet.
- A number converted to text is spelled the way a spreadsheet spells it:
  fifteen significant digits, an uppercase `E` with a signed two-digit exponent,
  and positional notation between 1E-04 and 1E+21. `=1/3&""` was two digits
  longer than Excel writes, and `=1E+21&""` came out as `1e+21`. Numbers are
  compared at the same precision, so `=0.1+0.2=0.3` is TRUE as it is in Excel.
  The value stored in `<v>` is unchanged and still round-trips exactly.
- The 1900 date system keeps its imaginary leap day. `DATE(1900,2,29)` is serial
  60 rather than 61, `DAY(60)` is 29, and serial 0 is 0 January 1900 rather than
  31 December 1899 - the convention every spreadsheet shares so that serial
  numbers agree.
- Relationships created by content outside the main document part are recorded
  in that part. A hyperlink added to a header, a footer, a footnote, an endnote
  or a comment wrote its target into `document.xml.rels`, where Word does not
  look for it, and reading one back could answer with an unrelated URL. Table
  cell paragraphs carried no part at all, so `AddHyperlink` returned a link with
  nowhere to store its target and `AddComment` refused for a reason that had
  nothing to do with the document. A paragraph with no part now returns nullptr
  from `AddHyperlink` instead of adding a link that points nowhere.
- `Paragraphs()` and `Tables()` reach into a block-level structured document tag,
  so a cover page, a table of contents, or a form no longer disappears from
  extraction, search, and replace.
- PowerPoint text extraction descends into group shapes and reads tables.
  Grouping three labelled boxes used to remove all three from the extract, and a
  table is a graphic frame rather than a shape with a text frame.
- A drawing identifier is allocated across every story of a Word document rather
  than from `document.xml` alone, so an image added beside an existing one in a
  header no longer collides with it.
- A picture added without an explicit size is scaled to the text width. At the
  96 DPI a file without a resolution defaults to, a four thousand pixel photo was
  forty-one inches across and mostly off the page.
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
- Numbers are read and written the same way whatever locale the hosting process
  installed. `xsd:decimal`, the Excel number-format renderer behind `TEXT()` and
  formatted cell text, the schematron numeric comparisons, and the JSON,
  Markdown, workbook-model and MCP converters used the C conversions, which take
  their decimal separator from the global C locale: under a German locale the
  same workbook wrote `1,5` into `<v>` - text Excel refuses - and read `1.5`
  back as 1, and `TEXT(1234.5,"#,##0.00")` produced `1,234,,50`. Everything now
  goes through `std::from_chars`/`std::to_chars`, which are locale-independent
  by definition, and the number-format renderer no longer truncates silently at
  a fixed 64-byte buffer from about 1e63 upwards.
- The built-in SHA implementation no longer copies the whole message to pad it,
  which halves the peak memory of digesting a large part. Whole blocks are
  compressed where they lie and only the final block or two is assembled.
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
