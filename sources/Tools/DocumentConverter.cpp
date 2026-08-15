// Copyright (c) 2026 Jakub Melka and Collaborators
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "ExyokiOffice/Tools/DocumentConverter.hpp"

#include "ExyokiOffice/Tools/DocumentModelIO.hpp"
#include "ExyokiOffice/StandardTypes.hpp"
#include "AsciiText.hpp"

#include <algorithm>
#include <fstream>
#include <sstream>

namespace ExyokiOffice::Tools
{

/// File-local format detection and media helpers behind the converter.
class DocumentConverterHelper
{
public:
    static void Error(std::vector<ToolDiagnostic>& diagnostics, std::string message, std::string context = {})
    {
        diagnostics.push_back(ToolDiagnostic{ToolSeverity::Error, std::move(message), std::move(context)});
    }

    static void Warn(std::vector<ToolDiagnostic>& diagnostics, std::string message, std::string context = {})
    {
        diagnostics.push_back(ToolDiagnostic{ToolSeverity::Warning, std::move(message), std::move(context)});
    }

    static bool IsOfficeFormat(ConvertFormat format)
    {
        return format == ConvertFormat::Docx || format == ConvertFormat::Xlsx || format == ConvertFormat::Pptx;
    }

    static bool IsTextFormat(ConvertFormat format)
    {
        return format == ConvertFormat::Json || format == ConvertFormat::Markdown ||
               format == ConvertFormat::Text || format == ConvertFormat::Xml ||
               format == ConvertFormat::Csv;
    }

    static DocumentFamily FamilyForFormat(ConvertFormat format)
    {
        switch (format)
        {
            case ConvertFormat::Docx:
                return DocumentFamily::Word;
            case ConvertFormat::Xlsx:
                return DocumentFamily::Excel;
            case ConvertFormat::Pptx:
                return DocumentFamily::PowerPoint;
            default:
                return DocumentFamily::Unknown;
        }
    }

    static std::string ExtensionForContentType(const std::string& contentType)
    {
        if (contentType == "image/png")
        {
            return "png";
        }
        if (contentType == "image/jpeg")
        {
            return "jpg";
        }
        if (contentType == "image/gif")
        {
            return "gif";
        }
        if (contentType == "image/bmp")
        {
            return "bmp";
        }
        if (contentType == "image/tiff")
        {
            return "tiff";
        }
        if (contentType == "image/x-emf")
        {
            return "emf";
        }
        if (contentType == "image/x-wmf")
        {
            return "wmf";
        }
        return "bin";
    }

    static std::string ContentTypeForExtension(std::string_view rawExtension)
    {
        const std::string extension = AsciiText::ToLower(rawExtension);
        if (extension == ".png")
        {
            return "image/png";
        }
        if (extension == ".jpg" || extension == ".jpeg")
        {
            return "image/jpeg";
        }
        if (extension == ".gif")
        {
            return "image/gif";
        }
        if (extension == ".bmp")
        {
            return "image/bmp";
        }
        if (extension == ".tiff" || extension == ".tif")
        {
            return "image/tiff";
        }
        if (extension == ".emf")
        {
            return "image/x-emf";
        }
        if (extension == ".wmf")
        {
            return "image/x-wmf";
        }
        return "application/octet-stream";
    }

    static bool ReadTextFile(const std::filesystem::path& path, std::string& text,
                             std::vector<ToolDiagnostic>& diagnostics)
    {
        std::ifstream file(path, std::ios::binary);
        if (!file)
        {
            Error(diagnostics, "Cannot read input file", path.string());
            return false;
        }
        std::ostringstream buffer;
        buffer << file.rdbuf();
        text = buffer.str();
        return true;
    }

    static bool WriteTextFile(const std::filesystem::path& path, const std::string& text,
                              std::vector<ToolDiagnostic>& diagnostics)
    {
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        if (!file)
        {
            Error(diagnostics, "Cannot write output file", path.string());
            return false;
        }
        file << text;
        return true;
    }

