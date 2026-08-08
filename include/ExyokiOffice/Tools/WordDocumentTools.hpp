// Copyright (c) 2026 Jakub Melka and Collaborators
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#pragma once

#include "ExyokiOffice/Export.hpp"
#include "ExyokiOffice/Tools/PackageModel.hpp"
#include "ExyokiOffice/Word/WordDocument.hpp"
#include "ExyokiOffice/StandardTypes.hpp"

#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

namespace ExyokiOffice::Tools
{

/** @brief Structural boundary used when splitting a WordprocessingML body. */
enum class WordSplitStrategy
{
    /** Finish a part after a paragraph containing `w:pPr/w:sectPr`. */
    SectionBreaks,
    /** Split at explicit/rendered page breaks or `pageBreakBefore`. */
    PageBreaks,
    /** Finish a part after a fixed number of body paragraphs. */
    ParagraphCount,
    /** Start a new part at each paragraph containing an exact text marker. */
    Marker
};

/** @brief Word-specific split configuration used by SplitWordDocument(). */
struct EXYOKIOFFICE_EXPORT WordSplitOptions
{
    WordSplitStrategy Strategy = WordSplitStrategy::SectionBreaks;
    /** Required and greater than zero for ParagraphCount; ignored otherwise. */
    Size ParagraphsPerDocument = 0;
    /** Required and non-empty for Marker; matching is literal and case-sensitive. */
    std::string Marker;
    /** Prefix for numbered `.docx` files; defaults to `part`. */
    std::string OutputPrefix = "part";
    /** Allow replacement of existing numbered output files. */
    bool Overwrite = false;
};

/** @brief Outcome and ordered file list produced by SplitWordDocument(). */
struct EXYOKIOFFICE_EXPORT WordSplitResult
{
    bool Ok = false;
    std::vector<std::filesystem::path> OutputFiles;
    std::vector<ToolDiagnostic> Diagnostics;
};

/** @brief Word-specific merge configuration used by MergeWordDocuments(). */
struct EXYOKIOFFICE_EXPORT WordMergeOptions
{
    /** Insert an explicit page-break paragraph between consecutive inputs. */
    bool InsertPageBreaks = true;
    /** Allow replacement of an existing output package. */
    bool Overwrite = false;
    /** Policy used when imported body content references a colliding style ID. */
    Word::StyleCopyConflictPolicy StyleConflictPolicy = Word::StyleCopyConflictPolicy::Rename;
};

/** @brief Outcome and input count produced by MergeWordDocuments(). */
struct EXYOKIOFFICE_EXPORT WordMergeResult
{
    bool Ok = false;
    Size DocumentsMerged = 0;
    std::filesystem::path OutputFile;
    std::vector<ToolDiagnostic> Diagnostics;
};

/**
 * @brief Splits a Word package into numbered `.docx` files.
 *
 * Boundaries are structural Open XML markers, not renderer-computed visual
 * pages. Each slice is imported through `BodyCursor::InsertDocument`, which
 * deep-copies body blocks and remaps referenced styles, numbering, bookmarks,
 * notes, comments, content-control IDs, images, and hyperlinks. The source is
 * opened read-only and is never modified.
 *
 * Every destination is checked before writing begins when overwrite is false.
 * An empty body, invalid strategy parameter, wrong package family, I/O failure,
 * or dependency-import failure returns `Ok == false` with diagnostics.
 *
 * @param inputFile Existing WordprocessingML package.
 * @param outputDirectory Directory created when necessary.
 * @param options Boundary, naming, and overwrite behavior.
 * @return Ordered output paths and diagnostic details.
 * @note PageBreaks recognizes explicit `w:br type="page"`, Word's
 *       `w:lastRenderedPageBreak`, and paragraph `pageBreakBefore`; it cannot
 *       infer pagination that would require font metrics and a layout engine.
 */
EXYOKIOFFICE_EXPORT WordSplitResult SplitWordDocument(
    const std::filesystem::path& inputFile,
    const std::filesystem::path& outputDirectory,
    const WordSplitOptions& options = {});

/**
 * @brief Merges Word packages in input order into one new `.docx` file.
 *
 * Body content is independently deep-copied through
 * `WordDocumentEditor::BodyCursor::InsertDocument`. Referenced style IDs,
 * numbering instances, bookmarks, footnotes, endnotes, comments, structured
 * document tags, images, drawing IDs, and internal/external hyperlinks are
 * imported or remapped to avoid target collisions. Optional page-break
 * paragraphs separate source documents.
 *
 * @param inputFiles Non-empty ordered list of Word packages.
 * @param outputFile Destination package. Existing files require Overwrite.
 * @param options Separator, style-conflict, and overwrite behavior.
 * @return Status, successfully incorporated input count, output path, and
 *         diagnostics. Inputs are never modified.
 */
EXYOKIOFFICE_EXPORT WordMergeResult MergeWordDocuments(
    const std::vector<std::filesystem::path>& inputFiles,
    const std::filesystem::path& outputFile,
    const WordMergeOptions& options = {});

} // namespace ExyokiOffice::Tools
