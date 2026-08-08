// Copyright (c) 2026 Jakub Melka and Collaborators
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "ExyokiOffice/Security/PackageSignatures.hpp"

#include "MessageDigest.hpp"
#include "RelationshipTransform.hpp"
#include "SignatureNames.hpp"
#include "SignatureXml.hpp"
#include "XmlCanonicalization.hpp"

#include "ExyokiOffice/Packaging/GeneratedParts.hpp"

#include "OpenXmlPackageUri.hpp"
#include "ExyokiOffice/StandardTypes.hpp"

#include <algorithm>
#include <chrono>
#include <memory>
#include <sstream>
#include <string_view>
#include <utility>

namespace ExyokiOffice::Security
{

/// Locates the signature parts of a package and resolves what a reference points at.
class SignatureLocator final
{
public:
    SignatureLocator() = delete;

    /// Returns the signature origin part, or nullptr when the package has none.
    static std::shared_ptr<OpenXmlPackagePart> FindOriginPart(const OpenXmlPackage& package)
    {
        for (const auto& part : package.Parts())
        {
            if (part && part->ContentType() == SignatureNames::OriginContentType)
            {
                return part;
            }
        }
        return nullptr;
    }

    /// Returns the signature parts in the order the origin part relates to them.
    static std::vector<std::shared_ptr<OpenXmlPackagePart>> FindSignatureParts(const OpenXmlPackage& package)
    {
        std::vector<std::shared_ptr<OpenXmlPackagePart>> signatures;
        const auto origin = FindOriginPart(package);
        if (!origin)
        {
            return signatures;
        }

        for (const auto& part : origin->Parts())
        {
            if (part && part->ContentType() == SignatureNames::SignatureContentType)
            {
                signatures.push_back(part);
            }
        }
        return signatures;
    }

    /// True for URIs of relationship parts, which are not parts of the package.
    static bool IsRelationshipPartUri(std::string_view uri)
    {
        if (uri.size() < 6 || uri.substr(uri.size() - 5) != ".rels")
        {
            return false;
        }
        return uri == "/_rels/.rels" || uri.find("/_rels/") != std::string_view::npos;
    }

    /// Maps a relationship part URI to the container whose relationships it stores.
    static const OpenXmlPartContainer* ResolveRelationshipContainer(const OpenXmlPackage& package,
                                                                    const std::string& relationshipPartUri)
    {
        if (relationshipPartUri == "/_rels/.rels")
        {
            return &package;
        }

        const auto folderEnd = relationshipPartUri.rfind("/_rels/");
        if (folderEnd == std::string::npos)
        {
            return nullptr;
        }

        auto folder = relationshipPartUri.substr(0, folderEnd);
        auto file = relationshipPartUri.substr(folderEnd + 7);
        if (file.size() >= 5)
        {
            file.resize(file.size() - 5);
        }

        const auto ownerUri = Detail::NormalizePartUri(folder + "/" + file);
        return package.GetPartByUri(ownerUri).get();
    }

    /// Returns the relationship part URI that stores the relationships of the package.
    static std::string PackageRelationshipPartUri() { return "/_rels/.rels"; }

    /// Returns the relationship part URI that stores the relationships of a part.
    static std::string RelationshipPartUriFor(const OpenXmlPackagePart& part)
    {
        const auto& uri = part.Uri();
        const auto slash = uri.rfind('/');
        if (slash == std::string::npos)
        {
            return {};
        }
        return uri.substr(0, slash) + "/_rels/" + uri.substr(slash + 1) + ".rels";
    }
};

/// Splits a signature reference URI into the part it addresses and the content
/// type the signature expects that part to have.
class ReferenceTarget final
{
public:
    /// True when the reference points inside the signature itself (#idPackageObject).
    bool IsSameDocument = false;
    /// Identifier of the referenced Object, without the leading hash.
    std::string FragmentId;
    /// Normalized part URI, for references that address a part.
    std::string PartUri;
    /// Content type recorded in the query string, when present.
    std::string ContentType;

    static ReferenceTarget Parse(std::string_view uri)
    {
        ReferenceTarget target;
        if (!uri.empty() && uri.front() == '#')
        {
            target.IsSameDocument = true;
            target.FragmentId = std::string(uri.substr(1));
            return target;
        }

        auto path = uri;
        const auto query = uri.find('?');
        if (query != std::string_view::npos)
        {
            path = uri.substr(0, query);
            const auto parameters = uri.substr(query + 1);
            constexpr std::string_view kContentType = "ContentType=";
            const auto position = parameters.find(kContentType);
            if (position != std::string_view::npos)
            {
                auto value = parameters.substr(position + kContentType.size());
                const auto separator = value.find('&');
                if (separator != std::string_view::npos)
                {
                    value = value.substr(0, separator);
                }
                target.ContentType = std::string(value);
            }
        }

        target.PartUri = Detail::NormalizePartUri(path);
        return target;
    }
};

/// Shared machinery for verifying and for creating signatures: it turns a part,
/// an object, or a set of relationships into the exact bytes that get hashed.
class SignatureDigestSource final
{
public:
    SignatureDigestSource() = delete;

