// Copyright (c) 2026 Jakub Melka and Collaborators
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#pragma once

#include "ExyokiOffice/Export.hpp"
#include "ExyokiOffice/Tools/PackageModel.hpp"
#include "ExyokiOffice/Tools/ValidationRunner.hpp"
#include "ExyokiOffice/StandardTypes.hpp"

#include <cstdint>
#include <filesystem>
#include <vector>

namespace ExyokiOffice::Tools
{

/// Name of the manifest file `Unpack()` writes into the output directory when needed.
inline constexpr std::string_view kArchiverManifestFileName = "_exyoki.manifest.xml";

/// Options controlling PackageArchiver::Unpack().
struct EXYOKIOFFICE_EXPORT UnpackOptions
{
    /// Re-indents XML/rels entries for readability. Breaks byte-identical round-trip; see docs/tools/exyoki.md.
    bool PrettyPrintXml = false;
    /// Allows overwriting a non-empty output directory.
    bool Overwrite = false;
    /**
     * @brief ZIP safety limits enforced while extracting.
     *
     * Unpack reads the archive directly rather than through OpenXmlPackage, so
     * the loader's limits do not apply to it and it needs its own: an entry is
     * decompressed into memory whole, which without a bound is a decompression
     * bomb's entire attack. Entry count, per-entry size, compression ratio and
     * the running totals are checked against the same rules the loader uses,
     * through OpenXmlPackageLimits::CheckEntry and its siblings.
     *
     * See Tools::DefaultPackageLimits for what the default is.
     */
    OpenXmlPackageLimits Limits = DefaultPackageLimits();
};

/// Result of an Unpack() call.
struct EXYOKIOFFICE_EXPORT UnpackResult
{
    bool Ok = false;
    Size EntryCount = 0;
    /// True when at least one entry name required Windows-safe escaping or pretty-printing was requested
    /// (either condition causes a manifest file to be written next to the extracted tree).
    bool ManifestWritten = false;
    std::vector<ToolDiagnostic> Diagnostics;
};

/// Options controlling PackageArchiver::Pack().
struct EXYOKIOFFICE_EXPORT PackOptions
{
    /// Deflate compression level, 0 (store) to 9 (best compression).
    int CompressionLevel = 6;
    /// Rebuilds `[Content_Types].xml` from a built-in extension table instead of copying the tree's copy verbatim.
    bool RegenerateContentTypes = false;
    /// Runs ValidationRunner::Run() on the freshly written package and reports it in PackResult::Validation.
    bool ValidateAfterPack = false;
};

/// Result of a Pack() call.
struct EXYOKIOFFICE_EXPORT PackResult
{
    bool Ok = false;
    Size EntryCount = 0;
    bool ContentTypesRegenerated = false;
    /// Populated only when PackOptions::ValidateAfterPack is true.
    ValidationReport Validation;
    std::vector<ToolDiagnostic> Diagnostics;
};

/**
 * @brief Extracts every ZIP entry of an OPC package to a directory tree, preserving names byte-for-byte
 * whenever the entry name is already a valid Windows path (the common case).
 *
 * Entry names are used verbatim as relative disk paths so `[Content_Types].xml`
 * and every `_rels` relationship file keeps its exact on-disk layout. Entry names
 * that are not valid Windows paths (reserved device names, forbidden
 * characters, `..` segments) are percent-escaped OPC-style; when that
 * happens, or when PrettyPrintXml is set, a small manifest file
 * (kArchiverManifestFileName) is written into @p outDir recording the
 * original entry name for each escaped entry so Pack() can restore it.
 *
 * Without PrettyPrintXml, `Unpack()` followed by `Pack()` (default options)
 * reproduces the original ZIP entries byte-for-byte. With PrettyPrintXml,
 * the round-trip is only semantically equivalent (re-serialized XML
 * whitespace differs from the source).
 */
EXYOKIOFFICE_EXPORT UnpackResult Unpack(const std::filesystem::path& packagePath,
                                        const std::filesystem::path& outDir,
                                        const UnpackOptions& options = {});

/**
 * @brief Rebuilds an OPC ZIP package from a directory tree previously produced by Unpack().
 *
 * Reads the manifest written by Unpack() (if present) to restore original
 * entry names for any escaped/renamed files, then walks every remaining file
 * in @p inDir (in sorted relative-path order for reproducible output) and
 * writes it as a ZIP entry. `[Content_Types].xml` is preserved verbatim
 * unless PackOptions::RegenerateContentTypes is set or the file is missing,
 * in which case a best-effort table (Default entries keyed by file
 * extension) is generated instead.
 */
EXYOKIOFFICE_EXPORT PackResult Pack(const std::filesystem::path& inDir,
                                    const std::filesystem::path& outPackage,
                                    const PackOptions& options = {});

} // namespace ExyokiOffice::Tools
