# exyoki conversion formats

This document specifies the formats produced and consumed by
`exyoki convert` (and by the underlying `ExyokiOffice::Tools` conversion
API): the **semantic JSON envelope**, its **semantic XML** projection, the
**Markdown** conventions, the **plain text** rendering, and the **CSV**
mapping for workbooks. The primary audience is automated consumers — AI
agents and scripts that read, transform, and regenerate Office documents.

Related commands:

- `exyoki convert` — the conversions described here (see
  [exyoki.md](exyoki.md#convert--convert-between-office-and-ai-friendly-formats)).
- `exyoki to-flat-opc` / `from-flat-opc` — **lossless** XML round trip of the
  complete package (Microsoft Flat OPC). Use Flat OPC when byte-level
  fidelity matters; use the formats here when you want clean, semantic,
  AI-friendly content.

## Design contract

The conversion goes through a **semantic document model** shared by all
formats. JSON is the canonical serialization of that model; semantic XML is a
mechanical projection of the same tree; Markdown is a structure-preserving
but lossy rendering; plain text keeps only text.

- The model captures what the ExyokiOffice high-level editors model:
  paragraphs, runs and character formatting, styles by ID, tables including
  merges, lists, hyperlinks, images, footnotes/endnotes, comments,
  headers/footers, worksheets/cells/formulas, slides/shapes/text
  frames/notes.
- Constructs outside the model (charts, SmartArt, OLE objects, content
  controls, floating-image positioning, conditional formatting, tracked
  revisions, ...) are **dropped with a `warning` diagnostic** naming the
  location. Nothing is dropped silently.
- The serializer omits fields at their default value (`false`, `0`, empty
  string, empty array), so the JSON stays small; absent = default.
- The parser is tolerant: unknown keys are ignored, scalar types are coerced
  (`"true"`, `"42"`), a missing `version` is assumed to be `1`, and a newer
  version is parsed best-effort with a warning.
- The JSON envelope has a machine-readable schema, so a generator can check
  its output before handing it to `convert` — see [JSON Schema](#json-schema).

## The envelope (JSON)

```json
{
  "format": "exyokioffice-document",
  "version": 1,
  "family": "word",
  "properties": { "title": "…", "creator": "…" },
  "media": [
    { "id": "media1", "file": "report_media/image1.png", "contentType": "image/png" }
  ],
  "document": { }
}
```

| Field | Type | Meaning |
| --- | --- | --- |
| `format` | string | Always `"exyokioffice-document"`. |
| `version` | int | Model version; currently `1`. |
| `family` | string | `"word"`, `"excel"`, or `"powerpoint"`; selects the `document` schema. |
| `properties` | object | OPC core properties (`title`, `subject`, `creator`, `keywords`, `description`, `lastModifiedBy`, `category`, `created`, `modified`, `application`, `company`); non-empty values only. |
| `media` | array | Media payloads referenced from content by `id`. `file` is a reference relative to the JSON/Markdown file; with `--embed-media` a `data` field carries base64 bytes instead. |
| `document` | object | Family-specific payload, below. |

### Word `document`

```json
{
  "body": [ /* blocks */ ],
  "lists": [ { "id": 3, "levels": [ { "format": "decimal", "text": "%1.", "start": 1 } ] } ],
  "footnotes": [ { "id": 1, "blocks": [ /* blocks */ ] } ],
  "endnotes": [ ],
  "comments": [ { "id": 0, "author": "JM", "initials": "JM", "date": "2026-07-24T10:00:00Z", "blocks": [ ] } ],
  "headers": [ { "kind": "default", "blocks": [ ] } ],
  "footers": [ ]
}
```

**Blocks** (elements of `body`, note `blocks`, cell `blocks`):

| `type` | Fields |
| --- | --- |
| `paragraph` | `style` (style ID), `heading` (1–9, derived from `Heading<N>` style), `align` (`left`/`center`/`right`/`both`/…), `list` (`{id, level}` referencing `lists`), `content` (inline array). |
| `table` | `style`, `rows`: array of `{ "cells": [cell] }`. Cell: `rowSpan`, `colSpan` (default 1), `covered: true` for grid positions covered by a span, `blocks` (nested content; may contain nested tables). |
| `sectionBreak` | — |

**Inlines** (elements of `content`):

| `type` | Fields |
| --- | --- |
| `text` | `text`, plus formatting: `bold`, `italic`, `underline`, `strike`, `caps`, `smallCaps` (booleans), `color` (RRGGBB), `highlight` (token, e.g. `yellow`), `font`, `style` (character style ID), `sizePt`. |
| `link` | `target` (external URL) or `anchor` (internal bookmark), `tooltip`, `content` (inline array). |
| `image` | `media` (media ID), `alt`, `widthEmu`, `heightEmu` (914400 EMU = 1 inch). |
| `footnoteRef` / `endnoteRef` | `id` into `footnotes`/`endnotes`. |
| `commentRef` | `id` into `comments`. |
| `field` | `code` (field instruction, e.g. `PAGE`), `text` (cached result; never evaluated). |
| `break` | `kind`: `line`, `page`, or `column`. |

`lists` records the numbering definitions paragraphs reference: per level a
`format` token (`decimal`, `bullet`, `lowerLetter`, `upperRoman`, …), the
Word `text` pattern (e.g. `%1.`), and the `start` value.

### Excel `document`

```json
{
  "sheets": [ {
    "name": "Data",
    "cells": [
      { "cell": "A1", "type": "string", "value": "Name" },
      { "cell": "B2", "type": "number", "value": "42.5" },
      { "cell": "C2", "type": "bool", "value": "true" },
      { "cell": "D2", "type": "formula", "formula": "SUM(B2:C2)",
        "cached": { "type": "number", "value": "42" } },
      { "cell": "E1", "type": "error", "value": "#DIV/0!" },
      { "cell": "F1", "type": "datetime", "value": "2026-01-01T00:00:00" }
    ],
    "merges": [ "A4:B4" ],
    "tables": [ { "name": "Table1", "range": "A1:C4" } ],
    "hyperlinks": [ { "cell": "A1", "target": "https://example.com", "tooltip": "…" } ]
  } ]
}
```

- Cells are **sparse** and row-major; addresses are A1-style.
- `value` is always a string; `type` says how to interpret it.
- Formulas are stored **as text** (without the leading `=`) plus the cached
  result stored in the package. **Formulas are never evaluated**; a stale
  cache round-trips verbatim. Shared/array formulas degrade to normal
  formulas with a diagnostic.

### PowerPoint `document`

```json
{
  "slides": [ {
    "layout": "Title Slide",
    "hidden": false,
    "shapes": [
      { "type": "placeholder", "placeholder": "title",
        "text": [ { "runs": [ { "text": "Q3 Results", "bold": true } ] } ] },
      { "type": "textBox", "transform": { "x": 457200, "y": 1600200, "cx": 8229600, "cy": 2000000 },
        "text": [ { "level": 1, "runs": [ { "text": "Point A" } ] } ] },
      { "type": "picture", "media": "media1", "alt": "Chart" },
      { "type": "table", "rows": [ [ { "text": [ ] }, { "covered": true } ] ] },
      { "type": "group", "children": [ ] },
      { "type": "other" }
    ],
    "notes": "Speaker notes",
    "comments": [ { "author": "JM", "text": "check numbers" } ]
  } ]
}
```

- Shape `type`: `placeholder` (with `placeholder` token: `title`, `ctrTitle`,
  `subTitle`, `body`, `ftr`, `sldNum`, `dt`, …), `textBox`, `picture`,
  `table`, `group` (recursive `children`), `other` (unsupported content such
  as charts — exported empty with a diagnostic).
- `transform` is in EMU; omitted when the shape has no explicit transform.
- Text frames are arrays of paragraphs `{level, runs}`; runs carry `bold`,
  `italic`, `underline`, `sizePt`, `color`, `link`.
- Table cells hold `text` frames; merged regions use `rowSpan`/`colSpan` on
  the anchor and `covered: true` on covered positions.
- When importing, `layout` and `comments` are informational only (a fresh
  package has a different layout catalog; comments are not written back).

## JSON Schema

Everything above is also stated as a JSON Schema (draft 07), published at
[`docs/schemas/exyokioffice-document-v1.schema.json`](../schemas/exyokioffice-document-v1.schema.json)
and printable from the tool:

```powershell
exyoki schema > exyokioffice-document-v1.schema.json
exyoki schema --check draft.json      # exit 0 conforms, 3 does not
```

The schema is the same document either way — [`exyoki schema`](exyoki.md#schema--the-json-schema-of-the-document-model)
prints what the library builds, and a test fails if the published file has
gone stale. It covers the envelope, the core properties, the media entries
and all three family payloads, including the tagged unions: Word blocks and
inlines are `if`/`then` branches on `type`, so a violation is reported
against the variant the document actually selected rather than against all
of them.

**What it is for.** The prose above tells you what the format means; the
schema lets a generator prove its output belongs to it before `convert` ever
opens it. `ValidateModelJson` in
[`DocumentModelSchema.hpp`](../../include/ExyokiOffice/Tools/DocumentModelSchema.hpp)
is the same check from C++.

**How it stays true.** The schema is written by hand next to the serializer,
not derived from it — the C++ structs are not shaped like the JSON (one flat
`WordInline` becomes eight tagged variants, defaults vanish, `PptTextFrame`
becomes a bare array), so generating it from the types would describe a
format nobody emits. Instead every object in the schema is closed with
`additionalProperties: false`, and the test suite serializes maximal
fixtures — every block type, every inline type, every shape type, every cell
type — plus every document under `corpus/` and validates the result. A field
added to the serializer without a matching schema entry fails those tests
immediately.

**Two deliberate asymmetries**, both in the importer's favour:

- fields the serializer omits at their default value are *optional* in the
  schema, not forbidden;
- booleans accept `false` even though the serializer only ever writes `true`.

So everything the serializer can emit validates, and everything that
validates is accepted by the parser. The reverse does not hold: the parser is
deliberately more tolerant than the schema (it coerces `"42"` to a number and
ignores unknown keys), so a document the schema rejects may still import.

**What it does not cover.** Cross-references are outside a JSON Schema's
reach: a `commentRef` pointing at a missing comment, an `image` naming a
media entry that is not in `media`, a `list` naming a numbering definition
that does not exist. Those are reported by the conversion itself as
diagnostics.

The schema is versioned with the model: `version` is a `const`, the file name
carries the version, and a `version: 2` model would ship
`exyokioffice-document-v2.schema.json` beside this one.

## Semantic XML

The same envelope tree serialized as XML — useful for XML-native pipelines.
The mapping from the JSON tree is mechanical and reversible:

- an object becomes an element; scalar members become **attributes** (all
  attribute values are strings; the parser coerces),
- an array member becomes a container element named after the key, with one
  `<item>` child per entry (`<item value="…"/>` for scalar entries, nested
  `<item>` groups for arrays of arrays),
- the root element is `<eoDocument format="exyokioffice-document" version="1" family="…">`.

Example fragment:

```xml
<eoDocument format="exyokioffice-document" version="1" family="word">
  <document>
    <body>
      <item type="paragraph" style="Heading1" heading="1">
        <content>
          <item type="text" text="Chapter" />
        </content>
      </item>
    </body>
  </document>
</eoDocument>
```

`from-xml` conversion accepts exactly this shape. For byte-level lossless
XML use `to-flat-opc` instead.

## Markdown conventions

Markdown preserves document **structure** (headings, lists, tables, links,
notes) but drops most visual formatting. The supported syntax is a strict
CommonMark/GFM subset:

ATX headings (`#`–`######`), paragraphs, `**strong**`, `*emphasis*`,
`~~strike~~`, `<u>underline</u>`, `` `code` ``, `[text](url "tooltip")`,
`![alt](file)`, `[^label]` footnote references and `[^label]: …`
definitions, `-`/`1.` lists nested by 4-space indentation, GFM pipe tables
(escaped `\|`, `<br>` for in-cell line breaks), fenced code blocks,
blockquotes, `---` thematic breaks, and backslash escapes. Not supported:
setext headings, indented code blocks, reference links, autolinks, raw HTML
other than `<u>` and `<br>`.

### Word ⇄ Markdown

| Word construct | Markdown | Notes |
| --- | --- | --- |
| `Heading1`–`Heading9` style | `#`–`######` | Levels 7–9 clamp to 6 (diagnostic). Import applies the `Heading<N>` style. |
| Bold / italic / strike | `**` / `*` / `~~` | |
| Underline | `<u>…</u>` | |
| Monospace run (Consolas/Courier) | `` `code` `` | Import writes the run with the Consolas font. |
| Hyperlink | `[text](url "tooltip")` | Internal bookmark links use `#anchor` targets. |
| Image | `![alt](media_dir/image1.png)` | Reference into the exported media directory. |
| Bullet / numbered list | `-` / `1.`, 4-space nesting | Ordered-vs-bullet from the list definition's level-0 format. Custom level texts flatten (kept in JSON). |
| Table | GFM pipe table | Merged cells flatten to empty covered cells (kept in JSON); `<br>` separates in-cell paragraphs; nested tables flatten (diagnostic). |
| Footnote / endnote | `[^1]` / `[^e1]` + definitions | Endnotes use the `e` label prefix. |
| Section break | `---` | |
| `Quote` style paragraphs | `> blockquote` | |
| `HTMLPreformatted` style paragraphs | fenced code block | |
| Headers/footers, comments | **dropped** (diagnostic) | Kept in JSON. |
| Fields | cached result text | The instruction is kept only in JSON. |

### Excel ⇄ Markdown

Each sheet renders as `## SheetName` followed by **one GFM table covering the
used range from A1** — empty cells are padded so that table row *i*, column
*j* is exactly sheet cell (*i*, *j*). The first table row doubles as the GFM
header row. Formulas render as `=SUM(...)`.

Import maps cell text back by position: leading `=` → formula, `true`/`false`
→ boolean, numeric text → number, anything else → string. Merges, worksheet
tables, and hyperlinks are not representable in Markdown (kept in JSON).

### PowerPoint ⇄ Markdown

- Slides are separated by `---`.
- The title placeholder renders as `# Title`; on import the first heading of
  each slide becomes the title.
- Text frames render as paragraphs, or as nested bullet lists when they use
  outline levels; on import lists become one content text box with matching
  levels (default stacked placement — transforms live only in JSON).
- Pictures render as `![alt](…)`; tables as GFM tables.
- Speaker notes render as a blockquote whose first line starts with
  `Notes:`; the same convention is recognized on import.
- Hidden flags, layouts, transforms, and comments are JSON-only (diagnostic).

## Plain text

Render-only for Excel and PowerPoint; text → Office is supported for Word
only (each line becomes one paragraph).

- **Word**: one line per paragraph; tables as tab-separated rows; footnotes
  and endnotes appended under `--- footnotes ---` / `--- endnotes ---`.
- **Excel**: `# SheetName` followed by tab-separated rows of the used range.
- **PowerPoint**: `## Slide N` sections; one line per paragraph; notes
  prefixed `Notes:`.

## CSV (Excel family only)

CSV maps one worksheet grid, in both directions
(`SerializeModelCsv`/`ParseModelCsv` in `Tools/DocumentModelIO.hpp`). A Word
or PowerPoint model cannot become CSV; the pair is rejected as a usage error.

**Export (`xlsx → csv`)**

- One worksheet per file: the first by default (with a warning when the
  workbook has more), or the one named by `--sheet` (case-insensitive).
- RFC 4180: fields containing the separator, a quote, or a line break are
  quoted with `"`, embedded quotes are doubled, rows end in CRLF. The
  separator defaults to `,` (`--csv-separator` overrides).
- Rows run from row 1 to the last stored row; cells the worksheet does not
  store become empty fields, so the grid stays rectangular.
- Cell rendering: formula cells emit their **cached result** (never the
  formula text — recalculate first with `exyoki recalc` if it may be stale),
  booleans emit `TRUE`/`FALSE`, dates emit their stored ISO text, and every
  other cell emits its canonical value text.
- CSV carries no formatting, merges, tables, hyperlinks, comments, or media;
  all of that is dropped (media with a diagnostic).

**Import (`csv → xlsx`)**

- Produces a single-worksheet workbook; `--sheet` names the worksheet
  (default `Sheet1`).
- Quoted fields, doubled quotes, CRLF/LF line breaks, and a UTF-8 BOM are
  handled. Empty fields produce no stored cell.
- Type inference is deliberately conservative:
  - `TRUE`/`FALSE` (case-insensitive) → boolean.
  - Plain decimal numbers (optional sign, fraction, exponent) → number —
    except leading-zero values such as `007`, which stay text so codes and
    identifiers survive.
  - Everything else → text. A value starting with `=` is imported as text
    with a warning; **CSV import never creates formulas**, so untrusted CSV
    cannot inject computation.

The `csv → xlsx → csv` round trip preserves values and types inferred above;
`xlsx → csv` is lossy exactly as listed (one sheet, values only).

## Media

- Exporting to md/json/xml writes media to a directory (default
  `<output stem>_media` next to the output; override with `--media-dir`) and
  references it relatively (`report_media/image1.png`). `--embed-media`
  (json/xml only) inlines base64 `data` instead; `--no-media` drops payloads
  with a diagnostic per item.
- Importing resolves references relative to the input file's directory (or
  `--media-dir`); a missing file produces a warning and the image is skipped.

## Fidelity matrix

Legend: ✔ preserved · ◐ partially preserved (see notes above) · ✖ dropped
with diagnostic. JSON and XML always behave identically.

| Construct | JSON/XML | Markdown | Plain text |
| --- | --- | --- | --- |
| Paragraph text, headings | ✔ | ✔ | ◐ (text only) |
| Bold/italic/underline/strike | ✔ | ✔ | ✖ |
| Color, highlight, font, size, char styles | ✔ | ✖ | ✖ |
| Paragraph styles, alignment | ✔ | ◐ (`Quote`, `HTMLPreformatted`, headings) | ✖ |
| Lists incl. definitions | ✔ | ◐ (nesting kept; formats flatten to bullet/ordered) | ✖ |
| Tables | ✔ (incl. merges, nesting) | ◐ (grid only) | ◐ (TSV) |
| Hyperlinks | ✔ | ✔ | ✖ (text only) |
| Images | ✔ (media + size) | ◐ (media, alt) | ✖ |
| Footnotes/endnotes | ✔ | ✔ | ◐ (appendix) |
| Comments | ✔ | ✖ | ✖ |
| Headers/footers | ✔ | ✖ | ✖ |
| Fields | ✔ (code + cached text) | ◐ (cached text) | ◐ (cached text) |
| Section breaks | ✔ | ✔ (`---`) | ✖ |
| Excel values/formulas/merges/tables/links | ✔ | ◐ (values + formulas) | ◐ (values) |
| Slides, titles, text levels | ✔ | ✔ | ◐ |
| Shape transforms, hidden slides | ✔ | ✖ | ✖ |
| Slide notes | ✔ | ✔ (`> Notes:`) | ✔ |
| Slide tables/pictures | ✔ | ✔/◐ | ◐/✖ |
| Charts, SmartArt, OLE, content controls, revisions | ✖ (everywhere) | ✖ | ✖ |

**Round-trip guarantees**: Office → JSON → Office preserves everything the
model captures (the ✔ rows). Office → Markdown → Office preserves document
structure and text but flattens the ◐ rows. For a byte-faithful round trip
use `to-flat-opc`/`from-flat-opc`.