    /// Returns the bytes of a part according to the requested source.
    static bool GetPartBytes(const OpenXmlPackagePart& part,
                             PartByteSource source,
                             std::vector<Byte>& bytes,
                             std::string& message)
    {
        if (!part.IsXmlPart())
        {
            // A binary part is stored verbatim, so its buffer is the stored stream.
            bytes = part.GetBinaryData();
            return true;
        }

        if (source == PartByteSource::Original)
        {
            if (!part.HasOriginalBytes())
            {
                message = "The bytes this part was loaded with were not retained, so its digest cannot be "
                          "reproduced. Load the package with PartByteRetention::Always or "
                          "PartByteRetention::WhenSignaturesPresent.";
                return false;
            }
            bytes = part.GetOriginalBytes();
            return true;
        }

        const auto xml = part.GetXmlString();
        bytes.assign(xml.begin(), xml.end());
        return true;
    }

    /// Applies the transform chain of a relationship reference.
    static bool GetRelationshipBytes(const OpenXmlPackage& package,
                                     const std::string& relationshipPartUri,
                                     const std::vector<SignatureTransformInfo>& transforms,
                                     std::vector<Byte>& bytes,
                                     std::string& message)
    {
        const auto* container = SignatureLocator::ResolveRelationshipContainer(package, relationshipPartUri);
        if (!container)
        {
            message = "The signature covers relationships of a part that is not in the package.";
            return false;
        }

        RelationshipSelection selection;
        for (const auto& transform : transforms)
        {
            if (transform.Algorithm == SignatureNames::RelationshipTransform)
            {
                selection = RelationshipTransform::ReadSelection(transform.Node);
            }
        }

        const auto canonical = RelationshipTransform::Apply(*container, selection);
        if (!canonical)
        {
            message = "The relationships covered by the signature could not be canonicalized.";
            return false;
        }
        bytes.assign(canonical->begin(), canonical->end());
        return true;
    }

    /// Canonicalizes an element of the signature itself, such as an Object.
    static bool GetElementBytes(const Pugi::xml_node& node,
                                CanonicalizationMethod method,
                                std::vector<Byte>& bytes,
                                std::string& message)
    {
        const auto canonical = XmlCanonicalization::CanonicalizeSubtree(node, method);
        if (!canonical)
        {
            message = "The referenced element could not be canonicalized.";
            return false;
        }
        bytes.assign(canonical->begin(), canonical->end());
        return true;
    }
};

/// Checks which canonicalization or transform identifiers this library supports.
class SignatureAlgorithmSupport final
{
public:
    SignatureAlgorithmSupport() = delete;

    static bool IsSupportedCanonicalization(std::string_view algorithm) noexcept
    {
        return algorithm == SignatureNames::CanonicalizationC14N ||
               algorithm == SignatureNames::CanonicalizationC14NWithComments;
    }

    static CanonicalizationMethod MethodFor(std::string_view algorithm) noexcept
    {
        return algorithm == SignatureNames::CanonicalizationC14NWithComments
                   ? CanonicalizationMethod::InclusiveC14NWithComments
                   : CanonicalizationMethod::InclusiveC14N;
    }

    /// A transform chain is supported when it only canonicalizes or selects relationships.
    static bool IsSupportedTransformChain(const std::vector<SignatureTransformInfo>& transforms,
                                          CanonicalizationMethod& method,
                                          std::string& unsupported)
    {
        method = CanonicalizationMethod::InclusiveC14N;
        for (const auto& transform : transforms)
        {
            if (transform.Algorithm == SignatureNames::RelationshipTransform)
            {
                continue;
            }
            if (IsSupportedCanonicalization(transform.Algorithm))
            {
                method = MethodFor(transform.Algorithm);
                continue;
            }
            unsupported = transform.Algorithm;
            return false;
        }
        return true;
    }

    static bool UsesRelationshipTransform(const std::vector<SignatureTransformInfo>& transforms) noexcept
    {
        return std::any_of(transforms.begin(), transforms.end(), [](const auto& transform)
                           { return transform.Algorithm == SignatureNames::RelationshipTransform; });
    }
};

/// Verifies the signatures of one package.
class SignatureVerifier final
{
public:
    SignatureVerifier(const OpenXmlPackage& package,
                      const ICryptoProvider::Ptr& provider,
                      const VerifySignaturesOptions& options)
        : m_package(package), m_provider(provider), m_options(options)
    {
    }

