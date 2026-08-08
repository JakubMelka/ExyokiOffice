// Copyright (c) 2026 Jakub Melka and Collaborators
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "ExyokiOffice/Tools/WordDocumentTools.hpp"
#include "ExyokiOffice/Tools/PackageLimits.hpp"

#include "ExyokiOffice/OpenXmlPackage.hpp"
#include "ExyokiOffice/OpenXmlQualifiedName.hpp"
#include "ExyokiOffice/Tools/OutputNaming.hpp"
#include "ExyokiOffice/Tools/PackageInspector.hpp"
#include "ExyokiOffice/StandardTypes.hpp"

#include <iomanip>
#include <sstream>

namespace ExyokiOffice::Tools
{
namespace
{
using Word::BodyBlock;
using Word::BodyBlockType;
using Word::WordDocumentEditor;

constexpr std::string_view WordNamespace =
    "http://schemas.openxmlformats.org/wordprocessingml/2006/main";

void AddError(std::vector<ToolDiagnostic>& diagnostics, std::string message, const std::filesystem::path& path = {})
{
    diagnostics.push_back({ToolSeverity::Error, std::move(message), path.string()});
}

bool ValidateWordPackage(const std::filesystem::path& path, std::vector<ToolDiagnostic>& diagnostics)
{
    OpenXmlPackage package;
    ApplyDefaultPackageLimits(package);
    if (!package.LoadFromFile(path))
    {
        AddError(diagnostics, "Failed to open package", path);
        return false;
    }
    const auto info = GetInfo(package);
    if (info.Family != DocumentFamily::Word)
    {
        diagnostics.push_back({ToolSeverity::Error,
                               "Expected a Word document, but the package is " +
                                   std::string(ToString(info.Family)),
                               path.string()});
        return false;
    }
    return true;
}

bool HasDescendant(const std::shared_ptr<OpenXMLElement>& root, std::string_view localName)
{
    if (!root)
    {
        return false;
    }
    for (const auto& child : root->Children())
    {
        if (!child)
        {
            continue;
        }
        const auto& name = child->ElementMetaClass()->QualifiedName();
        if (name.namespaceUri() == WordNamespace && name.localName() == localName)
        {
            return true;
        }
        if (HasDescendant(child, localName))
        {
            return true;
        }
    }
    return false;
}

bool HasPageBreak(const std::shared_ptr<OpenXMLElement>& root)
{
    if (!root)
    {
        return false;
    }
    const OpenXmlQualifiedName typeName(WordNamespace, "type");
    for (const auto& child : root->Children())
    {
        if (!child)
        {
            continue;
        }
        const auto& name = child->ElementMetaClass()->QualifiedName();
        if (name.namespaceUri() == WordNamespace && name.localName() == "lastRenderedPageBreak")
        {
            return true;
        }
        if (name.namespaceUri() == WordNamespace && name.localName() == "br")
        {
            std::string_view value;
            if (child->TryGetAttribute(typeName, value) && value == "page")
            {
                return true;
            }
        }
        if (HasPageBreak(child))
        {
            return true;
        }
    }
    return false;
}

std::vector<std::pair<Size, Size>> FindSegments(
    const std::vector<BodyBlock>& blocks, const WordSplitOptions& options)
{
    std::vector<std::pair<Size, Size>> ranges;
    Size begin = 0;
    Size paragraphs = 0;
    auto finish = [&](Size end)
    {
        if (end > begin)
        {
            ranges.emplace_back(begin, end);
        }
        begin = end;
        paragraphs = 0;
    };

    for (Size i = 0; i < blocks.size(); ++i)
    {
        const auto& block = blocks[i];
        if (block.Type() == BodyBlockType::Section)
        {
            continue;
        }

        bool before = false;
        bool after = false;
        if (auto paragraph = block.AsParagraph())
        {
            switch (options.Strategy)
            {
                case WordSplitStrategy::SectionBreaks:
                    after = HasDescendant(block.GetLowLevelApi(), "sectPr");
                    break;
                case WordSplitStrategy::PageBreaks:
                    before = paragraph->GetPageBreakBefore().value_or(false);
                    after = HasPageBreak(block.GetLowLevelApi());
                    break;
                case WordSplitStrategy::ParagraphCount:
                    ++paragraphs;
                    after = paragraphs >= options.ParagraphsPerDocument;
                    break;
                case WordSplitStrategy::Marker:
                    before = paragraph->PlainText().find(options.Marker) != std::string::npos;
                    break;
            }
        }
        if (before && i > begin)
        {
            finish(i);
        }
        if (after)
        {
            finish(i + 1);
        }
    }
    if (begin < blocks.size())
    {
        ranges.emplace_back(begin, blocks.size());
    }
    if (ranges.empty() && !blocks.empty())
    {
        ranges.emplace_back(0, blocks.size());
    }
    return ranges;
}

std::filesystem::path PartPath(const std::filesystem::path& directory, std::string_view prefix,
                               Size index, Size count)
{
    std::ostringstream name;
    const auto width = std::max<Size>(2, std::to_string(count).size());
    name << prefix << '_' << std::setw(static_cast<int>(width)) << std::setfill('0') << index << ".docx";
    return directory / name.str();
}
} // namespace

WordSplitResult SplitWordDocument(const std::filesystem::path& inputFile,
                                  const std::filesystem::path& outputDirectory,
                                  const WordSplitOptions& options)
{
    WordSplitResult result;
    if (options.Strategy == WordSplitStrategy::ParagraphCount && options.ParagraphsPerDocument == 0)
    {
        AddError(result.Diagnostics, "Paragraph count must be greater than zero");
        return result;
    }
    if (options.Strategy == WordSplitStrategy::Marker && options.Marker.empty())
    {
        AddError(result.Diagnostics, "Split marker must not be empty");
        return result;
    }
    // PartPath() concatenates the prefix into every output name, so a prefix
    // carrying a separator or `..` writes outside the directory the caller chose.
    if (!options.OutputPrefix.empty() && !IsPlainOutputName(options.OutputPrefix))
    {
        AddError(result.Diagnostics, "Output prefix is not a plain file name", options.OutputPrefix);
        return result;
    }
    if (!ValidateWordPackage(inputFile, result.Diagnostics))
    {
        return result;
    }
    auto source = WordDocumentEditor::Open(inputFile, UntrustedOpenSettings());
    if (!source)
    {
        AddError(result.Diagnostics, "Failed to open Word document", inputFile);
        return result;
    }
    std::error_code error;
    std::filesystem::create_directories(outputDirectory, error);
    if (error)
    {
        AddError(result.Diagnostics, "Failed to create output directory", outputDirectory);
        return result;
    }

    const auto sourceBytes = source->SaveToMemory();
    const auto blocks = source->BodyBlocks();
    const auto ranges = FindSegments(blocks, options);
    if (ranges.empty())
    {
        AddError(result.Diagnostics, "The source document has no body content", inputFile);
        return result;
    }

    std::vector<std::filesystem::path> outputPaths;
    outputPaths.reserve(ranges.size());
    for (Size part = 0; part < ranges.size(); ++part)
    {
        auto path = PartPath(outputDirectory, options.OutputPrefix, part + 1, ranges.size());
        if (!options.Overwrite && std::filesystem::exists(path))
        {
            AddError(result.Diagnostics, "Output file already exists", path);
            return result;
        }
        outputPaths.push_back(std::move(path));
    }

    for (Size part = 0; part < ranges.size(); ++part)
    {
        const auto& path = outputPaths[part];
        auto slice = WordDocumentEditor::Open(sourceBytes, OwnOutputOpenSettings());
        auto output = WordDocumentEditor::CreateNew();
        if (!slice || !output)
        {
            AddError(result.Diagnostics, "Failed to create split document", path);
            return result;
        }
        auto sliceBlocks = slice->BodyBlocks();
        const auto [first, last] = ranges[part];
        for (Size i = 0; i < sliceBlocks.size(); ++i)
        {
            if (sliceBlocks[i].Type() == BodyBlockType::Section || (i >= first && i < last))
            {
                continue;
            }
            const auto element = sliceBlocks[i].GetLowLevelApi();
            if (element && element->Parent())
            {
                element->Parent()->RemoveChild(element);
            }
        }
        if (!output->Body().InsertDocument(*slice) || !output->SaveToFile(path))
        {
            AddError(result.Diagnostics, "Failed to write split document", path);
            return result;
        }
        result.OutputFiles.push_back(path);
    }
    result.Ok = true;
    return result;
}

WordMergeResult MergeWordDocuments(const std::vector<std::filesystem::path>& inputFiles,
                                   const std::filesystem::path& outputFile,
                                   const WordMergeOptions& options)
{
    WordMergeResult result;
    result.OutputFile = outputFile;
    if (inputFiles.empty())
    {
        AddError(result.Diagnostics, "At least one input document is required");
        return result;
    }
    if (!options.Overwrite && std::filesystem::exists(outputFile))
    {
        AddError(result.Diagnostics, "Output file already exists", outputFile);
        return result;
    }
    auto output = WordDocumentEditor::CreateNew();
    if (!output)
    {
        AddError(result.Diagnostics, "Failed to create output document", outputFile);
        return result;
    }
    Word::DocumentMergeOptions mergeOptions;
    mergeOptions.StyleConflictPolicy = options.StyleConflictPolicy;
    for (const auto& input : inputFiles)
    {
        if (!ValidateWordPackage(input, result.Diagnostics))
        {
            return result;
        }
        auto source = WordDocumentEditor::Open(input, UntrustedOpenSettings());
        if (!source)
        {
            AddError(result.Diagnostics, "Failed to open Word document", input);
            return result;
        }
        if (result.DocumentsMerged > 0 && options.InsertPageBreaks)
        {
            output->AddPageBreak();
        }
        if (!output->Body().InsertDocument(*source, mergeOptions))
        {
            AddError(result.Diagnostics, "Failed to merge Word document", input);
            return result;
        }
        ++result.DocumentsMerged;
    }
    const auto parent = outputFile.parent_path();
    std::error_code error;
    if (!parent.empty())
    {
        std::filesystem::create_directories(parent, error);
    }
    if (error || !output->SaveToFile(outputFile))
    {
        AddError(result.Diagnostics, "Failed to write merged document", outputFile);
        return result;
    }
    result.Ok = true;
    return result;
}

} // namespace ExyokiOffice::Tools
