// Copyright (c) 2026 Jakub Melka and Contributors
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include <doctest.h>

#include "ExyokiOffice/Excel/ExcelDocument.hpp"

#include <algorithm>
#include <cstddef>
#include <optional>
#include <string>

using namespace ExyokiOffice::Excel;

namespace
{

void FillSampleData(const Worksheet::Ptr& sheet)
{
    // Categories in column A, values in columns B and C.
    sheet->SetCellText(1, 1, "Q1");
    sheet->SetCellText(2, 1, "Q2");
    sheet->SetCellText(3, 1, "Q3");
    sheet->SetCellNumber(1, 2, 10.0);
    sheet->SetCellNumber(2, 2, 20.0);
    sheet->SetCellNumber(3, 2, 30.0);
    sheet->SetCellNumber(1, 3, 5.0);
    sheet->SetCellNumber(2, 3, 15.0);
    sheet->SetCellNumber(3, 3, 25.0);
}

} // namespace

TEST_CASE("ChartBuilder inserts a column chart with cached data [unit] [excel] [excel-chart]")
{
    auto editor = ExcelDocumentEditor::CreateNew();
    auto sheet = editor->FirstWorksheet();
    FillSampleData(sheet);

    const auto id = ChartBuilder(sheet)
                        .SetType(ExcelChartType::Column)
                        .SetTitle("Sales")
                        .SetAnchor(*CellAddress::ParseA1("E2"), *CellAddress::ParseA1("K20"))
                        .AddSeries("North", *CellRange::ParseA1("B1:B3"))
                        .AddSeries("South", *CellRange::ParseA1("C1:C3"))
                        .SetXAxisLabels(*CellRange::ParseA1("A1:A3"))
                        .Build();

    REQUIRE(id);

    auto drawing = sheet->GetPart()->GetDrawingsPart();
    REQUIRE(drawing);
    REQUIRE(drawing->GetChartParts().size() == 1);

    const auto chartXml = drawing->GetChartParts().front()->GetXmlString();
    CHECK(chartXml.find("<c:barChart") != std::string::npos);
    CHECK(chartXml.find("<c:barDir val=\"col\"") != std::string::npos);
    // Sheet-qualified absolute references and embedded caches.
    CHECK(chartXml.find("Sheet1!$B$1:$B$3") != std::string::npos);
    CHECK(chartXml.find("<c:numCache") != std::string::npos);
    CHECK(chartXml.find("<c:strCache") != std::string::npos);
    CHECK(chartXml.find("<c:v>20</c:v>") != std::string::npos);
    CHECK(chartXml.find("<c:v>Q2</c:v>") != std::string::npos);

    // The worksheet references the drawing part. The link element carries the
    // SpreadsheetML prefix, so it lands in the right namespace and the
    // worksheet stays schema-valid.
    CHECK(sheet->GetPart()->GetXmlString().find("<x:drawing r:id=") != std::string::npos);
}

TEST_CASE("Charts enumerate and round-trip through a package [unit] [excel] [excel-chart]")
{
    auto editor = ExcelDocumentEditor::CreateNew();
    auto sheet = editor->FirstWorksheet();
    FillSampleData(sheet);

    ExcelChartDefinition definition;
    definition.Type = ExcelChartType::Line;
    definition.Title = "Trend";
    definition.From = *CellAddress::ParseA1("E2");
    definition.To = *CellAddress::ParseA1("K20");
    ExcelChartSeries series;
    series.Name = "North";
    series.Values = *CellRange::ParseA1("B1:B3");
    series.Categories = *CellRange::ParseA1("A1:A3");
    definition.Series.push_back(series);

    const auto id = sheet->AddChart(definition);
    REQUIRE(id);
    REQUIRE(sheet->Charts().size() == 1);
    CHECK(sheet->Charts().front().Type == ExcelChartType::Line);
    CHECK(sheet->Charts().front().Title == "Trend");
    REQUIRE(sheet->Charts().front().Series.size() == 1);
    CHECK(sheet->Charts().front().Series.front().Name == "North");
    REQUIRE(sheet->Charts().front().Series.front().Values.IsValid());
    CHECK(sheet->Charts().front().Series.front().Values.ToA1() == "B1:B3");

    const auto bytes = editor->SaveToMemory();
    REQUIRE_FALSE(bytes.empty());
    auto reopened = ExcelDocumentEditor::Open(bytes);
    REQUIRE(reopened);
    auto reopenedCharts = reopened->FirstWorksheet()->Charts();
    REQUIRE(reopenedCharts.size() == 1);
    CHECK(reopenedCharts.front().Type == ExcelChartType::Line);
    CHECK(reopenedCharts.front().Title == "Trend");
}