    /// Writes model media to the media directory and fills in relative file references.
    static void ExportExternalMedia(DocumentModel& model, const std::filesystem::path& mediaDir, bool overwrite,
                                    ConvertResult& result)
    {
        if (model.Media.empty())
        {
            return;
        }

        std::error_code errorCode;
        std::filesystem::create_directories(mediaDir, errorCode);
        const auto directoryName = mediaDir.filename().string();

        Size index = 1;
        for (auto& media : model.Media)
        {
            if (media.Data.empty())
            {
                // Reference-only media (for example round-tripped from Markdown):
                // keep the existing reference untouched.
                ++index;
                continue;
            }
            const auto fileName = "image" + std::to_string(index) + "." + ExtensionForContentType(media.ContentType);
            const auto outputPath = mediaDir / fileName;
            if (std::filesystem::exists(outputPath) && !overwrite)
            {
                Error(result.Diagnostics, "Media file already exists (use --overwrite)", outputPath.string());
                ++index;
                continue;
            }
            std::ofstream file(outputPath, std::ios::binary | std::ios::trunc);
            if (!file)
            {
                Error(result.Diagnostics, "Cannot write media file", outputPath.string());
                ++index;
                continue;
            }
            file.write(reinterpret_cast<const char*>(media.Data.data()),
                       static_cast<std::streamsize>(media.Data.size()));
            media.FileName = directoryName + "/" + fileName;

            MediaExportItem item;
            item.PartUri = media.Id;
            item.OutputPath = outputPath;
            item.ContentType = media.ContentType;
            item.Size = media.Data.size();
            result.MediaItems.push_back(std::move(item));
            ++index;
        }
    }

    /// Loads media payloads referenced by file name, relative to baseDir.
    static void ResolveExternalMedia(DocumentModel& model, const std::filesystem::path& baseDir,
                                     std::vector<ToolDiagnostic>& diagnostics)
    {
        for (auto& media : model.Media)
        {
            if (!media.Data.empty() || media.FileName.empty())
            {
                continue;
            }
            const auto path = baseDir / std::filesystem::path(media.FileName);
            std::ifstream file(path, std::ios::binary);
            if (!file)
            {
                Warn(diagnostics, "Referenced media file not found", path.string());
                continue;
            }
            media.Data.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
            if (media.ContentType.empty())
            {
                media.ContentType = ContentTypeForExtension(path.extension().string());
            }
        }
    }

    static void FillCounts(const DocumentModel& model, ConvertResult& result)
    {
        result.Family = model.Family;
        if (model.Word)
        {
            result.BlockCount = model.Word->Body.size();
        }
        if (model.Excel)
        {
            result.SheetCount = model.Excel->Sheets.size();
            for (const auto& sheet : model.Excel->Sheets)
            {
                result.CellCount += sheet.Cells.size();
            }
        }
        if (model.PowerPoint)
        {
            result.SlideCount = model.PowerPoint->Slides.size();
        }
    }

