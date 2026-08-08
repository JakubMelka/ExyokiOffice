// Copyright (c) 2026 Jakub Melka and Collaborators
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#pragma once

#include "ExyokiOffice/Export.hpp"
#include "ExyokiOffice/OpenXmlPackage.hpp"
#include "ExyokiOffice/Security/CryptoProvider.hpp"
#include "ExyokiOffice/ValidationResult.hpp"
#include "ExyokiOffice/StandardTypes.hpp"

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace ExyokiOffice::Security
{

/**
 * @brief Outcome of one check performed while verifying a signature.
 *
 * NotChecked is not a failure: it means the check could not be performed, most
 * often because no crypto provider was supplied for the signature value.
 */
enum class SignatureCheck
{
    /** @brief The check was not performed. */
    NotChecked,
    /** @brief The check succeeded. */
    Valid,
    /** @brief The check failed. */
    Invalid
};

/**
 * @brief Which bytes of a part the digests are computed over.
 *
 * A signature covers the byte stream a part had when it was stored. Saving
 * re-serializes XML parts, so the two sources answer two different questions.
 */
enum class PartByteSource
{
    /** @brief The bytes read from the package file; use this to verify a signature. */
    Original,
    /** @brief The bytes a save would write now; use this to see whether saving breaks a signature. */
    Current
};

/** @brief Result for a single reference inside a signature. */
struct EXYOKIOFFICE_EXPORT SignatureReferenceResult
{
    /** @brief The URI attribute as written in the signature, query string included. */
    std::string Uri;
    /** @brief The package part the reference resolves to, when it resolves at all. */
    std::string PartUri;
    /** @brief True when the reference covers a relationship part through the relationship transform. */
    bool IsRelationshipPart = false;
    /** @brief Whether the stored digest matches the data the reference covers. */
    SignatureCheck Digest = SignatureCheck::NotChecked;
    /** @brief Explanation for a failed or skipped check; empty when the digest matched. */
    std::string Message;
};

/** @brief Everything one signature part says about itself, plus the verification outcome. */
struct EXYOKIOFFICE_EXPORT SignatureResult
{
    /** @brief URI of the signature part, for example /_xmlsignatures/sig1.xml. */
    std::string PartUri;
    /** @brief Value of the Id attribute of the Signature element. */
    std::string SignatureId;
    /** @brief Signature algorithm, empty when the signature names one this library does not know. */
    std::optional<SignatureAlgorithm> Algorithm;
    /** @brief Digest algorithm of the SignedInfo references. */
    std::optional<DigestAlgorithm> Digest;
    /** @brief Certificates carried by the signature, in DER encoding and left unparsed. */
    std::vector<std::vector<Byte>> Certificates;
    /** @brief Signing time claimed by the signature; it is not evidence of anything by itself. */
    std::optional<std::chrono::system_clock::time_point> SigningTime;
    /** @brief Signing time exactly as written, for signatures using an unexpected format. */
    std::string SigningTimeText;
    /** @brief Whether every covered part and object still hashes to the stored digest. */
    SignatureCheck ContentIntegrity = SignatureCheck::NotChecked;
    /** @brief Whether the signature value verifies; requires a crypto provider. */
    SignatureCheck SignatureValue = SignatureCheck::NotChecked;
    /** @brief One entry per manifest and per SignedInfo reference. */
    std::vector<SignatureReferenceResult> References;
    /** @brief Problems that did not by themselves make the signature invalid. */
    std::vector<std::string> Warnings;

    /** @brief True when both the content and the signature value were checked and are valid. */
    [[nodiscard]] bool IsValid() const noexcept
    {
        return ContentIntegrity == SignatureCheck::Valid && SignatureValue == SignatureCheck::Valid;
    }
};

/** @brief Options for VerifySignatures. */
struct EXYOKIOFFICE_EXPORT VerifySignaturesOptions
{
    /** @brief Which bytes to hash; Original answers "is this package still the signed one". */
    PartByteSource ByteSource = PartByteSource::Original;
    /** @brief Stop after the first signature that turns out to be invalid. */
    bool StopOnFirstFailure = false;
};

/** @brief Result of verifying every signature in a package. */
struct EXYOKIOFFICE_EXPORT VerifySignaturesResult
{
    /** @brief One entry per signature part, in the order the origin part lists them. */
    std::vector<SignatureResult> Signatures;
    /** @brief Diagnostics describing why something could not be verified. */
    ValidationResult Diagnostics;

    /** @brief True when the package carries at least one signature. */
    [[nodiscard]] bool HasSignatures() const noexcept { return !Signatures.empty(); }

    /** @brief True when there is at least one signature and every one of them is fully valid. */
    [[nodiscard]] bool AllValid() const noexcept;

    /** @brief True when every signature still covers unmodified content, ignoring signature values. */
    [[nodiscard]] bool AllContentIntact() const noexcept;
};

/**
 * @brief Verifies the digital signatures stored in a package.
 *
 * Verification has two independent halves. The first recomputes every digest in
 * the signature and answers whether the signed content has changed; it needs no
 * cryptography and therefore works without a provider. The second checks the
 * signature value itself, which proves who signed it, and requires a provider.
 * Without one, SignatureValue stays NotChecked and the signature is reported as
 * not valid, because "unchanged" is not the same as "signed by someone".
 *
 * Verifying a signature written by another application needs the bytes the parts
 * had in the file. Loading keeps them for packages that carry signatures (see
 * PartByteRetention); with retention turned off, references over XML parts are
 * reported as not checked.
 *
 * @param package Package to inspect; it is not modified.
 * @param provider Optional crypto provider used for the signature value.
 * @param options Byte source and early-exit behavior.
 * @return One result per signature part plus diagnostics.
 *
 * @code
 * auto result = ExyokiOffice::Security::VerifySignatures(*document, provider);
 * for (const auto& signature : result.Signatures)
 * {
 *     std::cout << signature.PartUri << ": " << (signature.IsValid() ? "valid" : "not valid") << '\n';
 * }
 * @endcode
 */
[[nodiscard]] EXYOKIOFFICE_EXPORT VerifySignaturesResult VerifySignatures(
    const OpenXmlPackage& package,
    const ICryptoProvider::Ptr& provider = nullptr,
    const VerifySignaturesOptions& options = {});

/**
 * @brief Reports whether saving now would invalidate the signatures in the package.
 *
 * Recomputes the covered digests against the bytes a save would write. Use it
 * before SaveToFile to tell the user what is about to happen; the signature
 * values are not checked, because only the content matters for this question.
 *
 * @param package Package to inspect; it is not modified.
 * @return An empty result when the package carries no signatures.
 */
[[nodiscard]] EXYOKIOFFICE_EXPORT VerifySignaturesResult CheckSignaturesBeforeSave(const OpenXmlPackage& package);

/** @brief Returns true when the package contains at least one signature part. */
[[nodiscard]] EXYOKIOFFICE_EXPORT bool HasSignatures(const OpenXmlPackage& package);

/**
 * @brief Removes the signature origin part, all signature parts, and their relationships.
 *
 * @return true when something was removed.
 */
EXYOKIOFFICE_EXPORT bool RemoveAllSignatures(OpenXmlPackage& package);

/** @brief Optional Office specific details stored next to the signature. */
struct EXYOKIOFFICE_EXPORT SignatureInfo
{
    /** @brief Free text the signer wrote about the purpose of signing. */
    std::string Comments;
    /** @brief Identifier of the signature provider, written as-is. */
    std::string SignatureProviderId;
    /** @brief Version of the signing application. */
    std::string ApplicationVersion;
};

/** @brief Why signing failed. */
enum class SignatureError
{
    /** @brief Signing succeeded. */
    None,
    /** @brief No crypto provider was supplied. */
    NoProvider,
    /** @brief The provider returned no certificate, so the signature would have no key information. */
    NoCertificate,
    /** @brief The requested digest or signature algorithm is not supported. */
    UnsupportedAlgorithm,
    /** @brief A part listed in the options is not in the package. */
    PartMissing,
    /** @brief A part could not be canonicalized or read. */
    CanonicalizationFailed,
    /** @brief The provider failed to produce a signature value. */
    ProviderFailed,
    /** @brief The signature parts could not be created inside the package. */
    PackageWriteFailed
};

/** @brief Options for SignPackage. */
struct EXYOKIOFFICE_EXPORT SignPackageOptions
{
    /** @brief Digest algorithm used for every reference. */
    DigestAlgorithm Digest = DigestAlgorithm::Sha256;
    /** @brief Signature algorithm; defaults to what the provider reports. */
    std::optional<SignatureAlgorithm> Signature;
    /** @brief Value of the Id attribute written on the Signature element. */
    std::string SignatureId = "idPackageSignature";
    /** @brief Signing time; defaults to the current UTC time. */
    std::optional<std::chrono::system_clock::time_point> SigningTime;
    /** @brief Parts to sign; empty means every part except the signature parts themselves. */
    std::vector<std::string> PartUris;
    /** @brief Also cover the relationship parts, so relationships cannot be retargeted unnoticed. */
    bool SignRelationships = true;
    /** @brief Office specific details; every field is optional. */
    SignatureInfo Office;
};

/** @brief Outcome of SignPackage. */
struct EXYOKIOFFICE_EXPORT SignPackageResult
{
    /** @brief None on success. */
    SignatureError Error = SignatureError::None;
    /** @brief Human readable explanation of a failure. */
    std::string Message;
    /** @brief URI of the signature part that was created. */
    std::string SignaturePartUri;

    [[nodiscard]] bool Succeeded() const noexcept { return Error == SignatureError::None; }
    [[nodiscard]] explicit operator bool() const noexcept { return Succeeded(); }
};

/**
 * @brief Signs the package with a key held by the caller's crypto provider.
 *
 * The signature covers the parts as they would be written right now, so sign
 * immediately before saving and do not touch the document in between. Any later
 * edit invalidates the signature.
 *
 * Saving normally refreshes `dcterms:modified` in `docProps/core.xml`, which
 * would rewrite a part the digests were just taken over. Signing therefore arms
 * OpenXmlPackage::SuppressSaveTimePropertyUpdateOnce(), so the **next** save
 * writes the package byte for byte as it was signed. A save after that updates
 * the properties again, on the assumption the document has since been edited;
 * that does invalidate the signature, and the package's SignatureSavePolicy
 * decides how it is reported. Sign again before saving a modified document.
 *
 * The library builds the whole XML signature, computes every digest, and asks
 * the provider only to sign the canonicalized SignedInfo and to hand over the
 * certificate chain.
 *
 * @param package Package to sign; signature parts are added to it.
 * @param provider Crypto provider holding the signing key and certificate.
 * @param options Algorithms, coverage, and the optional Office details.
 * @return Success, or the reason signing failed.
 *
 * @code
 * ExyokiOffice::Security::SignPackageOptions options;
 * options.Office.Comments = "Approved";
 * if (ExyokiOffice::Security::SignPackage(*document, provider, options))
 * {
 *     document->SaveToFile("signed.docx");
 * }
 * @endcode
 */
EXYOKIOFFICE_EXPORT SignPackageResult SignPackage(OpenXmlPackage& package,
                                                  const ICryptoProvider::Ptr& provider,
                                                  const SignPackageOptions& options = {});

} // namespace ExyokiOffice::Security
