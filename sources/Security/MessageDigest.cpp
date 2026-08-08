// Copyright (c) 2026 Jakub Melka and Collaborators
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "MessageDigest.hpp"
#include "ExyokiOffice/StandardTypes.hpp"

#include <array>

namespace ExyokiOffice::Security
{

/// Block plumbing shared by the SHA-1 and SHA-2 compression functions.
class MessageDigestHelpers final
{
public:
    MessageDigestHelpers() = delete;

    static constexpr UInt32 RotateLeft32(UInt32 value, unsigned bits) noexcept
    {
        return (value << bits) | (value >> (32U - bits));
    }

    static constexpr UInt32 RotateRight32(UInt32 value, unsigned bits) noexcept
    {
        return (value >> bits) | (value << (32U - bits));
    }

    static constexpr UInt64 RotateRight64(UInt64 value, unsigned bits) noexcept
    {
        return (value >> bits) | (value << (64U - bits));
    }

    /// Applies the SHA-1/SHA-256 padding rule: a 0x80 byte, zero padding to a
    /// 56 byte remainder, then the big-endian bit length.
    static std::vector<Byte> PadTo512BitBlocks(std::span<const Byte> data)
    {
        std::vector<Byte> padded(data.begin(), data.end());
        const UInt64 bitLength = static_cast<UInt64>(data.size()) * 8U;
        padded.push_back(0x80U);
        while (padded.size() % 64U != 56U)
        {
            padded.push_back(0x00U);
        }
        for (int shift = 56; shift >= 0; shift -= 8)
        {
            padded.push_back(static_cast<UInt8>((bitLength >> shift) & 0xFFU));
        }
        return padded;
    }

    /// Applies the SHA-512 padding rule with a 128-bit big-endian bit length.
    /// The high 64 bits are always zero for data held in memory.
    static std::vector<Byte> PadTo1024BitBlocks(std::span<const Byte> data)
    {
        std::vector<Byte> padded(data.begin(), data.end());
        const UInt64 bitLength = static_cast<UInt64>(data.size()) * 8U;
        padded.push_back(0x80U);
        while (padded.size() % 128U != 112U)
        {
            padded.push_back(0x00U);
        }
        padded.insert(padded.end(), 8U, 0x00U);
        for (int shift = 56; shift >= 0; shift -= 8)
        {
            padded.push_back(static_cast<UInt8>((bitLength >> shift) & 0xFFU));
        }
        return padded;
    }

    static UInt32 ReadBigEndian32(const UInt8* source) noexcept
    {
        return (static_cast<UInt32>(source[0]) << 24) | (static_cast<UInt32>(source[1]) << 16) |
               (static_cast<UInt32>(source[2]) << 8) | static_cast<UInt32>(source[3]);
    }

    static UInt64 ReadBigEndian64(const UInt8* source) noexcept
    {
        UInt64 value = 0;
        for (Size index = 0; index < 8U; ++index)
        {
            value = (value << 8) | static_cast<UInt64>(source[index]);
        }
        return value;
    }

    static void AppendBigEndian32(std::vector<Byte>& buffer, UInt32 value)
    {
        for (int shift = 24; shift >= 0; shift -= 8)
        {
            buffer.push_back(static_cast<UInt8>((value >> shift) & 0xFFU));
        }
    }

    static void AppendBigEndian64(std::vector<Byte>& buffer, UInt64 value)
    {
        for (int shift = 56; shift >= 0; shift -= 8)
        {
            buffer.push_back(static_cast<UInt8>((value >> shift) & 0xFFU));
        }
    }

