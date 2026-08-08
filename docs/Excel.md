# Excel quickstart

This quickstart introduces `ExyokiOffice::Excel::ExcelDocumentEditor`, the
high-level API for authoring and editing Excel workbooks. It shows the
essential moves — creating, opening, saving, and a tour of cells, styles,
formulas, and charts — and hands off to the [Excel chapters](excel/workbooks.md)
for the thorough treatment of each topic.

Everything shown here lives in one header:

```cpp
#include "ExyokiOffice/Excel/ExcelDocument.hpp"
using namespace ExyokiOffice::Excel;
```

A complete runnable version of most snippets is in
[examples/ExampleExcelEditor/main.cpp](../examples/ExampleExcelEditor/main.cpp),
which builds a two-sheet report — styled data with formulas, a named range, a
summary sheet, a chart, a table with a slicer — recalculates it, and re-opens
the saved file to prove it round-trips. The full API reference is the Doxygen
documentation in
[ExcelDocument.hpp](../include/ExyokiOffice/Excel/ExcelDocument.hpp).

Two services are not pulled in by that header and need their own include:

```cpp
#include "ExyokiOffice/Excel/ExcelFormulaEngine.hpp"   // FormulaEngine
#include "ExyokiOffice/Excel/ExcelNamedRange.hpp"      // NamedRangeManager
```

## Hello world

```cpp
auto editor = ExcelDocumentEditor::CreateNew();
auto sheet = editor->FirstWorksheet();
sheet->SetCellText(1, 1, "Name");
sheet->SetCellNumber(2, 1, 42);
editor->SaveToFile("report.xlsx");
```

## Creating, opening, and saving

```cpp
auto fresh    = ExcelDocumentEditor::CreateNew();             // new .xlsx
auto fromDisk = ExcelDocumentEditor::Open("existing.xlsx");   // nullptr on failure
auto fromBytes= ExcelDocumentEditor::Open(bytes);             // std::vector<uint8_t> or std::span

editor->SaveToFile("out.xlsx");             // atomic save by default
std::vector<Byte> blob = editor->SaveToMemory();
```

All factories return `nullptr` when the source cannot be read or parsed —
always check the result. Macro-enabled and template package types, VBA
handling, undo snapshots, and workbook protection are covered in
[Workbooks](excel/workbooks.md).

## A short tour

Rows and columns are one-based (`A1` is row 1, column 1), and addresses
parse from A1 text:

```cpp
auto sheet = editor->AddWorksheet("Sales");
sheet->SetCellText(1, 1, "Product");
sheet->SetCellNumber(2, 1, 19.99);
sheet->SetCellFormula(*CellAddress::ParseA1("D2"), "B2*C2");

auto value = sheet->GetCellValue(2, 1);      // std::optional<ExcelCellValue>
```

Styles are registered once in the workbook's deduplicating catalog and
applied by index:

```cpp
ExcelStyle headerStyle;
headerStyle.Font = ExcelFont{.Color = ExcelColor::Rgb("FFFFFFFF"), .Bold = true};
headerStyle.Fill = ExcelFill{.Kind = ExcelFillKind::Pattern,
                             .Pattern = ExcelFillPattern::Solid,
                             .Foreground = ExcelColor::Rgb("FF4472C4")};
auto styles = editor->Styles();
auto header = styles.GetOrAdd(headerStyle);
styles.ApplyToRange(*sheet, *CellRange::ParseA1("A1:D1"), header.StyleIndex);
```

Formulas are stored as text; the built-in engine computes cached results so
non-Excel readers see numbers immediately:

```cpp
#include "ExyokiOffice/Excel/ExcelFormulaEngine.hpp"
FormulaEngine engine(editor->GetDocument());
engine.Recalculate();
```

A chart takes one builder chain:

```cpp
ChartBuilder(sheet)
    .SetType(ExcelChartType::Column)
    .SetTitle("Revenue by product")
    .SetAnchor(*CellAddress::ParseA1("G2"), *CellAddress::ParseA1("N20"))
    .AddSeries("Total", *CellRange::ParseA1("E4:E8"))
    .SetXAxisLabels(*CellRange::ParseA1("A4:A8"))
    .Build();
```

And the layout niceties — frozen header row, column width, a hyperlink —
are one call each:

```cpp
sheet->SetView(WorksheetView{.FrozenRows = 1});
sheet->SetColumnDimension(1, ColumnDimension{.Width = 18});
sheet->SetHyperlink(ExcelHyperlink{.Address = *CellAddress::ParseA1("F1"),
                                   .Target = "https://example.com"});
```

## The chapters

| Chapter | Covers |
| --- | --- |
| [Workbooks](excel/workbooks.md) | Lifecycle, snapshots, properties, themes, VBA, workbook protection. |
| [Worksheets](excel/worksheets.md) | Sheet management, copying between workbooks, sheet protection. |
| [Cells and ranges](excel/cells.md) | Values, shared strings, ranges, structural edits, merged cells. |
| [Styles and number formats](excel/styles.md) | `StyleRepository`, fonts, fills, borders, number formats. |
| [Named ranges](excel/named-ranges.md) | `NamedRangeManager`, scopes, name resolution. |
| [Formulas](excel/formulas.md) | The formula engine, supported functions, recalculation. |
| [Tables](excel/tables.md) | Worksheet tables and auto-filters. |
| [Charts](excel/charts.md) | `ChartBuilder`, series, cross-sheet data sources. |
| [Pivot tables](excel/pivot-tables.md) | Caches, definitions, aggregation, refresh. |
| [Slicers](excel/slicers.md) | Slicers over pivot tables and worksheet tables. |
| [Validation and conditional formatting](excel/validation.md) | Entry rules and value-driven formatting. |
| [Layout and annotations](excel/layout.md) | Dimensions, views, hyperlinks, comments, images. |
| [Printing](excel/printing.md) | Page setup, margins, print areas, headers and footers. |

## Known limitations

- Formulas are stored as text and are only evaluated when `FormulaEngine` is
  asked to; structural edits adjust references textually and never
  calculate. The engine implements a documented subset of Excel's function
  library, not all of it — see [Formulas](excel/formulas.md).
- Structural worksheet edits (row/column insertion or deletion, sheet
  removal or reordering) do not rewrite defined-name formulas.
- Rich-text shared-string runs are preserved but not synthesized by the
  high-level API yet; `SetCellText` writes plain-text shared strings.
- Conditional formatting rules reference `dxfs` differential-format entries
  by index but do not create them.

The [compatibility matrix](Compatibility.md) grades every Excel feature area
for create, edit, and preserve support, alongside the supported document
types and Office versions.
