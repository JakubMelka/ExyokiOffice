// Copyright (c) 2026 Jakub Melka and Contributors
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#pragma once

#include "ExyokiOffice/Export.hpp"
#include "ExyokiOffice/OpenXmlPackage.hpp"
#include "ExyokiOffice/Tools/PackageModel.hpp"
#include "ExyokiOffice/StandardTypes.hpp"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace ExyokiOffice::Tools
{

/// Selects which shared resources DeduplicateSharedResources() may merge.
struct EXYOKIOFFICE_EXPORT ResourceDeduplicationOptions
{
    /// Merge parts whose content type starts with "image/".
    bool IncludeImages = true;
    /// Merge parts whose content type starts with "audio/".
    bool IncludeAudio = true;
    /// Merge parts whose content type starts with "video/".
    bool IncludeVideo = true;
    /**
     * Merge embedded font parts. Off by default: fonts are usually obfuscated
     * per document, so identical payloads are rare and merging never pays off.
     */
    bool IncludeFonts = false;
    /**
     * Merge every leaf binary part regardless of content type. Media filters
     * above are ignored when set. XML parts and parts with outgoing
     * relationships or children are never merged.
     */
    bool IncludeAllBinaryParts = false;
    /// Analyze and report duplicate groups without modifying the package.
    bool DryRun = false;
    /// File overload only: allow overwriting an existing output file.
    bool Overwrite = false;
};

/// One group of byte-identical resources that was (or would be) merged.
struct EXYOKIOFFICE_EXPORT ResourceDuplicateGroup
{
    /// Shared MIME content type of the group.
    std::string ContentType;
    /// URI of the canonical part every relationship now points at.
    std::string KeptPartUri;
    /// URIs of the removed duplicate parts.
    std::vector<std::string> DuplicatePartUris;
    /// Payload size of one copy in bytes.
    UInt64 PayloadBytes = 0;
};

/// Result of DeduplicateSharedResources().
struct EXYOKIOFFICE_EXPORT ResourceDeduplicationResult
{
    bool Ok = false;
    /// Duplicate groups in deterministic (URI) order.
    std::vector<ResourceDuplicateGroup> Groups;
    /// Number of duplicate parts removed from the package.
    Size RemovedParts = 0;
    /// Number of relationships redirected to a canonical part.
    Size RewrittenRelationships = 0;
    /// Uncompressed payload bytes saved by removing duplicates.
    UInt64 BytesSaved = 0;
    std::vector<ToolDiagnostic> Diagnostics;
};

/**
 * @brief Merges byte-identical shared resources inside one Office package.
 *
 * Candidate parts are leaf binary parts (no children, no outgoing
 * relationships) selected by @p options — by default embedded images, audio,
 * and video. Parts are grouped by content type and exact payload bytes; within
 * a group, the part with the lexicographically smallest URI is kept and every
 * relationship that targeted a duplicate is redirected to it while keeping its
 * relationship id, so `r:embed`/`r:id` references in XML stay valid. Duplicates
 * that become unreachable are removed from the package.
 *
 * The operation edits the package in memory; the caller saves it. With
 * `DryRun`, groups and potential savings are reported without changes.
 */
EXYOKIOFFICE_EXPORT ResourceDeduplicationResult DeduplicateSharedResources(
    OpenXmlPackage& package,
    const ResourceDeduplicationOptions& options = {});

/**
 * @brief File-to-file convenience overload.
 *
 * Loads @p inputFile as a generic OPC package, deduplicates shared resources,
 * and saves the result to @p outputFile (which may equal the input path).
 * Existing outputs are protected unless `options.Overwrite` is set. With
 * `DryRun`, the package is analyzed and no output file is written.
 */
EXYOKIOFFICE_EXPORT ResourceDeduplicationResult DeduplicateSharedResources(
    const std::filesystem::path& inputFile,
    const std::filesystem::path& outputFile,
    const ResourceDeduplicationOptions& options = {});

} // namespace ExyokiOffice::Tools
