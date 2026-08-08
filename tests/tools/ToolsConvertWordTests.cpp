// Copyright (c) 2026 Jakub Melka and Collaborators
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "doctest.h"

#include "TestSupport.hpp"

#include "ExyokiOffice/Tools/DocumentConverter.hpp"
#include "ExyokiOffice/Tools/DocumentModelIO.hpp"
#include "ExyokiOffice/Word/WordDocument.hpp"
#include "ExyokiOffice/StandardTypes.hpp"

#include <chrono>
#include <fstream>

using namespace ExyokiOffice::Tools;
using ExyokiOffice::Word::WordDocumentEditor;

namespace
{

using ExyokiOfficeTests::MakeTemporaryPath;

std::filesystem::path SaveSampleDocument()
{
    auto editor = WordDocumentEditor::CreateNew();
    REQUIRE(editor);

    editor->AddHeading("Report title", 1);
    editor->AddHeading("Section", 2);

    auto paragraph = editor->AddParagraph();
    REQUIRE(paragraph);
    ExyokiOffice::Word::RunStyle bold;
    bold.Bold = true;
    paragraph->AddRun("bold text", bold);
    paragraph->AddText(" and plain ");
    paragraph->AddHyperlink("a link", "https://example.com");
    paragraph->AddFootnote("The footnote body");

    auto numbered = editor->EnsureNumberedListStyle("TestList");
    editor->AddParagraph("first item")->SetListStyle(numbered);
    editor->AddParagraph("second item")->SetListStyle(numbered);

    auto table = editor->AddTable(2, 2);
    REQUIRE(table);
    table->SetCellText(0, 0, "Name");
    table->SetCellText(0, 1, "Value");
    table->SetCellText(1, 0, "Answer");
    table->SetCellText(1, 1, "42");

    const auto path = MakeTemporaryPath("exyoki_convert_word", ".docx");
    REQUIRE(editor->SaveToFile(path));
    return path;
}

std::string ReadFileText(const std::filesystem::path& path)
{
    std::ifstream file(path, std::ios::binary);
    REQUIRE(file);
    return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

} // namespace

TEST_CASE("Word document converts to JSON and back with content intact [unit] [tools] [conversion]")
{
    const auto docxPath = SaveSampleDocument();
    const auto jsonPath = MakeTemporaryPath("exyoki_convert_word", ".json");
    const auto backPath = MakeTemporaryPath("exyoki_convert_word_back", ".docx");

    const auto toJson = ConvertDocument(docxPath, jsonPath);
    CHECK(toJson.Ok);
    CHECK(toJson.Family == DocumentFamily::Word);
    CHECK(toJson.From == ConvertFormat::Docx);
    CHECK(toJson.To == ConvertFormat::Json);
    CHECK(toJson.BlockCount > 0);

    const auto json = ReadFileText(jsonPath);
    CHECK(json.find("Report title") != std::string::npos);
    CHECK(json.find("\"heading\": 1") != std::string::npos);
    CHECK(json.find("https://example.com") != std::string::npos);
    CHECK(json.find("The footnote body") != std::string::npos);

    const auto toDocx = ConvertDocument(jsonPath, backPath);
    CHECK(toDocx.Ok);

    // Re-read the rebuilt document and verify the modeled content survived.
    std::vector<ToolDiagnostic> diagnostics;
    const auto model = ReadWordModel(backPath, {}, diagnostics);
    REQUIRE(model.Word);

    bool headingFound = false;
    bool boldFound = false;
    bool linkFound = false;
    ExyokiOffice::Size listItems = 0;
    bool tableFound = false;
    for (const auto& block : model.Word->Body)
    {
        if (block.Paragraph)
        {
            if (block.Paragraph->HeadingLevel == 1)
            {
                headingFound = true;
            }
            if (block.Paragraph->List)
            {
                ++listItems;
            }
            for (const auto& node : block.Paragraph->Inlines)
            {
                if (node.Kind == WordInline::Type::Text && node.Text == "bold text" && node.Props.Bold)
                {
                    boldFound = true;
                }
                if (node.Kind == WordInline::Type::Hyperlink && node.Target == "https://example.com")
                {
                    linkFound = true;
                }
            }
        }
        if (block.Table && block.Table->Rows.size() == 2)
        {
            tableFound = true;
        }
    }
    CHECK(headingFound);
    CHECK(boldFound);
    CHECK(linkFound);
    CHECK(listItems == 2);
    CHECK(tableFound);
    REQUIRE(model.Word->Footnotes.size() == 1);

    std::filesystem::remove(docxPath);
    std::filesystem::remove(jsonPath);
    std::filesystem::remove(backPath);
}

TEST_CASE("Word document converts to Markdown and back [unit] [tools] [conversion]")
{
    const auto docxPath = SaveSampleDocument();
    const auto markdownPath = MakeTemporaryPath("exyoki_convert_word", ".md");
    const auto backPath = MakeTemporaryPath("exyoki_convert_word_md_back", ".docx");

    const auto toMarkdown = ConvertDocument(docxPath, markdownPath);
    CHECK(toMarkdown.Ok);

    const auto markdown = ReadFileText(markdownPath);
    CHECK(markdown.find("# Report title") != std::string::npos);
    CHECK(markdown.find("## Section") != std::string::npos);
    CHECK(markdown.find("**bold text**") != std::string::npos);
    CHECK(markdown.find("[a link](https://example.com)") != std::string::npos);
    CHECK(markdown.find("1. first item") != std::string::npos);
    CHECK(markdown.find("| Name | Value |") != std::string::npos);
    CHECK(markdown.find("[^1]") != std::string::npos);

    const auto toDocx = ConvertDocument(markdownPath, backPath);
    CHECK(toDocx.Ok);

    std::vector<ToolDiagnostic> diagnostics;
    const auto model = ReadWordModel(backPath, {}, diagnostics);
    REQUIRE(model.Word);
    bool headingFound = false;
    ExyokiOffice::Size listItems = 0;
    for (const auto& block : model.Word->Body)
    {
        if (block.Paragraph && block.Paragraph->HeadingLevel == 1)
        {
            headingFound = true;
        }
        if (block.Paragraph && block.Paragraph->List)
        {
            ++listItems;
        }
    }
    CHECK(headingFound);
    CHECK(listItems == 2);
    REQUIRE(model.Word->Footnotes.size() == 1);

    std::filesystem::remove(docxPath);
    std::filesystem::remove(markdownPath);
    std::filesystem::remove(backPath);
}

TEST_CASE("Word document converts to plain text and semantic XML [unit] [tools] [conversion]")
{
    const auto docxPath = SaveSampleDocument();
    const auto textPath = MakeTemporaryPath("exyoki_convert_word", ".txt");
    const auto xmlPath = MakeTemporaryPath("exyoki_convert_word", ".xml");
    const auto backPath = MakeTemporaryPath("exyoki_convert_word_xml_back", ".docx");

    const auto toText = ConvertDocument(docxPath, textPath);
    CHECK(toText.Ok);
    const auto text = ReadFileText(textPath);
    CHECK(text.find("Report title") != std::string::npos);
    CHECK(text.find("Answer\t42") != std::string::npos);

    const auto toXml = ConvertDocument(docxPath, xmlPath);
    CHECK(toXml.Ok);
    const auto xml = ReadFileText(xmlPath);
    CHECK(xml.find("<eoDocument") != std::string::npos);
    CHECK(xml.find("family=\"word\"") != std::string::npos);

    const auto backFromXml = ConvertDocument(xmlPath, backPath);
    CHECK(backFromXml.Ok);
    std::vector<ToolDiagnostic> diagnostics;
    const auto model = ReadWordModel(backPath, {}, diagnostics);
    REQUIRE(model.Word);
    CHECK(!model.Word->Body.empty());

    std::filesystem::remove(docxPath);
    std::filesystem::remove(textPath);
    std::filesystem::remove(xmlPath);
    std::filesystem::remove(backPath);
}

TEST_CASE("Convert rejects unsupported format pairs [unit] [tools] [conversion]")
{
    const auto docxPath = SaveSampleDocument();
    const auto otherPath = MakeTemporaryPath("exyoki_convert_word_other", ".docx");

    const auto officeToOffice = ConvertDocument(docxPath, otherPath);
    CHECK(!officeToOffice.Ok);
    CHECK(!officeToOffice.UsageOk);

    const auto unknown = ConvertDocument(docxPath, MakeTemporaryPath("exyoki_convert", ".zip"));
    CHECK(!unknown.Ok);
    CHECK(!unknown.UsageOk);

    std::filesystem::remove(docxPath);
}