TEST_CASE("Removing a chart cleans the drawing part [unit] [excel] [excel-chart]")
{
    auto editor = ExcelDocumentEditor::CreateNew();
    auto sheet = editor->FirstWorksheet();
    FillSampleData(sheet);

    const auto id = ChartBuilder(sheet)
                        .SetType(ExcelChartType::Pie)
                        .SetAnchor(*CellAddress::ParseA1("E2"), *CellAddress::ParseA1("K20"))
                        .AddSeries("Share", *CellRange::ParseA1("B1:B3"))
                        .SetXAxisLabels(*CellRange::ParseA1("A1:A3"))
                        .Build();
    REQUIRE(id);

    CHECK(sheet->RemoveChart(*id));
    CHECK(sheet->Charts().empty());
    CHECK(sheet->GetPart()->GetDrawingsPart() == nullptr);
}

TEST_CASE("Charts and images share a drawing without id collisions [unit] [excel] [excel-chart]")
{
    auto editor = ExcelDocumentEditor::CreateNew();
    auto sheet = editor->FirstWorksheet();
    FillSampleData(sheet);

    ExcelWorksheetImage image;
    image.From = *CellAddress::ParseA1("A10");
    image.To = *CellAddress::ParseA1("C15");
    image.Data = {0x89, 0x50, 0x4e, 0x47};
    const auto imageId = sheet->AddImage(image);
    REQUIRE(imageId);

    const auto chartId = ChartBuilder(sheet)
                             .SetType(ExcelChartType::Bar)
                             .SetAnchor(*CellAddress::ParseA1("E2"), *CellAddress::ParseA1("K20"))
                             .AddSeries("North", *CellRange::ParseA1("B1:B3"))
                             .SetXAxisLabels(*CellRange::ParseA1("A1:A3"))
                             .Build();
    REQUIRE(chartId);
    CHECK(*chartId != *imageId);

    CHECK(sheet->Images().size() == 1);
    CHECK(sheet->Charts().size() == 1);

    // Removing the image leaves the chart intact.
    CHECK(sheet->RemoveImage(*imageId));
    CHECK(sheet->Images().empty());
    CHECK(sheet->Charts().size() == 1);
    CHECK(sheet->GetPart()->GetDrawingsPart() != nullptr);
}

