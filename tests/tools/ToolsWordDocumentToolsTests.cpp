// Copyright (c) 2026 Jakub Melka and Contributors
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "doctest.h"

#include "TestSupport.hpp"

#include "ExyokiOffice/Tools/WordDocumentTools.hpp"
#include "ExyokiOffice/Excel/ExcelDocument.hpp"
#include "ExyokiOffice/Word/WordDocument.hpp"

#include <filesystem>

namespace
{
using namespace ExyokiOffice::Tools;
using ExyokiOffice::Word::WordDocumentEditor;

std::filesystem::path TempPath(std::string_view name)
{
    const std::filesystem::path parts(name);
    return ExyokiOfficeTests::MakeTemporaryPath(parts.stem().string(), parts.extension().string());
}
} // namespace

TEST_CASE("Word document tools split by paragraph count and round-trip [unit] [tools] [word-document-tools]")
{
    const auto input = TempPath("exyokioffice_split_source.docx");
    const auto output = TempPath("exyokioffice_split_parts");
    std::filesystem::remove_all(output);
    auto editor = WordDocumentEditor::CreateNew();
    REQUIRE(editor != nullptr);
    editor->AddParagraph("one");
    editor->AddParagraph("two");
    editor->AddParagraph("three");
    REQUIRE(editor->SaveToFile(input));

    WordSplitOptions options;
    options.Strategy = WordSplitStrategy::ParagraphCount;
    options.ParagraphsPerDocument = 2;
    const auto result = SplitWordDocument(input, output, options);
    REQUIRE(result.Ok);
    REQUIRE(result.OutputFiles.size() == 2);
    auto first = WordDocumentEditor::Open(result.OutputFiles[0]);
    auto second = WordDocumentEditor::Open(result.OutputFiles[1]);
    REQUIRE(first != nullptr);
    REQUIRE(second != nullptr);
    REQUIRE(first->Paragraphs().size() == 2);
    REQUIRE(second->Paragraphs().size() == 1);
    CHECK(first->Paragraphs()[0]->PlainText() == "one");
    CHECK(second->Paragraphs()[0]->PlainText() == "three");

    std::filesystem::remove(input);
    std::filesystem::remove_all(output);
}

TEST_CASE("Word document tools merge in order with optional separators [unit] [tools] [word-document-tools]")
{
    const auto firstPath = TempPath("exyokioffice_merge_first.docx");
    const auto secondPath = TempPath("exyokioffice_merge_second.docx");
    const auto outputPath = TempPath("exyokioffice_merged.docx");
    std::filesystem::remove(outputPath);
    auto first = WordDocumentEditor::CreateNew();
    auto second = WordDocumentEditor::CreateNew();
    REQUIRE(first != nullptr);
    REQUIRE(second != nullptr);
    first->AddParagraph("first");
    second->AddParagraph("second");
    REQUIRE(first->SaveToFile(firstPath));
    REQUIRE(second->SaveToFile(secondPath));

    WordMergeOptions options;
    options.InsertPageBreaks = false;
    const auto result = MergeWordDocuments({firstPath, secondPath}, outputPath, options);
    REQUIRE(result.Ok);
    CHECK(result.DocumentsMerged == 2);
    auto merged = WordDocumentEditor::Open(outputPath);
    REQUIRE(merged != nullptr);
    REQUIRE(merged->Paragraphs().size() == 2);
    CHECK(merged->Paragraphs()[0]->PlainText() == "first");
    CHECK(merged->Paragraphs()[1]->PlainText() == "second");

    std::filesystem::remove(firstPath);
    std::filesystem::remove(secondPath);
    std::filesystem::remove(outputPath);
}

TEST_CASE("Word document tools refuse a split prefix that leaves the output directory [unit] [tools] [word-document-tools]")
{
    const auto input = TempPath("exyokioffice_split_prefix.docx");
    const auto output = TempPath("exyokioffice_split_prefix_parts");
    std::filesystem::remove_all(output);
    auto editor = WordDocumentEditor::CreateNew();
    REQUIRE(editor != nullptr);
    editor->AddParagraph("one");
    editor->AddParagraph("two");
    REQUIRE(editor->SaveToFile(input));

    // PartPath() concatenates the prefix into every output name, so this decides
    // where the files land, not just what they are called.
    for (const std::string prefix : {"../evil", "sub/evil", R"(..\evil)", "NUL"})
    {
        WordSplitOptions options;
        options.OutputPrefix = prefix;
        const auto result = SplitWordDocument(input, output, options);
        CHECK_FALSE(result.Ok);
        CHECK(result.OutputFiles.empty());
        REQUIRE_FALSE(result.Diagnostics.empty());
        CHECK(result.Diagnostics.back().Message.find("plain file name") != std::string::npos);
    }

    CHECK_FALSE(std::filesystem::exists(output.parent_path() / "evil_01.docx"));

    WordSplitOptions accepted;
    accepted.OutputPrefix = "chapter";
    const auto result = SplitWordDocument(input, output, accepted);
    CHECK(result.Ok);
    CHECK_FALSE(result.OutputFiles.empty());

    std::filesystem::remove(input);
    std::filesystem::remove_all(output);
}

TEST_CASE("Word document tools report a mismatched package family [unit] [tools] [word-document-tools]")
{
    const auto input = TempPath("exyokioffice_split_wrong_family.xlsx");
    const auto output = TempPath("exyokioffice_split_wrong_family_parts");
    std::filesystem::remove_all(output);
    auto spreadsheet = ExyokiOffice::Excel::ExcelDocumentEditor::CreateNew();
    REQUIRE(spreadsheet != nullptr);
    REQUIRE(spreadsheet->SaveToFile(input));

    const auto result = SplitWordDocument(input, output);
    CHECK_FALSE(result.Ok);
    REQUIRE(result.Diagnostics.size() == 1);
    CHECK(result.Diagnostics[0].Message.find("Expected a Word document") != std::string::npos);
    CHECK(result.Diagnostics[0].Message.find("excel") != std::string::npos);

    std::filesystem::remove(input);
    std::filesystem::remove_all(output);
}
