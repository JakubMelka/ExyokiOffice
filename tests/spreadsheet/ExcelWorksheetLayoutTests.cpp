// Copyright (c) 2026 Jakub Melka and Contributors
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "ExyokiOffice/DOM/DocumentFormat/OpenXml/Spreadsheet.hpp"
#include "ExyokiOffice/Excel/ExcelDocument.hpp"
#include "ExyokiOffice/StandardTypes.hpp"

#include <doctest.h>
#include <limits>

using namespace ExyokiOffice::Excel;
namespace Spreadsheet = ExyokiOffice::DocumentFormat::OpenXml::Spreadsheet;

TEST_CASE("worksheet dimension validation rejects non-interoperable metadata [unit] [excel] [layout]")
{
    RowDimension row;
    CHECK(IsValidRowDimension(row));
    row.Height = 0;
    CHECK_FALSE(IsValidRowDimension(row));
    row.Height = 15;
    row.OutlineLevel = 8;
    CHECK_FALSE(IsValidRowDimension(row));
    ColumnDimension column;
    column.Width = 255;
    CHECK(IsValidColumnDimension(column));
    column.Width = 256;
    CHECK_FALSE(IsValidColumnDimension(column));
    column.Width = std::numeric_limits<ExyokiOffice::Real>::quiet_NaN();
    CHECK_FALSE(IsValidColumnDimension(column));
    WorksheetView view;
    view.FrozenRows = MaxRowIndex;
    CHECK_FALSE(IsValidWorksheetView(view));
}

TEST_CASE("row dimensions preserve cells and clear only presentation metadata [unit] [excel] [layout]")
{
    auto sheet = ExcelDocumentEditor::CreateNew()->FirstWorksheet();
    REQUIRE(sheet->SetCellValue(*CellAddress::ParseA1("B4"), ExcelCellValue::Number(7)));
    RowDimension d;
    d.Height = 24.5;
    d.Hidden = true;
    d.OutlineLevel = 3;
    d.Collapsed = true;
    REQUIRE(sheet->SetRowDimension(4, d));
    auto read = sheet->GetRowDimension(4);
    REQUIRE(read);
    CHECK(read->Height == doctest::Approx(24.5));
    CHECK(read->Hidden);
    CHECK(read->OutlineLevel == 3);
    CHECK(read->Collapsed);
    CHECK_FALSE(sheet->SetRowDimension(0, d));
    CHECK_FALSE(sheet->SetRowDimension(MaxRowIndex + 1, d));
    REQUIRE(sheet->SetRowDimension(4, std::nullopt));
    CHECK_FALSE(sheet->GetRowDimension(4));
    CHECK(sheet->ContainsCell(*CellAddress::ParseA1("B4")));
}

TEST_CASE("column dimensions split existing ranges without losing adjacent formatting [unit] [excel] [layout]")
{
    auto sheet = ExcelDocumentEditor::CreateNew()->FirstWorksheet();
    auto root = sheet->GetLowLevelApi();
    auto data = root->GetFirstChildOfType<Spreadsheet::SheetData>();
    auto cols = root->InsertChild<Spreadsheet::Columns>(data);
    auto range = cols->AppendChild<Spreadsheet::Column>();
    range->SetMin(ExyokiOffice::UInt32Value(2));
    range->SetMax(ExyokiOffice::UInt32Value(4));
    range->SetWidth(ExyokiOffice::DoubleValue(11));
    range->SetCustomWidth(ExyokiOffice::BooleanValue(true));
    range->SetStyle(ExyokiOffice::UInt32Value(6));
    ColumnDimension changed;
    changed.Width = 22;
    changed.Hidden = true;
    changed.OutlineLevel = 2;
    REQUIRE(sheet->SetColumnDimension(3, changed));
    CHECK(sheet->GetColumnDimension(2)->Width == doctest::Approx(11));
    CHECK(sheet->GetColumnDimension(3)->Width == doctest::Approx(22));
    CHECK(sheet->GetColumnDimension(4)->Width == doctest::Approx(11));
    const auto definitions = cols->Elements<Spreadsheet::Column>();
    REQUIRE(definitions.size() == 3);
    for (const auto& definition : definitions)
    {
        CHECK(definition->GetStyle().ValueOr(0) == 6);
    }
    REQUIRE(sheet->SetColumnDimension(3, std::nullopt));
    CHECK_FALSE(sheet->GetColumnDimension(3));
    CHECK_FALSE(sheet->SetColumnDimension(0, changed));
    CHECK_FALSE(sheet->SetColumnDimension(MaxColumnIndex + 1, changed));
}

TEST_CASE("worksheet view stores active cell and frozen panes and can unfreeze [unit] [excel] [layout]")
{
    auto sheet = ExcelDocumentEditor::CreateNew()->FirstWorksheet();
    WorksheetView view;
    view.ActiveCell = CellAddress::ParseA1("D9");
    view.FrozenRows = 2;
    view.FrozenColumns = 3;
    REQUIRE(sheet->SetView(view));
    auto read = sheet->GetView();
    REQUIRE(read.ActiveCell);
    CHECK(read.ActiveCell->ToA1() == "D9");
    CHECK(read.FrozenRows == 2);
    CHECK(read.FrozenColumns == 3);
    view.FrozenRows = 0;
    view.FrozenColumns = 0;
    REQUIRE(sheet->SetView(view));
    read = sheet->GetView();
    CHECK(read.FrozenRows == 0);
    CHECK(read.FrozenColumns == 0);
}

TEST_CASE("worksheet dimensions and view survive package round trip [unit] [excel] [layout]")
{
    auto editor = ExcelDocumentEditor::CreateNew();
    auto sheet = editor->FirstWorksheet();
    RowDimension row;
    row.Height = 18;
    row.OutlineLevel = 1;
    REQUIRE(sheet->SetRowDimension(12, row));
    ColumnDimension column;
    column.Width = 32.25;
    column.Hidden = true;
    REQUIRE(sheet->SetColumnDimension(5, column));
    WorksheetView view;
    view.ActiveCell = CellAddress::ParseA1("F13");
    view.FrozenRows = 1;
    view.FrozenColumns = 2;
    REQUIRE(sheet->SetView(view));
    auto bytes = editor->SaveToMemory();
    REQUIRE_FALSE(bytes.empty());
    auto reopened = ExcelDocumentEditor::Open(bytes);
    REQUIRE(reopened);
    sheet = reopened->FirstWorksheet();
    REQUIRE(sheet->GetRowDimension(12));
    CHECK(sheet->GetRowDimension(12)->Height == doctest::Approx(18));
    REQUIRE(sheet->GetColumnDimension(5));
    CHECK(sheet->GetColumnDimension(5)->Width == doctest::Approx(32.25));
    CHECK(sheet->GetColumnDimension(5)->Hidden);
    const auto reopenedView = sheet->GetView();
    REQUIRE(reopenedView.ActiveCell);
    CHECK(reopenedView.ActiveCell->ToA1() == "F13");
    CHECK(reopenedView.FrozenRows == 1);
    CHECK(reopenedView.FrozenColumns == 2);
}
