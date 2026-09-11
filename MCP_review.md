# MCP server review

A pre-release audit of the three MCP servers against the capabilities the
library itself documents in [docs/Compatibility.md](docs/Compatibility.md).

The goal it is measured against: **every feature area the compatibility matrix
grades as supported should be reachable through an MCP tool.** Where that is
not intended, the gap belongs in
[Limits of this version](docs/tools/mcp-servers.md#limits-of-this-version) as a
decision, not as an absence.

Reviewed at 148 tools (Word 50, Excel 48, PowerPoint 50). P0 is done: the
catalog now stands at 158 (Word 53, Excel 53, PowerPoint 52).

## Contents

- [Test coverage](#test-coverage)
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

Every one of the 148 tools has at least one call site in the C++ tests, and the
Python suite validates every input and output schema, rejects an unknown
property on all 148, and checks the text block against `structuredContent`.
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

  Running that gate by hand against the P0 work found two defects nothing else
  could have caught, in files that were valid throughout: a shape given neither
  a fill nor an outline drew nothing at all, because a new `p:sp` carries no
  style reference to inherit from, and PowerPoint showed only its text floating
  over the slide; and a connector bound to two shapes was invisible too,
  because a connection says which shapes a connector joins without placing it,
  so it kept the default zero extent. Both are fixed and covered by tests, but
  neither is expressible as a schema or OPC violation - only rendering shows
  them.

Every tool added by this review is expected to arrive with both the happy path
and its invalid-input cases covered.

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
| VBA projects (`vba`) | Excel | extract, replace, remove `vbaProject.bin`, with document type conversion |
| Animations (`ppt-animations`) | PowerPoint | sequences, effects, triggers, reordering, removal policy |
| Shapes beyond a text box (`ppt-shapes`) | PowerPoint | preset and freeform geometry, connectors, fills, outlines, effects |
| Custom shows (`ppt-sections`) | PowerPoint | named shows over a slide subset |
| Style definitions (`word-styles`) | Word | `StyleManager`, latent styles, multi-level numbering |
| Content controls (`word-content-controls`) | Word | inline controls with tag, alias, lock, text |
| Protection (`protection`) | all three | editing restrictions with a password verifier |
| Themes (`themes`) | all three | `ThemeService` |

## Gap C: tools shallower than the library

| Tool | Missing against the library |
| --- | --- |
| Word `insert_table` | cell shading, borders, column widths, formatted text per cell; `data` is plain strings |
| Word `insert_image` | floating placement, text wrapping, crop; the tool is inline-only |
| Word `set_section` | columns |
| Word `apply_style` | character styles; only paragraph styles are applied |
| Excel `add_sheet` family | move, copy (including across workbooks), sheet protection |
| Excel ranges | copy and move a range |
| Excel `add_conditional_formatting` | colour scales and data bars |
| Excel `add_table` | auto-filter and sort state |
| Excel `add_chart` | cross-sheet sources, secondary axis, combined types |
| PowerPoint `list_layouts` | creating, removing, assigning masters and layouts, adding placeholders |
| PowerPoint `add_image` | embedded audio and video |
| PowerPoint `add_chart` | the same chart-type ceiling as Excel |

## Gap D: missing in the library too

Out of scope for this review; a tool cannot be written for these until the
library grows one. Recorded so the list is not mistaken for an MCP gap:
SmartArt (all families), Word text boxes and equations, a *new* chart anchor in
Word, rich-text cell content in Excel, OOXML package encryption.

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
- [ ] PowerPoint masters and layouts, write side
- [ ] PowerPoint custom shows; audio and video
- [ ] Excel slicers
- [x] Excel sheet move, copy, protection; range copy and move
- [ ] Excel auto-filter and sort; colour scales and data bars
- [ ] Excel VBA extract, replace, remove
- [ ] Word style definitions and numbering
- [ ] Word floating images with wrapping; section columns; character styles
- [ ] Word content controls
- [ ] Themes, all three families; document protection for Word and PowerPoint

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