    VerifySignaturesResult Run()
    {
        VerifySignaturesResult result;
        for (const auto& part : SignatureLocator::FindSignatureParts(m_package))
        {
            result.Signatures.push_back(VerifyPart(*part, result.Diagnostics));
            if (m_options.StopOnFirstFailure && !result.Signatures.back().IsValid())
            {
                break;
            }
        }
        return result;
    }

private:
    /// Loads the signature XML, preferring the bytes the part was stored with.
    static bool LoadSignatureDocument(const OpenXmlPackagePart& part, Pugi::xml_document& document)
    {
        if (part.HasOriginalBytes())
        {
            const auto bytes = part.GetOriginalBytes();
            return document.load_buffer(bytes.data(), bytes.size(), XmlCanonicalization::ParseOptions);
        }

        const auto xml = part.GetXmlString();
        return document.load_buffer(xml.data(), xml.size(), XmlCanonicalization::ParseOptions);
    }

    static void ReportIssue(ValidationResult& diagnostics,
                            ValidationSeverity severity,
                            ValidationErrorId id,
                            std::string message,
                            std::string partUri)
    {
        ValidationIssue issue;
        issue.Severity = severity;
        issue.Domain = ValidationDomain::Security;
        issue.Id = id;
        issue.Message = std::move(message);
        issue.PartUri = std::move(partUri);
        diagnostics.AddIssue(std::move(issue));
    }

    SignatureResult VerifyPart(const OpenXmlPackagePart& part, ValidationResult& diagnostics)
    {
        SignatureResult result;
        result.PartUri = part.Uri();

        Pugi::xml_document document;
        if (!LoadSignatureDocument(part, document))
        {
            result.ContentIntegrity = SignatureCheck::Invalid;
            result.SignatureValue = SignatureCheck::Invalid;
            ReportIssue(diagnostics,
                        ValidationSeverity::Error,
                        ValidationErrorId::SignatureMalformed,
                        "The signature part is not well formed XML.",
                        result.PartUri);
            return result;
        }

        ParsedSignature parsed;
        std::string error;
        if (!SignatureXml::Parse(document.document_element(), parsed, error))
        {
            result.ContentIntegrity = SignatureCheck::Invalid;
            result.SignatureValue = SignatureCheck::Invalid;
            ReportIssue(diagnostics, ValidationSeverity::Error, ValidationErrorId::SignatureMalformed, error,
                        result.PartUri);
            return result;
        }

        result.SignatureId = parsed.Id;
        result.Algorithm = ParseSignatureAlgorithmUri(parsed.SignatureAlgorithmUri);
        result.Certificates = parsed.Certificates;
        result.SigningTimeText = parsed.SigningTimeText;
        result.SigningTime = SignatureXml::ParseSigningTime(parsed.SigningTimeText);

        VerifyReferences(parsed, result, diagnostics);
        VerifySignatureValue(parsed, result, diagnostics);
        return result;
    }

    void VerifyReferences(const ParsedSignature& parsed, SignatureResult& result, ValidationResult& diagnostics)
    {
        bool anyInvalid = false;
        bool anyUnchecked = false;

        const auto process = [&](const SignatureReferenceInfo& reference)
        {
            auto entry = VerifyReference(parsed, reference, result, diagnostics);
            switch (entry.Digest)
            {
                case SignatureCheck::Invalid:
                    anyInvalid = true;
                    break;
                case SignatureCheck::NotChecked:
                    anyUnchecked = true;
                    break;
                case SignatureCheck::Valid:
                    break;
            }
            result.References.push_back(std::move(entry));
        };

        for (const auto& reference : parsed.ManifestReferences)
        {
            process(reference);
        }
        for (const auto& reference : parsed.SignedInfoReferences)
        {
            process(reference);
            if (!result.Digest.has_value())
            {
                result.Digest = ParseDigestAlgorithmUri(reference.DigestAlgorithmUri);
            }
        }

        if (anyInvalid)
        {
            result.ContentIntegrity = SignatureCheck::Invalid;
        }
        else if (anyUnchecked || result.References.empty())
        {
            result.ContentIntegrity = SignatureCheck::NotChecked;
        }
        else
        {
            result.ContentIntegrity = SignatureCheck::Valid;
        }
    }

    SignatureReferenceResult VerifyReference(const ParsedSignature& parsed,
                                             const SignatureReferenceInfo& reference,
                                             SignatureResult& result,
                                             ValidationResult& diagnostics)
    {
        SignatureReferenceResult entry;
        entry.Uri = reference.Uri;

        const auto digestAlgorithm = ParseDigestAlgorithmUri(reference.DigestAlgorithmUri);
        if (!digestAlgorithm)
        {
            entry.Message = "Unsupported digest algorithm " + reference.DigestAlgorithmUri + ".";
            ReportIssue(diagnostics,
                        ValidationSeverity::Warning,
                        ValidationErrorId::SignatureUnsupportedAlgorithm,
                        entry.Message,
                        result.PartUri);
            return entry;
        }

        CanonicalizationMethod method = CanonicalizationMethod::InclusiveC14N;
        std::string unsupported;
        if (!SignatureAlgorithmSupport::IsSupportedTransformChain(reference.Transforms, method, unsupported))
        {
            entry.Message = "Unsupported transform " + unsupported + ".";
            ReportIssue(diagnostics,
                        ValidationSeverity::Warning,
                        ValidationErrorId::SignatureUnsupportedAlgorithm,
                        entry.Message,
                        result.PartUri);
            return entry;
        }

        std::vector<Byte> data;
        if (!CollectReferenceData(parsed, reference, method, entry, data, result, diagnostics))
        {
            return entry;
        }

        const auto digest = ComputeDigestWithProvider(*digestAlgorithm, data);
        if (MessageDigest::DigestsEqual(digest, reference.DigestValue))
        {
            entry.Digest = SignatureCheck::Valid;
            return entry;
        }

        entry.Digest = SignatureCheck::Invalid;
        entry.Message = "The stored digest does not match the current content.";
        ReportIssue(diagnostics,
                    ValidationSeverity::Error,
                    ValidationErrorId::SignatureDigestMismatch,
                    "The signature no longer matches " + (entry.PartUri.empty() ? entry.Uri : entry.PartUri) + ".",
                    result.PartUri);
        return entry;
    }

