// Copyright (c) 2026 Jakub Melka and Contributors
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "OfficePasswordVerifier.hpp"

#include "Security/MessageDigest.hpp"
#include "ExyokiOffice/StandardTypes.hpp"
#include "AsciiText.hpp"

#include <algorithm>
#include <random>
#include <string>

namespace ExyokiOffice::Protection
{

/// Byte plumbing the verifier needs on top of the shared digest primitives.
/// The digests themselves live in ExyokiOffice::Security::MessageDigest,
/// because the library must not depend on an external crypto stack and the same
/// hash functions are also needed by the digital signature layer.
class PasswordHashHelpers final
{
public:
    PasswordHashHelpers() = delete;

    static std::vector<Byte> Hash(PasswordHashAlgorithm algorithm,
                                  const std::vector<Byte>& data)
    {
        return Security::MessageDigest::Compute(ToDigestAlgorithm(algorithm), data);
    }

    /// Maps the verifier algorithm onto the shared digest enumeration.
    static Security::DigestAlgorithm ToDigestAlgorithm(PasswordHashAlgorithm algorithm) noexcept
    {
        switch (algorithm)
        {
            case PasswordHashAlgorithm::Sha1:
                return Security::DigestAlgorithm::Sha1;
            case PasswordHashAlgorithm::Sha256:
                return Security::DigestAlgorithm::Sha256;
            case PasswordHashAlgorithm::Sha384:
                return Security::DigestAlgorithm::Sha384;
            case PasswordHashAlgorithm::Sha512:
                return Security::DigestAlgorithm::Sha512;
        }
        return Security::DigestAlgorithm::Sha512;
    }

    /// Encodes UTF-8 text as the UTF-16LE byte sequence hashed by Office.
    /// Malformed input is replaced by U+FFFD so a password always maps to a
    /// deterministic byte string.
    static std::vector<Byte> EncodeUtf16Le(std::string_view text)
    {
        std::vector<Byte> result;
        result.reserve(text.size() * 2);

        Size index = 0;
        while (index < text.size())
        {
            const auto lead = static_cast<UInt8>(text[index]);
            UInt32 codePoint = 0xFFFDU;
            Size length = 1;

            if (lead < 0x80U)
            {
                codePoint = lead;
            }
            else if ((lead & 0xE0U) == 0xC0U)
            {
                length = 2;
                codePoint = lead & 0x1FU;
            }
            else if ((lead & 0xF0U) == 0xE0U)
            {
                length = 3;
                codePoint = lead & 0x0FU;
            }
            else if ((lead & 0xF8U) == 0xF0U)
            {
                length = 4;
                codePoint = lead & 0x07U;
            }

            if (length > 1)
            {
                if (index + length > text.size())
                {
                    codePoint = 0xFFFDU;
                    length = 1;
                }
                else
                {
                    for (Size offset = 1; offset < length; ++offset)
                    {
                        const auto continuation = static_cast<UInt8>(text[index + offset]);
                        if ((continuation & 0xC0U) != 0x80U)
                        {
                            codePoint = 0xFFFDU;
                            length = 1;
                            break;
                        }
                        codePoint = (codePoint << 6) | (continuation & 0x3FU);
                    }
                }
            }

            if (codePoint > 0x10FFFFU || (codePoint >= 0xD800U && codePoint <= 0xDFFFU))
            {
                codePoint = 0xFFFDU;
            }

            if (codePoint <= 0xFFFFU)
            {
                AppendUtf16Unit(result, static_cast<UInt16>(codePoint));
            }
            else
            {
                const UInt32 adjusted = codePoint - 0x10000U;
                AppendUtf16Unit(result, static_cast<UInt16>(0xD800U + (adjusted >> 10)));
                AppendUtf16Unit(result, static_cast<UInt16>(0xDC00U + (adjusted & 0x3FFU)));
            }

            index += length;
        }

        return result;
    }

    /// Appends a 32-bit little-endian iteration counter.
    static void AppendIteration(std::vector<Byte>& buffer, UInt32 iteration)
    {
        buffer.push_back(static_cast<UInt8>(iteration & 0xFFU));
        buffer.push_back(static_cast<UInt8>((iteration >> 8) & 0xFFU));
        buffer.push_back(static_cast<UInt8>((iteration >> 16) & 0xFFU));
        buffer.push_back(static_cast<UInt8>((iteration >> 24) & 0xFFU));
    }

private:
    static void AppendUtf16Unit(std::vector<Byte>& buffer, UInt16 unit)
    {
        buffer.push_back(static_cast<UInt8>(unit & 0xFFU));
        buffer.push_back(static_cast<UInt8>(unit >> 8));
    }
};

std::optional<PasswordHashAlgorithm> OfficePasswordVerifier::ParseAlgorithmName(std::string_view name)
{
    std::string normalized;
    normalized.reserve(name.size());
    for (const auto character : name)
    {
        if (character == '-' || character == '_' || character == ' ')
        {
            continue;
        }
        normalized.push_back(AsciiText::ToUpper(character));
    }

    if (normalized == "SHA1")
    {
        return PasswordHashAlgorithm::Sha1;
    }
    if (normalized == "SHA256" || normalized == "SHA2")
    {
        return PasswordHashAlgorithm::Sha256;
    }
    if (normalized == "SHA384")
    {
        return PasswordHashAlgorithm::Sha384;
    }
    if (normalized == "SHA512")
    {
        return PasswordHashAlgorithm::Sha512;
    }
    return std::nullopt;
}

std::vector<Byte> OfficePasswordVerifier::GenerateSalt(Size size)
{
    std::random_device device;
    std::uniform_int_distribution<unsigned int> distribution(0U, 255U);

    std::vector<Byte> salt(size);
    for (auto& byte : salt)
    {
        byte = static_cast<UInt8>(distribution(device));
    }
    return salt;
}

std::vector<Byte> OfficePasswordVerifier::ComputeVerifier(PasswordHashAlgorithm algorithm,
                                                          std::string_view password,
                                                          const std::vector<Byte>& salt,
                                                          UInt32 spinCount)
{
    std::vector<Byte> buffer = salt;
    const auto encodedPassword = PasswordHashHelpers::EncodeUtf16Le(password);
    buffer.insert(buffer.end(), encodedPassword.begin(), encodedPassword.end());

    auto hash = PasswordHashHelpers::Hash(algorithm, buffer);
    for (UInt32 iteration = 0; iteration < spinCount; ++iteration)
    {
        std::vector<Byte> round = hash;
        PasswordHashHelpers::AppendIteration(round, iteration);
        hash = PasswordHashHelpers::Hash(algorithm, round);
    }
    return hash;
}

bool OfficePasswordVerifier::VerifiersEqual(const std::vector<Byte>& left,
                                            const std::vector<Byte>& right) noexcept
{
    return Security::MessageDigest::DigestsEqual(left, right);
}

} // namespace ExyokiOffice::Protection
