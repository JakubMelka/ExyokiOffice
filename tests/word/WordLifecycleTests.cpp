// Copyright (c) 2026 Jakub Melka and Contributors
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "doctest.h"

#include "TestSupport.hpp"

#include "ExyokiOffice/Word/WordDocument.hpp"
#include "ExyokiOffice/StandardTypes.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace
{
constexpr std::string_view kAttachedTemplateRelationship =
    "http://schemas.openxmlformats.org/officeDocument/2006/relationships/attachedTemplate";

using ExyokiOffice::Packaging::WordprocessingDocumentType;
using ExyokiOffice::Word::WordDocumentEditor;

struct WordTypeCase
{
    WordprocessingDocumentType Type;
    std::string_view Name;
};

using ExyokiOfficeTests::MakeTemporaryPath;

bool HasAttachedTemplateRelationship(const WordDocumentEditor::Ptr& editor)
{
    if (!editor || !editor->GetDocument())
    {
        return false;
    }

    auto mainPart = editor->GetDocument()->GetMainDocumentPart();
    if (!mainPart)
    {
        return false;
    }

    auto settingsPart = mainPart->GetDocumentSettingsPart();
    if (!settingsPart)
    {
        return false;
    }

    for (const auto& relationship : settingsPart->Relationships())
    {
        if (relationship.Type == kAttachedTemplateRelationship && relationship.IsExternal && relationship.TargetMode == "External" && !relationship.Target.empty())
        {
            return true;
        }
    }
    return false;
}

bool HasAttachedTemplateReference(const WordDocumentEditor::Ptr& editor)
{
    if (!editor || !editor->GetDocument())
    {
        return false;
    }

    auto mainPart = editor->GetDocument()->GetMainDocumentPart();
    if (!mainPart)
    {
        return false;
    }

    auto settingsPart = mainPart->GetDocumentSettingsPart();
    if (!settingsPart)
    {
        return false;
    }

    const auto xml = settingsPart->GetXmlString();
    return xml.find("<w:attachedTemplate") != std::string::npos && xml.find("r:id=\"") != std::string::npos;
}

} // namespace

