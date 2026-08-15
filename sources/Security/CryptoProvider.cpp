// Copyright (c) 2026 Jakub Melka and Contributors
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "ExyokiOffice/Security/CryptoProvider.hpp"

#include "MessageDigest.hpp"
#include "ExyokiOffice/StandardTypes.hpp"

namespace ExyokiOffice::Security
{

/// Algorithm identifiers used by XML Signature. The digest identifiers come
/// from two different specifications, which is why they do not share a prefix.
class CryptoAlgorithmNames final
{
public:
    CryptoAlgorithmNames() = delete;

    static constexpr std::string_view Sha1Digest = "http://www.w3.org/2000/09/xmldsig#sha1";
    static constexpr std::string_view Sha256Digest = "http://www.w3.org/2001/04/xmlenc#sha256";
    static constexpr std::string_view Sha384Digest = "http://www.w3.org/2001/04/xmldsig-more#sha384";
    static constexpr std::string_view Sha512Digest = "http://www.w3.org/2001/04/xmlenc#sha512";
    /// Alternative SHA-384 identifier written by some applications.
    static constexpr std::string_view Sha384DigestAlternate = "http://www.w3.org/2001/04/xmlenc#sha384";

    static constexpr std::string_view RsaSha1 = "http://www.w3.org/2000/09/xmldsig#rsa-sha1";
    static constexpr std::string_view RsaSha256 = "http://www.w3.org/2001/04/xmldsig-more#rsa-sha256";
    static constexpr std::string_view RsaSha384 = "http://www.w3.org/2001/04/xmldsig-more#rsa-sha384";
    static constexpr std::string_view RsaSha512 = "http://www.w3.org/2001/04/xmldsig-more#rsa-sha512";
    static constexpr std::string_view EcdsaSha256 = "http://www.w3.org/2001/04/xmldsig-more#ecdsa-sha256";
    static constexpr std::string_view EcdsaSha384 = "http://www.w3.org/2001/04/xmldsig-more#ecdsa-sha384";
    static constexpr std::string_view EcdsaSha512 = "http://www.w3.org/2001/04/xmldsig-more#ecdsa-sha512";
};

std::string_view GetDigestAlgorithmUri(DigestAlgorithm algorithm) noexcept
{
    switch (algorithm)
    {
        case DigestAlgorithm::Sha1:
            return CryptoAlgorithmNames::Sha1Digest;
        case DigestAlgorithm::Sha256:
            return CryptoAlgorithmNames::Sha256Digest;
        case DigestAlgorithm::Sha384:
            return CryptoAlgorithmNames::Sha384Digest;
        case DigestAlgorithm::Sha512:
            return CryptoAlgorithmNames::Sha512Digest;
    }
    return {};
}

std::optional<DigestAlgorithm> ParseDigestAlgorithmUri(std::string_view uri) noexcept
{
    if (uri == CryptoAlgorithmNames::Sha1Digest)
    {
        return DigestAlgorithm::Sha1;
    }
    if (uri == CryptoAlgorithmNames::Sha256Digest)
    {
        return DigestAlgorithm::Sha256;
    }
    if (uri == CryptoAlgorithmNames::Sha384Digest || uri == CryptoAlgorithmNames::Sha384DigestAlternate)
    {
        return DigestAlgorithm::Sha384;
    }
    if (uri == CryptoAlgorithmNames::Sha512Digest)
    {
        return DigestAlgorithm::Sha512;
    }
    return std::nullopt;
}

std::string_view GetSignatureAlgorithmUri(SignatureAlgorithm algorithm) noexcept
{
    switch (algorithm)
    {
        case SignatureAlgorithm::RsaSha1:
            return CryptoAlgorithmNames::RsaSha1;
        case SignatureAlgorithm::RsaSha256:
            return CryptoAlgorithmNames::RsaSha256;
        case SignatureAlgorithm::RsaSha384:
            return CryptoAlgorithmNames::RsaSha384;
        case SignatureAlgorithm::RsaSha512:
            return CryptoAlgorithmNames::RsaSha512;
        case SignatureAlgorithm::EcdsaSha256:
            return CryptoAlgorithmNames::EcdsaSha256;
        case SignatureAlgorithm::EcdsaSha384:
            return CryptoAlgorithmNames::EcdsaSha384;
        case SignatureAlgorithm::EcdsaSha512:
            return CryptoAlgorithmNames::EcdsaSha512;
    }
    return {};
}

std::optional<SignatureAlgorithm> ParseSignatureAlgorithmUri(std::string_view uri) noexcept
{
    if (uri == CryptoAlgorithmNames::RsaSha1)
    {
        return SignatureAlgorithm::RsaSha1;
    }
    if (uri == CryptoAlgorithmNames::RsaSha256)
    {
        return SignatureAlgorithm::RsaSha256;
    }
    if (uri == CryptoAlgorithmNames::RsaSha384)
    {
        return SignatureAlgorithm::RsaSha384;
    }
    if (uri == CryptoAlgorithmNames::RsaSha512)
    {
        return SignatureAlgorithm::RsaSha512;
    }
    if (uri == CryptoAlgorithmNames::EcdsaSha256)
    {
        return SignatureAlgorithm::EcdsaSha256;
    }
    if (uri == CryptoAlgorithmNames::EcdsaSha384)
    {
        return SignatureAlgorithm::EcdsaSha384;
    }
    if (uri == CryptoAlgorithmNames::EcdsaSha512)
    {
        return SignatureAlgorithm::EcdsaSha512;
    }
    return std::nullopt;
}

DigestAlgorithm GetDigestAlgorithm(SignatureAlgorithm algorithm) noexcept
{
    switch (algorithm)
    {
        case SignatureAlgorithm::RsaSha1:
            return DigestAlgorithm::Sha1;
        case SignatureAlgorithm::RsaSha256:
        case SignatureAlgorithm::EcdsaSha256:
            return DigestAlgorithm::Sha256;
        case SignatureAlgorithm::RsaSha384:
        case SignatureAlgorithm::EcdsaSha384:
            return DigestAlgorithm::Sha384;
        case SignatureAlgorithm::RsaSha512:
        case SignatureAlgorithm::EcdsaSha512:
            return DigestAlgorithm::Sha512;
    }
    return DigestAlgorithm::Sha256;
}

std::vector<Byte> ComputeDigest(DigestAlgorithm algorithm, std::span<const Byte> data)
{
    return MessageDigest::Compute(algorithm, data);
}

bool ICryptoProvider::ComputeDigest(DigestAlgorithm algorithm,
                                    std::span<const Byte> data,
                                    std::vector<Byte>& digest) const
{
    digest = MessageDigest::Compute(algorithm, data);
    return !digest.empty();
}

} // namespace ExyokiOffice::Security