    static bool HasErrorDiagnostic(const std::vector<ToolDiagnostic>& diagnostics)
    {
        for (const auto& diagnostic : diagnostics)
        {
            if (diagnostic.Severity == ToolSeverity::Error)
            {
                return true;
            }
        }
        return false;
    }
};

std::string_view ToString(ConvertFormat format) noexcept
{
    switch (format)
    {
        case ConvertFormat::Docx:
            return "docx";
        case ConvertFormat::Xlsx:
            return "xlsx";
        case ConvertFormat::Pptx:
            return "pptx";
        case ConvertFormat::Json:
            return "json";
        case ConvertFormat::Markdown:
            return "md";
        case ConvertFormat::Text:
            return "txt";
        case ConvertFormat::Xml:
            return "xml";
        case ConvertFormat::Csv:
            return "csv";
        case ConvertFormat::Auto:
            break;
    }
    return "auto";
}

ConvertFormat ParseConvertFormat(std::string_view token) noexcept
{
    if (token == "docx")
    {
        return ConvertFormat::Docx;
    }
    if (token == "xlsx")
    {
        return ConvertFormat::Xlsx;
    }
    if (token == "pptx")
    {
        return ConvertFormat::Pptx;
    }
    if (token == "json")
    {
        return ConvertFormat::Json;
    }
    if (token == "md" || token == "markdown")
    {
        return ConvertFormat::Markdown;
    }
    if (token == "txt" || token == "text")
    {
        return ConvertFormat::Text;
    }
    if (token == "xml")
    {
        return ConvertFormat::Xml;
    }
    if (token == "csv")
    {
        return ConvertFormat::Csv;
    }
    return ConvertFormat::Auto;
}

ConvertFormat InferConvertFormat(const std::filesystem::path& path) noexcept
{
    const std::string extension = AsciiText::ToLower(path.extension().string());
    if (extension == ".docx" || extension == ".docm" || extension == ".dotx" || extension == ".dotm")
    {
        return ConvertFormat::Docx;
    }
    if (extension == ".xlsx" || extension == ".xlsm" || extension == ".xltx" || extension == ".xltm")
    {
        return ConvertFormat::Xlsx;
    }
    if (extension == ".pptx" || extension == ".pptm" || extension == ".potx" || extension == ".potm")
    {
        return ConvertFormat::Pptx;
    }
    if (extension == ".json")
    {
        return ConvertFormat::Json;
    }
    if (extension == ".md" || extension == ".markdown")
    {
        return ConvertFormat::Markdown;
    }
    if (extension == ".txt" || extension == ".text")
    {
        return ConvertFormat::Text;
    }
    if (extension == ".xml")
    {
        return ConvertFormat::Xml;
    }
    if (extension == ".csv")
    {
        return ConvertFormat::Csv;
    }
    return ConvertFormat::Auto;
}

ConvertResult ConvertDocument(const std::filesystem::path& input, const std::filesystem::path& output,
                              const ConvertOptions& options)
{
    ConvertResult result;
    const bool toStdout = output == "-";

    result.From = options.From != ConvertFormat::Auto ? options.From : InferConvertFormat(input);
    result.To = options.To != ConvertFormat::Auto ? options.To
                                                  : (toStdout ? ConvertFormat::Auto : InferConvertFormat(output));

    if (result.From == ConvertFormat::Auto)
    {
        result.UsageOk = false;
        DocumentConverterHelper::Error(result.Diagnostics, "Cannot determine the input format; use --from", input.string());
        return result;
    }
    if (result.To == ConvertFormat::Auto)
    {
        result.UsageOk = false;
        DocumentConverterHelper::Error(result.Diagnostics,
                                       toStdout ? "Writing to stdout requires an explicit --to format"
                                                : "Cannot determine the output format; use --to",
                                       output.string());
        return result;
    }
    if (DocumentConverterHelper::IsOfficeFormat(result.From) && DocumentConverterHelper::IsOfficeFormat(result.To))
    {
        result.UsageOk = false;
        DocumentConverterHelper::Error(result.Diagnostics, "Office-to-Office conversion is not supported");
        return result;
    }
    if (!DocumentConverterHelper::IsOfficeFormat(result.To) && result.From != ConvertFormat::Json &&
        result.From != ConvertFormat::Xml && !DocumentConverterHelper::IsOfficeFormat(result.From))
    {
        result.UsageOk = false;
        DocumentConverterHelper::Error(result.Diagnostics,
                                       "Markdown/text/CSV input can only be converted to an Office format (docx/xlsx/pptx)");
        return result;
    }
    if (toStdout && !DocumentConverterHelper::IsTextFormat(result.To))
    {
        result.UsageOk = false;
        DocumentConverterHelper::Error(result.Diagnostics, "Only text formats (md/json/txt/xml) can be written to stdout");
        return result;
    }

    // --- Build the semantic model from the input ---------------------------
    DocumentModel model;
    ModelReadOptions readOptions;
    readOptions.IncludeMediaData = !options.NoMedia;

    switch (result.From)
    {
        case ConvertFormat::Docx:
            model = ReadWordModel(input, readOptions, result.Diagnostics);
            break;
        case ConvertFormat::Xlsx:
            model = ReadExcelModel(input, readOptions, result.Diagnostics);
            break;
        case ConvertFormat::Pptx:
            model = ReadPowerPointModel(input, readOptions, result.Diagnostics);
            break;
        case ConvertFormat::Json:
        case ConvertFormat::Xml:
        case ConvertFormat::Markdown:
        case ConvertFormat::Text:
        case ConvertFormat::Csv:
        {
            std::string text;
            if (!DocumentConverterHelper::ReadTextFile(input, text, result.Diagnostics))
            {
                return result;
            }
            if (result.From == ConvertFormat::Json)
            {
                model = ParseModelJson(text, result.Diagnostics);
            }
            else if (result.From == ConvertFormat::Xml)
            {
                model = ParseModelXml(text, result.Diagnostics);
            }
            else if (result.From == ConvertFormat::Markdown)
            {
                model = ParseModelMarkdown(text, DocumentConverterHelper::FamilyForFormat(result.To), result.Diagnostics);
            }
            else if (result.From == ConvertFormat::Csv)
            {
                CsvOptions csvOptions;
                csvOptions.Separator = options.CsvSeparator;
                csvOptions.SheetName = options.SheetName;
                model = ParseModelCsv(text, csvOptions, result.Diagnostics);
            }
            else
            {
                model = ParseModelText(text, DocumentConverterHelper::FamilyForFormat(result.To), result.Diagnostics);
            }
            break;
        }
        case ConvertFormat::Auto:
            break;
    }

    if (model.Family == DocumentFamily::Unknown)
    {
        if (!DocumentConverterHelper::HasErrorDiagnostic(result.Diagnostics))
        {
            DocumentConverterHelper::Error(result.Diagnostics, "Input could not be converted into a document model");
        }
        return result;
    }
    // A json/xml envelope fixes the family; reject a mismatched Office target.
    if (DocumentConverterHelper::IsOfficeFormat(result.To) && DocumentConverterHelper::FamilyForFormat(result.To) != model.Family)
    {
        result.UsageOk = false;
        DocumentConverterHelper::Error(result.Diagnostics, "Input document family does not match the requested output format",
                                       std::string(ToString(model.Family)));
        return result;
    }
    // CSV models a worksheet grid; a Word or PowerPoint model has none.
    if (result.To == ConvertFormat::Csv && model.Family != DocumentFamily::Excel)
    {
        result.UsageOk = false;
        DocumentConverterHelper::Error(result.Diagnostics, "CSV output requires Excel-family input (xlsx or an Excel envelope)",
                                       std::string(ToString(model.Family)));
        return result;
    }
    DocumentConverterHelper::FillCounts(model, result);

    // --- Media handling -----------------------------------------------------
    if (result.To == ConvertFormat::Csv)
    {
        // CSV has no media representation at all; drop with a notice.
        for (const auto& media : model.Media)
        {
            DocumentConverterHelper::Warn(result.Diagnostics, "Media dropped (CSV cannot carry media)", media.Id);
        }
        model.Media.clear();
    }
    else if (DocumentConverterHelper::IsTextFormat(result.To))
    {
        if (options.NoMedia)
        {
            for (const auto& media : model.Media)
            {
                DocumentConverterHelper::Warn(result.Diagnostics, "Media dropped (--no-media)", media.Id);
            }
            model.Media.clear();
        }
        else if (options.EmbedMedia)
        {
            if (result.To != ConvertFormat::Json && result.To != ConvertFormat::Xml)
            {
                DocumentConverterHelper::Warn(result.Diagnostics, "--embed-media only applies to json/xml output; ignored");
            }
        }
        else
        {
            auto mediaDir = options.MediaDir;
            if (mediaDir.empty())
            {
                if (toStdout)
                {
                    mediaDir = input.parent_path() / (input.stem().string() + "_media");
                }
                else
                {
                    mediaDir = output.parent_path() / (output.stem().string() + "_media");
                }
            }
            DocumentConverterHelper::ExportExternalMedia(model, mediaDir, options.Overwrite, result);
        }
    }
    else
    {
        const auto baseDir = options.MediaDir.empty() ? input.parent_path() : options.MediaDir;
        DocumentConverterHelper::ResolveExternalMedia(model, baseDir, result.Diagnostics);
    }

    // --- Produce the output -------------------------------------------------
    bool ok = true;
    switch (result.To)
    {
        case ConvertFormat::Docx:
            ok = WriteWordModel(model, output, result.Diagnostics);
            break;
        case ConvertFormat::Xlsx:
            ok = WriteExcelModel(model, output, result.Diagnostics);
            break;
        case ConvertFormat::Pptx:
            ok = WritePowerPointModel(model, output, result.Diagnostics);
            break;
        case ConvertFormat::Json:
        case ConvertFormat::Xml:
        case ConvertFormat::Markdown:
        case ConvertFormat::Text:
        case ConvertFormat::Csv:
        {
            const bool embed = options.EmbedMedia &&
                               (result.To == ConvertFormat::Json || result.To == ConvertFormat::Xml);
            std::string payload;
            if (result.To == ConvertFormat::Json)
            {
                payload = SerializeModelJson(model, embed);
            }
            else if (result.To == ConvertFormat::Xml)
            {
                payload = SerializeModelXml(model, embed);
            }
            else if (result.To == ConvertFormat::Markdown)
            {
                payload = SerializeModelMarkdown(model, result.Diagnostics);
            }
            else if (result.To == ConvertFormat::Csv)
            {
                CsvOptions csvOptions;
                csvOptions.Separator = options.CsvSeparator;
                csvOptions.SheetName = options.SheetName;
                payload = SerializeModelCsv(model, csvOptions, result.Diagnostics);
            }
            else
            {
                payload = SerializeModelText(model);
            }

            if (toStdout)
            {
                result.OutputText = std::move(payload);
            }
            else
            {
                ok = DocumentConverterHelper::WriteTextFile(output, payload, result.Diagnostics);
            }
            break;
        }
        case ConvertFormat::Auto:
            ok = false;
            break;
    }

    result.Ok = ok && !DocumentConverterHelper::HasErrorDiagnostic(result.Diagnostics);
    return result;
}

} // namespace ExyokiOffice::Tools
