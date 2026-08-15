// Copyright (c) 2026 Jakub Melka and Contributors
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

// One test case per row of the Excel table in docs/Compatibility.md, graded the
// same way the matrix grades it: Create through ExcelDocumentEditor, Edit after
// reopening the saved workbook, Preserve across an open-save cycle.
//
// Two rows carry a stated restriction rather than a plain Yes, and the cases
// for them assert the restriction itself: rich-text cell content is preserved
// but not authored, and structural edits do not rewrite references into other
// workbooks.

#include "doctest.h"

#include "TestSupport.hpp"

#include "ExyokiOffice/DOM/DocumentFormat/OpenXml/Spreadsheet.hpp"
#include "ExyokiOffice/Excel/ExcelChart.hpp"
#include "ExyokiOffice/Excel/ExcelConditionalFormatting.hpp"
#include "ExyokiOffice/Excel/ExcelDataValidation.hpp"
#include "ExyokiOffice/Excel/ExcelDocument.hpp"
#include "ExyokiOffice/Excel/ExcelFormulaEngine.hpp"
#include "ExyokiOffice/Excel/ExcelNamedRange.hpp"
#include "ExyokiOffice/Excel/ExcelPivotTable.hpp"
#include "ExyokiOffice/Excel/ExcelSlicer.hpp"
#include "ExyokiOffice/Excel/ExcelStyle.hpp"
#include "ExyokiOffice/StandardTypes.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace
{

using namespace ExyokiOffice::Excel;
using ExyokiOffice::Byte;
using ExyokiOffice::UInt32;
using ExyokiOfficeTests::CheckPreservation;
using ExyokiOfficeTests::RoundTrip;
using ExyokiOfficeTests::ValidatePackage;

namespace Spreadsheet = ExyokiOffice::DocumentFormat::OpenXml::Spreadsheet;

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
 * @brief A workbook with a "Data" sheet of sample sales and an empty "Report".
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
    for (UInt32 column = 0; column < 4; ++column)
    {
        REQUIRE(data->SetCellText(1, column + 1, headers[column]));
    }

    struct Row
    {
        const char* Region;
        const char* Quarter;
        double Amount;
        double Units;
    };
    constexpr Row rows[] = {{"East", "Q1", 10.0, 1.0},
                            {"East", "Q2", 20.0, 2.0},
                            {"West", "Q1", 30.0, 3.0},
                            {"West", "Q2", 40.0, 4.0},
                            {"East", "Q1", 5.0, 5.0}};

    UInt32 row = 2;
    for (const auto& entry : rows)
    {
        REQUIRE(data->SetCellText(row, 1, entry.Region));
        REQUIRE(data->SetCellText(row, 2, entry.Quarter));
        REQUIRE(data->SetCellNumber(row, 3, entry.Amount));
        REQUIRE(data->SetCellNumber(row, 4, entry.Units));
        ++row;
    }

    REQUIRE(editor->AddWorksheet("Report"));
    return editor;
}

/// The text a cell shows, resolving shared strings through the workbook.
std::string CellText(const ExcelDocumentEditor::Ptr& editor, const Worksheet::Ptr& sheet, CellAddress address)
{
    if (!editor || !sheet)
    {
        return {};
    }
    const auto value = sheet->GetCellValue(address);
    if (!value)
    {
        return {};
    }
    if (const auto index = value->SharedStringIndex())
    {
        return editor->SharedStrings().Lookup(*index).value_or(std::string{});
    }
    return value->Text();
}

/// Saves, checks the package validates, and reports that the bytes round-trip
/// unchanged. Every row below ends with this.
void CheckSavesValidatesAndPreserves(const ExcelDocumentEditor::Ptr& editor)
{
    REQUIRE(editor != nullptr);
    const auto bytes = editor->SaveToMemory();
    REQUIRE_FALSE(bytes.empty());

    const auto validation = ValidatePackage(bytes);
    CAPTURE(validation.FirstError);
    CHECK_FALSE(validation.HasErrors);

    const auto preservation = CheckPreservation(bytes);
    REQUIRE(preservation.Ok);
    for (const auto& difference : preservation.Differences)
    {
        CAPTURE(difference);
        CHECK_MESSAGE(false, "package changed through open-save");
    }
    CHECK(preservation.Preserved);
}

} // namespace

