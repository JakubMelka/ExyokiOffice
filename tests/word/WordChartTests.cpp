// Copyright (c) 2026 Jakub Melka and Contributors
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include <doctest.h>

#include "ExyokiOffice/Excel/ExcelDocument.hpp"
#include "ExyokiOffice/Packaging/GeneratedParts.hpp"
#include "ExyokiOffice/Word/WordDocument.hpp"
#include "pugixml/pugixml.hpp"
#include "ExyokiOffice/StandardTypes.hpp"

#include <sstream>
#include <string>
#include <vector>

using namespace ExyokiOffice::Word;
using ExyokiOffice::Packaging::ChartPart;

namespace
{

/**
 * @brief Builds a minimal, schema-valid single-series `c:chartSpace` document,
 * with the chart and DrawingML-main namespaces bound to @p chartPrefix and
 * @p drawingPrefix instead of the conventional `c`/`a`, so tests can prove
 * chart reading does not assume any specific literal prefix.
 */
std::string SampleChartSpaceXml(const std::string& title, const std::string& seriesName,
                                const std::vector<ExyokiOffice::Real>& values, const std::vector<std::string>& categories,
                                const std::string& chartPrefix = "c", const std::string& drawingPrefix = "a")
{
    const std::string& c = chartPrefix;
    const std::string& a = drawingPrefix;
    std::ostringstream xml;
    xml << "<" << c << ":chartSpace xmlns:" << c << "=\"http://schemas.openxmlformats.org/drawingml/2006/chart\" "
        << "xmlns:" << a << "=\"http://schemas.openxmlformats.org/drawingml/2006/main\" "
        << "xmlns:r=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships\">"
        << "<" << c << ":chart>";
    if (!title.empty())
    {
        xml << "<" << c << ":title><" << c << ":tx><" << c << ":rich><" << a << ":bodyPr/><" << a << ":lstStyle/>"
            << "<" << a << ":p><" << a << ":r><" << a << ":t>" << title << "</" << a << ":t></" << a << ":r></" << a
            << ":p></" << c << ":rich></" << c << ":tx><" << c << ":overlay val=\"0\"/></" << c << ":title>"
            << "<" << c << ":autoTitleDeleted val=\"0\"/>";
    }
    xml << "<" << c << ":plotArea><" << c << ":layout/><" << c << ":barChart><" << c << ":barDir val=\"col\"/>"
        << "<" << c << ":grouping val=\"clustered\"/><" << c << ":varyColors val=\"0\"/>"
        << "<" << c << ":ser><" << c << ":idx val=\"0\"/><" << c << ":order val=\"0\"/>"
        << "<" << c << ":tx><" << c << ":v>" << seriesName << "</" << c << ":v></" << c << ":tx>"
        << "<" << c << ":cat><" << c << ":strRef><" << c << ":f>Sheet1!$A$1:$A$" << categories.size() << "</" << c
        << ":f><" << c << ":strCache><" << c << ":ptCount val=\"" << categories.size() << "\"/>";
    for (ExyokiOffice::Size index = 0; index < categories.size(); ++index)
    {
        xml << "<" << c << ":pt idx=\"" << index << "\"><" << c << ":v>" << categories[index] << "</" << c
            << ":v></" << c << ":pt>";
    }
    xml << "</" << c << ":strCache></" << c << ":strRef></" << c << ":cat>"
        << "<" << c << ":val><" << c << ":numRef><" << c << ":f>Sheet1!$B$1:$B$" << values.size() << "</" << c
        << ":f><" << c << ":numCache><" << c << ":formatCode>General</" << c << ":formatCode><" << c
        << ":ptCount val=\"" << values.size() << "\"/>";
    for (ExyokiOffice::Size index = 0; index < values.size(); ++index)
    {
        xml << "<" << c << ":pt idx=\"" << index << "\"><" << c << ":v>" << values[index] << "</" << c << ":v></"
            << c << ":pt>";
    }
    xml << "</" << c << ":numCache></" << c << ":numRef></" << c << ":val></" << c << ":ser>"
        << "<" << c << ":axId val=\"1\"/><" << c << ":axId val=\"2\"/></" << c << ":barChart>"
        << "<" << c << ":catAx><" << c << ":axId val=\"1\"/><" << c << ":scaling><" << c
        << ":orientation val=\"minMax\"/></" << c << ":scaling><" << c << ":delete val=\"0\"/><" << c
        << ":axPos val=\"b\"/><" << c << ":crossAx val=\"2\"/></" << c << ":catAx>"
        << "<" << c << ":valAx><" << c << ":axId val=\"2\"/><" << c << ":scaling><" << c
        << ":orientation val=\"minMax\"/></" << c << ":scaling><" << c << ":delete val=\"0\"/><" << c
        << ":axPos val=\"l\"/><" << c << ":crossAx val=\"1\"/></" << c << ":valAx>"
        << "</" << c << ":plotArea></" << c << ":chart></" << c << ":chartSpace>";
    return xml.str();
}

/**
 * @brief Attaches a chart part with @p chartSpaceXml content to @p editor's
 * main document part, and embeds a `<w:drawing>` reference to it in a new
 * paragraph - simulating a chart pasted from Excel/PowerPoint or produced by
 * another Open XML tool, since this library has no chart-creation API of its
 * own for Word documents.
 *
 * The reference itself is written with @p chartPrefix/@p drawingPrefix too,
 * so the fixture can exercise non-canonical prefixes end to end.
 */
std::shared_ptr<ChartPart> EmbedChart(const WordDocumentEditor::Ptr& editor, const std::string& chartSpaceXml,
                                      const std::string& chartPrefix = "c", const std::string& drawingPrefix = "a")
{
    auto mainPart = editor->GetDocument()->GetMainDocumentPart();
    REQUIRE(mainPart);
    auto chartPart = mainPart->AddChartPart();
    REQUIRE(chartPart);
    chartPart->SetXmlString(chartSpaceXml);

    ExyokiOffice::Pugi::xml_document document;
    REQUIRE(document.load_string(mainPart->GetXmlString().c_str()));

    auto body = document.document_element().child("w:body");
    REQUIRE(body);

    auto paragraph = body.prepend_child("w:p");
    auto run = paragraph.append_child("w:r");
    auto drawing = run.append_child("w:drawing");
    auto inlineNode = drawing.append_child("wp:inline");
    inlineNode.append_attribute("xmlns:wp")
        .set_value("http://schemas.openxmlformats.org/drawingml/2006/wordprocessingDrawing");
    auto graphic = inlineNode.append_child((drawingPrefix + ":graphic").c_str());
    graphic.append_attribute(("xmlns:" + drawingPrefix).c_str())
        .set_value("http://schemas.openxmlformats.org/drawingml/2006/main");
    auto graphicData = graphic.append_child((drawingPrefix + ":graphicData").c_str());
    graphicData.append_attribute("uri").set_value("http://schemas.openxmlformats.org/drawingml/2006/chart");
    auto chartReference = graphicData.append_child((chartPrefix + ":chart").c_str());
    chartReference.append_attribute(("xmlns:" + chartPrefix).c_str())
        .set_value("http://schemas.openxmlformats.org/drawingml/2006/chart");
    chartReference.append_attribute("xmlns:r").set_value("http://schemas.openxmlformats.org/officeDocument/2006/relationships");
    chartReference.append_attribute("r:id").set_value(chartPart->RelationshipId().c_str());

    std::ostringstream serialized;
    document.print(serialized, "", ExyokiOffice::Pugi::format_raw);
    mainPart->SetXmlString(serialized.str());
    return chartPart;
}

} // namespace

