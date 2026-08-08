// Copyright (c) 2026 Jakub Melka and Collaborators
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "doctest.h"

#include "ExyokiOffice/Color.hpp"
#include "ExyokiOffice/DOM/DocumentFormat/OpenXml/Wordprocessing.hpp"
#include "ExyokiOffice/Word/WordDocument.hpp"
#include "ExyokiOffice/StandardTypes.hpp"

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

namespace
{
namespace W = ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing;

using ExyokiOffice::Color;
using ExyokiOffice::Word::Table;
using ExyokiOffice::Word::WordDocumentEditor;

template <typename TElement>
ExyokiOffice::Size CountDescendants(const std::shared_ptr<ExyokiOffice::OpenXMLElement>& root)
{
    return root ? root->Descendants<TElement>().size() : 0;
}

std::vector<ExyokiOffice::Size> PhysicalCellCounts(const std::shared_ptr<W::Table>& table)
{
    std::vector<ExyokiOffice::Size> counts;
    if (!table)
    {
        return counts;
    }

    for (const auto& row : table->Elements<W::TableRow>())
    {
        counts.push_back(row ? row->Elements<W::TableCell>().size() : 0);
    }
    return counts;
}

struct ReopenedTable
{
    std::shared_ptr<WordDocumentEditor> Editor;
    // Qualified: an unqualified `Table` would mean the type before this
    // declaration and the member after it, which GCC rejects.
    std::shared_ptr<ExyokiOffice::Word::Table> Table;
};

ReopenedTable ReopenFirstTable(const std::shared_ptr<WordDocumentEditor>& editor)
{
    const auto bytes = editor->SaveToMemory();
    REQUIRE(!bytes.empty());

    auto reopened = WordDocumentEditor::Open(bytes);
    REQUIRE(reopened != nullptr);

    auto tables = reopened->Tables();
    REQUIRE(tables.size() == 1);
    return {reopened, tables.front()};
}

} // namespace

