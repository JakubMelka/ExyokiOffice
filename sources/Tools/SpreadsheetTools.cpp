// Copyright (c) 2026 Jakub Melka and Contributors
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "ExyokiOffice/Tools/SpreadsheetTools.hpp"

#include "ExyokiOffice/Excel/ExcelDocument.hpp"
#include "ExyokiOffice/Excel/ExcelFormulaEngine.hpp"
#include "ExyokiOffice/Tools/PackageLimits.hpp"
#include "ExyokiOffice/StandardTypes.hpp"

#include <string>
#include <utility>

namespace ExyokiOffice::Tools
{

WorkbookRecalcResult RecalculateWorkbook(const std::filesystem::path& xlsxPath, std::string_view sheetName,
                                         const std::filesystem::path& outputPath, bool dryRun)
{
    WorkbookRecalcResult result;

    auto editor = Excel::ExcelDocumentEditor::Open(xlsxPath, UntrustedOpenSettings());
    if (!editor)
    {
        result.Diagnostics.push_back(
            ToolDiagnostic{ToolSeverity::Error, "Failed to open Excel document", xlsxPath.string()});
        return result;
    }

    Excel::FormulaEngine engine(editor->GetDocument());
    const auto recalculation =
        sheetName.empty() ? engine.Recalculate() : engine.RecalculateSheet(sheetName);
    if (!recalculation)
    {
        result.Diagnostics.push_back(ToolDiagnostic{ToolSeverity::Error, "Recalculation failed",
                                                    recalculation.Status.Message});
        return result;
    }

    result.RecalculatedCellCount = recalculation.RecalculatedCellCount;
    for (const auto& cycle : recalculation.CircularReferenceCycles)
    {
        std::string formatted;
        for (const auto& member : cycle)
        {
            if (!formatted.empty())
            {
                formatted += " -> ";
            }
            formatted += member.ToFormula();
        }
        result.CircularReferenceCycles.push_back(std::move(formatted));
    }
    if (!result.CircularReferenceCycles.empty())
    {
        result.Diagnostics.push_back(ToolDiagnostic{
            ToolSeverity::Warning,
            std::to_string(result.CircularReferenceCycles.size()) +
                " circular reference cycle(s) found; their cells keep the previous cached values"});
    }

    if (!dryRun)
    {
        const auto destination = outputPath.empty() ? xlsxPath : outputPath;
        if (!editor->SaveToFile(destination))
        {
            result.Diagnostics.push_back(
                ToolDiagnostic{ToolSeverity::Error, "Failed to save workbook", destination.string()});
            return result;
        }
        result.Saved = true;
    }

    result.Ok = true;
    return result;
}

} // namespace ExyokiOffice::Tools