TEST_CASE("Charts finds an embedded chart and reads its title, type, and series cache [unit] [word] [word-chart]")
{
    auto editor = WordDocumentEditor::CreateNew();
    EmbedChart(editor, SampleChartSpaceXml("Sales", "North", {10.0, 20.0, 30.0}, {"Q1", "Q2", "Q3"}));

    const auto charts = editor->Charts();
    REQUIRE(charts.size() == 1);
    CHECK(charts.front().Title == "Sales");
    CHECK(charts.front().Type == WordChartType::Column);
    CHECK_FALSE(charts.front().RelationshipId.empty());
    CHECK_FALSE(charts.front().HasEmbeddedWorkbook);

    REQUIRE(charts.front().Series.size() == 1);
    const auto& series = charts.front().Series.front();
    CHECK(series.Name == "North");
    REQUIRE(series.Values.size() == 3);
    CHECK(series.Values[0] == doctest::Approx(10.0));
    CHECK(series.Values[1] == doctest::Approx(20.0));
    CHECK(series.Values[2] == doctest::Approx(30.0));
    REQUIRE(series.Categories.has_value());
    REQUIRE(series.Categories->size() == 3);
    CHECK(series.Categories->at(0) == "Q1");
    CHECK(series.Categories->at(2) == "Q3");
}

