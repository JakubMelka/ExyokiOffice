// Copyright (c) 2026 Jakub Melka and Collaborators
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
