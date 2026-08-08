# Tables

Word tables are grids of cells whose columns may merge, whose rows may repeat
as headers, and whose cells may contain anything a body can — including other
tables. This chapter covers structure, formatting, merging, and nesting.

```cpp
#include "ExyokiOffice/Word/WordDocument.hpp"
using namespace ExyokiOffice::Word;
namespace W = ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing;
```

## Creating a table

```cpp
auto table = editor->AddTable(3, 3);              // rows, columns; at the end of the body
auto inserted = editor->After(intro).InsertTable(2, 3);   // via a body cursor

table->SetCellText(0, 0, "Region");
table->SetCellBackgroundColor(0, 0, ExyokiOffice::Color(0xE0, 0xE0, 0xE0));
table->SetRowHeader(0, true);            // repeat header row on each page
```

Rows and columns are zero-based. `SetCellText` replaces the cell content with
plain text; `AppendCellText` adds to what is there.

## Structural edits

```cpp
table->AddRow();                 // append a row (optionally with a column count)
table->AddColumn();              // append a column
table->InsertRow(1);             // insert before row 1
table->RemoveRow(2);
table->InsertColumn(0);
table->RemoveColumn(1);
```

`GetRowCount()` and `GetColumnCount()` report the physical grid.
`GetLogicalColumnCount()` and `GetLogicalGrid()` account for merged cells:
the logical grid returns one `TableGridCell` per grid position, telling you
which physical cell covers it — the right tool when walking a table with
merges.

## Table-level formatting

```cpp
table->SetWidth(Millimeters(160.0))
    .SetAlignment(W::TableRowAlignmentValues::Center)
    .SetBorders(W::BorderValues::Single, UInt32{8}, Color(0x1F, 0x4E, 0x79))
    .SetDefaultCellMargins(Millimeters(2.0), Millimeters(1.0),
                           Millimeters(2.0), Millimeters(1.0));
```

Like paragraph borders, `SetBorders` accepts the width either in Word's
native eighths of a point (`UInt32`) or in any physical unit
(`MeasuringUnits`).

## Cell and row formatting

| Target | Methods |
| --- | --- |
| Cell content | `SetCellText`, `AppendCellText` |
| Cell appearance | `SetCellBackgroundColor`, `SetCellBorders`, `SetCellMargins` |
| Cell geometry | `SetCellWidth`, `SetCellHorizontalAlignment`, `SetCellVerticalAlignment` |
| Row geometry | `SetRowHeight`, `SetColumnWidth` |
| Row pagination | `SetRowCantSplit` (keep the row on one page), `SetRowHeader` (repeat on every page) |

## Formatted text inside cells

`SetCellText` writes plain text. To format the text inside a cell, reach its
paragraph: `Table::Paragraphs()` returns one paragraph per cell in row-major
order, and each is an ordinary `Paragraph` with the full formatting API from
[Text and paragraphs](text.md):

```cpp
auto cells = table->Paragraphs();
cells[0]->AddRun("Region", RunStyle{.Bold = true, .Color = Color(0xFF, 0xFF, 0xFF)});
table->SetCellBackgroundColor(0, 0, Color(0x1F, 0x4E, 0x79));
```

## Merging and splitting

```cpp
table->MergeCells(1, 0, 1, 2);   // from (row 1, col 0): span 1 row, 2 columns
table->SplitCell(1, 0);          // undo one merge
table->SplitAllCells();          // dissolve every merge in the table
```

Merges are expressed with row and column spans from a top-left anchor cell.
After merging, address content through the anchor cell; the logical-grid API
above reports which positions each merge covers.

## Nested tables

```cpp
auto inner = table->AddNestedTable(2, 1, 2, 2);  // inside cell (2, 1): a 2×2 table
```

A nested table is a full `Table` wrapper. `Table::Tables()` enumerates the
tables nested anywhere inside a table, and `editor->Tables()` returns only
the top-level body tables.
