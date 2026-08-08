# Charts embedded in Word documents

Word documents frequently carry charts pasted from Excel or produced by other
Open XML tools. ExyokiOffice does not create new chart anchors in Word, but
it reads and updates the charts a document already contains — enough to
refresh a report template's charts with current numbers.

```cpp
#include "ExyokiOffice/Word/WordDocument.hpp"
using namespace ExyokiOffice::Word;
```

## Reading charts

```cpp
for (const auto& chart : editor->Charts())
{
    chart.Title;
    chart.Type;                  // WordChartType: Column, Bar, Line, Pie, ...
    chart.RelationshipId;        // handle for the update calls below
    chart.Series;                // names, cached values, category labels
    chart.HasEmbeddedWorkbook;
}
```

`Charts()` scans the document body for `w:drawing` references to chart parts
and parses each chart's title, plot type, and series *including the embedded
value and label caches* — so the numbers are available without an embedded
workbook or any recalculation. This works for charts produced by Word,
Excel, PowerPoint, or any other Open XML tool. Only the body is scanned;
charts in headers, footers, or notes (exceedingly rare) are not.

## Updating chart data

```cpp
auto chart = editor->Charts().front();
editor->UpdateChartData(chart.RelationshipId,
                        {{"Actuals", {12.0, 18.0, 9.0}},
                         {"Forecast", {10.0, 20.0, 15.0}}},
                        "Q3 Results");
```

`UpdateChartData` rewrites the chart part's title and series caches in
place; the chart's position, relationship ID, and embedded workbook are left
untouched. The series list replaces what the chart had, so series can be
added, removed, or reordered. Details worth knowing:

- Each rebuilt series keeps its previous source formula (`c:f`) by position;
  a series beyond the previous count gets an empty formula.
- Rebuilding a series drops per-series styling that previously existed on it
  (data point colors, markers, data labels) — the same shape the Excel chart
  writer produces.
- For scatter and bubble charts, each series' category strings are parsed as
  numbers and written as the X values.
- Passing `std::nullopt` as the title leaves the existing title unchanged;
  an empty string removes it.

## The embedded workbook

Charts pasted from Excel usually carry a complete `.xlsx` package behind the
chart part — the data source behind Word's "Edit Data in Excel". The editor
exposes it as bytes that round-trip through the Excel editor:

```cpp
auto bytes = editor->GetChartEmbeddedWorkbook(chart.RelationshipId);
if (bytes)
{
    auto workbook = Excel::ExcelDocumentEditor::Open(*bytes);
    workbook->FirstWorksheet()->SetCellNumber(2, 2, 42.0);
    editor->SetChartEmbeddedWorkbook(chart.RelationshipId, workbook->SaveToMemory());
}
```

`SetChartEmbeddedWorkbook` does not touch the chart's displayed cache — call
`UpdateChartData` as well when the visible numbers should match the new
workbook contents.

Creating charts from scratch is an Excel and PowerPoint feature: build the
chart there ([Excel charts](../excel/charts.md),
[PowerPoint charts](../powerpoint/charts.md)) or copy a document that
already contains one.
