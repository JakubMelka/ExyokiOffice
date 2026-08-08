// Copyright (c) 2026 Jakub Melka and Collaborators
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "doctest.h"

#include "ExyokiOffice/Excel/ExcelDocument.hpp"

#include <string>

using ExyokiOffice::Excel::CellAddress;
using ExyokiOffice::Excel::CellValueKind;
using ExyokiOffice::Excel::ExcelDocumentEditor;
using ExyokiOffice::Excel::MaxColumnIndex;
using ExyokiOffice::Excel::MaxRowIndex;

namespace
{
CellAddress Address(std::string_view text)
{
    const auto value = CellAddress::ParseA1(text);
    REQUIRE(value.has_value());
    return *value;
}
} // namespace

TEST_SUITE("ExcelSparseWorksheetTests")
{
    TEST_CASE("Sparse writes at extreme coordinates materialize only requested cells [unit] [excel] [excel-sparse]")
    {
        auto editor = ExcelDocumentEditor::CreateNew();
        REQUIRE(editor);
        auto sheet = editor->FirstWorksheet();
        REQUIRE(sheet);

        CHECK(sheet->StoredCellCount() == 0);
        CHECK(sheet->SetCellNumber(1, 1, 1.0));
        CHECK(sheet->SetCellNumber(MaxRowIndex, MaxColumnIndex, 2.0));
        CHECK(sheet->StoredCellCount() == 2);
        CHECK(sheet->ContainsCell(1, 1));
        CHECK(sheet->ContainsCell(MaxRowIndex, MaxColumnIndex));
        CHECK_FALSE(sheet->ContainsCell(MaxRowIndex, MaxColumnIndex - 1));

        const auto addresses = sheet->StoredCellAddresses();
        REQUIRE(addresses.size() == 2);
        CHECK(addresses[0].ToA1() == "A1");
        CHECK(addresses[1].ToA1() == "XFD1048576");
    }

    TEST_CASE("Out-of-order sparse writes keep rows and cells in canonical order [unit] [excel] [excel-sparse]")
    {
        auto editor = ExcelDocumentEditor::CreateNew();
        auto sheet = editor->FirstWorksheet();
        REQUIRE(sheet);
        CHECK(sheet->SetCellNumber(Address("Z100"), 1));
        CHECK(sheet->SetCellNumber(Address("C3"), 2));
        CHECK(sheet->SetCellNumber(Address("A3"), 3));
        CHECK(sheet->SetCellNumber(Address("B3"), 4));
        CHECK(sheet->SetCellNumber(Address("A1"), 5));

        const auto addresses = sheet->StoredCellAddresses();
        REQUIRE(addresses.size() == 5);
        CHECK(addresses[0].ToA1() == "A1");
        CHECK(addresses[1].ToA1() == "A3");
        CHECK(addresses[2].ToA1() == "B3");
        CHECK(addresses[3].ToA1() == "C3");
        CHECK(addresses[4].ToA1() == "Z100");
    }

    TEST_CASE("Repeated writes update one sparse cell without duplicates [unit] [excel] [excel-sparse]")
    {
        auto editor = ExcelDocumentEditor::CreateNew();
        auto sheet = editor->FirstWorksheet();
        REQUIRE(sheet);
        CHECK(sheet->SetCellNumber(Address("Q42"), 1));
        CHECK(sheet->SetCellNumber(Address("Q42"), 2));
        CHECK(sheet->SetCellBoolean(Address("Q42"), true));
        CHECK(sheet->StoredCellCount() == 1);
        auto value = sheet->GetCellValue(Address("Q42"));
        REQUIRE(value);
        CHECK(value->Kind() == CellValueKind::Boolean);
        REQUIRE(value->BooleanValue());
        CHECK(*value->BooleanValue());
    }

    TEST_CASE("Removing cells prunes empty ordinary rows and preserves missing-cell semantics [unit] [excel] [excel-sparse]")
    {
        auto editor = ExcelDocumentEditor::CreateNew();
        auto sheet = editor->FirstWorksheet();
        REQUIRE(sheet);
        CHECK(sheet->SetCellText(Address("A7"), "one"));
        CHECK(sheet->SetCellText(Address("B7"), "two"));
        CHECK_FALSE(sheet->RemoveCell(Address("C7")));
        CHECK(sheet->RemoveCell(Address("A7")));
        CHECK(sheet->StoredCellCount() == 1);
        CHECK(sheet->RemoveCell(Address("B7")));
        CHECK(sheet->StoredCellCount() == 0);
        CHECK_FALSE(sheet->ContainsCell(Address("B7")));
        auto blank = sheet->GetCellValue(Address("B7"));
        REQUIRE(blank);
        CHECK(blank->Kind() == CellValueKind::Blank);

        CHECK(editor->SharedStrings().Count() == 2);
        CHECK(editor->SharedStrings().Cleanup());
        CHECK(editor->SharedStrings().Count() == 0);
    }

    TEST_CASE("Sparse storage survives package round trip [unit] [excel] [excel-sparse]")
    {
        auto editor = ExcelDocumentEditor::CreateNew();
        auto sheet = editor->FirstWorksheet();
        REQUIRE(sheet);
        CHECK(sheet->SetCellNumber(Address("XFD1048576"), 9.5));
        CHECK(sheet->SetCellFormula(Address("A500000"), "SUM(1,2)"));

        const auto bytes = editor->SaveToMemory();
        REQUIRE_FALSE(bytes.empty());
        auto reopened = ExcelDocumentEditor::Open(bytes);
        REQUIRE(reopened);
        auto reopenedSheet = reopened->FirstWorksheet();
        REQUIRE(reopenedSheet);
        CHECK(reopenedSheet->StoredCellCount() == 2);
        CHECK(reopenedSheet->ContainsCell(Address("A500000")));
        CHECK(reopenedSheet->ContainsCell(Address("XFD1048576")));
        CHECK_FALSE(reopenedSheet->ContainsCell(Address("A499999")));
    }

    TEST_CASE("Sparse API rejects invalid addresses without mutation [unit] [excel] [excel-sparse]")
    {
        auto editor = ExcelDocumentEditor::CreateNew();
        auto sheet = editor->FirstWorksheet();
        REQUIRE(sheet);
        CHECK_FALSE(sheet->ContainsCell(0, 1));
        CHECK_FALSE(sheet->ContainsCell(1, 0));
        CHECK_FALSE(sheet->ContainsCell(MaxRowIndex + 1, 1));
        CHECK_FALSE(sheet->RemoveCell(1, MaxColumnIndex + 1));
        CHECK(sheet->StoredCellCount() == 0);
    }

    TEST_CASE("Sparse nodes use schema qualified names and the namespace prefix in scope [unit] [excel] [excel-sparse]")
    {
        auto editor = ExcelDocumentEditor::CreateNew();
        auto sheet = editor->FirstWorksheet();
        REQUIRE(sheet);
        sheet->GetPart()->SetXmlString(
            R"(<?xml version="1.0"?><ss:worksheet xmlns:ss="http://schemas.openxmlformats.org/spreadsheetml/2006/main" xmlns:alien="urn:unrelated"><alien:sheetData><alien:row r="1"><alien:c r="A1"/></alien:row></alien:sheetData><ss:sheetData><ss:row r="2"><ss:c alien:r="B2"/></ss:row></ss:sheetData></ss:worksheet>)");

        CHECK(sheet->SetCellNumber(Address("B2"), 17));
        CHECK(sheet->StoredCellCount() == 1);
        CHECK(sheet->ContainsCell(Address("B2")));
        CHECK_FALSE(sheet->ContainsCell(Address("A1")));

        const auto xml = sheet->GetPart()->GetXmlString();
        CHECK(xml.find("<ss:row r=\"2\"") != std::string::npos);
        CHECK(xml.find("<ss:c r=\"B2\"") != std::string::npos);
        CHECK(xml.find("alien:r=\"B2\"") != std::string::npos);
        CHECK(xml.find("<alien:c r=\"B2\"") == std::string::npos);
    }
}
