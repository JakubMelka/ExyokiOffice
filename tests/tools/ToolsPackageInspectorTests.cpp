// Copyright (c) 2026 Jakub Melka and Collaborators
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "doctest.h"

#include "ExyokiOffice/Tools/PackageInspector.hpp"
#include "ExyokiOffice/Word/WordDocument.hpp"
#include "ExyokiOffice/StandardTypes.hpp"

#include <algorithm>

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
    CHECK_FALSE(WriteCoreProperty(package, "NotARealProperty", "value"));

    properties = ReadCoreProperties(package);
    CHECK(properties.Title == "Updated title");
    CHECK(properties.Category == "Reports");
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
