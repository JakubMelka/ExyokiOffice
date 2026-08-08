// Copyright (c) 2026 Jakub Melka and Collaborators
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

// Verification measured against a signature this library did not write.
//
// The unit-layer signature tests sign and then verify with the same code, so
// they prove the two halves agree with each other and nothing about whether
// either agrees with Office. `word/DigitalSignature.docx` was signed by Word,
// which makes it the only fixture that can answer that: its digests were
// computed by a conforming implementation over canonical forms this library
// now has to reproduce byte for byte - the two Objects, and the XAdES
// SignedProperties element nested inside one of them.
//
// The second case is the invariant the first one depends on. Canonicalization
// signs characters, not elements, so a writer that re-indents a signature part
// on the way out replaces the very bytes the digest was taken over.

#include "doctest.h"

#include "CorpusManifest.hpp"
#include "TestSupport.hpp"

#include "ExyokiOffice/OpenXmlPackage.hpp"
#include "ExyokiOffice/Security/PackageSignatures.hpp"

#include <string>

namespace
{
using ExyokiOffice::OpenXmlPackage;
using ExyokiOffice::Security::SignatureCheck;
using ExyokiOffice::Security::VerifySignatures;

constexpr const char* kSignedFixture = "word/DigitalSignature.docx";
constexpr const char* kSignaturePartUri = "/_xmlsignatures/sig1.xml";

/// The fixture, or a failed REQUIRE when the manifest stopped describing it.
ExyokiOfficeTests::CorpusDocument SignedDocument()
{
    for (const auto& document : ExyokiOfficeTests::CorpusManifest())
    {
        if (document.File == kSignedFixture)
        {
            return document;
        }
    }
    REQUIRE_MESSAGE(false, "the corpus manifest no longer describes ", kSignedFixture);
    return {};
}
} // namespace

TEST_SUITE("Corpus signatures")
{

    TEST_CASE("a signature written by Word verifies [corpus] [corpus-signatures]")
    {
        OpenXmlPackage package;
        REQUIRE(package.LoadFromFile(SignedDocument().Path()));

        // No crypto provider: the signature value needs one, every digest in the
        // signature does not, and the digests are what measures canonicalization.
        const auto verification = VerifySignatures(package);
        REQUIRE(verification.Signatures.size() == 1U);

        const auto& signature = verification.Signatures.front();
        for (const auto& warning : signature.Warnings)
        {
            CAPTURE(warning);
        }
        CHECK(signature.PartUri == kSignaturePartUri);
        CHECK(signature.ContentIntegrity == SignatureCheck::Valid);
        CHECK(verification.AllContentIntact());

        // Every reference of the signature, not just the ones over parts: the
        // same-document references cover the two Objects and the XAdES
        // SignedProperties, and those are the canonicalized ones.
        ExyokiOffice::Size sameDocument = 0;
        for (const auto& reference : signature.References)
        {
            CAPTURE(reference.Uri);
            CHECK(reference.Digest == SignatureCheck::Valid);
            if (!reference.Uri.empty() && reference.Uri.front() == '#')
            {
                ++sameDocument;
            }
        }
        CHECK(sameDocument == 3U);
    }

    TEST_CASE("saving leaves the signature part byte for byte [corpus] [corpus-signatures]")
    {
        const auto document = SignedDocument();
        const auto original = ExyokiOfficeTests::ReadAllBytes(document.Path());
        REQUIRE_FALSE(original.empty());

        OpenXmlPackage package;
        REQUIRE(package.LoadFromMemory(original));
        const auto storedXml = package.GetPartByUri(kSignaturePartUri)->GetXmlString();

        const auto saved = package.SaveToMemory();
        REQUIRE_FALSE(saved.empty());

        OpenXmlPackage reopened;
        REQUIRE(reopened.LoadFromMemory(saved));
        const auto reopenedPart = reopened.GetPartByUri(kSignaturePartUri);
        REQUIRE(reopenedPart);

        // Not merely equivalent XML: the same characters. Re-indenting the part
        // or rewriting its declaration would leave a signature no conforming
        // verifier can check, and this library's own verifier would not notice.
        CHECK(reopenedPart->GetXmlString() == storedXml);
        CHECK(storedXml.find("\n\t") == std::string::npos);

        // What a save still costs: the signature part survives, but the parts it
        // covers are re-serialized from their trees, so their digests no longer
        // match. This is what SignatureSavePolicy warns about, and this case
        // pins it - the day the writer starts writing content parts as stored,
        // this flips and the assertion has to be turned around.
        const auto verification = VerifySignatures(reopened);
        REQUIRE(verification.Signatures.size() == 1U);
        CHECK(verification.Signatures.front().ContentIntegrity == SignatureCheck::Invalid);
    }
}
