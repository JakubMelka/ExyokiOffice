// Copyright (c) 2026 Jakub Melka and Collaborators
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "doctest.h"

#include "ExyokiOffice/DOM/DocumentFormat/OpenXml/Spreadsheet.hpp"
#include "ExyokiOffice/Excel/ExcelDocument.hpp"
#include "ExyokiOffice/OpenXmlDomValidator.hpp"
#include "ExyokiOffice/StandardTypes.hpp"

#include <algorithm>
#include <cmath>
#include <string>

using namespace ExyokiOffice::Excel;

namespace ExcelPivotTableTestHelpers
{

CellAddress Address(std::string_view text)
{
    const auto value = CellAddress::ParseA1(text);
    REQUIRE(value);
    return *value;
}

CellRange Range(std::string_view text)
{
    const auto value = CellRange::ParseA1(text);
    REQUIRE(value);
    return *value;
}

/**
 * Builds a workbook with a "Data" sheet holding the sample sales table and an
 * empty "Report" sheet:
 *
 *     Region | Quarter | Amount | Units
 *     East   | Q1      |     10 |     1
 *     East   | Q2      |     20 |     2
 *     West   | Q1      |     30 |     3
 *     West   | Q2      |     40 |     4
 *     East   | Q1      |      5 |     5
 */
ExcelDocumentEditor::Ptr MakeWorkbook()
{
    auto editor = ExcelDocumentEditor::CreateNew();
    REQUIRE(editor);
    REQUIRE(editor->RenameWorksheet(0, "Data"));
    auto data = editor->FirstWorksheet();
    REQUIRE(data);

    const char* headers[] = {"Region", "Quarter", "Amount", "Units"};
    for (ExyokiOffice::UInt32 column = 0; column < 4; ++column)
    {
        REQUIRE(data->SetCellText(1, column + 1, headers[column]));
    }
    struct Row
    {
        const char* region;
        const char* quarter;
        ExyokiOffice::Real amount;
        ExyokiOffice::Real units;
    };
    const Row rows[] = {{"East", "Q1", 10, 1},
                        {"East", "Q2", 20, 2},
                        {"West", "Q1", 30, 3},
                        {"West", "Q2", 40, 4},
                        {"East", "Q1", 5, 5}};
    ExyokiOffice::UInt32 row = 2;
    for (const auto& entry : rows)
    {
        REQUIRE(data->SetCellText(row, 1, entry.region));
        REQUIRE(data->SetCellText(row, 2, entry.quarter));
        REQUIRE(data->SetCellNumber(row, 3, entry.amount));
        REQUIRE(data->SetCellNumber(row, 4, entry.units));
        ++row;
    }
    REQUIRE(editor->AddWorksheet("Report"));
    return editor;
}

ExcelPivotTableDefinition BasicDefinition()
{
    ExcelPivotTableDefinition definition;
    definition.Name = "SalesPivot";
    definition.SourceSheet = "Data";
    definition.SourceRange = Range("A1:D6");
    definition.TargetCell = Address("A1");
    definition.Fields = {{"Region", PivotAxis::Row}, {"Quarter", PivotAxis::Column}};
    definition.DataFields.push_back(ExcelPivotDataField{"Amount"});
    return definition;
}

/** Reads a report cell as text, resolving shared strings. */
std::string CellText(const ExcelDocumentEditor& editor, const Worksheet& sheet,
                     std::string_view a1)
{
    const auto value = sheet.GetCellValue(Address(a1));
    if (!value)
    {
        return {};
    }
    if (value->Kind() == CellValueKind::SharedString)
    {
        const auto index = value->SharedStringIndex();
        if (!index)
        {
            return {};
        }
        return editor.SharedStrings().Lookup(*index).value_or(std::string{});
    }
    return value->Kind() == CellValueKind::Blank ? std::string{} : value->Text();
}

/** Reads a report cell as a number; returns NaN when the cell is not numeric. */
ExyokiOffice::Real CellNumber(const Worksheet& sheet, std::string_view a1)
{
    const auto value = sheet.GetCellValue(Address(a1));
    if (!value || value->Kind() != CellValueKind::Number)
    {
        return std::nan("");
    }
    return std::stod(value->Text());
}

} // namespace ExcelPivotTableTestHelpers

using namespace ExcelPivotTableTestHelpers;