TEST_CASE("Charts resolves chart references and elements regardless of the document's namespace prefixes [unit] [word] [word-chart]")
{
    auto editor = WordDocumentEditor::CreateNew();
    // Foreign/unconventional prefixes: "x" for the chart namespace, "y" for the
    // DrawingML-main namespace, instead of this library's own canonical "c"/"a".
    EmbedChart(editor, SampleChartSpaceXml("Sales", "North", {1.0, 2.0}, {"Q1", "Q2"}, "x", "y"), "x", "y");

    const auto charts = editor->Charts();
    REQUIRE(charts.size() == 1);
    CHECK(charts.front().Title == "Sales");
    CHECK(charts.front().Type == WordChartType::Column);
    REQUIRE(charts.front().Series.size() == 1);
    CHECK(charts.front().Series.front().Name == "North");
    REQUIRE(charts.front().Series.front().Values.size() == 2);
    CHECK(charts.front().Series.front().Values[1] == doctest::Approx(2.0));
}

TEST_CASE("Charts reports every recognized plot type [unit] [word] [word-chart]")
{
    auto editor = WordDocumentEditor::CreateNew();
    EmbedChart(editor, SampleChartSpaceXml("", "S", {1.0}, {"A"}));

    REQUIRE(editor->Charts().size() == 1);
    CHECK(editor->Charts().front().Title.empty());
}

TEST_CASE("Charts reports hasEmbeddedWorkbook when the chart part carries an embedded workbook [unit] [word] [word-chart]")
{
    auto editor = WordDocumentEditor::CreateNew();
    auto chartPart = EmbedChart(editor, SampleChartSpaceXml("Sales", "North", {1.0}, {"Q1"}));

    auto excelEditor = ExyokiOffice::Excel::ExcelDocumentEditor::CreateNew();
    const auto workbookBytes = excelEditor->SaveToMemory();
    REQUIRE_FALSE(workbookBytes.empty());

    auto embedded = chartPart->AddEmbeddedPackagePart();
    REQUIRE(embedded);
    embedded->SetBinaryData(workbookBytes);

    const auto charts = editor->Charts();
    REQUIRE(charts.size() == 1);
    CHECK(charts.front().HasEmbeddedWorkbook);
}

TEST_CASE("UpdateChartData replaces title and series cache while preserving the chart's relationship id and formula [unit] [word] [word-chart]")
{
    auto editor = WordDocumentEditor::CreateNew();
    auto chartPart = EmbedChart(editor, SampleChartSpaceXml("Original", "North", {10.0, 20.0}, {"Q1", "Q2"}));

    const auto before = editor->Charts();
    REQUIRE(before.size() == 1);
    const auto relationshipId = before.front().RelationshipId;

    WordChartSeries updatedSeries;
    updatedSeries.Name = "South";
    updatedSeries.Values = {100.0, 200.0, 300.0};
    updatedSeries.Categories = std::vector<std::string>{"Jan", "Feb", "Mar"};

    CHECK(editor->UpdateChartData(relationshipId, {updatedSeries}, std::string("Updated")));

    const auto after = editor->Charts();
    REQUIRE(after.size() == 1);
    CHECK(after.front().RelationshipId == relationshipId);
    CHECK(after.front().Title == "Updated");
    REQUIRE(after.front().Series.size() == 1);
    CHECK(after.front().Series.front().Name == "South");
    REQUIRE(after.front().Series.front().Values.size() == 3);
    CHECK(after.front().Series.front().Values[2] == doctest::Approx(300.0));
    REQUIRE(after.front().Series.front().Categories.has_value());
    CHECK(after.front().Series.front().Categories->at(1) == "Feb");

    // The original series' <c:f> formula is preserved verbatim on the rebuilt series.
    const auto chartXml = chartPart->GetXmlString();
    CHECK(chartXml.find("Sheet1!$B$1:$B$2") != std::string::npos);
}

