// Copyright (c) 2026 Jakub Melka and Contributors
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "doctest.h"

#include "ExyokiOffice/Tools/PackageInspector.hpp"
#include "ExyokiOffice/Word/WordDocument.hpp"
#include "ExyokiOffice/StandardTypes.hpp"

#include <algorithm>
#include <clocale>
#include <variant>

using ExyokiOffice::OpenXmlPackage;
using ExyokiOffice::Word::WordDocumentEditor;
using namespace ExyokiOffice::Tools;

namespace
{

std::vector<ExyokiOffice::Byte> SaveToBytes(const WordDocumentEditor::Ptr& editor)
{
    const auto bytes = editor->SaveToMemory();
    REQUIRE(!bytes.empty());
    return bytes;
}

} // namespace

TEST_CASE("CollectAllParts deduplicates a multi-parent graph [unit] [tools]")
{
    auto editor = WordDocumentEditor::CreateNew();
    REQUIRE(editor);
    auto image = editor->AddParagraph("Image holder");
    (void)image;
    const auto bytes = SaveToBytes(editor);
    OpenXmlPackage package;
    REQUIRE(package.LoadFromMemory(bytes));

    const auto parts = CollectAllParts(package);
    std::vector<std::string> uris;
    for (const auto& part : parts)
    {
        uris.push_back(part->Uri());
    }
    std::sort(uris.begin(), uris.end());
    CHECK(std::adjacent_find(uris.begin(), uris.end()) == uris.end());
    CHECK(!parts.empty());
}

TEST_CASE("DetectFamily via GetInfo recognizes a Word document [unit] [tools]")
{
    auto editor = WordDocumentEditor::CreateNew();
    REQUIRE(editor);
    editor->AddParagraph("Hello");
    const auto bytes = SaveToBytes(editor);
    OpenXmlPackage package;
    REQUIRE(package.LoadFromMemory(bytes));

    const auto info = GetInfo(package);
    CHECK(info.Family == DocumentFamily::Word);
    CHECK(info.MainPartUri == "/word/document.xml");
    CHECK(!info.DocumentTypeName.empty());
}

TEST_CASE("GetInfo reports Unknown family for a package with no officeDocument relationship [unit] [tools]")
{
    OpenXmlPackage package;
    const auto info = GetInfo(package);
    CHECK(info.Family == DocumentFamily::Unknown);
    CHECK(info.MainPartUri.empty());
}

TEST_CASE("Core properties round-trip through ReadCoreProperties/WriteCoreProperty [unit] [tools]")
{
    auto editor = WordDocumentEditor::CreateNew();
    REQUIRE(editor);
    REQUIRE(editor->GetDocument());
    REQUIRE(editor->GetDocument()->InitDocument());
    editor->GetDocument()->SetTitle("Original title");
    editor->GetDocument()->SetCreator("Original creator");

    const auto bytes = SaveToBytes(editor);
    OpenXmlPackage package;
    REQUIRE(package.LoadFromMemory(bytes));

    auto properties = ReadCoreProperties(package);
    CHECK(properties.Title == "Original title");
    CHECK(properties.Creator == "Original creator");

    CHECK(WriteCoreProperty(package, "Title", "Updated title"));
    CHECK(WriteCoreProperty(package, "Category", "Reports"));
    // Case-insensitively, and over the whole of what the properties editor
    // covers rather than only the fields of CoreProperties.
    CHECK(WriteCoreProperty(package, "hyperlinkbase", "https://example.invalid/"));
    // A timestamp is typed, not text, so this layer refuses it.
    CHECK_FALSE(WriteCoreProperty(package, "Created", "2026-08-15T00:00:00Z"));
    CHECK_FALSE(WriteCoreProperty(package, "", "value"));

    properties = ReadCoreProperties(package);
    CHECK(properties.Title == "Updated title");
    CHECK(properties.Category == "Reports");
}

