// Copyright (c) 2026 Jakub Melka and Contributors
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#pragma once

#include "ExyokiOffice/Export.hpp"
#include "ExyokiOffice/Tools/DocumentModel.hpp"
#include "ExyokiOffice/Tools/MediaExporter.hpp"
#include "ExyokiOffice/StandardTypes.hpp"

#include <filesystem>
#include <string>
#include <vector>

namespace ExyokiOffice::Tools
{

/// One conversion endpoint format. Auto means "infer from the file extension".
enum class ConvertFormat
{
    Auto,
    Docx,
    Xlsx,
    Pptx,
    Json,
    Markdown,
    Text,
    Xml,
    Csv
};

/// Renders a convert format as its CLI token ("docx", "md", "json", ...).
EXYOKIOFFICE_EXPORT std::string_view ToString(ConvertFormat format) noexcept;
/// Parses a CLI token into a convert format. Unknown tokens yield Auto.
EXYOKIOFFICE_EXPORT ConvertFormat ParseConvertFormat(std::string_view token) noexcept;
/// Infers the convert format from a file path extension. Unknown yields Auto.
EXYOKIOFFICE_EXPORT ConvertFormat InferConvertFormat(const std::filesystem::path& path) noexcept;

struct EXYOKIOFFICE_EXPORT ConvertOptions
{
    /// Input format; Auto infers from the input extension.
    ConvertFormat From = ConvertFormat::Auto;
    /// Output format; Auto infers from the output extension.
    ConvertFormat To = ConvertFormat::Auto;
    /// Media directory for external media (default: "<output stem>_media"
    /// next to the output when exporting, the input directory when importing).
    std::filesystem::path MediaDir;
    /// Embed media as base64 into JSON/XML instead of writing external files.
    bool EmbedMedia = false;
    /// Drop media entirely (a diagnostic reports each dropped item).
    bool NoMedia = false;
    /// Allow overwriting existing media files.
    bool Overwrite = false;
    /// CSV only: field separator (default comma).
    std::string CsvSeparator = ",";
    /// CSV output: worksheet to export (empty = first, with a warning when the
    /// workbook has several). CSV input: name of the created worksheet.
    std::string SheetName;
};

struct EXYOKIOFFICE_EXPORT ConvertResult
{
    bool Ok = false;
    /// False when the failure was invalid usage (bad format pair) rather than an I/O or content error.
    bool UsageOk = true;
    DocumentFamily Family = DocumentFamily::Unknown;
    ConvertFormat From = ConvertFormat::Auto;
    ConvertFormat To = ConvertFormat::Auto;
    /// Converted text payload when the output path is "-" (stdout).
    std::string OutputText;
    Size BlockCount = 0;
    Size SheetCount = 0;
    Size CellCount = 0;
    Size SlideCount = 0;
    /// External media files written next to a text-format output.
    std::vector<MediaExportItem> MediaItems;
    std::vector<ToolDiagnostic> Diagnostics;
};

/**
 * @brief Converts between Office packages (docx/xlsx/pptx) and text formats
 * (Markdown, JSON, plain text, semantic XML).
 *
 * The conversion goes through the semantic DocumentModel; see
 * docs/tools/conversion-formats.md for the exact mapping and fidelity guarantees.
 * Supported pairs: Office -> text format, text format -> Office, and
 * json/xml -> any text format. Office -> Office is rejected as a usage error.
 * An output path of "-" returns the converted text in ConvertResult::OutputText
 * (text-format outputs only).
 */
EXYOKIOFFICE_EXPORT ConvertResult ConvertDocument(const std::filesystem::path& input,
                                                  const std::filesystem::path& output,
                                                  const ConvertOptions& options = {});

} // namespace ExyokiOffice::Tools
