// Copyright (c) 2026 Jakub Melka and Contributors
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "ExyokiOffice/PowerPoint/PowerPointDocument.hpp"

#include "ExyokiOffice/Packaging/GeneratedParts.hpp"
#include "ExyokiOffice/DOM/DocumentFormat/OpenXml/Presentation.hpp"
#include "../OfficePasswordVerifier.hpp"

#include <optional>
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

    /// The CryptoAPI `ALG_SID` values `cryptAlgorithmSid` names, for the hashes
    /// this library can compute.
    static constexpr UInt32 AlgorithmSidSha1 = 4;
    static constexpr UInt32 AlgorithmSidSha256 = 12;
    static constexpr UInt32 AlgorithmSidSha384 = 13;
    static constexpr UInt32 AlgorithmSidSha512 = 14;

    static UInt32 AlgorithmSid(PasswordHashAlgorithm algorithm)
    {
        switch (algorithm)
        {
            case PasswordHashAlgorithm::Sha1:
                return AlgorithmSidSha1;
            case PasswordHashAlgorithm::Sha256:
                return AlgorithmSidSha256;
            case PasswordHashAlgorithm::Sha384:
                return AlgorithmSidSha384;
            default:
                return AlgorithmSidSha512;
        }
    }

    static std::optional<PasswordHashAlgorithm> ParseAlgorithmSid(UInt32 sid)
    {
        switch (sid)
        {
            case AlgorithmSidSha1:
                return PasswordHashAlgorithm::Sha1;
            case AlgorithmSidSha256:
                return PasswordHashAlgorithm::Sha256;
            case AlgorithmSidSha384:
                return PasswordHashAlgorithm::Sha384;
            case AlgorithmSidSha512:
                return PasswordHashAlgorithm::Sha512;
            default:
                return std::nullopt;
        }
    }

    /// The ISO attribute group: `algorithmName`, `saltValue`, `hashValue`, `spinValue`.
    static bool HasModernVerifier(const Presentation::ModificationVerifier::Ptr& node)
    {
        return node->GetHashValue().IsDefined();
    }

    /// The `cryptAlgorithmSid`/`saltData`/`hashData`/`spinCount` group, which is
    /// what PowerPoint itself writes and what `CT_ModifyVerifier` requires.
    static bool HasLegacyVerifier(const Presentation::ModificationVerifier::Ptr& node)
    {
        return node->GetHashData().IsDefined();
    }

    /// Reads the verifier out of whichever attribute group carries it.
    ///
    /// The two groups differ only in spelling: both hold the same salt, the
    /// same iteration count, and the same iterated hash, with the algorithm
    /// named as text in one and as a CryptoAPI identifier in the other. This
    /// was confirmed against a presentation PowerPoint wrote: recomputing its
    /// `hashData` with the ISO formula reproduces it byte for byte.
    struct StoredVerifier
    {
        std::optional<PasswordHashAlgorithm> Algorithm;
        std::vector<Byte> Salt;
        std::vector<Byte> Hash;
        UInt32 SpinCount = 0;
    };

    static StoredVerifier ReadVerifier(const Presentation::ModificationVerifier::Ptr& node)
    {
        StoredVerifier stored;
        if (HasModernVerifier(node))
        {
            stored.Algorithm = OfficePasswordVerifier::ParseAlgorithmName(node->GetAlgorithmName().View());
            stored.Salt = node->GetSaltValue().ValueOr({});
            stored.Hash = node->GetHashValue().ValueOr({});
            stored.SpinCount = node->GetSpinValue().ValueOr(0U);
            return stored;
        }

        stored.Algorithm = ParseAlgorithmSid(node->GetCryptographicAlgorithmSid().ValueOr(0U));
        stored.Salt = node->GetSaltData().ValueOr({});
        // The generated accessor types this attribute as a string even though
        // the schema calls it base64Binary, so the decoding happens here.
        stored.Hash = Base64BinaryValue(node->GetHashData().View()).ValueOr({});
        stored.SpinCount = node->GetSpinCount().ValueOr(0U);
        return stored;
    }

    /// Writes the attribute group PowerPoint writes.
    ///
    /// `CT_ModifyVerifier` declares the seven `crypt*`/`*Data`/`spinCount`
    /// attributes required and the ISO ones optional, and PowerPoint writes
    /// exactly the required seven and none of the ISO ones. Writing only the
    /// ISO group produced a presentation that failed schema validation on seven
    /// counts while looking, to this library, entirely correct.
    static void WriteVerifier(const Presentation::ModificationVerifier::Ptr& node, std::string_view password)
    {
        const auto salt = OfficePasswordVerifier::GenerateSalt();
        const auto hash = OfficePasswordVerifier::ComputeVerifier(PasswordHashAlgorithm::Sha512, password, salt,
                                                                  OfficePasswordVerifier::DefaultSpinCount);

        node->SetCryptographicProviderType(
            EnumValue<Presentation::CryptProviderValues>(Presentation::CryptProviderValues::RsaAES));
        node->SetCryptographicAlgorithmClass(
            EnumValue<Presentation::CryptAlgorithmClassValues>(Presentation::CryptAlgorithmClassValues::Hash));
        node->SetCryptographicAlgorithmType(
            EnumValue<Presentation::CryptAlgorithmValues>(Presentation::CryptAlgorithmValues::TypeAny));
        node->SetCryptographicAlgorithmSid(UInt32Value(AlgorithmSid(PasswordHashAlgorithm::Sha512)));
        node->SetSpinCount(UInt32Value(OfficePasswordVerifier::DefaultSpinCount));
        node->SetSaltData(Base64BinaryValue(salt));
        node->SetHashData(StringValue(Base64BinaryValue(hash).ToString()));
    }

    /// Recomputes the stored verifier for @p password.
    /// \return The failure result, or a successful result when the password matches.
    static PresentationProtectionResult VerifyPassword(const Presentation::ModificationVerifier::Ptr& node,
                                                       std::string_view password)
    {
        const auto stored = ReadVerifier(node);
        if (!stored.Algorithm)
        {
            return {PresentationProtectionError::UnsupportedVerifier,
                    "The modify-protection password uses a hash algorithm this API cannot compute."};
        }

        if (stored.SpinCount > OfficePasswordVerifier::MaximumSpinCount)
        {
            return {PresentationProtectionError::UnsupportedVerifier,
                    "The modify-protection password uses an unsupported iteration count."};
        }

        const auto computed =
            OfficePasswordVerifier::ComputeVerifier(*stored.Algorithm, password, stored.Salt, stored.SpinCount);
        if (!OfficePasswordVerifier::VerifiersEqual(computed, stored.Hash))
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
    result.VerifierSupported =
        result.HasPassword && ModifyProtectionHelpers::ReadVerifier(node).Algorithm.has_value();
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

    if (ModifyProtectionHelpers::HasModernVerifier(verifier) ||
        ModifyProtectionHelpers::HasLegacyVerifier(verifier))
    {
        if (const auto verification = ModifyProtectionHelpers::VerifyPassword(verifier, password);
            !verification.Succeeded())
        {
            return verification;
        }
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
