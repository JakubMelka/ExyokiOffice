# Compatibility matrix

This chapter is the single answer to the two questions that decide whether
ExyokiOffice fits a project: **which files can it read and write**, and
**what does it actually do with the content once it has them**.

The rest of the manual describes each subsystem in depth. This page is
deliberately flat: tables, one row per format or feature area, with a link to
the chapter that documents it. It is part of the release documentation and is
updated together with the feature it describes.

Two things it is not. It is not a list of Microsoft Office features — Office
does far more than any editing library models. And it is not a promise about
fidelity of *rendering*: ExyokiOffice never lays out, paginates, or draws a
document, so "supported" always means "read, modelled, and written back",
never "looks the same on screen".

## How to read this matrix

Every feature row is graded on three independent capabilities, because they
genuinely differ — a construct the API cannot author may still survive a save
untouched:

- **Create** — a high-level editor can author the construct from nothing, in
  a document it just created.
- **Edit** — an existing construct, in a document that was opened from disk,
  can be read back and modified through the same API.
- **Preserve** — the construct survives open → save unchanged, whether or not
  the API models it. This is the library's default posture, described under
  "Preservation over interpretation" in the
  [Introduction](introduction.md); content outside the typed model is carried
  through verbatim rather than dropped.

The cells use four values:

| Value | Meaning |
| --- | --- |
| `Yes` | Fully covered by the public high-level API. |
| `Partial` | Covered, but with a stated restriction — read the Notes column. |
| `Preserved` | No typed API; the content round-trips untouched. Reaching it requires the typed DOM. |
| `No` | Not supported, and not silently approximated either. |

A `Preserved` in the Create or Edit column is therefore not a gap in data
safety. It means the work has to happen one layer down, through
`GetDocument()` and the typed OpenXML DOM.

## How this matrix is tested

Every feature row carries a **CTest label**. The label is not decoration: it
names a set of tests you can run, and the grades in that row are what those
tests assert.

Run one row's tests with the label in the last column:

```powershell
ctest --preset debug -L word-tables
```

Labels come in two kinds, and a row's label always selects both:

- **Layer labels** — `unit`, `dom`, `package`, `word`, `spreadsheet`,
  `presentation`, `tools`, `generator`, `compat`, `corpus` — one per test
  executable under [`tests/`](../tests/). `spreadsheet` also answers to `excel`
  and `presentation` to `powerpoint`, so `ctest -L excel` selects the Excel work
  whichever name you reach for.
- **Area labels** — the values in the tables below. An area is a group of test
  cases inside a layer, selected by the bracket tags in their `TEST_CASE`
  names.

One label crosses both kinds: `slow` marks the areas that sweep the whole file
corpus or the whole schema import rather than one construct —
`Corpus.Validation.*`, `Corpus.RoundTrip.*`, `Corpus.ContentModel.*`,
`Unit.ContentModel` and `Unit.ContentModelMetadata`. Between them they are about
six sevenths of the suite's running time and everything else is a few seconds,
so `ctest -LE slow` (or `.\WinBuild.ps1 -QuickTest`) is the edit-test loop. They
still run in a plain `ctest`, which is what a change has to pass before it is
done.

The three corpus sweeps are registered as one entry per corpus document —
`Corpus.RoundTrip.Word.Open_Source_Software` and so on — generated from
`corpus/manifest.json` at configure time. Describing a new fixture there is
therefore all it takes to have it swept, and `ctest --parallel` runs the
documents concurrently: the suite is bounded by the slowest single file rather
than by their sum. The test presets already ask for a parallel run, and
`.\WinBuild.ps1 -Test` raises the level to the machine's core count.

The areas of one layer partition it: together with the layer's `.Other` entry
they cover every test case exactly once, so a plain `ctest` still runs the whole
suite and nothing runs twice. A `<Layer>.Partition` entry checks that, because
overlapping tags would otherwise inflate the suite silently. Sharding an area
does not change that count — the entries of one area run the same cases over
different fixtures.

