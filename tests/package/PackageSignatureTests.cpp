// Copyright (c) 2026 Jakub Melka and Contributors
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "doctest.h"

#include "ExyokiOffice/OpenXmlPackage.hpp"
#include "ExyokiOffice/Security/PackageSignatures.hpp"
#include "ExyokiOffice/Word/WordDocument.hpp"
#include "ExyokiOffice/StandardTypes.hpp"
#include "Security/XmlCanonicalization.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace
{
using ExyokiOffice::OpenXmlPackage;
using ExyokiOffice::PartByteRetention;
using ExyokiOffice::SignatureSavePolicy;
using ExyokiOffice::ValidationErrorId;
using ExyokiOffice::Security::DigestAlgorithm;
using ExyokiOffice::Security::ICryptoProvider;
using ExyokiOffice::Security::SignatureAlgorithm;
using ExyokiOffice::Security::SignatureCheck;
using ExyokiOffice::Security::SignPackage;
using ExyokiOffice::Security::SignPackageOptions;
using ExyokiOffice::Security::VerifySignatures;
using ExyokiOffice::Word::WordDocumentEditor;

/// Stands in for a real crypto backend in tests.
///
/// It performs no cryptography at all: the "signature" is the digest of the
/// signed bytes, which is enough to prove that the same canonical form is
/// produced when signing and when verifying, and that the plumbing around the
/// provider works. A real provider signs with a private key.
class StubCryptoProvider final : public ICryptoProvider
{
public:
    explicit StubCryptoProvider(std::vector<ExyokiOffice::Byte> certificate = {0x30, 0x82, 0x01, 0x0A})
        : m_certificate(std::move(certificate))
    {
    }

    SignatureAlgorithm GetSignatureAlgorithm() const override { return SignatureAlgorithm::RsaSha256; }

    std::vector<std::vector<ExyokiOffice::Byte>> GetCertificateChain() const override
    {
        if (m_certificate.empty())
        {
            return {};
        }
        return {m_certificate};
    }

    bool SignData(SignatureAlgorithm algorithm,
                  std::span<const ExyokiOffice::Byte> data,
                  std::vector<ExyokiOffice::Byte>& signature) const override
    {
        signature = ExyokiOffice::Security::ComputeDigest(ExyokiOffice::Security::GetDigestAlgorithm(algorithm),
                                                          data);
        return !signature.empty();
    }

    bool VerifyData(SignatureAlgorithm algorithm,
                    std::span<const ExyokiOffice::Byte> data,
                    std::span<const ExyokiOffice::Byte> signature,
                    std::span<const ExyokiOffice::Byte> certificateDer) const override
    {
        if (!std::equal(certificateDer.begin(), certificateDer.end(), m_certificate.begin(), m_certificate.end()))
        {
            return false;
        }
        const auto expected =
            ExyokiOffice::Security::ComputeDigest(ExyokiOffice::Security::GetDigestAlgorithm(algorithm), data);
        return std::equal(expected.begin(), expected.end(), signature.begin(), signature.end());
    }

private:
    std::vector<ExyokiOffice::Byte> m_certificate;
};

WordDocumentEditor::Ptr CreateDocument()
{
    auto editor = WordDocumentEditor::CreateNew();
    REQUIRE(editor);
    editor->AddParagraph("Signed content");
    return editor;
}

/// Signs a fresh document and returns the saved package bytes.
std::vector<ExyokiOffice::Byte> CreateSignedPackage(const std::shared_ptr<StubCryptoProvider>& provider)
{
    auto editor = CreateDocument();
    const auto result = SignPackage(*editor->GetDocument(), provider);
    REQUIRE_MESSAGE(result.Succeeded(), result.Message);
    const auto bytes = editor->SaveToMemory();
    REQUIRE(!bytes.empty());
    return bytes;
}

std::unique_ptr<OpenXmlPackage> LoadPackage(const std::vector<ExyokiOffice::Byte>& bytes)
{
    auto package = std::make_unique<OpenXmlPackage>();
    REQUIRE(package->LoadFromMemory(bytes));
    return package;
}

/// Rewrites the signature XML and repackages, so the tampered signature is what
/// the verifier reads from the file. Editing the loaded part would not do:
/// verification deliberately uses the bytes the part was stored with.
std::vector<ExyokiOffice::Byte> RepackageWithSignatureXml(const std::vector<ExyokiOffice::Byte>& bytes,
                                                          const std::function<std::string(std::string)>& transform)
{
    auto package = LoadPackage(bytes);
    auto signature = package->GetPartByUri("/_xmlsignatures/sig1.xml");
    REQUIRE(signature);
    signature->SetXmlString(transform(signature->GetXmlString()));

    package->SetSignatureSavePolicy(SignatureSavePolicy::Ignore);
    auto saved = package->SaveToMemory();
    REQUIRE(!saved.empty());
    return saved;
}

bool HasIssue(const ExyokiOffice::ValidationResult& result, ValidationErrorId id)
{
    const auto& issues = result.Issues();
    return std::any_of(issues.begin(), issues.end(), [id](const auto& issue)
                       { return issue.Id == id; });
}
} // namespace

