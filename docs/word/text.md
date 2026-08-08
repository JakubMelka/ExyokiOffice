# Text and paragraphs

This chapter covers where content goes (body cursors) and what most content
is made of (paragraphs and runs), plus paragraph- and run-level formatting,
breaks, and text search and replace.

Snippets use `W` for the generated WordprocessingML enum namespace:

```cpp
#include "ExyokiOffice/Word/WordDocument.hpp"
using namespace ExyokiOffice::Word;
namespace W = ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing;
```

## Body cursors: where content is inserted

Every block-level insertion goes through a `BodyCursor`, an insertion point
in the document body. `AddParagraph`, `AddTable`, and `AddHeading` are
shorthand for the cursor at the end of the body; the other cursors position
new content relative to blocks that already exist, which is what editing an
existing document needs.

```cpp
editor->Body()                      // end of the body (what AddParagraph uses)
editor->BodyStart()                 // before the first block
editor->Before(paragraphOrTable)    // immediately before an existing block
editor->After(paragraphOrTable)     // immediately after an existing block
```

A cursor is a lightweight value: take it, insert through it, discard it.

```cpp
auto intro = editor->Body().InsertParagraph("Introduction");

editor->Before(intro).InsertParagraph("Draft - not for distribution");

auto table = editor->After(intro).InsertTable(2, 3);
table->SetCellText(0, 0, "Region");

auto landscape = editor->Body().InsertSectionBreak(SectionStartType::NextPage);
```

Every insert returns the same wrapper type the read API hands out
(`Paragraph`, `Table`, `Section`, …), so inserted content can be formatted
immediately, and a document can be assembled in any order rather than
strictly front to back. Insertion never moves existing content apart from the
natural shift of placing a block ahead of another one. Two details are worth
knowing:

- The end-of-body cursor inserts *before* a trailing `<w:sectPr>`, because
  WordprocessingML requires the final section properties to stay last.
- A cursor built from a wrapper that is not a body-level block is invalid;
  `IsValid()` reports it and every insert on it returns `nullptr`.

Anchoring on content found in an opened document is the usual pattern:

```cpp
for (const auto& paragraph : editor->Paragraphs())
{
    if (paragraph->PlainText() == "Scope")
    {
        editor->After(paragraph).InsertParagraph("Added below the existing heading.");
        break;
    }
}
```

`examples/ExampleWordEdit/main.cpp` is a runnable version of that pattern
over a real template.

Cursors also carry the insertions that have no `Add…` shorthand:
`InsertSectionBreak` (see [Sections](sections.md)), `InsertContentControl`
(see [Notes, comments, and content controls](notes.md)), and
`InsertDocument`, which merges another document in at that point (see
[Revisions and document merging](revisions.md)).

## Paragraphs and runs

A paragraph contains runs; a run carries uniformly formatted text. `Text`
elements inside a run hold the actual characters — `AddText` on a paragraph
creates a run implicitly.

```cpp
auto paragraph = editor->AddParagraph();
paragraph->AddText("Runs can mix ");
paragraph->AddRun("bold", RunStyle{.Bold = true});
paragraph->AddRun()->SetItalic().AddText("italic");
paragraph->AddRun()->SetColor(ExyokiOffice::Color(0xC0, 0x00, 0x00))
                   .SetFontSize({14.0, ExyokiOffice::MeasurementUnit::Point})
                   .AddText("red 14pt");
```

Reading mirrors writing: `paragraph->Runs()`, `paragraph->Texts()`,
`paragraph->Images()`, and `paragraph->PlainText()` (the concatenated text of
every run). `run->PlainText()` does the same for one run.

Whitespace note: XML collapses leading and trailing spaces unless the text
element asks for preservation. Pass `preserveSpaces = true` to `AddText` (or
call `Text::SetPreserveSpaces`) when a run's text begins or ends with a
space you need to keep.

## Run formatting

`Run` exposes Set/Get/Clear triples for every supported property, and the
setters chain:

