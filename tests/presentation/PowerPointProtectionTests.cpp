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

    TEST_CASE("modify protection writes the verifier PowerPoint writes [unit] [powerpoint] [protection]")
    {
        auto editor = PowerPointDocumentEditor::CreateNew();
        REQUIRE(editor);
        REQUIRE(editor->AddSlide());
        REQUIRE(editor->ProtectFromModification("board only"));

        const auto info = editor->GetModifyProtection();
        REQUIRE(info.has_value());
        CHECK(info->HasPassword);
        CHECK(info->VerifierSupported);

        // Transcribed from a presentation PowerPoint wrote. `CT_ModifyVerifier`
        // declares these seven attributes required and the ISO group
        // (algorithmName, hashValue, saltValue, spinValue) optional, and
        // PowerPoint writes the required seven and none of the ISO ones. The
        // two groups hold the same values under different names: recomputing
        // PowerPoint's own hashData with the ISO formula reproduces it byte for
        // byte, with cryptAlgorithmSid 14 naming SHA-512.
        const auto xml = PresentationXml(editor);
        CHECK(xml.find("<p:modifyVerifier") != std::string::npos);
        CHECK(xml.find("cryptProviderType=\"rsaAES\"") != std::string::npos);
        CHECK(xml.find("cryptAlgorithmClass=\"hash\"") != std::string::npos);
        CHECK(xml.find("cryptAlgorithmType=\"typeAny\"") != std::string::npos);
        CHECK(xml.find("cryptAlgorithmSid=\"14\"") != std::string::npos);
        CHECK(xml.find("spinCount=\"100000\"") != std::string::npos);
        CHECK(xml.find("saltData=") != std::string::npos);
        CHECK(xml.find("hashData=") != std::string::npos);
        // The verifier is a user-interface restriction, not encryption.
        CHECK(xml.find("board only") == std::string::npos);
    }

    TEST_CASE("a verifier written under either attribute group is validated [unit] [powerpoint] [protection]")
    {
        // The ISO group is what this library used to write and what a document
        // from another producer may still carry, so reading has to accept both
        // spellings even though writing settles on one.
        auto editor = PowerPointDocumentEditor::CreateNew();
        REQUIRE(editor);
        REQUIRE(editor->ProtectFromModification("shared"));

        const auto part = editor->GetDocument()->GetPresentationPart();
        REQUIRE(part);
        auto xml = part->GetXmlString();

        const auto value = [&xml](std::string_view name)
        {
            const auto start = xml.find(std::string(name) + "=\"");
            if (start == std::string::npos)
            {
                return std::string();
            }
            const auto from = start + name.size() + 2;
            return xml.substr(from, xml.find('"', from) - from);
        };
        const auto salt = value("saltData");
        const auto hash = value("hashData");
        const auto spin = value("spinCount");
        REQUIRE_FALSE(salt.empty());
        REQUIRE_FALSE(hash.empty());
        REQUIRE_FALSE(spin.empty());

        const auto start = xml.find("<p:modifyVerifier");
        const auto end = xml.find('>', start);
        xml.replace(start, end - start + 1,
                    "<p:modifyVerifier algorithmName=\"SHA-512\" saltValue=\"" + salt + "\" hashValue=\"" + hash +
                        "\" spinValue=\"" + spin + "\"/>");
        part->SetXmlString(xml);

        auto reopened = PowerPointDocumentEditor::Open(editor->SaveToMemory());
        REQUIRE(reopened);
        const auto info = reopened->GetModifyProtection();
        REQUIRE(info.has_value());
        CHECK(info->HasPassword);
        CHECK(info->VerifierSupported);
        CHECK_FALSE(reopened->UnprotectFromModification("guessed").Succeeded());
        CHECK(reopened->UnprotectFromModification("shared"));
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
