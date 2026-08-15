// Copyright (c) 2026 Jakub Melka and Contributors
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "doctest.h"

#include "ExyokiOffice/Excel/ExcelDocument.hpp"

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

std::vector<ExcelTableColumn> BasicColumns()
{
    return {{0, "Product"}, {0, "Quantity"}, {0, "Amount"}};
}
} // namespace

TEST_SUITE("ExcelTableTests")
{
    TEST_CASE("CreateTable attaches definition relationship and worksheet "
              "registry [unit] [excel] [excel-table]")
    {
        auto editor = ExcelDocumentEditor::CreateNew();
        auto sheet = editor->FirstWorksheet();
        REQUIRE(sheet);
        const auto table =
            sheet->CreateTable("Sales_Table", Range("A1:C5"), BasicColumns());
        REQUIRE(table);
        CHECK(table->Id() == 1);
        CHECK(table->Name() == "Sales_Table");
        REQUIRE(table->Range());
        CHECK(table->Range()->ToA1() == "A1:C5");
        CHECK(table->AutoFilterEnabled());
        CHECK_FALSE(table->TotalsRowShown());
        CHECK(table->GetPart()->RelationshipId().empty() == false);
        CHECK(sheet->GetPart()->GetTableDefinitionParts().size() == 1);
        const auto worksheetXml = sheet->GetPart()->GetXmlString();
        CHECK(worksheetXml.find("tableParts count=\"1\"") != std::string::npos);
        CHECK(worksheetXml.find(table->GetPart()->RelationshipId()) !=
              std::string::npos);
    }

    TEST_CASE("Column CRUD preserves formulas totals and stable IDs [unit] "
              "[excel] [excel-table]")
    {
        auto sheet = ExcelDocumentEditor::CreateNew()->FirstWorksheet();
        REQUIRE(sheet);
        auto columns = BasicColumns();
        columns[0].Id = 7;
        columns[1].CalculatedColumnFormula = "[@Quantity]*2";
        columns[2].TotalsFunction = TableTotalsFunction::Custom;
        columns[2].TotalsRowFormula = "SUBTOTAL(109,[Amount])";
        columns[2].TotalsRowLabel = "Grand total";
        const auto table = sheet->CreateTable("Orders", Range("B2:D10"), columns);
        REQUIRE(table);
        const auto read = table->Columns();
        REQUIRE(read.size() == 3);
        CHECK(read[0].Id == 7);
        CHECK(read[1].Id != 0);
        CHECK(read[1].CalculatedColumnFormula ==
              columns[1].CalculatedColumnFormula);
        CHECK(read[2].TotalsFunction == TableTotalsFunction::Custom);
        CHECK(read[2].TotalsRowFormula == columns[2].TotalsRowFormula);
        CHECK(read[2].TotalsRowLabel == columns[2].TotalsRowLabel);

        auto replacement = read;
        replacement[1].Name = "Units";
        replacement[1].CalculatedColumnFormula = "[@Units]+1";
        REQUIRE(table->SetColumns(replacement));
        CHECK(table->Columns()[1].Name == "Units");
        CHECK(table->Columns()[1].CalculatedColumnFormula == "[@Units]+1");
    }

    TEST_CASE("Resize synchronizes table and auto-filter references [unit] "
              "[excel] [excel-table]")
    {
        auto sheet = ExcelDocumentEditor::CreateNew()->FirstWorksheet();
        REQUIRE(sheet);
        const auto table =
            sheet->CreateTable("Inventory", Range("A1:C4"), BasicColumns());
        REQUIRE(table);
        REQUIRE(table->Resize(Range("D3:F20")));
        REQUIRE(table->Range());
        CHECK(table->Range()->ToA1() == "D3:F20");
        auto xml = table->GetPart()->GetXmlString();
        CHECK(xml.find("ref=\"D3:F20\"") != std::string::npos);
        CHECK(xml.find("autoFilter") != std::string::npos);

        REQUIRE(table->SetAutoFilterEnabled(false));
        CHECK_FALSE(table->AutoFilterEnabled());
        CHECK(table->GetPart()->GetXmlString().find("autoFilter") ==
              std::string::npos);
        REQUIRE(table->SetAutoFilterEnabled(true));
        CHECK(table->GetPart()->GetXmlString().find("ref=\"D3:F20\"") !=
              std::string::npos);
        CHECK_FALSE(table->Resize(Range("A1:B2")));

        auto widerColumns = BasicColumns();
        widerColumns.push_back({0, "Warehouse"});
        REQUIRE(table->SetValueFilter({2, {"100"}, false}));
        REQUIRE(table->Resize(Range("A1:D8"), widerColumns));
        CHECK(table->Columns().size() == 4);
        CHECK(table->ValueFilters().size() == 1);
        REQUIRE(table->Resize(Range("A1:B8"), {{0, "Product"}, {0, "Quantity"}}));
        CHECK(table->Columns().size() == 2);
        CHECK(table->ValueFilters().empty());
    }

    TEST_CASE("Value filter CRUD validates columns and criteria [unit] [excel] "
              "[excel-table]")
    {
        auto sheet = ExcelDocumentEditor::CreateNew()->FirstWorksheet();
        REQUIRE(sheet);
        const auto table =
            sheet->CreateTable("Filtered", Range("A1:C10"), BasicColumns());
        REQUIRE(table);
        REQUIRE(table->SetValueFilter({1, {"10", "20"}, true}));
        auto filters = table->ValueFilters();
        REQUIRE(filters.size() == 1);
        CHECK(filters[0].ColumnIndex == 1);
        CHECK(filters[0].Values == std::vector<std::string>{"10", "20"});
        CHECK(filters[0].IncludeBlank);

        REQUIRE(table->SetValueFilter({1, {"30"}, false}));
        filters = table->ValueFilters();
        REQUIRE(filters.size() == 1);
        CHECK(filters[0].Values == std::vector<std::string>{"30"});
        CHECK_FALSE(table->SetValueFilter({3, {"x"}, false}));
        CHECK_FALSE(table->SetValueFilter({0, {}, false}));
        CHECK_FALSE(table->SetValueFilter({0, {"x", "x"}, false}));
        REQUIRE(table->RemoveValueFilter(1));
        CHECK(table->ValueFilters().empty());
        REQUIRE(table->SetValueFilter({0, {"Product A"}, false}));
        REQUIRE(table->SetValueFilter({2, {}, true}));
        REQUIRE(table->ClearValueFilters());
        CHECK(table->ValueFilters().empty());
        CHECK(table->AutoFilterEnabled());
    }

    TEST_CASE("Totals row visibility validates table height and retains metadata "
              "[unit] [excel] [excel-table]")
    {
        auto sheet = ExcelDocumentEditor::CreateNew()->FirstWorksheet();
        REQUIRE(sheet);
        auto columns = BasicColumns();
        columns[2].TotalsFunction = TableTotalsFunction::Sum;
        const auto table = sheet->CreateTable("Totals", Range("A1:C2"), columns);
        REQUIRE(table);
        REQUIRE(table->SetTotalsRowShown(true));
        CHECK(table->TotalsRowShown());
        CHECK(table->GetPart()->GetXmlString().find("totalsRowCount=\"1\"") !=
              std::string::npos);
        CHECK_FALSE(table->Resize(Range("A1:C1")));
        REQUIRE(table->SetTotalsRowShown(false));
        CHECK(table->Columns()[2].TotalsFunction == TableTotalsFunction::Sum);
    }

    TEST_CASE("Table validation rejects invalid names columns and workbook "
              "duplicates [unit] [excel] [excel-table]")
    {
        CHECK(IsValidExcelTableName("Table_1"));
        CHECK_FALSE(IsValidExcelTableName("A1"));
        CHECK_FALSE(IsValidExcelTableName("R1C1"));
        CHECK_FALSE(IsValidExcelTableName("1Table"));
        CHECK_FALSE(IsValidExcelTableName("Has Space"));

        // A table name shares its name space with a defined name, so it obeys
        // the same rule: letters in any script are allowed. Spelled as escapes
        // because the compiler is not told the source encoding.
        CHECK(IsValidExcelTableName("P\xC5\x99"
                                    "ehled")); // r with caron
        CHECK(IsValidExcelTableName("\xC4\x8C"
                                    "esko"));                     // C with caron
        CHECK(IsValidExcelTableName("\xE5\xA3\xB2\xE4\xB8\x8A")); // CJK
        // What the rule forbids is forbidden in any script.
        CHECK_FALSE(IsValidExcelTableName("P\xC5\x99"
                                          "ehled tabulky"));

        auto editor = ExcelDocumentEditor::CreateNew();
        auto first = editor->FirstWorksheet();
        auto second = editor->AddWorksheet("Data");
        REQUIRE(first);
        REQUIRE(second);
        REQUIRE(first->CreateTable("Sales", Range("A1:C4"), BasicColumns()));
        CHECK_FALSE(second->CreateTable("sales", Range("A1:C4"), BasicColumns()));
        CHECK_FALSE(first->CreateTable("Bad Width", Range("A1:C4"), {{0, "One"}}));

        auto duplicateColumns = BasicColumns();
        duplicateColumns[1].Name = "product";
        CHECK_FALSE(
            first->CreateTable("Another", Range("E1:G4"), duplicateColumns));
        CHECK(first->Tables().size() == 1);
    }

    TEST_CASE("Rename and remove enforce ownership and preserve worksheet cells "
              "[unit] [excel] [excel-table]")
    {
        auto editor = ExcelDocumentEditor::CreateNew();
        auto first = editor->FirstWorksheet();
        auto second = editor->AddWorksheet("Other");
        REQUIRE(first);
        REQUIRE(second);
        REQUIRE(first->SetCellText(Address("A1"), "Product"));
        const auto table =
            first->CreateTable("Before", Range("A1:C3"), BasicColumns());
        const auto other =
            second->CreateTable("Reserved", Range("A1:C3"), BasicColumns());
        REQUIRE(table);
        REQUIRE(other);
        REQUIRE(first->RenameTable(table, "After"));
        const auto renamed = first->TableByName("after");
        REQUIRE(renamed);
        CHECK(renamed->Name() == "After");
        CHECK_FALSE(first->RenameTable(table, "reserved"));
        CHECK_FALSE(second->RemoveTable(table));
        REQUIRE(first->RemoveTable(table));
        CHECK(first->Tables().empty());
        CHECK(first->ContainsCell(Address("A1")));
        CHECK(first->GetPart()->GetXmlString().find("tableParts") ==
              std::string::npos);
    }

    TEST_CASE("Tables and metadata survive package round trip [unit] [excel] "
              "[excel-table]")
    {
        auto editor = ExcelDocumentEditor::CreateNew();
        auto sheet = editor->FirstWorksheet();
        REQUIRE(sheet);
        auto columns = BasicColumns();
        columns[2].CalculatedColumnFormula = "[@Quantity]*10";
        columns[2].TotalsFunction = TableTotalsFunction::Sum;
        const auto table = sheet->CreateTable("RoundTrip", Range("A1:C8"), columns);
        REQUIRE(table);
        REQUIRE(table->SetTotalsRowShown(true));
        REQUIRE(table->Resize(Range("B2:D12")));

        const auto bytes = editor->SaveToMemory();
        REQUIRE_FALSE(bytes.empty());
        auto reopened = ExcelDocumentEditor::Open(bytes);
        REQUIRE(reopened);
        const auto reopenedTable =
            reopened->FirstWorksheet()->TableByName("roundtrip");
        REQUIRE(reopenedTable);
        REQUIRE(reopenedTable->Range());
        CHECK(reopenedTable->Range()->ToA1() == "B2:D12");
        CHECK(reopenedTable->AutoFilterEnabled());
        CHECK(reopenedTable->TotalsRowShown());
        CHECK(reopenedTable->Columns()[2].CalculatedColumnFormula ==
              "[@Quantity]*10");
        CHECK(reopenedTable->Columns()[2].TotalsFunction ==
              TableTotalsFunction::Sum);
    }
}