TEST_CASE("UpdateChart replaces type, title, series, and anchor while keeping the chart part identity [unit] [excel] [excel-chart]")
{
    auto editor = ExcelDocumentEditor::CreateNew();
    auto sheet = editor->FirstWorksheet();
    FillSampleData(sheet);

    const auto id = ChartBuilder(sheet)
                        .SetType(ExcelChartType::Column)
                        .SetTitle("Original")
                        .SetAnchor(*CellAddress::ParseA1("E2"), *CellAddress::ParseA1("K20"))
                        .AddSeries("North", *CellRange::ParseA1("B1:B3"))
                        .SetXAxisLabels(*CellRange::ParseA1("A1:A3"))
                        .Build();
    REQUIRE(id);

    auto drawing = sheet->GetPart()->GetDrawingsPart();
    REQUIRE(drawing);
    REQUIRE(drawing->GetChartParts().size() == 1);
    const auto originalRelationshipId = drawing->GetChartParts().front()->RelationshipId();

    ExcelChartDefinition updated;
    updated.Id = *id;
    updated.Type = ExcelChartType::Line;
    updated.Title = "Updated";
    updated.Name = "Renamed chart";
    updated.From = *CellAddress::ParseA1("F3");
    updated.To = *CellAddress::ParseA1("L21");
    ExcelChartSeries series;
    series.Name = "South";
    series.Values = *CellRange::ParseA1("C1:C3");
    series.Categories = *CellRange::ParseA1("A1:A3");
    updated.Series.push_back(series);

    CHECK(sheet->UpdateChart(updated));

    // Still exactly one chart part, and it is the same one (identity preserved).
    REQUIRE(drawing->GetChartParts().size() == 1);
    CHECK(drawing->GetChartParts().front()->RelationshipId() == originalRelationshipId);

    const auto charts = sheet->Charts();
    REQUIRE(charts.size() == 1);
    const auto& chart = charts.front();
    CHECK(chart.Id == *id);
    CHECK(chart.Type == ExcelChartType::Line);
    CHECK(chart.Title == "Updated");
    CHECK(chart.Name == "Renamed chart");
    CHECK(chart.From.ToA1() == "F3");
    CHECK(chart.To.ToA1() == "L21");
    REQUIRE(chart.Series.size() == 1);
    CHECK(chart.Series.front().Name == "South");
    REQUIRE(chart.Series.front().Values.IsValid());
    CHECK(chart.Series.front().Values.ToA1() == "C1:C3");
}

TEST_CASE("UpdateChart refreshes the embedded cache from current cell contents [unit] [excel] [excel-chart]")
{
    auto editor = ExcelDocumentEditor::CreateNew();
    auto sheet = editor->FirstWorksheet();
    FillSampleData(sheet);

    const auto id = ChartBuilder(sheet)
                        .SetType(ExcelChartType::Column)
                        .SetAnchor(*CellAddress::ParseA1("E2"), *CellAddress::ParseA1("K20"))
                        .AddSeries("North", *CellRange::ParseA1("B1:B3"))
                        .SetXAxisLabels(*CellRange::ParseA1("A1:A3"))
                        .Build();
    REQUIRE(id);

    // Change the underlying cell values after the chart was created.
    sheet->SetCellNumber(1, 2, 111.0);
    sheet->SetCellNumber(2, 2, 222.0);
    sheet->SetCellNumber(3, 2, 333.0);

    ExcelChartDefinition updated;
    updated.Id = *id;
    updated.Type = ExcelChartType::Column;
    updated.From = *CellAddress::ParseA1("E2");
    updated.To = *CellAddress::ParseA1("K20");
    ExcelChartSeries series;
    series.Name = "North";
    series.Values = *CellRange::ParseA1("B1:B3");
    series.Categories = *CellRange::ParseA1("A1:A3");
    updated.Series.push_back(series);

    CHECK(sheet->UpdateChart(updated));

    auto drawing = sheet->GetPart()->GetDrawingsPart();
    REQUIRE(drawing);
    REQUIRE(drawing->GetChartParts().size() == 1);
    const auto chartXml = drawing->GetChartParts().front()->GetXmlString();
    CHECK(chartXml.find("<c:v>111</c:v>") != std::string::npos);
    CHECK(chartXml.find("<c:v>222</c:v>") != std::string::npos);
    CHECK(chartXml.find("<c:v>333</c:v>") != std::string::npos);
    // Stale cached values are gone, not merely appended alongside the new ones.
    CHECK(chartXml.find("<c:v>10</c:v>") == std::string::npos);
}