TEST_SUITE("WordTableModelTests")
{

    TEST_CASE("Table logical grid represents rectangular merged cells without hMerge [unit] [word] [word-table-model]")
    {
        auto editor = WordDocumentEditor::CreateNew();
        REQUIRE(editor != nullptr);

        auto table = editor->AddTable(3, 3);
        REQUIRE(table != nullptr);
        table->SetCellText(0, 0, "origin");
        table->SetCellText(0, 2, "right");
        table->SetCellText(2, 2, "bottom");

        table->MergeCells(0, 0, 2, 2);

        auto grid = table->GetLogicalGrid();
        REQUIRE(grid.size() == 3);
        REQUIRE(grid[0].size() == 3);
        REQUIRE(grid[1].size() == 3);
        CHECK(table->GetColumnCount() == 3);
        CHECK(table->GetLogicalColumnCount() == 3);

        CHECK(grid[0][0].IsOrigin);
        CHECK(grid[0][0].RowSpan == 2);
        CHECK(grid[0][0].ColumnSpan == 2);
        CHECK_FALSE(grid[0][1].IsOrigin);
        CHECK(grid[0][1].OriginRow == 0);
        CHECK(grid[0][1].OriginColumn == 0);
        CHECK_FALSE(grid[1][0].IsOrigin);
        CHECK(grid[1][0].OriginRow == 0);
        CHECK(grid[1][0].OriginColumn == 0);

        auto lowTable = table->GetLowLevelApi();
        CHECK(PhysicalCellCounts(lowTable) == std::vector<ExyokiOffice::Size>{2, 2, 3});
        CHECK(CountDescendants<W::HorizontalMerge>(lowTable) == 0);
        CHECK(CountDescendants<W::GridSpan>(lowTable) == 2);
        CHECK(CountDescendants<W::VerticalMerge>(lowTable) == 2);

        auto reopened = ReopenFirstTable(editor);
        auto reopenedGrid = reopened.Table->GetLogicalGrid();
        REQUIRE(reopenedGrid.size() == 3);
        CHECK(reopenedGrid[0][0].RowSpan == 2);
        CHECK(reopenedGrid[0][0].ColumnSpan == 2);
        CHECK(CountDescendants<W::HorizontalMerge>(reopened.Table->GetLowLevelApi()) == 0);
    }

    TEST_CASE("Table split removes merge metadata and restores physical cells [unit] [word] [word-table-model]")
    {
        auto editor = WordDocumentEditor::CreateNew();
        REQUIRE(editor != nullptr);

        auto table = editor->AddTable(2, 3);
        REQUIRE(table != nullptr);
        table->MergeCells(0, 0, 2, 2);

        table->SplitCell(0, 1);

        auto grid = table->GetLogicalGrid();
        REQUIRE(grid.size() == 2);
        CHECK(grid[0].size() == 3);
        CHECK(grid[1].size() == 3);
        for (const auto& row : grid)
        {
            for (const auto& cell : row)
            {
                CHECK(cell.IsOrigin);
                CHECK(cell.RowSpan == 1);
                CHECK(cell.ColumnSpan == 1);
            }
        }

        auto lowTable = table->GetLowLevelApi();
        CHECK(PhysicalCellCounts(lowTable) == std::vector<ExyokiOffice::Size>{3, 3});
        CHECK(CountDescendants<W::GridSpan>(lowTable) == 0);
        CHECK(CountDescendants<W::HorizontalMerge>(lowTable) == 0);
        CHECK(CountDescendants<W::VerticalMerge>(lowTable) == 0);
    }

    TEST_CASE("Table insert and remove operations normalize merged cells [unit] [word] [word-table-model]")
    {
        auto editor = WordDocumentEditor::CreateNew();
        REQUIRE(editor != nullptr);

        auto table = editor->AddTable(2, 3);
        REQUIRE(table != nullptr);
        table->SetCellText(0, 0, "a");
        table->SetCellText(0, 1, "b");
        table->SetCellText(0, 2, "c");
        table->MergeCells(0, 1, 2, 2);

        table->InsertColumn(1);
        CHECK(table->GetRowCount() == 2);
        CHECK(table->GetLogicalColumnCount() == 4);
        CHECK(PhysicalCellCounts(table->GetLowLevelApi()) == std::vector<ExyokiOffice::Size>{4, 4});
        CHECK(CountDescendants<W::GridSpan>(table->GetLowLevelApi()) == 0);
        CHECK(CountDescendants<W::HorizontalMerge>(table->GetLowLevelApi()) == 0);
        CHECK(CountDescendants<W::VerticalMerge>(table->GetLowLevelApi()) == 0);

        table->InsertRow(1);
        CHECK(table->GetRowCount() == 3);
        CHECK(PhysicalCellCounts(table->GetLowLevelApi()) == std::vector<ExyokiOffice::Size>{4, 4, 4});

        table->RemoveColumn(2);
        CHECK(table->GetLogicalColumnCount() == 3);
        CHECK(PhysicalCellCounts(table->GetLowLevelApi()) == std::vector<ExyokiOffice::Size>{3, 3, 3});

        table->RemoveRow(0);
        CHECK(table->GetRowCount() == 2);
        CHECK(PhysicalCellCounts(table->GetLowLevelApi()) == std::vector<ExyokiOffice::Size>{3, 3});
    }

    TEST_CASE("Table nested tables are added to logical cells [unit] [word] [word-table-model]")
    {
        auto editor = WordDocumentEditor::CreateNew();
        REQUIRE(editor != nullptr);

        auto table = editor->AddTable(1, 2);
        REQUIRE(table != nullptr);
        table->SetCellText(0, 0, "outer");
        table->MergeCells(0, 0, 1, 2);

        auto nested = table->AddNestedTable(0, 1, 2, 2);
        REQUIRE(nested != nullptr);
        nested->SetCellText(0, 0, "nested");

        auto nestedTables = table->Tables();
        REQUIRE(nestedTables.size() == 1);
        CHECK(nestedTables.front()->GetRowCount() == 2);
        CHECK(nestedTables.front()->GetLogicalColumnCount() == 2);

        const auto paragraphs = table->Paragraphs();
        std::vector<std::string> texts;
        for (const auto& paragraph : paragraphs)
        {
            texts.push_back(paragraph->PlainText());
        }
        CHECK(texts == std::vector<std::string>{"outer", "nested", "", "", "", ""});

        auto reopened = ReopenFirstTable(editor);
        auto reopenedNested = reopened.Table->Tables();
        REQUIRE(reopenedNested.size() == 1);
        CHECK(reopenedNested.front()->GetRowCount() == 2);
        CHECK(reopenedNested.front()->GetLogicalColumnCount() == 2);
    }

    TEST_CASE("Table SetCellText replaces existing cell content and preserves cell properties [unit] [word] [word-table-model]")
    {
        auto editor = WordDocumentEditor::CreateNew();
        REQUIRE(editor != nullptr);

        auto table = editor->AddTable(1, 1);
        REQUIRE(table != nullptr);
        table->SetCellBackgroundColor(0, 0, Color(1, 2, 3));
        table->SetCellText(0, 0, "first");
        table->AppendCellText(0, 0, " second");

        auto nested = table->AddNestedTable(0, 0, 1, 1);
        REQUIRE(nested != nullptr);
        nested->SetCellText(0, 0, "nested");

        auto cell = table->GetLogicalGrid().front().front().Cell;
        REQUIRE(cell != nullptr);
        CHECK(CountDescendants<W::Run>(cell) == 3);
        CHECK(CountDescendants<W::Table>(cell) == 1);

        table->SetCellText(0, 0, " replacement ", true);

        cell = table->GetLogicalGrid().front().front().Cell;
        REQUIRE(cell != nullptr);
        CHECK(cell->Elements<W::Paragraph>().size() == 1);
        CHECK(CountDescendants<W::Run>(cell) == 1);
        auto texts = cell->Descendants<W::Text>();
        REQUIRE(texts.size() == 1);
        CHECK(texts.front()->GetText() == " replacement ");
        CHECK(CountDescendants<W::Table>(cell) == 0);

        auto props = cell->GetFirstChildOfType<W::TableCellProperties>();
        REQUIRE(props != nullptr);
        auto shading = props->GetFirstChildOfType<W::Shading>();
        REQUIRE(shading != nullptr);
        CHECK(shading->GetFill().ToString() == "010203");

        auto reopened = ReopenFirstTable(editor);
        auto reopenedCell = reopened.Table->GetLogicalGrid().front().front().Cell;
        REQUIRE(reopenedCell != nullptr);
        auto reopenedTexts = reopenedCell->Descendants<W::Text>();
        REQUIRE(reopenedTexts.size() == 1);
        CHECK(reopenedTexts.front()->GetText() == " replacement ");
        CHECK(CountDescendants<W::Table>(reopenedCell) == 0);
    }

    TEST_CASE("Table AppendCellText appends explicit runs without replacing content [unit] [word] [word-table-model]")
    {
        auto editor = WordDocumentEditor::CreateNew();
        REQUIRE(editor != nullptr);

        auto table = editor->AddTable(1, 1);
        REQUIRE(table != nullptr);
        table->AppendCellText(0, 0, "alpha");
        table->AppendCellText(0, 0, " beta", true);

        auto cell = table->GetLogicalGrid().front().front().Cell;
        REQUIRE(cell != nullptr);
        CHECK(cell->Elements<W::Paragraph>().size() == 1);
        CHECK(CountDescendants<W::Run>(cell) == 2);
        auto texts = cell->Descendants<W::Text>();
        REQUIRE(texts.size() == 2);
        CHECK(texts[0]->GetText() == "alpha");
        CHECK(texts[1]->GetText() == " beta");
        CHECK(table->Paragraphs().front()->PlainText() == "alpha beta");

        auto reopened = ReopenFirstTable(editor);
        auto reopenedCell = reopened.Table->GetLogicalGrid().front().front().Cell;
        REQUIRE(reopenedCell != nullptr);
        CHECK(CountDescendants<W::Run>(reopenedCell) == 2);
        CHECK(reopened.Table->Paragraphs().front()->PlainText() == "alpha beta");
    }

    TEST_CASE("Table SetCellText replaces the physical origin when addressing a covered logical cell [unit] [word] [word-table-model]")
    {
        auto editor = WordDocumentEditor::CreateNew();
        REQUIRE(editor != nullptr);

        auto table = editor->AddTable(1, 2);
        REQUIRE(table != nullptr);
        table->SetCellText(0, 0, "origin");
        table->AppendCellText(0, 0, " old");
        table->MergeCells(0, 0, 1, 2);

        table->SetCellText(0, 1, "covered");

        auto grid = table->GetLogicalGrid();
        REQUIRE(grid.size() == 1);
        REQUIRE(grid.front().size() == 2);
        CHECK(grid[0][0].Cell == grid[0][1].Cell);

        auto cell = grid[0][1].Cell;
        REQUIRE(cell != nullptr);
        auto texts = cell->Descendants<W::Text>();
        REQUIRE(texts.size() == 1);
        CHECK(texts.front()->GetText() == "covered");
        CHECK(CountDescendants<W::Run>(cell) == 1);
        CHECK(CountDescendants<W::GridSpan>(cell) == 1);

        auto reopened = ReopenFirstTable(editor);
        auto reopenedGrid = reopened.Table->GetLogicalGrid();
        REQUIRE(reopenedGrid.size() == 1);
        REQUIRE(reopenedGrid.front().size() == 2);
        auto reopenedTexts = reopenedGrid[0][1].Cell->Descendants<W::Text>();
        REQUIRE(reopenedTexts.size() == 1);
        CHECK(reopenedTexts.front()->GetText() == "covered");
    }

} // TEST_SUITE("WordTableModelTests")