    bool CollectReferenceData(const ParsedSignature& parsed,
                              const SignatureReferenceInfo& reference,
                              CanonicalizationMethod method,
                              SignatureReferenceResult& entry,
                              std::vector<Byte>& data,
                              SignatureResult& result,
                              ValidationResult& diagnostics)
    {
        const auto target = ReferenceTarget::Parse(reference.Uri);
        if (target.IsSameDocument)
        {
            const auto element = parsed.ElementsById.find(target.FragmentId);
            if (element == parsed.ElementsById.end())
            {
                entry.Digest = SignatureCheck::Invalid;
                entry.Message = "The signature references an element it does not contain.";
                ReportIssue(diagnostics,
                            ValidationSeverity::Error,
                            ValidationErrorId::SignatureMalformed,
                            entry.Message,
                            result.PartUri);
                return false;
            }
            return SignatureDigestSource::GetElementBytes(element->second, method, data, entry.Message);
        }

        entry.PartUri = target.PartUri;

        if (SignatureAlgorithmSupport::UsesRelationshipTransform(reference.Transforms) ||
            SignatureLocator::IsRelationshipPartUri(target.PartUri))
        {
            entry.IsRelationshipPart = true;
            if (!SignatureDigestSource::GetRelationshipBytes(m_package, target.PartUri, reference.Transforms, data,
                                                             entry.Message))
            {
                entry.Digest = SignatureCheck::Invalid;
                ReportIssue(diagnostics,
                            ValidationSeverity::Error,
                            ValidationErrorId::SignaturePartMissing,
                            entry.Message,
                            result.PartUri);
                return false;
            }
            return true;
        }

        const auto part = m_package.GetPartByUri(target.PartUri);
        if (!part)
        {
            entry.Digest = SignatureCheck::Invalid;
            entry.Message = "The signed part is no longer in the package.";
            ReportIssue(diagnostics,
                        ValidationSeverity::Error,
                        ValidationErrorId::SignaturePartMissing,
                        "The signature covers " + target.PartUri + ", which is not in the package.",
                        result.PartUri);
            return false;
        }

        if (!target.ContentType.empty() && target.ContentType != part->ContentType())
        {
            entry.Digest = SignatureCheck::Invalid;
            entry.Message = "The content type of the part changed since it was signed.";
            ReportIssue(diagnostics,
                        ValidationSeverity::Error,
                        ValidationErrorId::SignatureDigestMismatch,
                        entry.Message,
                        result.PartUri);
            return false;
        }

        if (!SignatureDigestSource::GetPartBytes(*part, m_options.ByteSource, data, entry.Message))
        {
            ReportIssue(diagnostics,
                        ValidationSeverity::Warning,
                        ValidationErrorId::SignatureNotVerified,
                        entry.Message,
                        result.PartUri);
            return false;
        }

        // A content part is hashed as stored, but a signature may still ask for
        // canonicalization; honor the transform when it is there.
        if (!reference.Transforms.empty() && part->IsXmlPart())
        {
            Pugi::xml_document document;
            std::optional<std::string> canonical;
            if (document.load_buffer(data.data(), data.size(), XmlCanonicalization::ParseOptions))
            {
                canonical = XmlCanonicalization::CanonicalizeSubtree(document.document_element(), method);
            }

            // A transform that could not be applied leaves nothing honest to
            // digest. Hashing the stored bytes would compare the wrong thing,
            // and hashing an empty result would let a signature computed over
            // an empty input pass; both report a broken part as intact.
            if (!canonical)
            {
                entry.Digest = SignatureCheck::Invalid;
                entry.Message = "The signed part could not be canonicalized.";
                ReportIssue(diagnostics,
                            ValidationSeverity::Error,
                            ValidationErrorId::SignatureDigestMismatch,
                            entry.Message,
                            result.PartUri);
                return false;
            }

            data.assign(canonical->begin(), canonical->end());
        }

        return true;
    }

