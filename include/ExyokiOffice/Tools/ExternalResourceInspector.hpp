// Copyright (c) 2026 Jakub Melka and Contributors
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#pragma once

#include "ExyokiOffice/Export.hpp"
#include "ExyokiOffice/Tools/PackageLimits.hpp"
#include "ExyokiOffice/OpenXmlPackage.hpp"
#include "ExyokiOffice/Security/ExternalResources.hpp"
#include "ExyokiOffice/Tools/PackageModel.hpp"
#include "ExyokiOffice/StandardTypes.hpp"

#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

namespace ExyokiOffice::Tools
{

/// Options controlling a single InspectExternalResources() invocation.
struct EXYOKIOFFICE_EXPORT ExternalResourceInspectionOptions
{
    /// ZIP/XML safety limits applied before loading; see Tools::DefaultPackageLimits.
    OpenXmlPackageLimits Limits = DefaultPackageLimits();
};

/// One outward pointing relationship, flattened for reporting.
struct EXYOKIOFFICE_EXPORT ExternalReferenceRecord
{
    /// Part owning the relationship, or "/" for the package root.
    std::string SourcePartUri;
    /// Relationship id in that container.
    std::string RelationshipId;
    /// Relationship type URI.
    std::string RelationshipType;
    /// Target exactly as stored; may be relative.
    std::string Target;
    /// Resource kind derived from the relationship type, as a stable identifier.
    std::string Kind;
};

/// Everything a package points at outside itself.
struct EXYOKIOFFICE_EXPORT ExternalResourceReport
{
    /// False when the package failed to load at all.
    bool Loaded = false;
    /// One entry per external relationship, package root first.
    std::vector<ExternalReferenceRecord> References;
    /// Diagnostics about loading; never about accessing a target.
    std::vector<ToolDiagnostic> Diagnostics;

    /// Number of external relationships found.
    [[nodiscard]] Size Count() const noexcept { return References.size(); }
};

/**
 * @brief Loads a package and lists every resource it references from outside.
 *
 * Nothing is accessed: no resolver is involved, no network or file system call
 * is made for any target, and the report says what the document claims, not
 * what is actually there. This is the audit that answers "does this document
 * phone home" before any decision is made about allowing it to.
 */
EXYOKIOFFICE_EXPORT ExternalResourceReport
InspectExternalResources(const std::filesystem::path& path, const ExternalResourceInspectionOptions& options = {});

/// Same as InspectExternalResources(path, options) but inspects an already-loaded package.
EXYOKIOFFICE_EXPORT ExternalResourceReport InspectExternalResources(const OpenXmlPackage& package);

} // namespace ExyokiOffice::Tools
