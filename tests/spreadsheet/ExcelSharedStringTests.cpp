// Copyright (c) 2026 Jakub Melka and Collaborators
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "doctest.h"

#include "ExyokiOffice/Excel/ExcelDocument.hpp"

using ExyokiOffice::Excel::CellAddress;
using ExyokiOffice::Excel::CellValueKind;
using ExyokiOffice::Excel::ExcelCellValue;
using ExyokiOffice::Excel::ExcelDocumentEditor;

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

} // namespace

TEST_SUITE("ExcelSharedStringTests")
{

    TEST_CASE("Shared string service deduplicates text and worksheet cells store indexes [unit] [excel] [excel-shared-strings]")
    {
        auto editor = ExcelDocumentEditor::CreateNew();
        REQUIRE(editor != nullptr);
        auto sheet = editor->FirstWorksheet();
        REQUIRE(sheet != nullptr);

        CHECK(sheet->SetCellText(Address("A1"), "Repeated"));
        CHECK(sheet->SetCellText(Address("B1"), "Repeated"));
        CHECK(sheet->SetCellText(Address("C1"), "Unique"));

        auto sharedStrings = editor->SharedStrings();
        CHECK(sharedStrings.IsValid());
        CHECK(sharedStrings.Count() == 2);
        CHECK(sharedStrings.UniqueCount() == 2);
        CHECK(sharedStrings.Find("Repeated") == 0);
        CHECK(sharedStrings.Find("Unique") == 1);
        CHECK(sharedStrings.Lookup(0) == "Repeated");
        CHECK(sharedStrings.Lookup(1) == "Unique");
        CHECK_FALSE(sharedStrings.Lookup(2));
        CHECK(sharedStrings.ReferenceCount(0) == 2);
        CHECK(sharedStrings.ReferenceCount(1) == 1);

        auto a1 = sheet->GetCellValue(Address("A1"));
        REQUIRE(a1);
        CHECK(a1->Kind() == CellValueKind::SharedString);
        REQUIRE(a1->SharedStringIndex());
        CHECK(*a1->SharedStringIndex() == 0);
    }

    TEST_CASE("Shared strings survive package round trip [unit] [excel] [excel-shared-strings]")
    {
        auto editor = ExcelDocumentEditor::CreateNew();
        REQUIRE(editor != nullptr);
        auto sheet = editor->FirstWorksheet();
        REQUIRE(sheet != nullptr);
        CHECK(sheet->SetCellText(Address("A1"), "Alpha"));
        CHECK(sheet->SetCellText(Address("A2"), "Beta"));
        CHECK(sheet->SetCellText(Address("A3"), "Alpha"));

        auto bytes = editor->SaveToMemory();
        REQUIRE(!bytes.empty());
        auto reopened = ExcelDocumentEditor::Open(bytes);
        REQUIRE(reopened != nullptr);

        auto sharedStrings = reopened->SharedStrings();
        CHECK(sharedStrings.Count() == 2);
        CHECK(sharedStrings.Lookup(0) == "Alpha");
        CHECK(sharedStrings.Lookup(1) == "Beta");
        CHECK(sharedStrings.ReferenceCount(0) == 2);
        CHECK(sharedStrings.ReferenceCount(1) == 1);
    }

    TEST_CASE("Shared string cleanup removes unused entries and remaps cells [unit] [excel] [excel-shared-strings]")
    {
        auto editor = ExcelDocumentEditor::CreateNew();
        REQUIRE(editor != nullptr);
        auto sheet = editor->FirstWorksheet();
        REQUIRE(sheet != nullptr);
        CHECK(sheet->SetCellText(Address("A1"), "Old"));
        CHECK(sheet->SetCellText(Address("A2"), "Keep"));
        CHECK(sheet->SetCellText(Address("A3"), "Old"));
        CHECK(sheet->SetCellText(Address("A1"), "Keep"));
        CHECK(sheet->SetCellText(Address("A3"), "Keep"));

        auto sharedStrings = editor->SharedStrings();
        CHECK(sharedStrings.Count() == 2);
        CHECK(sharedStrings.ReferenceCount(0) == 0);
        CHECK(sharedStrings.ReferenceCount(1) == 3);

        CHECK(sharedStrings.Cleanup());
        sharedStrings = editor->SharedStrings();
        CHECK(sharedStrings.Count() == 1);
        CHECK(sharedStrings.Lookup(0) == "Keep");
        CHECK(sharedStrings.ReferenceCount(0) == 3);

        auto a1 = sheet->GetCellValue(Address("A1"));
        auto a2 = sheet->GetCellValue(Address("A2"));
        REQUIRE(a1);
        REQUIRE(a2);
        REQUIRE(a1->SharedStringIndex());
        REQUIRE(a2->SharedStringIndex());
        CHECK(*a1->SharedStringIndex() == 0);
        auto a3 = sheet->GetCellValue(Address("A3"));
        REQUIRE(a3);
        REQUIRE(a2->SharedStringIndex());
        REQUIRE(a3->SharedStringIndex());
        CHECK(*a2->SharedStringIndex() == 0);
        CHECK(*a3->SharedStringIndex() == 0);
    }

    TEST_CASE("Shared string cleanup collapses duplicate entries introduced by copied worksheets [unit] [excel] [excel-shared-strings]")
    {
        auto editor = ExcelDocumentEditor::CreateNew();
        REQUIRE(editor != nullptr);
        auto sheet = editor->FirstWorksheet();
        REQUIRE(sheet != nullptr);
        CHECK(sheet->SetCellText(Address("A1"), "Copied"));

        auto copy = editor->CopyWorksheet(0, "Copy");
        REQUIRE(copy != nullptr);
        CHECK(copy->SetCellText(Address("B1"), "Copied"));

        auto sharedStrings = editor->SharedStrings();
        CHECK(sharedStrings.Count() == 1);
        CHECK(sharedStrings.ReferenceCount(0) == 3);
        CHECK(sharedStrings.Cleanup());
        CHECK(editor->SharedStrings().Count() == 1);
        CHECK(editor->SharedStrings().ReferenceCount(0) == 3);
    }

} // namespace