TEST_SUITE("ExcelMatrixTests")
{

    TEST_CASE("Workbooks: create, edit, preserve [compat] [excel] [excel-workbooks]")
    {
        auto editor = MakeWorkbook();
        editor->Properties().SetTitle("Workbook matrix");
        REQUIRE(editor->EnsureTheme());

        WorkbookProtectionOptions options;
        options.LockStructure = true;
        REQUIRE(editor->ProtectWorkbook(options, "secret").Succeeded());

        auto reopened = RoundTrip(editor);
        REQUIRE(reopened != nullptr);
        CHECK(reopened->Properties().GetTitle() == "Workbook matrix");
        CHECK(reopened->ThemeXml().has_value());
        auto protection = reopened->GetWorkbookProtection();
        REQUIRE(protection.has_value());
        CHECK(protection->Options.LockStructure);

        CHECK_FALSE(reopened->UnprotectWorkbook("wrong").Succeeded());
        CHECK(reopened->UnprotectWorkbook("secret").Succeeded());
        reopened->Properties().SetTitle("Edited");

        auto edited = RoundTrip(reopened);
        REQUIRE(edited != nullptr);
        CHECK(edited->Properties().GetTitle() == "Edited");
        CHECK_FALSE(edited->GetWorkbookProtection().has_value());

        CheckSavesValidatesAndPreserves(edited);
    }

    TEST_CASE("Worksheets: create, edit, preserve [compat] [excel] [excel-worksheets]")
    {
        auto editor = MakeWorkbook();
        REQUIRE(editor->AddWorksheet("Third"));
        REQUIRE(editor->Worksheets().size() == 3);

        auto copy = editor->CopyWorksheet(0, "Data copy");
        REQUIRE(copy != nullptr);

        SheetProtectionOptions sheetOptions;
        REQUIRE(editor->GetWorksheet("Third")->Protect(sheetOptions, "sheet").Succeeded());

        auto reopened = RoundTrip(editor);
        REQUIRE(reopened != nullptr);
        CHECK(reopened->Worksheets().size() == 4);
        CHECK(reopened->GetWorksheet("Data copy") != nullptr);
        CHECK(CellText(reopened, reopened->GetWorksheet("Data copy"), Address("A1")) == "Region");
        CHECK(reopened->GetWorksheet("Third")->GetProtection().has_value());

        // Edit: rename, move and remove.
        REQUIRE(reopened->RenameWorksheet(2, "Renamed"));
        REQUIRE(reopened->MoveWorksheet(3, 0));
        REQUIRE(reopened->RemoveWorksheet(0));

        auto edited = RoundTrip(reopened);
        REQUIRE(edited != nullptr);
        CHECK(edited->Worksheets().size() == 3);
        CHECK(edited->GetWorksheet("Renamed") != nullptr);

        CheckSavesValidatesAndPreserves(edited);
    }

    TEST_CASE("Cells and ranges: create, edit, preserve [compat] [excel] [excel-cells]")
    {
        auto editor = MakeWorkbook();
        auto data = editor->GetWorksheet("Data");
        REQUIRE(data != nullptr);

        REQUIRE(data->SetCellNumber(Address("F1"), 42.0));
        REQUIRE(data->SetCellBoolean(Address("F2"), true));
        REQUIRE(data->MergeRange(Range("H1:I1")));
        REQUIRE(data->CopyRange(Range("A1:D1"), Address("A10")).Succeeded());

        auto reopened = RoundTrip(editor);
        REQUIRE(reopened != nullptr);
        auto readBack = reopened->GetWorksheet("Data");
        REQUIRE(readBack != nullptr);
        CHECK(CellText(reopened, readBack, Address("A10")) == "Region");
        CHECK(readBack->MergedRanges().size() == 1);
        REQUIRE(readBack->GetCellValue(Address("F1")).has_value());
        CHECK(readBack->GetCellValue(Address("F1"))->Text() == "42");

        // Edit: a structural insert moves the rows below it.
        REQUIRE(readBack->InsertRows(1, 1).Succeeded());

        auto edited = RoundTrip(reopened);
        REQUIRE(edited != nullptr);
        auto editedSheet = edited->GetWorksheet("Data");
        REQUIRE(editedSheet != nullptr);
        CHECK(CellText(edited, editedSheet, Address("A2")) == "Region");

        CheckSavesValidatesAndPreserves(edited);
    }

    TEST_CASE("Rich-text cell content is preserved but never authored "
              "[compat] [excel] [excel-rich-text]")
    {
        auto editor = MakeWorkbook();
        auto data = editor->GetWorksheet("Data");
        REQUIRE(data != nullptr);

        // Create and Edit are graded No: SetCellText writes a plain-text shared
        // string, never a run-formatted one.
        REQUIRE(data->SetCellText(Address("F1"), "plain"));
        const auto sharedStringsXml =
            editor->GetDocument()->GetWorkbookPart()->GetSharedStringTablePart()->GetXmlString();
        CHECK(sharedStringsXml.find("<r>") == std::string::npos);

        // Preserve is graded Yes: a run-formatted shared string, such as another
        // producer would write, survives untouched. It is built through the
        // typed DOM so the namespace is right by construction.
        auto sharedStringsPart = editor->GetDocument()->GetWorkbookPart()->GetSharedStringTablePart();
        REQUIRE(sharedStringsPart != nullptr);
        auto table = sharedStringsPart->GetSharedStringTable();
        REQUIRE(table != nullptr);

        auto item = table->AppendChild<Spreadsheet::SharedStringItem>();
        REQUIRE(item != nullptr);
        auto boldRun = item->AppendChild<Spreadsheet::Run>();
        REQUIRE(boldRun != nullptr);
        auto runProperties = boldRun->AppendChild<Spreadsheet::RunProperties>();
        REQUIRE(runProperties != nullptr);
        REQUIRE(runProperties->AppendChild<Spreadsheet::Bold>() != nullptr);
        auto boldText = boldRun->AppendChild<Spreadsheet::Text>();
        REQUIRE(boldText != nullptr);
        boldText->SetText("bold run");

        auto reopened = RoundTrip(editor);
        REQUIRE(reopened != nullptr);
        auto reopenedTable =
            reopened->GetDocument()->GetWorkbookPart()->GetSharedStringTablePart()->GetSharedStringTable();
        REQUIRE(reopenedTable != nullptr);

        bool foundRichText = false;
        for (const auto& stringItem : reopenedTable->Elements<Spreadsheet::SharedStringItem>())
        {
            if (stringItem && stringItem->GetFirstChildOfType<Spreadsheet::Run>() != nullptr)
            {
                foundRichText = true;
            }
        }
        CHECK(foundRichText);
        CHECK(reopened->GetDocument()->GetWorkbookPart()->GetSharedStringTablePart()->GetXmlString().find(
                  "bold run") != std::string::npos);

        CheckSavesValidatesAndPreserves(reopened);
    }

    TEST_CASE("Styles and number formats: create, edit, preserve [compat] [excel] [excel-styles]")
    {
        auto editor = MakeWorkbook();
        auto data = editor->GetWorksheet("Data");
        REQUIRE(data != nullptr);

        auto styles = editor->Styles();
        ExcelStyle header;
        header.Font = ExcelFont{};
        header.Font->Bold = true;
        header.NumberFormat = ExcelNumberFormat::Integer();

        const auto registration = styles.GetOrAdd(header);
        REQUIRE(registration.Succeeded());
        REQUIRE(styles.ApplyToRange(*data, Range("A1:D1"), registration.StyleIndex));

        auto reopened = RoundTrip(editor);
        REQUIRE(reopened != nullptr);
        auto readBack = reopened->GetWorksheet("Data");
        REQUIRE(readBack != nullptr);

        auto reopenedStyles = reopened->Styles();
        const auto applied = reopenedStyles.GetCellStyle(*readBack, Address("A1"));
        REQUIRE(applied.has_value());
        REQUIRE(applied->Font.has_value());
        CHECK(applied->Font->Bold);

        // Edit: register a second style and move one cell onto it.
        ExcelStyle italic;
        italic.Font = ExcelFont{};
        italic.Font->Italic = true;
        const auto second = reopenedStyles.GetOrAdd(italic);
        REQUIRE(second.Succeeded());
        REQUIRE(reopenedStyles.ApplyToCell(*readBack, Address("A1"), second.StyleIndex));

        auto edited = RoundTrip(reopened);
        REQUIRE(edited != nullptr);
        const auto editedStyle = edited->Styles().GetCellStyle(*edited->GetWorksheet("Data"), Address("A1"));
        REQUIRE(editedStyle.has_value());
        REQUIRE(editedStyle->Font.has_value());
        CHECK(editedStyle->Font->Italic);

        CheckSavesValidatesAndPreserves(edited);
    }

    TEST_CASE("Named ranges: create and edit, but structural edits leave them alone "
              "[compat] [excel] [excel-named-ranges]")
    {
        auto editor = MakeWorkbook();

        NamedRangeManager names(editor->GetDocument());
        REQUIRE(names.Create("SalesAmounts", SheetCellRange("Data", Range("C2:C6"))).Succeeded());
        REQUIRE(names.Create("QuarterOne", SheetCellRange("Data", Range("C2:C2")), NamedRangeScope::Workbook)
                    .Succeeded());

        auto reopened = RoundTrip(editor);
        REQUIRE(reopened != nullptr);
        NamedRangeManager reopenedNames(reopened->GetDocument());
        CHECK(reopenedNames.Count() == 2);
        REQUIRE(reopenedNames.Find("SalesAmounts").has_value());

        // A formula that uses the name resolves through the engine.
        auto report = reopened->GetWorksheet("Report");
        REQUIRE(report != nullptr);
        REQUIRE(report->SetCellFormula(Address("A1"), "SUM(SalesAmounts)"));
        FormulaEngine engine(reopened->GetDocument());
        const auto evaluated = engine.EvaluateCell("Report", Address("A1"));
        REQUIRE(evaluated.Status.Succeeded());
        CHECK(evaluated.Value.ToDisplayText() == "105");

        // Edit: point the name somewhere else.
        REQUIRE(reopenedNames.SetRange("SalesAmounts", SheetCellRange("Data", Range("D2:D6"))).Succeeded());

        auto edited = RoundTrip(reopened);
        REQUIRE(edited != nullptr);
        NamedRangeManager editedNames(edited->GetDocument());
        const auto moved = editedNames.Find("SalesAmounts");
        REQUIRE(moved.has_value());
        CHECK(moved->Formula.find("$D$2") != std::string::npos);

        CheckSavesValidatesAndPreserves(edited);
    }

    TEST_CASE("Formulas: stored as text, rewritten on structural edits, evaluated on demand "
              "[compat] [excel] [excel-formulas]")
    {
        auto editor = MakeWorkbook();
        auto data = editor->GetWorksheet("Data");
        REQUIRE(data != nullptr);
        REQUIRE(data->SetCellFormula(Address("F2"), "SUM(C2:C6)"));

        auto reopened = RoundTrip(editor);
        REQUIRE(reopened != nullptr);
        auto readBack = reopened->GetWorksheet("Data");
        REQUIRE(readBack != nullptr);

        auto stored = readBack->GetCellFormula(Address("F2"));
        REQUIRE(stored.has_value());
        CHECK(stored->Formula == "SUM(C2:C6)");

        FormulaEngine engine(reopened->GetDocument());
        const auto evaluated = engine.EvaluateCell("Data", Address("F2"));
        REQUIRE(evaluated.Status.Succeeded());
        CHECK(evaluated.Value.ToDisplayText() == "105");

        // Edit: inserting a row above the range rewrites the reference.
        REQUIRE(readBack->InsertRows(1, 1).Succeeded());
        auto rewritten = readBack->GetCellFormula(Address("F3"));
        REQUIRE(rewritten.has_value());
        CHECK(rewritten->Formula == "SUM(C3:C7)");

        auto edited = RoundTrip(reopened);
        REQUIRE(edited != nullptr);
        auto editedFormula = edited->GetWorksheet("Data")->GetCellFormula(Address("F3"));
        REQUIRE(editedFormula.has_value());
        CHECK(editedFormula->Formula == "SUM(C3:C7)");

        CheckSavesValidatesAndPreserves(edited);
    }

    TEST_CASE("External workbook references are preserved but not updated "
              "[compat] [excel] [excel-external-refs]")
    {
        auto editor = MakeWorkbook();
        auto data = editor->GetWorksheet("Data");
        REQUIRE(data != nullptr);

        // A reference into another workbook, exactly as another producer writes
        // it. Create is graded No: nothing here authors the external link part.
        REQUIRE(data->SetCellFormula(Address("F2"), "[1]Other!$A$1+1"));

        auto reopened = RoundTrip(editor);
        REQUIRE(reopened != nullptr);
        auto readBack = reopened->GetWorksheet("Data");
        REQUIRE(readBack != nullptr);

        auto stored = readBack->GetCellFormula(Address("F2"));
        REQUIRE(stored.has_value());
        CHECK(stored->Formula == "[1]Other!$A$1+1");

        // The documented restriction: a structural edit rewrites local
        // references and deliberately leaves the foreign one untouched.
        REQUIRE(readBack->SetCellFormula(Address("F3"), "SUM(C2:C6)"));
        REQUIRE(readBack->InsertRows(1, 1).Succeeded());

        auto foreign = readBack->GetCellFormula(Address("F3"));
        REQUIRE(foreign.has_value());
        CHECK(foreign->Formula == "[1]Other!$A$1+1");

        auto local = readBack->GetCellFormula(Address("F4"));
        REQUIRE(local.has_value());
        CHECK(local->Formula == "SUM(C3:C7)");

        CheckSavesValidatesAndPreserves(reopened);
    }

    TEST_CASE("Tables and auto-filters: create, edit, preserve [compat] [excel] [excel-tables]")
    {
        auto editor = MakeWorkbook();
        auto data = editor->GetWorksheet("Data");
        REQUIRE(data != nullptr);

        const std::vector<ExcelTableColumn> columns = {{0, "Region", {}, {}, {}, TableTotalsFunction::None},
                                                       {0, "Quarter", {}, {}, {}, TableTotalsFunction::None},
                                                       {0, "Amount", {}, {}, {}, TableTotalsFunction::None},
                                                       {0, "Units", {}, {}, {}, TableTotalsFunction::None}};
        auto table = data->CreateTable("SalesTable", Range("A1:D6"), columns);
        REQUIRE(table != nullptr);
        CHECK(table->AutoFilterEnabled());

        auto reopened = RoundTrip(editor);
        REQUIRE(reopened != nullptr);
        auto readBack = reopened->GetWorksheet("Data");
        REQUIRE(readBack != nullptr);
        REQUIRE(readBack->Tables().size() == 1);

        auto reopenedTable = readBack->TableByName("SalesTable");
        REQUIRE(reopenedTable != nullptr);
        REQUIRE(reopenedTable->Range().has_value());
        CHECK(reopenedTable->Range()->ToA1() == "A1:D6");
        CHECK(reopenedTable->Columns().size() == 4);

        // Edit: rename and resize.
        CHECK(reopenedTable->SetName("RenamedTable"));
        CHECK(reopenedTable->Resize(Range("A1:D5")));

        auto edited = RoundTrip(reopened);
        REQUIRE(edited != nullptr);
        auto editedTable = edited->GetWorksheet("Data")->TableByName("RenamedTable");
        REQUIRE(editedTable != nullptr);
        REQUIRE(editedTable->Range().has_value());
        CHECK(editedTable->Range()->ToA1() == "A1:D5");

        CheckSavesValidatesAndPreserves(edited);
    }

    TEST_CASE("Charts: create, edit, preserve [compat] [excel] [excel-charts]")
    {
        auto editor = MakeWorkbook();
        auto report = editor->GetWorksheet("Report");
        REQUIRE(report != nullptr);

        const auto chartId = ChartBuilder(report)
                                 .SetType(ExcelChartType::Column)
                                 .SetTitle("Amount by row")
                                 .SetAnchor(Address("B2"), Address("H20"))
                                 .SetSourceSheet("Data")
                                 .AddSeries("Amount", Range("C2:C6"))
                                 .SetXAxisLabels(Range("A2:A6"))
                                 .Build();
        REQUIRE(chartId.has_value());

        auto reopened = RoundTrip(editor);
        REQUIRE(reopened != nullptr);
        auto readBack = reopened->GetWorksheet("Report");
        REQUIRE(readBack != nullptr);
        REQUIRE(readBack->Charts().size() == 1);

        auto definition = readBack->Charts().front();
        CHECK(definition.Title == "Amount by row");
        CHECK(definition.Type == ExcelChartType::Column);
        REQUIRE(definition.Series.size() == 1);

        // Edit: retitle the chart in place.
        definition.Title = "Edited title";
        CHECK(readBack->UpdateChart(definition));

        auto edited = RoundTrip(reopened);
        REQUIRE(edited != nullptr);
        REQUIRE_FALSE(edited->GetWorksheet("Report")->Charts().empty());
        CHECK(edited->GetWorksheet("Report")->Charts().front().Title == "Edited title");

        CheckSavesValidatesAndPreserves(edited);
    }

    TEST_CASE("Pivot tables: create, edit, preserve [compat] [excel] [excel-pivot]")
    {
        auto editor = MakeWorkbook();
        auto report = editor->GetWorksheet("Report");
        REQUIRE(report != nullptr);

        ExcelPivotTableDefinition definition;
        definition.Name = "SalesByRegion";
        definition.SourceSheet = "Data";
        definition.SourceRange = Range("A1:D6");
        definition.TargetCell = Address("A1");
        definition.Fields = {{"Region", PivotAxis::Row}, {"Quarter", PivotAxis::Column}};
        definition.DataFields.push_back(ExcelPivotDataField{"Amount"});

        const auto created = report->CreatePivotTable(definition);
        CAPTURE(created.Status.Message);
        REQUIRE(created.PivotTable != nullptr);

        auto reopened = RoundTrip(editor);
        REQUIRE(reopened != nullptr);
        auto readBack = reopened->GetWorksheet("Report");
        REQUIRE(readBack != nullptr);
        REQUIRE(readBack->PivotTables().size() == 1);

        auto pivot = readBack->PivotTableByName("SalesByRegion");
        REQUIRE(pivot != nullptr);
        CHECK(pivot->SourceSheet() == "Data");
        CHECK_FALSE(pivot->SourceFieldNames().empty());

        // Edit: rename and refresh the cache.
        CHECK(pivot->SetName("RenamedPivot"));
        CHECK(pivot->Refresh());

        auto edited = RoundTrip(reopened);
        REQUIRE(edited != nullptr);
        CHECK(edited->GetWorksheet("Report")->PivotTableByName("RenamedPivot") != nullptr);

        CheckSavesValidatesAndPreserves(edited);
    }

    TEST_CASE("Slicers: create, edit, preserve [compat] [excel] [excel-slicers]")
    {
        auto editor = MakeWorkbook();
        auto report = editor->GetWorksheet("Report");
        REQUIRE(report != nullptr);

        ExcelPivotTableDefinition pivotDefinition;
        pivotDefinition.Name = "SalesByRegion";
        pivotDefinition.SourceSheet = "Data";
        pivotDefinition.SourceRange = Range("A1:D6");
        pivotDefinition.TargetCell = Address("A1");
        pivotDefinition.Fields = {{"Region", PivotAxis::Row}};
        pivotDefinition.DataFields.push_back(ExcelPivotDataField{"Amount"});
        REQUIRE(report->CreatePivotTable(pivotDefinition).PivotTable != nullptr);

        ExcelSlicerDefinition slicerDefinition;
        slicerDefinition.Name = "RegionSlicer";
        slicerDefinition.SourceKind = SlicerSourceKind::PivotTable;
        slicerDefinition.PivotTableName = "SalesByRegion";
        slicerDefinition.SourceField = "Region";
        slicerDefinition.From = Address("F2");
        slicerDefinition.To = Address("H12");

        const auto created = report->CreateSlicer(slicerDefinition);
        CAPTURE(created.Status.Message);
        REQUIRE(created.Slicer != nullptr);

        auto reopened = RoundTrip(editor);
        REQUIRE(reopened != nullptr);
        auto slicer = reopened->GetWorksheet("Report")->SlicerByName("RegionSlicer");
        REQUIRE(slicer != nullptr);
        CHECK(slicer->SourceField() == "Region");
        CHECK_FALSE(slicer->Items().empty());

        // Edit: change the caption.
        CHECK(slicer->SetCaption("Regions"));

        auto edited = RoundTrip(reopened);
        REQUIRE(edited != nullptr);
        auto editedSlicer = edited->GetWorksheet("Report")->SlicerByName("RegionSlicer");
        REQUIRE(editedSlicer != nullptr);
        CHECK(editedSlicer->Caption() == "Regions");

        CheckSavesValidatesAndPreserves(edited);
    }

    TEST_CASE("Data validation: create, edit, preserve [compat] [excel] [excel-validation]")
    {
        auto editor = MakeWorkbook();
        auto data = editor->GetWorksheet("Data");
        REQUIRE(data != nullptr);

        ExcelDataValidationDefinition definition;
        definition.Type = DataValidationType::Whole;
        definition.Operation = DataValidationOperator::Between;
        definition.Formula1 = "1";
        definition.Formula2 = "100";
        definition.Ranges = {Range("D2:D6")};
        definition.ShowErrorMessage = true;
        definition.ErrorTitle = "Out of range";

        auto rule = data->CreateDataValidation(definition);
        REQUIRE(rule != nullptr);

        auto reopened = RoundTrip(editor);
        REQUIRE(reopened != nullptr);
        auto readBack = reopened->GetWorksheet("Data");
        REQUIRE(readBack != nullptr);
        REQUIRE(readBack->DataValidations().size() == 1);

        auto reopenedRule = readBack->DataValidations().front();
        CHECK(reopenedRule->Definition().Formula2 == "100");

        // Edit: widen the allowed interval.
        auto widened = reopenedRule->Definition();
        widened.Formula2 = "1000";
        CHECK(readBack->UpdateDataValidation(reopenedRule, widened));

        auto edited = RoundTrip(reopened);
        REQUIRE(edited != nullptr);
        REQUIRE_FALSE(edited->GetWorksheet("Data")->DataValidations().empty());
        CHECK(edited->GetWorksheet("Data")->DataValidations().front()->Definition().Formula2 == "1000");

        CheckSavesValidatesAndPreserves(edited);
    }

    TEST_CASE("Conditional formatting: rules reference dxfs but do not create them "
              "[compat] [excel] [excel-conditional-formatting]")
    {
        auto editor = MakeWorkbook();
        auto data = editor->GetWorksheet("Data");
        REQUIRE(data != nullptr);

        auto rule = data->CreateConditionalFormatting(ExcelConditionalFormattingDefinition::CellIs(
            {Range("C2:C6")}, ConditionalFormattingOperator::GreaterThan, "20"));
        REQUIRE(rule != nullptr);

        auto reopened = RoundTrip(editor);
        REQUIRE(reopened != nullptr);
        auto readBack = reopened->GetWorksheet("Data");
        REQUIRE(readBack != nullptr);
        REQUIRE(readBack->ConditionalFormattings().size() == 1);

        auto reopenedRule = readBack->ConditionalFormattings().front();
        REQUIRE_FALSE(reopenedRule->Definition().Formulas.empty());
        CHECK(reopenedRule->Definition().Formulas.front() == "20");
        // The rule points at a differential format by index; it does not create
        // one, which is what the Notes column of this row says.
        CHECK_FALSE(reopenedRule->Definition().DifferentialFormatId.has_value());

        // Edit: raise the threshold.
        auto raised = reopenedRule->Definition();
        raised.Formulas = {"25"};
        CHECK(readBack->UpdateConditionalFormatting(reopenedRule, raised));

        auto edited = RoundTrip(reopened);
        REQUIRE(edited != nullptr);
        REQUIRE_FALSE(edited->GetWorksheet("Data")->ConditionalFormattings().empty());
        const auto editedDefinition = edited->GetWorksheet("Data")->ConditionalFormattings().front()->Definition();
        REQUIRE_FALSE(editedDefinition.Formulas.empty());
        CHECK(editedDefinition.Formulas.front() == "25");

        CheckSavesValidatesAndPreserves(edited);
    }

    TEST_CASE("Layout and annotations: create, edit, preserve [compat] [excel] [excel-layout]")
    {
        auto editor = MakeWorkbook();
        auto data = editor->GetWorksheet("Data");
        REQUIRE(data != nullptr);

        REQUIRE(data->SetColumnDimension(1, ColumnDimension{.Width = 24.0}));
        REQUIRE(data->SetView(WorksheetView{.FrozenRows = 1}));

        ExcelHyperlink link;
        link.Address = Address("A1");
        link.Target = "https://example.com";
        link.Display = "Region";
        REQUIRE(data->SetHyperlink(link));

        ExcelComment comment;
        comment.Address = Address("B1");
        comment.Author = "Reviewer";
        comment.Text = "Check the quarter";
        REQUIRE(data->SetComment(comment));

        auto reopened = RoundTrip(editor);
        REQUIRE(reopened != nullptr);
        auto readBack = reopened->GetWorksheet("Data");
        REQUIRE(readBack != nullptr);
        REQUIRE(readBack->GetColumnDimension(1).has_value());
        CHECK(readBack->GetColumnDimension(1)->Width == doctest::Approx(24.0));
        CHECK(readBack->GetView().FrozenRows == 1);
        REQUIRE(readBack->GetHyperlink(Address("A1")).has_value());
        CHECK(readBack->GetHyperlink(Address("A1"))->Target == "https://example.com");
        REQUIRE(readBack->GetComment(Address("B1")).has_value());
        CHECK(readBack->GetComment(Address("B1"))->Text == "Check the quarter");

        // Edit: remove the hyperlink and change the comment.
        CHECK(readBack->RemoveHyperlink(Address("A1")));
        auto edit = *readBack->GetComment(Address("B1"));
        edit.Text = "Edited note";
        CHECK(readBack->SetComment(edit));

        auto edited = RoundTrip(reopened);
        REQUIRE(edited != nullptr);
        auto editedSheet = edited->GetWorksheet("Data");
        REQUIRE(editedSheet != nullptr);
        CHECK_FALSE(editedSheet->GetHyperlink(Address("A1")).has_value());
        REQUIRE(editedSheet->GetComment(Address("B1")).has_value());
        CHECK(editedSheet->GetComment(Address("B1"))->Text == "Edited note");

        CheckSavesValidatesAndPreserves(edited);
    }

    TEST_CASE("Printing: create, edit, preserve [compat] [excel] [excel-printing]")
    {
        auto editor = MakeWorkbook();
        auto data = editor->GetWorksheet("Data");
        REQUIRE(data != nullptr);

        PageSetup setup;
        setup.Orientation = PageOrientation::Landscape;
        REQUIRE(data->SetPageSetup(setup));
        REQUIRE(data->SetPrintArea({Range("A1:D6")}));

        HeaderFooter headerFooter;
        headerFooter.OddHeader = "&CSales";
        REQUIRE(data->SetHeaderFooter(headerFooter));

        auto reopened = RoundTrip(editor);
        REQUIRE(reopened != nullptr);
        auto readBack = reopened->GetWorksheet("Data");
        REQUIRE(readBack != nullptr);
        CHECK(readBack->GetPageSetup().Orientation == PageOrientation::Landscape);
        CHECK(readBack->GetPrintArea().size() == 1);
        CHECK(readBack->GetHeaderFooter().OddHeader == "&CSales");

        // Edit: flip back to portrait.
        auto portrait = readBack->GetPageSetup();
        portrait.Orientation = PageOrientation::Portrait;
        CHECK(readBack->SetPageSetup(portrait));

        auto edited = RoundTrip(reopened);
        REQUIRE(edited != nullptr);
        CHECK(edited->GetWorksheet("Data")->GetPageSetup().Orientation == PageOrientation::Portrait);

        CheckSavesValidatesAndPreserves(edited);
    }

} // TEST_SUITE("ExcelMatrixTests")
