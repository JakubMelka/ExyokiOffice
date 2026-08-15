// Copyright (c) 2026 Jakub Melka and Contributors
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "doctest.h"

#include "ExyokiOffice/Excel/ExcelDocument.hpp"

using ExyokiOffice::Excel::CellAddress;
using ExyokiOffice::Excel::CellValueKind;
using ExyokiOffice::Excel::ExcelCellValue;
using ExyokiOffice::Excel::ExcelDocumentEditor;
using ExyokiOffice::Excel::FormulaCachedValueKind;

namespace
{

CellAddress Address(std::string_view text)
{
    auto address = CellAddress::ParseA1(text);
    if (!address)
    {
        FAIL("Invalid test cell address");
        return {};
    }
    return *address;
}

ExcelCellValue RequireValue(const ExyokiOffice::Excel::Worksheet::Ptr& sheet, std::string_view address)
{
    auto value = sheet->GetCellValue(Address(address));
    if (!value)
    {
        FAIL("Expected cell value");
        return {};
    }
    return *value;
}

} // namespace

TEST_SUITE("ExcelCellValueTests")
{

    TEST_CASE("ExcelCellValue factories preserve type and text [unit] [excel] [excel-cell-value]")
    {
        auto blank = ExcelCellValue::Blank();
        CHECK(blank.Kind() == CellValueKind::Blank);
        CHECK(blank.IsBlank());

        auto inlineString = ExcelCellValue::InlineString("plain text");
        CHECK(inlineString.Kind() == CellValueKind::InlineString);
        CHECK(inlineString.Text() == "plain text");

        auto shared = ExcelCellValue::SharedString(42);
        CHECK(shared.Kind() == CellValueKind::SharedString);
        REQUIRE(shared.SharedStringIndex());
        CHECK(*shared.SharedStringIndex() == 42);
        CHECK(shared.Text() == "42");

        auto number = ExcelCellValue::NumberText("001.2300");
        CHECK(number.Kind() == CellValueKind::Number);
        CHECK(number.Text() == "001.2300");

        auto boolean = ExcelCellValue::Boolean(true);
        CHECK(boolean.Kind() == CellValueKind::Boolean);
        REQUIRE(boolean.BooleanValue());
        CHECK(*boolean.BooleanValue());
        CHECK(boolean.Text() == "1");

        auto error = ExcelCellValue::Error("#DIV/0!");
        CHECK(error.Kind() == CellValueKind::Error);
        CHECK(error.Text() == "#DIV/0!");

        auto date = ExcelCellValue::DateTimeText("2026-07-09T18:30:00Z");
        CHECK(date.Kind() == CellValueKind::DateTime);
        CHECK(date.Text() == "2026-07-09T18:30:00Z");

        auto formula = ExcelCellValue::Formula("=SUM(A1:A2)", FormulaCachedValueKind::Number, "15");
        CHECK(formula.Kind() == CellValueKind::Formula);
        CHECK(formula.FormulaValue().Formula == "SUM(A1:A2)");
        CHECK(formula.FormulaValue().CachedKind == FormulaCachedValueKind::Number);
        CHECK(formula.FormulaValue().CachedText == "15");
    }

    TEST_CASE("Worksheet writes and reads all supported cell value kinds [unit] [excel] [excel-cell-value]")
    {
        auto editor = ExcelDocumentEditor::CreateNew();
        REQUIRE(editor != nullptr);
        auto sheet = editor->FirstWorksheet();
        REQUIRE(sheet != nullptr);

        CHECK(sheet->SetCellValue(Address("A1"), ExcelCellValue::Blank()));
        CHECK(sheet->SetCellValue(Address("A2"), ExcelCellValue::InlineString("inline")));
        CHECK(sheet->SetCellValue(Address("A3"), ExcelCellValue::SharedString(7)));
        CHECK(sheet->SetCellValue(Address("A4"), ExcelCellValue::NumberText("001.2500")));
        CHECK(sheet->SetCellBoolean(Address("A5"), false));
        CHECK(sheet->SetCellError(Address("A6"), "#N/A"));
        CHECK(sheet->SetCellDateTimeText(Address("A7"), "2026-07-09T18:30:00Z"));
        CHECK(sheet->SetCellFormula(Address("A8"), "=A4*2", FormulaCachedValueKind::Number, "2.5000"));
        CHECK(sheet->SetCellFormula(Address("A9"), "A2", FormulaCachedValueKind::String, "cached text"));

        CHECK(RequireValue(sheet, "A1").Kind() == CellValueKind::Blank);
        CHECK(RequireValue(sheet, "A2").Kind() == CellValueKind::InlineString);
        CHECK(RequireValue(sheet, "A2").Text() == "inline");
        auto shared = RequireValue(sheet, "A3");
        CHECK(shared.Kind() == CellValueKind::SharedString);
        REQUIRE(shared.SharedStringIndex());
        CHECK(*shared.SharedStringIndex() == 7);
        CHECK(RequireValue(sheet, "A4").Kind() == CellValueKind::Number);
        CHECK(RequireValue(sheet, "A4").Text() == "001.2500");
        auto boolean = RequireValue(sheet, "A5");
        CHECK(boolean.Kind() == CellValueKind::Boolean);
        REQUIRE(boolean.BooleanValue());
        CHECK_FALSE(*boolean.BooleanValue());
        CHECK(RequireValue(sheet, "A6").Kind() == CellValueKind::Error);
        CHECK(RequireValue(sheet, "A6").Text() == "#N/A");
        CHECK(RequireValue(sheet, "A7").Kind() == CellValueKind::DateTime);
        CHECK(RequireValue(sheet, "A7").Text() == "2026-07-09T18:30:00Z");

        auto formulaNumber = RequireValue(sheet, "A8");
        CHECK(formulaNumber.Kind() == CellValueKind::Formula);
        CHECK(formulaNumber.FormulaValue().Formula == "A4*2");
        CHECK(formulaNumber.FormulaValue().CachedKind == FormulaCachedValueKind::Number);
        CHECK(formulaNumber.FormulaValue().CachedText == "2.5000");

        auto formulaString = RequireValue(sheet, "A9");
        CHECK(formulaString.Kind() == CellValueKind::Formula);
        CHECK(formulaString.FormulaValue().Formula == "A2");
        CHECK(formulaString.FormulaValue().CachedKind == FormulaCachedValueKind::String);
        CHECK(formulaString.FormulaValue().CachedText == "cached text");

        const auto xml = sheet->GetPart()->GetXmlString();
        CHECK(xml.find("t=\"inlineStr\"") != std::string::npos);
        CHECK(xml.find("t=\"s\"") != std::string::npos);
        CHECK(xml.find("t=\"n\"") != std::string::npos);
        CHECK(xml.find("t=\"b\"") != std::string::npos);
        CHECK(xml.find("t=\"e\"") != std::string::npos);
        CHECK(xml.find("t=\"d\"") != std::string::npos);
        CHECK(xml.find("<x:f>A4*2</x:f>") != std::string::npos);
        CHECK(xml.find("<x:v>2.5000</x:v>") != std::string::npos);
    }

    TEST_CASE("Worksheet cell values preserve type and text through package round trip [unit] [excel] [excel-cell-value]")
    {
        auto editor = ExcelDocumentEditor::CreateNew();
        REQUIRE(editor != nullptr);
        auto sheet = editor->FirstWorksheet();
        REQUIRE(sheet != nullptr);

        CHECK(sheet->SetCellValue(Address("B1"), ExcelCellValue::InlineString("round trip")));
        CHECK(sheet->SetCellValue(Address("B2"), ExcelCellValue::SharedString(3)));
        CHECK(sheet->SetCellValue(Address("B3"), ExcelCellValue::NumberText("42.500")));
        CHECK(sheet->SetCellBoolean(Address("B4"), true));
        CHECK(sheet->SetCellError(Address("B5"), "#VALUE!"));
        CHECK(sheet->SetCellDateTimeText(Address("B6"), "2026-07-09T20:00:00Z"));
        CHECK(sheet->SetCellFormula(Address("B7"), "B3+B3", FormulaCachedValueKind::Number, "85.000"));

        auto bytes = editor->SaveToMemory();
        REQUIRE(!bytes.empty());
        auto reopened = ExcelDocumentEditor::Open(bytes);
        REQUIRE(reopened != nullptr);
        auto reopenedSheet = reopened->FirstWorksheet();
        REQUIRE(reopenedSheet != nullptr);

        CHECK(RequireValue(reopenedSheet, "B1").Kind() == CellValueKind::InlineString);
        CHECK(RequireValue(reopenedSheet, "B1").Text() == "round trip");
        auto shared = RequireValue(reopenedSheet, "B2");
        CHECK(shared.Kind() == CellValueKind::SharedString);
        REQUIRE(shared.SharedStringIndex());
        CHECK(*shared.SharedStringIndex() == 3);
        CHECK(RequireValue(reopenedSheet, "B3").Kind() == CellValueKind::Number);
        CHECK(RequireValue(reopenedSheet, "B3").Text() == "42.500");
        auto boolean = RequireValue(reopenedSheet, "B4");
        CHECK(boolean.Kind() == CellValueKind::Boolean);
        REQUIRE(boolean.BooleanValue());
        CHECK(*boolean.BooleanValue());
        CHECK(RequireValue(reopenedSheet, "B5").Kind() == CellValueKind::Error);
        CHECK(RequireValue(reopenedSheet, "B5").Text() == "#VALUE!");
        CHECK(RequireValue(reopenedSheet, "B6").Kind() == CellValueKind::DateTime);
        CHECK(RequireValue(reopenedSheet, "B6").Text() == "2026-07-09T20:00:00Z");
        auto formula = RequireValue(reopenedSheet, "B7");
        CHECK(formula.Kind() == CellValueKind::Formula);
        CHECK(formula.FormulaValue().Formula == "B3+B3");
        CHECK(formula.FormulaValue().CachedKind == FormulaCachedValueKind::Number);
        CHECK(formula.FormulaValue().CachedText == "85.000");
    }

    TEST_CASE("Worksheet value writes reject invalid addresses and invalid wrappers [unit] [excel] [excel-cell-value]")
    {
        ExyokiOffice::Excel::Worksheet detached;
        CHECK_FALSE(detached.SetCellValue(Address("A1"), ExcelCellValue::InlineString("missing part")));
        CHECK_FALSE(detached.GetCellValue(Address("A1")));

        auto editor = ExcelDocumentEditor::CreateNew();
        REQUIRE(editor != nullptr);
        auto sheet = editor->FirstWorksheet();
        REQUIRE(sheet != nullptr);

        CHECK_FALSE(sheet->SetCellValue(0, 1, ExcelCellValue::NumberText("1")));
        CHECK_FALSE(sheet->GetCellValue(1, 0));
    }

} // namespace
