// Copyright (c) 2026 Jakub Melka and Contributors
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "doctest.h"

#include "ExyokiOffice/Excel/ExcelDocument.hpp"

using namespace ExyokiOffice::Excel;

class ExcelFormulaTestHelpers final
{
public:
    ExcelFormulaTestHelpers() = delete;

    static CellAddress Address(std::string_view text)
    {
        const auto address = CellAddress::ParseA1(text);
        REQUIRE(address);
        return *address;
    }
};

TEST_SUITE("ExcelFormulaTests")
{
    TEST_CASE("Normal formulas preserve expression cache and optional equals prefix [unit] [excel] [excel-formula]")
    {
        auto editor = ExcelDocumentEditor::CreateNew();
        auto sheet = editor->FirstWorksheet();
        REQUIRE(sheet->SetCellFormula(ExcelFormulaTestHelpers::Address("B2"), "=SUM(A1:A3)",
                                      FormulaCachedValueKind::Number, "12.5000"));
        const auto formula = sheet->GetCellFormula(ExcelFormulaTestHelpers::Address("B2"));
        REQUIRE(formula);
        CHECK(formula->Formula == "SUM(A1:A3)");
        CHECK(formula->Kind == CellFormulaKind::Normal);
        CHECK(formula->ReferenceStyle == FormulaReferenceStyle::A1);
        CHECK(formula->CachedKind == FormulaCachedValueKind::Number);
        CHECK(formula->CachedText == "12.5000");
        CHECK_FALSE(formula->Reference);
        CHECK_FALSE(formula->SharedIndex);
    }

    TEST_CASE("Shared formula anchors and dependents retain group metadata [unit] [excel] [excel-formula]")
    {
        auto editor = ExcelDocumentEditor::CreateNew();
        auto sheet = editor->FirstWorksheet();
        REQUIRE(sheet->SetCellFormula(ExcelFormulaTestHelpers::Address("C2"),
                                      CellFormulaValue::Shared("A2+B2", 17, "C2:C5",
                                                               FormulaCachedValueKind::Number, "3")));
        REQUIRE(sheet->SetCellFormula(ExcelFormulaTestHelpers::Address("C3"),
                                      CellFormulaValue::SharedDependent(17, FormulaCachedValueKind::Number, "5")));
        const auto anchor = sheet->GetCellFormula(ExcelFormulaTestHelpers::Address("C2"));
        const auto dependent = sheet->GetCellFormula(ExcelFormulaTestHelpers::Address("C3"));
        REQUIRE(anchor);
        REQUIRE(dependent);
        CHECK(anchor->Kind == CellFormulaKind::Shared);
        CHECK(anchor->Formula == "A2+B2");
        CHECK(anchor->SharedIndex == 17);
        CHECK(anchor->Reference == "C2:C5");
        CHECK(dependent->Kind == CellFormulaKind::Shared);
        CHECK(dependent->Formula.empty());
        CHECK(dependent->SharedIndex == 17);
        CHECK_FALSE(dependent->Reference);
    }

    TEST_CASE("Array formulas preserve result range and recalculation flag [unit] [excel] [excel-formula]")
    {
        auto editor = ExcelDocumentEditor::CreateNew();
        auto sheet = editor->FirstWorksheet();
        REQUIRE(sheet->SetCellFormula(ExcelFormulaTestHelpers::Address("D4"),
                                      CellFormulaValue::Array("TRANSPOSE(A1:B2)", "D4:E5",
                                                              FormulaCachedValueKind::Number, "1", true)));
        const auto formula = sheet->GetCellFormula(ExcelFormulaTestHelpers::Address("D4"));
        REQUIRE(formula);
        CHECK(formula->Kind == CellFormulaKind::Array);
        CHECK(formula->Reference == "D4:E5");
        CHECK(formula->AlwaysCalculateArray);
        const auto xml = sheet->GetPart()->GetXmlString();
        CHECK(xml.find("t=\"array\"") != std::string::npos);
        CHECK(xml.find("ref=\"D4:E5\"") != std::string::npos);
        CHECK(xml.find("aca=\"1\"") != std::string::npos);
    }

    TEST_CASE("R1C1 reference mode is stored at workbook scope and returned for every formula [unit] [excel] [excel-formula]")
    {
        auto editor = ExcelDocumentEditor::CreateNew();
        auto sheet = editor->FirstWorksheet();
        auto model = CellFormulaValue::Normal("RC[-1]*2", FormulaCachedValueKind::Number, "8",
                                              FormulaReferenceStyle::R1C1);
        REQUIRE(sheet->SetCellFormula(ExcelFormulaTestHelpers::Address("B2"), model));
        REQUIRE(sheet->GetCellFormula(ExcelFormulaTestHelpers::Address("B2")));
        CHECK(sheet->GetCellFormula(ExcelFormulaTestHelpers::Address("B2"))->ReferenceStyle ==
              FormulaReferenceStyle::R1C1);
        const auto xml = editor->GetDocument()->GetWorkbookPart()->GetXmlString();
        CHECK(xml.find("refMode=\"R1C1\"") != std::string::npos);
    }

    TEST_CASE("Invalid formula models do not create cells [unit] [excel] [excel-formula]")
    {
        auto editor = ExcelDocumentEditor::CreateNew();
        auto sheet = editor->FirstWorksheet();
        const auto address = ExcelFormulaTestHelpers::Address("B2");
        CHECK_FALSE(sheet->SetCellFormula(address, CellFormulaValue::Normal("")));
        CHECK_FALSE(sheet->SetCellFormula(address, CellFormulaValue::Shared("A1", 1, "C1:C2")));
        CHECK_FALSE(sheet->SetCellFormula(address, CellFormulaValue::Array("A1", "bad")));
        auto invalidCache = CellFormulaValue::Normal("1+1");
        invalidCache.CachedText = "2";
        CHECK_FALSE(sheet->SetCellFormula(address, invalidCache));
        CHECK_FALSE(sheet->ContainsCell(address));
    }

    TEST_CASE("Replacing a formula changes text without evaluating it [unit] [excel] [excel-formula]")
    {
        auto editor = ExcelDocumentEditor::CreateNew();
        auto sheet = editor->FirstWorksheet();
        const auto address = ExcelFormulaTestHelpers::Address("A1");
        REQUIRE(sheet->SetCellFormula(address, "1+1", FormulaCachedValueKind::Number, "2"));
        REQUIRE(sheet->SetCellFormula(address, "10/0"));
        const auto formula = sheet->GetCellFormula(address);
        REQUIRE(formula);
        CHECK(formula->Formula == "10/0");
        CHECK(formula->CachedKind == FormulaCachedValueKind::None);
        CHECK(formula->CachedText.empty());
        CHECK(sheet->GetPart()->GetXmlString().find("<x:v>") == std::string::npos);
    }

    TEST_CASE("Complete formula models survive package round trip [unit] [excel] [excel-formula]")
    {
        auto editor = ExcelDocumentEditor::CreateNew();
        auto sheet = editor->FirstWorksheet();
        REQUIRE(sheet->SetCellFormula(ExcelFormulaTestHelpers::Address("A1"),
                                      CellFormulaValue::Array("MMULT(B1:C2,D1:E2)", "A1:A2",
                                                              FormulaCachedValueKind::Error, "#N/A", true,
                                                              FormulaReferenceStyle::R1C1)));
        const auto bytes = editor->SaveToMemory();
        REQUIRE_FALSE(bytes.empty());
        auto reopened = ExcelDocumentEditor::Open(bytes);
        REQUIRE(reopened);
        const auto formula = reopened->FirstWorksheet()->GetCellFormula(ExcelFormulaTestHelpers::Address("A1"));
        REQUIRE(formula);
        CHECK(formula->Formula == "MMULT(B1:C2,D1:E2)");
        CHECK(formula->Kind == CellFormulaKind::Array);
        CHECK(formula->Reference == "A1:A2");
        CHECK(formula->AlwaysCalculateArray);
        CHECK(formula->ReferenceStyle == FormulaReferenceStyle::R1C1);
        CHECK(formula->CachedKind == FormulaCachedValueKind::Error);
        CHECK(formula->CachedText == "#N/A");
    }

    TEST_CASE("Formula reads distinguish missing and non-formula cells [unit] [excel] [excel-formula]")
    {
        auto editor = ExcelDocumentEditor::CreateNew();
        auto sheet = editor->FirstWorksheet();
        CHECK_FALSE(sheet->GetCellFormula(ExcelFormulaTestHelpers::Address("A1")));
        REQUIRE(sheet->SetCellNumber(ExcelFormulaTestHelpers::Address("A1"), 42));
        CHECK_FALSE(sheet->GetCellFormula(ExcelFormulaTestHelpers::Address("A1")));
        Worksheet detached;
        CHECK_FALSE(detached.GetCellFormula(ExcelFormulaTestHelpers::Address("A1")));
    }
}
