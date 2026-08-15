// Copyright (c) 2026 Jakub Melka and Contributors
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "doctest.h"

#include "ExyokiOffice/Excel/ExcelDocument.hpp"
#include "ExyokiOffice/Excel/ExcelReference.hpp"

using namespace ExyokiOffice::Excel;

namespace
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
} // namespace

TEST_SUITE("ExcelReferenceTests")
{
    TEST_CASE("Insert and delete transformations preserve absolute and mixed "
              "markers [unit] [excel] [excel-reference] [excel-address]")
    {
        struct Case
        {
            FormulaReferenceTransform transform;
            const char* input;
            const char* expected;
        };
        const Case cases[] = {
            {FormulaReferenceTransform::InsertRows(2, 3), "A1+A2+$B2+C$2+$D$2",
             "A1+A5+$B5+C$5+$D$5"},
            {FormulaReferenceTransform::InsertColumns(2, 2), "A1+B$2+$B3+$B$4",
             "A1+D$2+$D3+$D$4"},
            {FormulaReferenceTransform::DeleteRows(2, 2), "A1+A2+A3+A4",
             "A1+#REF!+#REF!+A2"},
            {FormulaReferenceTransform::DeleteColumns(2, 2), "A1+B1+C1+D1",
             "A1+#REF!+#REF!+B1"},
            {FormulaReferenceTransform::DeleteRows(3, 3), "SUM(A1:A10)+A3:A5",
             "SUM(A1:A7)+#REF!"},
            {FormulaReferenceTransform::DeleteColumns(3, 2), "SUM(A1:F1)",
             "SUM(A1:D1)"},
        };
        for (const auto& test : cases)
        {
            INFO(test.input);
            const auto result = FormulaReferenceRewriter::RewriteA1(
                test.input, "Sheet1", test.transform);
            REQUIRE(result);
            CHECK(result.Formula == test.expected);
        }
    }

    TEST_CASE("Only unqualified and current-sheet references are rewritten "
              "[unit] [excel] [excel-reference] [excel-address]")
    {
        const auto result = FormulaReferenceRewriter::RewriteA1(
            "A2+Sheet1!B2+'Sheet 1'!C2+'Bob''s Sheet'!D2+Other!E2", "Sheet 1",
            FormulaReferenceTransform::InsertRows(2, 1));
        REQUIRE(result);
        CHECK(result.Formula ==
              "A3+Sheet1!B2+'Sheet 1'!C3+'Bob''s Sheet'!D2+Other!E2");
        CHECK(result.Diagnostics.empty());
    }

    TEST_CASE("External references are preserved and diagnosed [unit] [excel] "
              "[excel-reference] [excel-address]")
    {
        const auto result = FormulaReferenceRewriter::RewriteA1(
            "A2+[Book.xlsx]Sheet1!B2+'[Other Book.xlsx]Data'!C2", "Sheet1",
            FormulaReferenceTransform::InsertRows(2, 1));
        REQUIRE(result);
        CHECK(result.Formula ==
              "A3+[Book.xlsx]Sheet1!B2+'[Other Book.xlsx]Data'!C2");
        REQUIRE(result.Diagnostics.size() == 2);
        CHECK(result.Diagnostics[0].Token == "[Book.xlsx]Sheet1!B2");
        CHECK_FALSE(result.Diagnostics[0].Message.empty());
    }

    TEST_CASE("Strings names structured references and R1C1 expressions remain "
              "intact [unit] [excel] [excel-reference] [excel-address]")
    {
        const auto result = FormulaReferenceRewriter::RewriteA1(
            "\"A2 and \"\"B2\"\"\"+A2Name+Table1[A2]+R[2]C[1]+LOG10(A2)", "Sheet1",
            FormulaReferenceTransform::InsertRows(2, 1));
        REQUIRE(result);
        CHECK(result.Formula ==
              "\"A2 and \"\"B2\"\"\"+A2Name+Table1[A2]+R[2]C[1]+LOG10(A3)");
        CHECK(result.RewrittenReferenceCount == 1);
    }

    TEST_CASE("MoveRange retargets references inside the moved rectangle [unit] "
              "[excel] [excel-reference] [excel-address]")
    {
        const auto transform =
            FormulaReferenceTransform::MoveRange(Range("B2:D4"), Address("F6"));
        const auto result = FormulaReferenceRewriter::RewriteA1(
            "A1+B2+$C3+D$4+E5+B2:D4+Other!B2", "Sheet1", transform);
        REQUIRE(result);
        CHECK(result.Formula == "A1+F6+$G7+H$8+E5+F6:H8+Other!B2");
        CHECK(result.RewrittenReferenceCount == 4);
    }

    TEST_CASE("Worksheet MoveRange applies the rewriter atomically to stored "
              "formulas [unit] [excel] [excel-reference] [excel-address]")
    {
        auto editor = ExcelDocumentEditor::CreateNew();
        auto sheet = editor->FirstWorksheet();
        REQUIRE(sheet);
        REQUIRE(sheet->SetCellNumber(Address("B2"), 10));
        REQUIRE(sheet->SetCellFormula(Address("E5"), "B2+$B$2+Other!B2",
                                      FormulaCachedValueKind::Number, "20"));

        const auto status = sheet->MoveRange(Range("B2:C3"), Address("F6"));
        REQUIRE(status);
        CHECK_FALSE(sheet->ContainsCell(Address("B2")));
        CHECK(sheet->ContainsCell(Address("F6")));
        const auto formula = sheet->GetCellFormula(Address("E5"));
        REQUIRE(formula);
        CHECK(formula->Formula == "F6+$F$6+Other!B2");
        CHECK(formula->CachedText == "20");
    }

    TEST_CASE("Invalid and overflowing transformations return structured "
              "failures [unit] [excel] [excel-reference] [excel-address]")
    {
        auto result = FormulaReferenceRewriter::RewriteA1(
            "A1", "Sheet1", FormulaReferenceTransform::InsertRows(0, 1));
        CHECK_FALSE(result);
        CHECK_FALSE(result.ErrorMessage.empty());

        result = FormulaReferenceRewriter::RewriteA1(
            "XFD1", "Sheet1", FormulaReferenceTransform::InsertColumns(1, 1));
        CHECK_FALSE(result);
        CHECK_FALSE(result.ErrorMessage.empty());

        result = FormulaReferenceRewriter::RewriteA1(
            "A1", "Sheet1",
            FormulaReferenceTransform::MoveRange(Range("A1:B2"),
                                                 Address("XFD1048576")));
        CHECK_FALSE(result);
    }
}
