// Copyright (c) 2026 Jakub Melka and Collaborators
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "doctest.h"

#include "TestSupport.hpp"

#include "ExyokiOffice/Tools/WordTextTools.hpp"
#include "ExyokiOffice/Word/WordDocument.hpp"

#include <algorithm>
#include <chrono>
#include <fstream>

using ExyokiOffice::Word::HeaderFooterType;
using ExyokiOffice::Word::WordDocumentEditor;
using namespace ExyokiOffice::Tools;

namespace
{

std::filesystem::path MakeTemporaryPath(std::string_view stem)
{
    return ExyokiOfficeTests::MakeTemporaryPath(stem, ".docx");
}

std::filesystem::path BuildFixtureDocument()
{
    auto editor = WordDocumentEditor::CreateNew();
    REQUIRE(editor);

    editor->AddParagraph("Alpha appears in the body.");

    auto table = editor->AddTable(1, 1);
    REQUIRE(table);
    table->SetCellText(0, 0, "Alpha appears in a table cell.");

    auto section = editor->EnsureFinalSection();
    REQUIRE(section);
    section->SetHeaderText(HeaderFooterType::Default, "Alpha appears in the header.");
    section->SetFooterText(HeaderFooterType::Default, "Alpha appears in the footer.");

    auto paragraph = editor->AddParagraph("See the note.");
    paragraph->AddFootnote("Alpha appears in a footnote.");

    const auto path = MakeTemporaryPath("exyoki_word_text");
    REQUIRE(editor->SaveToFile(path));
    return path;
}

} // namespace

TEST_CASE("Search finds matches across body, table, header, footer, and footnotes [unit] [tools]")
{
    const auto path = BuildFixtureDocument();
    const auto result = Search(path, "Alpha", 10);
    CHECK(result.Ok);
    // One "Alpha" occurrence in each of body, table, header, footer, and footnote scope.
    CHECK(result.Matches.size() == 5);

    auto hasScope = [&](TextScope scope)
    {
        return std::any_of(result.Matches.begin(), result.Matches.end(),
                           [scope](const auto& match)
                           { return match.Scope == scope; });
    };
    CHECK(hasScope(TextScope::Body));
    CHECK(hasScope(TextScope::Table));
    CHECK(hasScope(TextScope::Header));
    CHECK(hasScope(TextScope::Footer));
    CHECK(hasScope(TextScope::Footnote));

    for (const auto& match : result.Matches)
    {
        CHECK(match.MatchText == "Alpha");
        CHECK(match.Offset == 0);
        CHECK(match.Context.find("Alpha") != std::string::npos);
    }

    std::filesystem::remove(path);
}

TEST_CASE("Search rejects a non-Word document [unit] [tools]")
{
    const auto path = MakeTemporaryPath("exyoki_word_text_not_docx");
    {
        std::ofstream file(path, std::ios::binary);
        file << "not a zip";
    }
    const auto result = Search(path, "Alpha");
    CHECK_FALSE(result.Ok);
    CHECK(!result.Diagnostics.empty());
    std::filesystem::remove(path);
}

TEST_CASE("ExtractText returns a block per paragraph across every scope [unit] [tools]")
{
    const auto path = BuildFixtureDocument();
    const auto result = ExtractText(path);
    CHECK(result.Ok);

    // Footnotes parts carry extra bookkeeping (Separator/ContinuationSeparator) paragraphs
    // beyond the one normal footnote, so only assert a lower bound plus scope coverage here;
    // the exact "Alpha" count per scope is covered precisely by the Search test above.
    CHECK(result.Blocks.size() >= 6);

    auto hasScopeWithText = [&](TextScope scope, std::string_view text)
    {
        return std::any_of(result.Blocks.begin(), result.Blocks.end(), [&](const auto& block)
                           { return block.Scope == scope && block.Text.find(text) != std::string::npos; });
    };
    CHECK(hasScopeWithText(TextScope::Body, "Alpha appears in the body."));
    CHECK(hasScopeWithText(TextScope::Table, "Alpha appears in a table cell."));
    CHECK(hasScopeWithText(TextScope::Header, "Alpha appears in the header."));
    CHECK(hasScopeWithText(TextScope::Footer, "Alpha appears in the footer."));
    CHECK(hasScopeWithText(TextScope::Footnote, "Alpha appears in a footnote."));

    std::filesystem::remove(path);
}

