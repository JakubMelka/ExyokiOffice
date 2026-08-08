// Copyright (c) 2026 Jakub Melka and Collaborators
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "doctest.h"

#include "ExyokiOffice/Excel/ExcelDocument.hpp"

using ExyokiOffice::Excel::CellAddress;
using ExyokiOffice::Excel::CellRange;
using ExyokiOffice::Excel::CellValueKind;
using ExyokiOffice::Excel::ExcelCellMatrix;
using ExyokiOffice::Excel::ExcelCellValue;
using ExyokiOffice::Excel::ExcelDocumentEditor;
using ExyokiOffice::Excel::RangeOperationError;
using ExyokiOffice::Excel::Worksheet;

class ExcelRangeTestHelpers final
{
public:
    ExcelRangeTestHelpers() = delete;

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

    static ExcelCellValue Value(const Worksheet::Ptr& worksheet, std::string_view address)
    {
        const auto value = worksheet->GetCellValue(Address(address));
        REQUIRE(value.has_value());
        return *value;
    }
};

TEST_SUITE("ExcelRangeBulkTests")
{
    TEST_CASE("GetRangeValues returns a dense matrix without materializing missing cells [unit] [excel] [excel-range]")
    {
        auto editor = ExcelDocumentEditor::CreateNew();
        auto sheet = editor->FirstWorksheet();
        REQUIRE(sheet);
        CHECK(sheet->SetCellNumber(ExcelRangeTestHelpers::Address("B2"), 12.5));
        CHECK(sheet->SetCellBoolean(ExcelRangeTestHelpers::Address("C3"), true));
        const auto storedBefore = sheet->StoredCellCount();

        const auto result = sheet->GetRangeValues(ExcelRangeTestHelpers::Range("A1:C3"));
        REQUIRE(result);
        CHECK(result.Status.Error == RangeOperationError::None);
        CHECK(result.Status.AffectedCellCount == 9);
        REQUIRE(result.Values.size() == 3);
        REQUIRE(result.Values[0].size() == 3);
        CHECK(result.Values[0][0].Kind() == CellValueKind::Blank);
        CHECK(result.Values[1][1].Kind() == CellValueKind::Number);
        CHECK(result.Values[1][1].Text() == "12.5");
        CHECK(result.Values[2][2].Kind() == CellValueKind::Boolean);
        CHECK(sheet->StoredCellCount() == storedBefore);
    }

    TEST_CASE("SetRangeValues writes a typed matrix and blank entries remain sparse [unit] [excel] [excel-range]")
    {
        auto editor = ExcelDocumentEditor::CreateNew();
        auto sheet = editor->FirstWorksheet();
        REQUIRE(sheet);
        CHECK(sheet->SetCellNumber(ExcelRangeTestHelpers::Address("B2"), 99));

        ExcelCellMatrix values{
            {ExcelCellValue::NumberText("1.00"), ExcelCellValue::InlineString("two")},
            {ExcelCellValue::Boolean(true), ExcelCellValue::Blank()}};
        const auto result = sheet->SetRangeValues(ExcelRangeTestHelpers::Range("A1:B2"), values);
        REQUIRE(result);
        CHECK(result.AffectedCellCount == 4);
        CHECK(ExcelRangeTestHelpers::Value(sheet, "A1").Text() == "1.00");
        CHECK(ExcelRangeTestHelpers::Value(sheet, "B1").Text() == "two");
        CHECK(ExcelRangeTestHelpers::Value(sheet, "A2").Kind() == CellValueKind::Boolean);
        CHECK_FALSE(sheet->ContainsCell(ExcelRangeTestHelpers::Address("B2")));
        CHECK(sheet->StoredCellCount() == 3);
    }

    TEST_CASE("Dimension mismatches return structured errors without partial writes [unit] [excel] [excel-range]")
    {
        auto editor = ExcelDocumentEditor::CreateNew();
        auto sheet = editor->FirstWorksheet();
        REQUIRE(sheet);
        CHECK(sheet->SetCellNumber(ExcelRangeTestHelpers::Address("A1"), 10));
        CHECK(sheet->SetCellNumber(ExcelRangeTestHelpers::Address("B1"), 20));
        const auto originalXml = sheet->GetPart()->GetXmlString();

        ExcelCellMatrix tooFewRows{{ExcelCellValue::Number(1), ExcelCellValue::Number(2)}};
        auto result = sheet->SetRangeValues(ExcelRangeTestHelpers::Range("A1:B2"), tooFewRows);
        CHECK_FALSE(result);
        CHECK(result.Error == RangeOperationError::DimensionMismatch);
        CHECK_FALSE(result.Message.empty());
        CHECK(sheet->GetPart()->GetXmlString() == originalXml);

        ExcelCellMatrix jagged{{ExcelCellValue::Number(1), ExcelCellValue::Number(2)},
                               {ExcelCellValue::Number(3)}};
        result = sheet->SetRangeValues(ExcelRangeTestHelpers::Range("A1:B2"), jagged);
        CHECK_FALSE(result);
        CHECK(result.Error == RangeOperationError::DimensionMismatch);
        CHECK(sheet->GetPart()->GetXmlString() == originalXml);

        ExcelCellMatrix tooManyColumns{{ExcelCellValue::Number(1), ExcelCellValue::Number(2), ExcelCellValue::Number(3)}};
        result = sheet->SetRangeValues(ExcelRangeTestHelpers::Range("A1:B1"), tooManyColumns);
        CHECK_FALSE(result);
        CHECK(result.Error == RangeOperationError::DimensionMismatch);
        CHECK(sheet->GetPart()->GetXmlString() == originalXml);
    }

    TEST_CASE("ClearRange removes only stored cells inside the rectangle [unit] [excel] [excel-range]")
    {
        auto editor = ExcelDocumentEditor::CreateNew();
        auto sheet = editor->FirstWorksheet();
        REQUIRE(sheet);
        CHECK(sheet->FillRange(ExcelRangeTestHelpers::Range("A1:C3"), ExcelCellValue::Number(7)));
        CHECK(sheet->SetCellNumber(ExcelRangeTestHelpers::Address("D4"), 8));

        const auto result = sheet->ClearRange(ExcelRangeTestHelpers::Range("B2:C3"));
        REQUIRE(result);
        CHECK(result.AffectedCellCount == 4);
        CHECK_FALSE(sheet->ContainsCell(ExcelRangeTestHelpers::Address("B2")));
        CHECK_FALSE(sheet->ContainsCell(ExcelRangeTestHelpers::Address("C3")));
        CHECK(sheet->ContainsCell(ExcelRangeTestHelpers::Address("A1")));
        CHECK(sheet->ContainsCell(ExcelRangeTestHelpers::Address("D4")));
        CHECK(sheet->StoredCellCount() == 6);
    }

    TEST_CASE("FillRange supports values and blank clearing [unit] [excel] [excel-range]")
    {
        auto editor = ExcelDocumentEditor::CreateNew();
        auto sheet = editor->FirstWorksheet();
        REQUIRE(sheet);
        auto result = sheet->FillRange(ExcelRangeTestHelpers::Range("XFD1048575:XFD1048576"),
                                       ExcelCellValue::Error("#N/A"));
        REQUIRE(result);
        CHECK(result.AffectedCellCount == 2);
        CHECK(ExcelRangeTestHelpers::Value(sheet, "XFD1048576").Kind() == CellValueKind::Error);

        result = sheet->FillRange(ExcelRangeTestHelpers::Range("XFD1048575:XFD1048576"), ExcelCellValue::Blank());
        REQUIRE(result);
        CHECK(result.AffectedCellCount == 2);
        CHECK(sheet->StoredCellCount() == 0);
    }

    TEST_CASE("CopyRange snapshots overlapping source values [unit] [excel] [excel-range]")
    {
        auto editor = ExcelDocumentEditor::CreateNew();
        auto sheet = editor->FirstWorksheet();
        REQUIRE(sheet);
        ExcelCellMatrix values{{ExcelCellValue::Number(1)}, {ExcelCellValue::Number(2)}, {ExcelCellValue::Number(3)}};
        REQUIRE(sheet->SetRangeValues(ExcelRangeTestHelpers::Range("A1:A3"), values));

        const auto result = sheet->CopyRange(ExcelRangeTestHelpers::Range("A1:A3"),
                                             ExcelRangeTestHelpers::Address("A2"));
        REQUIRE(result);
        CHECK(ExcelRangeTestHelpers::Value(sheet, "A1").Text() == "1");
        CHECK(ExcelRangeTestHelpers::Value(sheet, "A2").Text() == "1");
        CHECK(ExcelRangeTestHelpers::Value(sheet, "A3").Text() == "2");
        CHECK(ExcelRangeTestHelpers::Value(sheet, "A4").Text() == "3");
    }

    TEST_CASE("MoveRange supports overlap and clears source cells outside destination [unit] [excel] [excel-range]")
    {
        auto editor = ExcelDocumentEditor::CreateNew();
        auto sheet = editor->FirstWorksheet();
        REQUIRE(sheet);
        ExcelCellMatrix values{{ExcelCellValue::InlineString("one")},
                               {ExcelCellValue::InlineString("two")},
                               {ExcelCellValue::InlineString("three")}};
        REQUIRE(sheet->SetRangeValues(ExcelRangeTestHelpers::Range("B1:B3"), values));

        const auto result = sheet->MoveRange(ExcelRangeTestHelpers::Range("B1:B3"),
                                             ExcelRangeTestHelpers::Address("B2"));
        REQUIRE(result);
        CHECK_FALSE(sheet->ContainsCell(ExcelRangeTestHelpers::Address("B1")));
        CHECK(ExcelRangeTestHelpers::Value(sheet, "B2").Text() == "one");
        CHECK(ExcelRangeTestHelpers::Value(sheet, "B3").Text() == "two");
        CHECK(ExcelRangeTestHelpers::Value(sheet, "B4").Text() == "three");
    }

    TEST_CASE("Copy and move reject destinations outside the worksheet without mutation [unit] [excel] [excel-range]")
    {
        auto editor = ExcelDocumentEditor::CreateNew();
        auto sheet = editor->FirstWorksheet();
        REQUIRE(sheet);
        REQUIRE(sheet->FillRange(ExcelRangeTestHelpers::Range("A1:B2"), ExcelCellValue::Number(5)));
        const auto originalXml = sheet->GetPart()->GetXmlString();

        auto result = sheet->CopyRange(ExcelRangeTestHelpers::Range("A1:B2"),
                                       ExcelRangeTestHelpers::Address("XFD1048576"));
        CHECK_FALSE(result);
        CHECK(result.Error == RangeOperationError::DestinationOutOfBounds);
        CHECK(sheet->GetPart()->GetXmlString() == originalXml);

        result = sheet->MoveRange(ExcelRangeTestHelpers::Range("A1:B2"),
                                  ExcelRangeTestHelpers::Address("XFD1048576"));
        CHECK_FALSE(result);
        CHECK(result.Error == RangeOperationError::DestinationOutOfBounds);
        CHECK(sheet->GetPart()->GetXmlString() == originalXml);
    }

    TEST_CASE("Range operations report detached worksheets and survive package round trip [unit] [excel] [excel-range]")
    {
        Worksheet detached;
        auto detachedResult = detached.FillRange(ExcelRangeTestHelpers::Range("A1:A1"), ExcelCellValue::Number(1));
        CHECK_FALSE(detachedResult);
        CHECK(detachedResult.Error == RangeOperationError::InvalidWorksheet);

        auto editor = ExcelDocumentEditor::CreateNew();
        auto sheet = editor->FirstWorksheet();
        REQUIRE(sheet);
        ExcelCellMatrix values{{ExcelCellValue::Formula("A1+1", ExyokiOffice::Excel::FormulaCachedValueKind::Number, "2"),
                                ExcelCellValue::DateTimeText("2026-07-10T12:00:00Z")}};
        REQUIRE(sheet->SetRangeValues(ExcelRangeTestHelpers::Range("C5:D5"), values));

        const auto bytes = editor->SaveToMemory();
        REQUIRE_FALSE(bytes.empty());
        auto reopened = ExcelDocumentEditor::Open(bytes);
        REQUIRE(reopened);
        const auto read = reopened->FirstWorksheet()->GetRangeValues(ExcelRangeTestHelpers::Range("C5:D5"));
        REQUIRE(read);
        CHECK(read.Values[0][0].Kind() == CellValueKind::Formula);
        CHECK(read.Values[0][0].FormulaValue().Formula == "A1+1");
        CHECK(read.Values[0][1].Kind() == CellValueKind::DateTime);
        CHECK(read.Values[0][1].Text() == "2026-07-10T12:00:00Z");
    }
}