TEST_SUITE("WordLifecycleTests")
{

    TEST_CASE("WordDocumentEditor creates and reopens all Word document types [unit] [word] [word-lifecycle]")
    {
        constexpr std::array<WordTypeCase, 4> cases = {{
            {WordprocessingDocumentType::Document, "Document"},
            {WordprocessingDocumentType::Template, "Template"},
            {WordprocessingDocumentType::MacroEnabledDocument, "MacroEnabledDocument"},
            {WordprocessingDocumentType::MacroEnabledTemplate, "MacroEnabledTemplate"},
        }};

        for (const auto& testCase : cases)
        {
            CAPTURE(testCase.Name);

            auto editor = WordDocumentEditor::CreateNew(testCase.Type);
            REQUIRE(editor != nullptr);
            REQUIRE(editor->GetDocument() != nullptr);
            CHECK(editor->GetDocument()->GetDocumentType() == testCase.Type);

            REQUIRE(editor->AddParagraph("Lifecycle round trip") != nullptr);

            auto packageBytes = editor->SaveToMemory();
            REQUIRE(!packageBytes.empty());

            auto reopened = WordDocumentEditor::Open(packageBytes);
            REQUIRE(reopened != nullptr);
            REQUIRE(reopened->GetDocument() != nullptr);
            CHECK(reopened->GetDocument()->GetDocumentType() == testCase.Type);
            CHECK(reopened->SaveToMemory().size() > 0);
        }
    }

    TEST_CASE("WordDocumentEditor forwards OpenSettings to the low-level opener [unit] [word] [word-lifecycle]")
    {
        auto editor = WordDocumentEditor::CreateNew();
        REQUIRE(editor != nullptr);
        REQUIRE(editor->AddParagraph("Open settings budget test") != nullptr);

        auto packageBytes = editor->SaveToMemory();
        REQUIRE(!packageBytes.empty());

        ExyokiOffice::Packaging::OpenSettings restrictiveSettings;
        restrictiveSettings.MaxCharactersInPart = 1;

        CHECK(WordDocumentEditor::Open(packageBytes, restrictiveSettings) == nullptr);
    }

    TEST_CASE("WordDocumentEditor wraps existing documents via Create, constructor, and SetDocument [unit] [word] [word-lifecycle]")
    {
        SUBCASE("Create without a document returns an editor with no document attached")
        {
            auto editor = WordDocumentEditor::Create();
            REQUIRE(editor != nullptr);
            CHECK(editor->GetDocument() == nullptr);
            CHECK(editor->AddParagraph("no-op") == nullptr);
            CHECK(editor->SaveToMemory().empty());
        }

        SUBCASE("Create wraps an existing low-level document")
        {
            auto document = ExyokiOffice::Packaging::WordDocument::Create(WordprocessingDocumentType::Document);
            REQUIRE(document != nullptr);
            REQUIRE(document->InitDocument());

            auto editor = WordDocumentEditor::Create(document);
            REQUIRE(editor != nullptr);
            CHECK(editor->GetDocument() == document);
            CHECK(editor->AddParagraph("wrapped") != nullptr);
        }

        SUBCASE("Constructor with a document behaves like Create")
        {
            auto document = ExyokiOffice::Packaging::WordDocument::Create(WordprocessingDocumentType::Document);
            REQUIRE(document != nullptr);
            REQUIRE(document->InitDocument());

            WordDocumentEditor editor(document);
            CHECK(editor.GetDocument() == document);
            CHECK(editor.AddParagraph("constructed") != nullptr);
        }

        SUBCASE("CreateDefaultDocument initializes an empty editor")
        {
            auto editor = WordDocumentEditor::Create();
            REQUIRE(editor != nullptr);
            REQUIRE(editor->CreateDefaultDocument(WordprocessingDocumentType::Template));
            REQUIRE(editor->GetDocument() != nullptr);
            CHECK(editor->GetDocument()->GetDocumentType() == WordprocessingDocumentType::Template);
            CHECK(editor->AddParagraph("default document") != nullptr);
            CHECK(!editor->SaveToMemory().empty());
        }

        SUBCASE("SetDocument swaps the edited document")
        {
            auto first = WordDocumentEditor::CreateNew();
            REQUIRE(first != nullptr);

            auto replacement = ExyokiOffice::Packaging::WordDocument::Create(WordprocessingDocumentType::Document);
            REQUIRE(replacement != nullptr);
            REQUIRE(replacement->InitDocument());

            first->SetDocument(replacement);
            CHECK(first->GetDocument() == replacement);
            REQUIRE(first->AddParagraph("swapped") != nullptr);
            CHECK(first->Paragraphs().size() == 1);
        }
    }

    TEST_CASE("WordDocumentEditor opens documents from span and from a disk path [unit] [word] [word-lifecycle]")
    {
        auto editor = WordDocumentEditor::CreateNew();
        REQUIRE(editor != nullptr);
        REQUIRE(editor->AddParagraph("Open overload coverage") != nullptr);

        const auto packageBytes = editor->SaveToMemory();
        REQUIRE(!packageBytes.empty());

        SUBCASE("Open from std::span")
        {
            const std::span<const ExyokiOffice::Byte> view(packageBytes.data(), packageBytes.size());
            auto reopened = WordDocumentEditor::Open(view);
            REQUIRE(reopened != nullptr);
            REQUIRE(reopened->Paragraphs().size() == 1);
            CHECK(reopened->Paragraphs().front()->PlainText() == "Open overload coverage");
        }

        SUBCASE("Open from a disk path")
        {
            const auto path = MakeTemporaryPath("ExyokiOfficeWordLifecycleOpenPath", ".docx");
            REQUIRE(editor->SaveToFile(path));

            auto reopened = WordDocumentEditor::Open(path);
            REQUIRE(reopened != nullptr);
            REQUIRE(reopened->Paragraphs().size() == 1);
            CHECK(reopened->Paragraphs().front()->PlainText() == "Open overload coverage");

            CHECK(WordDocumentEditor::Open(std::filesystem::path{}) == nullptr);
            std::filesystem::remove(path);
        }
    }

    TEST_CASE("WordDocumentEditor creates a document from a template and preserves attachment relationship [unit] [word] [word-lifecycle]")
    {
        const auto templatePath = MakeTemporaryPath("ExyokiOfficeWordLifecycleTemplate", ".dotx");

        auto templateEditor = WordDocumentEditor::CreateNew(WordprocessingDocumentType::Template);
        REQUIRE(templateEditor != nullptr);
        REQUIRE(templateEditor->AddParagraph("Template content") != nullptr);
        REQUIRE(templateEditor->SaveToFile(templatePath));

        auto documentEditor = WordDocumentEditor::CreateFromTemplate(templatePath, true);
        REQUIRE(documentEditor != nullptr);
        REQUIRE(documentEditor->GetDocument() != nullptr);
        CHECK(documentEditor->GetDocument()->GetDocumentType() == WordprocessingDocumentType::Document);
        CHECK(HasAttachedTemplateRelationship(documentEditor));
        CHECK(HasAttachedTemplateReference(documentEditor));

        auto roundTripBytes = documentEditor->SaveToMemory();
        REQUIRE(!roundTripBytes.empty());

        auto reopened = WordDocumentEditor::Open(roundTripBytes);
        REQUIRE(reopened != nullptr);
        REQUIRE(reopened->GetDocument() != nullptr);
        CHECK(reopened->GetDocument()->GetDocumentType() == WordprocessingDocumentType::Document);
        CHECK(HasAttachedTemplateRelationship(reopened));
        CHECK(HasAttachedTemplateReference(reopened));

        std::filesystem::remove(templatePath);
    }

} // TEST_SUITE("WordLifecycleTests")