    /// Shared SHA-512/SHA-384 core; the two differ only in the initial state
    /// and in the truncation of the final digest.
    static std::vector<Byte> Sha512Family(std::span<const Byte> data, bool truncateToSha384);
};

std::vector<Byte> MessageDigest::Sha1(std::span<const Byte> data)
{
    std::array<UInt32, 5> state{0x67452301U, 0xEFCDAB89U, 0x98BADCFEU, 0x10325476U, 0xC3D2E1F0U};
    const auto padded = MessageDigestHelpers::PadTo512BitBlocks(data);

    for (Size offset = 0; offset < padded.size(); offset += 64U)
    {
        std::array<UInt32, 80> schedule{};
        for (Size index = 0; index < 16U; ++index)
        {
            schedule[index] = MessageDigestHelpers::ReadBigEndian32(padded.data() + offset + index * 4U);
        }
        for (Size index = 16U; index < 80U; ++index)
        {
            schedule[index] = MessageDigestHelpers::RotateLeft32(
                schedule[index - 3] ^ schedule[index - 8] ^ schedule[index - 14] ^ schedule[index - 16], 1U);
        }

        UInt32 a = state[0];
        UInt32 b = state[1];
        UInt32 c = state[2];
        UInt32 d = state[3];
        UInt32 e = state[4];

        for (Size index = 0; index < 80U; ++index)
        {
            UInt32 mixed = 0;
            UInt32 constant = 0;
            if (index < 20U)
            {
                mixed = (b & c) | (~b & d);
                constant = 0x5A827999U;
            }
            else if (index < 40U)
            {
                mixed = b ^ c ^ d;
                constant = 0x6ED9EBA1U;
            }
            else if (index < 60U)
            {
                mixed = (b & c) | (b & d) | (c & d);
                constant = 0x8F1BBCDCU;
            }
            else
            {
                mixed = b ^ c ^ d;
                constant = 0xCA62C1D6U;
            }

            const UInt32 temporary = MessageDigestHelpers::RotateLeft32(a, 5U) + mixed + e + constant + schedule[index];
            e = d;
            d = c;
            c = MessageDigestHelpers::RotateLeft32(b, 30U);
            b = a;
            a = temporary;
        }

        state[0] += a;
        state[1] += b;
        state[2] += c;
        state[3] += d;
        state[4] += e;
    }

    std::vector<Byte> digest;
    digest.reserve(20U);
    for (const auto word : state)
    {
        MessageDigestHelpers::AppendBigEndian32(digest, word);
    }
    return digest;
}

std::vector<Byte> MessageDigest::Sha256(std::span<const Byte> data)
{
    static constexpr std::array<UInt32, 64> kConstants{
        0x428A2F98U, 0x71374491U, 0xB5C0FBCFU, 0xE9B5DBA5U, 0x3956C25BU, 0x59F111F1U, 0x923F82A4U, 0xAB1C5ED5U,
        0xD807AA98U, 0x12835B01U, 0x243185BEU, 0x550C7DC3U, 0x72BE5D74U, 0x80DEB1FEU, 0x9BDC06A7U, 0xC19BF174U,
        0xE49B69C1U, 0xEFBE4786U, 0x0FC19DC6U, 0x240CA1CCU, 0x2DE92C6FU, 0x4A7484AAU, 0x5CB0A9DCU, 0x76F988DAU,
        0x983E5152U, 0xA831C66DU, 0xB00327C8U, 0xBF597FC7U, 0xC6E00BF3U, 0xD5A79147U, 0x06CA6351U, 0x14292967U,
        0x27B70A85U, 0x2E1B2138U, 0x4D2C6DFCU, 0x53380D13U, 0x650A7354U, 0x766A0ABBU, 0x81C2C92EU, 0x92722C85U,
        0xA2BFE8A1U, 0xA81A664BU, 0xC24B8B70U, 0xC76C51A3U, 0xD192E819U, 0xD6990624U, 0xF40E3585U, 0x106AA070U,
        0x19A4C116U, 0x1E376C08U, 0x2748774CU, 0x34B0BCB5U, 0x391C0CB3U, 0x4ED8AA4AU, 0x5B9CCA4FU, 0x682E6FF3U,
        0x748F82EEU, 0x78A5636FU, 0x84C87814U, 0x8CC70208U, 0x90BEFFFAU, 0xA4506CEBU, 0xBEF9A3F7U, 0xC67178F2U};

    std::array<UInt32, 8> state{0x6A09E667U, 0xBB67AE85U, 0x3C6EF372U, 0xA54FF53AU,
                                0x510E527FU, 0x9B05688CU, 0x1F83D9ABU, 0x5BE0CD19U};
    const auto padded = MessageDigestHelpers::PadTo512BitBlocks(data);

    for (Size offset = 0; offset < padded.size(); offset += 64U)
    {
        std::array<UInt32, 64> schedule{};
        for (Size index = 0; index < 16U; ++index)
        {
            schedule[index] = MessageDigestHelpers::ReadBigEndian32(padded.data() + offset + index * 4U);
        }
        for (Size index = 16U; index < 64U; ++index)
        {
            const UInt32 s0 = MessageDigestHelpers::RotateRight32(schedule[index - 15], 7U) ^
                              MessageDigestHelpers::RotateRight32(schedule[index - 15], 18U) ^
                              (schedule[index - 15] >> 3U);
            const UInt32 s1 = MessageDigestHelpers::RotateRight32(schedule[index - 2], 17U) ^
                              MessageDigestHelpers::RotateRight32(schedule[index - 2], 19U) ^
                              (schedule[index - 2] >> 10U);
            schedule[index] = schedule[index - 16] + s0 + schedule[index - 7] + s1;
        }

        std::array<UInt32, 8> working = state;
        for (Size index = 0; index < 64U; ++index)
        {
            const UInt32 s1 = MessageDigestHelpers::RotateRight32(working[4], 6U) ^
                              MessageDigestHelpers::RotateRight32(working[4], 11U) ^
                              MessageDigestHelpers::RotateRight32(working[4], 25U);
            const UInt32 choice = (working[4] & working[5]) ^ (~working[4] & working[6]);
            const UInt32 temporary1 = working[7] + s1 + choice + kConstants[index] + schedule[index];
            const UInt32 s0 = MessageDigestHelpers::RotateRight32(working[0], 2U) ^
                              MessageDigestHelpers::RotateRight32(working[0], 13U) ^
                              MessageDigestHelpers::RotateRight32(working[0], 22U);
            const UInt32 majority = (working[0] & working[1]) ^ (working[0] & working[2]) ^ (working[1] & working[2]);
            const UInt32 temporary2 = s0 + majority;

            working[7] = working[6];
            working[6] = working[5];
            working[5] = working[4];
            working[4] = working[3] + temporary1;
            working[3] = working[2];
            working[2] = working[1];
            working[1] = working[0];
            working[0] = temporary1 + temporary2;
        }

        for (Size index = 0; index < 8U; ++index)
        {
            state[index] += working[index];
        }
    }

    std::vector<Byte> digest;
    digest.reserve(32U);
    for (const auto word : state)
    {
        MessageDigestHelpers::AppendBigEndian32(digest, word);
    }
    return digest;
}

std::vector<Byte> MessageDigestHelpers::Sha512Family(std::span<const Byte> data, bool truncateToSha384)
{
    static constexpr std::array<UInt64, 80> kConstants{
        0x428A2F98D728AE22ULL, 0x7137449123EF65CDULL, 0xB5C0FBCFEC4D3B2FULL, 0xE9B5DBA58189DBBCULL,
        0x3956C25BF348B538ULL, 0x59F111F1B605D019ULL, 0x923F82A4AF194F9BULL, 0xAB1C5ED5DA6D8118ULL,
        0xD807AA98A3030242ULL, 0x12835B0145706FBEULL, 0x243185BE4EE4B28CULL, 0x550C7DC3D5FFB4E2ULL,
        0x72BE5D74F27B896FULL, 0x80DEB1FE3B1696B1ULL, 0x9BDC06A725C71235ULL, 0xC19BF174CF692694ULL,
        0xE49B69C19EF14AD2ULL, 0xEFBE4786384F25E3ULL, 0x0FC19DC68B8CD5B5ULL, 0x240CA1CC77AC9C65ULL,
        0x2DE92C6F592B0275ULL, 0x4A7484AA6EA6E483ULL, 0x5CB0A9DCBD41FBD4ULL, 0x76F988DA831153B5ULL,
        0x983E5152EE66DFABULL, 0xA831C66D2DB43210ULL, 0xB00327C898FB213FULL, 0xBF597FC7BEEF0EE4ULL,
        0xC6E00BF33DA88FC2ULL, 0xD5A79147930AA725ULL, 0x06CA6351E003826FULL, 0x142929670A0E6E70ULL,
        0x27B70A8546D22FFCULL, 0x2E1B21385C26C926ULL, 0x4D2C6DFC5AC42AEDULL, 0x53380D139D95B3DFULL,
        0x650A73548BAF63DEULL, 0x766A0ABB3C77B2A8ULL, 0x81C2C92E47EDAEE6ULL, 0x92722C851482353BULL,
        0xA2BFE8A14CF10364ULL, 0xA81A664BBC423001ULL, 0xC24B8B70D0F89791ULL, 0xC76C51A30654BE30ULL,
        0xD192E819D6EF5218ULL, 0xD69906245565A910ULL, 0xF40E35855771202AULL, 0x106AA07032BBD1B8ULL,
        0x19A4C116B8D2D0C8ULL, 0x1E376C085141AB53ULL, 0x2748774CDF8EEB99ULL, 0x34B0BCB5E19B48A8ULL,
        0x391C0CB3C5C95A63ULL, 0x4ED8AA4AE3418ACBULL, 0x5B9CCA4F7763E373ULL, 0x682E6FF3D6B2B8A3ULL,
        0x748F82EE5DEFB2FCULL, 0x78A5636F43172F60ULL, 0x84C87814A1F0AB72ULL, 0x8CC702081A6439ECULL,
        0x90BEFFFA23631E28ULL, 0xA4506CEBDE82BDE9ULL, 0xBEF9A3F7B2C67915ULL, 0xC67178F2E372532BULL,
        0xCA273ECEEA26619CULL, 0xD186B8C721C0C207ULL, 0xEADA7DD6CDE0EB1EULL, 0xF57D4F7FEE6ED178ULL,
        0x06F067AA72176FBAULL, 0x0A637DC5A2C898A6ULL, 0x113F9804BEF90DAEULL, 0x1B710B35131C471BULL,
        0x28DB77F523047D84ULL, 0x32CAAB7B40C72493ULL, 0x3C9EBE0A15C9BEBCULL, 0x431D67C49C100D4CULL,
        0x4CC5D4BECB3E42B6ULL, 0x597F299CFC657E2AULL, 0x5FCB6FAB3AD6FAECULL, 0x6C44198C4A475817ULL};

    std::array<UInt64, 8> state =
        truncateToSha384
            ? std::array<UInt64, 8>{0xCBBB9D5DC1059ED8ULL, 0x629A292A367CD507ULL, 0x9159015A3070DD17ULL,
                                    0x152FECD8F70E5939ULL, 0x67332667FFC00B31ULL, 0x8EB44A8768581511ULL,
                                    0xDB0C2E0D64F98FA7ULL, 0x47B5481DBEFA4FA4ULL}
            : std::array<UInt64, 8>{0x6A09E667F3BCC908ULL, 0xBB67AE8584CAA73BULL, 0x3C6EF372FE94F82BULL,
                                    0xA54FF53A5F1D36F1ULL, 0x510E527FADE682D1ULL, 0x9B05688C2B3E6C1FULL,
                                    0x1F83D9ABFB41BD6BULL, 0x5BE0CD19137E2179ULL};

    const auto padded = PadTo1024BitBlocks(data);

    for (Size offset = 0; offset < padded.size(); offset += 128U)
    {
        std::array<UInt64, 80> schedule{};
        for (Size index = 0; index < 16U; ++index)
        {
            schedule[index] = ReadBigEndian64(padded.data() + offset + index * 8U);
        }
        for (Size index = 16U; index < 80U; ++index)
        {
            const UInt64 s0 = RotateRight64(schedule[index - 15], 1U) ^ RotateRight64(schedule[index - 15], 8U) ^
                              (schedule[index - 15] >> 7U);
            const UInt64 s1 = RotateRight64(schedule[index - 2], 19U) ^ RotateRight64(schedule[index - 2], 61U) ^
                              (schedule[index - 2] >> 6U);
            schedule[index] = schedule[index - 16] + s0 + schedule[index - 7] + s1;
        }

        std::array<UInt64, 8> working = state;
        for (Size index = 0; index < 80U; ++index)
        {
            const UInt64 s1 = RotateRight64(working[4], 14U) ^ RotateRight64(working[4], 18U) ^ RotateRight64(working[4], 41U);
            const UInt64 choice = (working[4] & working[5]) ^ (~working[4] & working[6]);
            const UInt64 temporary1 = working[7] + s1 + choice + kConstants[index] + schedule[index];
            const UInt64 s0 = RotateRight64(working[0], 28U) ^ RotateRight64(working[0], 34U) ^ RotateRight64(working[0], 39U);
            const UInt64 majority = (working[0] & working[1]) ^ (working[0] & working[2]) ^ (working[1] & working[2]);
            const UInt64 temporary2 = s0 + majority;

            working[7] = working[6];
            working[6] = working[5];
            working[5] = working[4];
            working[4] = working[3] + temporary1;
            working[3] = working[2];
            working[2] = working[1];
            working[1] = working[0];
            working[0] = temporary1 + temporary2;
        }

        for (Size index = 0; index < 8U; ++index)
        {
            state[index] += working[index];
        }
    }

    std::vector<Byte> digest;
    digest.reserve(64U);
    for (const auto word : state)
    {
        AppendBigEndian64(digest, word);
    }
    if (truncateToSha384)
    {
        digest.resize(48U);
    }
    return digest;
}

std::vector<Byte> MessageDigest::Sha384(std::span<const Byte> data)
{
    return MessageDigestHelpers::Sha512Family(data, true);
}

std::vector<Byte> MessageDigest::Sha512(std::span<const Byte> data)
{
    return MessageDigestHelpers::Sha512Family(data, false);
}

std::vector<Byte> MessageDigest::Compute(DigestAlgorithm algorithm, std::span<const Byte> data)
{
    switch (algorithm)
    {
        case DigestAlgorithm::Sha1:
            return Sha1(data);
        case DigestAlgorithm::Sha256:
            return Sha256(data);
        case DigestAlgorithm::Sha384:
            return Sha384(data);
        case DigestAlgorithm::Sha512:
            return Sha512(data);
    }
    return {};
}

Size MessageDigest::GetDigestSize(DigestAlgorithm algorithm) noexcept
{
    switch (algorithm)
    {
        case DigestAlgorithm::Sha1:
            return 20U;
        case DigestAlgorithm::Sha256:
            return 32U;
        case DigestAlgorithm::Sha384:
            return 48U;
        case DigestAlgorithm::Sha512:
            return 64U;
    }
    return 0U;
}

bool MessageDigest::DigestsEqual(std::span<const Byte> left, std::span<const Byte> right) noexcept
{
    if (left.size() != right.size() || left.empty())
    {
        return false;
    }

    UInt8 difference = 0;
    for (Size index = 0; index < left.size(); ++index)
    {
        difference |= static_cast<UInt8>(left[index] ^ right[index]);
    }
    return difference == 0;
}

} // namespace ExyokiOffice::Security
