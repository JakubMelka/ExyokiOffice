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

- `examples/ExampleWordDemo`, a one-page Word showcase document built with
  `Word::WordDocumentEditor`, enabled by `EXYOKIOFFICE_BUILD_EXAMPLE_WORD_DEMO`.
- Ten MCP tools, closing the gaps between what the compatibility matrix grades
  as supported and what the servers reach. See
  [MCP servers](docs/tools/mcp-servers.md#tool-catalog).
  - Word `format_table`: width, alignment, borders, cell padding, column
    widths, and per-cell shading, alignment and borders.
  - Word `list_charts` and `update_chart`: read and rewrite the series and
    title of a chart the document already carries.
  - Excel `add_comment`, `list_comments` and `delete_comment`, covering both
    threaded comments and plain notes.
  - Excel `add_image` and `set_print_setup`, the latter covering page setup,
    margins, print area, repeated titles, and headers and footers.
  - PowerPoint `add_shape` and `format_shape`: preset geometry, connectors,
    fills and outlines.
  - PowerPoint `list_animations`, `add_animation`, `update_animation` and
    `remove_animation`, covering entrance, emphasis, exit and motion-path
    effects with their triggers and timing.
  - Excel `move_sheet`, `copy_sheet` (including from another workbook),
    `copy_range` and `set_protection`.
  - Excel `add_slicer`, `list_slicers` and `set_slicer_selection`, over pivot
    tables and worksheet tables.
  - Excel `list_tables` and `update_table`: rename a table, toggle its filter
    buttons and totals row, and filter a column to a set of values, hiding the
    rows the filter excludes.
- Excel `add_conditional_formatting` takes the `containsErrors`,
  `notContainsErrors`, `top`, `bottom`, `aboveAverage` and `belowAverage` rule
  kinds, applies one rule to several ranges at once, and paints the cells a
  rule matches with the appearance passed in `format`.
- `StyleRepository::GetOrAddDifferentialFormat`, `GetDifferentialFormat` and
  `DifferentialFormatCount` register and read the `dxfs` differential formats a
  conditional formatting rule paints with. See
  [Data validation and conditional formatting](docs/excel/validation.md).
- `get_theme` and `set_theme` on all three servers read and change the scheme
  colours and fonts a document resolves its theme references against; a
  document without a theme is given the Office default first. See
  [MCP servers](docs/tools/mcp-servers.md).
- Excel `get_vba_project`, `set_vba_project` and `remove_vba_project` move the
  opaque `vbaProject.bin` between a workbook and a workspace file; the payload
  is never parsed or executed.
- Word and PowerPoint `set_protection`, matching the Excel tool of the same
  name: an editing restriction on a document, a password to modify on a
  presentation. Excel's moved from the `layout` group to `review` so the three
  can be filtered together with `--toolsets`.
- Word `define_style` and `delete_style` write and remove style definitions,
  including the run and paragraph formatting a style carries; `built_in` marks a
  definition as Word's own style of that name rather than a new one.
- Word `define_list` and `list_numbering` write and report multi-level list
  definitions, and `insert_list` takes a `numbering_id` to continue an existing
  sequence or lay out a definition.
- Word `insert_content_control`, `list_content_controls` and
  `update_content_control` write, read and remove content controls.
- Word `insert_image` takes a `layout` object for a floating picture with text
  wrapping, anchoring and distance from text; `set_section` takes `columns`; and
  `apply_style` puts a character style on the runs of the named blocks.
- PowerPoint `add_layout`, `delete_layout` and `set_slide_layout` write the
  layout side of a presentation's design.
- PowerPoint `list_custom_shows` and `set_custom_show` read and write the named
  slide sequences a deck plays.
- PowerPoint `add_media` places audio or video on a slide, embedded from a
  workspace file or linked by address.
- Excel and PowerPoint `add_chart` draw a series as another type or against a
  secondary axis, create bubble charts, and take axis titles, legend and
  gridlines; Excel's reads its ranges from another worksheet.
- `ExcelChartSeries::Type`, `ExcelChartSeries::SecondaryAxis`,
  `PresentationChartSeries::Type` and `PresentationChartSeries::SecondaryAxis`,
  with `SecondaryValueAxisTitle` on both chart definitions, write combination
  charts. See [Excel charts](docs/excel/charts.md) and
  [PowerPoint charts](docs/powerpoint/charts.md).
- A call to a tool that `--read-only` or `--toolsets` withheld, or to a
  capability the servers leave out such as equations, SmartArt, encryption or
  signatures, answers `unsupported` with a hint instead of `-32602`. See
  [Limits of this version](docs/tools/mcp-servers.md#limits-of-this-version).
- `tests/office-oracle/Invoke-OfficeOracle.ps1` opens documents in Microsoft
  Office and reports what Office refuses or drops, and
  `tests/mcp_python/oracle_corpus.py` writes a document per MCP server to run it
  on. See [RELEASE.md](RELEASE.md).
- `get_document_info` reports what a document restricts, on all three servers.

### Fixed

- `OpenXmlPackageValidator` reports the package-semantic rules Office enforces
  and `validate_document` no longer passes such files. See the `validate`
  section of [exyoki](docs/tools/exyoki.md).
  - `PackageDanglingRelationshipReference`: any relationship-namespace
    attribute (`r:id`, `r:embed`, …) naming no relationship of its part,
    including `x:tablePart`, which the schematron rules did not cover.
  - `PackageTableRangeOverlap`, `PackageThreadedCommentPersonUndefined`,
    `PackagePresentationMissingSlideMaster`, `PackageSlideMissingSlideLayout`,
    `PackageSlideLayoutMissingSlideMaster`, `PackageSlideMasterMissingTheme`.
  - `PackageStyleReferenceUndefined`, a warning when `w:pStyle`, `w:rStyle`
    or `w:tblStyle` names a style `styles.xml` does not define.
- Excel defects the Office COM test found. See [MCP servers](docs/tools/mcp-servers.md#exyoki-mcp-excel)
  and [Worksheets](docs/excel/worksheets.md).
  - `copy_sheet` and `ExcelDocumentEditor::CopyWorksheet` clone the sheet's
    drawings, charts, comments, tables and hyperlinks with valid relationships
    and fresh table and thread ids; Excel refused the copy before.
  - `CopyWorksheetFrom` and `merge_documents` remap styles into the target
    stylesheet and carry threaded-comment persons along, instead of refusing
    formatted sheets or writing a workbook Excel refuses.
  - `add_table` and `Worksheet::CreateTable` refuse a range overlapping
    another table with `range_invalid`.
  - `add_image` and `add_chart` keep the requested size: they write one-cell
    anchors with an extent (`Worksheet::DrawingAnchorForSize`,
    `DrawingAnchor`) instead of rounding to whole default cells.
  - `set_print_setup` and `Worksheet::SetPageSetup` switch
    `sheetPr/pageSetUpPr fitToPage` on for fit-to-width and fit-to-height.
  - `set_slicer_selection` on a table slicer hides the excluded rows as
    `update_table` does.
  - `read_range` reports numeric and boolean formula results typed, not as
    strings.
  - `set_column_width` and `set_row_height` refuse widths outside 0..255 and
    heights outside 0..409.5 with `input_invalid`.
  - An unknown table answers `block_not_found`, an unknown slicer
    `shape_not_found`, and `remove_vba_project` without a project answers ok
    with `removed: false`.
- Word defects the Office COM test found. See [MCP servers](docs/tools/mcp-servers.md#exyoki-mcp-word)
  and the Word chapters under [docs/word](docs/word/).
  - `insert_paragraph`, `edit_paragraph` and `apply_style` define the built-in
    `Normal` and `HeadingN` styles on first use
    (`StyleManager::EnsureBuiltInStyle`); Word showed such headings as body
    text before. `edit_paragraph` takes `heading_level`.
  - `Paragraph::AddBookmark` encloses the paragraph text, refuses a duplicate
    name and allocates ids over end markers; `edit_paragraph` keeps bookmark
    and comment markers in place.
  - `set_header_footer` kinds `first` and `even` write `w:titlePg` and
    `w:evenAndOddHeaders` (`Section::SetTitlePage`,
    `WordDocumentEditor::SetEvenAndOddHeaders`).
  - `Table::MergeCells` keeps earlier merges and carries the covered cells'
    text into the anchor; `Table::CanMergeCells` and `Table::SetStyleId` are
    new, and `insert_table` honours `style_id`.
  - `resolve_revisions` rejecting a tracked deletion keeps the restored
    text's `xml:space`.
  - `set_section`: a `page_size` preset keeps the section's orientation, and
    a preset together with `width` or `height` is `input_invalid`.
  - `split_document` by paragraphs writes no empty trailing file;
    `define_style` refuses a dangling `based_on` or `next`; `delete_style` of
    a missing style answers ok with `removed: false`.
- MCP `batch` no longer corrupts the undo history when it is already
  `--snapshot-depth` deep; `undo` after a batch restores the step before it.
  See [Sessions, undo, and batches](docs/tools/mcp-servers.md#sessions-undo-and-batches).
- MCP error codes that misled an agent branching on them. See
  [Troubleshooting](docs/tools/mcp-servers.md#troubleshooting).
  - `export_media` onto existing files and `split_document` onto existing
    outputs answer `file_exists`; a `split_document` prefix that is not a plain
    file name answers `path_invalid`.
  - `open_document` and the reading tools given a `path` of another family
    answer `family_mismatch` instead of `package_load_failed`.
  - `search_text`, `replace_text` and `query_xml` answer `input_invalid` for a
    malformed regular expression or XPath and for an unknown `part`.
  - `set_properties` refuses a custom value it cannot store with
    `input_invalid` instead of dropping it silently.
  - `get_document_model` and `get_document_markdown` answer `sheet_not_found`
    or `slide_not_found` for a `scope` the document does not have.
- The Python MCP suite and `oracle_corpus.py` take the most recently built
  server binaries and print which ones they run against. See
  [tests/mcp_python/README.md](tests/mcp_python/README.md).
- The Office oracle gate saves Word documents with `Save` on a copy rather than
  `SaveAs2`, which can hang, and on a timeout ends only the Office instance its
  worker started.
- Excel constructs that the library wrote as valid markup and Excel then threw
  away. See [MCP servers](docs/tools/mcp-servers.md).
  - `Worksheet::AddThreadedComment` writes the legacy note and VML drawing that
    back a thread; without them Excel discarded `xl/threadedcomments`
    altogether. `Worksheet::Comments` does not report the backing note.
  - `Worksheet::CreateSlicer` registers a table slicer under the extension URI
    `{3A4CF648-6AED-40f4-86FF-DC5316D8AED3}` and a pivot slicer under
    `{A8765BA9-456A-4dab-B4F3-ACF838C121DE}`; both were written under one wrong
    URI and discarded on open.
  - `Worksheet::CreateSlicer` declares the slicer cache as a workbook defined
    name, declares the pivot cache identifier its slicer cache names, and
    raises the pivot table's `updatedVersion` to 4. Without the first two Excel
    refused the workbook; without the third it dropped the slicer.
  - A pivot slicer cache names its sheet by `sheetId` rather than by tab
    position, so a slicer survives on a workbook whose sheets were reordered.
  - `ExcelTable::SetTotalsRowShown` grows the table reference by a row and keeps
    the totals row outside `autoFilter`; Excel refused to open a workbook whose
    auto-filter reached into it. `Resize` and `SetAutoFilterEnabled` hold the
    same invariant.
- `Worksheet::CreateTable` writes each column name into its header cell and
  refuses a range holding merged cells, and `Worksheet::MergeRange` refuses a
  range that overlaps a table; Excel refused to open a workbook breaking either
  rule. MCP `add_table` warns with `table_header_rewritten` when it replaces a
  header value.
- MCP `redact_document` on an open session answers with an envelope its output
  schema allows, `compare_documents` refuses an output name of another Office
  family, and `diff_documents` reports `package_load_failed` for a file that is
  not a package.
- MCP `set_transition`, `set_slide_size` and `modify_sheet_structure` answer an
  ambiguous or out-of-range request with `input_invalid` or `range_invalid`
  instead of guessing or reporting `operation_failed`, and `resolve_revisions`
  warns with `revision_not_found` about identifiers that name no revision.
- MCP `delete_blocks` refuses a range that runs past the last block or ends
  before it starts, instead of deleting what was left of it.
- Reading a combination chart returns the series of every plot group rather
  than the first one, in `Worksheet::Charts`, `PresentationShape::GetChart` and
  `WordDocumentEditor::Charts`, and rewriting its data keeps each series in its
  own group instead of duplicating them all into the first.
- `Worksheet::Charts` reports `ExcelChartSeries::SourceSheet` for a series kept
  on another worksheet, so updating a chart read back no longer re-resolves its
  ranges against the chart's own sheet.
- `PresentationShape::SetMedia` relates an embedded media part a second time as
  `http://schemas.microsoft.com/office/2007/relationships/media` and names it
  from a `p14:media` extension, which is how PowerPoint tells an embedded stream
  from a linked one; without them PowerPoint rewrote the relationship as
  external and dropped the media part on its next save.
- `OpenXmlPackageValidator` no longer reports a relationship-type mismatch for a
  part that is also related under its own descriptor type, which had made it
  report an error on presentations PowerPoint itself wrote.
- `Word::Image::SetAltText` writes the text on the drawing's `wp:docPr` as well
  as the picture's `pic:cNvPr`; Word reads the first and ignores the second, so
  a picture labelled through this API was unlabelled in Word's Alt Text pane and
  for anything reading it.
- `PowerPointDocumentEditor::ProtectFromModification` writes the seven
  `p:modifyVerifier` attributes `CT_ModifyVerifier` requires and PowerPoint
  itself writes; the ISO attribute group it wrote before is optional in the
  schema, so the presentation failed validation on seven counts.
  `UnprotectFromModification` now validates either attribute group, so a
  presentation protected in PowerPoint can be unprotected rather than reported
  as `UnsupportedVerifier`.

## [1.1.0] - 2026-08-20

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
  the load still succeeds, opening through an editor keeps the warning, and
  `Tools::RedactDocument` repeats it in its own diagnostics.
- A relationships part that reads but is not XML is reported as
  `ValidationErrorId::OpcMalformedPartXml`.
- `[Content_Types].xml` is matched case-sensitively, so a package cannot carry
  two of them with different meanings.
- An MCP message nesting arrays or objects deeper than 128 levels is refused
  before the JSON is parsed.
- Word identifier allocation hands out a value the document does not already
  use, instead of counting on from the highest one. A file carrying the largest
  identifier its type holds used to reach signed overflow and then produce a
  duplicate `w:id`, `w:name` or `wp:docPr/@id` - a document Word offers to
  repair - and one such attribute in an input was enough.
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
  character style that hides text counts as hidden - across every styles part a
  document carries, not just the last one - `w:vanish` and `w:specVanish` are
  read as on/off values, `xl/persons` and legacy `ppt/comments/*` go with the
  comments they belong to, and the metadata pass clears descriptive properties,
  last-printed time, attached template name, `w:rsid*` including the `w:rsids`
  registry in `word/settings.xml`, `customXml` and `docProps/thumbnail`. What it
  still does not reach is stated in the API and in
  [docs/tools/exyoki.md](docs/tools/exyoki.md).
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
  [docs/ABI.md](docs/ABI.md) allows a documented source break in a minor release,
  which this is; this release already requires a recompile.

### Changed

- `PresentationSlide::AddComment`/`UpdateComment` and
  `PowerPointDocumentEditor::AddCommentAuthor` accept only braced GUID identifiers
  (`Guid::New()`, `Guid::IsBraced()`), which is what PowerPoint reads there;
  the MCP `add_comment` tool mints its author ids accordingly.
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

- PowerPoint packages written by `PowerPointDocumentEditor` open in PowerPoint
  without repair and show their pictures:
  - a new presentation carries the required `p:notesSz`;
  - the notes master and the handout master get their own theme part;
  - `AddSlideLayout`, `CopySlideFrom` and `ImportSlideMaster` allocate layout ids
    unique across all masters and layouts;
  - `AddComment` writes the slide anchor (`pc:sldMkLst`) and the slide-side
    `p188:commentRel` extension, and the ids are documented as braced GUIDs;
  - `AddPicture` gives the picture a rectangle geometry, without which PowerPoint
    renders an empty area; `ReplacePictureFromData` adds it to a picture that lacks one.
- `OpenXmlDomValidator` and `exyoki validate` report content that ends before a
  required child (`ParticleConstraintViolation`, "content ends after N child
  elements"): the generator read a particle without `Occurs` in the imported
  schema metadata as optional, whereas it means exactly one, so a missing
  `p:notesSz`, `x:sheets`, `w:tblGrid` or `c:chart` passed validation. Documents
  that validated clean before may now report errors.
- Children of an open content model (`a:graphicData`) that share a name with
  another class are typed from the metadata's additional-element list, so
  `c:chart` and `cx:chart` in a drawing are validated as chart references, not as
  chart bodies (`data/exyokioffice_particle_extras.json` gained
  `AppendAdditionalElements` for the chartex reference).
- A derived element class (`w:top`, `w:b`, every `IsDerived` type) is validated
  with the attributes, facets, particle and constraints its base type declares;
  they were skipped, so `w:space="120"` on a paragraph border passed.
- `Paragraph::SetBorders` writes the border spacing in points (0-31), the unit of
  `w:space`, instead of twips.
- Tight and through image wrapping (`ImageWrap::Tight`/`Through`) write the
  required `wp:wrapPolygon`.
- `PresentationShapeTree::AddMedia` places the audio or video time node inside the
  timing root (`p:tnLst/p:par`), keeps an empty `p:blipFill` and a rectangle
  geometry on a media frame without a poster frame, and animation rebuilds keep
  the media nodes.
- `WordDocumentEditor::AddTable`/`InsertTable` and `Table::AddNestedTable` write the
  required `w:tblPr`.
- Flat OPC conversion tests path traversal one component at a time, so a part
  named `notes..xml` is no longer dropped, and counts a backslash as a separator
  because the ZIP writer rewrites one into a slash.
- Relationships created outside the main document part are recorded in that
  part; a paragraph with no part returns nullptr from `AddHyperlink`.
- An inline content control inherits the part its paragraph lives in.
- `Paragraphs()` and `Tables()` reach into a block-level structured document tag.
- A drawing identifier is allocated across every story of a Word document.
- A picture added without an explicit size is scaled to the text width.
- PowerPoint text extraction descends into group shapes and reads tables.
- A leading empty cell keeps its column when a PowerPoint table is extracted.
- Searching a Word document compiles the expression once instead of once per
  paragraph, and the reported context snaps to a UTF-8 character boundary
  instead of cutting a multi-byte character in half.
- An XML part is parsed once when a package is loaded: the limit check reads the
  same tree the part keeps rather than a second throwaway parse of the same bytes.
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
