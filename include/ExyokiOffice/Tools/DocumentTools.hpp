// Copyright (c) 2026 Jakub Melka and Collaborators
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#pragma once

#include "ExyokiOffice/Export.hpp"
#include "ExyokiOffice/Tools/PackageModel.hpp"
#include "ExyokiOffice/Tools/WordDocumentTools.hpp"
#include "ExyokiOffice/StandardTypes.hpp"

#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

namespace ExyokiOffice::Tools
{

/**
 * @brief Family-aware boundary selection used by SplitDocument().
 *
 * Auto selects SectionBreaks for Word, Worksheets for Excel, and Slides for
 * PowerPoint. The four Word-specific strategies are rejected for non-Word
 * packages; Worksheets and Slides are likewise accepted only for their matching
 * package family. This prevents an option from being silently reinterpreted.
 */
enum class DocumentSplitStrategy
{
    Auto,
    SectionBreaks,
    PageBreaks,
    ParagraphCount,
    Marker,
    Worksheets,
    Slides
};

/**
 * @brief Options for family-aware Office document splitting.
 *
 * Output groups always preserve source order. For Word, @ref ItemCount is used
 * only with ParagraphCount and counts body paragraphs. For Excel and PowerPoint,
 * it specifies worksheets or slides per output package and defaults to one when
 * zero. Marker is required only by the Word Marker strategy.
 */
struct EXYOKIOFFICE_EXPORT DocumentSplitOptions
{
    DocumentSplitStrategy Strategy = DocumentSplitStrategy::Auto;
    Size ItemCount = 0;
    std::string Marker;
    /** Filename prefix without an extension. Empty selects `part`. */
    std::string OutputPrefix = "part";
    /** Permit replacement of every pre-existing numbered output file. */
    bool Overwrite = false;
};

/** @brief Result of a family-aware split operation. */
struct EXYOKIOFFICE_EXPORT DocumentSplitResult
{
    bool Ok = false;
    /** Detected source family; Unknown when loading or family detection failed. */
    DocumentFamily Family = DocumentFamily::Unknown;
    /** Successfully written files, in source-content order. */
    std::vector<std::filesystem::path> OutputFiles;
    std::vector<ToolDiagnostic> Diagnostics;
};

/**
 * @brief Options for family-aware Office document merging.
 *
 * Word-only options are ignored for Excel and PowerPoint. Excel inputs append
 * worksheets; PowerPoint inputs append slides. Every input must have the same
 * detected family, and no destination is written when family validation fails.
 */
struct EXYOKIOFFICE_EXPORT DocumentMergeOptions
{
    bool Overwrite = false;
    bool InsertWordPageBreaks = true;
    Word::StyleCopyConflictPolicy WordStyleConflictPolicy = Word::StyleCopyConflictPolicy::Rename;
};

/** @brief Result of a family-aware merge operation. */
struct EXYOKIOFFICE_EXPORT DocumentMergeResult
{
    bool Ok = false;
    DocumentFamily Family = DocumentFamily::Unknown;
    /** Number of input packages incorporated, not the item count. */
    Size DocumentsMerged = 0;
    /** Number of body documents, worksheets, or slides appended. */
    Size ItemsMerged = 0;
    std::filesystem::path OutputFile;
    std::vector<ToolDiagnostic> Diagnostics;
};

/**
 * @brief Splits a Word, Excel, or PowerPoint package using family-native units.
 *
 * Word delegates to SplitWordDocument() and therefore supports structural
 * section/page/paragraph/marker boundaries. Excel clones the source workbook
 * and retains groups of worksheets. PowerPoint clones the source presentation
 * and retains groups of slides. Clone-and-prune splitting preserves package-wide
 * resources and all relationship graphs reachable by retained items. Excel and
 * PowerPoint split outputs retain the source filename extension.
 *
 * All destination names are preflighted before the first write. Unless
 * @p options permits overwrite, any collision fails the operation without
 * creating partial output. The extension follows the detected family
 * (`.docx`, `.xlsx`, or `.pptx`).
 *
 * @param inputFile Existing OPC Office package.
 * @param outputDirectory Directory created when necessary.
 * @param options Boundary, grouping, naming, and overwrite behavior.
 * @return Detailed result containing detected family, ordered output paths,
 *         and actionable diagnostics.
 */
EXYOKIOFFICE_EXPORT DocumentSplitResult SplitDocument(
    const std::filesystem::path& inputFile,
    const std::filesystem::path& outputDirectory,
    const DocumentSplitOptions& options = {});

/**
 * @brief Merges same-family Word, Excel, or PowerPoint packages in input order.
 *
 * Word imports body content with dependency and identifier remapping. Excel
 * appends worksheets and imports each worksheet's complete OPC relationship
 * graph; shared strings are remapped, while styled sheets require equivalent
 * workbook style catalogs to avoid corrupt style-index interpretation.
 * PowerPoint imports complete slide graphs, including layouts, masters, themes,
 * notes, media, charts, and embedded packages supported by CopySlideFrom().
 *
 * Mixed document families are rejected before output is saved. Existing output
 * is protected unless Overwrite is true. Input packages are never modified.
 *
 * @param inputFiles One or more source packages in desired merge order.
 * @param outputFile Destination package path.
 * @param options Overwrite and Word-specific separator/style behavior.
 * @return Merge counts, detected family, destination, and diagnostics.
 */
EXYOKIOFFICE_EXPORT DocumentMergeResult MergeDocuments(
    const std::vector<std::filesystem::path>& inputFiles,
    const std::filesystem::path& outputFile,
    const DocumentMergeOptions& options = {});

} // namespace ExyokiOffice::Tools
