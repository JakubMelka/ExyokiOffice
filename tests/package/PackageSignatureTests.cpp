// Copyright (c) 2026 Jakub Melka and Contributors
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "doctest.h"

#include "ExyokiOffice/OpenXmlPackage.hpp"
#include "ExyokiOffice/Security/PackageSignatures.hpp"
#include "ExyokiOffice/Word/WordDocument.hpp"
#include "ExyokiOffice/StandardTypes.hpp"
#include "Security/SignatureNames.hpp"
#include "Security/SignatureXml.hpp"
#include "Security/XmlCanonicalization.hpp"

#include "pugixml/pugixml.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <iterator>
#include <functional>
#include <memory>
#include <sstream>
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
class StubCryptoProvider : public ICryptoProvider
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
    // Deliberately without limits. These tests hand the verifier signature XML
    // built to break it - nested past the canonicalizer's own ceiling, for
    // instance - and the default limits would reject such a package at load
    // time, leaving the behaviour under test unreached.
    package->SetPackageLimits(ExyokiOffice::OpenXmlPackageLimits::Unlimited());
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

bool HasIssueContaining(const ExyokiOffice::ValidationResult& result, std::string_view fragment)
{
    const auto& issues = result.Issues();
    return std::any_of(issues.begin(), issues.end(), [fragment](const auto& issue)
                       { return issue.Message.find(fragment) != std::string::npos; });
}

using ExyokiOffice::Security::SignatureNames;
using ExyokiOffice::Security::SignatureXml;
using ExyokiOffice::Security::XmlCanonicalization;
namespace Pugi = ExyokiOffice::Pugi;

std::vector<ExyokiOffice::Byte> AsBytes(std::string_view text)
{
    return std::vector<ExyokiOffice::Byte>(text.begin(), text.end());
}

/// Finds the element carrying \p id anywhere under \p root.
Pugi::xml_node FindElementById(const Pugi::xml_node& root, std::string_view id)
{
    // Context-relative, and in document order: with a repeated Id the first one
    // wins here, which is what the signature writer would have digested.
    for (const auto& node : root.select_nodes(".//*[@Id]"))
    {
        if (node.node().attribute("Id").as_string() == id)
        {
            return node.node();
        }
    }
    return {};
}

/// Rewrites the signature XML and then makes it self-consistent again.
///
/// Every same-document reference digest is recomputed over the element it names
/// and the signature value is recomputed over SignedInfo, so what the verifier
/// reads is a forgery of the quality an attacker would actually produce rather
/// than something that fails for the incidental reason that a digest no longer
/// matches. A test that skipped this step could not tell "the verifier rejected
/// the forged structure" apart from "the verifier noticed a stale digest".
///
/// This works only because StubCryptoProvider "signs" by digesting: no private
/// key is involved anywhere in this file.
std::string ForgeSignatureXml(std::string xml, const std::function<void(Pugi::xml_node&)>& mutate)
{
    Pugi::xml_document document;
    REQUIRE(document.load_buffer(xml.data(), xml.size(), XmlCanonicalization::ParseOptions));

    auto signature = SignatureXml::FindChild(document, SignatureNames::DsigNamespace, "Signature");
    REQUIRE(signature);
    mutate(signature);

    auto signedInfo = SignatureXml::FindChild(signature, SignatureNames::DsigNamespace, "SignedInfo");
    REQUIRE(signedInfo);

    for (auto reference : SignatureXml::FindChildren(signedInfo, SignatureNames::DsigNamespace, "Reference"))
    {
        const std::string uri = reference.attribute("URI").as_string();
        if (uri.empty() || uri.front() != '#')
        {
            continue;
        }

        const auto target = FindElementById(signature, std::string_view(uri).substr(1));
        REQUIRE(target);
        const auto canonical = XmlCanonicalization::CanonicalizeSubtree(target);
        REQUIRE(canonical.has_value());

        const auto digest = ExyokiOffice::Security::ComputeDigest(DigestAlgorithm::Sha256, AsBytes(*canonical));
        auto digestValue = SignatureXml::FindChild(reference, SignatureNames::DsigNamespace, "DigestValue");
        REQUIRE(digestValue);
        digestValue.text().set(SignatureXml::EncodeBase64(digest).c_str());
    }

    const auto canonicalSignedInfo = XmlCanonicalization::CanonicalizeSubtree(signedInfo);
    REQUIRE(canonicalSignedInfo.has_value());
    const auto signatureValue =
        ExyokiOffice::Security::ComputeDigest(DigestAlgorithm::Sha256, AsBytes(*canonicalSignedInfo));
    auto signatureValueNode = SignatureXml::FindChild(signature, SignatureNames::DsigNamespace, "SignatureValue");
    REQUIRE(signatureValueNode);
    signatureValueNode.text().set(SignatureXml::EncodeBase64(signatureValue).c_str());

    // format_raw so that what is stored parses back into the very tree the
    // digests were taken over; any added indentation would be part of the
    // canonical form and would break them.
    std::ostringstream out;
    document.save(out, "", Pugi::format_raw);
    return out.str();
}

