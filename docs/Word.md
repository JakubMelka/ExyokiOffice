# Word quickstart

This quickstart introduces `ExyokiOffice::Word::WordDocumentEditor`, the
high-level API for authoring and editing Word documents. It shows the
essential moves — creating, opening, saving, and a tour of the main content
types — and hands off to the [Word chapters](word/documents.md) for the
thorough treatment of each topic.

Everything shown here lives in one header:

```cpp
#include "ExyokiOffice/Word/WordDocument.hpp"
using namespace ExyokiOffice::Word;
namespace W = ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing;
```

A complete runnable version of most snippets is in
[examples/ExampleWordEditor/main.cpp](../examples/ExampleWordEditor/main.cpp),
which builds a full report — title page, table of contents, prose, lists, a
table, an image, review markup, and a footer with page fields — and re-opens
the saved file to prove it round-trips.
[examples/ExampleWordDemo/main.cpp](../examples/ExampleWordDemo/main.cpp) is the
short counterpart: one A4 page with the project logo, a bulleted list, a
formatted table, a bordered code callout, and a footer page number. The full API
reference is the Doxygen documentation in
[WordDocument.hpp](../include/ExyokiOffice/Word/WordDocument.hpp).

## Hello world

```cpp
auto editor = WordDocumentEditor::CreateNew();
editor->AddHeading("Hello world");
editor->AddParagraph("This document was created by ExyokiOffice.");
editor->SaveToFile("Hello.docx");
```

## Creating, opening, and saving

```cpp
auto fresh    = WordDocumentEditor::CreateNew();                  // new .docx
auto fromDisk = WordDocumentEditor::Open("existing.docx");        // nullptr on failure
auto fromBytes= WordDocumentEditor::Open(bytes);                  // std::vector<uint8_t> or std::span
auto fromTmpl = WordDocumentEditor::CreateFromTemplate("letter.dotx");

editor->SaveToFile("out.docx");             // atomic save by default
std::vector<Byte> blob = editor->SaveToMemory();
```

All factories return `nullptr` when the source cannot be read or parsed —
always check the result. Document types (`.docm`, `.dotx`, …), open
settings, undo snapshots, and document properties are covered in
[Documents](word/documents.md).

## A short tour

Text is paragraphs of runs, formatted fluently:

```cpp
auto paragraph = editor->AddParagraph();
paragraph->AddText("Runs can mix ");
paragraph->AddRun("bold", RunStyle{.Bold = true});
paragraph->AddRun()->SetItalic().AddText("italic");
```

Content is inserted at *cursors*, so editing an existing document places new
blocks exactly where they belong:

```cpp
auto intro = editor->Body().InsertParagraph("Introduction");
editor->Before(intro).InsertParagraph("Draft - not for distribution");
```

Headings, lists, tables, and images each take one call to get started:

```cpp
editor->AddHeading("Background", 2);                  // Heading2 style

auto bullets = editor->EnsureBulletedListStyle();
editor->AddParagraph("First point")->SetListStyle(bullets);

auto table = editor->AddTable(3, 3);
table->SetCellText(0, 0, "Region");

editor->AddImageFromFile("logo.png");
```

Filling a template is a sweep over the paragraphs — `ReplaceAll` matches
across run boundaries, which is what makes it work on real Word files:

```cpp
for (const auto& paragraph : editor->Paragraphs())
{
    paragraph->ReplaceAll("{{CUSTOMER}}", "Contoso Ltd.");
}
```

A footer with page numbers combines sections, headers/footers, and fields:

```cpp
auto footer = editor->EnsureFinalSection()->EnsureFooter(HeaderFooterType::Default);
auto line = footer->AddParagraph();
line->AddText("Page ");
line->AddField("PAGE", "1");        // marked dirty; Word computes the value
line->AddText(" of ");
line->AddField("NUMPAGES", "1");
```

[examples/ExampleWordEdit/main.cpp](../examples/ExampleWordEdit/main.cpp)
shows the editing workflow end to end: open, find, fill, insert next to
existing content, save under a new name.

## The chapters

| Chapter | Covers |
| --- | --- |
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

## Known limitations

- Fields are never computed; layout-dependent values require Word (or
  another layout engine) to refresh.
- `DetectImageFormat` does not sniff SVG or WMF payloads without a placeable
  header; pass their format explicitly.
- Specialized content controls are preserved but have no typed editing API.
- New chart anchors, SmartArt, text boxes, and equations have no high-level
  authoring helpers (existing charts can be read and updated — see
  [Embedded charts](word/charts.md)).

The [compatibility matrix](Compatibility.md) grades every Word feature area
for create, edit, and preserve support, alongside the supported document
types and Office versions.