TEST_CASE("UpdateChartData can add series relative to what the chart previously had [unit] [word] [word-chart]")
{
    auto editor = WordDocumentEditor::CreateNew();
    EmbedChart(editor, SampleChartSpaceXml("Sales", "North", {1.0, 2.0}, {"Q1", "Q2"}));
    const auto relationshipId = editor->Charts().front().RelationshipId;

    WordChartSeries first;
    first.Name = "North";
    first.Values = {1.0, 2.0};
    WordChartSeries second;
    second.Name = "South";
    second.Values = {3.0, 4.0};

    CHECK(editor->UpdateChartData(relationshipId, {first, second}));

    const auto charts = editor->Charts();
    REQUIRE(charts.size() == 1);
    REQUIRE(charts.front().Series.size() == 2);
    CHECK(charts.front().Series[0].Name == "North");
    CHECK(charts.front().Series[1].Name == "South");
}

TEST_CASE("UpdateChartData can remove series relative to what the chart previously had [unit] [word] [word-chart]")
{
    auto editor = WordDocumentEditor::CreateNew();
    EmbedChart(editor, SampleChartSpaceXml("Sales", "North", {1.0}, {"Q1"}));
    const auto relationshipId = editor->Charts().front().RelationshipId;

    WordChartSeries onlySeries;
    onlySeries.Name = "Solo";
    onlySeries.Values = {42.0};

    CHECK(editor->UpdateChartData(relationshipId, {onlySeries}));

    const auto charts = editor->Charts();
    REQUIRE(charts.size() == 1);
    REQUIRE(charts.front().Series.size() == 1);
    CHECK(charts.front().Series.front().Name == "Solo");
}

TEST_CASE("UpdateChartData with an empty title removes the title [unit] [word] [word-chart]")
{
    auto editor = WordDocumentEditor::CreateNew();
    EmbedChart(editor, SampleChartSpaceXml("Has A Title", "North", {1.0}, {"Q1"}));
    const auto relationshipId = editor->Charts().front().RelationshipId;

    WordChartSeries series;
    series.Name = "North";
    series.Values = {1.0};

    CHECK(editor->UpdateChartData(relationshipId, {series}, std::string("")));

    const auto charts = editor->Charts();
    REQUIRE(charts.size() == 1);
    CHECK(charts.front().Title.empty());
}

TEST_CASE("UpdateChartData without a title argument leaves the existing title untouched [unit] [word] [word-chart]")
{
    auto editor = WordDocumentEditor::CreateNew();
    EmbedChart(editor, SampleChartSpaceXml("Keep Me", "North", {1.0}, {"Q1"}));
    const auto relationshipId = editor->Charts().front().RelationshipId;

    WordChartSeries series;
    series.Name = "North";
    series.Values = {2.0};

    CHECK(editor->UpdateChartData(relationshipId, {series}));

    CHECK(editor->Charts().front().Title == "Keep Me");
}

