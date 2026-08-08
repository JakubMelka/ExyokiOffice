// Copyright (c) 2026 Jakub Melka and Collaborators
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#pragma once

#include "ExyokiOffice/Export.hpp"
#include "ExyokiOffice/Tools/PackageModel.hpp"
#include "ExyokiOffice/StandardTypes.hpp"

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace ExyokiOffice::Tools
{

/** @brief Outcome of a workbook recalculation. */
struct EXYOKIOFFICE_EXPORT WorkbookRecalcResult
{
    bool Ok = false;
    /// Formula cells whose cached result was recomputed.
    Size RecalculatedCellCount = 0;
    /// Circular-reference cycles, each formatted as "Sheet1!A1 -> Sheet1!B2".
    /// Cycle members keep their previous cached values.
    std::vector<std::string> CircularReferenceCycles;
    /// True when the recalculated workbook was written to disk.
    bool Saved = false;
    std::vector<ToolDiagnostic> Diagnostics;
};

/**
 * @brief Recalculates every formula cell of a workbook (or one worksheet) and
 * rewrites the cached results.
 *
 * Delegates to Excel::FormulaEngine: formulas are evaluated in dependency
 * order with Excel-compatible semantics, so the workbook shows up-to-date
 * values when opened in a spreadsheet application. Worksheet errors such as
 * `#DIV/0!` are ordinary results, not failures. Circular references are
 * reported and their cells keep their previous cached values.
 *
 * @param sheetName Recalculate only this worksheet (case-insensitive); an
 *        empty name recalculates the whole workbook.
 * @param outputPath Destination; empty rewrites @p xlsxPath in place.
 * @param dryRun Evaluate and report without saving anything.
 */
EXYOKIOFFICE_EXPORT WorkbookRecalcResult RecalculateWorkbook(const std::filesystem::path& xlsxPath,
                                                             std::string_view sheetName = {},
                                                             const std::filesystem::path& outputPath = {},
                                                             bool dryRun = false);

} // namespace ExyokiOffice::Tools
