// Copyright (c) 2026 Jakub Melka and Collaborators
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "doctest.h"

#include "ExyokiOffice/DOM/DocumentFormat/OpenXml/Presentation.hpp"
#include "ExyokiOffice/Excel/ExcelAddress.hpp"
#include "ExyokiOffice/Excel/ExcelCellValue.hpp"
#include "ExyokiOffice/Excel/ExcelDocument.hpp"
#include "ExyokiOffice/PowerPoint/PowerPointDocument.hpp"
#include "ExyokiOffice/StandardTypes.hpp"

using namespace ExyokiOffice::PowerPoint;

namespace
{
PresentationChartDefinition ColumnChart()
{
    PresentationChartDefinition chart;
    chart.Type = PresentationChartType::Column;
    chart.Title = "Quarterly revenue";
    chart.CategoryAxisTitle = "Quarter";
    chart.ValueAxisTitle = "USD (millions)";
    chart.Transform = {{914400, 914400}, {5486400, 3200400}};
    PresentationChartSeries series;
    series.Name = "2026";
    series.Values = {12.0, 18.5, 9.0, 21.0};
    series.Categories = std::vector<std::string>{"Q1", "Q2", "Q3", "Q4"};
    chart.Series = {series};
    return chart;
}

PresentationShape::Ptr FirstChartShape(const PowerPointDocumentEditor::Ptr& editor)
{
    auto tree = editor->GetSlide(0)->ShapeTree();
    for (ExyokiOffice::Size i = 0; i < tree->Count(); ++i)
    {
        if (tree->Get(i)->GetChart())
        {
            return tree->Get(i);
        }
    }
    return nullptr;
}
} // namespace

