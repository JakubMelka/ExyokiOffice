# MCP server review

A pre-release audit of the three MCP servers against the capabilities the
library itself documents in [docs/Compatibility.md](docs/Compatibility.md).

The goal it is measured against: **every feature area the compatibility matrix
grades as supported should be reachable through an MCP tool.** Where that is
not intended, the gap belongs in
[Limits of this version](docs/tools/mcp-servers.md#limits-of-this-version) as a
decision, not as an absence.

Reviewed at 148 tools (Word 50, Excel 48, PowerPoint 50). P0 is done and P1 is
done; the catalog now stands at 195 (Word 63, Excel 67, PowerPoint 65).

## Contents

- [Test coverage](#test-coverage)
- [Library defects the oracle found](#library-defects-the-oracle-found)
- [Gap A: asymmetries between the three servers](#gap-a-asymmetries-between-the-three-servers)
- [Gap B: library areas with no tool at all](#gap-b-library-areas-with-no-tool-at-all)
- [Gap C: tools shallower than the library](#gap-c-tools-shallower-than-the-library)
- [Gap D: missing in the library too](#gap-d-missing-in-the-library-too)
- [Backlog](#backlog)
- [Catalog size](#catalog-size)

## Test coverage

| Layer | Extent |
| --- | --- |
| C++ ([tests/mcp](tests/mcp)) | 145 `TEST_CASE`, 5 624 lines, six CTest areas |
| Python black box ([tests/mcp_python](tests/mcp_python)) | 69 tests over the official MCP SDK, across real stdio |
| Catalog | `CheckMcpCatalog.cmake` compares [docs/schemas](docs/schemas) against the live server |

Every one of the tools has at least one call site in the C++ tests, and the
Python suite validates every input and output schema, rejects an unknown
property on every one of them, and checks the text block against
`structuredContent`.
That part is in good shape.

Two weaknesses:

- **Happy path only for 26 tools.** These have exactly one call site, so no
  invalid input, no boundary, no failure mode is exercised: Word
  `list_styles`, `list_revisions`, `delete_blocks`, `resolve_revisions`,
  `add_note`, `compare_documents`; Excel `modify_sheet_structure`, `add_table`,
  `add_data_validation`; PowerPoint `list_layouts`, `move_slide`,
  `set_slide_hidden`, `delete_shape`, `add_chart`, `set_notes`,
  `set_transition`, `set_slide_size`; and `diff_documents`, `merge_documents`,
  `redact_document` in all three. The structural ones matter most — an empty
  range, the last element, an index past the end.
- **Nothing checks that a document produced through MCP opens in Office.** The
  suites validate OPC structure and markup schema, which is not the same thing.
  A COM oracle gate belongs in the release checklist.

  Running that gate by hand found five defects nothing else could have caught,
  in files that were valid throughout. Three are recorded under
  [Library defects the oracle found](#library-defects-the-oracle-found) because
  they are the library's, not the servers'; the other two were the servers': a shape given neither
  a fill nor an outline drew nothing at all, because a new `p:sp` carries no
  style reference to inherit from, and PowerPoint showed only its text floating
  over the slide; and a connector bound to two shapes was invisible too,
  because a connection says which shapes a connector joins without placing it,
  so it kept the default zero extent. Both are fixed and covered by tests, but
  neither is expressible as a schema or OPC violation - only rendering shows
  them.

Every tool added by this review is expected to arrive with both the happy path
and its invalid-input cases covered.

## Library defects the oracle found

**Nine defects, eight in the library and one in its package validator, all now
fixed.**
Five produced a file that round-tripped through the library's own tests and
validated as OPC and against the schema, while Excel threw the feature away; the
sixth failed validation and nobody had looked. Each construct is graded `Yes` in
the compatibility matrix, so the matrix was overstating what shipped.

| Construct | Matrix | What Office did | Cause | Fix |
| --- | --- | --- | --- | --- |
| Threaded comments (`excel-layout`) | Yes | Dropped `xl/threadedcomments/*` entirely; `CommentsThreaded.Count` was 0 | A thread is not self-sufficient: Excel requires a legacy note carrying the flattened conversation, and that note's VML box | `AddThreadedComment` writes the backing note, its VML drawing and the `legacyDrawing` reference; `Comments()` hides the backing, and every rewrite of the comments part restores it |
| Table slicers (`excel-slicers`) | Yes | Dropped the cache and the drawing, leaving an orphaned `xl/slicers/slicer1.xml` | Two causes at once: the worksheet `slicerList` extension URI was wrong, and the cache had no workbook defined name | Table slicers register under `{3A4CF648-…}`; every slicer cache gets a `#N/A` defined name |
| Pivot slicers (`excel-slicers`) | Yes | Refused to open the workbook at all, then discarded the slicer once it opened | Three causes: the pivot `slicerList` URI was wrong in its last segment, the slicer cache named a `pivotCacheId` no extension declared, and the pivot table still claimed the Excel 2007 feature version | Pivot slicers register under `{A8765BA9-…-ACF838C121DE}`; the pivot cache declares its identifier; hosting a slicer raises `updatedVersion` to 4 |
| Pivot slicers on a reordered workbook (`excel-slicers`) | Yes | Discarded the slicer whenever a sheet's position and its `sheetId` differed | The cache's `tabId` was written as the tab position; Excel reads it as the `sheetId` | `SheetTabId` reads the `sheetId` off the workbook sheet list |
| Table totals row (`excel-tables`) | Yes | Refused to open the workbook | `SetTotalsRowShown` flipped two attributes without growing the table, which left the auto-filter covering the totals row | The table reference grows by a row and the auto-filter stays below it; `Resize` and `SetAutoFilterEnabled` hold the same invariant |
| Modify protection (`ppt-presentations`) | Yes | Validation reported seven missing required attributes, and a presentation PowerPoint protected could not be unprotected at all | `p:modifyVerifier` was written with the ISO attribute group, which `CT_ModifyVerifier` declares optional; the seven it declares required are the ones PowerPoint writes, and the reader treated them as a legacy form it could not validate | The writer emits the seven required attributes; the reader accepts both groups. The hash itself was already right: recomputing PowerPoint's own `hashData` with the ISO formula reproduces it byte for byte |
| Image alt text (`word-images`) | Yes | Reported no alternative text at all; `InlineShape.AlternativeText` was empty | `SetAltText` wrote only the picture's `pic:cNvPr`. Word reads the drawing's `wp:docPr` and writes both | Both are written, and reading prefers `wp:docPr`. An accessibility feature that silently labels nothing is worse than one that is absent |
| Embedded media (`ppt-media`) | Yes | Opened, then dropped the media part on its own save and rewrote the relationship as external | `a:audioFile` can only name a relationship through `r:link`, which reads as a link whatever the target is. PowerPoint tells the two apart by a second relationship and a `p14:media` extension naming it | Both are written. Verified by having PowerPoint re-save the file: before, the package came back with no media part at all; after, it keeps the part, both internal relationships, the extension, and the volume and loop settings |
| Relationship type check (the validator itself) | — | Reported an error on a presentation PowerPoint wrote | The rule assumed every incoming relationship to a part carries the part's own descriptor type; OPC allows several, and Office relies on it | The rule reports only a part reached solely by a wrong-typed relationship. A gate that rejects Office's own output is worse than no gate |

Each cause was isolated by bisecting against a file Excel itself wrote: our
parts were swapped into a working reference one at a time until it broke, then
the differences inside the guilty part were bisected the same way. Every fix is
confirmed the way the defect was found — Excel reports the object through its
object model — and is guarded by a test.

The last one had gone unnoticed because every test and every sample had the two
numbers coincide; they diverge as soon as a sheet is reordered or deleted.

Three things are worth carrying forward:

- **A wrong extension URI is invisible to every check but Excel.** The header
  that defines these URIs said so in its own doc comment, and the unit tests
  asserted the typo, which is what kept all three defects alive. URIs are now
  transcribed from files Excel wrote, and the tests say so.
- **Excel with `DisplayAlerts = $false` repairs silently**, where PowerPoint
  with alerts off refuses the file outright. An Excel workbook that opens is
  therefore not evidence of anything; ask the object model what it sees, or have
  Excel re-save the file and compare the parts.
- **An identifier that happens to match is not an identifier that is right.**
  A sheet's position equals its `sheetId` in every workbook built front to back,
  which is every workbook a test builds. Cases like this need a fixture that
  makes the two differ on purpose.
- **"Legacy" was an assumption, not an observation.** The modify verifier was
  written in the newer of two attribute groups and the older one was documented
  as pre-2010 and unvalidatable. Current PowerPoint writes only the older group,
  the schema requires it, and the two carry the same values under different
  names. One presentation saved by PowerPoint settled all three points.

## What the oracle cannot reach

One area is now covered by tools and not by the gate: **the VBA project**.
Verifying that Excel accepts a project the server embedded needs a genuine
`vbaProject.bin`, and producing one means turning on trusted access to the VBA
object model — a security setting on the machine, not something a test may
change. The round trip is covered instead: the payload is opaque from end to
end, what comes out is byte-for-byte what went in, and the package becomes
macro-enabled. Anyone releasing this should say plainly that the VBA path is
tested for fidelity rather than for acceptance.

## Gap A: asymmetries between the three servers

These read as oversights rather than decisions, because the same capability is
present in one server and absent in its sibling.

| Capability | Word | Excel | PowerPoint |
| --- | --- | --- | --- |
| Comments | yes | **no** | yes |
| Images | yes | **no** | yes |
| Charts | **no** | yes | yes |
| Tables | yes | yes | yes |
| Hyperlinks | yes (inline `link`) | yes | yes (run `link`) |

The library supports all three missing cells: `ExcelDocument::SetComment` and
`AddThreadedComment`, `ExcelDocument::AddImage`, and — for Word — reading and
updating an existing chart and its embedded workbook (`word-charts`, Edit
`Yes`).

## Gap B: library areas with no tool at all

Each row is graded supported in the compatibility matrix and has zero tools.

| Area | Family | Library surface |
| --- | --- | --- |
| Printing (`excel-printing`) | Excel | `GetPageSetup`/`SetPageSetup`, `SetHeaderFooter`, `SetPrintArea`, `SetPrintTitles`, `PrintOptions` |
| Comments (`excel-layout`) | Excel | `SetComment`, `GetComment`, `Comments`, `RemoveComment`, `AddThreadedComment`, `ThreadedComments`, `RemoveThreadedComment` |
| Images (`excel-layout`) | Excel | `AddImage`, `Images`, `RemoveImage`, `ExcelWorksheetImage` |
| Slicers (`excel-slicers`) | Excel | `ExcelSlicer.hpp`, over pivot tables and worksheet tables |
| VBA projects (`vba`) | Excel | ~~extract, replace, remove `vbaProject.bin`~~ done |
| Animations (`ppt-animations`) | PowerPoint | sequences, effects, triggers, reordering, removal policy |
| Shapes beyond a text box (`ppt-shapes`) | PowerPoint | preset and freeform geometry, connectors, fills, outlines, effects |
| Custom shows (`ppt-sections`) | PowerPoint | named shows over a slide subset |
| Style definitions (`word-styles`) | Word | `StyleManager`, latent styles, multi-level numbering |
| Content controls (`word-content-controls`) | Word | inline controls with tag, alias, lock, text |
| Protection (`protection`) | all three | editing restrictions with a password verifier |
| Themes (`themes`) | all three | ~~`ThemeService`~~ done |

## Gap C: tools shallower than the library

| Tool | Missing against the library |
| --- | --- |
| Word `insert_table` | cell shading, borders, column widths, formatted text per cell; `data` is plain strings |
| Word `insert_image` | floating placement, text wrapping, crop; the tool is inline-only |
| Word `set_section` | columns |
| Word `apply_style` | character styles; only paragraph styles are applied |
| Excel `add_sheet` family | move, copy (including across workbooks), sheet protection |
| Excel ranges | copy and move a range |
| Excel `add_conditional_formatting` | ~~the ranking and average rule kinds, several ranges per rule~~ done |
| Excel `add_table` | ~~listing tables, auto-filter, column filters, totals row~~ done |
| Excel `add_chart` | cross-sheet sources, secondary axis, combined types |
| PowerPoint `list_layouts` | creating, removing, assigning masters and layouts, adding placeholders |
| PowerPoint `add_image` | embedded audio and video |
| PowerPoint `add_chart` | the same chart-type ceiling as Excel |

## Gap D: missing in the library too

Out of scope for this review; a tool cannot be written for these until the
library grows one. Recorded so the list is not mistaken for an MCP gap:
SmartArt (all families), Word text boxes and equations, a *new* chart anchor in
Word, rich-text cell content in Excel, OOXML package encryption.

Three more were found while working through P1, and two of them are worth a
decision before the announcement rather than after it. The first of the three
was implemented after this review was written and is struck through:

| Missing | Consequence |
| --- | --- |
| ~~**Differential formats (`dxfs`)**~~ | **Closed.** `StyleRepository::GetOrAddDifferentialFormat()` registers one and `add_conditional_formatting` takes a `format` argument that interns it, so a rule now paints the cells it matches. Verified against Excel: our `dxf` and the one Excel writes for the same rule are identical property by property, and `DisplayFormat` on a matching cell reports the painted fill, font, border and number format. The trap was that a `dxf` solid fill carries only `bgColor` — the cell form, `patternType` plus `fgColor`, validates and paints nothing. |
| **Colour scales, data bars, icon sets** | Absent from the library, present only in the generated DOM. This was recorded under Gap C as a tool gap; it is not one. |
| **Table sort state** | `SortState` exists only in the generated DOM. Also recorded under Gap C by mistake. |

## Backlog

### P0 — the asymmetries and the one wholly missing area

- [x] Excel `add_comment` / `list_comments` / `delete_comment`, plain and threaded
- [x] Excel `add_image`
- [x] Excel `set_print_setup` — page setup, margins, print area, print titles, header and footer
- [x] PowerPoint `add_shape` and `format_shape` — preset geometry, connector, fill, outline
- [x] Word `format_table` — shading, borders, column widths
- [x] Word `update_chart` — rewrite the series of a chart already in the document

### P1 — the rest of Gap B and Gap C

- [x] PowerPoint animations
- [x] PowerPoint masters and layouts, write side
- [x] PowerPoint custom shows; audio and video
- [x] Excel slicers
- [x] Excel sheet move, copy, protection; range copy and move
- [x] Excel table listing, auto-filter and column filters; the ranking and
      average conditional-format rules. Sort state, colour scales and data bars
      turned out to be Gap D rather than Gap C — see above
- [x] Excel VBA extract, replace, remove
- [x] Word style definitions and numbering, and `insert_list` continuing an
      existing sequence, which its description already claimed
- [x] Word floating images with wrapping; section columns; character styles
- [x] Word content controls
- [x] Themes, all three families
- [x] Document protection for Word and PowerPoint, and `get_document_info`
      reporting what a document restricts

### P2 — decisions to state rather than gaps to close

- [ ] Record SmartArt, equations, encryption and signatures in
      [Limits of this version](docs/tools/mcp-servers.md#limits-of-this-version),
      each with what happens instead — SmartArt round-trips untouched
- [ ] Answer an unsupported request with `unsupported` and a hint rather than
      `Unknown tool`, so the boundary is legible from inside a conversation

### Testing, alongside every item above

- [ ] Invalid-input and boundary cases for the 26 single-call-site tools
- [ ] A COM oracle gate: open every MCP-produced document in real Office

## Catalog size

Fifty tools per server is near the point where a model starts reaching for the
wrong one, and the catalog is the largest fixed cost each server imposes on a
client's context window. P0 and P1 together add roughly 25 more. `--toolsets`
already exists to bound this; what it needs is for each new tool to land in a
group that is meaningful to filter on, and a decision about whether the default
should stay "everything".
