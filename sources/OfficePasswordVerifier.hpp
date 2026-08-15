// Copyright (c) 2026 Jakub Melka and Contributors
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#pragma once

#include "ExyokiOffice/Export.hpp"
#include "ExyokiOffice/StandardTypes.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

namespace ExyokiOffice::Protection
{

/// Hash algorithms usable inside an ISO/IEC 29500 password verifier.
enum class PasswordHashAlgorithm
{
    Sha1,
    Sha256,
    Sha384,
    Sha512
};

/// \brief Shared implementation of the ISO/IEC 29500 password verifier.
///
/// The verifier is the algorithmName/hashValue/saltValue/spinCount attribute
/// group used by Word document protection, Word write protection, PowerPoint
/// modify protection, and the ISO variants of the SpreadsheetML protection
/// elements. It stores an iterated salted hash of the password:
///
///     H(0) = Hash(salt + UTF16LE(password))
///     H(n) = Hash(H(n-1) + LE32(n-1))    for n = 1 .. spinCount
///
/// The verifier only lets a consumer application recognize the correct
/// password. It is not encryption: every part of the package stays readable
/// without the password, and any tool can drop the verifier. Real
/// confidentiality requires package encryption, which this class does not
/// implement.
/// \note The class is exported so the unit tests can check it against
///       published hash test vectors. The header is internal and not installed.
class EXYOKIOFFICE_EXPORT OfficePasswordVerifier final
{
public:
    OfficePasswordVerifier() = delete;

    /// Algorithm written by this library; matches current Word and PowerPoint.
    static constexpr std::string_view DefaultAlgorithmName = "SHA-512";
    /// Iteration count written by this library.
    static constexpr UInt32 DefaultSpinCount = 100000;
    /// Salt length in bytes written by this library.
    static constexpr Size DefaultSaltSize = 16;
    /// Largest iteration count accepted when reading a foreign verifier.
    static constexpr UInt32 MaximumSpinCount = 10000000;

    /// Maps an OOXML algorithm name to a supported algorithm.
    /// \return std::nullopt for names this library cannot compute.
    static std::optional<PasswordHashAlgorithm> ParseAlgorithmName(std::string_view name);

    /// Produces a fresh random salt of the requested length.
    static std::vector<Byte> GenerateSalt(Size size = DefaultSaltSize);

    /// Computes the iterated salted password hash stored as `hashValue`.
    static std::vector<Byte> ComputeVerifier(PasswordHashAlgorithm algorithm,
                                             std::string_view password,
                                             const std::vector<Byte>& salt,
                                             UInt32 spinCount);

    /// Compares two verifiers without leaking the mismatch position by timing.
    static bool VerifiersEqual(const std::vector<Byte>& left,
                               const std::vector<Byte>& right) noexcept;
};

} // namespace ExyokiOffice::Protection
