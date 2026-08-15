// Copyright (c) 2026 Jakub Melka and Contributors
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "doctest.h"

#include "ExyokiOffice/Excel/ExcelDocument.hpp"
#include "ExyokiOffice/StandardTypes.hpp"

using ExyokiOffice::Excel::CellAddress;
using ExyokiOffice::Excel::CellValueKind;
using ExyokiOffice::Excel::ExcelCellValue;
using ExyokiOffice::Excel::ExcelDocumentEditor;
using ExyokiOffice::Excel::FormulaCachedValueKind;
using ExyokiOffice::Excel::FormulaReferenceUpdatePolicy;
using ExyokiOffice::Excel::RangeOperationError;
using ExyokiOffice::Excel::Worksheet;

class ExcelStructuralEditTestHelpers final
{
public:
    ExcelStructuralEditTestHelpers() = delete;

    static CellAddress Address(std::string_view text)
    {
        const auto address = CellAddress::ParseA1(text);
        REQUIRE(address.has_value());
        return *address;
    }

    static ExcelCellValue Value(const Worksheet::Ptr& worksheet, std::string_view address)
    {
        const auto value = worksheet->GetCellValue(Address(address));
        REQUIRE(value.has_value());
        return *value;
    }
};

TEST_SUITE("ExcelStructuralEditTests")
{
    TEST_CASE("InsertRows moves sparse cells and rewrites basic local formulas [unit] [excel] [excel-structure]")
    {
        auto editor = ExcelDocumentEditor::CreateNew();
        auto sheet = editor->FirstWorksheet();
        REQUIRE(sheet);
        CHECK(sheet->SetCellNumber(ExcelStructuralEditTestHelpers::Address("A1"), 1));
        CHECK(sheet->SetCellFormula(ExcelStructuralEditTestHelpers::Address("B2"),
                                    "A1+$B$2+\"A1\"+Sheet2!A1+Table1[A1]",
                                    FormulaCachedValueKind::Number,
                                    "3"));
        CHECK(sheet->SetCellNumber(ExcelStructuralEditTestHelpers::Address("C5"), 5));

        const auto result = sheet->InsertRows(2, 2);
        REQUIRE(result);
        CHECK(sheet->StoredCellCount() == 3);
        CHECK_FALSE(sheet->ContainsCell(ExcelStructuralEditTestHelpers::Address("B2")));
        CHECK(sheet->ContainsCell(ExcelStructuralEditTestHelpers::Address("B4")));
        CHECK(sheet->ContainsCell(ExcelStructuralEditTestHelpers::Address("C7")));
        const auto formula = ExcelStructuralEditTestHelpers::Value(sheet, "B4");
        REQUIRE(formula.Kind() == CellValueKind::Formula);
        CHECK(formula.FormulaValue().Formula == "A1+$B$4+\"A1\"+Sheet2!A1+Table1[A1]");
        CHECK(formula.FormulaValue().CachedText == "3");
    }

    TEST_CASE("DeleteRows removes cells contracts ranges and produces REF for deleted scalar references [unit] [excel] [excel-structure]")
    {
        auto editor = ExcelDocumentEditor::CreateNew();
        auto sheet = editor->FirstWorksheet();
        REQUIRE(sheet);
        for (ExyokiOffice::UInt32 row = 1; row <= 5; ++row)
        {
            CHECK(sheet->SetCellNumber(row, 1, row));
        }
        CHECK(sheet->SetCellFormula(ExcelStructuralEditTestHelpers::Address("C6"), "SUM(A1:A5)+A3"));

        const auto result = sheet->DeleteRows(2, 2);
        REQUIRE(result);
        CHECK(ExcelStructuralEditTestHelpers::Value(sheet, "A1").Text() == "1");
        CHECK(ExcelStructuralEditTestHelpers::Value(sheet, "A2").Text() == "4");
        CHECK(ExcelStructuralEditTestHelpers::Value(sheet, "A3").Text() == "5");
        CHECK_FALSE(sheet->ContainsCell(ExcelStructuralEditTestHelpers::Address("A4")));
        const auto formula = ExcelStructuralEditTestHelpers::Value(sheet, "C4");
        REQUIRE(formula.Kind() == CellValueKind::Formula);
        CHECK(formula.FormulaValue().Formula == "SUM(A1:A3)+#REF!");
    }

    TEST_CASE("Column insertion and deletion update cells and formulas [unit] [excel] [excel-structure]")
    {
        auto editor = ExcelDocumentEditor::CreateNew();
        auto sheet = editor->FirstWorksheet();
        REQUIRE(sheet);
        CHECK(sheet->SetCellNumber(ExcelStructuralEditTestHelpers::Address("A1"), 1));
        CHECK(sheet->SetCellNumber(ExcelStructuralEditTestHelpers::Address("B1"), 2));
        CHECK(sheet->SetCellFormula(ExcelStructuralEditTestHelpers::Address("D1"), "SUM(A1:B1)+B1"));

        REQUIRE(sheet->InsertColumns(2, 2));
        CHECK(sheet->ContainsCell(ExcelStructuralEditTestHelpers::Address("D1")));
        CHECK(sheet->ContainsCell(ExcelStructuralEditTestHelpers::Address("F1")));
        CHECK(ExcelStructuralEditTestHelpers::Value(sheet, "F1").FormulaValue().Formula == "SUM(A1:D1)+D1");

        REQUIRE(sheet->DeleteColumns(2, 2));
        CHECK(sheet->ContainsCell(ExcelStructuralEditTestHelpers::Address("B1")));
        CHECK(sheet->ContainsCell(ExcelStructuralEditTestHelpers::Address("D1")));
        CHECK(ExcelStructuralEditTestHelpers::Value(sheet, "D1").FormulaValue().Formula == "SUM(A1:B1)+B1");
    }

    TEST_CASE("Structural edits update dimensions merges filters and related worksheet ranges [unit] [excel] [excel-structure]")
    {
        auto editor = ExcelDocumentEditor::CreateNew();
        auto sheet = editor->FirstWorksheet();
        REQUIRE(sheet);
        sheet->GetPart()->SetXmlString(
            R"(<?xml version="1.0"?><x:worksheet xmlns:x="http://schemas.openxmlformats.org/spreadsheetml/2006/main"><x:dimension ref="A1:D10"/><x:sheetData><x:row r="2"><x:c r="A2" t="n"><x:v>1</x:v></x:c></x:row></x:sheetData><x:autoFilter ref="A1:D5"/><x:mergeCells count="1"><x:mergeCell ref="B2:C4"/></x:mergeCells><x:conditionalFormatting sqref="A2:A4 C2:C4"/><x:dataValidations count="1"><x:dataValidation sqref="D2:D4"/></x:dataValidations><x:hyperlinks><x:hyperlink ref="B4"/></x:hyperlinks><x:ignoredErrors><x:ignoredError sqref="A2:D4"/></x:ignoredErrors></x:worksheet>)");

        REQUIRE(sheet->InsertRows(3, 2));
        const auto xml = sheet->GetPart()->GetXmlString();
        CHECK(xml.find("ref=\"A1:D12\"") != std::string::npos);
        CHECK(xml.find("ref=\"A1:D7\"") != std::string::npos);
        CHECK(xml.find("ref=\"B2:C6\"") != std::string::npos);
        CHECK(xml.find("sqref=\"A2:A6 C2:C6\"") != std::string::npos);
        CHECK(xml.find("sqref=\"D2:D6\"") != std::string::npos);
        CHECK(xml.find("ref=\"B6\"") != std::string::npos);
        CHECK(xml.find("sqref=\"A2:D6\"") != std::string::npos);
    }

    TEST_CASE("Deleting a complete referenced region removes fully deleted range records [unit] [excel] [excel-structure]")
    {
        auto editor = ExcelDocumentEditor::CreateNew();
        auto sheet = editor->FirstWorksheet();
        REQUIRE(sheet);
        sheet->GetPart()->SetXmlString(
            R"(<?xml version="1.0"?><x:worksheet xmlns:x="http://schemas.openxmlformats.org/spreadsheetml/2006/main"><x:sheetData/><x:mergeCells count="2"><x:mergeCell ref="B2:C3"/><x:mergeCell ref="E5:F6"/></x:mergeCells><x:hyperlinks><x:hyperlink ref="B2"/><x:hyperlink ref="E5"/></x:hyperlinks></x:worksheet>)");

        REQUIRE(sheet->DeleteRows(2, 2));
        const auto xml = sheet->GetPart()->GetXmlString();
        CHECK(xml.find("B2:C3") == std::string::npos);
        CHECK(xml.find("ref=\"B2\"") == std::string::npos);
        CHECK(xml.find("ref=\"E3:F4\"") != std::string::npos);
        CHECK(xml.find("ref=\"E3\"") != std::string::npos);
        CHECK(xml.find("count=\"1\"") != std::string::npos);
    }

    TEST_CASE("Formula preservation policy leaves formula text unchanged [unit] [excel] [excel-structure]")
    {
        auto editor = ExcelDocumentEditor::CreateNew();
        auto sheet = editor->FirstWorksheet();
        REQUIRE(sheet);
        CHECK(sheet->SetCellFormula(ExcelStructuralEditTestHelpers::Address("B2"), "A1+B2"));

        REQUIRE(sheet->InsertRows(1, 1, FormulaReferenceUpdatePolicy::PreserveFormulaText));
        const auto formula = ExcelStructuralEditTestHelpers::Value(sheet, "B3");
        CHECK(formula.FormulaValue().Formula == "A1+B2");
    }

    TEST_CASE("Invalid and overflowing structural edits are atomic structured failures [unit] [excel] [excel-structure]")
    {
        auto editor = ExcelDocumentEditor::CreateNew();
        auto sheet = editor->FirstWorksheet();
        REQUIRE(sheet);
        CHECK(sheet->SetCellNumber(ExcelStructuralEditTestHelpers::Address("XFD1048576"), 9));
        const auto originalXml = sheet->GetPart()->GetXmlString();

        auto result = sheet->InsertRows(1, 0);
        CHECK_FALSE(result);
        CHECK(result.Error == RangeOperationError::InvalidCount);
        CHECK(sheet->GetPart()->GetXmlString() == originalXml);

        result = sheet->DeleteColumns(16384, 2);
        CHECK_FALSE(result);
        CHECK(result.Error == RangeOperationError::InvalidAddress);
        CHECK(sheet->GetPart()->GetXmlString() == originalXml);

        result = sheet->InsertColumns(1, 1);
        CHECK_FALSE(result);
        CHECK(result.Error == RangeOperationError::ReferenceUpdateFailed);
        CHECK_FALSE(result.Message.empty());
        CHECK(sheet->GetPart()->GetXmlString() == originalXml);
    }

    TEST_CASE("Structural edits survive package round trip [unit] [excel] [excel-structure]")
    {
        auto editor = ExcelDocumentEditor::CreateNew();
        auto sheet = editor->FirstWorksheet();
        REQUIRE(sheet);
        CHECK(sheet->SetCellText(ExcelStructuralEditTestHelpers::Address("A1"), "header"));
        CHECK(sheet->SetCellFormula(ExcelStructuralEditTestHelpers::Address("C3"), "A1+1"));
        REQUIRE(sheet->InsertRows(2, 3));
        REQUIRE(sheet->InsertColumns(2, 1));

        const auto bytes = editor->SaveToMemory();
        REQUIRE_FALSE(bytes.empty());
        auto reopened = ExcelDocumentEditor::Open(bytes);
        REQUIRE(reopened);
        auto reopenedSheet = reopened->FirstWorksheet();
        REQUIRE(reopenedSheet);
        CHECK(reopenedSheet->ContainsCell(ExcelStructuralEditTestHelpers::Address("A1")));
        CHECK(reopenedSheet->ContainsCell(ExcelStructuralEditTestHelpers::Address("D6")));
        CHECK(ExcelStructuralEditTestHelpers::Value(reopenedSheet, "D6").FormulaValue().Formula == "A1+1");
    }
}
