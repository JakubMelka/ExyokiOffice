// Copyright (c) 2026 Jakub Melka and Contributors
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#pragma once

#include "ExyokiOffice/OpenXmlPackage.hpp"
#include "ExyokiOffice/Security/ExternalResources.hpp"
#include "ExyokiOffice/Security/ResourceResolver.hpp"

#include "ExternalUri.hpp"

#include <optional>
#include <vector>

namespace ExyokiOffice::Security
{

/**
 * @brief The only place in the library that calls an IExternalResourceResolver.
 *
 * Keeping the call behind a single function is what makes the guarantee
 * checkable: the policy is applied on the way in and on the way out, the
 * per-package budget is accounted here, and every refusal produces a diagnostic
 * on the package. Nothing else may talk to a resolver directly.
 */
class ExternalResourceGateway final
{
public:
    ExternalResourceGateway() = delete;

    /**
     * @brief Evaluates the policy and, unless the request is CheckOnly, asks the resolver.
     *
     * @param package Package owning the resolver, the policy, and the budget.
     * @param request What is wanted; Uri may be a relative target and is resolved
     *                against the policy base before it is checked.
     * @return The resource, or the status explaining why there is none.
     */
    [[nodiscard]] static ExternalResourceResponse Fetch(OpenXmlPackage& package, ExternalResourceRequest request);

    /**
     * @brief Lists the external relationships of the package root and of every part.
     *
     * The package part map is walked rather than the relationship graph, so a
     * part reachable through several edges still contributes its own
     * relationships exactly once.
     */
    [[nodiscard]] static std::vector<ExternalReference> Collect(const OpenXmlPackage& package);

    /** @brief Finds one external relationship by its source container and id. */
    [[nodiscard]] static std::optional<ExternalReference> Find(const OpenXmlPackage& package,
                                                               std::string_view sourcePartUri,
                                                               std::string_view relationshipId);

private:
    /// Reason a target failed the allowlist, or an empty string when it passed.
    static std::string CheckAgainstPolicy(const ExternalResourcePolicy& policy, const ExternalUriParts& parts);
    static bool IsKindAllowed(const ExternalResourcePolicy& policy, ExternalResourceKind kind);
    static bool IsHostAllowed(const ExternalResourcePolicy& policy, const std::string& host);
    static bool IsPortAllowed(const ExternalResourcePolicy& policy, const ExternalUriParts& parts);
    static bool IsPathAllowed(const ExternalResourcePolicy& policy, const std::string& path);

    static ExternalResourceResponse Deny(OpenXmlPackage& package,
                                         const ExternalResourceRequest& request,
                                         ExternalResourceStatus status,
                                         ValidationErrorId id,
                                         std::string message);
    static void Report(OpenXmlPackage& package,
                       const ExternalResourceRequest& request,
                       ValidationErrorId id,
                       const std::string& message);
};

} // namespace ExyokiOffice::Security