The layer that makes this page falsifiable is
[`tests/compat/`](../tests/compat/): **one test case per row below**, which
creates the construct from nothing, reopens the saved package and edits it
through the reading API, and then requires an open-save cycle to return the
package byte-for-byte. Rows graded `No` or `Partial` get the same treatment in
reverse — the case asserts the restriction the Notes column states, so a row
that quietly becomes stricter or looser fails the build instead of misleading a
reader.

Most rows also have older, finer-grained tests in their family layer, and the
label picks those up too: `ctest -L word-tables` runs both the table unit tests
and the Word tables row of this matrix.

## File formats

ExyokiOffice reads and writes OOXML packages — ZIP containers of XML parts.
Every document type below can be created from nothing, opened from a file or
from memory, and saved back.

### Conformance class: Transitional only

ECMA-376 / ISO 29500 defines two conformance classes, and ExyokiOffice
implements one of them:

| Conformance class | Namespaces | Support |
| --- | --- | --- |
| **Transitional** (what Office writes by default) | `schemas.openxmlformats.org/...` | Yes — everything on this page |
| **Strict** (ISO 29500 Strict, "Word Document (Strict)") | `purl.oclc.org/ooxml/...` | No |

The typed DOM is generated from the Transitional schemas, so a Strict package
is not merely unclassified — nothing above the package layer can read it. The
tools report it explicitly rather than as an unknown document family:

```console
$ exyoki extract-text strict.docx; echo $?
[error] ISO 29500 Strict packages are not supported; only the Transitional
        conformance class is (see docs/Compatibility.md). Re-save the document
        as Transitional to process it.
1
```

**`validate` rejects such a package with an error**
(`PackageStrictConformanceUnsupported`, exit code `3`). The OPC container on
its own is structurally fine, but reporting "no errors" would tell every
caller the file is supported, which is the one thing it is not:

```console
$ exyoki validate strict.docx; echo $?
    - severity=error, domain=packaging, errorId=PackageStrictConformanceUnsupported,
      message=Package uses the ISO 29500 Strict conformance class, which this
      library does not implement; only Transitional is supported. Re-save the
      document as Transitional.
3
```

The package layer itself is unaffected, because OPC is the same for both
classes: `parts`, `relationships`, `unpack`/`pack`, `to-flat-opc`, `diff`,
`dedup`, `export-media`, and `query` with an explicit `--part` all work on a
Strict package. The family-aware commands (`extract-text`, `stat`, `search`,
`replace`, `redact`, `convert`, `split`, `merge`) and the high-level editors
refuse it.

Strict documents are rare in practice — Office writes Transitional unless the
user picks Strict explicitly — and `File ▸ Save As` converts one to the other.

### Word

All four are covered by the `formats-word` label.

| Extension | `WordprocessingDocumentType` | Create | Open | Save |
| --- | --- | --- | --- | --- |
| `.docx` | `Document` | Yes | Yes | Yes |
| `.dotx` | `Template` | Yes | Yes | Yes |
| `.docm` | `MacroEnabledDocument` | Yes | Yes | Yes |
| `.dotm` | `MacroEnabledTemplate` | Yes | Yes | Yes |

### Excel

All four are covered by the `formats-excel` label.

| Extension | `SpreadsheetDocumentType` | Create | Open | Save |
| --- | --- | --- | --- | --- |
| `.xlsx` | `Workbook` | Yes | Yes | Yes |
| `.xltx` | `Template` | Yes | Yes | Yes |
| `.xlsm` | `MacroEnabledWorkbook` | Yes | Yes | Yes |
| `.xltm` | `MacroEnabledTemplate` | Yes | Yes | Yes |

### PowerPoint

All six are covered by the `formats-powerpoint` label.

| Extension | `PowerPointDocumentType` | Create | Open | Save |
| --- | --- | --- | --- | --- |
| `.pptx` | `Presentation` | Yes | Yes | Yes |
| `.potx` | `Template` | Yes | Yes | Yes |
| `.ppsx` | `SlideShow` | Yes | Yes | Yes |
| `.pptm` | `MacroEnabledPresentation` | Yes | Yes | Yes |
| `.potm` | `MacroEnabledTemplate` | Yes | Yes | Yes |
| `.ppsm` | `MacroEnabledSlideShow` | Yes | Yes | Yes |