    void VerifySignatureValue(const ParsedSignature& parsed, SignatureResult& result, ValidationResult& diagnostics)
    {
        if (!SignatureAlgorithmSupport::IsSupportedCanonicalization(parsed.CanonicalizationAlgorithmUri))
        {
            result.SignatureValue = SignatureCheck::NotChecked;
            result.Warnings.push_back("Unsupported canonicalization " + parsed.CanonicalizationAlgorithmUri + ".");
            ReportIssue(diagnostics,
                        ValidationSeverity::Warning,
                        ValidationErrorId::SignatureUnsupportedAlgorithm,
                        result.Warnings.back(),
                        result.PartUri);
            return;
        }

        if (!m_provider)
        {
            result.SignatureValue = SignatureCheck::NotChecked;
            ReportIssue(diagnostics,
                        ValidationSeverity::Warning,
                        ValidationErrorId::SignatureNotVerified,
                        "The signature value was not checked because no crypto provider was supplied.",
                        result.PartUri);
            return;
        }

        if (!result.Algorithm)
        {
            result.SignatureValue = SignatureCheck::NotChecked;
            result.Warnings.push_back("Unsupported signature algorithm " + parsed.SignatureAlgorithmUri + ".");
            ReportIssue(diagnostics,
                        ValidationSeverity::Warning,
                        ValidationErrorId::SignatureUnsupportedAlgorithm,
                        result.Warnings.back(),
                        result.PartUri);
            return;
        }

        if (result.Certificates.empty())
        {
            result.SignatureValue = SignatureCheck::Invalid;
            ReportIssue(diagnostics,
                        ValidationSeverity::Error,
                        ValidationErrorId::SignatureMalformed,
                        "The signature carries no certificate, so its value cannot be checked.",
                        result.PartUri);
            return;
        }

        const auto canonical = XmlCanonicalization::CanonicalizeSubtree(
            parsed.SignedInfoNode, SignatureAlgorithmSupport::MethodFor(parsed.CanonicalizationAlgorithmUri));
        if (!canonical)
        {
            // Handing empty bytes to the provider would let a signature made
            // over an empty SignedInfo verify, which is exactly the outcome a
            // caller who nested the element past MaximumDepth would be after.
            result.SignatureValue = SignatureCheck::Invalid;
            ReportIssue(diagnostics,
                        ValidationSeverity::Error,
                        ValidationErrorId::SignatureMalformed,
                        "The SignedInfo element could not be canonicalized, so its value cannot be checked.",
                        result.PartUri);
            return;
        }
        const std::vector<Byte> signedInfo(canonical->begin(), canonical->end());

        const bool valid =
            m_provider->VerifyData(*result.Algorithm, signedInfo, parsed.SignatureValue, result.Certificates.front());
        result.SignatureValue = valid ? SignatureCheck::Valid : SignatureCheck::Invalid;
        if (!valid)
        {
            ReportIssue(diagnostics,
                        ValidationSeverity::Error,
                        ValidationErrorId::SignatureValueInvalid,
                        "The signature value did not verify against the certificate in the signature.",
                        result.PartUri);
        }
    }

    std::vector<Byte> ComputeDigestWithProvider(DigestAlgorithm algorithm,
                                                const std::vector<Byte>& data) const
    {
        if (m_provider)
        {
            std::vector<Byte> digest;
            if (m_provider->ComputeDigest(algorithm, data, digest))
            {
                return digest;
            }
        }
        return ComputeDigest(algorithm, data);
    }

    const OpenXmlPackage& m_package;
    ICryptoProvider::Ptr m_provider;
    VerifySignaturesOptions m_options;
};

bool VerifySignaturesResult::AllValid() const noexcept
{
    return !Signatures.empty() &&
           std::all_of(Signatures.begin(), Signatures.end(), [](const auto& signature)
                       { return signature.IsValid(); });
}

bool VerifySignaturesResult::AllContentIntact() const noexcept
{
    return !Signatures.empty() && std::all_of(Signatures.begin(), Signatures.end(), [](const auto& signature)
                                              { return signature.ContentIntegrity == SignatureCheck::Valid; });
}

VerifySignaturesResult VerifySignatures(const OpenXmlPackage& package,
                                        const ICryptoProvider::Ptr& provider,
                                        const VerifySignaturesOptions& options)
{
    SignatureVerifier verifier(package, provider, options);
    return verifier.Run();
}

VerifySignaturesResult CheckSignaturesBeforeSave(const OpenXmlPackage& package)
{
    VerifySignaturesOptions options;
    options.ByteSource = PartByteSource::Current;
    return VerifySignatures(package, nullptr, options);
}

bool HasSignatures(const OpenXmlPackage& package)
{
    return !SignatureLocator::FindSignatureParts(package).empty();
}

bool RemoveAllSignatures(OpenXmlPackage& package)
{
    const auto origin = SignatureLocator::FindOriginPart(package);
    if (!origin)
    {
        return false;
    }

    // Removing the only edge to the origin part drops it together with the
    // signature parts that hang off it.
    return package.RemovePartReference(origin);
}

/// Builds the XML signature and attaches the parts that carry it.
class SignatureWriter final
{
public:
    SignatureWriter(OpenXmlPackage& package, ICryptoProvider::Ptr provider, SignPackageOptions options)
        : m_package(package), m_provider(std::move(provider)), m_options(std::move(options))
    {
    }

