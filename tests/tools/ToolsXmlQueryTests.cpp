// Copyright (c) 2026 Jakub Melka and Collaborators
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "doctest.h"

#include "TestSupport.hpp"

#include "ExyokiOffice/Tools/XmlQueryTool.hpp"
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

std::filesystem::path BuildFixtureDocument()
{
    auto editor = WordDocumentEditor::CreateNew();
    REQUIRE(editor);
    editor->AddParagraph("Alpha one");
    editor->AddParagraph("Beta two");
    editor->AddParagraph("Gamma three");
    const auto path = MakeTemporaryPath("exyoki_toolsquery");
    REQUIRE(editor->SaveToFile(path));
    return path;
}

bool AnyText(const QueryResult& result, std::string_view needle)
{
    return std::any_of(result.Matches.begin(), result.Matches.end(),
                       [&](const QueryMatch& match)
                       { return match.Text.find(needle) != std::string::npos; });
}

} // namespace

TEST_CASE("Query defaults to the main document part [unit] [tools]")
{
    const auto path = BuildFixtureDocument();

    const auto result = Query(path, "//w:p");
    CHECK(result.Ok);
    CHECK(result.PartName == "/word/document.xml");
    CHECK(result.Matches.size() >= 3);

    const auto text = Query(path, "//w:t");
    CHECK(text.Ok);
    CHECK(AnyText(text, "Alpha one"));
    CHECK(AnyText(text, "Gamma three"));

    std::filesystem::remove(path);
}

TEST_CASE("Query targets an explicit part and caps matches [unit] [tools]")
{
    const auto path = BuildFixtureDocument();

    const auto explicitPart = Query(path, "//w:p", QueryOptions{"/word/document.xml", {}, 0});
    CHECK(explicitPart.Ok);
    CHECK(explicitPart.PartName == "/word/document.xml");

    QueryOptions capped;
    capped.MaxMatches = 2;
    const auto limited = Query(path, "//w:p", capped);
    CHECK(limited.Ok);
    CHECK(limited.Matches.size() == 2);

    std::filesystem::remove(path);
}

TEST_CASE("Query reports a missing part with a diagnostic [unit] [tools]")
{
    const auto path = BuildFixtureDocument();

    QueryOptions options;
    options.PartName = "/word/does-not-exist.xml";
    const auto result = Query(path, "//w:p", options);
    CHECK_FALSE(result.Ok);
    CHECK_FALSE(result.Diagnostics.empty());
    CHECK(result.Diagnostics.front().Severity == ToolSeverity::Error);

    std::filesystem::remove(path);
}

TEST_CASE("Query reports malformed expressions with a diagnostic [unit] [tools]")
{
    const auto path = BuildFixtureDocument();

    const auto malformed = Query(path, "//[");
    CHECK_FALSE(malformed.Ok);
    CHECK_FALSE(malformed.Diagnostics.empty());

    const auto unbound = Query(path, "//zz:p");
    CHECK_FALSE(unbound.Ok);
    CHECK_FALSE(unbound.Diagnostics.empty());

    std::filesystem::remove(path);
}

TEST_CASE("Query succeeds with zero matches [unit] [tools]")
{
    const auto path = BuildFixtureDocument();

    const auto result = Query(path, "//w:tbl");
    CHECK(result.Ok);
    CHECK(result.Matches.empty());
    CHECK(result.Diagnostics.empty());

    std::filesystem::remove(path);
}

TEST_CASE("Query fails to open a missing package [unit] [tools]")
{
    const auto missing = ExyokiOfficeTests::MakeTemporaryPath("exyoki_no_such_package", ".docx");
    const auto result = Query(missing, "//w:p");
    CHECK_FALSE(result.Ok);
    CHECK_FALSE(result.Diagnostics.empty());
}