Creating a macro-enabled package does not create a VBA project; it selects the
content types and the extension that permit one. Attaching a project is an
Excel-only capability today — see [VBA projects](#cross-cutting-subsystems)
below.

### Other formats

| Format | Direction | Fidelity | CTest label | Notes |
| --- | --- | --- | --- | --- |
| Flat OPC (`.xml`) | Both | Lossless | `formats-flatopc` | The whole package as a single XML document. `exyoki to-flat-opc` / `from-flat-opc`, see [exyoki](tools/exyoki.md). |
| Semantic JSON, semantic XML | Both | Lossy | `formats-conversion` | The canonical serialization of the conversion model. Constructs outside the model are dropped with a warning that names the location. See [Conversion formats](tools/conversion-formats.md). |
| Markdown | Both | Lossy | `formats-conversion` | Structure-preserving, in both directions, for all three families — a `.docx`, `.xlsx`, or `.pptx` can be rendered to Markdown and a Markdown file converted into any of them. |
| Plain text | Both | Lossy | `formats-conversion` | Rendered from all three families; converted back into **Word only** (one paragraph per line). An Excel or PowerPoint target is rejected with an error. |
| `.doc`, `.xls`, `.ppt` | — | — | — | The legacy binary formats are not read and not written. |
| PDF, HTML, RTF, ODF, images | — | — | — | Out of scope; see [Out of scope](#out-of-scope). |

## Office versions

`ExyokiOffice::OpenXml::FileFormatVersions` names the Office releases the
library knows about. It appears wherever a target version has to be chosen:

| Value | Office release | `exyoki --office-version` |
| --- | --- | --- |
| `Office2007` | Office 2007 — the OOXML baseline | `2007` |
| `Office2010` | Office 2010 | `2010` |
| `Office2013` | Office 2013 | `2013` |
| `Office2016` | Office 2016 | `2016` |
| `Office2019` | Office 2019 | `2019` |
| `Office2021` | Office 2021 / LTSC | `2021` |
| `Microsoft365` | Microsoft 365 | `365` |

**The target version is a checking knob, not a save-as knob.** Setting it does
not down-convert anything. ExyokiOffice writes the OOXML the document
already uses, and a document authored with constructs introduced in a later
release stays that way. There is no "save as Office 2010" conversion, and this
is intentional: silently rewriting content to fit an older schema is exactly
the kind of lossy interpretation the library avoids elsewhere.

What the target version does control:

- **Validation.** Parts, elements, attributes, and individual enumeration
  values carry the release that introduced them. Validating against an older
  target reports everything newer than it as unavailable, so a *lower* target
  surfaces *more* findings, not fewer. Both `OpenXmlDomValidator` and
  `OpenXmlPackageValidator` take the target through
  `OpenXmlDomValidationSettings::TargetVersion`.
- **Markup compatibility.** When a document offers several renderings of the
  same content through `mc:AlternateContent`, the target version decides which
  `mc:Choice` branch is selected and which are discarded. This is set through
  `MarkupCompatibilityProcessSettings::TargetFileFormatVersions`, reachable at
  open time as `OpenSettings::MarkupCompatibility`.

The two defaults differ, which is worth knowing before debugging a surprise:

| Setting | Default | Effect of the default |
| --- | --- | --- |
| `OpenXmlDomValidationSettings::TargetVersion` | `Microsoft365` | The most permissive target; nothing is rejected for being too new. |
| `Tools::ValidationRunOptions::TargetVersion` | `Microsoft365` | Same, for `exyoki validate` and the `Tools` validation runner. |
| `MarkupCompatibilityProcessSettings::TargetFileFormatVersions` | `Office2007` | The most conservative target; the widest-compatibility branch wins. |

Everything in this section is covered by the `office-versions` label.

Validating against the oldest release a document has to survive is the useful
workflow:

```powershell
exyoki validate report.docx --office-version 2010
```

## Word

Chapter links point at the guide for each area; see the
[Word quickstart](Word.md) for the API tour.

| Feature area | Create | Edit | Preserve | CTest label | Notes |
| --- | --- | --- | --- | --- | --- |
| [Documents and lifecycle](word/documents.md) | Yes | Yes | Yes | `word-documents` | All four document types, templates, transactions, properties, themes. |
| [Text and paragraphs](word/text.md) | Yes | Yes | Yes | `word-text` | Body cursors, runs, character and paragraph formatting, find and replace including regular expressions. |
| [Styles, headings, lists](word/styles.md) | Yes | Yes | Yes | `word-styles` | `StyleManager`, latent styles, multi-level numbering. |
| [Tables](word/tables.md) | Yes | Yes | Yes | `word-tables` | Rows, columns, merges, nesting, the logical grid. |
| [Images](word/images.md) | Yes | Yes | Yes | `word-images` | Inline and floating, wrapping, crop, alt text. `DetectImageFormat` does not sniff EMF/WMF/TIFF payloads. |
| [Hyperlinks and bookmarks](word/hyperlinks.md) | Yes | Yes | Yes | `word-hyperlinks` | External and internal links, bookmark ranges. |
| [Fields and TOC](word/fields.md) | Yes | Yes | Yes | `word-fields` | Field instructions and cached results are authored; **values are never computed**. A layout-dependent field refuses a written result outright and is marked dirty so Word refreshes it. |
| [Embedded charts](word/charts.md) | No | Yes | Yes | `word-charts` | Existing charts and their embedded workbooks are read and updated. Anchoring a *new* chart has no high-level helper. |
| [Sections and page setup](word/sections.md) | Yes | Yes | Yes | `word-sections` | Page size, margins, columns, headers and footers. |
| [Notes and comments](word/notes.md) | Yes | Yes | Yes | `word-notes` | Footnotes, endnotes, comments, threaded comments with replies and resolution. |
| [Content controls](word/notes.md) | Partial | Partial | Yes | `word-content-controls` | Inline controls with tag, alias, lock, and text. Specialized control types are preserved without a typed API. |
| [Revisions](word/revisions.md) | Partial | Yes | Yes | `word-revisions` | Existing revisions are enumerated, accepted, and rejected. Revisions are produced by `CompareWith`, a conservative paragraph-level comparison — there is no "record my edits" tracking mode. |
| [Merging and mail merge](word/revisions.md) | Yes | Yes | Yes | `word-merge` | `InsertDocument`, `MergeTemplate` over `MERGEFIELD`, bookmarks, and repeating regions. |
| [Document protection](word/protection.md) | Yes | Yes | Yes | `word-protection` | Editing restrictions with a password verifier. Not encryption — see the note under [Cross-cutting subsystems](#cross-cutting-subsystems). |
| SmartArt, text boxes, equations, OLE objects | No | No | Yes | `word-preserved` | No typed API at any level above the DOM; round-tripped untouched. |

## Excel

See the [Excel quickstart](Excel.md) for the API tour.

| Feature area | Create | Edit | Preserve | CTest label | Notes |
| --- | --- | --- | --- | --- | --- |
| [Workbooks](excel/workbooks.md) | Yes | Yes | Yes | `excel-workbooks` | All four document types, snapshots, properties, themes, workbook protection. |
| [Worksheets](excel/worksheets.md) | Yes | Yes | Yes | `excel-worksheets` | Add, copy (including across workbooks), move, remove, sheet protection. |
| [Cells and ranges](excel/cells.md) | Yes | Yes | Yes | `excel-cells` | Typed values, shared strings, range copy/move, row and column insertion and deletion, merges. Addressing itself is `excel-addresses`. |
| Rich-text cell content | No | No | Yes | `excel-rich-text` | Rich-text shared-string runs round-trip; `SetCellText` writes plain-text shared strings. |
| [Styles and number formats](excel/styles.md) | Yes | Yes | Yes | `excel-styles` | `StyleRepository`: fonts, fills, borders, number formats. |
| [Named ranges](excel/named-ranges.md) | Yes | Yes | Yes | `excel-named-ranges` | Workbook and worksheet scope. Structural edits do **not** rewrite defined-name formulas. |
| [Formulas](excel/formulas.md) | Yes | Yes | Yes | `excel-formulas` | Stored as text and rewritten on structural edits. `FormulaEngine` evaluates on demand and implements a documented subset of Excel's function library. |
| External workbook references | No | Partial | Yes | `excel-external-refs` | Preserved, but structural edits do not update references into other workbooks. |
| [Tables and auto-filters](excel/tables.md) | Yes | Yes | Yes | `excel-tables` | |
| [Charts](excel/charts.md) | Yes | Yes | Yes | `excel-charts` | `ChartBuilder`, series, cross-sheet sources. |
| [Pivot tables](excel/pivot-tables.md) | Yes | Yes | Yes | `excel-pivot` | Caches, records, definitions, aggregation, refresh. |
| [Slicers](excel/slicers.md) | Yes | Yes | Yes | `excel-slicers` | Over pivot tables and worksheet tables, with shared caches. |
| [Data validation](excel/validation.md) | Yes | Yes | Yes | `excel-validation` | |
| [Conditional formatting](excel/validation.md) | Yes | Yes | Yes | `excel-conditional-formatting` | Rules reference `dxfs` differential formats by index but do not create them. |
| [Layout and annotations](excel/layout.md) | Yes | Yes | Yes | `excel-layout` | Row/column dimensions, views and frozen panes, hyperlinks, comments, threaded comments, images. |
| [Printing](excel/printing.md) | Yes | Yes | Yes | `excel-printing` | Page setup, margins, print areas and titles, headers and footers. |

## PowerPoint

See the [PowerPoint quickstart](PowerPoint.md) for the API tour.

| Feature area | Create | Edit | Preserve | CTest label | Notes |
| --- | --- | --- | --- | --- | --- |
| [Presentations](powerpoint/presentations.md) | Yes | Yes | Yes | `ppt-presentations` | All six document types, slide size, handout settings, modify protection. Presentation-level metadata beyond those has no typed wrapper. |
| [Slides](powerpoint/slides.md) | Yes | Yes | Yes | `ppt-slides` | Add, copy (including from another presentation), move, remove; `SlideBuilder`. |
| [Sections and custom shows](powerpoint/slides.md) | Yes | Yes | Yes | `ppt-sections` | |
| [Masters and layouts](powerpoint/masters.md) | Yes | Partial | Yes | `ppt-masters` | Masters and layouts are created, removed, and assigned, and placeholders added. They expose no `ShapeTree()`, so positioning their own shapes needs the DOM. |
| [Shapes](powerpoint/shapes.md) | Yes | Yes | Yes | `ppt-shapes` | Shape tree, preset and freeform geometry, connectors, transforms, fills, outlines, effects. |
| [Text](powerpoint/text.md) | Yes | Yes | Yes | `ppt-text` | Text frames, paragraphs, runs, bullets. `PresentationTextRun` exposes bold, italic, language, and hyperlinks only; other run formatting needs the DOM. |
| [Pictures and media](powerpoint/pictures-and-media.md) | Yes | Yes | Yes | `ppt-media` | Embedded pictures and audio/video. Linked media is preserved but never resolved, downloaded, or played. |
| [DrawingML tables](powerpoint/pictures-and-media.md) | Yes | Yes | Yes | `ppt-tables` | Including row and column insertion and removal, and merged cells. |
| [Charts](powerpoint/charts.md) | Yes | Yes | Yes | `ppt-charts` | Creation, cached data, embedded workbooks, chart and colour style XML. |
| [Transitions](powerpoint/transitions.md) | Yes | Yes | Yes | `ppt-transitions` | Typed effects and timing; effects outside the typed set are preserved. |
| [Animations](powerpoint/animations.md) | Yes | Yes | Yes | `ppt-animations` | Sequences, effects, triggers, reordering, and a removal policy for shapes that are animated. |
| [Notes and comments](powerpoint/notes-and-comments.md) | Yes | Yes | Yes | `ppt-notes` | Speaker notes, comments, comment authors, comment status. |
| SmartArt, OLE objects | No | No | Yes | `ppt-preserved` | Round-tripped untouched. |

## Cross-cutting subsystems

| Subsystem | Word | Excel | PowerPoint | CTest label | Notes |
| --- | --- | --- | --- | --- | --- |
| OPC package round-trip | Yes | Yes | Yes | `opc-roundtrip` | Parts, content types, and the relationship graph, including external relationships and parts the library does not model. |
| Unknown and vendor-specific parts | Preserved | Preserved | Preserved | `opc-unknown-parts` | Carried through as opaque parts; the relationship graph keeps them attached. |
| Markup compatibility (`mc:*`) | Yes | Yes | Yes | `markup-compatibility` | `AlternateContent` branch selection, `Ignorable`, `ProcessContent`, `MustUnderstand`. See [Office versions](#office-versions). |
| Schema and semantic validation | Yes | Yes | Yes | `validation` | `OpenXmlDomValidator` and `OpenXmlPackageValidator`, with an explicit target version and positional diagnostics. Content models are decided by an automaton compiled from the schema particle tree; `exyoki validate --cross-check-content-model` re-checks every verdict against the recursive matcher it replaced. |
| Document properties | Yes | Yes | Yes | `properties` | Core, extended, and custom properties. |
| Themes | Yes | Yes | Yes | `themes` | |
| VBA projects | Preserved | **Yes** | Preserved | `vba` | Excel can extract, replace, and remove the opaque `vbaProject.bin` payload and converts the document type accordingly. Word and PowerPoint round-trip the project but have no extraction or replacement API. VBA code is never interpreted or executed. |
| Protection | Yes | Yes | Yes | `protection` | Editing/modify restrictions with a password verifier. **Not encryption**: every part stays readable and any tool that ignores the setting can rewrite the document. |
| [Digital signatures](Signatures.md) | Yes | Yes | Yes | `signatures` | Verification and signing through an application-supplied `ICryptoProvider`; the library links no cryptographic code of its own. |
| OOXML package encryption | No | No | No | `encryption` | An encrypted OOXML file is a compound-file container rather than a ZIP package, so it cannot be opened at all. Encryption is not implemented and is tracked separately from signing. |
| [External resources](ExternalResources.md) | Yes | Yes | Yes | `external-resources` | Off by default. The library never fetches anything on its own; an application supplies a resolver and the library enforces the policy around it. Opening, saving, and validating never call a resolver. |

## Out of scope

These are permanent boundaries, not gaps waiting to be filled. ExyokiOffice
is an editing library:

- **No rendering, layout, or pagination.** Nothing computes page counts, line
  breaks, or the position of content on a page. This is why Word fields are
  never evaluated.
- **No printing.**
- **No conversion to PDF, SVG, or raster images.**
- **No legacy binary formats** — `.doc`, `.xls`, `.ppt` are neither read nor
  written.
- **No ODF, RTF, or HTML** — neither read nor written.
- **No macro execution.** VBA projects are binary payloads; their contents are
  never parsed or run.

What *is* supported, and is easy to mistake for one of the above, is the
conversion between Office packages and the Markdown, JSON, semantic XML, and
plain-text formats in [Conversion formats](tools/conversion-formats.md).
Authoring a `.docx` from a Markdown file, or rendering one to Markdown, is a
normal use of the library and needs no other tool. Those conversions are lossy
by design — they carry what the semantic model covers and emit a diagnostic
naming everything they drop — and they are not a route to the formats listed
above.

## Keeping this page current

Every change to a modelled feature updates its row here in the same change
that ships it, and a row is graded against what the public API actually does,
not what it is expected to do next. A missing row is better than an optimistic
one: applications choose this library on the strength of these tables, and a
wrong `Yes` costs more than a candid `Partial`.

A new row brings a label and a test with it. Add the area to the layer's
`CMakeLists.txt` with `exyokioffice_add_test_area`, tag the test cases that
belong to it, and add the case in [`tests/compat/`](../tests/compat/) that
grades the row. `ctest --print-labels` lists every label that exists; a row
citing a label that is not in that list is a documentation bug.
