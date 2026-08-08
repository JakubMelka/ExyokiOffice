// Copyright (c) 2026 Jakub Melka and Collaborators
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#pragma once

#include "ExyokiOffice/Export.hpp"
#include "ExyokiOffice/Tools/DocumentEditors.hpp"
#include "ExyokiOffice/Tools/PackageModel.hpp"

#include <filesystem>
#include <string>
#include <vector>

namespace ExyokiOffice::Tools
{

/// One labeled block of extracted text (a paragraph, a shape, a worksheet cell, ...).
struct EXYOKIOFFICE_EXPORT ExtractedTextBlock
{
    std::string Label;
    std::string Text;
};

/// Result of Extract(): every family (Word, Excel, PowerPoint) is dispatched to its own extractor.
struct EXYOKIOFFICE_EXPORT ExtractedDocumentText
{
    bool Ok = false;
    DocumentFamily Family = DocumentFamily::Unknown;
    std::vector<ExtractedTextBlock> Blocks;
    std::vector<ToolDiagnostic> Diagnostics;
};

/**
 * @brief Extracts all readable text from a Word, Excel, or PowerPoint package.
 *
 * Word documents are extracted via WordTextTools::ExtractText (body, tables,
 * headers/footers, footnotes/endnotes, comments). PowerPoint presentations
 * are extracted shape-by-shape and slide-by-slide via
 * PowerPointDocumentEditor, labeled "slide N shape M" (plus "slide N notes"
 * when present). Excel workbooks are extracted cell-by-cell via
 * ExcelDocumentEditor, labeled "SheetName!A1", resolving shared strings
 * through SharedStringTableService.
 */
EXYOKIOFFICE_EXPORT ExtractedDocumentText Extract(const std::filesystem::path& path);

// The same extraction over a document that is already open; the overload above
// detects the family and then does exactly this.

EXYOKIOFFICE_EXPORT ExtractedDocumentText Extract(Word::WordDocumentEditor& editor);
EXYOKIOFFICE_EXPORT ExtractedDocumentText Extract(Excel::ExcelDocumentEditor& editor);
EXYOKIOFFICE_EXPORT ExtractedDocumentText Extract(PowerPoint::PowerPointDocumentEditor& editor);

} // namespace ExyokiOffice::Tools
