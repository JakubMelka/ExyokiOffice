# ExyokiOffice documentation

This directory is the ExyokiOffice user manual. Every page is a chapter,
and the order below is the reading order: what the library is and how it is
put together, then a quickstart and a folder of chapters per document
format, then the subsystems that cut across formats, then the tooling, and
finally the project's own policies.

The repository [README](../README.md) is the short version — what the
library is, how to build it, and a hello world per format. Start here when
you need more than that. The complete API reference is the Doxygen
documentation generated from the headers under `include/ExyokiOffice`.

## Introduction

| Chapter | Covers |
| --- | --- |
| [Introduction](introduction.md) | What the library does (and deliberately does not), the four-layer architecture — editors, typed DOM, packaging, and the tooling and front ends above them — the OPC package model, safe package-opening limits, API conventions, building and linking, how this manual is organized. |
| [How ExyokiOffice compares](comparison.md) | Where the library sits next to Open XML SDK, Apache POI, the Python libraries, the single-format C++ libraries, the commercial suites, embedded office suites, and the other Office MCP servers — and when to choose one of those instead. |

## Word

| Chapter | Covers |
| --- | --- |
| [Word quickstart](Word.md) | Hello world, lifecycle, and a short tour of `WordDocumentEditor`. |
| [Documents](word/documents.md) | Lifecycle, document types, templates, snapshots and transactions, properties, themes. |
| [Text and paragraphs](word/text.md) | Body cursors, paragraphs, runs, formatting, breaks, find and replace. |
| [Styles, headings, and lists](word/styles.md) | `StyleManager`, latent styles, simple lists, multi-level numbering. |
| [Tables](word/tables.md) | Structure, formatting, merged cells, nested tables. |
| [Images](word/images.md) | Inline and floating images, wrapping, position, crop, alt text. |
| [Hyperlinks and bookmarks](word/hyperlinks.md) | External and internal links, bookmark ranges. |
| [Fields and tables of contents](word/fields.md) | Field structures, TOC, page numbering. |
| [Embedded charts](word/charts.md) | Reading and updating charts and their embedded workbooks. |
| [Sections and page setup](word/sections.md) | Page size, margins, columns, headers and footers. |
| [Notes, comments, and content controls](word/notes.md) | Footnotes, endnotes, comments, structured document tags. |
| [Revisions and document merging](word/revisions.md) | Tracked changes, comparing, `InsertDocument`, mail merge. |
| [Document protection](word/protection.md) | Editing restrictions and password verifiers. |

## Excel

| Chapter | Covers |
| --- | --- |
| [Excel quickstart](Excel.md) | Hello world, lifecycle, and a short tour of `ExcelDocumentEditor`. |
| [Workbooks](excel/workbooks.md) | Lifecycle, snapshots, properties, themes, VBA, workbook protection. |
| [Worksheets](excel/worksheets.md) | Sheet management, copying between workbooks, sheet protection. |
| [Cells and ranges](excel/cells.md) | Values, shared strings, ranges, structural edits, merged cells. |
| [Styles and number formats](excel/styles.md) | `StyleRepository`, fonts, fills, borders, number formats. |
| [Named ranges](excel/named-ranges.md) | `NamedRangeManager`, workbook and worksheet scope, name resolution. |
| [Formulas](excel/formulas.md) | The formula engine: parsing, evaluation, supported functions, recalculation. |
| [Tables](excel/tables.md) | Worksheet tables and auto-filters. |
| [Charts](excel/charts.md) | `ChartBuilder`, series, cross-sheet data sources. |
| [Pivot tables](excel/pivot-tables.md) | Pivot caches, records, definitions, aggregation, refresh. |
| [Slicers](excel/slicers.md) | Slicers over pivot tables and worksheet tables, shared caches. |
| [Validation and conditional formatting](excel/validation.md) | Entry rules and value-driven formatting. |
| [Layout and annotations](excel/layout.md) | Row/column dimensions, views, hyperlinks, comments, images. |
| [Printing](excel/printing.md) | Page setup, margins, print areas, headers and footers. |

## PowerPoint

| Chapter | Covers |
| --- | --- |
| [PowerPoint quickstart](PowerPoint.md) | Hello world, lifecycle, and a short tour of `PowerPointDocumentEditor`. |
| [Presentations](powerpoint/presentations.md) | Lifecycle, slide size, modify protection, low-level access. |
| [Slides](powerpoint/slides.md) | Slide management, `SlideBuilder`, sections, custom shows. |
| [Shapes](powerpoint/shapes.md) | The shape tree, geometry, transforms, fills, outlines, effects. |
| [Text](powerpoint/text.md) | Text frames, runs, bullets. |
| [Pictures, media, and tables](powerpoint/pictures-and-media.md) | Embedded and linked pictures, audio/video, DrawingML tables. |
| [Charts](powerpoint/charts.md) | Chart creation, cached data, embedded workbooks. |
| [Masters, layouts, and placeholders](powerpoint/masters.md) | The design hierarchy, themes, placeholder inheritance. |
| [Transitions](powerpoint/transitions.md) | Slide transition effects and timing. |
| [Animations](powerpoint/animations.md) | Animation sequences, effects, triggers. |
| [Notes and comments](powerpoint/notes-and-comments.md) | Speaker notes and slide comments. |

