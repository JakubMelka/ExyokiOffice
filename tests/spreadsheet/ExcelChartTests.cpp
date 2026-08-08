// Copyright (c) 2026 Jakub Melka and Collaborators
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include <doctest.h>

#include "ExyokiOffice/Excel/ExcelDocument.hpp"

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
