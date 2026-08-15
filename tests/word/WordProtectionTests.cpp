// Copyright (c) 2026 Jakub Melka and Contributors
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "doctest.h"

#include "ExyokiOffice/Word/WordDocument.hpp"

#include <string>

namespace
{
using ExyokiOffice::Word::WordDocumentEditor;
using ExyokiOffice::Word::WordProtectionError;
using ExyokiOffice::Word::WordProtectionOptions;
using ExyokiOffice::Word::WordProtectionType;

std::string SettingsXml(const WordDocumentEditor::Ptr& editor)
{
    const auto document = editor ? editor->GetDocument() : nullptr;
    const auto mainPart = document ? document->GetMainDocumentPart() : nullptr;
    const auto settingsPart = mainPart ? mainPart->GetDocumentSettingsPart() : nullptr;
    return settingsPart ? settingsPart->GetXmlString() : std::string();
}
} // namespace

TEST_SUITE("WordProtectionTests")
{
    TEST_CASE("an unprotected document reports no protection [unit] [word] [protection]")
    {
        auto editor = WordDocumentEditor::CreateNew();
        REQUIRE(editor);
        CHECK_FALSE(editor->GetDocumentProtection().has_value());

        // Removing absent protection is a successful no-op.
        CHECK(editor->UnprotectDocument());
    }

    TEST_CASE("protection without a password records the requested restrictions [unit] [word] [protection]")
    {
        auto editor = WordDocumentEditor::CreateNew();
        REQUIRE(editor);

        WordProtectionOptions options;
        options.Editing = WordProtectionType::TrackedChanges;
        options.RestrictFormattingToUnlockedStyles = true;
        REQUIRE(editor->ProtectDocument(options));

        const auto info = editor->GetDocumentProtection();
        REQUIRE(info.has_value());
        CHECK(info->Options == options);
        CHECK_FALSE(info->HasPassword);

        const auto xml = SettingsXml(editor);
        CHECK(xml.find("<w:documentProtection") != std::string::npos);
        CHECK(xml.find("w:edit=\"trackedChanges\"") != std::string::npos);
        CHECK(xml.find("w:formatting=\"true\"") != std::string::npos);
        CHECK(xml.find("w:hashValue=") == std::string::npos);
    }

    TEST_CASE("every editing restriction round-trips through the package [unit] [word] [protection]")
    {
        const WordProtectionType restrictions[] = {WordProtectionType::ReadOnly, WordProtectionType::Comments,
                                                   WordProtectionType::TrackedChanges, WordProtectionType::Forms};
        for (const auto restriction : restrictions)
        {
            auto editor = WordDocumentEditor::CreateNew();
            REQUIRE(editor);

            WordProtectionOptions options;
            options.Editing = restriction;
            REQUIRE(editor->ProtectDocument(options));

            auto reopened = WordDocumentEditor::Open(editor->SaveToMemory());
            REQUIRE(reopened);
            const auto info = reopened->GetDocumentProtection();
            REQUIRE(info.has_value());
            CHECK(info->Options.Editing == restriction);
            CHECK(info->Options.Enforce);
        }
    }

    TEST_CASE("formatting-only protection is a valid restriction [unit] [word] [protection]")
    {
        auto editor = WordDocumentEditor::CreateNew();
        REQUIRE(editor);

        WordProtectionOptions options;
        options.Editing = WordProtectionType::None;
        options.RestrictFormattingToUnlockedStyles = true;
        REQUIRE(editor->ProtectDocument(options));

        const auto info = editor->GetDocumentProtection();
        REQUIRE(info.has_value());
        CHECK(info->Options.Editing == WordProtectionType::None);
        CHECK(info->Options.RestrictFormattingToUnlockedStyles);
    }

    TEST_CASE("protection that restricts nothing is rejected [unit] [word] [protection]")
    {
        auto editor = WordDocumentEditor::CreateNew();
        REQUIRE(editor);

        WordProtectionOptions options;
        options.Editing = WordProtectionType::None;
        options.RestrictFormattingToUnlockedStyles = false;
        const auto result = editor->ProtectDocument(options);
        CHECK_FALSE(result.Succeeded());
        CHECK(result.Error == WordProtectionError::InvalidOptions);
        CHECK_FALSE(result.Message.empty());
        CHECK_FALSE(editor->GetDocumentProtection().has_value());
    }

    TEST_CASE("a non-enforced restriction is reported as such [unit] [word] [protection]")
    {
        auto editor = WordDocumentEditor::CreateNew();
        REQUIRE(editor);

        WordProtectionOptions options;
        options.Editing = WordProtectionType::ReadOnly;
        options.Enforce = false;
        REQUIRE(editor->ProtectDocument(options));

        const auto info = editor->GetDocumentProtection();
        REQUIRE(info.has_value());
        CHECK_FALSE(info->Options.Enforce);
    }

    TEST_CASE("a password verifier is written and validated [unit] [word] [protection]")
    {
        auto editor = WordDocumentEditor::CreateNew();
        REQUIRE(editor);
        REQUIRE(editor->ProtectDocument({}, "letmein"));

        const auto info = editor->GetDocumentProtection();
        REQUIRE(info.has_value());
        CHECK(info->HasPassword);
        CHECK(info->Options.Editing == WordProtectionType::ReadOnly);

        const auto xml = SettingsXml(editor);
        CHECK(xml.find("w:algorithmName=\"SHA-512\"") != std::string::npos);
        CHECK(xml.find("w:hashValue=") != std::string::npos);
        CHECK(xml.find("w:saltValue=") != std::string::npos);
        CHECK(xml.find("w:spinCount=\"100000\"") != std::string::npos);
        // The verifier is not encryption; the document body stays plain XML.
        CHECK(xml.find("letmein") == std::string::npos);

        const auto wrong = editor->UnprotectDocument("letmeout");
        CHECK_FALSE(wrong.Succeeded());
        CHECK(wrong.Error == WordProtectionError::PasswordMismatch);
        CHECK(editor->GetDocumentProtection().has_value());

        const auto missing = editor->UnprotectDocument();
        CHECK_FALSE(missing.Succeeded());
        CHECK(missing.Error == WordProtectionError::PasswordMismatch);

        CHECK(editor->UnprotectDocument("letmein"));
        CHECK_FALSE(editor->GetDocumentProtection().has_value());
        CHECK(SettingsXml(editor).find("<w:documentProtection") == std::string::npos);
    }

    TEST_CASE("a password verifier survives a package round trip [unit] [word] [protection]")
    {
        auto editor = WordDocumentEditor::CreateNew();
        REQUIRE(editor);
        WordProtectionOptions options;
        options.Editing = WordProtectionType::Forms;
        REQUIRE(editor->ProtectDocument(options, "pa55 word"));

        auto reopened = WordDocumentEditor::Open(editor->SaveToMemory());
        REQUIRE(reopened);
        const auto info = reopened->GetDocumentProtection();
        REQUIRE(info.has_value());
        CHECK(info->Options.Editing == WordProtectionType::Forms);
        CHECK(info->HasPassword);

        CHECK_FALSE(reopened->UnprotectDocument("pa55word").Succeeded());
        CHECK(reopened->UnprotectDocument("pa55 word"));
        CHECK_FALSE(reopened->GetDocumentProtection().has_value());
    }

    TEST_CASE("removing a password requires an empty password [unit] [word] [protection]")
    {
        auto editor = WordDocumentEditor::CreateNew();
        REQUIRE(editor);
        REQUIRE(editor->ProtectDocument());

        const auto result = editor->UnprotectDocument("unnecessary");
        CHECK_FALSE(result.Succeeded());
        CHECK(result.Error == WordProtectionError::PasswordMismatch);
        CHECK(editor->UnprotectDocument());
    }

    TEST_CASE("re-protecting replaces the previous restrictions [unit] [word] [protection]")
    {
        auto editor = WordDocumentEditor::CreateNew();
        REQUIRE(editor);
        REQUIRE(editor->ProtectDocument({WordProtectionType::Comments, false, true}, "first"));

        WordProtectionOptions replacement;
        replacement.Editing = WordProtectionType::ReadOnly;
        REQUIRE(editor->ProtectDocument(replacement));

        const auto info = editor->GetDocumentProtection();
        REQUIRE(info.has_value());
        CHECK(info->Options.Editing == WordProtectionType::ReadOnly);
        CHECK_FALSE(info->HasPassword);

        const auto xml = SettingsXml(editor);
        CHECK(xml.find("w:edit=\"comments\"") == std::string::npos);
        CHECK(xml.find("w:hashValue=") == std::string::npos);
        CHECK(editor->UnprotectDocument());
    }

    TEST_CASE("an editor without a document reports a failure [unit] [word] [protection]")
    {
        auto editor = WordDocumentEditor::Create();
        REQUIRE(editor);
        CHECK_FALSE(editor->GetDocumentProtection().has_value());
        CHECK(editor->ProtectDocument().Error == WordProtectionError::InvalidDocument);
        CHECK(editor->UnprotectDocument().Error == WordProtectionError::InvalidDocument);
    }
}
