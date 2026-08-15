// Copyright (c) 2026 Jakub Melka and Contributors
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "doctest.h"

#include "Security/MessageDigest.hpp"

#include "ExyokiOffice/Security/CryptoProvider.hpp"
#include "ExyokiOffice/StandardTypes.hpp"

#include <cstdint>
#include <optional> // doctest stringifies the std::optional results of the URI parsers
#include <ostream>  // doctest stringifies the std::string_view returned by the URI mappers
#include <string>
#include <string_view>
#include <vector>

namespace
{
using ExyokiOffice::Security::DigestAlgorithm;
using ExyokiOffice::Security::MessageDigest;

std::vector<ExyokiOffice::Byte> Bytes(std::string_view text)
{
    return std::vector<ExyokiOffice::Byte>(text.begin(), text.end());
}

std::string ToHex(const std::vector<ExyokiOffice::Byte>& value)
{
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string result;
    result.reserve(value.size() * 2);
    for (const auto byte : value)
    {
        result.push_back(kDigits[byte >> 4]);
        result.push_back(kDigits[byte & 0x0FU]);
    }
    return result;
}
} // namespace

TEST_SUITE("Security message digests")
{

    TEST_CASE("SHA-1 matches the published test vectors [unit] [security] [digest]")
    {
        CHECK(ToHex(MessageDigest::Sha1(Bytes(""))) == "da39a3ee5e6b4b0d3255bfef95601890afd80709");
        CHECK(ToHex(MessageDigest::Sha1(Bytes("abc"))) == "a9993e364706816aba3e25717850c26c9cd0d89d");
        CHECK(ToHex(MessageDigest::Sha1(Bytes("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"))) ==
              "84983e441c3bd26ebaae4aa1f95129e5e54670f1");
    }

    TEST_CASE("SHA-256 matches the published test vectors [unit] [security] [digest]")
    {
        CHECK(ToHex(MessageDigest::Sha256(Bytes(""))) ==
              "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
        CHECK(ToHex(MessageDigest::Sha256(Bytes("abc"))) ==
              "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
        CHECK(ToHex(MessageDigest::Sha256(Bytes("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"))) ==
              "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    }

    TEST_CASE("SHA-384 matches the published test vectors [unit] [security] [digest]")
    {
        CHECK(ToHex(MessageDigest::Sha384(Bytes(""))) ==
              "38b060a751ac96384cd9327eb1b1e36a21fdb71114be07434c0cc7bf63f6e1da274edebfe76f65fbd51ad2f14898b95b");
        CHECK(ToHex(MessageDigest::Sha384(Bytes("abc"))) ==
              "cb00753f45a35e8bb5a03d699ac65007272c32ab0eded1631a8b605a43ff5bed8086072ba1e7cc2358baeca134c825a7");
    }

    TEST_CASE("SHA-512 matches the published test vectors [unit] [security] [digest]")
    {
        CHECK(ToHex(MessageDigest::Sha512(Bytes(""))) ==
              "cf83e1357eefb8bdf1542850d66d8007d620e4050b5715dc83f4a921d36ce9ce47d0d13c5d85f2b0ff8318d2877eec2f"
              "63b931bd47417a81a538327af927da3e");
        CHECK(ToHex(MessageDigest::Sha512(Bytes("abc"))) ==
              "ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a2192992a274fc1a836ba3c23a3feebbd"
              "454d4423643ce80e2a9ac94fa54ca49f");
    }

    TEST_CASE("Compute dispatches to the selected algorithm [unit] [security] [digest]")
    {
        const auto data = Bytes("ExyokiOffice");
        CHECK(MessageDigest::Compute(DigestAlgorithm::Sha1, data) == MessageDigest::Sha1(data));
        CHECK(MessageDigest::Compute(DigestAlgorithm::Sha256, data) == MessageDigest::Sha256(data));
        CHECK(MessageDigest::Compute(DigestAlgorithm::Sha384, data) == MessageDigest::Sha384(data));
        CHECK(MessageDigest::Compute(DigestAlgorithm::Sha512, data) == MessageDigest::Sha512(data));

        CHECK(MessageDigest::GetDigestSize(DigestAlgorithm::Sha1) == 20U);
        CHECK(MessageDigest::GetDigestSize(DigestAlgorithm::Sha256) == 32U);
        CHECK(MessageDigest::GetDigestSize(DigestAlgorithm::Sha384) == 48U);
        CHECK(MessageDigest::GetDigestSize(DigestAlgorithm::Sha512) == 64U);
    }

    TEST_CASE("The public ComputeDigest wrapper uses the same implementation [unit] [security] [digest]")
    {
        const auto data = Bytes("ExyokiOffice");
        CHECK(ExyokiOffice::Security::ComputeDigest(DigestAlgorithm::Sha256, data) ==
              MessageDigest::Sha256(data));
    }

    TEST_CASE("DigestsEqual rejects mismatched and empty digests [unit] [security] [digest]")
    {
        const auto left = MessageDigest::Sha256(Bytes("abc"));
        auto right = left;
        CHECK(MessageDigest::DigestsEqual(left, right));

        right.back() ^= 0x01U;
        CHECK_FALSE(MessageDigest::DigestsEqual(left, right));
        CHECK_FALSE(MessageDigest::DigestsEqual(left, {}));
        CHECK_FALSE(MessageDigest::DigestsEqual({}, {}));
    }

    TEST_CASE("Algorithm identifiers round-trip [unit] [security] [digest]")
    {
        using ExyokiOffice::Security::GetDigestAlgorithmUri;
        using ExyokiOffice::Security::GetSignatureAlgorithmUri;
        using ExyokiOffice::Security::ParseDigestAlgorithmUri;
        using ExyokiOffice::Security::ParseSignatureAlgorithmUri;
        using ExyokiOffice::Security::SignatureAlgorithm;

        CHECK(GetDigestAlgorithmUri(DigestAlgorithm::Sha256) == "http://www.w3.org/2001/04/xmlenc#sha256");
        CHECK(ParseDigestAlgorithmUri(GetDigestAlgorithmUri(DigestAlgorithm::Sha512)) == DigestAlgorithm::Sha512);
        CHECK(ParseDigestAlgorithmUri("http://www.w3.org/2001/04/xmlenc#sha384") == DigestAlgorithm::Sha384);
        CHECK_FALSE(ParseDigestAlgorithmUri("http://example.com/unknown").has_value());

        CHECK(GetSignatureAlgorithmUri(SignatureAlgorithm::RsaSha256) ==
              "http://www.w3.org/2001/04/xmldsig-more#rsa-sha256");
        CHECK(ParseSignatureAlgorithmUri(GetSignatureAlgorithmUri(SignatureAlgorithm::EcdsaSha384)) ==
              SignatureAlgorithm::EcdsaSha384);
        CHECK(ExyokiOffice::Security::GetDigestAlgorithm(SignatureAlgorithm::RsaSha512) == DigestAlgorithm::Sha512);
    }

} // TEST_SUITE
