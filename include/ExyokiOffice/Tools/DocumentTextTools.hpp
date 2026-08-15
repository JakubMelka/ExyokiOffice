// Copyright (c) 2026 Jakub Melka and Contributors
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#pragma once

#include "ExyokiOffice/Export.hpp"
#include "ExyokiOffice/Tools/DocumentEditors.hpp"
#include "ExyokiOffice/Tools/PackageModel.hpp"
#include "ExyokiOffice/StandardTypes.hpp"

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace ExyokiOffice::Tools
{

/// One search hit inside a Word, Excel, or PowerPoint document.
struct EXYOKIOFFICE_EXPORT DocumentSearchMatch
{
    /// Human-readable location. Word: "body: body paragraph 1"; Excel:
    /// "Sheet1!B2"; PowerPoint: "slide 1 shape 2 paragraph 1" or "slide 1 notes".
    std::string Label;
    /// Character offset of the match within the containing text unit
    /// (paragraph, cell, or notes page).
    Size Offset = 0;
    Size Length = 0;
    std::string MatchText;
    /// Surrounding text (up to the requested number of characters on each side).
    std::string Context;
};

/// Result of a family-aware document text search.
struct EXYOKIOFFICE_EXPORT DocumentSearchResult
{
    bool Ok = false;
    DocumentFamily Family = DocumentFamily::Unknown;
    std::vector<DocumentSearchMatch> Matches;
    std::vector<ToolDiagnostic> Diagnostics;
};

/// Result of a family-aware document text replacement.
struct EXYOKIOFFICE_EXPORT DocumentReplaceResult
{
    bool Ok = false;
    DocumentFamily Family = DocumentFamily::Unknown;
    Size ReplacementCount = 0;
    /// Excel only: matches found in non-text cells (numbers, dates, booleans,
    /// errors, formulas) that were deliberately not replaced.
    Size SkippedMatches = 0;
    /// True when the modified document was written to disk (always false for dry runs).
    bool Saved = false;
    std::vector<ToolDiagnostic> Diagnostics;
};

/**
 * @brief Searches the readable text of a Word, Excel, or PowerPoint package.
 *
 * The package family is detected from the main OPC part and the search is
 * dispatched accordingly:
 *
 * - **Word** — delegates to Search() from WordTextTools.hpp: every paragraph
 *   in the body, tables, headers, footers, footnotes, endnotes, and comments.
 * - **Excel** — every stored non-blank cell of every worksheet, with
 *   shared-string cells resolved to their text. Number, boolean, date, and
 *   formula cells are matched by their stored value text.
 * - **PowerPoint** — every text-frame paragraph of every slide shape, plus
 *   each slide's speaker-notes text.
 *
 * Matching never crosses a paragraph, cell, or notes-page boundary. When
 * @p useRegex is true, @p needle is compiled as an ECMAScript regular
 * expression; otherwise it is matched as a literal substring. @p ignoreCase
 * makes either mode case-insensitive.
 *
 * Returns Ok = false with a diagnostic when the package cannot be opened,
 * the family is unknown, @p needle is empty, or a requested regex is invalid.
 */
EXYOKIOFFICE_EXPORT DocumentSearchResult SearchDocumentText(const std::filesystem::path& path,
                                                            std::string_view needle,
                                                            Size contextChars = 40,
                                                            bool useRegex = false,
                                                            bool ignoreCase = false);

/**
 * @brief Replaces text across a Word, Excel, or PowerPoint package.
 *
 * The scope per family matches SearchDocumentText() with one Excel exception:
 * only text cells (shared or inline strings) are rewritten. Matches inside
 * number, boolean, date, error, or formula cells are never touched, because
 * rewriting them would silently change the cell type; when such matches
 * exist, a warning diagnostic reports how many were skipped.
 *
 * PowerPoint replacements keep run formatting intact for text around the
 * match; a match spanning several runs keeps the formatting of the run the
 * match starts in.
 *
 * When @p useRegex is true, @p replacement may reference capture groups as
 * $1, $2, ... (and $&, $`, $', $$); otherwise both needle and replacement are
 * literal text. When @p dryRun is true, matches are only counted and the
 * document is never modified or saved. Otherwise the result is saved to
 * @p outputPath (or back to @p path when @p outputPath is empty).
 */
EXYOKIOFFICE_EXPORT DocumentReplaceResult ReplaceDocumentText(const std::filesystem::path& path,
                                                              std::string_view needle,
                                                              std::string_view replacement,
                                                              bool dryRun,
                                                              const std::filesystem::path& outputPath = {},
                                                              bool useRegex = false,
                                                              bool ignoreCase = false);

// The same two over a document that is already open. The path overloads detect
// the family, open the editor and delegate here; a caller holding an editor
// pays for neither. Replace never saves in this shape - the edits are left in
// @p editor and writing them out is the caller's decision, so Saved stays
// false and there is no output path to pass.

EXYOKIOFFICE_EXPORT DocumentSearchResult SearchDocumentText(Word::WordDocumentEditor& editor,
                                                            std::string_view needle,
                                                            Size contextChars = 40,
                                                            bool useRegex = false,
                                                            bool ignoreCase = false);
EXYOKIOFFICE_EXPORT DocumentSearchResult SearchDocumentText(Excel::ExcelDocumentEditor& editor,
                                                            std::string_view needle,
                                                            Size contextChars = 40,
                                                            bool useRegex = false,
                                                            bool ignoreCase = false);
EXYOKIOFFICE_EXPORT DocumentSearchResult SearchDocumentText(PowerPoint::PowerPointDocumentEditor& editor,
                                                            std::string_view needle,
                                                            Size contextChars = 40,
                                                            bool useRegex = false,
                                                            bool ignoreCase = false);

EXYOKIOFFICE_EXPORT DocumentReplaceResult ReplaceDocumentText(Word::WordDocumentEditor& editor,
                                                              std::string_view needle,
                                                              std::string_view replacement,
                                                              bool dryRun,
                                                              bool useRegex = false,
                                                              bool ignoreCase = false);
EXYOKIOFFICE_EXPORT DocumentReplaceResult ReplaceDocumentText(Excel::ExcelDocumentEditor& editor,
                                                              std::string_view needle,
                                                              std::string_view replacement,
                                                              bool dryRun,
                                                              bool useRegex = false,
                                                              bool ignoreCase = false);
EXYOKIOFFICE_EXPORT DocumentReplaceResult ReplaceDocumentText(PowerPoint::PowerPointDocumentEditor& editor,
                                                              std::string_view needle,
                                                              std::string_view replacement,
                                                              bool dryRun,
                                                              bool useRegex = false,
                                                              bool ignoreCase = false);

} // namespace ExyokiOffice::Tools