TEST_SUITE("ExcelPivotTableTests")
{

    TEST_CASE("CreatePivotTable builds cache, definition, and workbook registry "
              "[unit] [excel] [excel-pivot]")
    {
        auto editor = MakeWorkbook();
        auto report = editor->GetWorksheet("Report");
        REQUIRE(report);

        const auto created = report->CreatePivotTable(BasicDefinition());
        REQUIRE(created.Status.Succeeded());
        const auto pivot = created.PivotTable;
        REQUIRE(pivot);

        CHECK(pivot->Name() == "SalesPivot");
        CHECK(pivot->SourceSheet() == "Data");
        REQUIRE(pivot->SourceRange());
        CHECK(pivot->SourceRange()->ToA1() == "A1:D6");
        CHECK(pivot->CacheId() == 1);
        CHECK(pivot->SourceFieldNames() ==
              std::vector<std::string>{"Region", "Quarter", "Amount", "Units"});

        REQUIRE(pivot->GetPart());
        REQUIRE(pivot->GetCacheDefinitionPart());
        REQUIRE(pivot->GetCacheRecordsPart());
        CHECK(report->GetPart()->GetPivotTableParts().size() == 1);
        CHECK(editor->GetDocument()
                  ->GetWorkbookPart()
                  ->GetPivotTableCacheDefinitionParts()
                  .size() == 1);

        const auto workbookXml =
            editor->GetDocument()->GetWorkbookPart()->GetXmlString();
        CHECK(workbookXml.find("pivotCaches") != std::string::npos);
        CHECK(workbookXml.find("cacheId=\"1\"") != std::string::npos);

        const auto cacheXml = pivot->GetCacheDefinitionPart()->GetXmlString();
        CHECK(cacheXml.find("type=\"worksheet\"") != std::string::npos);
        CHECK(cacheXml.find("ref=\"A1:D6\"") != std::string::npos);
        CHECK(cacheXml.find("sheet=\"Data\"") != std::string::npos);
        CHECK(cacheXml.find("name=\"Region\"") != std::string::npos);

        const auto recordsXml = pivot->GetCacheRecordsPart()->GetXmlString();
        CHECK(recordsXml.find("count=\"5\"") != std::string::npos);
    }

    TEST_CASE("Cached report cells hold the computed aggregates [unit] [excel] "
              "[excel-pivot]")
    {
        auto editor = MakeWorkbook();
        auto report = editor->GetWorksheet("Report");
        REQUIRE(report->CreatePivotTable(BasicDefinition()).PivotTable);

        // Header rows: column field name and items, then row field and data names.
        CHECK(CellText(*editor, *report, "A1") == "Quarter");
        CHECK(CellText(*editor, *report, "B1") == "Q1");
        CHECK(CellText(*editor, *report, "C1") == "Q2");
        CHECK(CellText(*editor, *report, "A2") == "Region");
        CHECK(CellText(*editor, *report, "B2") == "Sum of Amount");
        CHECK(CellText(*editor, *report, "D2") == "Grand Total");

        // Body: East = 10 + 5 in Q1 and 20 in Q2; West = 30 and 40.
        CHECK(CellText(*editor, *report, "A3") == "East");
        CHECK(CellNumber(*report, "B3") == doctest::Approx(15.0));
        CHECK(CellNumber(*report, "C3") == doctest::Approx(20.0));
        CHECK(CellNumber(*report, "D3") == doctest::Approx(35.0));
        CHECK(CellText(*editor, *report, "A4") == "West");
        CHECK(CellNumber(*report, "B4") == doctest::Approx(30.0));
        CHECK(CellNumber(*report, "C4") == doctest::Approx(40.0));
        CHECK(CellNumber(*report, "D4") == doctest::Approx(70.0));
        CHECK(CellText(*editor, *report, "A5") == "Grand Total");
        CHECK(CellNumber(*report, "B5") == doctest::Approx(45.0));
        CHECK(CellNumber(*report, "C5") == doctest::Approx(60.0));
        CHECK(CellNumber(*report, "D5") == doctest::Approx(105.0));
    }

    TEST_CASE("Report geometry is reported through ReportRange and WrittenRange "
              "[unit] [excel] [excel-pivot]")
    {
        auto editor = MakeWorkbook();
        auto report = editor->GetWorksheet("Report");
        const auto pivot = report->CreatePivotTable(BasicDefinition()).PivotTable;
        REQUIRE(pivot);
        REQUIRE(pivot->ReportRange());
        CHECK(pivot->ReportRange()->ToA1() == "A1:D5");
        REQUIRE(pivot->WrittenRange());
        CHECK(pivot->WrittenRange()->ToA1() == "A1:D5");
    }

    TEST_CASE("Every aggregate function is evaluated [unit] [excel] "
              "[excel-pivot]")
    {
        auto editor = MakeWorkbook();
        auto report = editor->GetWorksheet("Report");

        struct Expectation
        {
            PivotAggregateFunction function;
            ExyokiOffice::Real east;
            const char* name;
        };
        // East holds the values 10, 20, and 5.
        const Expectation expectations[] = {
            {PivotAggregateFunction::Sum, 35.0, "Sum of Amount"},
            {PivotAggregateFunction::Count, 3.0, "Count of Amount"},
            {PivotAggregateFunction::CountNumbers, 3.0, "Count of Amount"},
            {PivotAggregateFunction::Average, 35.0 / 3.0, "Average of Amount"},
            {PivotAggregateFunction::Maximum, 20.0, "Max of Amount"},
            {PivotAggregateFunction::Minimum, 5.0, "Min of Amount"},
            {PivotAggregateFunction::Product, 1000.0, "Product of Amount"},
            {PivotAggregateFunction::Variance, 58.333333333, "Var of Amount"},
            {PivotAggregateFunction::VarianceP, 38.888888888, "Varp of Amount"},
            {PivotAggregateFunction::StandardDeviation, 7.637626158,
             "StdDev of Amount"},
            {PivotAggregateFunction::StandardDeviationP, 6.236095645,
             "StdDevp of Amount"}};

        for (const auto& expectation : expectations)
        {
            auto definition = BasicDefinition();
            definition.Fields = {{"Region", PivotAxis::Row}};
            definition.DataFields.clear();
            definition.DataFields.push_back(
                ExcelPivotDataField{"Amount", "", expectation.function});
            const auto pivot = report->CreatePivotTable(definition).PivotTable;
            REQUIRE(pivot);
            CHECK(pivot->DataFields().front().Name == expectation.name);
            // One header row, then the East group on row 2.
            CHECK(CellNumber(*report, "B2") ==
                  doctest::Approx(expectation.east).epsilon(1e-6));
            REQUIRE(report->RemovePivotTable(pivot));
        }
    }

    TEST_CASE("Multiple data fields are placed on the column axis [unit] [excel] "
              "[excel-pivot]")
    {
        auto editor = MakeWorkbook();
        auto report = editor->GetWorksheet("Report");
        auto definition = BasicDefinition();
        definition.Fields = {{"Region", PivotAxis::Row}};
        definition.DataFields.push_back(
            ExcelPivotDataField{"Units", "", PivotAggregateFunction::Sum});
        const auto pivot = report->CreatePivotTable(definition).PivotTable;
        REQUIRE(pivot);

        // The data field axis is marked with the reserved field index -2.
        const auto xml = pivot->GetPart()->GetXmlString();
        CHECK(xml.find("x=\"-2\"") != std::string::npos);

        // Without column fields the report has a single header row.
        CHECK(CellText(*editor, *report, "A1") == "Region");
        CHECK(CellText(*editor, *report, "B1") == "Sum of Amount");
        CHECK(CellText(*editor, *report, "C1") == "Sum of Units");
        CHECK(CellText(*editor, *report, "A2") == "East");
        CHECK(CellNumber(*report, "B2") == doctest::Approx(35.0));
        CHECK(CellNumber(*report, "C2") == doctest::Approx(8.0));
        CHECK(CellNumber(*report, "B3") == doctest::Approx(70.0));
        CHECK(CellNumber(*report, "C3") == doctest::Approx(7.0));
        CHECK(CellText(*editor, *report, "A4") == "Grand Total");
        CHECK(CellNumber(*report, "B4") == doctest::Approx(105.0));
        CHECK(CellNumber(*report, "C4") == doctest::Approx(15.0));
    }

    TEST_CASE("Nested row fields emit subtotals when requested [unit] [excel] "
              "[excel-pivot]")
    {
        auto editor = MakeWorkbook();
        auto report = editor->GetWorksheet("Report");
        auto definition = BasicDefinition();
        definition.Fields = {{"Region", PivotAxis::Row, /*showSubtotal*/ true},
                             {"Quarter", PivotAxis::Row}};
        const auto pivot = report->CreatePivotTable(definition).PivotTable;
        REQUIRE(pivot);

        CHECK(CellText(*editor, *report, "A1") == "Region");
        CHECK(CellText(*editor, *report, "B1") == "Quarter");
        CHECK(CellText(*editor, *report, "A2") == "East");
        CHECK(CellText(*editor, *report, "B2") == "Q1");
        CHECK(CellNumber(*report, "C2") == doctest::Approx(15.0));
        CHECK(CellText(*editor, *report, "B3") == "Q2");
        CHECK(CellNumber(*report, "C3") == doctest::Approx(20.0));
        CHECK(CellText(*editor, *report, "A4") == "East Total");
        CHECK(CellNumber(*report, "C4") == doctest::Approx(35.0));
        CHECK(CellText(*editor, *report, "A5") == "West");
        CHECK(CellText(*editor, *report, "A7") == "West Total");
        CHECK(CellNumber(*report, "C7") == doctest::Approx(70.0));
        CHECK(CellText(*editor, *report, "A8") == "Grand Total");
        CHECK(CellNumber(*report, "C8") == doctest::Approx(105.0));
    }

    TEST_CASE("Row field order follows the placement order [unit] [excel] "
              "[excel-pivot]")
    {
        auto editor = MakeWorkbook();
        auto report = editor->GetWorksheet("Report");
        auto definition = BasicDefinition();
        // Quarter is source column 2 but is placed as the outer row field.
        definition.Fields = {{"Quarter", PivotAxis::Row}, {"Region", PivotAxis::Row}};
        const auto pivot = report->CreatePivotTable(definition).PivotTable;
        REQUIRE(pivot);
        CHECK(CellText(*editor, *report, "A1") == "Quarter");
        CHECK(CellText(*editor, *report, "B1") == "Region");
        CHECK(CellText(*editor, *report, "A2") == "Q1");
        CHECK(CellText(*editor, *report, "B2") == "East");

        const auto roundTripped = pivot->Definition();
        REQUIRE(roundTripped);
        REQUIRE(roundTripped->Fields.size() == 2);
        CHECK(roundTripped->Fields[0].Name == "Quarter");
        CHECK(roundTripped->Fields[1].Name == "Region");
    }

    TEST_CASE("Page fields filter the report and reserve header lines [unit] "
              "[excel] [excel-pivot]")
    {
        auto editor = MakeWorkbook();
        auto report = editor->GetWorksheet("Report");
        auto definition = BasicDefinition();
        definition.Fields = {{"Region", PivotAxis::Row}};
        // Quarter items sort ascending, so index 0 is Q1.
        ExcelPivotField page;
        page.Name = "Quarter";
        page.Axis = PivotAxis::Page;
        page.SelectedItem = 0;
        definition.Fields.push_back(page);

        const auto pivot = report->CreatePivotTable(definition).PivotTable;
        REQUIRE(pivot);
        CHECK(CellText(*editor, *report, "A1") == "Quarter");
        CHECK(CellText(*editor, *report, "B1") == "Q1");
        // Row 2 is the blank separator, the report starts on row 3.
        CHECK(CellText(*editor, *report, "A3") == "Region");
        CHECK(CellText(*editor, *report, "A4") == "East");
        CHECK(CellNumber(*report, "B4") == doctest::Approx(15.0));
        CHECK(CellNumber(*report, "B5") == doctest::Approx(30.0));
        CHECK(CellNumber(*report, "B6") == doctest::Approx(45.0));

        REQUIRE(pivot->ReportRange());
        CHECK(pivot->ReportRange()->First().ToA1() == "A3");
        REQUIRE(pivot->WrittenRange());
        CHECK(pivot->WrittenRange()->ToA1() == "A1:B6");

        const auto fields = pivot->Fields();
        const auto quarter =
            std::find_if(fields.begin(), fields.end(),
                         [](const ExcelPivotField& f)
                         { return f.Name == "Quarter"; });
        REQUIRE(quarter != fields.end());
        CHECK(quarter->Axis == PivotAxis::Page);
        REQUIRE(quarter->SelectedItem);
        CHECK(*quarter->SelectedItem == 0);
    }

    TEST_CASE("Pivot items are enumerated in ascending report order [unit] "
              "[excel] [excel-pivot]")
    {
        auto editor = MakeWorkbook();
        auto report = editor->GetWorksheet("Report");
        const auto pivot = report->CreatePivotTable(BasicDefinition()).PivotTable;
        REQUIRE(pivot);
        const auto regions = pivot->FieldItems("Region");
        REQUIRE(regions.size() == 2);
        CHECK(regions[0].Caption == "East");
        CHECK(regions[1].Caption == "West");
        // Unplaced fields are not enumerated.
        CHECK(pivot->FieldItems("Units").empty());
        CHECK(pivot->FieldItems("Missing").empty());
    }

    TEST_CASE("AggregatedValue resolves data, subtotal, and grand total groups "
              "[unit] [excel] [excel-pivot]")
    {
        auto editor = MakeWorkbook();
        auto report = editor->GetWorksheet("Report");
        const auto pivot = report->CreatePivotTable(BasicDefinition()).PivotTable;
        REQUIRE(pivot);

        CHECK(pivot->AggregatedValue({"East"}, {"Q1"}).value_or(0) ==
              doctest::Approx(15.0));
        CHECK(pivot->AggregatedValue({"West"}, {"Q2"}).value_or(0) ==
              doctest::Approx(40.0));
        CHECK(pivot->AggregatedValue({"East"}, {}).value_or(0) ==
              doctest::Approx(35.0));
        CHECK(pivot->AggregatedValue({}, {}).value_or(0) == doctest::Approx(105.0));
        CHECK_FALSE(pivot->AggregatedValue({"North"}, {}).has_value());
        CHECK_FALSE(pivot->AggregatedValue({"East"}, {}, "Unknown").has_value());
    }

    TEST_CASE("Refresh picks up edited source data [unit] [excel] "
              "[excel-pivot]")
    {
        auto editor = MakeWorkbook();
        auto report = editor->GetWorksheet("Report");
        auto data = editor->GetWorksheet("Data");
        const auto pivot = report->CreatePivotTable(BasicDefinition()).PivotTable;
        REQUIRE(pivot);
        CHECK(CellNumber(*report, "D5") == doctest::Approx(105.0));

        REQUIRE(data->SetCellNumber(2, 3, 110.0)); // East/Q1: 10 -> 110
        REQUIRE(pivot->Refresh().Succeeded());
        CHECK(CellNumber(*report, "B3") == doctest::Approx(115.0));
        CHECK(CellNumber(*report, "D5") == doctest::Approx(205.0));
    }

    TEST_CASE("Refresh adds items that appeared in the source [unit] [excel] "
              "[excel-pivot]")
    {
        auto editor = MakeWorkbook();
        auto report = editor->GetWorksheet("Report");
        auto data = editor->GetWorksheet("Data");
        const auto pivot = report->CreatePivotTable(BasicDefinition()).PivotTable;
        REQUIRE(pivot);
        CHECK(pivot->FieldItems("Region").size() == 2);

        REQUIRE(data->SetCellText(7, 1, "North"));
        REQUIRE(data->SetCellText(7, 2, "Q1"));
        REQUIRE(data->SetCellNumber(7, 3, 7.0));
        REQUIRE(pivot->SetSourceRange(Range("A1:D7")).Succeeded());

        const auto regions = pivot->FieldItems("Region");
        REQUIRE(regions.size() == 3);
        CHECK(regions[0].Caption == "East");
        CHECK(regions[1].Caption == "North");
        CHECK(regions[2].Caption == "West");
        CHECK(CellText(*editor, *report, "A4") == "North");
        CHECK(CellNumber(*report, "B4") == doctest::Approx(7.0));
    }

    TEST_CASE("SetFieldAxis, SetGrandTotals, and MoveTo rebuild the report "
              "[unit] [excel] [excel-pivot]")
    {
        auto editor = MakeWorkbook();
        auto report = editor->GetWorksheet("Report");
        auto definition = BasicDefinition();
        definition.Fields = {{"Region", PivotAxis::Row}};
        const auto pivot = report->CreatePivotTable(definition).PivotTable;
        REQUIRE(pivot);

        REQUIRE(pivot->SetFieldAxis("Quarter", PivotAxis::Column).Succeeded());
        CHECK(CellText(*editor, *report, "B1") == "Q1");
        CHECK(CellNumber(*report, "D5") == doctest::Approx(105.0));

        REQUIRE(pivot->SetGrandTotals(false, false).Succeeded());
        CHECK_FALSE(pivot->RowGrandTotals());
        CHECK_FALSE(pivot->ColumnGrandTotals());
        REQUIRE(pivot->ReportRange());
        CHECK(pivot->ReportRange()->ToA1() == "A1:C4");
        // The former grand total cells were cleared.
        CHECK(CellText(*editor, *report, "D2").empty());
        CHECK_FALSE(report->ContainsCell(Address("A5")));

        REQUIRE(pivot->MoveTo(Address("F10")).Succeeded());
        REQUIRE(pivot->ReportRange());
        CHECK(pivot->ReportRange()->First().ToA1() == "F10");
        CHECK(CellText(*editor, *report, "F11") == "Region");
        CHECK_FALSE(report->ContainsCell(Address("A1")));

        CHECK(pivot->SetFieldAxis("NotAColumn", PivotAxis::Row).Error ==
              PivotTableError::UnknownField);
    }

    TEST_CASE("Renaming enforces workbook-wide uniqueness [unit] [excel] "
              "[excel-pivot]")
    {
        auto editor = MakeWorkbook();
        auto report = editor->GetWorksheet("Report");
        auto first = BasicDefinition();
        const auto pivot = report->CreatePivotTable(first).PivotTable;
        REQUIRE(pivot);

        auto second = BasicDefinition();
        second.Name = "Other";
        second.TargetCell = Address("A10");
        const auto otherPivot = report->CreatePivotTable(second).PivotTable;
        REQUIRE(otherPivot);

        CHECK(otherPivot->SetName("SalesPivot").Error == PivotTableError::InvalidName);
        CHECK(otherPivot->SetName("").Error == PivotTableError::InvalidName);
        CHECK(otherPivot->SetName("Renamed").Succeeded());
        CHECK(otherPivot->Name() == "Renamed");
        CHECK(report->PivotTableByName("renamed") != nullptr);
        CHECK(report->PivotTables().size() == 2);
        CHECK(otherPivot->CacheId() != pivot->CacheId());
    }

    TEST_CASE("Invalid definitions are rejected without changing the workbook "
              "[unit] [excel] [excel-pivot]")
    {
        auto editor = MakeWorkbook();
        auto report = editor->GetWorksheet("Report");
        auto data = editor->GetWorksheet("Data");

        const auto expectFailure = [&](ExcelPivotTableDefinition definition,
                                       PivotTableError expected)
        {
            const auto created = report->CreatePivotTable(definition);
            CHECK(created.PivotTable == nullptr);
            CHECK(created.Status.Error == expected);
            CHECK_FALSE(created.Status.Message.empty());
            CHECK(report->GetPart()->GetPivotTableParts().empty());
            CHECK(editor->GetDocument()
                      ->GetWorkbookPart()
                      ->GetPivotTableCacheDefinitionParts()
                      .empty());
        };

        SUBCASE("unknown row field")
        {
            auto definition = BasicDefinition();
            definition.Fields = {{"Nope", PivotAxis::Row}};
            expectFailure(definition, PivotTableError::UnknownField);
        }
        SUBCASE("unknown data field")
        {
            auto definition = BasicDefinition();
            definition.DataFields = {ExcelPivotDataField{"Nope"}};
            expectFailure(definition, PivotTableError::UnknownField);
        }
        SUBCASE("no data field")
        {
            auto definition = BasicDefinition();
            definition.DataFields.clear();
            expectFailure(definition, PivotTableError::InvalidFieldConfiguration);
        }
        SUBCASE("field placed twice")
        {
            auto definition = BasicDefinition();
            definition.Fields = {{"Region", PivotAxis::Row},
                                 {"Region", PivotAxis::Column}};
            expectFailure(definition, PivotTableError::InvalidFieldConfiguration);
        }
        SUBCASE("duplicate data field name")
        {
            auto definition = BasicDefinition();
            definition.DataFields.push_back(ExcelPivotDataField{"Amount"});
            expectFailure(definition, PivotTableError::InvalidFieldConfiguration);
        }
        SUBCASE("missing source sheet")
        {
            auto definition = BasicDefinition();
            definition.SourceSheet = "Nope";
            expectFailure(definition, PivotTableError::InvalidSource);
        }
        SUBCASE("source without data rows")
        {
            auto definition = BasicDefinition();
            definition.SourceRange = Range("A1:D1");
            expectFailure(definition, PivotTableError::InvalidSource);
        }
        SUBCASE("blank header cell")
        {
            auto definition = BasicDefinition();
            definition.SourceRange = Range("A1:E6");
            expectFailure(definition, PivotTableError::InvalidSourceHeader);
        }
        SUBCASE("duplicate header caption")
        {
            REQUIRE(data->SetCellText(1, 4, "Region"));
            expectFailure(BasicDefinition(), PivotTableError::InvalidSourceHeader);
        }
        SUBCASE("invalid target cell")
        {
            auto definition = BasicDefinition();
            definition.TargetCell = CellAddress{};
            expectFailure(definition, PivotTableError::InvalidTarget);
        }
        SUBCASE("report would not fit on the grid")
        {
            auto definition = BasicDefinition();
            definition.TargetCell = Address("XFC1048570");
            expectFailure(definition, PivotTableError::InvalidTarget);
        }
        SUBCASE("report overlaps its own source range")
        {
            auto definition = BasicDefinition();
            definition.SourceSheet = "Data";
            // Build the pivot on the source worksheet itself, right on the data.
            const auto created = data->CreatePivotTable(definition);
            CHECK(created.PivotTable == nullptr);
            CHECK(created.Status.Error == PivotTableError::OverlappingReport);
        }
    }

    TEST_CASE("Overlapping reports on one worksheet are rejected [unit] [excel] "
              "[excel-pivot]")
    {
        auto editor = MakeWorkbook();
        auto report = editor->GetWorksheet("Report");
        REQUIRE(report->CreatePivotTable(BasicDefinition()).PivotTable);

        auto second = BasicDefinition();
        second.Name = "Overlapping";
        second.TargetCell = Address("C4");
        const auto created = report->CreatePivotTable(second);
        CHECK(created.PivotTable == nullptr);
        CHECK(created.Status.Error == PivotTableError::OverlappingReport);
        CHECK(report->PivotTables().size() == 1);
    }

    TEST_CASE("writeCachedReport suppresses the report cells [unit] [excel] "
              "[excel-pivot]")
    {
        auto editor = MakeWorkbook();
        auto report = editor->GetWorksheet("Report");
        auto definition = BasicDefinition();
        definition.WriteCachedReport = false;
        const auto pivot = report->CreatePivotTable(definition).PivotTable;
        REQUIRE(pivot);
        CHECK(report->StoredCellCount() == 0);
        REQUIRE(pivot->ReportRange());
        CHECK(pivot->ReportRange()->ToA1() == "A1:D5");

        // The setting survives a rebuild.
        const auto roundTripped = pivot->Definition();
        REQUIRE(roundTripped);
        CHECK_FALSE(roundTripped->WriteCachedReport);
        REQUIRE(pivot->Refresh().Succeeded());
        CHECK(report->StoredCellCount() == 0);
    }

    TEST_CASE("Style flags round-trip through pivotTableStyleInfo [unit] "
              "[excel] [excel-pivot]")
    {
        auto editor = MakeWorkbook();
        auto report = editor->GetWorksheet("Report");
        const auto pivot = report->CreatePivotTable(BasicDefinition()).PivotTable;
        REQUIRE(pivot);
        CHECK(pivot->Style().Name == "PivotStyleLight16");

        ExcelPivotTableStyle style;
        style.Name = "PivotStyleMedium9";
        style.ShowRowStripes = true;
        style.ShowLastColumn = false;
        REQUIRE(pivot->SetStyle(style).Succeeded());
        const auto read = pivot->Style();
        CHECK(read.Name == "PivotStyleMedium9");
        CHECK(read.ShowRowStripes);
        CHECK_FALSE(read.ShowLastColumn);

        REQUIRE(pivot->SetStyle(ExcelPivotTableStyle{""}).Succeeded());
        CHECK(pivot->Style().Name.empty());
        CHECK(pivot->GetPart()->GetXmlString().find("pivotTableStyleInfo") ==
              std::string::npos);
    }

    TEST_CASE("The definition round-trips through a saved package [unit] "
              "[excel] [excel-pivot]")
    {
        auto editor = MakeWorkbook();
        auto report = editor->GetWorksheet("Report");
        auto definition = BasicDefinition();
        definition.GrandTotalCaption = "All";
        definition.DataFields.push_back(
            ExcelPivotDataField{"Units", "Unit count", PivotAggregateFunction::Count});
        REQUIRE(report->CreatePivotTable(definition).PivotTable);

        const auto bytes = editor->SaveToMemory();
        REQUIRE_FALSE(bytes.empty());
        auto reopened = ExcelDocumentEditor::Open(bytes);
        REQUIRE(reopened);
        auto reopenedReport = reopened->GetWorksheet("Report");
        REQUIRE(reopenedReport);
        const auto pivot = reopenedReport->PivotTableByName("SalesPivot");
        REQUIRE(pivot);

        const auto roundTripped = pivot->Definition();
        REQUIRE(roundTripped);
        CHECK(roundTripped->Name == "SalesPivot");
        CHECK(roundTripped->SourceSheet == "Data");
        CHECK(roundTripped->SourceRange.ToA1() == "A1:D6");
        CHECK(roundTripped->TargetCell.ToA1() == "A1");
        CHECK(roundTripped->GrandTotalCaption == "All");
        REQUIRE(roundTripped->Fields.size() == 2);
        CHECK(roundTripped->Fields[0].Name == "Region");
        CHECK(roundTripped->Fields[0].Axis == PivotAxis::Row);
        CHECK(roundTripped->Fields[1].Name == "Quarter");
        CHECK(roundTripped->Fields[1].Axis == PivotAxis::Column);
        REQUIRE(roundTripped->DataFields.size() == 2);
        CHECK(roundTripped->DataFields[0].SourceField == "Amount");
        CHECK(roundTripped->DataFields[0].Function == PivotAggregateFunction::Sum);
        CHECK(roundTripped->DataFields[1].Name == "Unit count");
        CHECK(roundTripped->DataFields[1].Function == PivotAggregateFunction::Count);

        // Rebuilding from the round-tripped definition reproduces the report.
        REQUIRE(pivot->Refresh().Succeeded());
        CHECK(CellNumber(*reopenedReport, "B3") == doctest::Approx(15.0));
    }

    TEST_CASE("RemovePivotTable deletes parts, registry entry, and cells [unit] "
              "[excel] [excel-pivot]")
    {
        auto editor = MakeWorkbook();
        auto report = editor->GetWorksheet("Report");
        const auto pivot = report->CreatePivotTable(BasicDefinition()).PivotTable;
        REQUIRE(pivot);
        CHECK(report->StoredCellCount() > 0);

        REQUIRE(report->RemovePivotTable(pivot));
        CHECK(report->PivotTables().empty());
        CHECK(report->GetPart()->GetPivotTableParts().empty());
        CHECK(editor->GetDocument()
                  ->GetWorkbookPart()
                  ->GetPivotTableCacheDefinitionParts()
                  .empty());
        CHECK(report->StoredCellCount() == 0);
        CHECK(editor->GetDocument()->GetWorkbookPart()->GetXmlString().find(
                  "pivotCaches") == std::string::npos);

        // A second removal and a foreign handle are rejected.
        CHECK_FALSE(report->RemovePivotTable(pivot));
        CHECK_FALSE(report->RemovePivotTable(nullptr));
    }

    TEST_CASE("PivotTableBuilder assembles an equivalent report [unit] [excel] "
              "[excel-pivot]")
    {
        auto editor = MakeWorkbook();
        auto report = editor->GetWorksheet("Report");
        auto data = editor->GetWorksheet("Data");

        const auto pivot = PivotTableBuilder(report)
                               .SetName("BuilderPivot")
                               .SetSource(*data, Range("A1:D6"))
                               .SetTarget(Address("A1"))
                               .AddRowField("Region")
                               .AddColumnField("Quarter")
                               .AddDataField("Amount")
                               .ShowGrandTotals(true, true)
                               .Build();
        REQUIRE(pivot);
        CHECK(pivot->Name() == "BuilderPivot");
        CHECK(pivot->SourceSheet() == "Data");
        CHECK(CellNumber(*report, "D5") == doctest::Approx(105.0));

        // A builder without a data field cannot produce a pivot table.
        CHECK(PivotTableBuilder(report)
                  .SetSource(*data, Range("A1:D6"))
                  .SetTarget(Address("H1"))
                  .AddRowField("Region")
                  .Build() == nullptr);
    }

    TEST_CASE("Generated names stay unique within the workbook [unit] [excel] "
              "[excel-pivot]")
    {
        auto editor = MakeWorkbook();
        auto report = editor->GetWorksheet("Report");
        auto definition = BasicDefinition();
        definition.Name.clear();
        definition.Fields = {{"Region", PivotAxis::Row}};
        const auto first = report->CreatePivotTable(definition).PivotTable;
        REQUIRE(first);
        CHECK(first->Name() == "PivotTable1");

        definition.TargetCell = Address("E1");
        const auto second = report->CreatePivotTable(definition).PivotTable;
        REQUIRE(second);
        CHECK(second->Name() == "PivotTable2");
        CHECK(second->CacheId() == 2);
    }

    TEST_CASE("Blank and non-numeric source values are handled [unit] [excel] "
              "[excel-pivot]")
    {
        auto editor = MakeWorkbook();
        auto data = editor->GetWorksheet("Data");
        auto report = editor->GetWorksheet("Report");
        // East/Q2 loses its amount, and a blank region appears.
        REQUIRE(data->RemoveCell(3, 3));
        REQUIRE(data->RemoveCell(6, 1));

        auto definition = BasicDefinition();
        definition.Fields = {{"Region", PivotAxis::Row}};
        const auto pivot = report->CreatePivotTable(definition).PivotTable;
        REQUIRE(pivot);

        const auto regions = pivot->FieldItems("Region");
        REQUIRE(regions.size() == 3);
        CHECK(regions[0].Blank);
        CHECK(regions[1].Caption == "East");
        CHECK(regions[2].Caption == "West");

        // Row 1 is the only header row; the blank item group comes first and holds
        // the 5 from the row whose region was removed.
        CHECK(CellNumber(*report, "B2") == doctest::Approx(5.0));
        // East now only contributes the 10 from Q1, because its Q2 amount is blank.
        CHECK(CellNumber(*report, "B3") == doctest::Approx(10.0));
        CHECK(CellNumber(*report, "B4") == doctest::Approx(70.0));
        CHECK(CellText(*editor, *report, "A5") == "Grand Total");
        CHECK(CellNumber(*report, "B5") == doctest::Approx(85.0));
    }

    TEST_CASE("Generated pivot parts satisfy the SpreadsheetML schema [unit] "
              "[excel] [excel-pivot]")
    {
        auto editor = MakeWorkbook();
        auto report = editor->GetWorksheet("Report");
        auto definition = BasicDefinition();
        // Exercise every emitted element: page fields, subtotals, several data
        // fields, and both grand totals.
        definition.Fields = {{"Region", PivotAxis::Row, /*showSubtotal*/ true},
                             {"Quarter", PivotAxis::Row}};
        ExcelPivotField page;
        page.Name = "Units";
        page.Axis = PivotAxis::Page;
        definition.Fields.push_back(page);
        definition.DataFields.push_back(
            ExcelPivotDataField{"Units", "", PivotAggregateFunction::Average});
        const auto pivot = report->CreatePivotTable(definition).PivotTable;
        REQUIRE(pivot);

        const ExyokiOffice::OpenXmlDomValidator validator;
        const auto describe = [](const ExyokiOffice::ValidationResult& result)
        {
            std::string message;
            for (const auto& issue : result.Issues())
            {
                message += issue.Location.Path + ": " + issue.Message + "; ";
            }
            return message;
        };

        const auto definitionResult =
            validator.Validate(*pivot->GetPart()->GetPivotTableDefinition());
        INFO("pivotTableDefinition: ", describe(definitionResult));
        CHECK(definitionResult.IsValid());

        const auto cacheResult = validator.Validate(
            *pivot->GetCacheDefinitionPart()->GetPivotCacheDefinition());
        INFO("pivotCacheDefinition: ", describe(cacheResult));
        CHECK(cacheResult.IsValid());

        const auto workbookResult = validator.Validate(
            *editor->GetDocument()->GetWorkbookPart()->GetTypedRootElement());
        INFO("workbook: ", describe(workbookResult));
        CHECK(workbookResult.IsValid());

        // `x:r` is declared by two classes - the shared-string rich text run
        // (CT_RElt) and the pivot cache record (CT_Record). Validation resolves it
        // from the parent's content model, so the records part validates as records.
        const auto recordsResult =
            validator.Validate(*pivot->GetCacheRecordsPart()->GetPivotCacheRecords());
        INFO("pivotCacheRecords: ", describe(recordsResult));
        CHECK(recordsResult.IsValid());
    }

    TEST_CASE("IsValidPivotTableName enforces Excel's naming rules [unit] "
              "[excel] [excel-pivot]")
    {
        CHECK(IsValidPivotTableName("PivotTable1"));
        CHECK(IsValidPivotTableName("Sales by region"));
        CHECK_FALSE(IsValidPivotTableName(""));
        CHECK_FALSE(IsValidPivotTableName(" leading"));
        CHECK_FALSE(IsValidPivotTableName("trailing "));
        CHECK_FALSE(IsValidPivotTableName(std::string(256, 'x')));
        CHECK_FALSE(IsValidPivotTableName(std::string("with\tcontrol")));
    }
}