TEST_SUITE("Package signatures")
{

    TEST_CASE("Signing creates the origin and signature parts [unit] [security] [signature]")
    {
        auto provider = std::make_shared<StubCryptoProvider>();
        auto editor = CreateDocument();

        const auto result = SignPackage(*editor->GetDocument(), provider);
        REQUIRE_MESSAGE(result.Succeeded(), result.Message);
        CHECK(result.SignaturePartUri == "/_xmlsignatures/sig1.xml");

        auto origin = editor->GetDocument()->GetPartByUri("/_xmlsignatures/origin.sigs");
        REQUIRE(origin);
        CHECK(origin->ContentType() == "application/vnd.openxmlformats-package.digital-signature-origin");

        auto signature = editor->GetDocument()->GetPartByUri(result.SignaturePartUri);
        REQUIRE(signature);
        CHECK(signature->ContentType() ==
              "application/vnd.openxmlformats-package.digital-signature-xmlsignature+xml");
        CHECK(signature->GetXmlString().find("idPackageObject") != std::string::npos);
    }

    TEST_CASE("A signature survives a save and reload round-trip [unit] [security] [signature]")
    {
        auto provider = std::make_shared<StubCryptoProvider>();
        const auto bytes = CreateSignedPackage(provider);

        auto package = LoadPackage(bytes);
        CHECK(ExyokiOffice::Security::HasSignatures(*package));

        const auto verification = VerifySignatures(*package, provider);
        REQUIRE(verification.Signatures.size() == 1U);

        const auto& signature = verification.Signatures.front();
        CHECK(signature.ContentIntegrity == SignatureCheck::Valid);
        CHECK(signature.SignatureValue == SignatureCheck::Valid);
        CHECK(signature.IsValid());
        CHECK(verification.AllValid());
        CHECK(signature.Algorithm == SignatureAlgorithm::RsaSha256);
        CHECK(signature.Digest == DigestAlgorithm::Sha256);
        CHECK(signature.Certificates.size() == 1U);
        CHECK(signature.SigningTime.has_value());
        CHECK(!signature.References.empty());
    }

    TEST_CASE("Saving a signed package keeps the save-time properties frozen [unit] [security] [signature]")
    {
        // Regression guard. Saving normally refreshes `dcterms:modified`, which
        // rewrites a part the signature has just digested. The round-trip test
        // above only caught this when the clock happened to cross a second
        // boundary between signing and saving, so it passed almost always.
        // Backdating the timestamp makes the rewrite - and therefore the bug -
        // deterministic: if BeforeSave() still updated the properties, the
        // stored 2001 value would become "now" and the digest would not match.
        auto provider = std::make_shared<StubCryptoProvider>();
        auto editor = CreateDocument();
        auto document = editor->GetDocument();

        const auto backdated = std::chrono::system_clock::from_time_t(1000000000); // 2001-09-09Z
        REQUIRE(document->Properties().SetModified(backdated));

        const auto result = SignPackage(*document, provider);
        REQUIRE_MESSAGE(result.Succeeded(), result.Message);
        CHECK(document->IsSaveTimePropertyUpdateSuppressed());

        const auto bytes = editor->SaveToMemory();
        REQUIRE(!bytes.empty());
        // The save consumed the one-shot suppression.
        CHECK_FALSE(document->IsSaveTimePropertyUpdateSuppressed());

        auto package = LoadPackage(bytes);
        const auto verification = VerifySignatures(*package, provider);
        REQUIRE(verification.Signatures.size() == 1U);
        CHECK(verification.Signatures.front().ContentIntegrity == SignatureCheck::Valid);
        CHECK(verification.AllValid());

        // The signed package really does still carry the backdated value.
        auto reopened = ExyokiOffice::Packaging::WordDocument::Open(bytes);
        REQUIRE(reopened);
        const auto modified = reopened->Properties().GetModified();
        REQUIRE(modified.has_value());
        CHECK(std::chrono::floor<std::chrono::seconds>(*modified) ==
              std::chrono::floor<std::chrono::seconds>(backdated));
    }

    TEST_CASE("A later save updates the properties again [unit] [security] [signature]")
    {
        // The suppression is one-shot: once the signed bytes are out, the next
        // save behaves normally, because the document may have been edited.
        auto provider = std::make_shared<StubCryptoProvider>();
        auto editor = CreateDocument();
        auto document = editor->GetDocument();

        const auto backdated = std::chrono::system_clock::from_time_t(1000000000);
        REQUIRE(document->Properties().SetModified(backdated));
        REQUIRE(SignPackage(*document, provider).Succeeded());

        REQUIRE(!editor->SaveToMemory().empty());   // consumes the suppression
        const auto second = editor->SaveToMemory(); // ordinary save again
        REQUIRE(!second.empty());

        auto reopened = ExyokiOffice::Packaging::WordDocument::Open(second);
        REQUIRE(reopened);
        const auto modified = reopened->Properties().GetModified();
        REQUIRE(modified.has_value());
        CHECK(*modified > backdated);
    }

    TEST_CASE("Content integrity is checked without a crypto provider [unit] [security] [signature]")
    {
        auto provider = std::make_shared<StubCryptoProvider>();
        const auto bytes = CreateSignedPackage(provider);

        auto package = LoadPackage(bytes);
        const auto verification = VerifySignatures(*package);
        REQUIRE(verification.Signatures.size() == 1U);

        // Unchanged content, but nothing proves who signed it.
        CHECK(verification.Signatures.front().ContentIntegrity == SignatureCheck::Valid);
        CHECK(verification.Signatures.front().SignatureValue == SignatureCheck::NotChecked);
        CHECK_FALSE(verification.Signatures.front().IsValid());
        CHECK(verification.AllContentIntact());
        CHECK(HasIssue(verification.Diagnostics, ValidationErrorId::SignatureNotVerified));
    }

    TEST_CASE("Editing a signed part is detected [unit] [security] [signature]")
    {
        auto provider = std::make_shared<StubCryptoProvider>();
        const auto bytes = CreateSignedPackage(provider);

        auto package = LoadPackage(bytes);
        auto document = package->GetPartByUri("/word/document.xml");
        REQUIRE(document);

        auto xml = document->GetXmlString();
        const auto position = xml.find("Signed content");
        REQUIRE(position != std::string::npos);
        xml.replace(position, std::string("Signed content").size(), "Edited content");
        document->SetXmlString(xml);

        ExyokiOffice::Security::VerifySignaturesOptions options;
        options.ByteSource = ExyokiOffice::Security::PartByteSource::Current;
        const auto verification = VerifySignatures(*package, provider, options);

        REQUIRE(verification.Signatures.size() == 1U);
        CHECK(verification.Signatures.front().ContentIntegrity == SignatureCheck::Invalid);
        CHECK_FALSE(verification.AllContentIntact());
        CHECK(HasIssue(verification.Diagnostics, ValidationErrorId::SignatureDigestMismatch));

        const auto& references = verification.Signatures.front().References;
        const auto broken = std::find_if(references.begin(), references.end(), [](const auto& reference)
                                         { return reference.PartUri == "/word/document.xml"; });
        REQUIRE(broken != references.end());
        CHECK(broken->Digest == SignatureCheck::Invalid);
    }

    TEST_CASE("Removing a signed part is detected [unit] [security] [signature]")
    {
        auto provider = std::make_shared<StubCryptoProvider>();
        const auto bytes = CreateSignedPackage(provider);

        auto package = LoadPackage(bytes);
        auto document = package->GetPartByUri("/word/document.xml");
        REQUIRE(document);

        // The main document part is covered by the signature, so dropping it has to
        // be reported rather than silently skipped.
        REQUIRE(package->RemovePartReference(document));
        REQUIRE(package->GetPartByUri("/word/document.xml") == nullptr);

        const auto verification = VerifySignatures(*package, provider);
        REQUIRE(verification.Signatures.size() == 1U);
        CHECK(verification.Signatures.front().ContentIntegrity == SignatureCheck::Invalid);
        CHECK(HasIssue(verification.Diagnostics, ValidationErrorId::SignaturePartMissing));
    }

    TEST_CASE("Verification without retained bytes reports what it could not check [unit] [security] [signature]")
    {
        auto provider = std::make_shared<StubCryptoProvider>();
        const auto bytes = CreateSignedPackage(provider);

        OpenXmlPackage package;
        package.SetPartByteRetention(PartByteRetention::Never);
        REQUIRE(package.LoadFromMemory(bytes));

        const auto verification = VerifySignatures(package, provider);
        REQUIRE(verification.Signatures.size() == 1U);
        CHECK(verification.Signatures.front().ContentIntegrity == SignatureCheck::NotChecked);
        CHECK(HasIssue(verification.Diagnostics, ValidationErrorId::SignatureNotVerified));
    }

    TEST_CASE("Signing fails without a provider or a certificate [unit] [security] [signature]")
    {
        auto editor = CreateDocument();

        const auto withoutProvider = SignPackage(*editor->GetDocument(), nullptr);
        CHECK_FALSE(withoutProvider.Succeeded());
        CHECK(withoutProvider.Error == ExyokiOffice::Security::SignatureError::NoProvider);

        auto emptyProvider = std::make_shared<StubCryptoProvider>(std::vector<ExyokiOffice::Byte>{});
        const auto withoutCertificate = SignPackage(*editor->GetDocument(), emptyProvider);
        CHECK_FALSE(withoutCertificate.Succeeded());
        CHECK(withoutCertificate.Error == ExyokiOffice::Security::SignatureError::NoCertificate);
    }

    TEST_CASE("Signing a part that is not in the package fails [unit] [security] [signature]")
    {
        auto provider = std::make_shared<StubCryptoProvider>();
        auto editor = CreateDocument();

        SignPackageOptions options;
        options.PartUris.emplace_back("/word/missing.xml");

        const auto result = SignPackage(*editor->GetDocument(), provider, options);
        CHECK_FALSE(result.Succeeded());
        CHECK(result.Error == ExyokiOffice::Security::SignatureError::PartMissing);
    }

    TEST_CASE("A signature part that is not a signature is reported [unit] [security] [signature]")
    {
        auto provider = std::make_shared<StubCryptoProvider>();
        const auto bytes = CreateSignedPackage(provider);

        const auto tampered =
            RepackageWithSignatureXml(bytes, [](std::string)
                                      { return std::string("<NotASignature/>"); });

        auto package = LoadPackage(tampered);
        const auto verification = VerifySignatures(*package, provider);
        REQUIRE(verification.Signatures.size() == 1U);
        CHECK(verification.Signatures.front().ContentIntegrity == SignatureCheck::Invalid);
        CHECK(HasIssue(verification.Diagnostics, ValidationErrorId::SignatureMalformed));
    }

    TEST_CASE("An unknown digest algorithm is reported as unsupported [unit] [security] [signature]")
    {
        auto provider = std::make_shared<StubCryptoProvider>();
        const auto bytes = CreateSignedPackage(provider);

        const auto tampered = RepackageWithSignatureXml(bytes, [](std::string xml)
                                                        {
        const std::string known = "http://www.w3.org/2001/04/xmlenc#sha256";
        const auto position = xml.find(known);
        REQUIRE(position != std::string::npos);
        xml.replace(position, known.size(), "http://example.com/digest#unknown");
        return xml; });

        auto package = LoadPackage(tampered);
        const auto verification = VerifySignatures(*package, provider);
        REQUIRE(verification.Signatures.size() == 1U);
        CHECK(verification.Signatures.front().ContentIntegrity == SignatureCheck::NotChecked);
        CHECK(HasIssue(verification.Diagnostics, ValidationErrorId::SignatureUnsupportedAlgorithm));
    }

    TEST_CASE("A tampered signature value does not verify [unit] [security] [signature]")
    {
        auto provider = std::make_shared<StubCryptoProvider>();
        const auto bytes = CreateSignedPackage(provider);

        const auto tampered = RepackageWithSignatureXml(bytes, [](std::string xml)
                                                        {
        const std::string opening = "<SignatureValue>";
        const auto start = xml.find(opening);
        const auto end = xml.find("</SignatureValue>");
        REQUIRE(start != std::string::npos);
        REQUIRE(end != std::string::npos);
        xml.replace(start + opening.size(), end - start - opening.size(), "AAAA");
        return xml; });

        auto package = LoadPackage(tampered);
        const auto verification = VerifySignatures(*package, provider);
        REQUIRE(verification.Signatures.size() == 1U);
        // The content is untouched; only the signature value is wrong.
        CHECK(verification.Signatures.front().ContentIntegrity == SignatureCheck::Valid);
        CHECK(verification.Signatures.front().SignatureValue == SignatureCheck::Invalid);
        CHECK(HasIssue(verification.Diagnostics, ValidationErrorId::SignatureValueInvalid));
    }

    TEST_CASE("A SignedInfo that cannot be canonicalized does not verify [unit] [security] [signature]")
    {
        // The depth cap in the canonicalizer stops the recursion, but stopping
        // is only half the contract: the verifier must treat "no canonical
        // form" as a failure rather than hand an empty buffer to the provider,
        // which would verify against a signature made over an empty input.
        auto provider = std::make_shared<StubCryptoProvider>();
        const auto bytes = CreateSignedPackage(provider);

        const auto tampered = RepackageWithSignatureXml(
            bytes,
            [](std::string xml)
            {
                const std::string closing = "</SignedInfo>";
                const auto position = xml.find(closing);
                REQUIRE(position != std::string::npos);

                constexpr unsigned int levels = ExyokiOffice::Security::XmlCanonicalization::MaximumDepth + 8U;
                std::string nested;
                for (unsigned int level = 0; level < levels; ++level)
                {
                    nested += "<x>";
                }
                for (unsigned int level = 0; level < levels; ++level)
                {
                    nested += "</x>";
                }
                xml.insert(position, nested);
                return xml;
            });

        auto package = LoadPackage(tampered);
        const auto verification = VerifySignatures(*package, provider);
        REQUIRE(verification.Signatures.size() == 1U);
        CHECK(verification.Signatures.front().SignatureValue == SignatureCheck::Invalid);
        CHECK(HasIssue(verification.Diagnostics, ValidationErrorId::SignatureMalformed));
        // Reported as malformed rather than as a value mismatch: the provider
        // was never reached, which is the point.
        CHECK_FALSE(HasIssue(verification.Diagnostics, ValidationErrorId::SignatureValueInvalid));
    }

    TEST_CASE("RemoveAllSignatures leaves an unsigned package [unit] [security] [signature]")
    {
        auto provider = std::make_shared<StubCryptoProvider>();
        const auto bytes = CreateSignedPackage(provider);

        auto package = LoadPackage(bytes);
        REQUIRE(ExyokiOffice::Security::HasSignatures(*package));

        CHECK(ExyokiOffice::Security::RemoveAllSignatures(*package));
        CHECK_FALSE(ExyokiOffice::Security::HasSignatures(*package));
        CHECK(package->GetPartByUri("/_xmlsignatures/origin.sigs") == nullptr);
        CHECK_FALSE(ExyokiOffice::Security::RemoveAllSignatures(*package));
    }

} // TEST_SUITE