TEST_CASE("UpdateChart fails for a chart id that does not exist [unit] [excel] [excel-chart]")
{
    auto editor = ExcelDocumentEditor::CreateNew();
    auto sheet = editor->FirstWorksheet();
    FillSampleData(sheet);

    const auto id = ChartBuilder(sheet)
                        .SetType(ExcelChartType::Column)
                        .SetAnchor(*CellAddress::ParseA1("E2"), *CellAddress::ParseA1("K20"))
                        .AddSeries("North", *CellRange::ParseA1("B1:B3"))
                        .Build();
    REQUIRE(id);

    ExcelChartDefinition updated;
    updated.Id = *id + 1000;
    updated.Type = ExcelChartType::Line;
    updated.From = *CellAddress::ParseA1("E2");
    updated.To = *CellAddress::ParseA1("K20");
    ExcelChartSeries series;
    series.Name = "North";
    series.Values = *CellRange::ParseA1("B1:B3");
    updated.Series.push_back(series);

    CHECK_FALSE(sheet->UpdateChart(updated));
    CHECK(sheet->Charts().size() == 1);
    CHECK(sheet->Charts().front().Type == ExcelChartType::Column);
}

TEST_CASE("UpdateChart rejects an empty series list and an invalid anchor [unit] [excel] [excel-chart]")
{
    auto editor = ExcelDocumentEditor::CreateNew();
    auto sheet = editor->FirstWorksheet();
    FillSampleData(sheet);

    const auto id = ChartBuilder(sheet)
                        .SetType(ExcelChartType::Column)
                        .SetAnchor(*CellAddress::ParseA1("E2"), *CellAddress::ParseA1("K20"))
                        .AddSeries("North", *CellRange::ParseA1("B1:B3"))
                        .Build();
    REQUIRE(id);

    ExcelChartDefinition noSeries;
    noSeries.Id = *id;
    noSeries.From = *CellAddress::ParseA1("E2");
    noSeries.To = *CellAddress::ParseA1("K20");
    CHECK_FALSE(sheet->UpdateChart(noSeries));

    ExcelChartDefinition invalidAnchor;
    invalidAnchor.Id = *id;
    invalidAnchor.From = *CellAddress::ParseA1("K20");
    invalidAnchor.To = *CellAddress::ParseA1("E2"); // to before from
    ExcelChartSeries series;
    series.Name = "North";
    series.Values = *CellRange::ParseA1("B1:B3");
    invalidAnchor.Series.push_back(series);
    CHECK_FALSE(sheet->UpdateChart(invalidAnchor));
}

TEST_CASE("UpdateChart changes survive a save/reopen round-trip [unit] [excel] [excel-chart]")
{
    auto editor = ExcelDocumentEditor::CreateNew();
    auto sheet = editor->FirstWorksheet();
    FillSampleData(sheet);

    const auto id = ChartBuilder(sheet)
                        .SetType(ExcelChartType::Column)
                        .SetTitle("Original")
                        .SetAnchor(*CellAddress::ParseA1("E2"), *CellAddress::ParseA1("K20"))
                        .AddSeries("North", *CellRange::ParseA1("B1:B3"))
                        .Build();
    REQUIRE(id);

    ExcelChartDefinition updated;
    updated.Id = *id;
    updated.Type = ExcelChartType::Pie;
    updated.Title = "Round-tripped";
    updated.From = *CellAddress::ParseA1("E2");
    updated.To = *CellAddress::ParseA1("K20");
    ExcelChartSeries series;
    series.Name = "South";
    series.Values = *CellRange::ParseA1("C1:C3");
    updated.Series.push_back(series);
    REQUIRE(sheet->UpdateChart(updated));

    const auto bytes = editor->SaveToMemory();
    REQUIRE_FALSE(bytes.empty());
    auto reopened = ExcelDocumentEditor::Open(bytes);
    REQUIRE(reopened);

    const auto charts = reopened->FirstWorksheet()->Charts();
    REQUIRE(charts.size() == 1);
    CHECK(charts.front().Type == ExcelChartType::Pie);
    CHECK(charts.front().Title == "Round-tripped");
    REQUIRE(charts.front().Series.size() == 1);
    CHECK(charts.front().Series.front().Name == "South");
}

