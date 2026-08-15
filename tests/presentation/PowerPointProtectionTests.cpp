// Copyright (c) 2026 Jakub Melka and Contributors
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "doctest.h"

#include "ExyokiOffice/PowerPoint/PowerPointDocument.hpp"

#include <string>

namespace
{
using ExyokiOffice::PowerPoint::PowerPointDocumentEditor;
using ExyokiOffice::PowerPoint::PresentationProtectionError;

std::string PresentationXml(const PowerPointDocumentEditor::Ptr& editor)
{
    const auto document = editor ? editor->GetDocument() : nullptr;
    const auto part = document ? document->GetPresentationPart() : nullptr;
    return part ? part->GetXmlString() : std::string();
}
} // namespace

TEST_SUITE("PowerPointProtectionTests")
{
    TEST_CASE("an unprotected presentation reports no modify protection [unit] [powerpoint] [protection]")
    {
        auto editor = PowerPointDocumentEditor::CreateNew();
        REQUIRE(editor);
        CHECK_FALSE(editor->GetModifyProtection().has_value());

        // Removing absent protection is a successful no-op.
        CHECK(editor->UnprotectFromModification("anything"));
    }

    TEST_CASE("modify protection requires a password [unit] [powerpoint] [protection]")
    {
        auto editor = PowerPointDocumentEditor::CreateNew();
        REQUIRE(editor);

        const auto result = editor->ProtectFromModification("");
        CHECK_FALSE(result.Succeeded());
        CHECK(result.Error == PresentationProtectionError::InvalidPassword);
        CHECK_FALSE(result.Message.empty());
        CHECK_FALSE(editor->GetModifyProtection().has_value());
    }

    TEST_CASE("modify protection writes an ISO password verifier [unit] [powerpoint] [protection]")
    {
        auto editor = PowerPointDocumentEditor::CreateNew();
        REQUIRE(editor);
        REQUIRE(editor->AddSlide());
        REQUIRE(editor->ProtectFromModification("board only"));

        const auto info = editor->GetModifyProtection();
        REQUIRE(info.has_value());
        CHECK(info->HasPassword);
        CHECK(info->VerifierSupported);

        const auto xml = PresentationXml(editor);
        CHECK(xml.find("<p:modifyVerifier") != std::string::npos);
        CHECK(xml.find("algorithmName=\"SHA-512\"") != std::string::npos);
        CHECK(xml.find("hashValue=") != std::string::npos);
        CHECK(xml.find("saltValue=") != std::string::npos);
        CHECK(xml.find("spinValue=\"100000\"") != std::string::npos);
        // The verifier is a user-interface restriction, not encryption.
        CHECK(xml.find("board only") == std::string::npos);
    }

    TEST_CASE("the password is validated before protection is removed [unit] [powerpoint] [protection]")
    {
        auto editor = PowerPointDocumentEditor::CreateNew();
        REQUIRE(editor);
        REQUIRE(editor->ProtectFromModification("letmein"));

        const auto wrong = editor->UnprotectFromModification("letmeout");
        CHECK_FALSE(wrong.Succeeded());
        CHECK(wrong.Error == PresentationProtectionError::PasswordMismatch);
        CHECK(editor->GetModifyProtection().has_value());

        CHECK(editor->UnprotectFromModification("letmein"));
        CHECK_FALSE(editor->GetModifyProtection().has_value());
        CHECK(PresentationXml(editor).find("<p:modifyVerifier") == std::string::npos);
    }

    TEST_CASE("modify protection survives a package round trip [unit] [powerpoint] [protection]")
    {
        auto editor = PowerPointDocumentEditor::CreateNew();
        REQUIRE(editor);
        REQUIRE(editor->AddSlide());
        REQUIRE(editor->ProtectFromModification("pa55 word"));

        auto reopened = PowerPointDocumentEditor::Open(editor->SaveToMemory());
        REQUIRE(reopened);
        const auto info = reopened->GetModifyProtection();
        REQUIRE(info.has_value());
        CHECK(info->HasPassword);
        CHECK(info->VerifierSupported);

        CHECK_FALSE(reopened->UnprotectFromModification("pa55word").Succeeded());
        CHECK(reopened->UnprotectFromModification("pa55 word"));
        CHECK_FALSE(reopened->GetModifyProtection().has_value());
    }

    TEST_CASE("the verifier is inserted at its schema position [unit] [powerpoint] [protection]")
    {
        auto editor = PowerPointDocumentEditor::CreateNew();
        REQUIRE(editor);
        auto slide = editor->AddSlide();
        REQUIRE(slide);
        REQUIRE(editor->ProtectFromModification("secret"));
        // p:custShowLst precedes p:modifyVerifier in CT_Presentation, so adding
        // it after protection must not leave the presentation out of order.
        REQUIRE(editor->AddCustomShow({1, "Short", {slide->Id()}}));

        const auto xml = PresentationXml(editor);
        const auto customShows = xml.find("<p:custShowLst");
        const auto verifier = xml.find("<p:modifyVerifier");
        REQUIRE(customShows != std::string::npos);
        REQUIRE(verifier != std::string::npos);
        CHECK(customShows < verifier);
    }

    TEST_CASE("re-protecting replaces the previous verifier [unit] [powerpoint] [protection]")
    {
        auto editor = PowerPointDocumentEditor::CreateNew();
        REQUIRE(editor);
        REQUIRE(editor->ProtectFromModification("first"));
        REQUIRE(editor->ProtectFromModification("second"));

        CHECK_FALSE(editor->UnprotectFromModification("first").Succeeded());
        CHECK(editor->UnprotectFromModification("second"));
    }

    TEST_CASE("an editor without a presentation reports a failure [unit] [powerpoint] [protection]")
    {
        auto editor = PowerPointDocumentEditor::Create();
        REQUIRE(editor);
        CHECK_FALSE(editor->GetModifyProtection().has_value());
        CHECK(editor->ProtectFromModification("secret").Error == PresentationProtectionError::InvalidPresentation);
        CHECK(editor->UnprotectFromModification("secret").Error ==
              PresentationProtectionError::InvalidPresentation);
    }
}
