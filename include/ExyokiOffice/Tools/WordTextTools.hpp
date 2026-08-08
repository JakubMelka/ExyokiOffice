// Copyright (c) 2026 Jakub Melka and Collaborators
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#pragma once

#include "ExyokiOffice/Export.hpp"
#include "ExyokiOffice/Tools/DocumentEditors.hpp"
#include "ExyokiOffice/Tools/PackageModel.hpp"
#include "ExyokiOffice/StandardTypes.hpp"

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace ExyokiOffice::Tools
{

/// Where a piece of Word text was found.
enum class TextScope
{
    Body,
    Table,
    Header,
    Footer,
    Footnote,
    Endnote,
    Comment
};

EXYOKIOFFICE_EXPORT std::string_view ToString(TextScope scope) noexcept;

/// One search hit inside a Word document.
struct EXYOKIOFFICE_EXPORT SearchMatch
{
    TextScope Scope = TextScope::Body;
    /// Human-readable location, e.g. "table 1 paragraph 2", "footnote 3", "comment 1 (Reviewer)".
    std::string Label;
    /// Character offset of the match within the containing paragraph's plain text.
    Size Offset = 0;
    Size Length = 0;
    std::string MatchText;
    /// Surrounding text (up to the requested number of characters on each side).
    std::string Context;
};

struct EXYOKIOFFICE_EXPORT SearchResult
{
    bool Ok = false;
    std::vector<SearchMatch> Matches;
    std::vector<ToolDiagnostic> Diagnostics;
};

/// One block of extracted plain text.
struct EXYOKIOFFICE_EXPORT TextBlock
{
    TextScope Scope = TextScope::Body;
    std::string Label;
    std::string Text;
};

struct EXYOKIOFFICE_EXPORT ExtractTextResult
{
    bool Ok = false;
    std::vector<TextBlock> Blocks;
    std::vector<ToolDiagnostic> Diagnostics;
};

struct EXYOKIOFFICE_EXPORT ReplaceResult
{
    bool Ok = false;
    Size ReplacementCount = 0;
    /// True when the modified document was written to disk (always false for dry runs).
    bool Saved = false;
    std::vector<ToolDiagnostic> Diagnostics;
};

/**
 * @brief Searches every paragraph in a Word document (body, tables, headers,
 * footers, footnotes, endnotes, and comments) for a substring or regex match.
 *
 * When @p useRegex is true, @p needle is compiled as an ECMAScript regular
 * expression (the std::regex default grammar); otherwise it is matched as a
 * literal substring. @p ignoreCase makes either mode case-insensitive.
 *
 * Returns Ok = false with a diagnostic when @p docxPath is not a Word
 * document, cannot be opened, or (when @p useRegex or @p ignoreCase is set)
 * @p needle is not a valid regular expression.
 */
EXYOKIOFFICE_EXPORT SearchResult Search(const std::filesystem::path& docxPath, std::string_view needle,
                                        Size contextChars = 40, bool useRegex = false,
                                        bool ignoreCase = false);

/// Extracts the plain text of every paragraph, grouped by scope, in document order.
EXYOKIOFFICE_EXPORT ExtractTextResult ExtractText(const std::filesystem::path& docxPath);

/**
 * @brief Replaces every occurrence of a substring or regex match across the whole document.
 *
 * When @p useRegex is true, @p needle is compiled as an ECMAScript regular
 * expression and @p replacement may reference capture groups as $1, $2, ...
 * (and $&, $`, $', $$) via std::match_results::format; otherwise both are
 * treated as literal text. @p ignoreCase makes either mode case-insensitive.
 *
 * When @p dryRun is true, only counts matches (via Paragraph::FindAll /
 * FindAllRegex) and never modifies or saves the document. Otherwise applies
 * Paragraph::ReplaceAll / ReplaceAllRegex everywhere and saves to
 * @p outputPath (or back to @p docxPath when @p outputPath is empty).
 *
 * Returns Ok = false with a diagnostic when (with @p useRegex or
 * @p ignoreCase set) @p needle is not a valid regular expression.
 */
EXYOKIOFFICE_EXPORT ReplaceResult Replace(const std::filesystem::path& docxPath, std::string_view needle,
                                          std::string_view replacement, bool dryRun,
                                          const std::filesystem::path& outputPath = {}, bool useRegex = false,
                                          bool ignoreCase = false);

// The same three over a document that is already open. The path overloads are
// wrappers: they check the family, open the editor, delegate here, and — for
// Replace — save afterwards.

/// @copydoc Search
EXYOKIOFFICE_EXPORT SearchResult Search(Word::WordDocumentEditor& editor, std::string_view needle,
                                        Size contextChars = 40, bool useRegex = false,
                                        bool ignoreCase = false);

/// @copydoc ExtractText
EXYOKIOFFICE_EXPORT ExtractTextResult ExtractText(Word::WordDocumentEditor& editor);

/**
 * @brief Replaces across a document that is already open, without saving it.
 *
 * There is no output path because nothing is written: the replacements are
 * applied to @p editor and Saved stays false. Saving, or throwing the edits
 * away, is the caller's call. @p dryRun still only counts.
 */
EXYOKIOFFICE_EXPORT ReplaceResult Replace(Word::WordDocumentEditor& editor, std::string_view needle,
                                          std::string_view replacement, bool dryRun, bool useRegex = false,
                                          bool ignoreCase = false);

} // namespace ExyokiOffice::Tools
