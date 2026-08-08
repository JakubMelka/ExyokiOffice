// Copyright (c) 2026 Jakub Melka and Collaborators
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#pragma once

#include "ExyokiOffice/Export.hpp"
#include "ExyokiOffice/OpenXmlPackage.hpp"
#include "ExyokiOffice/Tools/PackageModel.hpp"

#include <memory>
#include <string_view>
#include <vector>

namespace ExyokiOffice::Tools
{

/**
 * @brief Traverses every part reachable from the package root exactly once.
 *
 * The part graph can have more than one parent for a given node (e.g. a
 * shared image referenced from two slides), so a plain recursive walk over
 * OpenXmlPartContainer::Parts() would visit some parts more than once.
 * This does a breadth-first traversal and de-duplicates by part URI.
 * OpenXmlPartContainer::ForEachPart() is protected, so this is the public
 * substitute used by every other Tools function that needs "all parts".
 */
EXYOKIOFFICE_EXPORT std::vector<std::shared_ptr<OpenXmlPackagePart>> CollectAllParts(
    const OpenXmlPackage& package);

/// Flat listing of every part in the package (root parts and all descendants).
EXYOKIOFFICE_EXPORT std::vector<PartRecord> ListParts(const OpenXmlPackage& package);

/**
 * @brief Flat listing of every relationship edge, from the package root and every part.
 *
 * Each edge's target is resolved to a normalized part URI; TargetExists reports
 * whether that URI corresponds to a part that actually exists in the package.
 */
EXYOKIOFFICE_EXPORT std::vector<RelationshipRecord> ListRelationships(const OpenXmlPackage& package);

/**
 * @brief Determines the document family by following the root "officeDocument" relationship.
 *
 * Looks up the package-level relationship whose type ends in
 * ".../officeDocument", resolves its target part, and matches the part's
 * content type against WordDocument::DocumentTypeFromMime,
 * ExcelDocument::DocumentTypeFromMime, and PowerPointDocument::DocumentTypeFromMime
 * in turn. Returns DocumentFamily::Unknown (with empty fields) when no match
 * is found.
 */
EXYOKIOFFICE_EXPORT PackageInfo GetInfo(const OpenXmlPackage& package);

/// Reads OPC core (docProps/core.xml) and extended (docProps/app.xml) properties, if present.
EXYOKIOFFICE_EXPORT CoreProperties ReadCoreProperties(const OpenXmlPackage& package);

/**
 * @brief Writes a single named core or extended property.
 *
 * @param name Property name, matching a CoreProperties field name case-insensitively
 *             (Title, Subject, Creator, Keywords, Description, LastModifiedBy,
 *             Category, ContentStatus, Company).
 * @return True when the property was written. False when the name is
 *         unrecognized or the package has no core/extended properties part to
 *         update (this function never creates new package parts).
 */
EXYOKIOFFICE_EXPORT bool WriteCoreProperty(OpenXmlPackage& package, std::string_view name, std::string_view value);

/**
 * @brief Explains why a package has no recognized document family.
 *
 * Returns the ISO 29500 Strict message when PackageInfo::IsStrictConformance
 * is set, so a caller reports the actual reason instead of a generic
 * "unrecognized family". Intended for the family-aware tools, which all reject
 * such a package the same way.
 */
EXYOKIOFFICE_EXPORT std::string DescribeUnknownFamily(const PackageInfo& info);

} // namespace ExyokiOffice::Tools
