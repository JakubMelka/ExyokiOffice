// Copyright (c) 2026 Jakub Melka and Contributors
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "doctest.h"

#include "ExyokiOffice/DOM/DocumentFormat/OpenXml/Wordprocessing.hpp"
#include "ExyokiOffice/Word/WordDocument.hpp"

#include <memory>
#include <string>
#include <vector>

namespace
{
using ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::Drawing;
using ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::ParagraphProperties;
using ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::SectionProperties;
using ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::TableCell;
using ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::TableRow;
using ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::Text;
using ExyokiOffice::Word::BodyBlockType;
using ExyokiOffice::Word::WordDocumentEditor;

void AddTrailingSectionProperties(const WordDocumentEditor::Ptr& editor)
{
    REQUIRE(editor != nullptr);
    REQUIRE(editor->GetDocument() != nullptr);

    auto mainPart = editor->GetDocument()->GetMainDocumentPart();
    REQUIRE(mainPart != nullptr);

    auto document = mainPart->GetTypedRootElement();
    REQUIRE(document != nullptr);

    auto body = document->GetFirstChildOfType<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::Body>();
    REQUIRE(body != nullptr);
    REQUIRE(body->AppendChild<SectionProperties>() != nullptr);
}

void AddParagraphSectionProperties(const std::shared_ptr<ExyokiOffice::Word::Paragraph>& paragraph)
{
    REQUIRE(paragraph != nullptr);
    auto lowLevel = paragraph->GetLowLevelApi();
    REQUIRE(lowLevel != nullptr);

    auto properties = lowLevel->GetFirstChildOfType<ParagraphProperties>();
    if (!properties)
    {
        properties = lowLevel->InsertChild<ParagraphProperties>();
    }
    REQUIRE(properties != nullptr);
    REQUIRE(properties->AppendChild<SectionProperties>() != nullptr);
}

std::vector<std::string> PlainTexts(const std::vector<std::shared_ptr<ExyokiOffice::Word::Paragraph>>& paragraphs)
{
    std::vector<std::string> result;
    for (const auto& paragraph : paragraphs)
    {
        result.push_back(paragraph ? paragraph->PlainText() : std::string{});
    }
    return result;
}

} // namespace

