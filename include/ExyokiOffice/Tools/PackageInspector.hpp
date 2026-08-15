// Copyright (c) 2026 Jakub Melka and Contributors
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#pragma once

#include "ExyokiOffice/Export.hpp"
#include "ExyokiOffice/OpenXmlPackage.hpp"
#include "ExyokiOffice/Packaging/DocumentProperties.hpp"
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
 * @brief Reads the user-defined properties (docProps/custom.xml), if present.
 *
 * The counterpart of the fall-through in WriteCoreProperty: a name that is not
 * one of the document's own properties is stored here, and this is what reads
 * it back. Returns an empty vector for a package with no custom properties
 * part. The package is taken by reference rather than by const reference
 * because the underlying editor is a single read/write view of it.
 */
EXYOKIOFFICE_EXPORT std::vector<Packaging::DocumentCustomProperty> ReadCustomProperties(
    OpenXmlPackage& package);

/**
 * @brief Writes a single named document property.
 *
 * @param name Property name, matched case-insensitively against the fields of
 *             CoreProperties and the rest of what Packaging::DocumentProperties
 *             covers: Title, Subject, Creator, Keywords, Description,
 *             LastModifiedBy, Category, ContentStatus, Language, Identifier,
 *             Revision, Version, Application, AppVersion, Company, Manager and
 *             HyperlinkBase. Any other name is written as a user-defined
 *             property, which ReadCustomProperties reads back.
 * @param value New value. An empty value clears the property: the underlying
 *              element is removed rather than left behind empty.
 * @return True when the property was written. False for an empty name, and for
 *         Created, Modified and LastPrinted, which are timestamps rather than
 *         text and are set through Packaging::DocumentProperties directly.
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
