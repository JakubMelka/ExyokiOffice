# Excel pivot tables

This guide covers the high-level pivot table API in
`ExyokiOffice::Excel`. It builds a complete SpreadsheetML pivot table —
pivot cache, cache records, pivot table definition, workbook registry entry —
from a worksheet range, and it also computes the report itself so the result
is readable without a spreadsheet application.

```cpp
#include "ExyokiOffice/Excel/ExcelDocument.hpp"
using namespace ExyokiOffice::Excel;
```

The full API reference is the Doxygen documentation in
[ExcelPivotTable.hpp](../../include/ExyokiOffice/Excel/ExcelPivotTable.hpp).
The general Excel guide is [Excel quickstart](../Excel.md).

## Hello world

```cpp
auto editor = ExcelDocumentEditor::CreateNew();
editor->RenameWorksheet(0, "Data");
auto data   = editor->FirstWorksheet();
auto report = editor->AddWorksheet("Report");

// ... fill Data!A1:C13 with a header row and twelve data rows ...

auto pivot = PivotTableBuilder(report)
                 .SetName("SalesByRegion")
                 .SetSource(*data, *CellRange::ParseA1("A1:C13"))
                 .SetTarget(*CellAddress::ParseA1("A1"))
                 .AddRowField("Region")
                 .AddColumnField("Quarter")
                 .AddDataField("Amount")          // "Sum of Amount"
                 .Build();

editor->SaveToFile("report.xlsx");
```

`Build()` returns `nullptr` on failure. When you need the reason, call
`Worksheet::CreatePivotTable` directly; it returns a `PivotTableCreationResult`
carrying both the pivot table and the structured status:

```cpp
ExcelPivotTableDefinition definition;
definition.Name        = "SalesByRegion";
definition.SourceSheet = "Data";
definition.SourceRange = *CellRange::ParseA1("A1:C13");
definition.TargetCell  = *CellAddress::ParseA1("A1");
definition.Fields      = {{"Region", PivotAxis::Row}, {"Quarter", PivotAxis::Column}};
definition.DataFields  = {ExcelPivotDataField{"Amount"}};

auto created = report->CreatePivotTable(definition);
if (!created)
{
    // created.Status.Error carries the code, created.PivotTable is nullptr
    std::cerr << created.Status.Message << '\n';
}
auto pivot = created.PivotTable;
```

## What gets created

One pivot table owns three package parts plus one workbook entry:

| Artifact | Owner | Purpose |
|---|---|---|
| `/xl/pivotTables/pivotTableN.xml` | the hosting worksheet | field placement, layout, style |
| `/xl/pivotCache/pivotCacheDefinitionN.xml` | the workbook | source reference and the distinct values of each field |
| `/xl/pivotCache/pivotCacheRecordsN.xml` | the cache definition | one record per source row |
| `workbook.xml/pivotCaches/pivotCache` | the workbook | binds a workbook-unique `cacheId` to the cache part |

All four are created, kept consistent, and removed together. The operation is
atomic: a validation or write failure leaves the workbook exactly as it was.

## The source range

The source is a rectangle on any worksheet of the same workbook. Its first row
supplies the field names.

```cpp
definition.SourceSheet = "Data";              // empty = the worksheet hosting the report
definition.SourceRange = *CellRange::ParseA1("A1:D101");
```

Requirements, each reported through a distinct `PivotTableError`:

- the sheet must exist (`InvalidSource`);
- the range needs a header row plus at least one data row (`InvalidSource`);
- every header cell must be non-blank and unique, compared case-insensitively
  (`InvalidSourceHeader`).

Every source column becomes a pivot cache field, whether or not it is placed.

Source cells are read through the same typed model as the rest of the Excel
API: shared strings are resolved, and a formula cell contributes its cached
result. A cell with no cached result counts as blank.

## Placing fields

`ExcelPivotTableDefinition::fields` lists only the columns you actually use;
every other source column stays an unplaced pivot field, available in the
spreadsheet application's field list. **The order of the list is the nesting
order of each axis.**

```cpp
definition.Fields = {
    {"Year",    PivotAxis::Row},               // outer row field
    {"Region",  PivotAxis::Row},               // nested inside Year
    {"Quarter", PivotAxis::Column},
};
```

| Axis | Effect |
|---|---|
| `PivotAxis::Row` | groups report rows; one label column per row field |
| `PivotAxis::Column` | groups report columns |
| `PivotAxis::Page` | a report filter written above the report |
| `PivotAxis::None` | not placed |

A column may be placed at most once (`InvalidFieldConfiguration`), and its name
must exist in the source (`UnknownField`).

### Pivot items and their order

The distinct values of every placed field become that field's items, sorted
ascending: blanks first, then numbers, then booleans, then text, then error
literals; text compares case-insensitively with a byte-order tiebreak. Values
that differ only in letter case collapse into one item, keeping the casing that
appears first in the source — the same rule Excel applies to item labels.

`ExcelPivotTable::FieldItems("Region")` returns those items in exactly that
order, and the zero-based position is the index used by
`ExcelPivotField::selectedItem` and `ExcelPivotDataField::baseItem`.