TEST_CASE("Image name and description round-trip XML special characters [unit] [excel] [excel-chart]")
{
    auto editor = ExcelDocumentEditor::CreateNew();
    auto sheet = editor->FirstWorksheet();

    ExcelWorksheetImage image;
    image.Name = "A & B <C> \"D\"";
    image.Description = "Tom & Jerry's <logo>";
    image.From = *CellAddress::ParseA1("A10");
    image.To = *CellAddress::ParseA1("C15");
    image.Data = {0x89, 0x50, 0x4e, 0x47};
    REQUIRE(sheet->AddImage(image));

    const auto readBack = sheet->Images().front();
    CHECK(readBack.Name == "A & B <C> \"D\"");
    CHECK(readBack.Description == "Tom & Jerry's <logo>");
}

namespace
{

/// Occurrences of @p needle in @p text.
std::size_t CountOf(const std::string& text, const std::string& needle)
{
    std::size_t count = 0;
    for (auto at = text.find(needle); at != std::string::npos; at = text.find(needle, at + needle.size()))
    {
        ++count;
    }
    return count;
}

/// North as columns on the primary axis and South as a line on the secondary one.
ExcelChartDefinition CombinationChart()
{
    ExcelChartDefinition chart;
    chart.Type = ExcelChartType::Column;
    chart.From = *CellAddress::ParseA1("E2");
    chart.To = *CellAddress::ParseA1("K20");
    chart.SecondaryValueAxisTitle = "South";

    ExcelChartSeries north;
    north.Name = "North";
    north.Values = *CellRange::ParseA1("B1:B3");
    north.Categories = *CellRange::ParseA1("A1:A3");

    ExcelChartSeries south = north;
    south.Name = "South";
    south.Values = *CellRange::ParseA1("C1:C3");
    south.Type = ExcelChartType::Line;
    south.SecondaryAxis = true;

    chart.Series = {north, south};
    return chart;
}

} // namespace

TEST_CASE("A series of another type on a secondary axis is written as Excel writes a combination chart [unit] [excel] [excel-chart]")
{
    auto editor = ExcelDocumentEditor::CreateNew();
    auto sheet = editor->FirstWorksheet();
    FillSampleData(sheet);
    REQUIRE(sheet->AddChart(CombinationChart()));

    // The expectations are transcribed from the chart Excel 365 writes when a
    // column chart's second series is switched to a line on the secondary axis:
    // one group per type, every group before every axis, a second axis pair
    // whose value axis crosses at the maximum and whose category axis is deleted.
    const auto xml = sheet->GetPart()->GetDrawingsPart()->GetChartParts().front()->GetXmlString();
    const auto bars = xml.find("<c:barChart");
    const auto line = xml.find("<c:lineChart");
    const auto firstAxis = std::min(xml.find("<c:catAx"), xml.find("<c:valAx"));
    REQUIRE(bars != std::string::npos);
    REQUIRE(line != std::string::npos);
    CHECK(bars < line);
    CHECK(line < firstAxis);
    CHECK(CountOf(xml, "<c:catAx") == 2);
    CHECK(CountOf(xml, "<c:valAx") == 2);
    CHECK(CountOf(xml, "<c:crosses val=\"max\"") == 1);
    CHECK(CountOf(xml, "<c:delete val=\"1\"") == 1);
    CHECK(xml.find("<c:axPos val=\"r\"") != std::string::npos);
    // A series keeps its position across the whole chart, not within its group.
    CHECK(xml.find("<c:order val=\"1\"") > line);

    const auto charts = sheet->Charts();
    REQUIRE(charts.size() == 1);
    const auto& read = charts.front();
    CHECK(read.Type == ExcelChartType::Column);
    REQUIRE(read.Series.size() == 2);
    CHECK(read.Series[0].Name == "North");
    CHECK_FALSE(read.Series[0].Type.has_value());
    CHECK_FALSE(read.Series[0].SecondaryAxis);
    CHECK(read.Series[1].Name == "South");
    REQUIRE(read.Series[1].Type.has_value());
    CHECK(*read.Series[1].Type == ExcelChartType::Line);
    CHECK(read.Series[1].SecondaryAxis);
    CHECK(read.Series[1].Values.ToA1() == "C1:C3");
}

