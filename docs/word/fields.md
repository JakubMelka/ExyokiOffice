# Fields and tables of contents

A Word field is an instruction (`PAGE`, `DATE`, `TOC …`) plus a cached
result. ExyokiOffice creates and edits field structures but **never
evaluates them** — values that depend on layout (page numbers, TOC entries)
are marked dirty so Word computes them when the document is opened. This is a
deliberate boundary: computing them correctly would require a full layout
engine.

```cpp
#include "ExyokiOffice/Word/WordDocument.hpp"
using namespace ExyokiOffice::Word;
```

## Creating fields

```cpp
paragraph->AddField("PAGE", "1");                    // complex field, marked dirty
paragraph->AddSimpleField("DATE \\@ \"yyyy-MM-dd\"", "2026-01-01");
```

WordprocessingML has two field encodings, and the API exposes both:

- **Simple fields** (`w:fldSimple`) — one element wrapping the cached
  result. `AddSimpleField` writes these.
- **Complex fields** — a begin/separator/end run triple with the instruction
  and result between them. `AddField` writes these and marks the result
  dirty; Word recomputes it on open.

The second argument is the placeholder result shown until Word refreshes the
field. Common instructions: `PAGE`, `NUMPAGES`, `DATE`, `TIME`, `REF name`,
`PAGEREF name`, `SEQ Figure`, `TOC \o "1-3"`.

## The Field wrapper

`paragraph->Fields()` and `editor->Fields()` enumerate existing fields (the
editor-level call covers the body only; fields in headers, footers, and notes
are reached through their own content wrappers):

```cpp
for (const auto& field : editor->Fields())
{
    field->Kind();                 // Simple or Complex
    field->GetInstruction();       // "PAGE", "TOC \\o \"1-3\"", ...
    field->GetResult();            // the cached result text
    field->SetResult("42");        // rewrite the cache
    field->InvalidateResult();     // mark dirty -> Word recomputes on open
    field->IsDirty();
    field->IsLayoutDependent();    // PAGE, NUMPAGES, TOC, PAGEREF, ...
}
```

`SetInstruction` rewrites the instruction in place. `IsLayoutDependent` is a
convenience for deciding whether a cached result can be trusted without Word:
a `DATE` result is honest, a `PAGE` result is only as good as the last
program that laid the document out.

## Table of contents

```cpp
editor->AddTableOfContents(1, 3);                    // TOC over headings 1-3
```

This writes the `TOC \o "from-to"` field with hyperlinked entries and marks
it dirty. Word asks the user to update fields on open (or does so when
printing) and fills in the real entries and page numbers. The TOC collects
paragraphs styled `Heading1`–`Heading9` — which is what `AddHeading` applies;
see [Styles, headings, and lists](styles.md).

## Page numbering in a footer

A "Page N of M" footer combines both field kinds with a header/footer part
(see [Sections and page setup](sections.md) for the surrounding machinery):

```cpp
namespace W = ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing;

auto footer = editor->EnsureFinalSection()->EnsureFooter(HeaderFooterType::Default);
footer->Clear();
auto line = footer->AddParagraph();
line->SetAlignment(W::JustificationValues::Center);
line->AddText("Page ");
line->AddField("PAGE", "1");
line->AddText(" of ");
line->AddField("NUMPAGES", "1");
```

## Mail-merge fields

`MERGEFIELD` placeholders and repeating regions are filled by
`editor->MergeTemplate(data)` — see
[Revisions and document merging](revisions.md).
