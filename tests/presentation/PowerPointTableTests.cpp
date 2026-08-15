// Copyright (c) 2026 Jakub Melka and Contributors
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "doctest.h"

#include "ExyokiOffice/OpenXmlPackageValidator.hpp"
#include "ExyokiOffice/PowerPoint/PowerPointDocument.hpp"

using namespace ExyokiOffice::PowerPoint;

namespace
{
PresentationTableData SampleTable()
{
    PresentationTableData table;
    table.ColumnWidths = {1000000, 2000000, 3000000};
    table.Rows = {
        {500000, {{"Header"}, {"Q1"}, {"Q2"}}},
        {600000, {{"Revenue\nEUR"}, {"10"}, {"20"}}},
        {700000, {{"Profit"}, {"3"}, {"7"}}}};
    table.Merges = {{0, 0, 1, 2}, {1, 2, 2, 1}};
    table.Style.Id = "{5C22544A-7EE6-4342-B048-85BDC9FD1C3A}";
    table.Style.FirstRow = true;
    table.Style.FirstColumn = true;
    table.Style.LastRow = true;
    table.Style.BandedRows = true;
    table.Transform.Position = {914400, 1828800};
    table.Transform.Size = {6000000, 1800000};
    table.Transform.Rotation = 60000;
    return table;
}

void CheckRectangular(const PresentationTableData& table)
{
    REQUIRE_FALSE(table.ColumnWidths.empty());
    REQUIRE_FALSE(table.Rows.empty());
    for (const auto& row : table.Rows)
    {
        CHECK(row.Cells.size() == table.ColumnWidths.size());
    }
}
} // namespace

TEST_SUITE("PowerPointTableTests")
{
    TEST_CASE("table grid text merges style and transform round trip [unit] [powerpoint] [table]")
    {
        auto editor = PowerPointDocumentEditor::CreateNew();
        auto shape = editor->AddSlide()->ShapeTree()->AddTable(SampleTable());
        REQUIRE(shape);
        REQUIRE(shape->GetTable());
        CHECK(shape->GetTable() == SampleTable());

        auto reopened = PowerPointDocumentEditor::Open(editor->SaveToMemory());
        REQUIRE(reopened);
        auto actual = reopened->GetSlide(0)->ShapeTree()->Get(0)->GetTable();
        REQUIRE(actual);
        CHECK(*actual == SampleTable());
        CheckRectangular(*actual);
    }

    TEST_CASE("row structural edits preserve rectangularity and update merges [unit] [powerpoint] [table]")
    {
        auto editor = PowerPointDocumentEditor::CreateNew();
        auto shape = editor->AddSlide()->ShapeTree()->AddTable(SampleTable());
        REQUIRE(shape);
        REQUIRE(shape->InsertTableRow(2, 123456));
        auto table = shape->GetTable();
        REQUIRE(table);
        REQUIRE(table->Rows.size() == 4);
        CHECK(table->Rows[2].Height == 123456);
        CHECK(table->Merges == std::vector<PresentationTableMerge>{{0, 0, 1, 2}, {1, 2, 3, 1}});
        CheckRectangular(*table);

        REQUIRE(shape->RemoveTableRow(1));
        table = shape->GetTable();
        REQUIRE(table);
        CHECK(table->Rows[1].Cells[2].Text == "20");
        CHECK(table->Merges == std::vector<PresentationTableMerge>{{0, 0, 1, 2}, {1, 2, 2, 1}});
        CheckRectangular(*table);
    }

    TEST_CASE("column structural edits preserve rectangularity and anchor text [unit] [powerpoint] [table]")
    {
        auto editor = PowerPointDocumentEditor::CreateNew();
        auto shape = editor->AddSlide()->ShapeTree()->AddTable(SampleTable());
        REQUIRE(shape);
        REQUIRE(shape->InsertTableColumn(1, 444444));
        auto table = shape->GetTable();
        REQUIRE(table);
        CHECK(table->ColumnWidths == std::vector<ExyokiOffice::MeasuringUnits>{1000000, 444444, 2000000, 3000000});
        CHECK(table->Merges == std::vector<PresentationTableMerge>{{0, 0, 1, 3}, {1, 3, 2, 1}});
        CheckRectangular(*table);

        REQUIRE(shape->RemoveTableColumn(0));
        table = shape->GetTable();
        REQUIRE(table);
        CHECK(table->Rows[0].Cells[0].Text == "Header");
        CHECK(table->Merges == std::vector<PresentationTableMerge>{{0, 0, 1, 2}, {1, 2, 2, 1}});
        CheckRectangular(*table);
    }

    TEST_CASE("merge and unmerge reject overlap and preserve physical cell content [unit] [powerpoint] [table]")
    {
        auto editor = PowerPointDocumentEditor::CreateNew();
        auto table = SampleTable();
        table.Merges.clear();
        auto shape = editor->AddSlide()->ShapeTree()->AddTable(table);
        REQUIRE(shape);
        REQUIRE(shape->MergeTableCells(0, 0, 2, 2));
        CHECK_FALSE(shape->MergeTableCells(1, 1, 2, 2));
        REQUIRE(shape->UnmergeTableCells(0, 0));
        auto actual = shape->GetTable();
        REQUIRE(actual);
        CHECK(actual->Merges.empty());
        CHECK(actual->Rows[1].Cells[1].Text == "10");
        CHECK_FALSE(shape->UnmergeTableCells(0, 0));
    }

    TEST_CASE("invalid tables and structural boundaries are rejected transactionally [unit] [powerpoint] [table]")
    {
        auto editor = PowerPointDocumentEditor::CreateNew();
        auto tree = editor->AddSlide()->ShapeTree();
        auto shape = tree->AddTable(SampleTable());
        REQUIRE(shape);
        const auto original = shape->GetTable();

        auto invalid = SampleTable();
        invalid.Rows[1].Cells.pop_back();
        CHECK_FALSE(shape->SetTable(invalid));
        invalid = SampleTable();
        invalid.ColumnWidths[0] = -1;
        CHECK_FALSE(shape->SetTable(invalid));
        invalid = SampleTable();
        invalid.Merges.push_back({0, 1, 2, 2});
        CHECK_FALSE(shape->SetTable(invalid));
        CHECK(shape->GetTable() == original);
        CHECK_FALSE(shape->InsertTableRow(99));
        CHECK_FALSE(shape->RemoveTableColumn(99));
        CHECK_FALSE(tree->AddShape()->SetTable(SampleTable()));

        PresentationTableData oneCell;
        oneCell.ColumnWidths = {1};
        oneCell.Rows = {{1, {{"only"}}}};
        auto single = tree->AddTable(oneCell);
        REQUIRE(single);
        CHECK_FALSE(single->RemoveTableRow(0));
        CHECK_FALSE(single->RemoveTableColumn(0));
    }

    TEST_CASE("authored and structurally edited table passes package validation [unit] [powerpoint] [table]")
    {
        auto editor = PowerPointDocumentEditor::CreateNew();
        auto shape = editor->AddSlide()->ShapeTree()->AddTable(SampleTable());
        REQUIRE(shape);
        REQUIRE(shape->InsertTableRow(1, 250000));
        REQUIRE(shape->InsertTableColumn(3, 500000));
        REQUIRE(shape->MergeTableCells(2, 0, 2, 2));
        const auto report = ExyokiOffice::OpenXmlPackageValidator().Validate(*editor->GetDocument());
        CHECK(report.IsValid());
    }
} // TEST_SUITE("PowerPointTableTests")
