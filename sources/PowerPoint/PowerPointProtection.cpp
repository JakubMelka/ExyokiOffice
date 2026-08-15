// Copyright (c) 2026 Jakub Melka and Contributors
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "ExyokiOffice/PowerPoint/PowerPointDocument.hpp"

#include "ExyokiOffice/Packaging/GeneratedParts.hpp"
#include "ExyokiOffice/DOM/DocumentFormat/OpenXml/Presentation.hpp"
#include "../OfficePasswordVerifier.hpp"

#include <string>
#include <vector>

namespace ExyokiOffice::PowerPoint
{
namespace Presentation = ExyokiOffice::DocumentFormat::OpenXml::Presentation;

using ExyokiOffice::Protection::OfficePasswordVerifier;
using ExyokiOffice::Protection::PasswordHashAlgorithm;

/// Translation between the high-level modify-protection model and the
/// `p:modifyVerifier` element stored on the presentation part.
class ModifyProtectionHelpers final
{
public:
    ModifyProtectionHelpers() = delete;

    static Presentation::Presentation::Ptr Root(const PowerPointDocument::Ptr& document)
    {
        const auto part = document ? document->GetPresentationPart() : nullptr;
        return part ? part->GetTypedRootElement() : nullptr;
    }

    static Presentation::ModificationVerifier::Ptr Find(const Presentation::Presentation::Ptr& root)
    {
        return root ? root->GetFirstChildOfType<Presentation::ModificationVerifier>() : nullptr;
    }

    /// PowerPoint 2010 and later store the ISO verifier; earlier releases wrote
    /// the `hashData`/`saltData` pair, which this library can read but not validate.
    static bool HasModernVerifier(const Presentation::ModificationVerifier::Ptr& node)
    {
        return node->GetHashValue().IsDefined();
    }

    static bool HasLegacyVerifier(const Presentation::ModificationVerifier::Ptr& node)
    {
        return node->GetHashData().IsDefined();
    }

    static void WriteVerifier(const Presentation::ModificationVerifier::Ptr& node, std::string_view password)
    {
        const auto salt = OfficePasswordVerifier::GenerateSalt();
        const auto hash = OfficePasswordVerifier::ComputeVerifier(PasswordHashAlgorithm::Sha512, password, salt,
                                                                  OfficePasswordVerifier::DefaultSpinCount);

        node->SetAlgorithmName(StringValue(std::string(OfficePasswordVerifier::DefaultAlgorithmName)));
        node->SetSaltValue(Base64BinaryValue(salt));
        node->SetHashValue(Base64BinaryValue(hash));
        node->SetSpinValue(UInt32Value(OfficePasswordVerifier::DefaultSpinCount));
    }

    /// Recomputes the stored verifier for @p password.
    /// \return The failure result, or a successful result when the password matches.
    static PresentationProtectionResult VerifyPassword(const Presentation::ModificationVerifier::Ptr& node,
                                                       std::string_view password)
    {
        const auto algorithm = OfficePasswordVerifier::ParseAlgorithmName(node->GetAlgorithmName().View());
        if (!algorithm)
        {
            return {PresentationProtectionError::UnsupportedVerifier,
                    "The modify-protection password uses a hash algorithm this API cannot compute."};
        }

        const auto spinValue = node->GetSpinValue().ValueOr(0U);
        if (spinValue > OfficePasswordVerifier::MaximumSpinCount)
        {
            return {PresentationProtectionError::UnsupportedVerifier,
                    "The modify-protection password uses an unsupported iteration count."};
        }

        const auto computed = OfficePasswordVerifier::ComputeVerifier(*algorithm, password,
                                                                      node->GetSaltValue().ValueOr({}), spinValue);
        if (!OfficePasswordVerifier::VerifiersEqual(computed, node->GetHashValue().ValueOr({})))
        {
            return {PresentationProtectionError::PasswordMismatch, "The modify-protection password is incorrect."};
        }
        return {};
    }
};

std::optional<PresentationModifyProtectionInfo> PowerPointDocumentEditor::GetModifyProtection() const
{
    const auto node = ModifyProtectionHelpers::Find(ModifyProtectionHelpers::Root(m_document));
    if (!node)
    {
        return std::nullopt;
    }

    PresentationModifyProtectionInfo result;
    result.HasPassword = ModifyProtectionHelpers::HasModernVerifier(node) ||
                         ModifyProtectionHelpers::HasLegacyVerifier(node);
    result.VerifierSupported = ModifyProtectionHelpers::HasModernVerifier(node) &&
                               OfficePasswordVerifier::ParseAlgorithmName(node->GetAlgorithmName().View()).has_value();
    return result;
}

PresentationProtectionResult PowerPointDocumentEditor::ProtectFromModification(std::string_view password)
{
    if (password.empty())
    {
        return {PresentationProtectionError::InvalidPassword,
                "Modify protection requires a non-empty password; use UnprotectFromModification to remove it."};
    }

    const auto part = m_document ? m_document->GetPresentationPart() : nullptr;
    const auto root = part ? part->GetTypedRootElement() : nullptr;
    if (!root)
    {
        return {PresentationProtectionError::InvalidPresentation,
                "The editor has no attached presentation part."};
    }

    const auto originalXml = part->GetXmlString();
    if (const auto old = ModifyProtectionHelpers::Find(root))
    {
        root->RemoveChild(old);
    }

    const auto verifier = root->AppendChild<Presentation::ModificationVerifier>();
    if (!verifier)
    {
        part->SetXmlString(originalXml);
        return {PresentationProtectionError::WriteFailed, "Modify protection could not be created."};
    }

    ModifyProtectionHelpers::WriteVerifier(verifier, password);
    return {};
}

PresentationProtectionResult PowerPointDocumentEditor::UnprotectFromModification(std::string_view password)
{
    if (!m_document)
    {
        return {PresentationProtectionError::InvalidPresentation, "The editor has no attached presentation."};
    }

    const auto root = ModifyProtectionHelpers::Root(m_document);
    const auto verifier = ModifyProtectionHelpers::Find(root);
    if (!verifier)
    {
        return {};
    }

    if (ModifyProtectionHelpers::HasModernVerifier(verifier))
    {
        if (const auto verification = ModifyProtectionHelpers::VerifyPassword(verifier, password);
            !verification.Succeeded())
        {
            return verification;
        }
    }
    else if (ModifyProtectionHelpers::HasLegacyVerifier(verifier))
    {
        return {PresentationProtectionError::UnsupportedVerifier,
                "This presentation uses a legacy password verifier that this API cannot validate."};
    }
    else if (!password.empty())
    {
        return {PresentationProtectionError::PasswordMismatch,
                "The presentation is not password protected; pass an empty password to remove protection."};
    }

    if (!root->RemoveChild(verifier))
    {
        return {PresentationProtectionError::WriteFailed, "Modify protection could not be removed."};
    }
    return {};
}

} // namespace ExyokiOffice::PowerPoint