TEST_SUITE("WordReadWrapperTests")
{

    TEST_CASE("WordDocumentEditor read wrappers enumerate opened body content in document order [unit] [word] [word-read-wrapper]")
    {
        auto created = WordDocumentEditor::CreateNew();
        REQUIRE(created != nullptr);

        auto paragraph = created->AddParagraph();
        REQUIRE(paragraph != nullptr);
        auto firstRun = paragraph->AddRun();
        auto secondRun = paragraph->AddRun();
        REQUIRE(firstRun != nullptr);
        REQUIRE(secondRun != nullptr);
        REQUIRE(firstRun->AddText("Hello ", true) != nullptr);
        REQUIRE(secondRun->AddText("world") != nullptr);
        REQUIRE(secondRun->GetLowLevelApi()->AppendChild<Drawing>() != nullptr);

        auto table = created->AddTable(1, 2);
        REQUIRE(table != nullptr);
        table->SetCellText(0, 0, "Left");
        table->SetCellText(0, 1, "Right");
        AddTrailingSectionProperties(created);

        auto bytes = created->SaveToMemory();
        REQUIRE(!bytes.empty());

        auto opened = WordDocumentEditor::Open(bytes);
        REQUIRE(opened != nullptr);

        auto blocks = opened->BodyBlocks();
        REQUIRE(blocks.size() == 3);
        CHECK(blocks[0].Type() == BodyBlockType::Paragraph);
        CHECK(blocks[1].Type() == BodyBlockType::Table);
        CHECK(blocks[2].Type() == BodyBlockType::Section);
        REQUIRE(blocks[0].AsParagraph() != nullptr);
        REQUIRE(blocks[1].AsTable() != nullptr);
        REQUIRE(blocks[2].AsSection() != nullptr);
        CHECK(blocks[2].AsSection()->IsFinalBodySection());

        auto paragraphs = opened->Paragraphs();
        REQUIRE(paragraphs.size() == 1);
        CHECK(paragraphs[0]->PlainText() == "Hello world");
        CHECK(PlainTexts(opened->Paragraphs()) == std::vector<std::string>{"Hello world"});

        auto runs = paragraphs[0]->Runs();
        REQUIRE(runs.size() == 2);
        CHECK(runs[0]->PlainText() == "Hello ");
        CHECK(runs[1]->PlainText() == "world");
        CHECK(paragraphs[0]->Texts().size() == 2);
        CHECK(paragraphs[0]->Images().size() == 1);
        CHECK(runs[1]->Images().size() == 1);

        auto tables = opened->Tables();
        REQUIRE(tables.size() == 1);
        CHECK(tables[0]->GetRowCount() == 1);
        CHECK(tables[0]->GetColumnCount() == 2);

        auto texts = paragraphs[0]->Texts();
        REQUIRE(texts.size() == 2);
        texts[1]->SetText("WRD-003");
        REQUIRE(opened->After(paragraphs[0]).InsertParagraph("Inserted through read wrapper") != nullptr);

        auto editedBytes = opened->SaveToMemory();
        REQUIRE(!editedBytes.empty());

        auto reopened = WordDocumentEditor::Open(editedBytes);
        REQUIRE(reopened != nullptr);
        CHECK(PlainTexts(reopened->Paragraphs()) == std::vector<std::string>{"Hello WRD-003", "Inserted through read wrapper"});
    }

    TEST_CASE("Table read wrappers enumerate cell paragraphs and nested tables [unit] [word] [word-read-wrapper]")
    {
        auto editor = WordDocumentEditor::CreateNew();
        REQUIRE(editor != nullptr);

        auto table = editor->AddTable(1, 1);
        REQUIRE(table != nullptr);
        table->SetCellText(0, 0, "Outer");

        auto lowTable = table->GetLowLevelApi();
        REQUIRE(lowTable != nullptr);
        auto rows = lowTable->Elements<TableRow>();
        REQUIRE(rows.size() == 1);
        auto cells = rows[0]->Elements<TableCell>();
        REQUIRE(cells.size() == 1);

        auto nested = cells[0]->AppendChild<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::Table>();
        REQUIRE(nested != nullptr);
        auto nestedRow = nested->AppendChild<TableRow>();
        REQUIRE(nestedRow != nullptr);
        auto nestedCell = nestedRow->AppendChild<TableCell>();
        REQUIRE(nestedCell != nullptr);
        auto nestedParagraph = nestedCell->AppendChild<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::Paragraph>();
        REQUIRE(nestedParagraph != nullptr);
        auto nestedRun = nestedParagraph->AppendChild<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::Run>();
        REQUIRE(nestedRun != nullptr);
        auto nestedText = nestedRun->AppendChild<Text>();
        REQUIRE(nestedText != nullptr);
        nestedText->SetText("Nested");

        auto bytes = editor->SaveToMemory();
        REQUIRE(!bytes.empty());

        auto opened = WordDocumentEditor::Open(bytes);
        REQUIRE(opened != nullptr);
        auto tables = opened->Tables();
        REQUIRE(tables.size() == 1);

        CHECK(tables[0]->Tables().size() == 1);
        CHECK(PlainTexts(tables[0]->Paragraphs()) == std::vector<std::string>{"Outer", "Nested"});
    }

    TEST_CASE("Section read wrappers enumerate paragraph and final body section properties [unit] [word] [word-read-wrapper]")
    {
        auto editor = WordDocumentEditor::CreateNew();
        REQUIRE(editor != nullptr);

        auto firstSectionEnd = editor->AddParagraph("First section");
        REQUIRE(firstSectionEnd != nullptr);
        AddParagraphSectionProperties(firstSectionEnd);
        AddTrailingSectionProperties(editor);

        auto sections = editor->Sections();
        REQUIRE(sections.size() == 2);
        CHECK_FALSE(sections[0]->IsFinalBodySection());
        CHECK(sections[1]->IsFinalBodySection());

        auto bytes = editor->SaveToMemory();
        REQUIRE(!bytes.empty());

        auto opened = WordDocumentEditor::Open(bytes);
        REQUIRE(opened != nullptr);
        auto reopenedSections = opened->Sections();
        REQUIRE(reopenedSections.size() == 2);
        CHECK_FALSE(reopenedSections[0]->IsFinalBodySection());
        CHECK(reopenedSections[1]->IsFinalBodySection());
    }

    TEST_CASE("Read wrappers are empty for invalid wrapper instances [unit] [word] [word-read-wrapper]")
    {
        ExyokiOffice::Word::Paragraph paragraph(nullptr);
        CHECK(paragraph.Runs().empty());
        CHECK(paragraph.Texts().empty());
        CHECK(paragraph.Images().empty());
        CHECK(paragraph.PlainText().empty());

        ExyokiOffice::Word::Run run(nullptr);
        CHECK(run.Texts().empty());
        CHECK(run.Images().empty());
        CHECK(run.PlainText().empty());

        ExyokiOffice::Word::Table table(nullptr);
        CHECK(table.Paragraphs().empty());
        CHECK(table.Tables().empty());

        ExyokiOffice::Word::Section section(nullptr);
        CHECK(section.GetLowLevelApi() == nullptr);
        CHECK_FALSE(section.IsFinalBodySection());

        ExyokiOffice::Word::BodyBlock block;
        CHECK(block.Type() == BodyBlockType::Unsupported);
        CHECK(block.GetLowLevelApi() == nullptr);
        CHECK(block.AsParagraph() == nullptr);
        CHECK(block.AsTable() == nullptr);
        CHECK(block.AsSection() == nullptr);

        WordDocumentEditor editor;
        CHECK(editor.BodyBlocks().empty());
        CHECK(editor.Paragraphs().empty());
        CHECK(editor.Tables().empty());
        CHECK(editor.Sections().empty());
    }

} // TEST_SUITE("WordReadWrapperTests")
