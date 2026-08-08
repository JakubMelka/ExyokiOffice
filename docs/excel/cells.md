# Cells and ranges

This chapter covers addressing, reading, and writing individual cells,
working with rectangular ranges, sheet-qualified references, structural
row/column edits, and merged cells.

```cpp
#include "ExyokiOffice/Excel/ExcelDocument.hpp"
using namespace ExyokiOffice::Excel;
```

## Addressing

Rows and columns are one-based (`row 1` / `column 1` is `A1`), matching
Excel's own coordinate system. Every cell API exists both as a
`(row, column)` overload and a `CellAddress` overload, and addresses parse
from A1 text:

```cpp
auto address = *CellAddress::ParseA1("C1");
auto range   = *CellRange::ParseA1("A1:C3");
```

`ParseA1` returns `std::nullopt` for malformed text — dereference only after
checking, or bind the optional first in production code.

## Writing and reading cells

```cpp
sheet->SetCellText(1, 1, "Product");
sheet->SetCellNumber(2, 1, 19.99);
sheet->SetCellBoolean(*CellAddress::ParseA1("C1"), true);
sheet->SetCellError(*CellAddress::ParseA1("C2"), "#N/A");
sheet->SetCellDateTimeText(*CellAddress::ParseA1("C3"), "2026-07-30T00:00:00");
sheet->SetCellFormula(*CellAddress::ParseA1("D2"), "B2*C2");

auto value = sheet->GetCellValue(2, 1);      // std::optional<ExcelCellValue>
auto formula = sheet->GetCellFormula(*CellAddress::ParseA1("D2"));
```

`ExcelCellValue` is the typed union the getters return and `SetCellValue`
accepts: number, text (shared or inline), boolean, error, or date-time.
Housekeeping helpers: `ContainsCell`, `RemoveCell`, `StoredCellCount`, and
`StoredCellAddresses` (only cells physically stored in the XML — an empty
cell that was never written does not appear).

### Shared strings

`SetCellText` writes deduplicated shared strings automatically; use
`SetCellValue` with `ExcelCellValue::InlineString()` when an inline string
is required instead. `SharedStringTableService` (via
`editor->SharedStrings()`) exposes lookup, insertion, reference counting,
and cleanup of the shared string table directly — `Cleanup()` drops entries
no cell references anymore.

## Ranges

Rectangular ranges are read and written as a row-major
`std::vector<std::vector<ExcelCellValue>>` (`ExcelCellMatrix`):

```cpp
auto range = *CellRange::ParseA1("A1:C3");
auto read = sheet->GetRangeValues(range);        // RangeReadResult
if (read.Succeeded()) { /* read.Values */ }

sheet->SetRangeValues(range, matrix);            // atomic; validates shape first
sheet->ClearRange(range);
sheet->FillRange(range, ExcelCellValue::Number(0));
sheet->CopyRange(source, destinationTopLeft);
sheet->MoveRange(source, destinationTopLeft);    // rewrites local A1 references
```

Every range operation returns a `RangeOperationResult` with a structured
`RangeOperationError` and message; check `Succeeded()` (or use it directly
in a boolean context) rather than assuming success. Writes validate their
input — for example that the matrix shape matches the range — *before*
touching the sheet, so a failed call leaves the worksheet unchanged.

## Sheet-qualified references

`SheetCellRange` pairs a worksheet name with a `CellRange` for
sheet-qualified A1 formulas such as `Sheet1!$A$2:$A$5` or `'My Data'!$B$2` —
the format used by chart data-source references and named ranges.
`ToFormula()` always emits an absolute range and quotes the sheet name only
when required; `Parse()` accepts the same syntax back, including quoted
names:

```cpp
auto qualified = SheetCellRange("Sheet1", *CellRange::ParseA1("A2:A5"));
qualified.ToFormula();                    // "Sheet1!$A$2:$A$5"

auto parsed = SheetCellRange::Parse("'My Data'!$B$2");
parsed->Sheet();                          // "My Data"
parsed->Range();                          // CellRange for B2
```

## Structural edits

```cpp
sheet->InsertRows(2, 3);       // insert 3 rows before row 2
sheet->DeleteRows(2, 1);
sheet->InsertColumns(1, 1);
sheet->DeleteColumns(4, 1);
```

Structural edits update cell addresses, merged ranges, filters, hyperlinks,
validation/conditional-format ranges, and formula-owned ranges atomically.
`FormulaReferenceUpdatePolicy` controls whether formula *text* is rewritten
(`UpdateUnqualifiedA1References`, the default) or preserved byte-for-byte
(`PreserveFormulaText`); ExyokiOffice never evaluates formulas either way.
One caveat: structural edits do not rewrite defined-name formulas — see
[Named ranges](named-ranges.md).

## Merged cells

```cpp
sheet->MergeRange(*CellRange::ParseA1("A1:C1"));   // keeps the top-left value
sheet->UnmergeRange(*CellRange::ParseA1("A1:C1"));
auto merged = sheet->MergedRanges();
auto containing = sheet->MergedRangeAt(*CellAddress::ParseA1("B1"));
```
