// Copyright (c) 2026 Jakub Melka and Contributors
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#pragma once

#include "ExyokiOffice/Export.hpp"
#include "ExyokiOffice/StandardTypes.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace ExyokiOffice::Security
{

/**
 * @brief Digest (hash) algorithms understood by the package signature layer.
 *
 * These are the algorithms an XML digital signature may name in DigestMethod.
 * The library computes them itself, so a digest never requires a crypto provider.
 * Sha1 is only accepted when reading signatures produced by older applications.
 * New signatures should use Sha256 or stronger.
 */
enum class DigestAlgorithm
{
    /** @brief SHA-1; legacy, accepted for verification of existing signatures. */
    Sha1,
    /** @brief SHA-256; the default for signatures created by this library. */
    Sha256,
    /** @brief SHA-384. */
    Sha384,
    /** @brief SHA-512. */
    Sha512
};

/**
 * @brief Asymmetric signature algorithms the signature layer can name.
 *
 * The value selects both the public key algorithm and the digest used inside
 * the signature. The library never implements these; it only writes and reads
 * the matching algorithm identifier and asks a crypto provider to do the work.
 */
enum class SignatureAlgorithm
{
    /** @brief RSASSA-PKCS1-v1_5 with SHA-1; legacy. */
    RsaSha1,
    /** @brief RSASSA-PKCS1-v1_5 with SHA-256. */
    RsaSha256,
    /** @brief RSASSA-PKCS1-v1_5 with SHA-384. */
    RsaSha384,
    /** @brief RSASSA-PKCS1-v1_5 with SHA-512. */
    RsaSha512,
    /** @brief ECDSA with SHA-256. */
    EcdsaSha256,
    /** @brief ECDSA with SHA-384. */
    EcdsaSha384,
    /** @brief ECDSA with SHA-512. */
    EcdsaSha512
};

/** @brief Returns the XML algorithm identifier written for a digest algorithm. */
[[nodiscard]] EXYOKIOFFICE_EXPORT std::string_view GetDigestAlgorithmUri(DigestAlgorithm algorithm) noexcept;

/** @brief Maps an XML DigestMethod identifier back to an algorithm, if supported. */
[[nodiscard]] EXYOKIOFFICE_EXPORT std::optional<DigestAlgorithm> ParseDigestAlgorithmUri(std::string_view uri) noexcept;

/** @brief Returns the XML algorithm identifier written for a signature algorithm. */
[[nodiscard]] EXYOKIOFFICE_EXPORT std::string_view GetSignatureAlgorithmUri(SignatureAlgorithm algorithm) noexcept;

/** @brief Maps an XML SignatureMethod identifier back to an algorithm, if supported. */
[[nodiscard]] EXYOKIOFFICE_EXPORT std::optional<SignatureAlgorithm> ParseSignatureAlgorithmUri(std::string_view uri) noexcept;

/** @brief Returns the digest algorithm embedded in a signature algorithm. */
[[nodiscard]] EXYOKIOFFICE_EXPORT DigestAlgorithm GetDigestAlgorithm(SignatureAlgorithm algorithm) noexcept;

/**
 * @brief Computes a digest with the library's built-in SHA implementation.
 *
 * No crypto provider is needed for this; the hash functions are part of the
 * library. This is what makes it possible to check whether a signed document
 * has been modified even when no provider is available.
 *
 * @param algorithm Digest algorithm to use.
 * @param data Bytes to hash.
 * @return The raw digest bytes.
 */
[[nodiscard]] EXYOKIOFFICE_EXPORT std::vector<Byte> ComputeDigest(DigestAlgorithm algorithm,
                                                                  std::span<const Byte> data);

/**
 * @brief Asymmetric cryptography supplied by the application.
 *
 * ExyokiOffice deliberately links no cryptographic library. It implements the
 * OPC and XML side of digital signatures (canonicalization, manifests, digests)
 * and delegates everything that needs a private key or an X.509 certificate to
 * an implementation of this interface that you write and pass in as a
 * std::shared_ptr. A provider can sit on top of Windows CNG, OpenSSL, a smart
 * card, a hardware security module, or a remote signing service.
 *
 * Certificates are exchanged as raw DER bytes; the library never parses them.
 *
 * Implementations must be safe to call concurrently from several threads for
 * verification. The library keeps the shared pointer alive for the whole
 * operation, so a provider may outlive the call that used it.
 *
 * @code
 * class MyProvider final : public ExyokiOffice::Security::ICryptoProvider
 * {
 * public:
 *     ExyokiOffice::Security::SignatureAlgorithm GetSignatureAlgorithm() const override
 *     {
 *         return ExyokiOffice::Security::SignatureAlgorithm::RsaSha256;
 *     }
 *     std::vector<std::vector<Byte>> GetCertificateChain() const override { return {m_certificateDer}; }
 *     bool SignData(ExyokiOffice::Security::SignatureAlgorithm, std::span<const Byte> data,
 *                   std::vector<Byte>& signature) const override { ... }
 *     bool VerifyData(ExyokiOffice::Security::SignatureAlgorithm, std::span<const Byte> data,
 *                     std::span<const Byte> signature,
 *                     std::span<const Byte> certificateDer) const override { ... }
 * };
 *
 * auto provider = std::make_shared<MyProvider>();
 * ExyokiOffice::Security::SignPackage(*document, provider);
 * @endcode
 */
class EXYOKIOFFICE_EXPORT ICryptoProvider
{
public:
    /** @brief Shared ownership handle used everywhere a provider is passed. */
    using Ptr = std::shared_ptr<ICryptoProvider>;

    /** @brief Destroys the provider interface. */
    virtual ~ICryptoProvider() = default;

    /**
     * @brief Returns the algorithm this provider signs with.
     *
     * Used when the caller does not pick an algorithm explicitly. It must match
     * the key the provider holds, because the identifier ends up in the
     * signature and every verifier will rely on it.
     */
    [[nodiscard]] virtual SignatureAlgorithm GetSignatureAlgorithm() const = 0;

    /**
     * @brief Returns the signing certificate chain in DER encoding.
     *
     * The first entry must be the certificate matching the signing key. Further
     * entries are intermediate certificates and are embedded so a verifier can
     * build the chain. Return an empty vector when no certificate is available;
     * signing then fails, because a package signature must carry one.
     */
    [[nodiscard]] virtual std::vector<std::vector<Byte>> GetCertificateChain() const = 0;

    /**
     * @brief Signs already canonicalized bytes with the provider's private key.
     *
     * The library passes the canonical form of the SignedInfo element. The
     * provider must hash it with the digest belonging to @p algorithm and
     * produce the raw signature value, exactly as it is written into the
     * SignatureValue element (base64 encoding is done by the library).
     *
     * @param algorithm Signature algorithm requested by the caller.
     * @param data Canonicalized bytes to sign.
     * @param signature Receives the raw signature value.
     * @return false when the algorithm is unsupported or signing failed.
     */
    [[nodiscard]] virtual bool SignData(SignatureAlgorithm algorithm,
                                        std::span<const Byte> data,
                                        std::vector<Byte>& signature) const = 0;

    /**
     * @brief Verifies a signature value against a certificate.
     *
     * @param algorithm Signature algorithm named by the signature.
     * @param data Canonicalized bytes that were signed.
     * @param signature Raw signature value taken from the signature.
     * @param certificateDer Signing certificate in DER encoding.
     * @return false both for an invalid signature and for any internal failure;
     *         the library reports the result as "not valid" either way.
     */
    [[nodiscard]] virtual bool VerifyData(SignatureAlgorithm algorithm,
                                          std::span<const Byte> data,
                                          std::span<const Byte> signature,
                                          std::span<const Byte> certificateDer) const = 0;

    /**
     * @brief Computes a digest; override only to use your own hash backend.
     *
     * The default implementation calls the library's built-in SHA code, so a
     * provider normally does not need to implement this at all.
     *
     * @param algorithm Digest algorithm to use.
     * @param data Bytes to hash.
     * @param digest Receives the raw digest bytes.
     * @return false when the algorithm is unsupported or hashing failed.
     */
    [[nodiscard]] virtual bool ComputeDigest(DigestAlgorithm algorithm,
                                             std::span<const Byte> data,
                                             std::vector<Byte>& digest) const;
};

} // namespace ExyokiOffice::Security
