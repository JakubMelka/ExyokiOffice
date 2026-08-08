// Copyright (c) 2026 Jakub Melka and Collaborators
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "ExyokiOffice/Excel/ExcelDocument.hpp"
#include "ExyokiOffice/StandardTypes.hpp"

#include <doctest.h>

using namespace ExyokiOffice::Excel;

class PrintTestUnits final
{
public:
    static ExyokiOffice::MeasuringUnits Centimeters(ExyokiOffice::Real value)
    {
        return ExyokiOffice::MeasuringUnits(value, ExyokiOffice::MeasurementUnit::Centimeter);
    }

    static ExyokiOffice::MeasuringUnits Inches(ExyokiOffice::Real value)
    {
        return ExyokiOffice::MeasuringUnits(value, ExyokiOffice::MeasurementUnit::Inch);
    }
};

TEST_CASE("worksheet page setup and print settings round trip [unit] [excel] [print]")
{
    auto editor = ExcelDocumentEditor::CreateNew();
    auto sheet = editor->FirstWorksheet();
    PageSetup setup;
    setup.Orientation = PageOrientation::Landscape;
    setup.PaperSize = PaperSize::A4;
    setup.Scale = 85;
    setup.FitToWidth = 1;
    REQUIRE(sheet->SetPageSetup(setup));
    PageMargins margins{PrintTestUnits::Centimeters(1.27),
                        PrintTestUnits::Inches(0.6),
                        PrintTestUnits::Inches(0.7),
                        PrintTestUnits::Inches(0.8),
                        PrintTestUnits::Inches(0.2),
                        PrintTestUnits::Inches(0.25)};
    REQUIRE(sheet->SetPageMargins(margins));
    REQUIRE(sheet->SetPrintOptions({true, false, true, true}));
    REQUIRE_FALSE(sheet->SetPageMargins({PrintTestUnits::Inches(-1),
                                         PrintTestUnits::Inches(0),
                                         PrintTestUnits::Inches(0),
                                         PrintTestUnits::Inches(0),
                                         PrintTestUnits::Inches(0),
                                         PrintTestUnits::Inches(0)}));
    REQUIRE_FALSE(sheet->SetPageSetup(PageSetup{PageOrientation::Portrait, {}, 401, {}, {}}));

    auto reopened = ExcelDocumentEditor::Open(editor->SaveToMemory());
    REQUIRE(reopened);
    sheet = reopened->FirstWorksheet();
    const auto readSetup = sheet->GetPageSetup();
    CHECK(readSetup.Orientation == PageOrientation::Landscape);
    CHECK(readSetup.PaperSize == PaperSize::A4);
    CHECK(readSetup.Scale == 85);
    CHECK(readSetup.FitToWidth == 1);
    CHECK(sheet->GetPageMargins().Left.ToIN().GetValue() == doctest::Approx(0.5));
    CHECK(sheet->GetPageMargins().Bottom.ToIN().GetValue() == doctest::Approx(0.8));
    const auto options = sheet->GetPrintOptions();
    CHECK(options.HorizontalCentered);
    CHECK(options.Headings);
    CHECK(options.GridLines);
}

TEST_CASE("worksheet print area titles and header footer are workbook-safe [unit] [excel] [print]")
{
    auto editor = ExcelDocumentEditor::CreateNew();
    auto sheet = editor->FirstWorksheet();
    REQUIRE(sheet->SetPrintArea({*CellRange::ParseA1("A1:C10"), *CellRange::ParseA1("E1:E10")}));
    PrintTitles titles;
    titles.Rows = {{1, 2}};
    titles.Columns = {{1, 1}};
    REQUIRE(sheet->SetPrintTitles(titles));
    HeaderFooter headerFooter;
    headerFooter.OddHeader = "&CReport &P of &N";
    headerFooter.OddFooter = "&LConfidential";
    headerFooter.DifferentFirst = true;
    REQUIRE(sheet->SetHeaderFooter(headerFooter));
    PrintTitles invalidTitles;
    invalidTitles.Rows = {{2, 1}};
    REQUIRE_FALSE(sheet->SetPrintTitles(invalidTitles));

    auto reopened = ExcelDocumentEditor::Open(editor->SaveToMemory());
    REQUIRE(reopened);
    sheet = reopened->FirstWorksheet();
    const auto areas = sheet->GetPrintArea();
    REQUIRE(areas.size() == 2);
    CHECK(areas[0].ToA1() == "A1:C10");
    CHECK(areas[1].ToA1() == "E1:E10");
    const auto readTitles = sheet->GetPrintTitles();
    REQUIRE(readTitles.Rows);
    REQUIRE(readTitles.Columns);
    CHECK(readTitles.Rows->second == 2);
    CHECK(readTitles.Columns->first == 1);
    const auto readHeaderFooter = sheet->GetHeaderFooter();
    CHECK(readHeaderFooter.OddHeader == "&CReport &P of &N");
    CHECK(readHeaderFooter.DifferentFirst);
    REQUIRE(sheet->SetPrintArea({}));
    REQUIRE(sheet->SetPrintTitles({}));
    CHECK(sheet->GetPrintArea().empty());
    CHECK_FALSE(sheet->GetPrintTitles().Rows);
}