### Report filters

```cpp
ExcelPivotField filter;
filter.Name         = "Year";
filter.Axis         = PivotAxis::Page;
filter.SelectedItem = 1;                // std::nullopt selects (All)
definition.Fields.push_back(filter);
```

A page field genuinely filters the computed report, not just the stored
markup. Each report filter occupies one line above the report — a label cell
and a selection cell — followed by one blank separator line.

## Data fields

Data fields are the aggregated value columns, and at least one is required.
They always sit on the column axis, as its innermost level.

```cpp
definition.DataFields = {
    ExcelPivotDataField{"Amount"},                                        // Sum of Amount
    ExcelPivotDataField{"Amount", "Orders", PivotAggregateFunction::Count},
    ExcelPivotDataField{"Units",  "",       PivotAggregateFunction::Average},
};
```

An empty `name` is generated from the function and source column
(`Sum of Amount`, `Count of Amount`, `Average of Units`, …). Names must be
unique within the pivot table.

| `PivotAggregateFunction` | Result |
|---|---|
| `Sum` | sum of the numeric values |
| `Count` | number of non-blank values (Excel's `COUNTA`) |
| `CountNumbers` | number of numeric values (Excel's `COUNT`) |
| `Average` | mean of the numeric values |
| `Maximum`, `Minimum` | numeric extremes |
| `Product` | product of the numeric values |
| `Variance`, `VarianceP` | sample and population variance |
| `StandardDeviation`, `StandardDeviationP` | sample and population standard deviation |

Blank source cells never take part in an aggregate; text and error cells count
towards `Count` only. A group with nothing to aggregate leaves its report cell
blank — that includes `Variance` and `StandardDeviation` over fewer than two
numeric values.

Subtotals and grand totals are computed from the underlying source rows, not
by combining already-aggregated cells, so `Average` and the variance family are
correct at every level.

`ShowDataAs`, `BaseField`, `BaseItem`, and `NumberFormatId` are stored on the
data field for spreadsheet applications that recalculate the report. The cached
cells that ExyokiOffice writes always hold the raw aggregate.

## The cached report layout

Unless `WriteCachedReport` is disabled, the computed report is written into the
hosting worksheet as ordinary cell values. The layout is a deterministic
tabular form — `outline="0" compact="0"`, subtotals below their group:

```
                 ← label columns →  ←    value columns    →
report filters   Year               2024
                 (blank separator line)
header rows      Quarter            Q1     Q2     (grand)
                 Region             Sum…   Sum…   Grand Total
body             East               15     20     35
                 West               30     40     70
                 Grand Total        45     60     105
```

- **Label columns**: one per row field, or a single column when there are no
  row fields. Repeated group labels are printed only where the group starts.
- **Header rows**: one row per column field, naming that field in the first
  label column and captioning its items at each group start, then one final row
  holding the row field names and the data field names.
- **Body**: one line per row-item combination, plus subtotal and grand total
  lines. A subtotal label reads `<item> Total`; the grand total label is
  `GrandTotalCaption`, defaulting to `Grand Total`.
- **Value cells** are written as numbers; empty groups are left blank.

`ReportRange()` returns the rectangle recorded in `location/@ref` — header rows
plus body. `WrittenRange()` additionally covers the report filter lines above
it, and is the rectangle that is cleared when the pivot table is rebuilt or
removed.

Excel re-renders the report from the cache when it refreshes, which by default
happens on open (`RefreshOnLoad`). Its rendering may differ cosmetically from
the layout above; the numbers agree.

```cpp
definition.WriteCachedReport = false;   // only markup, no report cells
definition.RefreshOnLoad     = false;   // show exactly what ExyokiOffice wrote
```

### Subtotals

Set `ShowSubtotal` on a row or column field that has another field nested
inside it. The innermost field of an axis never produces subtotals, because
each of its groups is already a single line.

```cpp
definition.Fields = {
    {"Region",  PivotAxis::Row, /*showSubtotal*/ true},
    {"Quarter", PivotAxis::Row},
};
```

```
Region   Quarter   Sum of Amount
East     Q1        15
East     Q2        20
East Total         35
West     Q1        30
West Total         30
Grand Total        45
```

### Grand totals

```cpp
definition.RowGrandTotals    = false;   // no grand total line below the body
definition.ColumnGrandTotals = false;   // no grand total column
pivot->SetGrandTotals(true, true);      // or change them later
```

## Reading a pivot table back

```cpp
auto pivot = report->PivotTableByName("SalesByRegion");   // case-insensitive
for (const auto& other : report->PivotTables()) { /* ... */ }

pivot->Name();
pivot->SourceSheet();        // "Data"
pivot->SourceRange();        // A1:D101
pivot->CacheId();
pivot->SourceFieldNames();   // every cache field, in order
pivot->Fields();             // placement of every source column
pivot->DataFields();
pivot->Style();
pivot->ReportRange();
```

`Definition()` reconstructs the whole `ExcelPivotTableDefinition`, including
axis nesting order, so the round trip
`auto d = *pivot->Definition(); /* edit */ pivot->Update(d);` is the general
way to change an existing pivot table.

`AggregatedValue()` resolves a single report value without touching the
worksheet — useful for verification and when `WriteCachedReport` is off:

```cpp
pivot->AggregatedValue({"East"}, {"Q1"});        // one cell
pivot->AggregatedValue({"East"}, {});            // East across all columns
pivot->AggregatedValue({}, {});                  // grand total
pivot->AggregatedValue({"East"}, {}, "Orders");  // a named data field
```

Fewer captions than there are fields on that axis selects the corresponding
subtotal group; an empty vector selects the total across the whole axis.

## Updating and refreshing

Every mutating call rebuilds the cache, the definition, and the report cells,
clearing the previously written rectangle first. The pivot table keeps its
package parts, its `cacheId`, and its relationships, so anything already
referring to it stays valid. On failure the workbook is left unchanged.

```cpp
pivot->Refresh();                                       // re-read the same source
pivot->SetSourceRange(*CellRange::ParseA1("A1:D201"));  // grow the source
pivot->SetSourceRange(range, "Data2024");               // and switch sheets
pivot->MoveTo(*CellAddress::ParseA1("H2"));
pivot->SetFieldAxis("Quarter", PivotAxis::Column);
pivot->SetFieldAxis("Quarter", PivotAxis::None);        // unplace it
pivot->SetGrandTotals(true, false);
pivot->SetName("Q1Report");
pivot->Update(editedDefinition);
```

`Refresh()` drops items that disappeared from the source and adds new ones. A
report filter selection that no longer resolves falls back to `(All)`.

## Style

```cpp
ExcelPivotTableStyle style;
style.Name             = "PivotStyleMedium9";
style.ShowRowStripes   = true;
style.ShowLastColumn   = false;
pivot->SetStyle(style);

pivot->SetStyle(ExcelPivotTableStyle{""});   // remove pivotTableStyleInfo
```

The name refers to a built-in or workbook-defined table style. Built-in style
names are not stored in the package, so the name is not validated.

## Naming

Pivot table names must be non-empty, at most 255 characters, free of control
characters and of leading or trailing spaces, and unique across the whole
workbook when compared case-insensitively. `IsValidPivotTableName` exposes the
syntax check. An empty `ExcelPivotTableDefinition::name` requests a generated
`PivotTableN` name.

## Removing

```cpp
report->RemovePivotTable(pivot);
```

This removes the pivot table part, the workbook `pivotCaches` entry, the cache
definition and records parts, and clears the report cells. A cache still used
by another pivot table is retained. Wrappers held by callers become detached.

## Errors

`PivotTableResult` pairs a `PivotTableError` code with a diagnostic message. It
is returned directly by the mutating methods, and as the `Status` member of the
`PivotTableCreationResult` that `Worksheet::CreatePivotTable` returns.

| Code | Meaning |
|---|---|
| `None` | success |
| `InvalidWorksheet` | the worksheet, workbook, or pivot table part is detached |
| `InvalidName` | the name is invalid or already used in the workbook |
| `InvalidSource` | the source range or its worksheet is unusable |
| `InvalidSourceHeader` | a header cell is blank or duplicated |
| `UnknownField` | a field or data field names a column the source does not have |
| `InvalidFieldConfiguration` | a field is placed twice, a data field name repeats, or there is no data field |
| `InvalidTarget` | the target cell is invalid or the report would leave the grid |
| `OverlappingReport` | the report would overlap its own source or another pivot table |
| `WriteFailed` | a part, relationship, or cell write failed |

## Escape hatch to the low-level DOM

```cpp
pivot->GetPart();                  // Packaging::PivotTablePart
pivot->GetCacheDefinitionPart();   // Packaging::PivotTableCacheDefinitionPart
pivot->GetCacheRecordsPart();      // Packaging::PivotTableCacheRecordsPart

auto root = pivot->GetPart()->GetPivotTableDefinition();   // typed SpreadsheetML DOM
```

## Known limitations

- The source is always a worksheet range. External, consolidation, scenario,
  and OLAP cache sources are not created by this API, although the typed DOM
  can express them.
- `ShowDataAs` is stored but not evaluated; the cached cells hold raw
  aggregates.
- Item grouping (dates into months/quarters, numbers into bins), calculated
  fields, calculated items, label and value filters, and pivot charts are not
  part of this API. They remain reachable through the typed DOM.
- Slicers over a pivot table field are supported; see
  [slicers.md](slicers.md).
- `NumberFormatId` is recorded on the data field but is not applied to the
  cached report cells, which keep the worksheet's default cell format.

All three generated parts validate cleanly against `OpenXmlDomValidator`.
Note that `x:r` is declared both by the shared-string rich text run
(`CT_RElt`) and by the pivot cache record (`CT_Record`); validation resolves
the two from the surrounding content model, so reading pivot cache records
through the untyped `OpenXMLElement::Children()` still yields the rich text
run class. Use `ChildrenInContentModel()` or the typed accessors instead.
