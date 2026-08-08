// Copyright (c) 2026 Jakub Melka and Collaborators
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

// End-to-end tour of the high-level ExcelDocumentEditor API. It builds a small
// two-sheet workbook: a styled data sheet with formulas, number formats, a
// worksheet table, a slicer, data validation and conditional formatting, plus a
// summary sheet with cross-sheet formulas, a named range, and a chart. The
// formula engine then computes the cached results, and the saved package is
// re-opened to prove the workbook round-trips.
#include "ExyokiOffice/Excel/ExcelDocument.hpp"
#include "ExyokiOffice/Excel/ExcelFormulaEngine.hpp"
#include "ExyokiOffice/Excel/ExcelNamedRange.hpp"
#include "ExyokiOffice/StandardTypes.hpp"

#include <cstdint>
#include <iostream>
#include <optional>
#include <string>
#include <utility>

using namespace ExyokiOffice::Excel;

namespace ExampleExcel
{

CellAddress Address(const std::string& a1)
{
    auto address = CellAddress::ParseA1(a1);
    return address ? *address : CellAddress{};
}

CellRange Range(const std::string& a1)
{
    auto range = CellRange::ParseA1(a1);
    return range ? *range : CellRange{};
}

std::string Row(ExyokiOffice::UInt32 row)
{
    return std::to_string(row);
}

} // namespace ExampleExcel

