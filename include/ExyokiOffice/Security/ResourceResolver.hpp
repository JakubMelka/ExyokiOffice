// Copyright (c) 2026 Jakub Melka and Contributors
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#pragma once

#include "ExyokiOffice/Export.hpp"
#include "ExyokiOffice/ICancellationToken.hpp"
#include "ExyokiOffice/StandardTypes.hpp"

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace ExyokiOffice::Security
{

/**
 * @brief Why the library is asking about an external resource.
 *
 * The kind is derived from the type URI of the OPC relationship that carries the
 * external target, so it says what the document meant the resource to be, not
 * what the resource turns out to contain. A policy can allow linked images and
 * still refuse to let anything read an attached template.
 */
enum class ExternalResourceKind
{
    /** @brief The relationship type is not one of the kinds listed below. */
    Unknown,
    /** @brief An image referenced through `r:link` instead of `r:embed`. */
    LinkedImage,
    /** @brief Linked audio or video. */
    LinkedMedia,
    /** @brief The template a Word document is attached to. */
    AttachedTemplate,
    /** @brief A workbook an Excel file links to through an external link part. */
    ExternalWorkbook,
    /** @brief A linked (not embedded) OLE object or package. */
    LinkedOleObject,
    /** @brief A hyperlink target; content is never read for this kind. */
    Hyperlink
};

/**
 * @brief Whether the caller wants the bytes or only a policy decision.
 *
 * CheckOnly answers "would this target be allowed" without asking the resolver
 * for anything, which is what hyperlink and OLE targets need: the question is
 * whether the document points somewhere it should not, not what is there.
 */
enum class ExternalResourceAccess
{
    /** @brief Evaluate the policy and return; the resolver is not called. */
    CheckOnly,
    /** @brief Ask the resolver for the resource content. */
    Read
};

/** @brief Outcome of a request for an external resource. */
enum class ExternalResourceStatus
{
    /** @brief The resource was resolved, or the policy check passed. */
    Ok,
    /** @brief No resolver was supplied, which is the default state of every package. */
    NoResolver,
    /** @brief The policy refused the target, or the resolver refused to serve it. */
    AccessDenied,
    /** @brief The resolver could not find the resource. */
    NotFound,
    /** @brief The resolver gave up before the deadline in the request. */
    TimedOut,
    /** @brief The resource is larger than the request allowed; no data is returned. */
    TooLarge,
    /** @brief The per-package request count or byte budget is exhausted. */
    BudgetExceeded,
    /** @brief The resolver does not handle this scheme or kind. */
    Unsupported,
    /** @brief The cancellation token was signalled. */
    Cancelled,
    /** @brief Anything else the resolver reports as a failure. */
    Failed
};

/**
 * @brief One request handed to a resolver.
 *
 * Everything the library knows about the reference is here, so a resolver can
 * apply its own policy on top of the library's. Uri is absolute and already
 * checked against the package policy; OriginalTarget is what the relationship
 * actually stores, which is useful for logging and for relative targets.
 */
struct EXYOKIOFFICE_EXPORT ExternalResourceRequest
{
    /** @brief Absolute URI, after any relative target was resolved against the policy base. */
    std::string Uri;
    /** @brief The relationship target exactly as stored in the .rels part. */
    std::string OriginalTarget;
    /** @brief What the document uses the resource for. */
    ExternalResourceKind Kind = ExternalResourceKind::Unknown;
    /** @brief Whether content is wanted; a resolver never sees a CheckOnly request. */
    ExternalResourceAccess Access = ExternalResourceAccess::Read;
    /** @brief URI of the part owning the relationship, or "/" for the package root. */
    std::string SourcePartUri;
    /** @brief Relationship id in the source part. */
    std::string RelationshipId;
    /** @brief Relationship type URI. */
    std::string RelationshipType;
    /** @brief Deadline for the whole operation; zero means the policy set no limit. */
    std::chrono::milliseconds Timeout{0};
    /** @brief Largest response the caller will accept; zero means no limit. */
    UInt64 MaxBytes = 0;
    /** @brief Non-owning token; valid for the duration of the call. May be nullptr. */
    const ICancellationToken* CancellationToken = nullptr;
};

/** @brief What a resolver returns for one request. */
struct EXYOKIOFFICE_EXPORT ExternalResourceResponse
{
    /** @brief Ok only when Data holds the complete resource. */
    ExternalResourceStatus Status = ExternalResourceStatus::Failed;
    /** @brief The resource content; empty for every status other than Ok. */
    std::vector<Byte> Data;
    /** @brief MIME type reported by the source, when it reported one. Treated as a hint only. */
    std::string ContentType;
    /** @brief The URI the content actually came from, when redirects were followed. */
    std::string EffectiveUri;
    /** @brief Human readable explanation of a failure. */
    std::string Message;

    /** @brief True when the request succeeded. */
    [[nodiscard]] bool Succeeded() const noexcept { return Status == ExternalResourceStatus::Ok; }
    [[nodiscard]] explicit operator bool() const noexcept { return Succeeded(); }
};

/**
 * @brief Access to resources outside the package, supplied by the application.
 *
 * ExyokiOffice never downloads anything and never reads a file outside the
 * package it was given. A document can nevertheless point outward - a linked
 * image, an attached template, a workbook another workbook links to - and those
 * targets are kept as opaque URIs in external OPC relationships.
 *
 * When an application has a legitimate reason to follow such a target, it
 * implements this interface and passes it in as a std::shared_ptr, exactly like
 * ICryptoProvider. The library then asks; it never fetches. A resolver can sit
 * on top of an HTTP client, a local file reader, a document management system,
 * or a cache that only ever answers from resources the application already
 * trusts.
 *
 * A resolver is only ever reached after the package policy
 * (ExternalResourcePolicy) has allowed the target, and the response is checked
 * against the same policy afterwards, so a straightforward implementation stays
 * safe. Resolve() must not throw; the library guards against it, but a thrown
 * exception is reported as an opaque failure.
 *
 * Nothing in loading, saving, or validating a package calls a resolver. It is
 * reached only from the explicit ResolveExternalResource() family of functions
 * and the high level helpers built on them.
 *
 * Implementations must be safe to call concurrently from several threads. The
 * library keeps the shared pointer alive for the whole operation.
 *
 * @code
 * class FileResolver final : public ExyokiOffice::Security::IExternalResourceResolver
 * {
 * public:
 *     ExyokiOffice::Security::ExternalResourceResponse Resolve(
 *         const ExyokiOffice::Security::ExternalResourceRequest& request) override
 *     {
 *         ExyokiOffice::Security::ExternalResourceResponse response;
 *         // Read request.Uri, honour request.Timeout and request.MaxBytes, and
 *         // fill response.Data plus response.Status.
 *         return response;
 *     }
 * };
 *
 * auto resolver = std::make_shared<FileResolver>();
 * document->SetExternalResourceResolver(resolver);
 * document->SetExternalResourcePolicy(policy);
 * @endcode
 */
class EXYOKIOFFICE_EXPORT IExternalResourceResolver
{
public:
    /** @brief Shared ownership handle used everywhere a resolver is passed. */
    using Ptr = std::shared_ptr<IExternalResourceResolver>;

    /** @brief Destroys the resolver interface. */
    virtual ~IExternalResourceResolver() = default;

    /**
     * @brief Returns the resource named by the request, or the reason it cannot.
     *
     * The implementation should stop once @p request.MaxBytes have been read and
     * report TooLarge rather than buffering the whole resource, and it should
     * give up once @p request.Timeout has elapsed. Both limits are also enforced
     * by the library afterwards, but only the resolver can avoid doing the work.
     *
     * When @p request.CancellationToken is not null and reports cancellation,
     * return Cancelled as soon as it is safe to do so.
     *
     * @param request What the library wants and under which limits.
     * @return The resource, or a status explaining why there is none.
     */
    [[nodiscard]] virtual ExternalResourceResponse Resolve(const ExternalResourceRequest& request) = 0;
};

/**
 * @brief What a package is allowed to reach, enforced by the library.
 *
 * The default value allows nothing: with no allowed schemes and no allowed
 * kinds, every request is denied before a resolver is consulted. Turning the
 * feature on therefore takes two deliberate steps - installing a resolver and
 * widening the policy - and forgetting either one keeps the package sealed.
 *
 * The library checks the target against this policy before calling the
 * resolver and checks the response against it afterwards, so a resolver that
 * naively fetches whatever URI it is handed still cannot be steered outside
 * the allowlist.
 */
struct EXYOKIOFFICE_EXPORT ExternalResourcePolicy
{
    /** @brief Allowed URI schemes, lower case and without the colon ("https", "file"). */
    std::vector<std::string> AllowedSchemes;
    /**
     * @brief Allowed hosts.
     *
     * An entry starting with a dot matches that domain and its subdomains
     * (".example.com" matches "example.com" and "cdn.example.com"); any other
     * entry must match the host exactly. Comparison is case insensitive.
     *
     * There is deliberately no wildcard for "any host": a policy is a list of
     * names someone wrote down, and a document cannot widen it. The other half
     * of that decision is that the list is the *only* thing checked. The
     * library does not know which names resolve to loopback, to a private
     * range, or to a cloud instance metadata address, so an entry that resolves
     * there is allowed exactly as written. A resolver reaching a network where
     * that distinction matters has to make it itself - the names are checked
     * here, the addresses they resolve to are not.
     */
    std::vector<std::string> AllowedHosts;
    /** @brief Allowed ports; empty means only the default port of the scheme. */
    std::vector<UInt16> AllowedPorts;
    /**
     * @brief Allowed path prefixes, used for schemes that address a file system.
     *
     * Empty means every path of an allowed host is acceptable. Paths are
     * percent-decoded and normalized before the comparison, and a target whose
     * original form navigated upwards is rejected outright.
     */
    std::vector<std::string> AllowedPathPrefixes;
    /** @brief Resource kinds that may be requested at all; empty allows none. */
    std::vector<ExternalResourceKind> AllowedKinds;
    /**
     * @brief Base URI relative relationship targets are resolved against.
     *
     * Some external targets are stored relative, most notably a Word attached
     * template. Without a base there is nothing to resolve them against, so an
     * empty value denies every relative target.
     */
    std::string BaseUri;
    /** @brief Deadline passed to the resolver for one request; zero means no limit. */
    std::chrono::milliseconds Timeout{5000};
    /** @brief Largest single resource that may be read. */
    UInt64 MaxResourceBytes = 8ull * 1024 * 1024;
    /** @brief Total bytes that may be read for one package. */
    UInt64 MaxTotalBytes = 32ull * 1024 * 1024;
    /** @brief Number of requests that may be made for one package. */
    UInt32 MaxRequests = 64;
    /** @brief Whether a URI may carry credentials ("https://user:secret@host/"). */
    bool AllowUserInfoInUri = false;
    /**
     * @brief Whether a redirect may land outside the allowlist.
     *
     * When a resolver reports an EffectiveUri different from the requested one,
     * that URI is checked against the policy again. Leaving this false makes a
     * redirect to a denied target fail and the data be dropped.
     */
    bool FollowRedirectOutsideAllowlist = false;

    /** @brief The policy every package starts with: nothing is allowed. */
    [[nodiscard]] static ExternalResourcePolicy Deny();

    /**
     * @brief Allows https access to the given hosts for the given kinds.
     *
     * A convenience for the common case. Every other setting keeps its default,
     * so the size and request budgets still apply.
     *
     * @param hosts Host entries in the AllowedHosts format.
     * @param kinds Resource kinds to allow.
     */
    [[nodiscard]] static ExternalResourcePolicy HttpsOnly(std::vector<std::string> hosts,
                                                          std::vector<ExternalResourceKind> kinds);
};

/** @brief Returns a stable identifier for a resource kind, for logs and reports. */
[[nodiscard]] EXYOKIOFFICE_EXPORT std::string_view ToString(ExternalResourceKind kind) noexcept;

/** @brief Returns a stable identifier for a request status, for logs and reports. */
[[nodiscard]] EXYOKIOFFICE_EXPORT std::string_view ToString(ExternalResourceStatus status) noexcept;

/** @brief Maps an OPC relationship type URI to the resource kind it represents. */
[[nodiscard]] EXYOKIOFFICE_EXPORT ExternalResourceKind ClassifyRelationshipType(std::string_view relationshipType) noexcept;

} // namespace ExyokiOffice::Security