TEST_CASE("A name with no document property of its own becomes a custom one [unit] [tools]")
{
    auto editor = WordDocumentEditor::CreateNew();
    REQUIRE(editor);
    REQUIRE(editor->GetDocument());
    REQUIRE(editor->GetDocument()->InitDocument());

    const auto bytes = SaveToBytes(editor);
    OpenXmlPackage package;
    REQUIRE(package.LoadFromMemory(bytes));

    CHECK(ReadCustomProperties(package).empty());
    CHECK(WriteCoreProperty(package, "Department", "Research"));

    const auto custom = ReadCustomProperties(package);
    REQUIRE(custom.size() == 1);
    CHECK(custom.front().Name == "Department");
    REQUIRE(std::holds_alternative<std::string>(custom.front().Value));
    CHECK(std::get<std::string>(custom.front().Value) == "Research");

    // Clearing removes it, and clearing one that was never there is not a failure.
    CHECK(WriteCoreProperty(package, "Department", ""));
    CHECK(ReadCustomProperties(package).empty());
    CHECK(WriteCoreProperty(package, "NeverExisted", ""));
}

/// Sets a locale for the body of a test and puts "C" back afterwards.
class ScopedLocale
{
public:
    explicit ScopedLocale(const char* name)
        : m_applied(std::setlocale(LC_ALL, name) != nullptr) {}

    ~ScopedLocale() { std::setlocale(LC_ALL, "C"); }

    ScopedLocale(const ScopedLocale&) = delete;
    ScopedLocale& operator=(const ScopedLocale&) = delete;
    ScopedLocale(ScopedLocale&&) = delete;
    ScopedLocale& operator=(ScopedLocale&&) = delete;

    /// False when the platform does not ship the locale, so the test can skip.
    [[nodiscard]] bool Applied() const noexcept { return m_applied; }

private:
    bool m_applied;
};

TEST_CASE("Property names are matched without consulting the C locale [unit] [tools]")
{
    // A property name is an ASCII token, but the Turkish locale folds 'I' to a
    // dotless 'i': under it, a locale-driven comparison stops recognizing TITLE
    // as Title, and `exyoki props set --title` writes a custom property instead
    // of the document title. The hosting application decides the locale, so the
    // library cannot ask it.
    const ScopedLocale turkish("tr_TR.UTF-8");
    if (!turkish.Applied())
    {
        MESSAGE("tr_TR.UTF-8 is not installed; the locale independence check did not run");
        return;
    }

    auto editor = WordDocumentEditor::CreateNew();
    REQUIRE(editor);
    REQUIRE(editor->GetDocument());
    REQUIRE(editor->GetDocument()->InitDocument());

    const auto bytes = SaveToBytes(editor);
    OpenXmlPackage package;
    REQUIRE(package.LoadFromMemory(bytes));

    REQUIRE(WriteCoreProperty(package, "TITLE", "Written in Turkish"));

    CHECK(ReadCoreProperties(package).Title == "Written in Turkish");
    CHECK(ReadCustomProperties(package).empty());
}

TEST_CASE("ListParts and ListRelationships report descriptor names and resolved targets [unit] [tools]")
{
    auto editor = WordDocumentEditor::CreateNew();
    REQUIRE(editor);
    editor->AddParagraph("Body text");
    const auto bytes = SaveToBytes(editor);
    OpenXmlPackage package;
    REQUIRE(package.LoadFromMemory(bytes));

    const auto parts = ListParts(package);
    const auto mainPart = std::find_if(parts.begin(), parts.end(),
                                       [](const auto& part)
                                       { return part.Uri == "/word/document.xml"; });
    REQUIRE(mainPart != parts.end());
    CHECK(mainPart->Kind == PartPayloadKind::Xml);
    CHECK(!mainPart->DescriptorName.empty());

    const auto relationships = ListRelationships(package);
    const auto mainRelationship = std::find_if(relationships.begin(), relationships.end(), [](const auto& rel)
                                               { return rel.ResolvedTargetUri == "/word/document.xml"; });
    REQUIRE(mainRelationship != relationships.end());
    CHECK(mainRelationship->TargetExists);
}