TEST_SUITE("PowerPointChartTests")
{
    TEST_CASE("a column chart round-trips its type, title, and cached series [unit] [powerpoint] [chart]")
    {
        auto editor = PowerPointDocumentEditor::CreateNew();
        auto tree = editor->AddSlide()->ShapeTree();
        auto definition = ColumnChart();
        auto shape = tree->AddChart(definition);
        REQUIRE(shape);

        auto info = shape->GetChart();
        REQUIRE(info);
        CHECK(info->Type == PresentationChartType::Column);
        CHECK(info->Title == "Quarterly revenue");
        REQUIRE(info->Series.size() == 1);
        CHECK(info->Series[0] == definition.Series[0]);

        // The chart lives in the slide shape tree and its own chart part.
        CHECK(shape->GetTransform() == definition.Transform);
        const auto slideXml = editor->GetSlide(0)->GetPart()->GetXmlString();
        CHECK(slideXml.find("<p:graphicFrame>") != std::string::npos);
        CHECK(slideXml.find("drawingml/2006/chart") != std::string::npos);
        auto parts = editor->GetSlide(0)->GetPart()->GetChartParts();
        REQUIRE(parts.size() == 1);
        CHECK(parts[0]->GetXmlString().find("c:barChart") != std::string::npos);

        auto reopened = PowerPointDocumentEditor::Open(editor->SaveToMemory());
        REQUIRE(reopened);
        auto roundTripped = FirstChartShape(reopened);
        REQUIRE(roundTripped);
        auto reread = roundTripped->GetChart();
        REQUIRE(reread);
        CHECK(reread->Type == PresentationChartType::Column);
        CHECK(reread->Title == "Quarterly revenue");
        REQUIRE(reread->Series.size() == 1);
        CHECK(reread->Series[0] == definition.Series[0]);
    }

    TEST_CASE("multi-series line chart preserves every series [unit] [powerpoint] [chart]")
    {
        auto editor = PowerPointDocumentEditor::CreateNew();
        auto tree = editor->AddSlide()->ShapeTree();
        PresentationChartDefinition chart;
        chart.Type = PresentationChartType::Line;
        chart.Transform = {{0, 0}, {4000000, 3000000}};
        PresentationChartSeries a;
        a.Name = "North";
        a.Values = {1.0, 2.0, 3.0};
        a.Categories = std::vector<std::string>{"Jan", "Feb", "Mar"};
        PresentationChartSeries b;
        b.Name = "South";
        b.Values = {3.0, 2.0, 1.0};
        b.Categories = std::vector<std::string>{"Jan", "Feb", "Mar"};
        chart.Series = {a, b};

        auto shape = tree->AddChart(chart);
        REQUIRE(shape);
        auto info = shape->GetChart();
        REQUIRE(info);
        CHECK(info->Type == PresentationChartType::Line);
        REQUIRE(info->Series.size() == 2);
        CHECK(info->Series[0] == a);
        CHECK(info->Series[1] == b);
    }

    TEST_CASE("scatter chart carries X values as category text [unit] [powerpoint] [chart]")
    {
        auto editor = PowerPointDocumentEditor::CreateNew();
        auto tree = editor->AddSlide()->ShapeTree();
        PresentationChartDefinition chart;
        chart.Type = PresentationChartType::XyScatter;
        chart.Transform = {{0, 0}, {4000000, 3000000}};
        PresentationChartSeries series;
        series.Name = "Samples";
        series.Values = {10.0, 20.0, 30.0};
        series.Categories = std::vector<std::string>{"1", "2", "3"}; // X values as text
        chart.Series = {series};

        auto shape = tree->AddChart(chart);
        REQUIRE(shape);
        auto info = shape->GetChart();
        REQUIRE(info);
        CHECK(info->Type == PresentationChartType::XyScatter);
        REQUIRE(info->Series.size() == 1);
        CHECK(info->Series[0].Values == std::vector<ExyokiOffice::Real>{10.0, 20.0, 30.0});
        REQUIRE(info->Series[0].Categories);
        CHECK(*info->Series[0].Categories == std::vector<std::string>{"1", "2", "3"});
    }

    TEST_CASE("bubble chart is created with a bubble plot group [unit] [powerpoint] [chart]")
    {
        auto editor = PowerPointDocumentEditor::CreateNew();
        auto tree = editor->AddSlide()->ShapeTree();
        PresentationChartDefinition chart;
        chart.Type = PresentationChartType::Bubble;
        chart.Transform = {{0, 0}, {4000000, 3000000}};
        PresentationChartSeries series;
        series.Name = "Markets";
        series.Values = {5.0, 6.0};
        series.Categories = std::vector<std::string>{"1", "2"};
        series.BubbleSizes = std::vector<ExyokiOffice::Real>{3.0, 4.0};
        chart.Series = {series};

        auto shape = tree->AddChart(chart);
        REQUIRE(shape);
        CHECK(shape->GetChart()->Type == PresentationChartType::Bubble);
        auto parts = editor->GetSlide(0)->GetPart()->GetChartParts();
        REQUIRE(parts.size() == 1);
        const auto xml = parts[0]->GetXmlString();
        CHECK(xml.find("c:bubbleChart") != std::string::npos);
        CHECK(xml.find("c:bubbleSize") != std::string::npos);
    }

    TEST_CASE("chart authoring rejects invalid definitions [unit] [powerpoint] [chart]")
    {
        auto editor = PowerPointDocumentEditor::CreateNew();
        auto tree = editor->AddSlide()->ShapeTree();

        PresentationChartDefinition noSeries;
        noSeries.Type = PresentationChartType::Column;
        CHECK_FALSE(tree->AddChart(noSeries));

        PresentationChartDefinition unknown = ColumnChart();
        unknown.Type = PresentationChartType::Unknown;
        CHECK_FALSE(tree->AddChart(unknown));

        PresentationChartDefinition emptyValues = ColumnChart();
        emptyValues.Series[0].Values.clear();
        CHECK_FALSE(tree->AddChart(emptyValues));

        CHECK(tree->Count() == 0);
        CHECK(editor->GetSlide(0)->GetPart()->GetChartParts().empty());
    }

    TEST_CASE("update chart data replaces cached values and title [unit] [powerpoint] [chart]")
    {
        auto editor = PowerPointDocumentEditor::CreateNew();
        auto tree = editor->AddSlide()->ShapeTree();
        auto shape = tree->AddChart(ColumnChart());
        REQUIRE(shape);

        PresentationChartSeries updated;
        updated.Name = "2027";
        updated.Values = {30.0, 31.0, 32.0, 33.0};
        updated.Categories = std::vector<std::string>{"Q1", "Q2", "Q3", "Q4"};
        REQUIRE(shape->UpdateChartData({updated}, "Updated revenue"));

        auto info = shape->GetChart();
        REQUIRE(info);
        CHECK(info->Title == "Updated revenue");
        REQUIRE(info->Series.size() == 1);
        CHECK(info->Series[0].Name == "2027");
        CHECK(info->Series[0].Values == std::vector<ExyokiOffice::Real>{30.0, 31.0, 32.0, 33.0});

        // An empty title string clears the title; std::nullopt would keep it.
        REQUIRE(shape->UpdateChartData({updated}, std::string{}));
        CHECK(shape->GetChart()->Title.empty());
    }

    TEST_CASE("embedded workbook bytes round-trip and are reported [unit] [powerpoint] [chart]")
    {
        auto editor = PowerPointDocumentEditor::CreateNew();
        auto tree = editor->AddSlide()->ShapeTree();
        auto shape = tree->AddChart(ColumnChart());
        REQUIRE(shape);
        CHECK_FALSE(shape->GetChart()->HasEmbeddedWorkbook);
        CHECK_FALSE(shape->GetChartEmbeddedWorkbook());

        const std::vector<ExyokiOffice::Byte> workbook{'P', 'K', 0x03, 0x04, 0x01, 0x02};
        REQUIRE(shape->SetChartEmbeddedWorkbook(workbook));
        CHECK(shape->GetChart()->HasEmbeddedWorkbook);
        auto read = shape->GetChartEmbeddedWorkbook();
        REQUIRE(read);
        CHECK(*read == workbook);

        auto reopened = PowerPointDocumentEditor::Open(editor->SaveToMemory());
        REQUIRE(reopened);
        auto roundTripped = FirstChartShape(reopened);
        REQUIRE(roundTripped);
        auto reread = roundTripped->GetChartEmbeddedWorkbook();
        REQUIRE(reread);
        CHECK(*reread == workbook);
    }

    TEST_CASE("chart accessors ignore non-chart shapes [unit] [powerpoint] [chart]")
    {
        auto editor = PowerPointDocumentEditor::CreateNew();
        auto tree = editor->AddSlide()->ShapeTree();
        auto plain = tree->AddShape("Box");
        REQUIRE(plain);
        CHECK_FALSE(plain->GetChart());
        CHECK_FALSE(plain->GetChartEmbeddedWorkbook());
        CHECK_FALSE(plain->SetChartEmbeddedWorkbook(std::vector<ExyokiOffice::Byte>{1, 2, 3}));
        PresentationChartSeries series;
        series.Name = "x";
        series.Values = {1.0};
        CHECK_FALSE(plain->UpdateChartData({series}));
        CHECK_FALSE(plain->RefreshChartDataFromEmbeddedWorkbook());
        CHECK_FALSE(plain->GetChartStyleXml());
        CHECK_FALSE(plain->SetChartStyleXml("<x/>"));
        CHECK_FALSE(plain->GetChartColorStyleXml());
        CHECK_FALSE(plain->SetChartColorStyleXml("<x/>"));
    }

    TEST_CASE("refreshing chart data reads values and labels from the embedded workbook [unit] [powerpoint] [chart]")
    {
        namespace Excel = ExyokiOffice::Excel;

        auto editor = PowerPointDocumentEditor::CreateNew();
        auto tree = editor->AddSlide()->ShapeTree();
        auto definition = ColumnChart(); // categories in column A, values in column B, sheet "Sheet1"
        auto shape = tree->AddChart(definition);
        REQUIRE(shape);

        auto workbook = Excel::ExcelDocumentEditor::CreateNew(); // already has a default "Sheet1"
        auto sheet = workbook->GetWorksheet("Sheet1");
        REQUIRE(sheet);
        const std::vector<std::string> categories = {"Jan", "Feb", "Mar", "Apr"};
        const std::vector<ExyokiOffice::Real> values = {100.0, 200.0, 300.0, 400.0};
        for (ExyokiOffice::Size i = 0; i < categories.size(); ++i)
        {
            const auto row = std::to_string(i + 2);
            REQUIRE(sheet->SetCellValue(*Excel::CellAddress::ParseA1("A" + row),
                                        Excel::ExcelCellValue::InlineString(categories[i])));
            REQUIRE(sheet->SetCellValue(*Excel::CellAddress::ParseA1("B" + row), Excel::ExcelCellValue::Number(values[i])));
        }
        REQUIRE(shape->SetChartEmbeddedWorkbook(workbook->SaveToMemory()));

        REQUIRE(shape->RefreshChartDataFromEmbeddedWorkbook());
        auto info = shape->GetChart();
        REQUIRE(info);
        CHECK(info->Title == definition.Title);             // title untouched by refresh
        CHECK(info->Type == PresentationChartType::Column); // plot type untouched by refresh
        REQUIRE(info->Series.size() == 1);
        CHECK(info->Series[0].Values == values);
        REQUIRE(info->Series[0].Categories);
        CHECK(*info->Series[0].Categories == categories);
    }

    TEST_CASE("refreshing chart data fails safely without a matching embedded sheet [unit] [powerpoint] [chart]")
    {
        namespace Excel = ExyokiOffice::Excel;

        auto editor = PowerPointDocumentEditor::CreateNew();
        auto tree = editor->AddSlide()->ShapeTree();
        auto definition = ColumnChart();
        auto shape = tree->AddChart(definition);
        REQUIRE(shape);

        CHECK_FALSE(shape->RefreshChartDataFromEmbeddedWorkbook()); // no embedded workbook yet

        auto workbook = Excel::ExcelDocumentEditor::CreateNew();
        // The chart's cached formulas reference "Sheet1"; renaming the only sheet away
        // from that name means no sheet in this workbook can resolve them.
        REQUIRE(workbook->RenameWorksheet(0, "WrongSheetName"));
        REQUIRE(shape->SetChartEmbeddedWorkbook(workbook->SaveToMemory()));
        CHECK_FALSE(shape->RefreshChartDataFromEmbeddedWorkbook());

        // The failed refresh must not have mutated the existing cached data.
        auto info = shape->GetChart();
        REQUIRE(info);
        CHECK(info->Series[0] == definition.Series[0]);
    }

    TEST_CASE("chart style and color-style XML round-trip verbatim [unit] [powerpoint] [chart]")
    {
        auto editor = PowerPointDocumentEditor::CreateNew();
        auto tree = editor->AddSlide()->ShapeTree();
        auto shape = tree->AddChart(ColumnChart());
        REQUIRE(shape);
        CHECK_FALSE(shape->GetChartStyleXml());
        CHECK_FALSE(shape->GetChartColorStyleXml());

        const std::string styleXml =
            "<cs:chartStyle xmlns:cs=\"http://schemas.microsoft.com/office/drawing/2012/chartStyle\" id=\"381\">"
            "<cs:axisTitle/></cs:chartStyle>";
        const std::string colorXml =
            "<cs:colorStyle xmlns:cs=\"http://schemas.microsoft.com/office/drawing/2012/chartStyle\" meth=\"cycle\" "
            "id=\"10\"><a:schemeClr xmlns:a=\"http://schemas.openxmlformats.org/drawingml/2006/main\" val=\"accent1\"/>"
            "</cs:colorStyle>";
        REQUIRE(shape->SetChartStyleXml(styleXml));
        REQUIRE(shape->SetChartColorStyleXml(colorXml));

        auto readStyle = shape->GetChartStyleXml();
        REQUIRE(readStyle);
        CHECK(readStyle->find("id=\"381\"") != std::string::npos);
        auto readColor = shape->GetChartColorStyleXml();
        REQUIRE(readColor);
        CHECK(readColor->find("id=\"10\"") != std::string::npos);

        // Replacing again updates the same part instead of adding a second one.
        REQUIRE(shape->SetChartStyleXml(
            "<cs:chartStyle xmlns:cs=\"http://schemas.microsoft.com/office/drawing/2012/chartStyle\" id=\"999\"/>"));
        auto updatedStyle = shape->GetChartStyleXml();
        REQUIRE(updatedStyle);
        CHECK(updatedStyle->find("id=\"999\"") != std::string::npos);

        auto reopened = PowerPointDocumentEditor::Open(editor->SaveToMemory());
        REQUIRE(reopened);
        auto roundTripped = FirstChartShape(reopened);
        REQUIRE(roundTripped);
        auto reReadColor = roundTripped->GetChartColorStyleXml();
        REQUIRE(reReadColor);
        CHECK(reReadColor->find("id=\"10\"") != std::string::npos);
    }
}
