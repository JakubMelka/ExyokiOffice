// Copyright (c) 2026 Jakub Melka and Contributors
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "doctest.h"

#include "ExyokiOffice/Excel/ExcelDocument.hpp"

using ExyokiOffice::Excel::CellAddress;
using ExyokiOffice::Excel::CellRange;
using ExyokiOffice::Excel::CellValueKind;
using ExyokiOffice::Excel::ExcelDocumentEditor;
using ExyokiOffice::Excel::RangeOperationError;
using ExyokiOffice::Excel::Worksheet;

class ExcelMergeTestHelpers final
{
public:
    ExcelMergeTestHelpers() = delete;

    static CellAddress Address(std::string_view text)
    {
        const auto address = CellAddress::ParseA1(text);
        REQUIRE(address.has_value());
        return *address;
    }

    static CellRange Range(std::string_view text)
    {
        const auto range = CellRange::ParseA1(text);
        REQUIRE(range.has_value());
        return *range;
    }
};

TEST_SUITE("ExcelMergeTests")
{
    TEST_CASE("MergeRange preserves the top-left value and removes covered sparse cells [unit] [excel] [excel-merge]")
    {
        auto editor = ExcelDocumentEditor::CreateNew();
        auto sheet = editor->FirstWorksheet();
        REQUIRE(sheet);
        CHECK(sheet->SetCellText(ExcelMergeTestHelpers::Address("B2"), "preserved"));
        CHECK(sheet->SetCellNumber(ExcelMergeTestHelpers::Address("C2"), 2));
        CHECK(sheet->SetCellNumber(ExcelMergeTestHelpers::Address("B3"), 3));
        CHECK(sheet->SetCellFormula(ExcelMergeTestHelpers::Address("C3"), "C2+B3"));
        CHECK(sheet->SetCellNumber(ExcelMergeTestHelpers::Address("D4"), 4));

        const auto result = sheet->MergeRange(ExcelMergeTestHelpers::Range("B2:C3"));
        REQUIRE(result);
        CHECK(result.AffectedCellCount == 3);
        CHECK(sheet->ContainsCell(ExcelMergeTestHelpers::Address("B2")));
        CHECK_FALSE(sheet->ContainsCell(ExcelMergeTestHelpers::Address("C2")));
        CHECK_FALSE(sheet->ContainsCell(ExcelMergeTestHelpers::Address("B3")));
        CHECK_FALSE(sheet->ContainsCell(ExcelMergeTestHelpers::Address("C3")));
        CHECK(sheet->ContainsCell(ExcelMergeTestHelpers::Address("D4")));
        const auto topLeft = sheet->GetCellValue(ExcelMergeTestHelpers::Address("B2"));
        REQUIRE(topLeft);
        CHECK(topLeft->Kind() == CellValueKind::SharedString);

        const auto ranges = sheet->MergedRanges();
        REQUIRE(ranges.size() == 1);
        CHECK(ranges[0].ToA1() == "B2:C3");
        const auto containing = sheet->MergedRangeAt(ExcelMergeTestHelpers::Address("C3"));
        REQUIRE(containing);
        CHECK(containing->ToA1() == "B2:C3");
        CHECK_FALSE(sheet->MergedRangeAt(ExcelMergeTestHelpers::Address("D4")));
        CHECK(sheet->GetPart()->GetXmlString().find("count=\"1\"") != std::string::npos);
    }

    TEST_CASE("MergeRange rejects every overlap and permits adjacent ranges without mutation [unit] [excel] [excel-merge]")
    {
        auto editor = ExcelDocumentEditor::CreateNew();
        auto sheet = editor->FirstWorksheet();
        REQUIRE(sheet);
        REQUIRE(sheet->MergeRange(ExcelMergeTestHelpers::Range("B2:C3")));

        for (const auto text : {"B2:C3", "C3:D4", "A1:D4", "B3:B4"})
        {
            const auto originalXml = sheet->GetPart()->GetXmlString();
            const auto result = sheet->MergeRange(ExcelMergeTestHelpers::Range(text));
            CHECK_FALSE(result);
            CHECK(result.Error == RangeOperationError::OverlappingRange);
            CHECK_FALSE(result.Message.empty());
            CHECK(sheet->GetPart()->GetXmlString() == originalXml);
        }

        REQUIRE(sheet->MergeRange(ExcelMergeTestHelpers::Range("D2:E3")));
        const auto ranges = sheet->MergedRanges();
        REQUIRE(ranges.size() == 2);
        CHECK(ranges[0].ToA1() == "B2:C3");
        CHECK(ranges[1].ToA1() == "D2:E3");
    }

    TEST_CASE("UnmergeRange requires an exact match and retains the top-left value [unit] [excel] [excel-merge]")
    {
        auto editor = ExcelDocumentEditor::CreateNew();
        auto sheet = editor->FirstWorksheet();
        REQUIRE(sheet);
        CHECK(sheet->SetCellNumber(ExcelMergeTestHelpers::Address("A1"), 42));
        REQUIRE(sheet->MergeRange(ExcelMergeTestHelpers::Range("A1:C2")));

        auto result = sheet->UnmergeRange(ExcelMergeTestHelpers::Range("A1:B2"));
        CHECK_FALSE(result);
        CHECK(result.Error == RangeOperationError::RangeNotFound);
        CHECK(sheet->MergedRanges().size() == 1);

        result = sheet->UnmergeRange(ExcelMergeTestHelpers::Range("A1:C2"));
        REQUIRE(result);
        CHECK(result.AffectedCellCount == 1);
        CHECK(sheet->MergedRanges().empty());
        CHECK(sheet->ContainsCell(ExcelMergeTestHelpers::Address("A1")));
        CHECK(sheet->GetCellValue(ExcelMergeTestHelpers::Address("A1"))->Text() == "42");
        CHECK_FALSE(sheet->ContainsCell(ExcelMergeTestHelpers::Address("B1")));
        CHECK(sheet->GetPart()->GetXmlString().find("mergeCells") == std::string::npos);
    }

    TEST_CASE("Merge registry uses the namespace in scope and schema-aware worksheet ordering [unit] [excel] [excel-merge]")
    {
        auto editor = ExcelDocumentEditor::CreateNew();
        auto sheet = editor->FirstWorksheet();
        REQUIRE(sheet);
        sheet->GetPart()->SetXmlString(
            R"(<?xml version="1.0"?><ss:worksheet xmlns:ss="http://schemas.openxmlformats.org/spreadsheetml/2006/main"><ss:sheetData><ss:row r="1"><ss:c r="A1" t="n"><ss:v>1</ss:v></ss:c></ss:row></ss:sheetData><ss:conditionalFormatting sqref="A1"/></ss:worksheet>)");

        REQUIRE(sheet->MergeRange(ExcelMergeTestHelpers::Range("A1:B2")));
        const auto xml = sheet->GetPart()->GetXmlString();
        const auto mergePosition = xml.find("<ss:mergeCells");
        const auto conditionalPosition = xml.find("<ss:conditionalFormatting");
        REQUIRE(mergePosition != std::string::npos);
        REQUIRE(conditionalPosition != std::string::npos);
        CHECK(mergePosition < conditionalPosition);
        CHECK(xml.find("<ss:mergeCell ref=\"A1:B2\"") != std::string::npos);
        CHECK(xml.find("<x:mergeCell") == std::string::npos);
    }

    TEST_CASE("Malformed registries and invalid merge requests return structured errors atomically [unit] [excel] [excel-merge]")
    {
        Worksheet detached;
        auto result = detached.MergeRange(ExcelMergeTestHelpers::Range("A1:B2"));
        CHECK_FALSE(result);
        CHECK(result.Error == RangeOperationError::InvalidWorksheet);

        auto editor = ExcelDocumentEditor::CreateNew();
        auto sheet = editor->FirstWorksheet();
        REQUIRE(sheet);
        result = sheet->MergeRange(ExcelMergeTestHelpers::Range("A1"));
        CHECK_FALSE(result);
        CHECK(result.Error == RangeOperationError::InvalidAddress);

        sheet->GetPart()->SetXmlString(
            R"(<?xml version="1.0"?><x:worksheet xmlns:x="http://schemas.openxmlformats.org/spreadsheetml/2006/main"><x:sheetData/><x:mergeCells count="1"><x:mergeCell ref="invalid"/></x:mergeCells></x:worksheet>)");
        const auto originalXml = sheet->GetPart()->GetXmlString();
        result = sheet->MergeRange(ExcelMergeTestHelpers::Range("C1:D2"));
        CHECK_FALSE(result);
        CHECK(result.Error == RangeOperationError::ReferenceUpdateFailed);
        CHECK(sheet->GetPart()->GetXmlString() == originalXml);
        CHECK(sheet->MergedRanges().empty());
    }

    TEST_CASE("Merged ranges follow row and column structural edits [unit] [excel] [excel-merge]")
    {
        auto editor = ExcelDocumentEditor::CreateNew();
        auto sheet = editor->FirstWorksheet();
        REQUIRE(sheet);
        REQUIRE(sheet->MergeRange(ExcelMergeTestHelpers::Range("B2:C4")));
        REQUIRE(sheet->InsertRows(3, 2));
        REQUIRE(sheet->InsertColumns(2, 1));
        auto ranges = sheet->MergedRanges();
        REQUIRE(ranges.size() == 1);
        CHECK(ranges[0].ToA1() == "C2:D6");

        REQUIRE(sheet->DeleteRows(2, 5));
        CHECK(sheet->MergedRanges().empty());
    }

    TEST_CASE("Merge and unmerge survive package round trips [unit] [excel] [excel-merge]")
    {
        auto editor = ExcelDocumentEditor::CreateNew();
        auto sheet = editor->FirstWorksheet();
        REQUIRE(sheet);
        CHECK(sheet->SetCellText(ExcelMergeTestHelpers::Address("C5"), "round trip"));
        REQUIRE(sheet->MergeRange(ExcelMergeTestHelpers::Range("C5:E7")));

        auto bytes = editor->SaveToMemory();
        REQUIRE_FALSE(bytes.empty());
        auto reopened = ExcelDocumentEditor::Open(bytes);
        REQUIRE(reopened);
        auto reopenedSheet = reopened->FirstWorksheet();
        REQUIRE(reopenedSheet);
        const auto range = reopenedSheet->MergedRangeAt(ExcelMergeTestHelpers::Address("D6"));
        REQUIRE(range);
        CHECK(range->ToA1() == "C5:E7");
        REQUIRE(reopenedSheet->UnmergeRange(*range));

        bytes = reopened->SaveToMemory();
        REQUIRE_FALSE(bytes.empty());
        auto reopenedAgain = ExcelDocumentEditor::Open(bytes);
        REQUIRE(reopenedAgain);
        CHECK(reopenedAgain->FirstWorksheet()->MergedRanges().empty());
        CHECK(reopenedAgain->FirstWorksheet()->ContainsCell(ExcelMergeTestHelpers::Address("C5")));
    }
}