    SignPackageResult Run()
    {
        if (!m_provider)
        {
            return Fail(SignatureError::NoProvider, "No crypto provider was supplied.");
        }

        const auto certificates = m_provider->GetCertificateChain();
        if (certificates.empty())
        {
            return Fail(SignatureError::NoCertificate, "The crypto provider returned no certificate.");
        }

        const auto algorithm = m_options.Signature.value_or(m_provider->GetSignatureAlgorithm());
        if (GetSignatureAlgorithmUri(algorithm).empty() || GetDigestAlgorithmUri(m_options.Digest).empty())
        {
            return Fail(SignatureError::UnsupportedAlgorithm, "The requested algorithm is not supported.");
        }

        std::vector<std::shared_ptr<OpenXmlPackagePart>> parts;
        if (auto failure = CollectPartsToSign(parts); !failure.Succeeded())
        {
            return failure;
        }

        Pugi::xml_document document;
        auto signature = CreateSignatureSkeleton(document);
        auto packageObject = BuildPackageObject(signature, parts);
        if (!packageObject)
        {
            return Fail(SignatureError::CanonicalizationFailed, "A signed part could not be read.");
        }
        BuildOfficeObject(signature);

        if (auto failure = BuildSignedInfo(signature, algorithm); !failure.Succeeded())
        {
            return failure;
        }

        std::vector<Byte> signatureValue;
        const auto canonicalSignedInfo = XmlCanonicalization::CanonicalizeSubtree(
            SignatureXml::FindChild(signature, SignatureNames::DsigNamespace, "SignedInfo"));
        if (!canonicalSignedInfo)
        {
            return Fail(SignatureError::CanonicalizationFailed, "The SignedInfo element could not be canonicalized.");
        }
        const std::vector<Byte> signedInfoBytes(canonicalSignedInfo->begin(), canonicalSignedInfo->end());
        if (!m_provider->SignData(algorithm, signedInfoBytes, signatureValue) || signatureValue.empty())
        {
            return Fail(SignatureError::ProviderFailed, "The crypto provider did not produce a signature value.");
        }

        AppendTextElement(signature, "SignatureValue", SignatureXml::EncodeBase64(signatureValue));
        MoveBefore(signature, "SignatureValue", "Object");
        BuildKeyInfo(signature, certificates);
        MoveBefore(signature, "KeyInfo", "Object");

        return AttachSignatureParts(document);
    }

private:
    static SignPackageResult Fail(SignatureError error, std::string message)
    {
        SignPackageResult result;
        result.Error = error;
        result.Message = std::move(message);
        return result;
    }

    /// Signature parts never sign themselves.
    static bool IsSignatureRelatedPart(const OpenXmlPackagePart& part)
    {
        return part.ContentType() == SignatureNames::OriginContentType ||
               part.ContentType() == SignatureNames::SignatureContentType ||
               part.ContentType() == SignatureNames::CertificateContentType;
    }

    SignPackageResult CollectPartsToSign(std::vector<std::shared_ptr<OpenXmlPackagePart>>& parts)
    {
        if (!m_options.PartUris.empty())
        {
            for (const auto& uri : m_options.PartUris)
            {
                auto part = m_package.GetPartByUri(uri);
                if (!part)
                {
                    return Fail(SignatureError::PartMissing, "The package has no part " + uri + ".");
                }
                parts.push_back(std::move(part));
            }
            return {};
        }

        CollectPartsRecursively(m_package, parts);
        return {};
    }

    static void CollectPartsRecursively(const OpenXmlPartContainer& container,
                                        std::vector<std::shared_ptr<OpenXmlPackagePart>>& parts)
    {
        for (const auto& part : container.Parts())
        {
            if (!part || IsSignatureRelatedPart(*part))
            {
                continue;
            }
            const auto known = std::any_of(parts.begin(), parts.end(), [&part](const auto& existing)
                                           { return existing->Uri() == part->Uri(); });
            if (known)
            {
                continue;
            }
            parts.push_back(part);
            CollectPartsRecursively(*part, parts);
        }
    }

    Pugi::xml_node CreateSignatureSkeleton(Pugi::xml_document& document)
    {
        auto signature = document.append_child("Signature");
        signature.append_attribute("xmlns") = std::string(SignatureNames::DsigNamespace).c_str();
        signature.append_attribute("Id") = m_options.SignatureId.c_str();
        return signature;
    }

    /// Appends one Reference element and fills in its digest.
    void AppendReference(Pugi::xml_node& parent,
                         const std::string& uri,
                         const std::vector<Byte>& data,
                         const std::string& referenceType)
    {
        auto reference = parent.append_child("Reference");
        reference.append_attribute("URI") = uri.c_str();
        if (!referenceType.empty())
        {
            reference.append_attribute("Type") = referenceType.c_str();
        }
        AppendDigest(reference, data);
    }

