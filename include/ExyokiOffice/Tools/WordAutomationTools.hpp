// Copyright (c) 2026 Jakub Melka and Contributors
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#pragma once

#include "ExyokiOffice/Export.hpp"
#include "ExyokiOffice/Tools/PackageModel.hpp"
#include "ExyokiOffice/StandardTypes.hpp"

#include <filesystem>
#include <string>
#include <vector>

namespace ExyokiOffice::Tools
{

/** @brief Counts and outcome reported by FillWordTemplate(). */
struct EXYOKIOFFICE_EXPORT TemplateFillResult
{
    bool Ok = false;
    /// Scalar MERGEFIELD results replaced.
    Size FieldsMerged = 0;
    /// Same-paragraph bookmarks replaced.
    Size BookmarksMerged = 0;
    /// TableStart/TableEnd repeating regions expanded.
    Size RegionsMerged = 0;
    /// Row copies inserted across all repeating regions.
    Size RegionRowsInserted = 0;
    /// True when the filled document was written to disk.
    bool Saved = false;
    std::vector<ToolDiagnostic> Diagnostics;
};

/**
 * @brief Fills a Word mail-merge template from a JSON data file.
 *
 * The JSON root must be an object. String, number, boolean, and null members
 * become scalar values merged into matching `MERGEFIELD Name` fields and
 * same-paragraph bookmarks (null merges as empty text). A member whose value
 * is an array of objects becomes a repeating region driven by
 * `MERGEFIELD TableStart:Name` / `MERGEFIELD TableEnd:Name` markers, one copy
 * per array element. Members of any other shape are skipped with a warning.
 *
 * The merge is literal: no expression language is evaluated and the data
 * never triggers external access — see WordDocumentEditor::MergeTemplate()
 * for the exact merge semantics this delegates to.
 *
 * The filled document is saved to @p outputPath, or back to @p docxPath when
 * @p outputPath is empty (the template is then overwritten).
 */
EXYOKIOFFICE_EXPORT TemplateFillResult FillWordTemplate(const std::filesystem::path& docxPath,
                                                        const std::filesystem::path& dataJsonPath,
                                                        const std::filesystem::path& outputPath = {});

/** @brief Outcome of a semantic Word document comparison. */
struct EXYOKIOFFICE_EXPORT WordCompareResult
{
    bool Ok = false;
    /// Tracked-revision containers created by the comparison.
    Size RevisionsCreated = 0;
    /// True when the two documents produced no revisions (equal paragraph text).
    bool Identical = false;
    std::filesystem::path OutputFile;
    std::vector<ToolDiagnostic> Diagnostics;
};

/**
 * @brief Compares two Word documents and writes a tracked-revisions result.
 *
 * Delegates to WordDocumentEditor::CompareWith(): the original document is
 * annotated with tracked deletions for paragraphs missing from the revised
 * document and tracked insertions for paragraphs present only there, then
 * saved to @p outputPath. The comparison is paragraph-level plain text — a
 * conservative compatibility stage, not Word's full diff engine. Neither
 * input file is modified.
 *
 * @param author Author name written to the generated revision metadata.
 */
EXYOKIOFFICE_EXPORT WordCompareResult CompareWordDocuments(const std::filesystem::path& originalPath,
                                                           const std::filesystem::path& revisedPath,
                                                           const std::filesystem::path& outputPath,
                                                           const std::string& author = "exyoki");

} // namespace ExyokiOffice::Tools