TEST_CASE("UpdateChartData rejects an unknown relationship id and an empty series list [unit] [word] [word-chart]")
{
    auto editor = WordDocumentEditor::CreateNew();
    EmbedChart(editor, SampleChartSpaceXml("Sales", "North", {1.0}, {"Q1"}));
    const auto relationshipId = editor->Charts().front().RelationshipId;

    WordChartSeries series;
    series.Name = "North";
    series.Values = {1.0};

    CHECK_FALSE(editor->UpdateChartData("rIdDoesNotExist", {series}));
    CHECK_FALSE(editor->UpdateChartData(relationshipId, {}));
}

TEST_CASE("GetChartEmbeddedWorkbook and SetChartEmbeddedWorkbook round-trip a real Excel package [unit] [word] [word-chart]")
{
    auto editor = WordDocumentEditor::CreateNew();
    EmbedChart(editor, SampleChartSpaceXml("Sales", "North", {1.0}, {"Q1"}));
    const auto relationshipId = editor->Charts().front().RelationshipId;

    CHECK_FALSE(editor->GetChartEmbeddedWorkbook(relationshipId).has_value());

    auto excelEditor = ExyokiOffice::Excel::ExcelDocumentEditor::CreateNew();
    excelEditor->FirstWorksheet()->SetCellNumber(1, 1, 42.0);
    const auto workbookBytes = excelEditor->SaveToMemory();
    REQUIRE_FALSE(workbookBytes.empty());

    CHECK(editor->SetChartEmbeddedWorkbook(relationshipId, workbookBytes));
    CHECK(editor->Charts().front().HasEmbeddedWorkbook);

    const auto readBack = editor->GetChartEmbeddedWorkbook(relationshipId);
    REQUIRE(readBack.has_value());

    auto reopenedWorkbook = ExyokiOffice::Excel::ExcelDocumentEditor::Open(*readBack);
    REQUIRE(reopenedWorkbook);
    const auto value = reopenedWorkbook->FirstWorksheet()->GetCellValue(1, 1);
    REQUIRE(value.has_value());
    CHECK(value->Text() == "42");
}

TEST_CASE("GetChartEmbeddedWorkbook returns nullopt for an unknown relationship id [unit] [word] [word-chart]")
{
    auto editor = WordDocumentEditor::CreateNew();
    EmbedChart(editor, SampleChartSpaceXml("Sales", "North", {1.0}, {"Q1"}));

    CHECK_FALSE(editor->GetChartEmbeddedWorkbook("rIdDoesNotExist").has_value());
    CHECK_FALSE(editor->SetChartEmbeddedWorkbook("rIdDoesNotExist", {}));
}

TEST_CASE("Charts and UpdateChartData survive a save/reopen round-trip [unit] [word] [word-chart]")
{
    auto editor = WordDocumentEditor::CreateNew();
    EmbedChart(editor, SampleChartSpaceXml("Sales", "North", {1.0, 2.0}, {"Q1", "Q2"}));
    const auto relationshipId = editor->Charts().front().RelationshipId;

    WordChartSeries series;
    series.Name = "Updated series";
    series.Values = {5.0, 6.0};
    series.Categories = std::vector<std::string>{"A", "B"};
    REQUIRE(editor->UpdateChartData(relationshipId, {series}, std::string("Round-tripped")));

    const auto bytes = editor->SaveToMemory();
    REQUIRE_FALSE(bytes.empty());
    auto reopened = WordDocumentEditor::Open(bytes);
    REQUIRE(reopened);

    const auto charts = reopened->Charts();
    REQUIRE(charts.size() == 1);
    CHECK(charts.front().Title == "Round-tripped");
    REQUIRE(charts.front().Series.size() == 1);
    CHECK(charts.front().Series.front().Name == "Updated series");
    REQUIRE(charts.front().Series.front().Values.size() == 2);
    CHECK(charts.front().Series.front().Values[0] == doctest::Approx(5.0));
    REQUIRE(charts.front().Series.front().Categories.has_value());
    CHECK(charts.front().Series.front().Categories->at(1) == "B");
}

TEST_CASE("Charts returns empty for a document with no embedded charts [unit] [word] [word-chart]")
{
    auto editor = WordDocumentEditor::CreateNew();
    editor->AddParagraph("No charts here.");
    CHECK(editor->Charts().empty());
}
