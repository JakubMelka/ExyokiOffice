// Copyright (c) 2026 Jakub Melka and Collaborators
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "ExyokiOffice/Word/WordDocument.hpp"

#include "ExyokiOffice/Packaging/GeneratedParts.hpp"
#include "ExyokiOffice/DOM/DocumentFormat/OpenXml/Wordprocessing.hpp"
#include "../OfficePasswordVerifier.hpp"
#include "ExyokiOffice/StandardTypes.hpp"

#include <string>
#include <vector>

namespace ExyokiOffice::Word
{
namespace Wordprocessing = ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing;

using ExyokiOffice::Protection::OfficePasswordVerifier;
using ExyokiOffice::Protection::PasswordHashAlgorithm;

/// Translation between the high-level protection model and the
/// `w:documentProtection` element stored in the document settings part.
class DocumentProtectionHelpers final
{
public:
    DocumentProtectionHelpers() = delete;

    static Wordprocessing::Settings::Ptr Settings(const WordDocument::Ptr& document)
    {
        const auto mainPart = document ? document->GetMainDocumentPart() : nullptr;
        const auto settingsPart = mainPart ? mainPart->GetDocumentSettingsPart() : nullptr;
        return settingsPart ? settingsPart->GetTypedRootElement() : nullptr;
    }

    static Wordprocessing::DocumentProtection::Ptr Find(const Wordprocessing::Settings::Ptr& settings)
    {
        return settings ? settings->GetFirstChildOfType<Wordprocessing::DocumentProtection>() : nullptr;
    }

    static Wordprocessing::DocumentProtectionValues ToDomEditRestriction(WordProtectionType type)
    {
        switch (type)
        {
            case WordProtectionType::ReadOnly:
                return Wordprocessing::DocumentProtectionValues::ReadOnly;
            case WordProtectionType::Comments:
                return Wordprocessing::DocumentProtectionValues::Comments;
            case WordProtectionType::TrackedChanges:
                return Wordprocessing::DocumentProtectionValues::TrackedChanges;
            case WordProtectionType::Forms:
                return Wordprocessing::DocumentProtectionValues::Forms;
            case WordProtectionType::None:
                break;
        }
        return Wordprocessing::DocumentProtectionValues::None;
    }

    static WordProtectionType FromDomEditRestriction(const Wordprocessing::DocumentProtectionValues& value)
    {
        switch (value.GetValue())
        {
            case Wordprocessing::DocumentProtectionValues::ReadOnly:
                return WordProtectionType::ReadOnly;
            case Wordprocessing::DocumentProtectionValues::Comments:
                return WordProtectionType::Comments;
            case Wordprocessing::DocumentProtectionValues::TrackedChanges:
                return WordProtectionType::TrackedChanges;
            case Wordprocessing::DocumentProtectionValues::Forms:
                return WordProtectionType::Forms;
            default:
                break;
        }
        return WordProtectionType::None;
    }

    static WordProtectionOptions ReadOptions(const Wordprocessing::DocumentProtection::Ptr& node)
    {
        WordProtectionOptions options;
        const auto edit = node->GetEdit();
        options.Editing = edit.IsDefined() ? FromDomEditRestriction(edit.Value()) : WordProtectionType::None;
        options.RestrictFormattingToUnlockedStyles = node->GetFormatting().ValueOr(false);
        options.Enforce = node->GetEnforcement().ValueOr(false);
        return options;
    }

    /// A protection element that restricts nothing is treated as absent, so the
    /// reported state matches what a word processor actually applies.
    static bool RestrictsAnything(const WordProtectionOptions& options)
    {
        return options.Editing != WordProtectionType::None || options.RestrictFormattingToUnlockedStyles;
    }

    static bool HasLegacyVerifier(const Wordprocessing::DocumentProtection::Ptr& node)
    {
        return node->GetHash().IsDefined();
    }

    static bool HasModernVerifier(const Wordprocessing::DocumentProtection::Ptr& node)
    {
        return node->GetHashValue().IsDefined();
    }

    static void WriteVerifier(const Wordprocessing::DocumentProtection::Ptr& node, std::string_view password)
    {
        const auto salt = OfficePasswordVerifier::GenerateSalt();
        const auto hash = OfficePasswordVerifier::ComputeVerifier(PasswordHashAlgorithm::Sha512, password, salt,
                                                                  OfficePasswordVerifier::DefaultSpinCount);

        node->SetAlgorithmName(StringValue(std::string(OfficePasswordVerifier::DefaultAlgorithmName)));
        node->SetSaltValue(Base64BinaryValue(salt));
        node->SetHashValue(Base64BinaryValue(hash));
        node->SetSpinCount(Int32Value(static_cast<Int32>(OfficePasswordVerifier::DefaultSpinCount)));
    }

