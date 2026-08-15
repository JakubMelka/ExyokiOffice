// Copyright (c) 2026 Jakub Melka and Contributors
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#pragma once

#include "ExyokiOffice/Export.hpp"
#include "ExyokiOffice/Tools/DocumentEditors.hpp"
#include "ExyokiOffice/Tools/PackageModel.hpp"
#include "ExyokiOffice/StandardTypes.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <vector>

namespace ExyokiOffice::Tools
{

/**
 * @brief Aggregate content statistics for a Word, Excel, or PowerPoint package.
 *
 * Every family reports Ok/Family/Diagnostics; the remaining fields are
 * populated only for the family they apply to (e.g. SlideCount is
 * PowerPoint-only, WorksheetCount is Excel-only) and left as std::nullopt
 * otherwise. Fields that share a meaning across families (ImageCount,
 * TableCount, HyperlinkCount, CommentCount) reuse the same member rather than
 * duplicating one per family.
 */
struct EXYOKIOFFICE_EXPORT DocumentStats
{
    bool Ok = false;
    DocumentFamily Family = DocumentFamily::Unknown;
    std::vector<ToolDiagnostic> Diagnostics;

    // Word: body + table-cell paragraphs (headers/footers/notes are excluded,
    // matching Word's own default word-count scope).
    std::optional<UInt64> WordCount;
    std::optional<UInt64> CharacterCount;
    std::optional<UInt64> ParagraphCount;
    std::optional<UInt64> HeadingCount;
    std::optional<UInt64> EquationCount;
    std::optional<UInt64> FootnoteCount;
    std::optional<UInt64> EndnoteCount;
    std::optional<UInt64> BookmarkCount;
    std::optional<UInt64> SectionCount;

    // Excel
    std::optional<UInt64> WorksheetCount;
    std::optional<UInt64> CellCount;
    std::optional<UInt64> FormulaCount;
    std::optional<UInt64> MergedRangeCount;

    // PowerPoint
    std::optional<UInt64> SlideCount;
    std::optional<UInt64> HiddenSlideCount;
    std::optional<UInt64> SlideWithNotesCount;
    std::optional<UInt64> ShapeCount;
    std::optional<UInt64> ChartCount;

    // Shared across families (Word paragraphs+tables; Excel worksheets; PowerPoint shape trees).
    std::optional<UInt64> TableCount;
    std::optional<UInt64> ImageCount;
    std::optional<UInt64> HyperlinkCount;
    std::optional<UInt64> CommentCount;

    /// Estimated reading time in minutes at 200 words/minute (Word and PowerPoint only).
    std::optional<Real> ReadingTimeMinutes;
};

/**
 * @brief Computes aggregate content statistics for a Word, Excel, or PowerPoint package.
 *
 * Dispatches on DocumentFamily like Extract()/GetInfo(). Word statistics walk
 * body and table paragraphs (recursing into nested tables); headings are
 * detected from paragraph style IDs equal to "Title" or starting with
 * "Heading", and equations are counted as `m:oMath` descendants. Excel
 * statistics walk every worksheet's stored cells, formulas, tables,
 * hyperlinks, comments, and images. PowerPoint statistics walk every slide's
 * shape tree recursively through groups, counting words, shapes, images,
 * tables, charts, and hyperlinks.
 */
EXYOKIOFFICE_EXPORT DocumentStats Stat(const std::filesystem::path& path);

// The same statistics over a document that is already open. The overload above
// detects the family and then does exactly this; a caller holding an editor
// skips both the detection and the reopen. Family is always set, so a caller
// need not ask which overload answered.

EXYOKIOFFICE_EXPORT DocumentStats Stat(Word::WordDocumentEditor& editor);
EXYOKIOFFICE_EXPORT DocumentStats Stat(Excel::ExcelDocumentEditor& editor);
EXYOKIOFFICE_EXPORT DocumentStats Stat(PowerPoint::PowerPointDocumentEditor& editor);

} // namespace ExyokiOffice::Tools
