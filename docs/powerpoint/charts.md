# Charts

`AddChart` inserts a graphic frame backed by a new chart part. A
presentation chart has no worksheet of its own, so each series carries
literal values that are written as the chart's display cache.

The namespace aliases from [Presentations](presentations.md) are assumed.

## Creating a chart

```cpp
PresentationChartDefinition chart;
chart.Type = PresentationChartType::Column;   // Bar, Line, Pie, Area, XyScatter, Bubble
chart.Title = "Quarterly revenue";
chart.CategoryAxisTitle = "Quarter";
chart.ValueAxisTitle = "USD (millions)";
chart.LegendPosition = PresentationChartLegendPosition::Right;
chart.Transform = {.Position = {MeasuringUnits(1.0, MeasurementUnit::Inch),
                                MeasuringUnits(1.0, MeasurementUnit::Inch)},
                   .Size = {MeasuringUnits(6.0, MeasurementUnit::Inch),
                            MeasuringUnits(3.5, MeasurementUnit::Inch)}};

PresentationChartSeries series;
series.Name = "2026";
series.Values = {12.0, 18.5, 9.0, 21.0};
series.Categories = std::vector<std::string>{"Q1", "Q2", "Q3", "Q4"};
chart.Series = {series};

auto chartShape = tree->AddChart(chart);
```

For `XyScatter` and `Bubble`, `Values` holds the Y values and `Categories`
holds the decimal text of the X values; `BubbleSizes` applies to bubble
charts on creation. The chart-space assembly and cache rewriting are shared
with the [Excel](../excel/charts.md) and [Word](../word/charts.md) chart
APIs.

## Reading and updating

Reading and updating work off the same cached values. `UpdateChartData`
preserves the chart's plot type, formatting, and series source formulas, and
`std::nullopt` for the title leaves it untouched (an empty string removes
it):

```cpp
auto info = chartShape->GetChart();          // std::nullopt for non-chart shapes
info->Type;                                  // PresentationChartType::Column
info->Series[0].Values;                      // cached numbers, not a live workbook read

series.Values = {30.0, 31.0, 32.0, 33.0};
chartShape->UpdateChartData({series}, "Updated revenue");
```

## The embedded workbook

A chart can carry an editable data source — a complete `.xlsx` package —
which is what PowerPoint's "Edit Data in Excel" opens:

```cpp
chartShape->SetChartEmbeddedWorkbook(xlsxBytes);   // optional editable data source
auto workbook = chartShape->GetChartEmbeddedWorkbook();
```

Once an embedded workbook is attached,
`RefreshChartDataFromEmbeddedWorkbook` re-reads it and rewrites the cache —
each series' existing source formula (for example `Sheet1!$B$2:$B$5`,
whether authored by `AddChart` or edited by hand) is resolved against the
workbook's sheets and cells:

```cpp
chartShape->SetChartEmbeddedWorkbook(xlsxBytes);
chartShape->RefreshChartDataFromEmbeddedWorkbook(); // false if any series formula can't be resolved
```

This is all-or-nothing: if any series' formula does not resolve to an
existing sheet and range in the workbook, the call fails and the chart's
cached data is left unchanged. The title, plot type, and formatting are
always preserved.

## Chart styles

A chart's optional style and color-style parts
(`c:chartStyle`/`cs:colorStyle`) are exposed losslessly as raw XML — this
library does not interpret the DrawingML 2013 chart-style schema, only
stores and returns it verbatim:

```cpp
chartShape->SetChartStyleXml(styleXml);
chartShape->SetChartColorStyleXml(colorXml);
auto style = chartShape->GetChartStyleXml();       // std::nullopt if absent
```