    void AppendDigest(Pugi::xml_node& reference, const std::vector<Byte>& data)
    {
        auto method = reference.append_child("DigestMethod");
        method.append_attribute("Algorithm") = std::string(GetDigestAlgorithmUri(m_options.Digest)).c_str();

        std::vector<Byte> digest;
        if (!m_provider || !m_provider->ComputeDigest(m_options.Digest, data, digest))
        {
            digest = ComputeDigest(m_options.Digest, data);
        }
        AppendTextElement(reference, "DigestValue", SignatureXml::EncodeBase64(digest));
    }

    static Pugi::xml_node AppendTextElement(Pugi::xml_node& parent, const char* name, const std::string& text)
    {
        auto node = parent.append_child(name);
        node.append_child(Pugi::node_pcdata).set_value(text.c_str());
        return node;
    }

    /// Keeps the element order the signature schema prescribes.
    static void MoveBefore(Pugi::xml_node& parent, const char* name, const char* successor)
    {
        auto node = parent.child(name);
        auto target = parent.child(successor);
        if (node && target)
        {
            parent.insert_move_before(node, target);
        }
    }

    Pugi::xml_node BuildPackageObject(Pugi::xml_node& signature,
                                      const std::vector<std::shared_ptr<OpenXmlPackagePart>>& parts)
    {
        auto object = signature.append_child("Object");
        object.append_attribute("Id") = std::string(SignatureNames::PackageObjectId).c_str();

        auto manifest = object.append_child("Manifest");
        for (const auto& part : parts)
        {
            std::vector<Byte> data;
            std::string message;
            if (!SignatureDigestSource::GetPartBytes(*part, PartByteSource::Current, data, message))
            {
                return {};
            }

            std::string uri = part->Uri();
            uri += "?ContentType=";
            uri += part->ContentType();
            AppendReference(manifest, uri, data, {});
        }

        if (m_options.SignRelationships)
        {
            if (!AppendRelationshipReference(manifest, m_package, SignatureLocator::PackageRelationshipPartUri()))
            {
                return {};
            }
            for (const auto& part : parts)
            {
                if (!AppendRelationshipReference(manifest, *part, SignatureLocator::RelationshipPartUriFor(*part)))
                {
                    return {};
                }
            }
        }

        AppendSignatureTime(object);
        return object;
    }

    /// Adds a reference that covers the relationships of one container.
    /// @return False when the transform output could not be canonicalized,
    ///         so that signing fails rather than writing a digest over nothing.
    bool AppendRelationshipReference(Pugi::xml_node& manifest,
                                     const OpenXmlPartContainer& container,
                                     const std::string& relationshipPartUri)
    {
        if (container.Relationships().empty() || relationshipPartUri.empty())
        {
            return true;
        }

        auto reference = manifest.append_child("Reference");
        std::string uri = relationshipPartUri;
        uri += "?ContentType=";
        uri += SignatureNames::RelationshipsContentType;
        reference.append_attribute("URI") = uri.c_str();

        auto transforms = reference.append_child("Transforms");
        auto transform = transforms.append_child("Transform");
        transform.append_attribute("Algorithm") = std::string(SignatureNames::RelationshipTransform).c_str();
        transform.append_attribute("xmlns:mdssi") =
            std::string(SignatureNames::PackageDigitalSignatureNamespace).c_str();

        RelationshipSelection selection;
        for (const auto& relationship : container.Relationships())
        {
            auto selector = transform.append_child("mdssi:RelationshipReference");
            selector.append_attribute("SourceId") = relationship.Id.c_str();
            selection.SourceIds.push_back(relationship.Id);
        }

        auto canonicalization = transforms.append_child("Transform");
        canonicalization.append_attribute("Algorithm") = std::string(SignatureNames::CanonicalizationC14N).c_str();

        const auto canonical = RelationshipTransform::Apply(container, selection);
        if (!canonical)
        {
            return false;
        }
        const std::vector<Byte> data(canonical->begin(), canonical->end());
        AppendDigest(reference, data);
        return true;
    }

    void AppendSignatureTime(Pugi::xml_node& object)
    {
        auto properties = object.append_child("SignatureProperties");
        auto property = properties.append_child("SignatureProperty");
        property.append_attribute("Id") = std::string(SignatureNames::SignatureTimePropertyId).c_str();
        property.append_attribute("Target") = ("#" + m_options.SignatureId).c_str();

        auto time = property.append_child("mdssi:SignatureTime");
        time.append_attribute("xmlns:mdssi") =
            std::string(SignatureNames::PackageDigitalSignatureNamespace).c_str();
        AppendTextElement(time, "mdssi:Format", std::string(SignatureNames::SignatureTimeFormat));
        AppendTextElement(time,
                          "mdssi:Value",
                          SignatureXml::FormatSigningTime(
                              m_options.SigningTime.value_or(std::chrono::system_clock::now())));
    }

