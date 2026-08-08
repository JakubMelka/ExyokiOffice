// Copyright (c) 2026 Jakub Melka and Collaborators
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#pragma once

#include "ExyokiOffice/Export.hpp"
#include "ExyokiOffice/OpenXmlPackagePart.hpp"
#include "ExyokiOffice/Tools/PackageModel.hpp"

#include <filesystem>
#include <string>
#include <vector>

namespace ExyokiOffice::Tools
{

enum class PartChangeKind
{
    Added,
    Removed,
    ChangedXml,
    ChangedBinary,
    ContentTypeChanged
};

/// One part-level difference between two packages.
struct EXYOKIOFFICE_EXPORT PartDiffEntry
{
    std::string Uri;
    PartChangeKind Kind = PartChangeKind::ChangedXml;
    std::string LeftContentType;
    std::string RightContentType;
    /// Approximate element path of the first structural/text difference (ChangedXml with normalization only).
    std::string FirstDifferencePath;
};

enum class RelationshipChangeKind
{
    Added,
    Removed,
    Changed
};

/// One relationship-level difference between two packages.
struct EXYOKIOFFICE_EXPORT RelationshipDiffEntry
{
    /// Source part URI ("/" for the package root) the relationship belongs to.
    std::string SourceUri;
    std::string RelationshipId;
    RelationshipChangeKind Kind = RelationshipChangeKind::Changed;
    OpenXmlRelationship Left;
    OpenXmlRelationship Right;
};

/// Result of comparing two packages.
struct EXYOKIOFFICE_EXPORT DiffResult
{
    bool Ok = false;
    /// True when no part or relationship differences were found.
    bool Identical = false;
    std::vector<PartDiffEntry> PartChanges;
    std::vector<RelationshipDiffEntry> RelationshipChanges;
    std::vector<ToolDiagnostic> Diagnostics;
};

/**
 * @brief Compares two OPC packages part-by-part and relationship-by-relationship.
 *
 * Parts are matched by URI. Parts present in only one package are reported as
 * Added/Removed. For parts present in both: a content-type mismatch is
 * reported as ContentTypeChanged; otherwise binary parts are compared
 * byte-for-byte (ChangedBinary) and XML parts are compared either as raw
 * bytes (@p normalizeXml == false) or as a normalized XML tree that ignores
 * insignificant whitespace and attribute order (@p normalizeXml == true,
 * the default), reporting the first differing element's approximate path
 * in FirstDifferencePath.
 *
 * Relationships are matched by (container URI, relationship Id); a
 * Type/Target/TargetMode/IsExternal mismatch is reported as Changed.
 */
EXYOKIOFFICE_EXPORT DiffResult Compare(const std::filesystem::path& left, const std::filesystem::path& right,
                                       bool normalizeXml = true);

} // namespace ExyokiOffice::Tools