| Property group | Methods |
| --- | --- |
| Weight and posture | `SetBold`, `SetItalic` |
| Underline and strike | `SetUnderline` (style or on/off), `SetStrike`, `SetDoubleStrike` |
| Capitalization | `SetCaps`, `SetSmallCaps` |
| Color and highlight | `SetColor`, `SetHighlight` |
| Font and size | `SetFont` (ASCII + high-ANSI), `SetFontSize`, `SetKerning` |
| Spacing and position | `SetSpacing` (character spacing), `SetPosition` (sub/superscript offset) |
| Language and proofing | `SetLanguage` (Latin, East Asian, complex script), `SetNoProof` |
| Effects and style | `SetTextEffect`, `SetStyleId` (character style) |

`GetFormatting()` returns the whole set as one `RunFormatting` structure, and
`ClearFormatting()` removes all direct run formatting at once.

One overload pitfall: underline has both a style setter and a plain on/off
flag, so wrap the enumerator to select the style form —
`SetUnderline(W::UnderlineValues(W::UnderlineValues::Single))`. A bare
`W::UnderlineValues::Single` converts to `bool` and silently means
"underlined with the default style".

## Paragraph formatting

`Paragraph` covers alignment, spacing (physical units or line units),
indentation, tab stops, borders, shading, and pagination flags. Formatting
chains, so a pull quote is one statement:

```cpp
editor->AddParagraph("Everything is written through the public API.")
    ->SetAlignment(W::JustificationValues::Center)
    .SetIndentation(Millimeters(20.0), Millimeters(20.0), std::nullopt, std::nullopt)
    .SetSpacing(Points(12.0), Points(12.0), std::nullopt)
    .SetShading(Color(0xF2, 0xF6, 0xFB))
    .SetBorders(W::BorderValues::Single, UInt32{8}, Color(0x1F, 0x4E, 0x79), Points(6.0));
```

Notes on the individual groups:

- `SetSpacing(before, after, lineSpacing)` takes physical units;
  `SetSpacingLines` expresses the same in Word's line units (hundredths of a
  line). `TryGetSpacing`/`TryGetSpacingLines` read whichever form is present.
- `SetIndentation(left, right, firstLine, hanging)` — pass `std::nullopt` for
  components you do not want to touch; `TryGetIndentation` reads them back.
- `AddTabStop(position, alignment, leader)` appends custom tab stops;
  `ClearTabStops` removes them.
- `SetBorders` accepts the border width either in Word's native eighths of a
  point (`UInt32`) or in any physical unit (`MeasuringUnits`), plus a
  color and the padding distance.
- Pagination flags: `SetKeepWithNext`, `SetKeepLines`, `SetPageBreakBefore`,
  `SetWidowControl`, each with a getter and covered collectively by
  `ClearPagination`.
- `GetFormatting()` returns everything as one `ParagraphFormatting` value;
  `ClearFormatting()` resets the paragraph to its style's formatting.

Style assignment is part of the same chain: `SetStyleId("MyQuote")` applies a
paragraph style, `GetStyleId`/`ClearStyleId` read and remove it. Styles
themselves are the subject of [Styles, headings, and lists](styles.md).

## Breaks

```cpp
paragraph->AddBreak();                    // line break within the paragraph
paragraph->AddBreak(BreakType::Column);   // column break
editor->AddPageBreak();                   // paragraph containing a page break
```

## Find and replace

`Paragraph` offers text search and replace that works *across run
boundaries*: `Find`, `FindAll`, `GetText`, `ReplaceText`, `ReplaceAll`,
`FindAllRegex`, and `ReplaceAllRegex`. The "across run boundaries" part
matters: Word splits text into runs for reasons of its own (spell-check
state, revision ids, direct formatting), so a placeholder a human sees as one
word is often three runs. Filling a template is therefore just a sweep over
the paragraphs:

```cpp
Size replaced = 0;
for (const auto& paragraph : editor->Paragraphs())
{
    replaced += paragraph->ReplaceAll("{{CUSTOMER}}", "Contoso Ltd.");
}
```

`Find` returns a `ContentRange` (offset and length in the paragraph's plain
text) that `GetText` and `ReplaceText` consume; `ReplaceAll` adjusts
subsequent ranges as it rewrites, and replacements preserve the formatting of
the run where each match starts. The regex variants take a `std::regex`, and
`ReplaceAllRegex` supports the usual `$1`-style capture references in the
replacement format string.

For MERGEFIELD-based templates and repeating regions, use
`editor->MergeTemplate(data)` instead — see
[Revisions and document merging](revisions.md). For find and replace from
the command line, see [exyoki](../tools/exyoki.md) `search` and `replace`.
