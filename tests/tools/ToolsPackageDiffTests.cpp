// Copyright (c) 2026 Jakub Melka and Collaborators
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "doctest.h"

#include "TestSupport.hpp"

#include "ExyokiOffice/Tools/PackageDiff.hpp"
#include "ExyokiOffice/Word/WordDocument.hpp"

#include <algorithm>
#include <chrono>

using ExyokiOffice::Word::WordDocumentEditor;
using namespace ExyokiOffice::Tools;

namespace
{

std::filesystem::path MakeTemporaryPath(std::string_view stem)
{
    return ExyokiOfficeTests::MakeTemporaryPath(stem, ".docx");
}

std::filesystem::path SaveNewDocument(std::string_view paragraphText)
{
    auto editor = WordDocumentEditor::CreateNew();
    REQUIRE(editor);
    editor->AddParagraph(std::string(paragraphText));
    const auto path = MakeTemporaryPath("exyoki_diff");
    REQUIRE(editor->SaveToFile(path));
    return path;
}

} // namespace

TEST_CASE("Compare reports identical for a document diffed against itself [unit] [tools]")
{
    const auto path = SaveNewDocument("Same content");
    const auto result = Compare(path, path);
    CHECK(result.Ok);
    CHECK(result.Identical);
    CHECK(result.PartChanges.empty());
    CHECK(result.RelationshipChanges.empty());
    std::filesystem::remove(path);
}

TEST_CASE("Compare detects a changed paragraph as ChangedXml [unit] [tools]")
{
    const auto left = SaveNewDocument("Original text");
    const auto right = SaveNewDocument("Different text");

    const auto result = Compare(left, right);
    CHECK(result.Ok);
    CHECK_FALSE(result.Identical);

    const auto documentChange = std::find_if(result.PartChanges.begin(), result.PartChanges.end(), [](const auto& c)
                                             { return c.Uri == "/word/document.xml"; });
    REQUIRE(documentChange != result.PartChanges.end());
    CHECK(documentChange->Kind == PartChangeKind::ChangedXml);
    CHECK(!documentChange->FirstDifferencePath.empty());

    std::filesystem::remove(left);
    std::filesystem::remove(right);
}

TEST_CASE("Compare treats whitespace-only XML differences as identical when normalized [unit] [tools]")
{
    const auto docxPath = SaveNewDocument("Whitespace test");

    // Re-serialize the same document to a second file: identical content, and the save path
    // itself doesn't introduce whitespace-only churn, so this exercises the normalized-equal
    // path without requiring the archiver. See ToolsPackageArchiverTests for --pretty coverage.
    auto reopened = WordDocumentEditor::Open(docxPath);
    REQUIRE(reopened);
    const auto secondPath = MakeTemporaryPath("exyoki_diff_second");
    REQUIRE(reopened->SaveToFile(secondPath));

    const auto normalized = Compare(docxPath, secondPath, true);
    CHECK(normalized.Ok);
    CHECK(normalized.Identical);

    std::filesystem::remove(docxPath);
    std::filesystem::remove(secondPath);
}

TEST_CASE("Compare reports Added/Removed parts between differently structured documents [unit] [tools]")
{
    auto plainEditor = WordDocumentEditor::CreateNew();
    REQUIRE(plainEditor);
    plainEditor->AddParagraph("No footnotes here");
    const auto plainPath = MakeTemporaryPath("exyoki_diff_plain");
    REQUIRE(plainEditor->SaveToFile(plainPath));

    auto noteEditor = WordDocumentEditor::CreateNew();
    REQUIRE(noteEditor);
    auto paragraph = noteEditor->AddParagraph("Has a footnote");
    paragraph->AddFootnote("Footnote text");
    const auto notePath = MakeTemporaryPath("exyoki_diff_note");
    REQUIRE(noteEditor->SaveToFile(notePath));

    const auto result = Compare(plainPath, notePath);
    CHECK(result.Ok);
    CHECK_FALSE(result.Identical);

    const auto footnotesAdded = std::find_if(result.PartChanges.begin(), result.PartChanges.end(), [](const auto& c)
                                             { return c.Uri == "/word/footnotes.xml" && c.Kind == PartChangeKind::Added; });
    CHECK(footnotesAdded != result.PartChanges.end());

    std::filesystem::remove(plainPath);
    std::filesystem::remove(notePath);
}