int main()
{
    using namespace ExampleExcel;

    auto editor = ExcelDocumentEditor::CreateNew();
    if (!editor)
    {
        std::cerr << "Failed to create the workbook" << std::endl;
        return 1;
    }

    auto properties = editor->Properties();
    properties.SetTitle("ExyokiOffice Sales Report");
    properties.SetCreator("ExyokiOffice Library");
    properties.SetSubject("High-level Excel API demonstration");
    properties.SetCompany("ExyokiOffice");

    editor->RenameWorksheet(0, "Sales Report");
    auto sheet = editor->FirstWorksheet();
    auto summary = editor->AddWorksheet("Summary");
    if (!sheet || !summary)
    {
        std::cerr << "Failed to create the worksheets" << std::endl;
        return 1;
    }

    auto styles = editor->Styles();

    // --- A merged, centered report title above the data. ---------------------
    sheet->SetCellText(1, 1, "Quarterly sales by product");
    sheet->MergeRange(Range("A1:E1"));

    ExcelStyle titleStyle;
    titleStyle.Font = ExcelFont{.Size = 16.0, .Bold = true};
    titleStyle.Alignment = ExcelAlignment{.Horizontal = ExcelHorizontalAlignment::Center};
    styles.ApplyToRange(*sheet, Range("A1:E1"), styles.GetOrAdd(titleStyle).StyleIndex);

    // --- Header row. ---------------------------------------------------------
    constexpr ExyokiOffice::UInt32 headerRow = 3;
    const char* headers[] = {"Product", "Category", "Quantity", "Unit Price", "Total"};
    for (ExyokiOffice::UInt32 column = 0; column < 5; ++column)
    {
        sheet->SetCellText(headerRow, column + 1, headers[column]);
    }

    // Bold white text on a solid blue fill for the header row.
    ExcelStyle headerStyle;
    headerStyle.Font = ExcelFont{.Color = ExcelColor::Rgb("FFFFFFFF"), .Bold = true};
    headerStyle.Fill = ExcelFill{.Kind = ExcelFillKind::Pattern,
                                 .Pattern = ExcelFillPattern::Solid,
                                 .Foreground = ExcelColor::Rgb("FF4472C4")};
    headerStyle.Alignment = ExcelAlignment{.Horizontal = ExcelHorizontalAlignment::Center};
    styles.ApplyToRange(*sheet, Range("A3:E3"), styles.GetOrAdd(headerStyle).StyleIndex);

    // --- Data rows: quantity and unit price are entered, total is a formula. -
    struct Item
    {
        const char* product;
        const char* category;
        ExyokiOffice::Real quantity;
        ExyokiOffice::Real unitPrice;
    };
    const Item items[] = {
        {"Widget", "Hardware", 12, 9.99},
        {"Gadget", "Hardware", 5, 24.50},
        {"Gizmo", "Hardware", 8, 14.75},
        {"License", "Software", 40, 49.00},
        {"Support plan", "Services", 3, 320.00},
    };

    ExyokiOffice::UInt32 row = headerRow + 1;
    for (const auto& item : items)
    {
        sheet->SetCellText(row, 1, item.product);
        sheet->SetCellText(row, 2, item.category);
        sheet->SetCellNumber(row, 3, item.quantity);
        sheet->SetCellNumber(row, 4, item.unitPrice);
        sheet->SetCellFormula(Address("E" + Row(row)), "C" + Row(row) + "*D" + Row(row));
        ++row;
    }
    const ExyokiOffice::UInt32 lastRow = row - 1;

    // --- Number formats. -----------------------------------------------------
    // Accounting() validates the currency symbol and decimal count, so it
    // returns an optional rather than a silently broken format code.
    const auto accounting = ExcelNumberFormat::Accounting("$", 2);
    if (!accounting)
    {
        std::cerr << "Failed to build the accounting number format" << std::endl;
        return 1;
    }
    ExcelStyle amountStyle;
    amountStyle.NumberFormat = *accounting;
    const auto amountStyleIndex = styles.GetOrAdd(amountStyle).StyleIndex;
    styles.ApplyToRange(*sheet, Range("D4:E" + Row(lastRow)), amountStyleIndex);

    ExcelStyle quantityStyle;
    quantityStyle.NumberFormat = ExcelNumberFormat::Integer();
    styles.ApplyToRange(*sheet, Range("C4:C" + Row(lastRow)), styles.GetOrAdd(quantityStyle).StyleIndex);

    // --- Column widths, a frozen header, and printing. -----------------------
    sheet->SetColumnDimension(1, ColumnDimension{.Width = 18});
    sheet->SetColumnDimension(2, ColumnDimension{.Width = 14});
    sheet->SetColumnDimension(3, ColumnDimension{.Width = 12});
    sheet->SetColumnDimension(4, ColumnDimension{.Width = 14});
    sheet->SetColumnDimension(5, ColumnDimension{.Width = 14});
    sheet->SetView(WorksheetView{.FrozenRows = headerRow});

    sheet->SetPageSetup(PageSetup{.Orientation = PageOrientation::Landscape,
                                  .PaperSize = PaperSize::A4,
                                  .FitToWidth = 1});
    sheet->SetPrintOptions(PrintOptions{.HorizontalCentered = true, .GridLines = true});
    sheet->SetPrintArea({Range("A1:E" + Row(lastRow))});
    sheet->SetPrintTitles(PrintTitles{.Rows = std::pair<ExyokiOffice::UInt32, ExyokiOffice::UInt32>{headerRow, headerRow}});
    sheet->SetHeaderFooter(HeaderFooter{.OddHeader = "&LQuarterly sales&RGenerated by ExyokiOffice",
                                        .OddFooter = "&CPage &P of &N"});

    // --- Restrict Quantity entries to positive whole numbers. ----------------
    ExcelDataValidationDefinition quantityRule;
    quantityRule.Type = DataValidationType::Whole;
    quantityRule.Operation = DataValidationOperator::GreaterThan;
    quantityRule.Formula1 = "0";
    quantityRule.Ranges = {Range("C4:C" + Row(lastRow))};
    quantityRule.ShowErrorMessage = true;
    quantityRule.ErrorTitle = "Invalid quantity";
    quantityRule.Error = "Enter a whole number greater than zero.";
    sheet->CreateDataValidation(quantityRule);

    // --- Flag totals above 100 for quick visual scanning. --------------------
    sheet->CreateConditionalFormatting(ExcelConditionalFormattingDefinition::CellIs(
        {Range("E4:E" + Row(lastRow))}, ConditionalFormattingOperator::GreaterThan, "100"));

    // --- A hyperlink and a cell comment. -------------------------------------
    sheet->SetHyperlink(ExcelHyperlink{.Address = Address("G1"),
                                       .Target = "https://example.com/sales-report",
                                       .Display = "Report details"});
    sheet->SetComment(ExcelComment{.Address = Address("E3"),
                                   .Author = "Reviewer",
                                   .Text = "Totals are formulas; refresh after editing quantities."});

    // --- A named range, visible to formulas and to Excel's name manager. -----
    NamedRangeManager names(editor->GetDocument());
    const auto totalsRange = SheetCellRange("Sales Report", Range("E4:E" + Row(lastRow)));
    if (!names.Create("SalesTotals", totalsRange))
    {
        std::cerr << "Failed to define the SalesTotals name" << std::endl;
        return 1;
    }

    // --- The summary sheet: cross-sheet formulas over the named range. -------
    summary->SetCellText(1, 1, "Metric");
    summary->SetCellText(1, 2, "Value");
    styles.ApplyToRange(*summary, Range("A1:B1"), styles.GetOrAdd(headerStyle).StyleIndex);

    summary->SetCellText(2, 1, "Line items");
    summary->SetCellFormula(Address("B2"), "COUNT(SalesTotals)");
    summary->SetCellText(3, 1, "Revenue");
    summary->SetCellFormula(Address("B3"), "SUM(SalesTotals)");
    summary->SetCellText(4, 1, "Largest order");
    summary->SetCellFormula(Address("B4"), "MAX(SalesTotals)");
    summary->SetCellText(5, 1, "Average order");
    summary->SetCellFormula(Address("B5"), "AVERAGE(SalesTotals)");
    styles.ApplyToRange(*summary, Range("B3:B5"), amountStyleIndex);
    summary->SetColumnDimension(1, ColumnDimension{.Width = 18});
    summary->SetColumnDimension(2, ColumnDimension{.Width = 16});

    // A column chart of the per-product totals, anchored beside the metrics.
    // Chart ranges carry no sheet name, so they resolve against the worksheet
    // the chart is anchored into. This chart sits on Summary but plots the data
    // sheet, which is exactly what SetSourceSheet is for; without it the series
    // would silently read Summary's own empty cells. Cross-sheet series are
    // written as references without cached values, so Excel fills the numbers in
    // when the workbook opens - anchor a chart on its own data sheet when the
    // cached values have to be readable without a recalculation.
    const auto chartId = ChartBuilder(summary)
                             .SetType(ExcelChartType::Column)
                             .SetTitle("Revenue by product")
                             .SetCategoryAxisTitle("Product")
                             .SetValueAxisTitle("Total")
                             .SetAnchor(Address("D2"), Address("K20"))
                             .SetSourceSheet("Sales Report")
                             .AddSeries("Total", Range("E4:E" + Row(lastRow)))
                             .SetXAxisLabels(Range("A4:A" + Row(lastRow)))
                             .Build();
    if (!chartId)
    {
        std::cerr << "Failed to create the revenue chart" << std::endl;
        return 1;
    }

    // --- Compute the cached formula results. ---------------------------------
    // Excel recalculates on open, but caching the values here means every
    // reader - including this example, and tools that never evaluate formulas -
    // sees the numbers immediately.
    FormulaEngine engine(editor->GetDocument());
    const auto recalculation = engine.Recalculate();
    if (!recalculation)
    {
        std::cerr << "Recalculation failed: " << recalculation.Status.Message << std::endl;
        return 1;
    }

    // --- A worksheet table over the header and data rows adds an auto-filter
    //     and lets Excel show the range as a named, styled table. Added late
    //     because `tableParts` must follow the other sheet features in
    //     SpreadsheetML's required child order. -------------------------------
    // A zero column ID asks for automatic allocation; the names must match the
    // text already sitting in the header row.
    const auto table =
        sheet->CreateTable("SalesTable", Range("A3:E" + Row(lastRow)),
                           {{0, "Product"}, {0, "Category"}, {0, "Quantity"}, {0, "Unit Price"}, {0, "Total"}});
    if (!table)
    {
        std::cerr << "Failed to create the sales table" << std::endl;
        return 1;
    }

    // A slicer over the table's Category column: a button panel the reader can
    // click to filter the table. The visible shape needs Excel 2010 or newer.
    auto slicer = SlicerBuilder(sheet)
                      .SetName("CategorySlicer")
                      .SetTableSource("SalesTable", "Category")
                      .SetAnchor(Address("G3"), Address("I12"))
                      .SetCaption("Category")
                      .Build();
    if (!slicer)
    {
        std::cerr << "Failed to create the category slicer" << std::endl;
        return 1;
    }

    if (!editor->SaveToFile("SalesReport.xlsx"))
    {
        std::cerr << "Failed to save SalesReport.xlsx" << std::endl;
        return 1;
    }

    // --- Re-open what was just written, so the example also proves the package
    //     round-trips through the library. -------------------------------------
    auto reopened = ExcelDocumentEditor::Open("SalesReport.xlsx");
    if (!reopened)
    {
        std::cerr << "Failed to reopen SalesReport.xlsx" << std::endl;
        return 1;
    }

    auto reopenedSheet = reopened->GetWorksheet("Sales Report");
    auto reopenedSummary = reopened->GetWorksheet("Summary");
    auto reopenedSlicer = reopenedSheet ? reopenedSheet->SlicerByName("CategorySlicer") : nullptr;
    std::optional<ExcelCellValue> revenue;
    if (reopenedSummary)
    {
        revenue = reopenedSummary->GetCellValue(Address("B3"));
    }
    if (!reopenedSlicer || !revenue)
    {
        std::cerr << "The workbook did not survive the round trip" << std::endl;
        return 1;
    }

    std::cout << "Wrote SalesReport.xlsx: " << reopened->Worksheets().size() << " worksheets, "
              << recalculation.RecalculatedCellCount << " recalculated formula cells, cached revenue "
              << revenue->Text() << ", the '" << reopenedSlicer->SourceField() << "' slicer with "
              << reopenedSlicer->Items().size() << " items, and "
              << NamedRangeManager(reopened->GetDocument()).List().size()
              << " defined name(s) - SalesTotals plus the built-in _xlnm names the print area and "
                 "print titles are stored as."
              << std::endl;
    return 0;
}