TEST_CASE("Replace dry-run count matches the wet count, and only the wet run saves [unit] [tools]")
{
    const auto path = BuildFixtureDocument();

    const auto dryRun = Replace(path, "Alpha", "Beta", true);
    CHECK(dryRun.Ok);
    CHECK(dryRun.ReplacementCount == 5);
    CHECK_FALSE(dryRun.Saved);

    // Dry run must not have modified the document.
    const auto stillAlpha = Search(path, "Alpha");
    CHECK(stillAlpha.Matches.size() == 5);

    const auto outputPath = MakeTemporaryPath("exyoki_word_text_replaced");
    const auto wetRun = Replace(path, "Alpha", "Beta", false, outputPath);
    CHECK(wetRun.Ok);
    CHECK(wetRun.ReplacementCount == dryRun.ReplacementCount);
    CHECK(wetRun.Saved);

    const auto afterReplace = Search(outputPath, "Beta");
    CHECK(afterReplace.Matches.size() == 5);
    const auto noMoreAlpha = Search(outputPath, "Alpha");
    CHECK(noMoreAlpha.Matches.empty());

    std::filesystem::remove(path);
    std::filesystem::remove(outputPath);
}

TEST_CASE("Search with useRegex=true matches an ECMAScript pattern across scopes [unit] [tools]")
{
    const auto path = BuildFixtureDocument();
    const auto result = Search(path, "Alph.", 10, /*useRegex=*/true);
    CHECK(result.Ok);
    CHECK(result.Matches.size() == 5);
    for (const auto& match : result.Matches)
    {
        CHECK(match.MatchText == "Alpha");
    }

    std::filesystem::remove(path);
}

TEST_CASE("Search with ignoreCase=true matches regardless of case [unit] [tools]")
{
    const auto path = BuildFixtureDocument();

    const auto caseSensitive = Search(path, "alpha");
    CHECK(caseSensitive.Ok);
    CHECK(caseSensitive.Matches.empty());

    const auto ignoreCase = Search(path, "alpha", 10, /*useRegex=*/false, /*ignoreCase=*/true);
    CHECK(ignoreCase.Ok);
    CHECK(ignoreCase.Matches.size() == 5);

    std::filesystem::remove(path);
}

TEST_CASE("Replace with useRegex=true substitutes capture groups and reports the count [unit] [tools]")
{
    const auto path = BuildFixtureDocument();
    const auto outputPath = MakeTemporaryPath("exyoki_word_text_regex_replaced");

    const auto result = Replace(path, "(Alph)a", "[$1]", false, outputPath, /*useRegex=*/true);
    CHECK(result.Ok);
    CHECK(result.ReplacementCount == 5);
    CHECK(result.Saved);

    const auto afterReplace = Search(outputPath, "[Alph]");
    CHECK(afterReplace.Matches.size() == 5);

    std::filesystem::remove(path);
    std::filesystem::remove(outputPath);
}

TEST_CASE("Search and Replace reject an invalid regular expression with a diagnostic [unit] [tools]")
{
    const auto path = BuildFixtureDocument();

    const auto searchResult = Search(path, "(unclosed", 10, /*useRegex=*/true);
    CHECK_FALSE(searchResult.Ok);
    CHECK(!searchResult.Diagnostics.empty());

    const auto replaceResult = Replace(path, "(unclosed", "x", false, {}, /*useRegex=*/true);
    CHECK_FALSE(replaceResult.Ok);
    CHECK(!replaceResult.Diagnostics.empty());

    std::filesystem::remove(path);
}
