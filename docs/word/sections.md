# Sections, page setup, headers, and footers

A Word document is divided into sections, and page geometry — size,
orientation, margins, columns — belongs to the section, not the document.
Every document has at least one section: the final body section, whose
properties element (`w:sectPr`) sits at the end of the body.

```cpp
#include "ExyokiOffice/Word/WordDocument.hpp"
using namespace ExyokiOffice::Word;
```

## Getting at sections

```cpp
auto section = editor->EnsureFinalSection();   // the last (often only) section
auto all     = editor->Sections();             // every section, in order

auto next = editor->Body().InsertSectionBreak(SectionStartType::NextPage);
```

`InsertSectionBreak` starts a new section at the cursor; `SectionStartType`
covers Word's five break kinds (`NextPage`, `Continuous`, `EvenPage`,
`OddPage`, `NextColumn`). `Section::GetStartType`/`SetStartType` edit the
kind afterwards, and `IsFinalBodySection()` identifies the trailing section.

## Page size, orientation, and margins

```cpp
section->SetPageSize({{210.0, ExyokiOffice::MeasurementUnit::Millimeter},
                      {297.0, ExyokiOffice::MeasurementUnit::Millimeter},
                      PageOrientation::Portrait});
section->SetPageOrientation(PageOrientation::Landscape);   // swaps the dimensions

section->SetMargins({{25.0, ExyokiOffice::MeasurementUnit::Millimeter},   // top
                     {20.0, ExyokiOffice::MeasurementUnit::Millimeter},   // right
                     {25.0, ExyokiOffice::MeasurementUnit::Millimeter},   // bottom
                     {20.0, ExyokiOffice::MeasurementUnit::Millimeter},   // left
                     {12.5, ExyokiOffice::MeasurementUnit::Millimeter},   // header
                     {12.5, ExyokiOffice::MeasurementUnit::Millimeter},   // footer
                     {}});                                                  // gutter
```

`GetPageSize` and `GetMargins` read the current values back as
`SectionPageSize` and `SectionMargins`.

## Columns

```cpp
SectionColumns columns;
columns.Count = 2;
columns.Spacing = Millimeters(8.0);
section->SetColumns(columns);
auto current = section->GetColumns();
```

A column break within the text is `paragraph->AddBreak(BreakType::Column)`;
a `NextColumn` section break starts a new section at the top of the next
column.

## Headers and footers

Headers and footers exist per section and per type — `Default`, `Even`
(even pages), and `First` (first page of the section):

```cpp
// One-liners for plain text:
section->SetHeaderText(HeaderFooterType::Default, "My header");
section->SetFooterText(HeaderFooterType::Default, "My footer");

// Full content editing:
auto footer = section->EnsureFooter(HeaderFooterType::Default);
footer->Clear();
auto line = footer->AddParagraph("Confidential");
```

`EnsureHeader`/`EnsureFooter` create the part and its relationship when
missing and return a `HeaderFooterContent`, which edits like a small body:
`Paragraphs()`, `AddParagraph`, `PlainText()`, `Clear()`, `SetText`. The
paragraphs are ordinary `Paragraph` wrappers, so runs, formatting, and
fields all work — a page-number footer is shown in
[Fields and tables of contents](fields.md).

The rest of the lifecycle:

```cpp
section->HasHeader(HeaderFooterType::First);
section->GetHeader();                          // nullptr when absent
section->RemoveFooter(HeaderFooterType::Even);

section->IsHeaderLinkedToPrevious();           // no own header -> inherits
section->LinkHeaderToPrevious();               // drop the own header reference
```

"Linked to previous" is Word's inheritance model: a section without its own
header/footer reference of a given type shows the previous section's one.
`Ensure…` breaks the link by creating an own part; `Link…ToPrevious` restores
inheritance by removing the reference.

Note that Word only *shows* `Even` headers when the document's
"different odd and even pages" setting is on, and `First` when the section's
"different first page" flag is set — Word toggles these when you use the
corresponding UI options.
