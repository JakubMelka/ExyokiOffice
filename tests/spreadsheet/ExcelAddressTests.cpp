// Copyright (c) 2026 Jakub Melka and Collaborators
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "doctest.h"

#include "ExyokiOffice/Excel/ExcelAddress.hpp"
#include "ExyokiOffice/Excel/ExcelDocument.hpp"

using ExyokiOffice::Excel::CellAddress;
using ExyokiOffice::Excel::CellRange;
using ExyokiOffice::Excel::ColumnIndex;
using ExyokiOffice::Excel::ExcelDocumentEditor;
using ExyokiOffice::Excel::MaxColumnIndex;
using ExyokiOffice::Excel::MaxRowIndex;
using ExyokiOffice::Excel::RowIndex;

TEST_SUITE("ExcelAddressTests")
{

    TEST_CASE("Excel address indexes validate worksheet grid limits [unit] [excel] [excel-address]")
    {
        CHECK_FALSE(RowIndex::TryCreate(0));
        CHECK(RowIndex::TryCreate(1)->Value() == 1);
        CHECK(RowIndex::TryCreate(MaxRowIndex)->Value() == MaxRowIndex);
        CHECK_FALSE(RowIndex::TryCreate(MaxRowIndex + 1));

        CHECK_FALSE(ColumnIndex::TryCreate(0));
        CHECK(ColumnIndex::TryCreate(1)->ToName() == "A");
        CHECK(ColumnIndex::TryCreate(26)->ToName() == "Z");
        CHECK(ColumnIndex::TryCreate(27)->ToName() == "AA");
        CHECK(ColumnIndex::TryCreate(MaxColumnIndex)->ToName() == "XFD");
        CHECK_FALSE(ColumnIndex::TryCreate(MaxColumnIndex + 1));
    }

    TEST_CASE("Excel column names parse strictly and case-insensitively [unit] [excel] [excel-address]")
    {
        CHECK(ColumnIndex::ParseName("A")->Value() == 1);
        CHECK(ColumnIndex::ParseName("z")->Value() == 26);
        CHECK(ColumnIndex::ParseName("Aa")->Value() == 27);
        CHECK(ColumnIndex::ParseName("XFD")->Value() == MaxColumnIndex);

        CHECK_FALSE(ColumnIndex::ParseName(""));
        CHECK_FALSE(ColumnIndex::ParseName("A1"));
        CHECK_FALSE(ColumnIndex::ParseName("XFE"));
        CHECK_FALSE(ColumnIndex::ParseName("FXSHRXW"));
    }

    TEST_CASE("Excel A1 cell addresses parse and format boundary cells [unit] [excel] [excel-address]")
    {
        auto first = CellAddress::ParseA1("A1");
        REQUIRE(first);
        CHECK(first->Row().Value() == 1);
        CHECK(first->Column().Value() == 1);
        CHECK(first->ToA1() == "A1");
        CHECK(first->ToR1C1() == "R1C1");

        auto last = CellAddress::ParseA1("xfd1048576");
        REQUIRE(last);
        CHECK(last->Row().Value() == MaxRowIndex);
        CHECK(last->Column().Value() == MaxColumnIndex);
        CHECK(last->ToA1() == "XFD1048576");
        CHECK(last->ToR1C1() == "R1048576C16384");

        CHECK(CellAddress::ParseA1("$B$2")->ToA1() == "B2");
        CHECK(CellAddress::ParseA1("C$10")->ToA1() == "C10");
        CHECK(CellAddress::ParseA1("$D11")->ToA1() == "D11");
    }

    TEST_CASE("Excel A1 cell parser rejects invalid input [unit] [excel] [excel-address]")
    {
        CHECK_FALSE(CellAddress::ParseA1(""));
        CHECK_FALSE(CellAddress::ParseA1("A0"));
        CHECK_FALSE(CellAddress::ParseA1("A1048577"));
        CHECK_FALSE(CellAddress::ParseA1("XFE1"));
        CHECK_FALSE(CellAddress::ParseA1("1A"));
        CHECK_FALSE(CellAddress::ParseA1("A"));
        CHECK_FALSE(CellAddress::ParseA1("1"));
        CHECK_FALSE(CellAddress::ParseA1("Sheet1!A1"));
        CHECK_FALSE(CellAddress::ParseA1("A1:B2"));
        CHECK_FALSE(CellAddress::ParseA1("A-1"));
        CHECK_FALSE(CellAddress::ParseA1("A1 "));
    }

    TEST_CASE("Excel R1C1 cell addresses parse absolute coordinates [unit] [excel] [excel-address]")
    {
        auto first = CellAddress::ParseR1C1("R1C1");
        REQUIRE(first);
        CHECK(first->ToA1() == "A1");
        CHECK(first->ToR1C1() == "R1C1");

        auto last = CellAddress::ParseR1C1("r1048576c16384");
        REQUIRE(last);
        CHECK(last->ToA1() == "XFD1048576");

        CHECK_FALSE(CellAddress::ParseR1C1(""));
        CHECK_FALSE(CellAddress::ParseR1C1("R0C1"));
        CHECK_FALSE(CellAddress::ParseR1C1("R1C0"));
        CHECK_FALSE(CellAddress::ParseR1C1("R1048577C1"));
        CHECK_FALSE(CellAddress::ParseR1C1("R1C16385"));
        CHECK_FALSE(CellAddress::ParseR1C1("R[1]C[1]"));
        CHECK_FALSE(CellAddress::ParseR1C1("R1"));
        CHECK_FALSE(CellAddress::ParseR1C1("C1R1"));
    }

    TEST_CASE("Excel ranges parse normalized A1 and R1C1 rectangles [unit] [excel] [excel-address]")
    {
        auto single = CellRange::ParseA1("A1");
        REQUIRE(single);
        CHECK(single->IsValid());
        CHECK(single->RowCount() == 1);
        CHECK(single->ColumnCount() == 1);
        CHECK(single->ToA1() == "A1");
        CHECK(single->ToR1C1() == "R1C1");

        auto block = CellRange::ParseA1("$B$2:D5");
        REQUIRE(block);
        CHECK(block->First().ToA1() == "B2");
        CHECK(block->Last().ToA1() == "D5");
        CHECK(block->RowCount() == 4);
        CHECK(block->ColumnCount() == 3);
        CHECK(block->ToA1() == "B2:D5");
        CHECK(block->ToR1C1() == "R2C2:R5C4");

        auto r1c1 = CellRange::ParseR1C1("R2C2:R5C4");
        REQUIRE(r1c1);
        CHECK(r1c1->ToA1() == "B2:D5");

        CHECK_FALSE(CellRange::ParseA1("B2:A1"));
        CHECK_FALSE(CellRange::ParseA1("A1:"));
        CHECK_FALSE(CellRange::ParseA1(":B2"));
        CHECK_FALSE(CellRange::ParseA1("A1:B2:C3"));
        CHECK_FALSE(CellRange::ParseA1("A:B"));
        CHECK_FALSE(CellRange::ParseR1C1("R2C2:R1C1"));
        CHECK_FALSE(CellRange::ParseR1C1("R1C1:"));
    }

    TEST_CASE("Excel worksheet accepts parsed cell addresses [unit] [excel] [excel-address]")
    {
        auto editor = ExcelDocumentEditor::CreateNew();
        REQUIRE(editor != nullptr);
        auto sheet = editor->FirstWorksheet();
        REQUIRE(sheet != nullptr);

        auto textAddress = CellAddress::ParseA1("C3");
        auto numberAddress = CellAddress::ParseR1C1("R4C4");
        REQUIRE(textAddress);
        REQUIRE(numberAddress);

        CHECK(sheet->SetCellText(*textAddress, "parsed"));
        CHECK(sheet->SetCellNumber(*numberAddress, 12.25));

        const auto xml = sheet->GetPart()->GetXmlString();
        CHECK(xml.find("C3") != std::string::npos);
        CHECK(editor->SharedStrings().Lookup(0) == "parsed");
        CHECK(xml.find("D4") != std::string::npos);
        CHECK(xml.find("12.25") != std::string::npos);
    }

} // namespace