    /// Recomputes the stored verifier for @p password.
    /// \return The failure result, or a successful result when the password matches.
    static WordProtectionResult VerifyPassword(const Wordprocessing::DocumentProtection::Ptr& node,
                                               std::string_view password)
    {
        const auto algorithm = OfficePasswordVerifier::ParseAlgorithmName(node->GetAlgorithmName().View());
        if (!algorithm)
        {
            return {WordProtectionError::UnsupportedVerifier,
                    "The document protection password uses a hash algorithm this API cannot compute."};
        }

        const auto spinCount = node->GetSpinCount().ValueOr(0);
        if (spinCount < 0 || static_cast<UInt32>(spinCount) > OfficePasswordVerifier::MaximumSpinCount)
        {
            return {WordProtectionError::UnsupportedVerifier,
                    "The document protection password uses an unsupported iteration count."};
        }

        const auto computed = OfficePasswordVerifier::ComputeVerifier(*algorithm, password,
                                                                      node->GetSaltValue().ValueOr({}),
                                                                      static_cast<UInt32>(spinCount));
        if (!OfficePasswordVerifier::VerifiersEqual(computed, node->GetHashValue().ValueOr({})))
        {
            return {WordProtectionError::PasswordMismatch, "The document protection password is incorrect."};
        }
        return {};
    }
};

std::optional<WordProtectionInfo> WordDocumentEditor::GetDocumentProtection() const
{
    const auto node = DocumentProtectionHelpers::Find(DocumentProtectionHelpers::Settings(m_document));
    if (!node)
    {
        return std::nullopt;
    }

    WordProtectionInfo result;
    result.Options = DocumentProtectionHelpers::ReadOptions(node);
    if (!DocumentProtectionHelpers::RestrictsAnything(result.Options))
    {
        return std::nullopt;
    }

    result.HasPassword = DocumentProtectionHelpers::HasModernVerifier(node) ||
                         DocumentProtectionHelpers::HasLegacyVerifier(node);
    return result;
}

WordProtectionResult WordDocumentEditor::ProtectDocument(const WordProtectionOptions& options,
                                                         std::string_view password)
{
    if (!DocumentProtectionHelpers::RestrictsAnything(options))
    {
        return {WordProtectionError::InvalidOptions,
                "Document protection requires an editing restriction or the formatting restriction."};
    }

    const auto settingsPart = m_document ? m_document->EnsureDocumentSettingsPart() : nullptr;
    const auto settings = settingsPart ? settingsPart->GetTypedRootElement() : nullptr;
    if (!settings)
    {
        return {WordProtectionError::InvalidDocument, "The editor has no attached document settings part."};
    }

    const auto originalXml = settingsPart->GetXmlString();
    if (const auto old = DocumentProtectionHelpers::Find(settings))
    {
        settings->RemoveChild(old);
    }

    const auto protection = settings->AppendChild<Wordprocessing::DocumentProtection>();
    if (!protection)
    {
        settingsPart->SetXmlString(originalXml);
        return {WordProtectionError::WriteFailed, "Document protection could not be created."};
    }

    protection->SetEdit(EnumValue<Wordprocessing::DocumentProtectionValues>(
        DocumentProtectionHelpers::ToDomEditRestriction(options.Editing)));
    protection->SetFormatting(OnOffValue(options.RestrictFormattingToUnlockedStyles));
    protection->SetEnforcement(OnOffValue(options.Enforce));
    if (!password.empty())
    {
        DocumentProtectionHelpers::WriteVerifier(protection, password);
    }
    return {};
}

WordProtectionResult WordDocumentEditor::UnprotectDocument(std::string_view password)
{
    if (!m_document)
    {
        return {WordProtectionError::InvalidDocument, "The editor has no attached document."};
    }

    const auto settings = DocumentProtectionHelpers::Settings(m_document);
    const auto protection = DocumentProtectionHelpers::Find(settings);
    if (!protection || !DocumentProtectionHelpers::RestrictsAnything(DocumentProtectionHelpers::ReadOptions(protection)))
    {
        return {};
    }

    if (DocumentProtectionHelpers::HasModernVerifier(protection))
    {
        if (const auto verification = DocumentProtectionHelpers::VerifyPassword(protection, password);
            !verification.Succeeded())
        {
            return verification;
        }
    }
    else if (DocumentProtectionHelpers::HasLegacyVerifier(protection))
    {
        return {WordProtectionError::UnsupportedVerifier,
                "This document uses a legacy password verifier that this API cannot validate."};
    }
    else if (!password.empty())
    {
        return {WordProtectionError::PasswordMismatch,
                "The document is not password protected; pass an empty password to remove protection."};
    }

    if (!settings->RemoveChild(protection))
    {
        return {WordProtectionError::WriteFailed, "Document protection could not be removed."};
    }
    return {};
}

} // namespace ExyokiOffice::Word
