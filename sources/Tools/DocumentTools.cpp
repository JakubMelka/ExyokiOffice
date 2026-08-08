// Copyright (c) 2026 Jakub Melka and Collaborators
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "ExyokiOffice/Tools/DocumentTools.hpp"
#include "ExyokiOffice/Tools/PackageLimits.hpp"

#include "ExyokiOffice/Excel/ExcelDocument.hpp"
#include "ExyokiOffice/OpenXmlPackage.hpp"
#include "ExyokiOffice/PowerPoint/PowerPointDocument.hpp"
#include "ExyokiOffice/Tools/OutputNaming.hpp"
#include "ExyokiOffice/Tools/PackageInspector.hpp"
#include "ExyokiOffice/StandardTypes.hpp"

#include <algorithm>
#include <iomanip>
#include <sstream>

namespace ExyokiOffice::Tools
{
namespace
{
void Error(std::vector<ToolDiagnostic>& diagnostics, std::string message,
           const std::filesystem::path& context = {})
{
    diagnostics.push_back({ToolSeverity::Error, std::move(message), context.string()});
}

DocumentFamily Detect(const std::filesystem::path& path, std::vector<ToolDiagnostic>& diagnostics)
{
    OpenXmlPackage package;
    ApplyDefaultPackageLimits(package);
    if (!package.LoadFromFile(path))
    {
        Error(diagnostics, "Failed to open package", path);
        return DocumentFamily::Unknown;
    }
    const auto family = GetInfo(package).Family;
    if (family == DocumentFamily::Unknown)
    {
        Error(diagnostics, "Package is not a supported Word, Excel, or PowerPoint document", path);
    }
    return family;
}

std::filesystem::path NumberedPath(const std::filesystem::path& directory,
                                   std::string_view prefix, std::string_view extension,
                                   Size index, Size count)
{
    std::ostringstream name;
    const auto width = std::max<Size>(2, std::to_string(count).size());
    name << (prefix.empty() ? "part" : prefix) << '_' << std::setw(static_cast<int>(width))
         << std::setfill('0') << index << extension;
    return directory / name.str();
}

bool PrepareDirectory(const std::filesystem::path& directory,
                      const std::vector<std::filesystem::path>& paths,
                      bool overwrite, std::vector<ToolDiagnostic>& diagnostics)
{
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    if (error)
    {
        Error(diagnostics, "Failed to create output directory", directory);
        return false;
    }
    if (!overwrite)
    {
        for (const auto& path : paths)
        {
            if (std::filesystem::exists(path))
            {
                Error(diagnostics, "Output file already exists", path);
                return false;
            }
        }
    }
    return true;
}

template <typename Editor, typename Remove>
bool WritePrunedGroups(const std::vector<Byte>& bytes, Size itemCount,
                       Size perFile, const std::vector<std::filesystem::path>& paths,
                       Remove remove, std::vector<std::filesystem::path>& written,
                       std::vector<ToolDiagnostic>& diagnostics)
{
    for (Size group = 0; group < paths.size(); ++group)
    {
        auto editor = Editor::Open(bytes, OwnOutputOpenSettings());
        if (!editor)
        {
            Error(diagnostics, "Failed to clone source package", paths[group]);
            return false;
        }
        const auto first = group * perFile;
        const auto last = std::min(itemCount, first + perFile);
        for (Size index = itemCount; index-- > 0;)
        {
            if ((index < first || index >= last) && !remove(*editor, index))
            {
                Error(diagnostics, "Failed to remove an item while creating split package", paths[group]);
                return false;
            }
        }
        if (!editor->SaveToFile(paths[group]))
        {
            Error(diagnostics, "Failed to write split package", paths[group]);
            return false;
        }
        written.push_back(paths[group]);
    }
    return true;
}
} // namespace

DocumentSplitResult SplitDocument(const std::filesystem::path& inputFile,
                                  const std::filesystem::path& outputDirectory,
                                  const DocumentSplitOptions& options)
{
    DocumentSplitResult result;

    // The prefix is concatenated into every output name, so a caller that passes
    // `../../evil` writes outside the directory it chose. Checked before the
    // package is even opened: nothing about the input can make the prefix safe.
    if (!options.OutputPrefix.empty() && !IsPlainOutputName(options.OutputPrefix))
    {
        Error(result.Diagnostics, "Output prefix is not a plain file name", options.OutputPrefix);
        return result;
    }

    result.Family = Detect(inputFile, result.Diagnostics);
    if (result.Family == DocumentFamily::Unknown)
    {
        return result;
    }

    if (result.Family == DocumentFamily::Word)
    {
        if (options.Strategy == DocumentSplitStrategy::Worksheets ||
            options.Strategy == DocumentSplitStrategy::Slides)
        {
            Error(result.Diagnostics, "Selected split strategy is not valid for Word documents", inputFile);
            return result;
        }
        WordSplitOptions word;
        word.ParagraphsPerDocument = options.ItemCount;
        word.Marker = options.Marker;
        word.OutputPrefix = options.OutputPrefix;
        word.Overwrite = options.Overwrite;
        switch (options.Strategy)
        {
            case DocumentSplitStrategy::Auto:
            case DocumentSplitStrategy::SectionBreaks:
                word.Strategy = WordSplitStrategy::SectionBreaks;
                break;
            case DocumentSplitStrategy::PageBreaks:
                word.Strategy = WordSplitStrategy::PageBreaks;
                break;
            case DocumentSplitStrategy::ParagraphCount:
                word.Strategy = WordSplitStrategy::ParagraphCount;
                break;
            case DocumentSplitStrategy::Marker:
                word.Strategy = WordSplitStrategy::Marker;
                break;
            default:
                break;
        }
        auto wordResult = SplitWordDocument(inputFile, outputDirectory, word);
        result.Ok = wordResult.Ok;
        result.OutputFiles = std::move(wordResult.OutputFiles);
        result.Diagnostics.insert(result.Diagnostics.end(), wordResult.Diagnostics.begin(), wordResult.Diagnostics.end());
        return result;
    }

    const bool excel = result.Family == DocumentFamily::Excel;
    const auto expected = excel ? DocumentSplitStrategy::Worksheets : DocumentSplitStrategy::Slides;
    if (options.Strategy != DocumentSplitStrategy::Auto && options.Strategy != expected)
    {
        Error(result.Diagnostics, "Selected split strategy is not valid for " + std::string(ToString(result.Family)) + " documents", inputFile);
        return result;
    }
    const auto perFile = options.ItemCount == 0 ? 1 : options.ItemCount;
    std::vector<Byte> bytes;
    Size items = 0;
    if (excel)
    {
        auto editor = Excel::ExcelDocumentEditor::Open(inputFile, UntrustedOpenSettings());
        if (!editor)
        {
            Error(result.Diagnostics, "Failed to open Excel workbook", inputFile);
            return result;
        }
        bytes = editor->SaveToMemory();
        items = editor->Worksheets().size();
    }
    else
    {
        auto editor = PowerPoint::PowerPointDocumentEditor::Open(inputFile, UntrustedOpenSettings());
        if (!editor)
        {
            Error(result.Diagnostics, "Failed to open PowerPoint presentation", inputFile);
            return result;
        }
        bytes = editor->SaveToMemory();
        items = editor->SlideCount();
    }
    if (items == 0)
    {
        Error(result.Diagnostics, excel ? "Workbook has no worksheets" : "Presentation has no slides", inputFile);
        return result;
    }
    const auto groups = (items + perFile - 1) / perFile;
    std::vector<std::filesystem::path> paths;
    auto extension = inputFile.extension().string();
    if (extension.empty())
    {
        extension = excel ? ".xlsx" : ".pptx";
    }
    for (Size i = 0; i < groups; ++i)
    {
        paths.push_back(NumberedPath(outputDirectory, options.OutputPrefix, extension, i + 1, groups));
    }
    if (!PrepareDirectory(outputDirectory, paths, options.Overwrite, result.Diagnostics))
    {
        return result;
    }
    if (excel)
    {
        result.Ok = WritePrunedGroups<Excel::ExcelDocumentEditor>(
            bytes, items, perFile, paths,
            [](auto& editor, Size index)
            { return editor.RemoveWorksheet(index); },
            result.OutputFiles, result.Diagnostics);
    }
    else
    {
        result.Ok = WritePrunedGroups<PowerPoint::PowerPointDocumentEditor>(
            bytes, items, perFile, paths,
            [](auto& editor, Size index)
            { return editor.RemoveSlide(index); },
            result.OutputFiles, result.Diagnostics);
    }
    return result;
}

DocumentMergeResult MergeDocuments(const std::vector<std::filesystem::path>& inputFiles,
                                   const std::filesystem::path& outputFile,
                                   const DocumentMergeOptions& options)
{
    DocumentMergeResult result;
    result.OutputFile = outputFile;
    if (inputFiles.empty())
    {
        Error(result.Diagnostics, "At least one input document is required");
        return result;
    }
    for (const auto& input : inputFiles)
    {
        const auto family = Detect(input, result.Diagnostics);
        if (family == DocumentFamily::Unknown)
        {
            return result;
        }
        if (result.Family == DocumentFamily::Unknown)
        {
            result.Family = family;
        }
        else if (family != result.Family)
        {
            Error(result.Diagnostics, "All merge inputs must have the same document family", input);
            return result;
        }
    }
    if (!options.Overwrite && std::filesystem::exists(outputFile))
    {
        Error(result.Diagnostics, "Output file already exists", outputFile);
        return result;
    }
    std::error_code error;
    if (!outputFile.parent_path().empty())
    {
        std::filesystem::create_directories(outputFile.parent_path(), error);
    }
    if (error)
    {
        Error(result.Diagnostics, "Failed to create output directory", outputFile.parent_path());
        return result;
    }

    if (result.Family == DocumentFamily::Word)
    {
        WordMergeOptions word;
        word.Overwrite = options.Overwrite;
        word.InsertPageBreaks = options.InsertWordPageBreaks;
        word.StyleConflictPolicy = options.WordStyleConflictPolicy;
        auto merged = MergeWordDocuments(inputFiles, outputFile, word);
        result.Ok = merged.Ok;
        result.DocumentsMerged = merged.DocumentsMerged;
        result.ItemsMerged = merged.DocumentsMerged;
        result.Diagnostics.insert(result.Diagnostics.end(), merged.Diagnostics.begin(), merged.Diagnostics.end());
        return result;
    }
    if (result.Family == DocumentFamily::Excel)
    {
        auto output = Excel::ExcelDocumentEditor::Open(inputFiles.front(), UntrustedOpenSettings());
        if (!output)
        {
            Error(result.Diagnostics, "Failed to open first Excel workbook", inputFiles.front());
            return result;
        }
        result.DocumentsMerged = 1;
        result.ItemsMerged = output->Worksheets().size();
        for (Size file = 1; file < inputFiles.size(); ++file)
        {
            auto source = Excel::ExcelDocumentEditor::Open(inputFiles[file], UntrustedOpenSettings());
            if (!source)
            {
                Error(result.Diagnostics, "Failed to open Excel workbook", inputFiles[file]);
                return result;
            }
            const auto count = source->Worksheets().size();
            for (Size sheet = 0; sheet < count; ++sheet)
            {
                if (!output->CopyWorksheetFrom(*source, sheet))
                {
                    Error(result.Diagnostics,
                          "Failed to import worksheet; styled sheets require equivalent workbook style catalogs",
                          inputFiles[file]);
                    return result;
                }
                ++result.ItemsMerged;
            }
            ++result.DocumentsMerged;
        }
        result.Ok = output->SaveToFile(outputFile);
    }
    else
    {
        auto output = PowerPoint::PowerPointDocumentEditor::CreateNew();
        if (!output)
        {
            Error(result.Diagnostics, "Failed to create PowerPoint presentation", outputFile);
            return result;
        }
        for (const auto& input : inputFiles)
        {
            auto source = PowerPoint::PowerPointDocumentEditor::Open(input, UntrustedOpenSettings());
            if (!source)
            {
                Error(result.Diagnostics, "Failed to open PowerPoint presentation", input);
                return result;
            }
            for (Size slide = 0; slide < source->SlideCount(); ++slide)
            {
                if (!output->CopySlideFrom(*source, slide))
                {
                    Error(result.Diagnostics, "Failed to import PowerPoint slide", input);
                    return result;
                }
                ++result.ItemsMerged;
            }
            ++result.DocumentsMerged;
        }
        result.Ok = output->SaveToFile(outputFile);
    }
    if (!result.Ok)
    {
        Error(result.Diagnostics, "Failed to write merged document", outputFile);
    }
    return result;
}

} // namespace ExyokiOffice::Tools