TEST_SUITE("Signature save policy")
{

    TEST_CASE("Warn records a diagnostic but still saves [unit] [security] [signature]")
    {
        auto provider = std::make_shared<StubCryptoProvider>();
        const auto bytes = CreateSignedPackage(provider);

        auto package = LoadPackage(bytes);
        CHECK(package->GetSignatureSavePolicy() == SignatureSavePolicy::Warn);

        auto document = package->GetPartByUri("/word/document.xml");
        REQUIRE(document);
        document->SetXmlString("<w:document xmlns:w=\"http://schemas.openxmlformats.org/wordprocessingml/2006/main\">"
                               "<w:body/></w:document>");

        const auto saved = package->SaveToMemory();
        CHECK(!saved.empty());
        CHECK(HasIssue(package->LastValidationResult(), ValidationErrorId::SignatureInvalidatedBySave));
    }

    TEST_CASE("FailSave refuses to write an invalidated signature [unit] [security] [signature]")
    {
        auto provider = std::make_shared<StubCryptoProvider>();
        const auto bytes = CreateSignedPackage(provider);

        auto package = LoadPackage(bytes);
        package->SetSignatureSavePolicy(SignatureSavePolicy::FailSave);

        auto document = package->GetPartByUri("/word/document.xml");
        REQUIRE(document);
        document->SetXmlString("<w:document xmlns:w=\"http://schemas.openxmlformats.org/wordprocessingml/2006/main\">"
                               "<w:body/></w:document>");

        CHECK(package->SaveToMemory().empty());
    }

    TEST_CASE("RemoveSignatures drops the signature parts before writing [unit] [security] [signature]")
    {
        auto provider = std::make_shared<StubCryptoProvider>();
        const auto bytes = CreateSignedPackage(provider);

        auto package = LoadPackage(bytes);
        package->SetSignatureSavePolicy(SignatureSavePolicy::RemoveSignatures);

        const auto saved = package->SaveToMemory();
        REQUIRE(!saved.empty());
        CHECK_FALSE(ExyokiOffice::Security::HasSignatures(*package));

        auto reloaded = LoadPackage(saved);
        CHECK_FALSE(ExyokiOffice::Security::HasSignatures(*reloaded));
    }

    TEST_CASE("Ignore saves without touching anything [unit] [security] [signature]")
    {
        auto provider = std::make_shared<StubCryptoProvider>();
        const auto bytes = CreateSignedPackage(provider);

        auto package = LoadPackage(bytes);
        package->SetSignatureSavePolicy(SignatureSavePolicy::Ignore);

        auto document = package->GetPartByUri("/word/document.xml");
        REQUIRE(document);
        document->SetXmlString("<w:document xmlns:w=\"http://schemas.openxmlformats.org/wordprocessingml/2006/main\">"
                               "<w:body/></w:document>");

        CHECK(!package->SaveToMemory().empty());
        CHECK_FALSE(HasIssue(package->LastValidationResult(), ValidationErrorId::SignatureInvalidatedBySave));
        CHECK(ExyokiOffice::Security::HasSignatures(*package));
    }

    TEST_CASE("An unchanged signed package saves without a warning [unit] [security] [signature]")
    {
        auto provider = std::make_shared<StubCryptoProvider>();
        const auto bytes = CreateSignedPackage(provider);

        auto package = LoadPackage(bytes);
        CHECK(!package->SaveToMemory().empty());
        CHECK_FALSE(HasIssue(package->LastValidationResult(), ValidationErrorId::SignatureInvalidatedBySave));
    }

} // TEST_SUITE
