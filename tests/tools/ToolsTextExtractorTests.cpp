// Copyright (c) 2026 Jakub Melka and Contributors
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "doctest.h"

#include "TestSupport.hpp"

#include "ExyokiOffice/Excel/ExcelDocument.hpp"
#include "ExyokiOffice/PowerPoint/PowerPointDocument.hpp"
#include "ExyokiOffice/Tools/TextExtractor.hpp"
#include "ExyokiOffice/Word/WordDocument.hpp"

#include <algorithm>
#include <chrono>
#include <string>
#include <vector>

using namespace ExyokiOffice::Tools;

namespace
{

using ExyokiOfficeTests::MakeTemporaryPath;

bool AnyBlockContains(const ExtractedDocumentText& result, std::string_view text)
{
    return std::any_of(result.Blocks.begin(), result.Blocks.end(),
                       [text](const auto& block)
                       { return block.Text.find(text) != std::string::npos; });
}

} // namespace

TEST_CASE("Extract dispatches Word documents to WordTextTools [unit] [tools] [conversion]")
{
    using ExyokiOffice::Word::WordDocumentEditor;

    auto editor = WordDocumentEditor::CreateNew();
    REQUIRE(editor);
    editor->AddParagraph("Word extraction check");
    const auto path = MakeTemporaryPath("exyoki_extract_word", ".docx");
    REQUIRE(editor->SaveToFile(path));

    const auto result = Extract(path);
    CHECK(result.Ok);
    CHECK(result.Family == DocumentFamily::Word);
    CHECK(AnyBlockContains(result, "Word extraction check"));

    std::filesystem::remove(path);
}

TEST_CASE("Extract dispatches Excel workbooks and resolves shared strings [unit] [tools] [conversion]")
{
    using ExyokiOffice::Excel::ExcelDocumentEditor;

    auto editor = ExcelDocumentEditor::CreateNew();
    REQUIRE(editor);
    auto sheet = editor->FirstWorksheet();
    REQUIRE(sheet);
    REQUIRE(sheet->SetCellText(1, 1, "Excel extraction check"));

    const auto path = MakeTemporaryPath("exyoki_extract_excel", ".xlsx");
    REQUIRE(editor->SaveToFile(path));

    const auto result = Extract(path);
    CHECK(result.Ok);
    CHECK(result.Family == DocumentFamily::Excel);
    REQUIRE(!result.Blocks.empty());
    CHECK(AnyBlockContains(result, "Excel extraction check"));
    CHECK(result.Blocks.front().Label.find('!') != std::string::npos);

    std::filesystem::remove(path);
}

TEST_CASE("Extract dispatches PowerPoint presentations to shapes and notes [unit] [tools] [conversion]")
{
    using namespace ExyokiOffice::PowerPoint;

    auto editor = PowerPointDocumentEditor::CreateNew();
    REQUIRE(editor);
    auto slide = editor->AddSlide();
    REQUIRE(slide);
    auto tree = slide->ShapeTree();
    REQUIRE(tree);
    auto shape = tree->AddShape("Title 1");
    REQUIRE(shape);

    PresentationTextFrame frame;
    PresentationTextParagraph paragraph;
    PresentationTextRun run;
    run.Text = "PowerPoint extraction check";
    paragraph.Runs.push_back(run);
    frame.Paragraphs.push_back(paragraph);
    REQUIRE(shape->SetTextFrame(frame));

    slide->SetNotesText("Speaker notes check");

    const auto path = MakeTemporaryPath("exyoki_extract_pptx", ".pptx");
    REQUIRE(editor->SaveToFile(path));

    const auto result = Extract(path);
    CHECK(result.Ok);
    CHECK(result.Family == DocumentFamily::PowerPoint);
    CHECK(AnyBlockContains(result, "PowerPoint extraction check"));
    CHECK(AnyBlockContains(result, "Speaker notes check"));

    std::filesystem::remove(path);
}

