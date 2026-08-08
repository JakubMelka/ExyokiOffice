// Copyright (c) 2026 Jakub Melka and Collaborators
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#pragma once

#include "ExyokiOffice/Export.hpp"
#include "ExyokiOffice/Tools/DocumentEditors.hpp"
#include "ExyokiOffice/Tools/DocumentModel.hpp"

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace ExyokiOffice::Tools
{

/// Options shared by the family model readers.
struct EXYOKIOFFICE_EXPORT ModelReadOptions
{
    /// When false, media payload bytes are not loaded (references remain).
    bool IncludeMediaData = true;
};

// --- Office package <-> model -----------------------------------------------
//
// Readers open a package via the family's high-level editor and build the
// semantic model; constructs the editors do not model are skipped with a
// Warning diagnostic. Writers create a fresh package from the model. A failed
// read returns a model whose Family stays Unknown.

EXYOKIOFFICE_EXPORT DocumentModel ReadWordModel(const std::filesystem::path& path,
                                                const ModelReadOptions& options,
                                                std::vector<ToolDiagnostic>& diagnostics);
EXYOKIOFFICE_EXPORT DocumentModel ReadExcelModel(const std::filesystem::path& path,
                                                 const ModelReadOptions& options,
                                                 std::vector<ToolDiagnostic>& diagnostics);
EXYOKIOFFICE_EXPORT DocumentModel ReadPowerPointModel(const std::filesystem::path& path,
                                                      const ModelReadOptions& options,
                                                      std::vector<ToolDiagnostic>& diagnostics);

// The same readers over a document that is already open. The path overloads
// above are thin wrappers over these: they open the editor and delegate. Use
// these when you hold the editor — a server keeping documents open between
// requests, an application that has just edited one — because writing the
// document out only to read it back costs a full serialize and reparse, and
// the round trip reports the document as it was last written rather than as it
// currently stands.

EXYOKIOFFICE_EXPORT DocumentModel ReadWordModel(Word::WordDocumentEditor& editor,
                                                const ModelReadOptions& options,
                                                std::vector<ToolDiagnostic>& diagnostics);
EXYOKIOFFICE_EXPORT DocumentModel ReadExcelModel(Excel::ExcelDocumentEditor& editor,
                                                 const ModelReadOptions& options,
                                                 std::vector<ToolDiagnostic>& diagnostics);
EXYOKIOFFICE_EXPORT DocumentModel ReadPowerPointModel(PowerPoint::PowerPointDocumentEditor& editor,
                                                      const ModelReadOptions& options,
                                                      std::vector<ToolDiagnostic>& diagnostics);

EXYOKIOFFICE_EXPORT bool WriteWordModel(const DocumentModel& model,
                                        const std::filesystem::path& path,
                                        std::vector<ToolDiagnostic>& diagnostics);
EXYOKIOFFICE_EXPORT bool WriteExcelModel(const DocumentModel& model,
                                         const std::filesystem::path& path,
                                         std::vector<ToolDiagnostic>& diagnostics);
EXYOKIOFFICE_EXPORT bool WritePowerPointModel(const DocumentModel& model,
                                              const std::filesystem::path& path,
                                              std::vector<ToolDiagnostic>& diagnostics);

// --- Model <-> text formats -------------------------------------------------

/// Serializes the model as the canonical JSON envelope. With embedMedia, media
/// bytes are base64-encoded into the envelope; otherwise only file references
/// are written (see DocumentConverter for the media directory workflow).
EXYOKIOFFICE_EXPORT std::string SerializeModelJson(const DocumentModel& model, bool embedMedia);
EXYOKIOFFICE_EXPORT DocumentModel ParseModelJson(std::string_view json,
                                                 std::vector<ToolDiagnostic>& diagnostics);

/// Serializes the model as the semantic XML envelope (same tree as JSON).
EXYOKIOFFICE_EXPORT std::string SerializeModelXml(const DocumentModel& model, bool embedMedia);
EXYOKIOFFICE_EXPORT DocumentModel ParseModelXml(std::string_view xml,
                                                std::vector<ToolDiagnostic>& diagnostics);

/// Renders the model as structure-preserving Markdown (lossy; see docs/tools/conversion-formats.md).
EXYOKIOFFICE_EXPORT std::string SerializeModelMarkdown(const DocumentModel& model,
                                                       std::vector<ToolDiagnostic>& diagnostics);
/// Parses Markdown into a model for the requested target family.
EXYOKIOFFICE_EXPORT DocumentModel ParseModelMarkdown(std::string_view markdown,
                                                     DocumentFamily targetFamily,
                                                     std::vector<ToolDiagnostic>& diagnostics);

/// Renders the model as plain text (render-only for Excel/PowerPoint).
EXYOKIOFFICE_EXPORT std::string SerializeModelText(const DocumentModel& model);
/// Parses plain text into a model (Word only: one paragraph per non-empty line).
EXYOKIOFFICE_EXPORT DocumentModel ParseModelText(std::string_view text,
                                                 DocumentFamily targetFamily,
                                                 std::vector<ToolDiagnostic>& diagnostics);

/// Options for the CSV serializer and parser (Excel family only).
struct EXYOKIOFFICE_EXPORT CsvOptions
{
    /// Field separator written between cells and expected between input fields.
    std::string Separator = ",";
    /// Serializing: worksheet to export (case-insensitive; empty = the first
    /// sheet, with a warning when the workbook has more than one).
    /// Parsing: name of the single created worksheet (empty = "Sheet1").
    std::string SheetName;
};

/**
 * @brief Renders one worksheet of an Excel model as RFC 4180 CSV.
 *
 * Rows are emitted from row 1 to the last stored row; unstored cells become
 * empty fields. Formula cells emit their cached result (never the formula
 * text), booleans emit TRUE/FALSE, and every other cell emits its canonical
 * value text. Fields containing the separator, quotes, or line breaks are
 * quoted; rows end with CRLF. Returns an empty string with an error
 * diagnostic when the model is not an Excel model or the sheet is missing.
 */
EXYOKIOFFICE_EXPORT std::string SerializeModelCsv(const DocumentModel& model,
                                                  const CsvOptions& options,
                                                  std::vector<ToolDiagnostic>& diagnostics);

/**
 * @brief Parses RFC 4180 CSV into a single-worksheet Excel model.
 *
 * Quoted fields, doubled quotes, and CRLF/LF row breaks are handled. Cell
 * types are inferred: TRUE/FALSE (case-insensitive) become booleans, plain
 * decimal numbers (no leading zeros) become numbers, and everything else —
 * including values starting with '=' — is imported as text; formulas are
 * never evaluated or created. Empty fields produce no stored cell.
 */
EXYOKIOFFICE_EXPORT DocumentModel ParseModelCsv(std::string_view csv,
                                                const CsvOptions& options,
                                                std::vector<ToolDiagnostic>& diagnostics);

} // namespace ExyokiOffice::Tools
