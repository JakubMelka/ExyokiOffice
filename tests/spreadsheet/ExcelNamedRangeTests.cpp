// Copyright (c) 2026 Jakub Melka and Contributors
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "doctest.h"

#include "ExyokiOffice/Excel/ExcelDocument.hpp"
#include "ExyokiOffice/Excel/ExcelFormulaEngine.hpp"
#include "ExyokiOffice/Excel/ExcelNamedRange.hpp"
#include "ExyokiOffice/StandardTypes.hpp"

#include <algorithm>

using namespace ExyokiOffice::Excel;

class NamedRangeTestHelpers final
{
public:
    NamedRangeTestHelpers() = delete;

    static CellAddress Address(std::string_view text)
    {
        const auto address = CellAddress::ParseA1(text);
        REQUIRE(address);
        return *address;
    }

    static SheetCellRange Range(std::string_view sheet, std::string_view rangeText)
    {
        const auto range = CellRange::ParseA1(rangeText);
        REQUIRE(range);
        return SheetCellRange(std::string(sheet), *range);
    }

    static ExyokiOffice::Real Number(const FormulaEngine& engine, std::string_view formula,
                                     std::string_view sheetName = {})
    {
        const auto result = engine.EvaluateFormula(formula, sheetName);
        REQUIRE(result.Succeeded());
        REQUIRE(result.Value.Kind() == FormulaValueKind::Number);
        return *result.Value.NumberValue();
    }

    static FormulaErrorCode Error(const FormulaEngine& engine, std::string_view formula)
    {
        const auto result = engine.EvaluateFormula(formula);
        REQUIRE(result.Succeeded());
        REQUIRE(result.Value.IsError());
        return result.Value.ErrorCode();
    }
};

using H = NamedRangeTestHelpers;