TEST_CASE("Grouped shapes and tables are extracted too [unit] [tools] [text-extract]")
{
    // A `p:grpSp` holds no text of its own, so walking only the direct children
    // of the shape tree made every shape inside a group vanish - and grouping
    // is what a deck author does to move a set of labels together. A table is a
    // `p:graphicFrame`, not a shape with a text frame, and disappeared for a
    // second reason.
    using namespace ExyokiOffice::PowerPoint;

    auto editor = PowerPointDocumentEditor::CreateNew();
    REQUIRE(editor);
    auto slide = editor->AddSlide();
    REQUIRE(slide);
    auto tree = slide->ShapeTree();
    REQUIRE(tree);

    const auto setText = [](const PresentationShape::Ptr& shape, std::string_view text)
    {
        PresentationTextFrame frame;
        PresentationTextParagraph paragraph;
        PresentationTextRun run;
        run.Text = std::string(text);
        paragraph.Runs.push_back(run);
        frame.Paragraphs.push_back(paragraph);
        REQUIRE(shape->SetTextFrame(frame));
    };

    auto first = tree->AddShape("Grouped 1");
    REQUIRE(first);
    setText(first, "Text inside a group");
    auto second = tree->AddShape("Grouped 2");
    REQUIRE(second);
    setText(second, "Also inside the group");

    const auto before = tree->Shapes().size();
    REQUIRE(before >= 2);
    auto group = tree->Group({before - 2, before - 1});
    REQUIRE(group);
    REQUIRE(group->IsGroup());

    PresentationTableData table;
    table.ColumnWidths = {ExyokiOffice::MeasuringUnits(3.0, ExyokiOffice::MeasurementUnit::Centimeter)};
    PresentationTableRow row;
    row.Height = ExyokiOffice::MeasuringUnits(1.0, ExyokiOffice::MeasurementUnit::Centimeter);
    row.Cells.push_back(PresentationTableCell{"Text inside a table"});
    table.Rows.push_back(row);
    REQUIRE(tree->AddTable(table));

    const auto path = MakeTemporaryPath("exyoki_extract_groups", ".pptx");
    REQUIRE(editor->SaveToFile(path));

    const auto result = Extract(path);
    CHECK(result.Ok);
    CHECK(AnyBlockContains(result, "Text inside a group"));
    CHECK(AnyBlockContains(result, "Also inside the group"));
    CHECK(AnyBlockContains(result, "Text inside a table"));

    std::filesystem::remove(path);
}

TEST_CASE("An empty table cell keeps its column in the extract [unit] [tools] [text-extract]")
{
    // The tabs of a row were emitted only once something had been written, so
    // every leading empty cell lost its separator: {"", "Q2"} extracted as `Q2`,
    // which is what a one-column table looks like. Which column a value sits in
    // is the whole point of a tab-separated extract.
    using namespace ExyokiOffice::PowerPoint;

    auto editor = PowerPointDocumentEditor::CreateNew();
    REQUIRE(editor);
    auto slide = editor->AddSlide();
    REQUIRE(slide);
    auto tree = slide->ShapeTree();
    REQUIRE(tree);

    const auto centimeters = [](double value)
    { return ExyokiOffice::MeasuringUnits(value, ExyokiOffice::MeasurementUnit::Centimeter); };

    PresentationTableData table;
    table.ColumnWidths = {centimeters(3.0), centimeters(3.0), centimeters(3.0)};

    // Empty first, empty middle and empty last cell, one per row.
    const std::vector<std::vector<std::string>> rows = {
        {"", "Leading", "Gap"}, {"Middle", "", "Gap"}, {"Trailing", "Gap", ""}};
    for (const auto& cells : rows)
    {
        PresentationTableRow row;
        row.Height = centimeters(1.0);
        for (const auto& cell : cells)
        {
            row.Cells.push_back(PresentationTableCell{cell});
        }
        table.Rows.push_back(row);
    }
    REQUIRE(tree->AddTable(table));

    const auto path = MakeTemporaryPath("exyoki_extract_empty_cells", ".pptx");
    REQUIRE(editor->SaveToFile(path));

    const auto result = Extract(path);
    CHECK(result.Ok);
    CHECK(AnyBlockContains(result, "\tLeading\tGap"));
    CHECK(AnyBlockContains(result, "Middle\t\tGap"));
    CHECK(AnyBlockContains(result, "Trailing\tGap\t"));

    std::filesystem::remove(path);
}

TEST_CASE("Word content inside a structured document tag is extracted [unit] [tools] [text-extract]")
{
    // A cover page, a table of contents and a form field are all block-level
    // `w:sdt` wrappers. Enumerating only the direct children of the body made
    // an extract that started at the first heading and looked complete.
    using ExyokiOffice::Word::WordDocumentEditor;

    auto editor = WordDocumentEditor::CreateNew();
    REQUIRE(editor);
    editor->AddParagraph("Ordinary body paragraph");

    auto control = editor->Body().InsertContentControl("coverTag", "Cover Page");
    REQUIRE(control);
    control->SetText("Text inside a content control");

    const auto path = MakeTemporaryPath("exyoki_extract_sdt", ".docx");
    REQUIRE(editor->SaveToFile(path));

    const auto result = Extract(path);
    CHECK(result.Ok);
    CHECK(AnyBlockContains(result, "Ordinary body paragraph"));
    CHECK(AnyBlockContains(result, "Text inside a content control"));

    std::filesystem::remove(path);
}
