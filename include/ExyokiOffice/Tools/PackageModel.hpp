// Copyright (c) 2026 Jakub Melka and Collaborators
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#pragma once

#include "ExyokiOffice/Export.hpp"
#include "ExyokiOffice/FileFormatVersions.h"
#include "ExyokiOffice/OpenXmlPackagePart.hpp"
#include "ExyokiOffice/ValidationResult.hpp"
#include "ExyokiOffice/StandardTypes.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ExyokiOffice::Tools
{

/**
 * @brief High-level document family inferred from the package's main part.
 */
enum class DocumentFamily
{
    Unknown,
    Word,
    Excel,
    PowerPoint
};

/// Mirrors OpenXmlPartKind for parts reported by the Tools module.
enum class PartPayloadKind
{
    Xml,
    Binary
};

/**
 * @brief Flat description of a single package part, independent of its generated type.
 */
struct EXYOKIOFFICE_EXPORT PartRecord
{
    std::string Uri;
    std::string ContentType;
    PartPayloadKind Kind = PartPayloadKind::Xml;
    UInt64 Size = 0;
    /// Name of the generated part class, or "OpaquePackagePart" for untyped parts.
    std::string DescriptorName;
    /// Every incoming relationship edge that targets this part.
    std::vector<OpenXmlIncomingRelationship> Incoming;
};

/**
 * @brief Flat description of a single relationship edge, with target resolution.
 */
struct EXYOKIOFFICE_EXPORT RelationshipRecord
{
    /// URI of the source part, or "/" for the package-level relationship container.
    std::string SourceUri;
    OpenXmlRelationship Relationship;
    /// Normalized target URI (empty for external targets).
    std::string ResolvedTargetUri;
    /// True when the target resolves to a part that exists in the package (always false for external targets).
    bool TargetExists = false;
};

/**
 * @brief Common OPC core/extended document properties, read generically across families.
 */
struct EXYOKIOFFICE_EXPORT CoreProperties
{
    std::string Title;
    std::string Subject;
    std::string Creator;
    std::string Keywords;
    std::string Description;
    std::string LastModifiedBy;
    std::string Category;
    std::string ContentStatus;
    std::string Created;
    std::string Modified;
    std::string Application;
    std::string AppVersion;
    std::string Company;
};

/**
 * @brief Aggregate package summary used by the `info` command.
 */
struct EXYOKIOFFICE_EXPORT PackageInfo
{
    DocumentFamily Family = DocumentFamily::Unknown;
    /**
     * True when the package declares its main part with the ISO 29500 Strict
     * relationship type. Strict packages are recognized only to report them:
     * the typed DOM is generated from the Transitional schemas, so Family stays
     * Unknown and the family-aware tools cannot read such a document. See
     * docs/Compatibility.md.
     */
    bool IsStrictConformance = false;
    /// Human-readable document type name (e.g. "Document", "MacroEnabledWorkbook").
    std::string DocumentTypeName;
    std::string MainPartUri;
    std::string MainPartContentType;
    CoreProperties Properties;
    Size PartCount = 0;
    Size RelationshipCount = 0;
    UInt64 TotalPartSize = 0;
};

/// Severity for Tools-level diagnostics (distinct from library ValidationSeverity).
enum class ToolSeverity
{
    Info,
    Warning,
    Error
};

/**
 * @brief A single Tools-level diagnostic message (I/O errors, skipped entries, etc.).
 */
struct EXYOKIOFFICE_EXPORT ToolDiagnostic
{
    ToolSeverity Severity = ToolSeverity::Error;
    std::string Message{};
    /// Optional context such as a part URI or file path.
    std::string Context{};
};

/// Renders a document family as a lowercase identifier (e.g. "word").
EXYOKIOFFICE_EXPORT std::string_view ToString(DocumentFamily family) noexcept;
/// Renders a Tools diagnostic severity ("info" | "warning" | "error").
EXYOKIOFFICE_EXPORT std::string_view ToString(ToolSeverity severity) noexcept;
/// Renders a library validation severity ("error" | "warning").
EXYOKIOFFICE_EXPORT std::string_view ToString(ValidationSeverity severity) noexcept;
/// Renders a library validation domain (e.g. "opc", "dom", "schema").
EXYOKIOFFICE_EXPORT std::string_view ToString(ValidationDomain domain) noexcept;
/// Renders a library validation error id as its enumerator name (e.g. "OpcDanglingRelationshipTarget").
EXYOKIOFFICE_EXPORT std::string_view ToString(ValidationErrorId id) noexcept;

/**
 * @brief Parses a `--office-version` CLI value into a FileFormatVersions enumerator.
 *
 * Accepts "2007", "2010", "2013", "2016", "2019", "2021", and "365"
 * (case-insensitive). Returns std::nullopt for anything else.
 */
EXYOKIOFFICE_EXPORT std::optional<ExyokiOffice::OpenXml::FileFormatVersions> ParseFileFormatVersion(
    std::string_view text) noexcept;

/**
 * @brief Expands `*`/`?` wildcards in the filename component of each pattern.
 *
 * Windows shells pass wildcard arguments through unexpanded, so batch commands
 * expand them here: a pattern whose filename component contains `*` or `?` is
 * matched (case-insensitively) against the entries of its parent directory,
 * non-recursively, and the matches are appended in lexicographic order. A
 * pattern without wildcards is appended as-is, even when the file does not
 * exist — the command working through the list reports that per file.
 * Wildcard patterns that match nothing add a warning diagnostic.
 */
EXYOKIOFFICE_EXPORT std::vector<std::filesystem::path> ExpandInputPaths(
    const std::vector<std::string>& patterns, std::vector<ToolDiagnostic>& diagnostics);

} // namespace ExyokiOffice::Tools