    void BuildOfficeObject(Pugi::xml_node& signature)
    {
        auto object = signature.append_child("Object");
        object.append_attribute("Id") = std::string(SignatureNames::OfficeObjectId).c_str();

        auto properties = object.append_child("SignatureProperties");
        auto property = properties.append_child("SignatureProperty");
        property.append_attribute("Id") = std::string(SignatureNames::OfficeDetailsPropertyId).c_str();
        property.append_attribute("Target") = ("#" + m_options.SignatureId).c_str();

        auto details = property.append_child("SignatureInfoV1");
        details.append_attribute("xmlns") = std::string(SignatureNames::OfficeDigitalSignatureNamespace).c_str();
        if (!m_options.Office.Comments.empty())
        {
            AppendTextElement(details, "SignatureComments", m_options.Office.Comments);
        }
        if (!m_options.Office.SignatureProviderId.empty())
        {
            AppendTextElement(details, "SignatureProviderId", m_options.Office.SignatureProviderId);
        }
        if (!m_options.Office.ApplicationVersion.empty())
        {
            AppendTextElement(details, "ApplicationVersion", m_options.Office.ApplicationVersion);
        }
    }

    SignPackageResult BuildSignedInfo(Pugi::xml_node& signature, SignatureAlgorithm algorithm)
    {
        auto signedInfo = signature.append_child("SignedInfo");
        MoveBefore(signature, "SignedInfo", "Object");

        auto canonicalization = signedInfo.append_child("CanonicalizationMethod");
        canonicalization.append_attribute("Algorithm") = std::string(SignatureNames::CanonicalizationC14N).c_str();

        auto method = signedInfo.append_child("SignatureMethod");
        method.append_attribute("Algorithm") = std::string(GetSignatureAlgorithmUri(algorithm)).c_str();

        for (const auto& object : signature.children("Object"))
        {
            const std::string id = object.attribute("Id").as_string();
            if (id.empty())
            {
                continue;
            }

            std::vector<Byte> data;
            std::string message;
            if (!SignatureDigestSource::GetElementBytes(object, CanonicalizationMethod::InclusiveC14N, data, message))
            {
                return Fail(SignatureError::CanonicalizationFailed, message);
            }

            auto reference = signedInfo.append_child("Reference");
            reference.append_attribute("URI") = ("#" + id).c_str();
            reference.append_attribute("Type") = std::string(SignatureNames::ObjectReferenceType).c_str();

            auto transforms = reference.append_child("Transforms");
            auto transform = transforms.append_child("Transform");
            transform.append_attribute("Algorithm") = std::string(SignatureNames::CanonicalizationC14N).c_str();

            AppendDigest(reference, data);
        }

        return {};
    }

    static void BuildKeyInfo(Pugi::xml_node& signature, const std::vector<std::vector<Byte>>& certificates)
    {
        auto keyInfo = signature.append_child("KeyInfo");
        auto data = keyInfo.append_child("X509Data");
        for (const auto& certificate : certificates)
        {
            AppendTextElement(data, "X509Certificate", SignatureXml::EncodeBase64(certificate));
        }
    }

    SignPackageResult AttachSignatureParts(const Pugi::xml_document& document)
    {
        auto origin = SignatureLocator::FindOriginPart(m_package);
        if (!origin)
        {
            auto created = std::make_shared<Packaging::DigitalSignatureOriginPart>();
            if (!m_package.AttachCustomPart(created, Packaging::DigitalSignatureOriginPart::Descriptor(), false))
            {
                return Fail(SignatureError::PackageWriteFailed, "The signature origin part could not be created.");
            }
            origin = created;
        }

        auto signaturePart = std::make_shared<Packaging::XmlSignaturePart>();
        if (!origin->AttachCustomPart(signaturePart, Packaging::XmlSignaturePart::Descriptor(), true))
        {
            return Fail(SignatureError::PackageWriteFailed, "The signature part could not be created.");
        }

        std::ostringstream stream;
        document.save(stream, "", Pugi::format_raw);
        signaturePart->SetXmlString(stream.str());

        SignPackageResult result;
        result.SignaturePartUri = signaturePart->Uri();
        return result;
    }

    OpenXmlPackage& m_package;
    ICryptoProvider::Ptr m_provider;
    SignPackageOptions m_options;
};

SignPackageResult SignPackage(OpenXmlPackage& package,
                              const ICryptoProvider::Ptr& provider,
                              const SignPackageOptions& options)
{
    SignatureWriter writer(package, provider, options);
    auto result = writer.Run();
    if (result.Succeeded())
    {
        // Saving normally refreshes `dcterms:modified`, which would rewrite a
        // part the digests were just computed over and leave the file carrying
        // a signature that cannot verify. Hold that update back for the save
        // that writes this signature out; later saves resume updating.
        package.SuppressSaveTimePropertyUpdateOnce();
    }
    return result;
}

} // namespace ExyokiOffice::Security