/// Replaces the text of the first paragraph, so a part the signature covers
/// differs from what was signed.
void TamperWithDocumentText(OpenXmlPackage& package)
{
    auto document = package.GetPartByUri("/word/document.xml");
    REQUIRE(document);
    auto xml = document->GetXmlString();
    const std::string original = "Signed content";
    const auto position = xml.find(original);
    REQUIRE(position != std::string::npos);
    xml.replace(position, original.size(), "Forged content");
    document->SetXmlString(xml);
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

    TEST_CASE("SHA-1 is refused unless the caller asks for it [unit] [security] [signature]")
    {
        // A signature commits to a digest, so a digest that collisions can be
        // constructed for is a signature two different documents share. Plenty
        // of archived packages carry SHA-1 signatures, which is why this is a
        // policy the caller sets rather than a format this library cannot read.
        auto provider = std::make_shared<StubCryptoProvider>();
        auto editor = CreateDocument();

        SignPackageOptions options;
        options.Digest = DigestAlgorithm::Sha1;
        options.Signature = SignatureAlgorithm::RsaSha1;
        REQUIRE(SignPackage(*editor->GetDocument(), provider, options).Succeeded());
        const auto bytes = editor->SaveToMemory();

        // A genuinely SHA-1 signed package: every digest matches, and the
        // library still refuses to call that evidence.
        auto refusedPackage = LoadPackage(bytes);
        const auto refused = VerifySignatures(*refusedPackage, provider);
        REQUIRE(refused.Signatures.size() == 1U);
        CHECK(refused.Signatures.front().ContentIntegrity == SignatureCheck::Invalid);
        CHECK(refused.Signatures.front().SignatureValue == SignatureCheck::Invalid);
        CHECK_FALSE(refused.Signatures.front().IsValid());
        CHECK(HasIssue(refused.Diagnostics, ValidationErrorId::SignatureUnsupportedAlgorithm));

        ExyokiOffice::Security::VerifySignaturesOptions permissive;
        permissive.AllowSha1 = true;
        auto allowedPackage = LoadPackage(bytes);
        const auto allowed = VerifySignatures(*allowedPackage, provider, permissive);
        REQUIRE(allowed.Signatures.size() == 1U);
        CHECK(allowed.Signatures.front().Digest == DigestAlgorithm::Sha1);
        CHECK(allowed.Signatures.front().Algorithm == SignatureAlgorithm::RsaSha1);
        CHECK(allowed.Signatures.front().IsValid());
    }

    TEST_CASE("RSA-SHA1 is refused with no crypto provider at all [unit] [security] [signature]")
    {
        // Refusing a signature for its algorithm reads that algorithm out of the
        // document: no key, no certificate and no provider are involved. Behind
        // the provider check it never fired for the callers that need it most -
        // `exyoki signatures` and the MCP servers verify content integrity with
        // no provider, and a collision-broken signature came back as merely not
        // checked.
        auto provider = std::make_shared<StubCryptoProvider>();
        auto editor = CreateDocument();

        // SHA-256 references, so the signature method is the only weak thing
        // about this package and the content digests all verify.
        SignPackageOptions options;
        options.Digest = DigestAlgorithm::Sha256;
        options.Signature = SignatureAlgorithm::RsaSha1;
        REQUIRE(SignPackage(*editor->GetDocument(), provider, options).Succeeded());
        const auto bytes = editor->SaveToMemory();

        auto package = LoadPackage(bytes);
        const auto verification = VerifySignatures(*package);
        REQUIRE(verification.Signatures.size() == 1U);
        const auto& signature = verification.Signatures.front();

        CHECK(signature.ContentIntegrity == SignatureCheck::Valid);
        CHECK(signature.SignatureValue == SignatureCheck::Invalid);
        CHECK_FALSE(signature.IsValid());
        CHECK(HasIssue(verification.Diagnostics, ValidationErrorId::SignatureUnsupportedAlgorithm));

        // With AllowSha1 the refusal is gone and the missing provider is once
        // more the reason nothing was checked - the two answers stay distinct.
        ExyokiOffice::Security::VerifySignaturesOptions permissive;
        permissive.AllowSha1 = true;
        auto permissivePackage = LoadPackage(bytes);
        const auto allowed = VerifySignatures(*permissivePackage, nullptr, permissive);
        REQUIRE(allowed.Signatures.size() == 1U);
        CHECK(allowed.Signatures.front().SignatureValue == SignatureCheck::NotChecked);
        CHECK(HasIssue(allowed.Diagnostics, ValidationErrorId::SignatureNotVerified));
    }

    TEST_CASE("A signature says which parts it does not cover [unit] [security] [signature]")
    {
        // Every digest can match and the signature still say nothing about a
        // part it did not name: coverage is a list inside the manifest, and
        // IsValid() reports only that what *is* listed has not changed. Nothing
        // in the signature announces the gap, so the verifier has to.
        auto provider = std::make_shared<StubCryptoProvider>();

        {
            auto editor = CreateDocument();
            REQUIRE(SignPackage(*editor->GetDocument(), provider).Succeeded());
            auto package = LoadPackage(editor->GetDocument()->SaveToMemory());

            const auto verification = VerifySignatures(*package, provider);
            REQUIRE(verification.Signatures.size() == 1U);
            REQUIRE(verification.Signatures.front().IsValid());
            // Signing everything leaves nothing over; the signature parts and
            // their origin are excluded by definition, not counted as gaps.
            CHECK(verification.Signatures.front().UncoveredParts.empty());
        }

        auto editor = CreateDocument();
        SignPackageOptions options;
        options.PartUris = {"/word/document.xml"};
        REQUIRE(SignPackage(*editor->GetDocument(), provider, options).Succeeded());
        auto package = LoadPackage(editor->GetDocument()->SaveToMemory());

        const auto verification = VerifySignatures(*package, provider);
        REQUIRE(verification.Signatures.size() == 1U);
        const auto& signature = verification.Signatures.front();

        // Nothing is wrong with the signature: it is valid, and it covers one part.
        CHECK(signature.ContentIntegrity == SignatureCheck::Valid);
        CHECK(signature.IsValid());
        CHECK_FALSE(signature.UncoveredParts.empty());
        CHECK(std::find(signature.UncoveredParts.begin(), signature.UncoveredParts.end(), "/word/document.xml") ==
              signature.UncoveredParts.end());
        CHECK(std::find(signature.UncoveredParts.begin(), signature.UncoveredParts.end(),
                        "/docProps/core.xml") != signature.UncoveredParts.end());
        CHECK(std::is_sorted(signature.UncoveredParts.begin(), signature.UncoveredParts.end()));
    }

    TEST_CASE("Wrapping the signed object does not move the manifest out of reach [unit] [security] [signature]")
    {
        // Regression guard for a signature-wrapping bypass. Nesting the signed
        // Object inside a second Object leaves both the same-document digest and
        // the signature value intact - the inner element canonicalizes to the
        // same bytes it always did - while moving the Manifest out of the place
        // a verifier looks if it collects manifests from the Signature's own
        // Object children. Such a verifier then enforces no part digest at all,
        // and reports an edited package as intact content.
        auto provider = std::make_shared<StubCryptoProvider>();
        const auto bytes = CreateSignedPackage(provider);

        const auto wrapped = RepackageWithSignatureXml(
            bytes,
            [](std::string xml)
            {
                return ForgeSignatureXml(
                    std::move(xml),
                    [](Pugi::xml_node& signature)
                    {
                        auto packageObject = FindElementById(signature, "idPackageObject");
                        REQUIRE(packageObject);

                        auto wrapper = signature.insert_child_before(Pugi::node_element, packageObject);
                        // Same qualified name, so the wrapper is a dsig:Object too.
                        wrapper.set_name(packageObject.name());
                        wrapper.append_attribute("Id").set_value("wrapperObject");
                        wrapper.append_move(packageObject);
                    });
            });

        ExyokiOffice::Security::VerifySignaturesOptions options;
        options.ByteSource = ExyokiOffice::Security::PartByteSource::Current;

        // The manifest still has to be found and enforced: the wrapped signature
        // is self-consistent, so anything else would be rejecting it for the
        // wrong reason and would say nothing about part coverage.
        {
            auto package = LoadPackage(wrapped);
            const auto verification = VerifySignatures(*package, provider, options);
            REQUIRE(verification.Signatures.size() == 1U);

            const auto& signature = verification.Signatures.front();
            CHECK(signature.ContentIntegrity == SignatureCheck::Valid);
            CHECK(signature.IsValid());

            const auto& references = signature.References;
            CHECK(std::any_of(references.begin(), references.end(), [](const auto& reference)
                              { return reference.PartUri == "/word/document.xml"; }));
        }

        // ... and with the document edited, the same manifest is what catches it.
        {
            auto package = LoadPackage(wrapped);
            TamperWithDocumentText(*package);

            const auto verification = VerifySignatures(*package, provider, options);
            REQUIRE(verification.Signatures.size() == 1U);

            const auto& signature = verification.Signatures.front();
            CHECK(signature.ContentIntegrity == SignatureCheck::Invalid);
            CHECK_FALSE(signature.IsValid());
            CHECK_FALSE(verification.AllValid());
            CHECK(HasIssue(verification.Diagnostics, ValidationErrorId::SignatureDigestMismatch));
        }
    }

    TEST_CASE("A signature covering no package part is not content evidence [unit] [security] [signature]")
    {
        // The Object holding the Manifest is removed along with the SignedInfo
        // reference that named it, and the signature is then made consistent
        // again: every digest matches, the signature value verifies, and the
        // whole thing says nothing whatsoever about the package. "Nothing to
        // check" must not come out the same as "checked and intact".
        auto provider = std::make_shared<StubCryptoProvider>();
        const auto bytes = CreateSignedPackage(provider);

        const auto forged = RepackageWithSignatureXml(
            bytes,
            [](std::string xml)
            {
                return ForgeSignatureXml(
                    std::move(xml),
                    [](Pugi::xml_node& signature)
                    {
                        auto signedInfo =
                            SignatureXml::FindChild(signature, SignatureNames::DsigNamespace, "SignedInfo");
                        REQUIRE(signedInfo);

                        for (auto reference : SignatureXml::FindChildren(
                                 signedInfo, SignatureNames::DsigNamespace, "Reference"))
                        {
                            if (std::string_view(reference.attribute("URI").as_string()) == "#idPackageObject")
                            {
                                signedInfo.remove_child(reference);
                                break;
                            }
                        }

                        auto packageObject = FindElementById(signature, "idPackageObject");
                        REQUIRE(packageObject);
                        signature.remove_child(packageObject);
                    });
            });

        auto package = LoadPackage(forged);
        TamperWithDocumentText(*package);

        ExyokiOffice::Security::VerifySignaturesOptions options;
        options.ByteSource = ExyokiOffice::Security::PartByteSource::Current;
        const auto verification = VerifySignatures(*package, provider, options);
        REQUIRE(verification.Signatures.size() == 1U);

        const auto& signature = verification.Signatures.front();
        // The forgery is internally sound - which is exactly why the verdict
        // has to come from coverage rather than from a broken digest.
        CHECK(signature.SignatureValue == SignatureCheck::Valid);
        CHECK(signature.ContentIntegrity == SignatureCheck::Invalid);
        CHECK_FALSE(signature.IsValid());
        CHECK_FALSE(verification.AllValid());
        CHECK(HasIssueContaining(verification.Diagnostics, "covers no package part"));
    }

    TEST_CASE("A manifest naming only its own signature is not coverage [unit] [security] [signature]")
    {
        // A subtler form of the previous forgery: the Manifest stays, so a check
        // for "is there a manifest, and does it have entries" is satisfied. Its
        // one entry is a bare-name reference into the signature XML, though, so
        // it digests an element of the signature itself and names no part and no
        // relationship set. Everything verifies and nothing about the package is
        // established.
        auto provider = std::make_shared<StubCryptoProvider>();
        const auto bytes = CreateSignedPackage(provider);

        const auto forged = RepackageWithSignatureXml(
            bytes,
            [](std::string xml)
            {
                return ForgeSignatureXml(
                    std::move(xml),
                    [](Pugi::xml_node& signature)
                    {
                        auto packageObject = FindElementById(signature, "idPackageObject");
                        REQUIRE(packageObject);
                        auto manifest =
                            SignatureXml::FindChild(packageObject, SignatureNames::DsigNamespace, "Manifest");
                        REQUIRE(manifest);

                        // The element the surviving entry will point at. Nothing
                        // rewrites it afterwards, so its digest stays correct.
                        auto decoy = signature.append_child(Pugi::node_element);
                        decoy.set_name(packageObject.name());
                        decoy.append_attribute("Id").set_value("idDecoyObject");
                        decoy.text().set("nothing to see here");

                        // Keep one entry, so the manifest is neither empty nor
                        // structurally odd, and repoint it at the decoy.
                        auto references = SignatureXml::FindChildren(manifest, SignatureNames::DsigNamespace,
                                                                     "Reference");
                        REQUIRE_FALSE(references.empty());
                        for (auto it = std::next(references.begin()); it != references.end(); ++it)
                        {
                            manifest.remove_child(*it);
                        }

                        auto kept = references.front();
                        kept.attribute("URI").set_value("#idDecoyObject");
                        // The relationship transform would not apply to an
                        // element inside the signature.
                        if (auto transforms =
                                SignatureXml::FindChild(kept, SignatureNames::DsigNamespace, "Transforms"))
                        {
                            kept.remove_child(transforms);
                        }

                        const auto canonical = XmlCanonicalization::CanonicalizeSubtree(decoy);
                        REQUIRE(canonical.has_value());
                        const auto digest =
                            ExyokiOffice::Security::ComputeDigest(DigestAlgorithm::Sha256, AsBytes(*canonical));
                        auto digestValue =
                            SignatureXml::FindChild(kept, SignatureNames::DsigNamespace, "DigestValue");
                        REQUIRE(digestValue);
                        digestValue.text().set(SignatureXml::EncodeBase64(digest).c_str());
                    });
            });

        auto package = LoadPackage(forged);
        TamperWithDocumentText(*package);

        ExyokiOffice::Security::VerifySignaturesOptions options;
        options.ByteSource = ExyokiOffice::Security::PartByteSource::Current;
        const auto verification = VerifySignatures(*package, provider, options);
        REQUIRE(verification.Signatures.size() == 1U);

        const auto& signature = verification.Signatures.front();
        CHECK(signature.SignatureValue == SignatureCheck::Valid);
        // Every digest in the forgery matches, including the manifest entry.
        const auto& references = signature.References;
        CHECK(std::all_of(references.begin(), references.end(),
                          [](const auto& reference)
                          { return reference.Digest == SignatureCheck::Valid; }));
        CHECK(std::none_of(references.begin(), references.end(),
                           [](const auto& reference)
                           { return !reference.PartUri.empty(); }));

        CHECK(signature.ContentIntegrity == SignatureCheck::Invalid);
        CHECK_FALSE(signature.IsValid());
        CHECK_FALSE(verification.AllValid());
        CHECK(HasIssueContaining(verification.Diagnostics, "covers no package part"));
    }

    TEST_CASE("An id repeated from the Signature element fails the signature [unit] [security] [signature]")
    {
        // The root Signature carries an Id like any other element, so a nested
        // element repeating it is the same ambiguity as two nested elements
        // sharing one: this library resolves the bare name to one of them, and
        // another verifier may resolve it to the root.
        auto provider = std::make_shared<StubCryptoProvider>();
        const auto bytes = CreateSignedPackage(provider);

        const auto forged = RepackageWithSignatureXml(
            bytes,
            [](std::string xml)
            {
                return ForgeSignatureXml(
                    std::move(xml),
                    [](Pugi::xml_node& signature)
                    {
                        const std::string signatureId = signature.attribute("Id").as_string();
                        REQUIRE_FALSE(signatureId.empty());

                        auto packageObject = FindElementById(signature, "idPackageObject");
                        REQUIRE(packageObject);

                        auto decoy = signature.append_child(Pugi::node_element);
                        decoy.set_name(packageObject.name());
                        decoy.append_attribute("Id").set_value(signatureId.c_str());
                    });
            });

        auto package = LoadPackage(forged);
        const auto verification = VerifySignatures(*package, provider);
        REQUIRE(verification.Signatures.size() == 1U);

        const auto& signature = verification.Signatures.front();
        CHECK(signature.ContentIntegrity == SignatureCheck::Invalid);
        CHECK_FALSE(signature.IsValid());
        CHECK(HasIssue(verification.Diagnostics, ValidationErrorId::SignatureMalformed));
        CHECK(HasIssueContaining(verification.Diagnostics, "same Id"));
    }

    TEST_CASE("A repeated element id fails the signature [unit] [security] [signature]")
    {
        // With two elements carrying the same Id, which one `#idPackageObject`
        // resolves to is up to the verifier: this one could check the real
        // manifest while Word checks the decoy, or the other way round. Nothing
        // in the document decides it, so the signature is refused instead.
        auto provider = std::make_shared<StubCryptoProvider>();
        const auto bytes = CreateSignedPackage(provider);

        const auto forged = RepackageWithSignatureXml(
            bytes,
            [](std::string xml)
            {
                return ForgeSignatureXml(
                    std::move(xml),
                    [](Pugi::xml_node& signature)
                    {
                        auto packageObject = FindElementById(signature, "idPackageObject");
                        REQUIRE(packageObject);

                        auto decoy = signature.append_child(Pugi::node_element);
                        decoy.set_name(packageObject.name());
                        decoy.append_attribute("Id").set_value("idPackageObject");
                    });
            });

        auto package = LoadPackage(forged);
        const auto verification = VerifySignatures(*package, provider);
        REQUIRE(verification.Signatures.size() == 1U);

        const auto& signature = verification.Signatures.front();
        CHECK(signature.ContentIntegrity == SignatureCheck::Invalid);
        CHECK_FALSE(signature.IsValid());
        CHECK(HasIssue(verification.Diagnostics, ValidationErrorId::SignatureMalformed));
        CHECK(HasIssueContaining(verification.Diagnostics, "same Id"));
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

    TEST_CASE("The provider is offered the whole embedded certificate chain [unit] [security] [signature]")
    {
        // A provider that builds a path to its own trust anchors needs the
        // intermediates, and the signature is the only place they exist. Only
        // the leaf used to be passed, so such a provider had to answer with
        // less than the signature actually carried.
        class ChainRecordingProvider final : public StubCryptoProvider
        {
        public:
            bool VerifyDataWithChain(SignatureAlgorithm algorithm,
                                     std::span<const ExyokiOffice::Byte> data,
                                     std::span<const ExyokiOffice::Byte> signature,
                                     std::span<const std::vector<ExyokiOffice::Byte>> chain) const override
            {
                ChainSize = chain.size();
                return StubCryptoProvider::VerifyDataWithChain(algorithm, data, signature, chain);
            }

            mutable ExyokiOffice::Size ChainSize = 0;
        };

        auto provider = std::make_shared<ChainRecordingProvider>();
        const auto bytes = CreateSignedPackage(provider);

        auto package = LoadPackage(bytes);
        const auto result = VerifySignatures(*package, provider);
        REQUIRE(result.Signatures.size() == 1);
        CHECK(result.Signatures.front().IsValid());
        CHECK(provider->ChainSize == result.Signatures.front().Certificates.size());
        CHECK(provider->ChainSize >= 1);
    }

    TEST_CASE("A provider that implements only VerifyData still works [unit] [security] [signature]")
    {
        // The chain-aware entry point has a default implementation that forwards
        // the leaf, so an existing provider does not have to be rewritten.
        auto provider = std::make_shared<StubCryptoProvider>();
        const auto bytes = CreateSignedPackage(provider);

        auto package = LoadPackage(bytes);
        const auto result = VerifySignatures(*package, provider);
        REQUIRE(result.Signatures.size() == 1);
        CHECK(result.Signatures.front().SignatureValue == SignatureCheck::Valid);
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
