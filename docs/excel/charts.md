# Charts

`ChartBuilder` assembles a chart and anchors it into a worksheet drawing;
`ExcelChartDefinition` is the underlying structure for callers that prefer
filling in a struct. Both come with `ExcelDocument.hpp` — no extra include
is needed.

```cpp
#include "ExyokiOffice/Excel/ExcelDocument.hpp"
using namespace ExyokiOffice::Excel;
```

## Building a chart

```cpp
auto chartId = ChartBuilder(sheet)
                   .SetType(ExcelChartType::Column)
                   .SetTitle("Revenue by product")
                   .SetCategoryAxisTitle("Product")
                   .SetValueAxisTitle("Total")
                   .SetAnchor(*CellAddress::ParseA1("G2"), *CellAddress::ParseA1("N20"))
                   .AddSeries("Total", *CellRange::ParseA1("E4:E8"))
                   .SetXAxisLabels(*CellRange::ParseA1("A4:A8"))
                   .Build();
```

`Build()` returns the drawing object identifier, or `std::nullopt` when the
definition is incomplete — a chart needs at least one series and a valid
two-cell anchor. Column, bar, line, pie, area, XY scatter, and bubble plots
are available through `ExcelChartType`; `ShowLegend(bool,
ExcelLegendPosition)` and `ShowGridLines(bool)` control the surrounding
chrome. `SetXAxisLabels` applies the category range to every series added
*so far*, so call it again after adding more series.

## Series on another worksheet

Chart ranges carry no sheet name of their own. They resolve against the
worksheet the chart is anchored into, which silently plots the wrong cells
when a chart is placed on a summary sheet and reads a data sheet.
`SetSourceSheet` names the worksheet the ranges live on:

```cpp
ChartBuilder(summarySheet)
    .SetType(ExcelChartType::Column)
    .SetAnchor(*CellAddress::ParseA1("D2"), *CellAddress::ParseA1("K20"))
    .SetSourceSheet("Sales Report")               // where the data is
    .AddSeries("Total", *CellRange::ParseA1("E4:E8"))
    .SetXAxisLabels(*CellRange::ParseA1("A4:A8"))
    .Build();
```

The name applies to series added before and after the call, so its position
in the chain does not matter. Passing an empty name restores host-worksheet
resolution.

Ranges are written as sheet-qualified absolute references. Values from the
host worksheet are additionally embedded as numeric and string caches, so
the chart renders in viewers that never evaluate a formula; cross-sheet
series emit the reference only, and Excel fills the values in on open.

## Combination charts and a secondary axis

A series can be drawn as a different type from the chart, and against a
secondary value axis on the opposite side — columns of amounts with a line of
percentages over them, say. Both are set per series on the definition:

```cpp
ExcelChartDefinition chart;
chart.Type = ExcelChartType::Column;
chart.From = *CellAddress::ParseA1("E2");
chart.To = *CellAddress::ParseA1("L20");
chart.SecondaryValueAxisTitle = "Margin";

ExcelChartSeries revenue;
revenue.Name = "Revenue";
revenue.Values = *CellRange::ParseA1("B2:B5");
revenue.Categories = *CellRange::ParseA1("A2:A5");

ExcelChartSeries margin = revenue;
margin.Name = "Margin";
margin.Values = *CellRange::ParseA1("C2:C5");
margin.Type = ExcelChartType::Line;     // drawn as a line over the columns
margin.SecondaryAxis = true;            // against its own scale on the right

chart.Series = {revenue, margin};
sheet->AddChart(chart);
```

The chart is written the way Excel writes one: a plot group per type and axis,
and a second axis pair whose value axis crosses at the maximum and whose
category axis is hidden. Only kinds that share axes combine — column, line and
area with each other, and bar, scatter, bubble and pie only with their own
kind — and at least one series has to stay on the primary axis. `AddChart`
returns `std::nullopt` for any other combination and adds nothing.

`Charts()` reads the combination back: a series drawn as the chart's own type
has an empty `Type`, and every series reports its `SecondaryAxis`.

## Reading, updating, and removing

`Worksheet::AddChart(ExcelChartDefinition)` takes the same structure the
builder assembles, for callers that would rather fill in a struct — and it
is the only way to give individual series different `SourceSheet` values.

```cpp
auto charts = sheet->Charts();              // one definition per chart anchor
auto definition = charts.front();
definition.Title = "Updated title";
sheet->UpdateChart(definition);             // keeps the chart part identity
sheet->RemoveChart(definition.Id);          // drops empty drawing parts too
```

`Charts()` reads back type, anchors, titles, legend and gridline settings,
and series formula references. The embedded value caches are not read back:
they are write-only presentation data, and the workbook cells remain the
single source of truth.

The chart-space assembly is shared across the library — the same schema
backs [charts in PowerPoint](../powerpoint/charts.md) and the
[chart reading and updating API in Word](../word/charts.md).
