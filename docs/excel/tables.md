# Worksheet tables

An Excel table (a *ListObject* in Excel's own terminology) turns a
rectangular range into a named, structured region with a header row, an
auto-filter, and stable column names that formulas and slicers can
reference.

```cpp
#include "ExyokiOffice/Excel/ExcelDocument.hpp"
using namespace ExyokiOffice::Excel;
```

## Creating a table

```cpp
auto table = sheet->CreateTable("SalesTable", *CellRange::ParseA1("A1:D10"),
                                {{0, "Product"}, {0, "Quantity"}, {0, "Total"}, {0, "Notes"}});
sheet->RenameTable(table, "Sales");
sheet->RemoveTable(table);
```

The supplied range includes the header row; the column list must match the
range width. Creating a table registers a stable ID, a package relationship,
a worksheet `tableParts` entry, and an auto-filter automatically.

**Ordering caveat:** add a table *after* other worksheet features (data
validation, conditional formatting, hyperlinks) on the same worksheet, since
`tableParts` must be the last sheet-level element in SpreadsheetML's
required child order.

## Finding tables

```cpp
auto tables = sheet->Tables();
auto byName = sheet->TableByName("Sales");
```

Table names are workbook-unique; `CreateTable` rejects duplicates.

## Tables and other features

- A table column can drive a slicer (`SetTableSource`) — see
  [Slicers](slicers.md).
- A table's data region is a natural pivot-table source — see
  [Pivot tables](pivot-tables.md).
- Table ranges participate in structural row/column edits like every other
  feature-owned range — see [Cells and ranges](cells.md).
