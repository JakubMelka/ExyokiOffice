// Copyright (c) 2026 Jakub Melka and Contributors
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#pragma once

#include "ExyokiOffice/Export.hpp"
#include "ExyokiOffice/Security/CryptoProvider.hpp"
#include "ExyokiOffice/StandardTypes.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace ExyokiOffice::Security
{

/// \brief Self-contained SHA-1 and SHA-2 digests.
///
/// The library must not depend on an external crypto stack, so the hash
/// functions are implemented here. They are used for password verifiers
/// (see ExyokiOffice::Protection::OfficePasswordVerifier) and for the digests
/// of an OPC digital signature. Hashing alone is not confidentiality and not a
/// signature; the asymmetric part always comes from an ICryptoProvider.
/// \note The class is exported so the unit tests can check it against published
///       hash test vectors. The header is internal and not installed.
class EXYOKIOFFICE_EXPORT MessageDigest final
{
public:
    MessageDigest() = delete;

    /// Computes the digest selected by \p algorithm.
    static std::vector<Byte> Compute(DigestAlgorithm algorithm, std::span<const Byte> data);

    /// Returns the digest length in bytes for \p algorithm.
    static Size GetDigestSize(DigestAlgorithm algorithm) noexcept;

    /// Computes a SHA-1 digest (20 bytes).
    static std::vector<Byte> Sha1(std::span<const Byte> data);

    /// Computes a SHA-256 digest (32 bytes).
    static std::vector<Byte> Sha256(std::span<const Byte> data);

    /// Computes a SHA-384 digest (48 bytes).
    static std::vector<Byte> Sha384(std::span<const Byte> data);

    /// Computes a SHA-512 digest (64 bytes).
    static std::vector<Byte> Sha512(std::span<const Byte> data);

    /// Compares two digests without leaking the mismatch position by timing.
    static bool DigestsEqual(std::span<const Byte> left, std::span<const Byte> right) noexcept;
};

} // namespace ExyokiOffice::Security
