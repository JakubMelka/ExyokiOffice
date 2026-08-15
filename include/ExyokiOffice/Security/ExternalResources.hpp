// Copyright (c) 2026 Jakub Melka and Contributors
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#pragma once

#include "ExyokiOffice/Export.hpp"
#include "ExyokiOffice/ICancellationToken.hpp"
#include "ExyokiOffice/OpenXmlPackage.hpp"
#include "ExyokiOffice/Security/ResourceResolver.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace ExyokiOffice::Security
{

/**
 * @brief One relationship in a package whose target lies outside it.
 *
 * These are the edges the OPC layer stores with `TargetMode="External"`. The
 * library keeps them as written and never dereferences them on its own; this
 * structure is what makes them enumerable so an application can decide what,
 * if anything, to do about them.
 */
struct EXYOKIOFFICE_EXPORT ExternalReference
{
    /** @brief URI of the part owning the relationship, or "/" for the package root. */
    std::string SourcePartUri;
    /** @brief Relationship id in the source container. */
    std::string RelationshipId;
    /** @brief Relationship type URI. */
    std::string RelationshipType;
    /** @brief Target exactly as stored in the .rels part; may be relative. */
    std::string Target;
    /** @brief What the relationship type says the target is. */
    ExternalResourceKind Kind = ExternalResourceKind::Unknown;
};

/**
 * @brief Lists every external relationship in a package.
 *
 * Nothing is resolved and no resolver is needed, so this works on any package
 * and answers the plain question "what does this document point at". It is the
 * basis of `exyoki external` and of any audit an application wants to run
 * before it decides on a policy.
 *
 * @param package Package to inspect; it is not modified.
 * @return One entry per external relationship, package root first.
 */
[[nodiscard]] EXYOKIOFFICE_EXPORT std::vector<ExternalReference> CollectExternalReferences(
    const OpenXmlPackage& package);

/**
 * @brief Asks whether the package policy would allow a target, without reading it.
 *
 * This is the check that fits hyperlinks and linked OLE objects: the question
 * is whether the document points somewhere it should not, and the content is
 * never wanted. No resolver is required, and no budget is consumed.
 *
 * @param package Package whose policy decides.
 * @param reference Reference to evaluate.
 * @return Ok when the target passes, otherwise the reason it does not.
 */
[[nodiscard]] EXYOKIOFFICE_EXPORT ExternalResourceStatus CheckExternalReference(OpenXmlPackage& package,
                                                                                const ExternalReference& reference);

/**
 * @brief Reads an external resource through the resolver installed on the package.
 *
 * The target is checked against the package policy first, so a package with no
 * resolver reports NoResolver and a package with a default policy reports
 * AccessDenied. Every refusal is also recorded in
 * OpenXmlPackage::LastValidationResult().
 *
 * @param package Package owning the resolver, the policy, and the byte budget.
 * @param reference Reference to read.
 * @param cancellationToken Optional non-owning token handed to the resolver.
 * @return The resource, or the status explaining why there is none.
 *
 * @code
 * for (const auto& reference : ExyokiOffice::Security::CollectExternalReferences(*document))
 * {
 *     if (reference.Kind != ExyokiOffice::Security::ExternalResourceKind::LinkedImage)
 *     {
 *         continue;
 *     }
 *     const auto image = ExyokiOffice::Security::ResolveExternalResource(*document, reference);
 *     if (image)
 *     {
 *         // image.Data holds the bytes the application asked for.
 *     }
 * }
 * @endcode
 */
[[nodiscard]] EXYOKIOFFICE_EXPORT ExternalResourceResponse ResolveExternalResource(
    OpenXmlPackage& package,
    const ExternalReference& reference,
    const ICancellationToken* cancellationToken = nullptr);

/**
 * @brief Reads an external resource named by its source part and relationship id.
 *
 * @param package Package owning the resolver and the policy.
 * @param sourcePartUri Part owning the relationship, or "/" for the package root.
 * @param relationshipId Relationship id in that container.
 * @param cancellationToken Optional non-owning token handed to the resolver.
 * @return The resource, or AccessDenied when the id names no external relationship.
 */
[[nodiscard]] EXYOKIOFFICE_EXPORT ExternalResourceResponse ResolveExternalResource(
    OpenXmlPackage& package,
    std::string_view sourcePartUri,
    std::string_view relationshipId,
    const ICancellationToken* cancellationToken = nullptr);

} // namespace ExyokiOffice::Security