## Cross-cutting subsystems

| Chapter | Covers |
| --- | --- |
| [Threading and concurrency](Threading.md) | Thread-safety contract, independent-document parallelism, external locking, cooperative cancellation, callbacks, and transactions. |
| [Digital signatures](Signatures.md) | Verifying and creating package signatures through an application-supplied `ICryptoProvider`; the library links no cryptographic code of its own. |
| [External resources](ExternalResources.md) | What a document can point at outside itself, and the resolver plus policy an application must install before any of it is ever read. Everything is off by default. |

## Tooling

| Chapter | Covers |
| --- | --- |
| [exyoki](tools/exyoki.md) | The command-line utility: inspecting, validating, unpacking and repacking, converting, deduplicating resources, diffing, querying, and editing packages from a shell or a script. |
| [Conversion formats](tools/conversion-formats.md) | The JSON, XML, Markdown, plain-text, and CSV formats `exyoki convert` and the `Tools` conversion API produce and consume, and the [JSON Schema](tools/conversion-formats.md#json-schema) that makes the JSON envelope checkable. |
| [MCP servers](tools/mcp-servers.md) | The three Model Context Protocol servers that expose Word, Excel, and PowerPoint documents to AI agents: registration and smoke-testing, the workspace sandbox, worked sessions for each family, task recipes, the tool catalog, the shared result envelope, and troubleshooting. |
| [The container image](tools/docker.md) | The distroless image that carries the library, `exyoki` and the three MCP servers: loading it, running the tool, registering the servers with `docker` as the command, the `/work` workspace and file ownership, and building the image yourself. |

## Project policies

| Chapter | Covers |
| --- | --- |
| [Compatibility matrix](Compatibility.md) | Which file formats and Office versions are supported, and what create, edit, and preserve support each feature area has in Word, Excel, and PowerPoint. |
| [Versioning and ABI](ABI.md) | Semantic versioning, why only patch releases keep the ABI, and what the installed shared library guarantees. |
| [Continuous integration](ci.md) | The manual-only workflows and what each one builds. |
| [Fuzzing](fuzzing.md) | The libFuzzer targets, `WinFuzz.ps1`, the corpus, and how crash artifacts become regression tests. |

## Runnable examples

The guides quote from examples that are built and smoke-tested with the rest
of the repository — each one writes a document, opens it again, and fails
the build if anything breaks.

| Example | Guide it illustrates |
| --- | --- |
| [ExampleWordEditor](../examples/ExampleWordEditor/main.cpp) | [Word](Word.md) — a complete report authored from scratch. |
| [ExampleWordEdit](../examples/ExampleWordEdit/main.cpp) | [Word](Word.md) — opening an existing document, filling placeholders, inserting through cursors. |
| [ExampleExcelEditor](../examples/ExampleExcelEditor/main.cpp) | [Excel](Excel.md) — a two-sheet workbook with formulas, a chart, a table, and a slicer. |
| [ExamplePowerPointEditor](../examples/ExamplePowerPointEditor/main.cpp) | [PowerPoint](PowerPoint.md) — a five-slide deck. |
| [ExampleWord](../examples/ExampleWord/main.cpp) | The typed DOM and packaging layers underneath all three editors. |

## The PDF manual and API reference

The `docs-pdf.yml` GitHub workflow renders this directory into a single
hyperlinked PDF with pandoc. It is manual-only, like every workflow in this
repository: trigger it from the Actions tab and download the
`ExyokiOffice-manual-<version>` artifact. The chapter order and the
preprocessing live under [`_pdf/`](_pdf/chapters.txt); a new chapter must be
added both to this index and to `_pdf/chapters.txt` to appear in the PDF.

The `doxygen-pdf.yml` workflow is its companion for the API reference: it
runs Doxygen over the hand-written public headers (the generated DOM tree is
excluded) and uploads the result as the
`ExyokiOffice-api-reference-<version>` artifact. Its configuration lives in
[`_doxygen/`](_doxygen/Doxyfile).

Both PDFs open with the project logo on the title page and carry the release
number in their file name and on their title page. The number is read from
`VERSION.txt` in the repository root — the same file the build uses — so
neither workflow ever hard-codes a version. See
[Versioning and ABI policy](ABI.md) for how that number is bumped.

## Conventions for these pages

This directory is published as a wiki and exported as a PDF user manual, so
each page has to stand on its own in three places at once:

- **One `#` title per page**, and a heading hierarchy that never skips a
  level — the PDF's chapter and section numbering is derived from it.
- **Links between pages are relative and file-based** (`Excel.md`,
  `excel/slicers.md`, `../Signatures.md`), never absolute repository URLs.
- **Links out of this directory** (`../examples/...`,
  `../include/ExyokiOffice/...`) point at code the wiki and the PDF cannot
  render. Keep them rare, always name what the reader will find there, and
  never make a sentence depend on the target being open. The PDF build
  rewrites them into absolute repository URLs.
- **Code blocks are tagged with their language** and stay under roughly 80
  columns, because the PDF cannot reflow them.
- **New pages are listed in this index and in `_pdf/chapters.txt`.** A page
  that is not linked from here is invisible in the wiki, and one missing
  from the chapter list is missing from the PDF.