TEST_CASE("A combination chart keeps its series types and axes through a package and an update [unit] [excel] [excel-chart]")
{
    auto editor = ExcelDocumentEditor::CreateNew();
    auto sheet = editor->FirstWorksheet();
    FillSampleData(sheet);
    const auto id = sheet->AddChart(CombinationChart());
    REQUIRE(id);

    auto reopened = ExcelDocumentEditor::Open(editor->SaveToMemory());
    REQUIRE(reopened);
    auto charts = reopened->FirstWorksheet()->Charts();
    REQUIRE(charts.size() == 1);
    REQUIRE(charts.front().Series.size() == 2);
    CHECK(charts.front().Series[1].SecondaryAxis);

    // Updating from what was read back must not collapse the chart to one group.
    auto definition = charts.front();
    definition.Title = "Updated";
    REQUIRE(reopened->FirstWorksheet()->UpdateChart(definition));
    const auto updated = reopened->FirstWorksheet()->Charts();
    REQUIRE(updated.size() == 1);
    CHECK(updated.front().Title == "Updated");
    REQUIRE(updated.front().Series.size() == 2);
    REQUIRE(updated.front().Series[1].Type.has_value());
    CHECK(*updated.front().Series[1].Type == ExcelChartType::Line);
    CHECK(updated.front().Series[1].SecondaryAxis);
}

TEST_CASE("Series types that cannot share axes are refused and leave nothing behind [unit] [excel] [excel-chart]")
{
    auto editor = ExcelDocumentEditor::CreateNew();
    auto sheet = editor->FirstWorksheet();
    FillSampleData(sheet);

    const auto attempt = [&](ExcelChartType chartType, std::optional<ExcelChartType> second, bool firstSecondary,
                             bool secondSecondary)
    {
        auto chart = CombinationChart();
        chart.Type = chartType;
        chart.Series[0].SecondaryAxis = firstSecondary;
        chart.Series[1].Type = second;
        chart.Series[1].SecondaryAxis = secondSecondary;
        return sheet->AddChart(chart).has_value();
    };

    // A pie has no axes to share, and combines with nothing.
    CHECK_FALSE(attempt(ExcelChartType::Pie, ExcelChartType::Column, false, false));
    CHECK_FALSE(attempt(ExcelChartType::Column, ExcelChartType::Pie, false, false));
    // A horizontal bar swaps the axes of every other category kind.
    CHECK_FALSE(attempt(ExcelChartType::Bar, ExcelChartType::Line, false, false));
    // Scatter plots two value axes; bubble does not combine even with scatter.
    CHECK_FALSE(attempt(ExcelChartType::Column, ExcelChartType::XyScatter, false, false));
    CHECK_FALSE(attempt(ExcelChartType::XyScatter, ExcelChartType::Bubble, false, false));
    // Something has to stay on the primary axis.
    CHECK_FALSE(attempt(ExcelChartType::Column, std::nullopt, true, true));
    CHECK(sheet->Charts().empty());
    CHECK(sheet->GetPart()->GetDrawingsPart() == nullptr);

    // Kinds that share axes do combine, on either axis.
    CHECK(attempt(ExcelChartType::Area, ExcelChartType::Column, false, false));
    CHECK(attempt(ExcelChartType::XyScatter, ExcelChartType::XyScatter, false, true));
    CHECK(sheet->Charts().size() == 2);
}