TEST_SUITE("ExcelNamedRangeTests")
{

    // ---------------------------------------------------------------------------
    // Manager: creation and queries
    // ---------------------------------------------------------------------------

    TEST_CASE("Create stores an absolute sheet-qualified range [unit] [excel] [excel-named-range]")
    {
        auto editor = ExcelDocumentEditor::CreateNew();
        NamedRangeManager names(editor->GetDocument());
        REQUIRE(names.IsValid());
        CHECK(names.Count() == 0);

        REQUIRE(names.Create("SalesData", H::Range("Sheet1", "A1:B4")));
        CHECK(names.Count() == 1);

        const auto found = names.Find("SalesData");
        REQUIRE(found);
        CHECK(found->Name == "SalesData");
        CHECK(found->Scope == NamedRangeScope::Workbook);
        CHECK(found->ScopeSheet.empty());
        CHECK(found->Formula == "Sheet1!$A$1:$B$4");
        CHECK_FALSE(found->Hidden);

        const auto range = names.GetRange("SalesData");
        REQUIRE(range);
        CHECK(range->Sheet() == "Sheet1");
        CHECK(range->Range().ToA1() == "A1:B4");

        // Lookup is case-insensitive.
        CHECK(names.Find("salesdata"));
        CHECK(names.Find("SALESDATA"));
        CHECK_FALSE(names.Find("Other"));

        // The workbook XML holds the definedName entry.
        const auto xml = editor->GetDocument()->GetWorkbookPart()->GetXmlString();
        CHECK(xml.find("definedName") != std::string::npos);
        CHECK(xml.find("SalesData") != std::string::npos);
        CHECK(xml.find("Sheet1!$A$1:$B$4") != std::string::npos);
    }

    TEST_CASE("Create validates names, sheets, and ranges [unit] [excel] [excel-named-range]")
    {
        auto editor = ExcelDocumentEditor::CreateNew();
        NamedRangeManager names(editor->GetDocument());

        // Invalid names.
        for (const std::string_view invalid :
             {"", "A1", "XFD1048576", "R1C1", "R", "C", "r", "1Data", "has space", "a-b", "x!y"})
        {
            CAPTURE(invalid);
            const auto result = names.Create(invalid, H::Range("Sheet1", "A1"));
            CHECK_FALSE(result);
            CHECK(result.Error == NamedRangeError::InvalidName);
            CHECK_FALSE(NamedRangeManager::IsValidName(invalid));
        }
        const std::string tooLong(256, 'a');
        CHECK_FALSE(NamedRangeManager::IsValidName(tooLong));

        // Valid names.
        for (const std::string_view valid : {"_x", "Data.2024", "TAX_RATE", "\\key", "N\xC3\xA1klady"})
        {
            CAPTURE(valid);
            CHECK(NamedRangeManager::IsValidName(valid));
        }

        // Unknown target sheet.
        const auto unknownSheet = names.Create("Data", H::Range("Missing", "A1"));
        CHECK(unknownSheet.Error == NamedRangeError::UnknownSheet);

        // Invalid range.
        const auto invalidRange = names.Create("Data", SheetCellRange("Sheet1", CellRange()));
        CHECK(invalidRange.Error == NamedRangeError::InvalidRange);

        // Detached manager.
        NamedRangeManager detached;
        CHECK_FALSE(detached.IsValid());
        CHECK(detached.Create("Data", H::Range("Sheet1", "A1")).Error == NamedRangeError::InvalidDocument);
        CHECK_FALSE(detached.Find("Data"));
        CHECK(detached.List().empty());
    }

    TEST_CASE("Names are unique per scope, not globally [unit] [excel] [excel-named-range]")
    {
        auto editor = ExcelDocumentEditor::CreateNew();
        REQUIRE(editor->AddWorksheet("Report"));
        NamedRangeManager names(editor->GetDocument());

        REQUIRE(names.Create("Data", H::Range("Sheet1", "A1")));
        // Same spelling again in the workbook scope is rejected, case-insensitively.
        CHECK(names.Create("data", H::Range("Sheet1", "B1")).Error == NamedRangeError::DuplicateName);
        // The same spelling is allowed as a sheet-scoped name.
        REQUIRE(names.Create("Data", H::Range("Report", "C1"), NamedRangeScope::Sheet, "Report"));
        CHECK(names.Count() == 2);
        // And rejected a second time in that same sheet scope.
        CHECK(names.Create("DATA", H::Range("Report", "D1"), NamedRangeScope::Sheet, "Report").Error ==
              NamedRangeError::DuplicateName);
        // Sheet scope requires an existing worksheet.
        CHECK(names.Create("Other", H::Range("Sheet1", "A1"), NamedRangeScope::Sheet, "Missing").Error ==
              NamedRangeError::UnknownSheet);

        // Find distinguishes scopes.
        const auto workbookScoped = names.Find("Data");
        REQUIRE(workbookScoped);
        CHECK(workbookScoped->Formula == "Sheet1!$A$1");
        const auto sheetScoped = names.Find("Data", "Report");
        REQUIRE(sheetScoped);
        CHECK(sheetScoped->Scope == NamedRangeScope::Sheet);
        CHECK(sheetScoped->ScopeSheet == "Report");
        CHECK(sheetScoped->Formula == "Report!$C$1");

        // Resolve prefers the sheet scope on its sheet and falls back elsewhere.
        CHECK(names.Resolve("Data", "Report")->Formula == "Report!$C$1");
        CHECK(names.Resolve("Data", "Sheet1")->Formula == "Sheet1!$A$1");

        // List filters by scope.
        CHECK(names.List().size() == 2);
        CHECK(names.List(NamedRangeScope::Workbook).size() == 1);
        CHECK(names.List(NamedRangeScope::Sheet).size() == 1);
    }

    TEST_CASE("CreateFromFormula stores constants and computed names [unit] [excel] [excel-named-range]")
    {
        auto editor = ExcelDocumentEditor::CreateNew();
        NamedRangeManager names(editor->GetDocument());

        REQUIRE(names.CreateFromFormula("TaxRate", "=0.21"));
        REQUIRE(names.CreateFromFormula("Total", "SUM(Sheet1!$A$1:$A$9)"));

        const auto taxRate = names.Find("TaxRate");
        REQUIRE(taxRate);
        CHECK(taxRate->Formula == "0.21"); // leading '=' stripped
        CHECK_FALSE(taxRate->Range());     // not a plain range
        const auto total = names.Find("Total");
        REQUIRE(total);
        CHECK_FALSE(total->Range());

        // Empty definitions are rejected.
        CHECK(names.CreateFromFormula("Empty", "").Error == NamedRangeError::InvalidRange);
        CHECK(names.CreateFromFormula("Empty", "=").Error == NamedRangeError::InvalidRange);
    }

    // ---------------------------------------------------------------------------
    // Manager: mutation
    // ---------------------------------------------------------------------------

    TEST_CASE("SetRange, SetFormula, and Rename update names in place [unit] [excel] [excel-named-range]")
    {
        auto editor = ExcelDocumentEditor::CreateNew();
        NamedRangeManager names(editor->GetDocument());
        REQUIRE(names.Create("Data", H::Range("Sheet1", "A1:A3")));
        REQUIRE(names.Create("Other", H::Range("Sheet1", "B1")));

        REQUIRE(names.SetRange("Data", H::Range("Sheet1", "A1:A9")));
        CHECK(names.GetRange("Data")->Range().ToA1() == "A1:A9");
        CHECK(names.SetRange("Missing", H::Range("Sheet1", "A1")).Error == NamedRangeError::NameNotFound);
        CHECK(names.SetRange("Data", H::Range("Missing", "A1")).Error == NamedRangeError::UnknownSheet);

        REQUIRE(names.SetFormula("Data", "=1+2"));
        CHECK(names.Find("Data")->Formula == "1+2");
        CHECK(names.SetFormula("Data", "").Error == NamedRangeError::InvalidRange);

        REQUIRE(names.Rename("Data", "Renamed"));
        CHECK_FALSE(names.Find("Data"));
        CHECK(names.Find("Renamed"));
        CHECK(names.Rename("Missing", "X").Error == NamedRangeError::NameNotFound);
        CHECK(names.Rename("Renamed", "Other").Error == NamedRangeError::DuplicateName);
        CHECK(names.Rename("Renamed", "A1").Error == NamedRangeError::InvalidName);
        // Case-only rename of the same entry is allowed.
        REQUIRE(names.Rename("Renamed", "RENAMED"));
        CHECK(names.Find("renamed")->Name == "RENAMED");
    }

    TEST_CASE("Remove deletes entries and prunes the empty collection [unit] [excel] [excel-named-range]")
    {
        auto editor = ExcelDocumentEditor::CreateNew();
        NamedRangeManager names(editor->GetDocument());
        REQUIRE(names.Create("First", H::Range("Sheet1", "A1")));
        REQUIRE(names.Create("Second", H::Range("Sheet1", "B1")));

        REQUIRE(names.Remove("First"));
        CHECK(names.Count() == 1);
        CHECK_FALSE(names.Find("First"));
        CHECK(names.Remove("First").Error == NamedRangeError::NameNotFound);

        REQUIRE(names.Remove("Second"));
        CHECK(names.Count() == 0);
        // The empty definedNames element is removed from the workbook markup.
        const auto xml = editor->GetDocument()->GetWorkbookPart()->GetXmlString();
        CHECK(xml.find("definedNames") == std::string::npos);
    }

    // ---------------------------------------------------------------------------
    // Persistence
    // ---------------------------------------------------------------------------

    TEST_CASE("Defined names survive a package round-trip [unit] [excel] [excel-named-range]")
    {
        auto editor = ExcelDocumentEditor::CreateNew();
        REQUIRE(editor->AddWorksheet("Report"));
        NamedRangeManager names(editor->GetDocument());
        REQUIRE(names.Create("GlobalData", H::Range("Sheet1", "A1:B2")));
        REQUIRE(names.Create("LocalData", H::Range("Report", "C1:C9"), NamedRangeScope::Sheet, "Report"));
        REQUIRE(names.CreateFromFormula("TaxRate", "=0.21"));

        const auto bytes = editor->SaveToMemory();
        REQUIRE_FALSE(bytes.empty());
        auto reopened = ExcelDocumentEditor::Open(bytes);
        REQUIRE(reopened);
        NamedRangeManager reopenedNames(reopened->GetDocument());
        CHECK(reopenedNames.Count() == 3);

        const auto global = reopenedNames.Find("GlobalData");
        REQUIRE(global);
        CHECK(global->Scope == NamedRangeScope::Workbook);
        CHECK(global->Formula == "Sheet1!$A$1:$B$2");

        const auto local = reopenedNames.Find("LocalData", "Report");
        REQUIRE(local);
        CHECK(local->Scope == NamedRangeScope::Sheet);
        CHECK(local->ScopeSheet == "Report");
        CHECK(local->Range()->Range().ToA1() == "C1:C9");
        // The sheet-scoped entry is invisible in the workbook scope.
        CHECK_FALSE(reopenedNames.Find("LocalData"));

        CHECK(reopenedNames.Find("TaxRate")->Formula == "0.21");
    }

    // ---------------------------------------------------------------------------
    // Formula engine integration: evaluation
    // ---------------------------------------------------------------------------

    TEST_CASE("Formulas resolve defined names [unit] [excel] [excel-named-range]")
    {
        auto editor = ExcelDocumentEditor::CreateNew();
        auto sheet = editor->FirstWorksheet();
        REQUIRE(sheet->SetCellNumber(1, 1, 10.0));
        REQUIRE(sheet->SetCellNumber(2, 1, 32.0));
        NamedRangeManager names(editor->GetDocument());
        REQUIRE(names.Create("SalesData", H::Range("Sheet1", "A1:A2")));
        REQUIRE(names.CreateFromFormula("TaxRate", "=0.5"));
        FormulaEngine engine(editor->GetDocument());

        // Range-valued names flow into aggregates as references.
        CHECK(H::Number(engine, "=SUM(SalesData)") == doctest::Approx(42.0));
        CHECK(H::Number(engine, "=COUNT(SalesData)") == doctest::Approx(2.0));
        // Name lookup in formulas is case-insensitive.
        CHECK(H::Number(engine, "=SUM(salesdata)") == doctest::Approx(42.0));
        // Constant names act as scalars.
        CHECK(H::Number(engine, "=TaxRate*100") == doctest::Approx(50.0));
        CHECK(H::Number(engine, "=SUM(SalesData)*TaxRate") == doctest::Approx(21.0));
        // Unknown names still evaluate to #NAME?.
        CHECK(H::Error(engine, "=NoSuchName+1") == FormulaErrorCode::Name);

        // Computed names evaluate their definition.
        REQUIRE(names.CreateFromFormula("Total", "SUM(Sheet1!$A$1:$A$2)"));
        CHECK(H::Number(engine, "=Total+8") == doctest::Approx(50.0));
        // Names may reference other names.
        REQUIRE(names.CreateFromFormula("TotalWithTax", "Total*(1+TaxRate)"));
        CHECK(H::Number(engine, "=TotalWithTax") == doctest::Approx(63.0));
    }

    TEST_CASE("Sheet-scoped names shadow workbook names on their sheet [unit] [excel] [excel-named-range]")
    {
        auto editor = ExcelDocumentEditor::CreateNew();
        auto first = editor->FirstWorksheet();
        auto second = editor->AddWorksheet("Report");
        REQUIRE(second);
        REQUIRE(first->SetCellNumber(1, 1, 100.0));
        REQUIRE(second->SetCellNumber(1, 1, 999.0));

        NamedRangeManager names(editor->GetDocument());
        REQUIRE(names.CreateFromFormula("Rate", "=0.1"));
        REQUIRE(names.CreateFromFormula("Rate", "=0.2", NamedRangeScope::Sheet, "Report"));
        FormulaEngine engine(editor->GetDocument());

        // The workbook-scoped name applies on Sheet1.
        CHECK(H::Number(engine, "=Rate", "Sheet1") == doctest::Approx(0.1));
        // The sheet-scoped name shadows it on Report.
        CHECK(H::Number(engine, "=Rate", "Report") == doctest::Approx(0.2));
        // A sheet qualifier selects the sheet scope explicitly.
        CHECK(H::Number(engine, "=Report!Rate", "Sheet1") == doctest::Approx(0.2));

        // Unqualified references inside a name resolve on the evaluating sheet.
        REQUIRE(names.CreateFromFormula("FirstCell", "Sheet1!$A$1"));
        CHECK(H::Number(engine, "=FirstCell", "Report") == doctest::Approx(100.0));
    }

    TEST_CASE("ValidateFormula reports unknown names [unit] [excel] [excel-named-range]")
    {
        auto editor = ExcelDocumentEditor::CreateNew();
        NamedRangeManager names(editor->GetDocument());
        REQUIRE(names.Create("KnownName", H::Range("Sheet1", "A1")));
        FormulaEngine engine(editor->GetDocument());

        CHECK(engine.ValidateFormula("=KnownName+1"));
        CHECK(engine.ValidateFormula("=SUM(KnownName)"));
        const auto unknown = engine.ValidateFormula("=MysteryName+1");
        REQUIRE_FALSE(unknown.Succeeded());
        REQUIRE_FALSE(unknown.Diagnostics.empty());
        CHECK(unknown.Diagnostics.front().Message.find("MysteryName") != std::string::npos);
    }

    // ---------------------------------------------------------------------------
    // Formula engine integration: recalculation
    // ---------------------------------------------------------------------------

    TEST_CASE("Recalculation tracks dependencies through names [unit] [excel] [excel-named-range]")
    {
        auto editor = ExcelDocumentEditor::CreateNew();
        auto sheet = editor->FirstWorksheet();
        // B1 = A1*2 (stored later than its dependent), C1 = Doubled + 1 where
        // Doubled is a name for B1. Correct ordering requires the dependency to
        // flow through the name.
        REQUIRE(sheet->SetCellFormula(H::Address("C1"), "=Doubled+1"));
        REQUIRE(sheet->SetCellFormula(H::Address("B1"), "=A1*2"));
        REQUIRE(sheet->SetCellNumber(1, 1, 21.0));
        NamedRangeManager names(editor->GetDocument());
        REQUIRE(names.Create("Doubled", H::Range("Sheet1", "B1")));

        FormulaEngine engine(editor->GetDocument());
        const auto result = engine.Recalculate();
        REQUIRE(result.Succeeded());
        CHECK(result.RecalculatedCellCount == 2);
        CHECK(result.CircularReferenceCycles.empty());
        CHECK(sheet->GetCellFormula(H::Address("B1"))->CachedText == "42");
        CHECK(sheet->GetCellFormula(H::Address("C1"))->CachedText == "43");

        // EvaluateCell resolves names as well.
        const auto cell = engine.EvaluateCell("Sheet1", H::Address("C1"));
        REQUIRE(cell.Succeeded());
        CHECK(*cell.Value.NumberValue() == doctest::Approx(43.0));
    }

    TEST_CASE("Cycles through names are detected [unit] [excel] [excel-named-range]")
    {
        auto editor = ExcelDocumentEditor::CreateNew();
        auto sheet = editor->FirstWorksheet();
        NamedRangeManager names(editor->GetDocument());
        // B1 refers to itself through the name.
        REQUIRE(names.Create("Myself", H::Range("Sheet1", "B1")));
        REQUIRE(sheet->SetCellFormula(H::Address("B1"), "=Myself+1",
                                      FormulaCachedValueKind::Number, "7"));
        // A healthy cell coexists.
        REQUIRE(sheet->SetCellNumber(1, 1, 5.0));
        REQUIRE(sheet->SetCellFormula(H::Address("A2"), "=A1*2"));

        FormulaEngine engine(editor->GetDocument());
        const auto cycles = engine.FindCircularReferences();
        REQUIRE(cycles.size() == 1);
        REQUIRE(cycles.front().size() == 1);
        CHECK(cycles.front().front().ToFormula() == "Sheet1!B1");

        const auto result = engine.Recalculate();
        REQUIRE(result.Succeeded());
        CHECK(result.CircularReferenceCycles.size() == 1);
        // The cycle member keeps its previous cached value.
        CHECK(sheet->GetCellFormula(H::Address("B1"))->CachedText == "7");
        CHECK(sheet->GetCellFormula(H::Address("A2"))->CachedText == "10");

        // Name-to-name definition cycles terminate with an error value instead
        // of recursing forever.
        REQUIRE(names.CreateFromFormula("Ping", "Pong+1"));
        REQUIRE(names.CreateFromFormula("Pong", "Ping+1"));
        const auto pingPong = engine.EvaluateFormula("=Ping");
        REQUIRE(pingPong.Succeeded());
        CHECK(pingPong.Value.IsError());
    }

    TEST_CASE("Recalculated named-range workbooks round-trip [unit] [excel] [excel-named-range]")
    {
        auto editor = ExcelDocumentEditor::CreateNew();
        auto sheet = editor->FirstWorksheet();
        REQUIRE(sheet->SetCellNumber(1, 1, 10.0));
        REQUIRE(sheet->SetCellNumber(2, 1, 32.0));
        NamedRangeManager names(editor->GetDocument());
        REQUIRE(names.Create("SalesData", H::Range("Sheet1", "A1:A2")));
        REQUIRE(sheet->SetCellFormula(H::Address("B1"), "=SUM(SalesData)"));

        FormulaEngine engine(editor->GetDocument());
        REQUIRE(engine.Recalculate());
        CHECK(sheet->GetCellFormula(H::Address("B1"))->CachedText == "42");

        const auto bytes = editor->SaveToMemory();
        REQUIRE_FALSE(bytes.empty());
        auto reopened = ExcelDocumentEditor::Open(bytes);
        REQUIRE(reopened);
        auto reopenedSheet = reopened->FirstWorksheet();
        REQUIRE(reopenedSheet);
        // The formula text and cached value survive.
        const auto formula = reopenedSheet->GetCellFormula(H::Address("B1"));
        REQUIRE(formula);
        CHECK(formula->Formula == "SUM(SalesData)");
        CHECK(formula->CachedText == "42");
        // A fresh engine on the reopened workbook still resolves the name.
        FormulaEngine reopenedEngine(reopened->GetDocument());
        const auto value = reopenedEngine.EvaluateCell("Sheet1", H::Address("B1"));
        REQUIRE(value.Succeeded());
        CHECK(*value.Value.NumberValue() == doctest::Approx(42.0));
    }

} // TEST_SUITE
