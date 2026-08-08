# Layout and annotations

This chapter covers the worksheet features around the cell grid: row and
column dimensions, the sheet view (frozen panes), hyperlinks, comments —
classic and threaded — and embedded images.

```cpp
#include "ExyokiOffice/Excel/ExcelDocument.hpp"
using namespace ExyokiOffice::Excel;
```

## Rows, columns, and the view

```cpp
sheet->SetColumnDimension(1, ColumnDimension{.Width = 18});
sheet->SetRowDimension(1, RowDimension{.Height = 20});
sheet->SetView(WorksheetView{.FrozenRows = 1});   // freeze the header row
```

`ColumnDimension` and `RowDimension` also carry hidden flags and custom-size
markers; passing `std::nullopt` as the dimension removes the explicit record
and returns the row or column to its defaults. `GetRowDimension`,
`GetColumnDimension`, and `GetView` read the current state; `WorksheetView`
covers frozen rows/columns, gridline and heading visibility, zoom, and the
selected cell.

Column width uses Excel's own unit (characters of the default font), and row
height is in points — these are the values Excel itself stores.

## Hyperlinks

```cpp
sheet->SetHyperlink(ExcelHyperlink{.Address = *CellAddress::ParseA1("F1"),
                                   .Target = "https://example.com",
                                   .Display = "More info"});
auto link  = sheet->GetHyperlink(*CellAddress::ParseA1("F1"));
auto all   = sheet->Hyperlinks();
sheet->RemoveHyperlink(*CellAddress::ParseA1("F1"));
```

External targets are stored as package relationships; internal targets (a
`Location` such as `Summary!A1`) jump within the workbook.

## Comments

A classic comment (Excel calls it a note) is stored in two places: the text
in the worksheet's comments part, and its on-sheet box in a legacy VML
drawing the worksheet references through `legacyDrawing`. Both are written
and removed together — Excel treats a comments part without the drawing as
damaged content.

```cpp
sheet->SetComment(ExcelComment{.Address = *CellAddress::ParseA1("A1"),
                               .Author = "Reviewer", .Text = "Double-check this total."});
auto comment = sheet->GetComment(*CellAddress::ParseA1("A1"));
sheet->RemoveComment(*CellAddress::ParseA1("A1"));
```

Threaded comments — the modern, reply-capable kind — have their own API:
`AddThreadedComment` (returns the new comment's ID), `ThreadedComments()`,
and `RemoveThreadedComment(id)`. Excel shows classic and threaded comments
differently and stores them in different parts; the two APIs do not convert
between the models.

A reply carries the `ParentId` of the comment it answers, and removing a
comment removes its replies with it. `ThreadedComments()` resolves each
entry's author against the workbook-level person list, so `PersonName` and
`PersonEmail` come back filled and a comment read from one workbook can be
written into another unchanged. Adding a comment needs `PersonName` only when
it introduces a new person; an existing `PersonId` is enough on its own.

## Images

`Worksheet::AddImage(ExcelWorksheetImage)` embeds picture bytes behind a
two-cell drawing anchor; `Images()` enumerates existing pictures and
`RemoveImage(id)` deletes one. See
[ExcelWorksheetContent.hpp](../../include/ExyokiOffice/Excel/ExcelWorksheetContent.hpp)
for the full payload shape (bytes, content type, anchor, offsets, alt
text).
