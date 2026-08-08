// Copyright (c) 2026 Jakub Melka and Collaborators
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "ExyokiOffice/Tools/TextExtractor.hpp"
#include "ExyokiOffice/Tools/PackageLimits.hpp"

#include "ExyokiOffice/Excel/ExcelDocument.hpp"
#include "ExyokiOffice/OpenXmlPackage.hpp"
#include "ExyokiOffice/PowerPoint/PowerPointDocument.hpp"
#include "ExyokiOffice/Word/WordDocument.hpp"
#include "ExyokiOffice/Tools/PackageInspector.hpp"
#include "ExyokiOffice/Tools/WordTextTools.hpp"
#include "ExyokiOffice/StandardTypes.hpp"

namespace ExyokiOffice::Tools
{

namespace
{

std::string JoinShapeText(const PowerPoint::PresentationTextFrame& frame)
{
    std::string text;
    for (const auto& paragraph : frame.Paragraphs)
    {
        for (const auto& run : paragraph.Runs)
        {
            text += run.Text;
        }
        text += "\n";
    }
    while (!text.empty() && text.back() == '\n')
    {
        text.pop_back();
    }
    return text;
}

ExtractedDocumentText ExtractPowerPoint(PowerPoint::PowerPointDocumentEditor& editor)
{
    ExtractedDocumentText result;
    result.Family = DocumentFamily::PowerPoint;

    Size slideIndex = 1;
    for (const auto& slide : editor.Slides())
    {
        Size shapeIndex = 1;
        if (auto tree = slide->ShapeTree())
        {
            for (const auto& shape : tree->Shapes())
            {
                if (auto frame = shape->GetTextFrame())
                {
                    auto text = JoinShapeText(*frame);
                    if (!text.empty())
                    {
                        result.Blocks.push_back(ExtractedTextBlock{
                            "slide " + std::to_string(slideIndex) + " shape " + std::to_string(shapeIndex),
                            std::move(text)});
                    }
                }
                ++shapeIndex;
            }
        }

        if (auto notes = slide->NotesText(); !notes.empty())
        {
            result.Blocks.push_back(ExtractedTextBlock{"slide " + std::to_string(slideIndex) + " notes", notes});
        }
        ++slideIndex;
    }

    result.Ok = true;
    return result;
}

ExtractedDocumentText ExtractExcel(Excel::ExcelDocumentEditor& editor)
{
    ExtractedDocumentText result;
    result.Family = DocumentFamily::Excel;

    auto sharedStrings = editor.SharedStrings();
    for (const auto& worksheet : editor.Worksheets())
    {
        const auto sheetName = worksheet->Name();
        for (const auto& address : worksheet->StoredCellAddresses())
        {
            const auto value = worksheet->GetCellValue(address);
            if (!value || value->IsBlank())
            {
                continue;
            }

            std::string text;
            if (value->Kind() == Excel::CellValueKind::SharedString)
            {
                if (auto index = value->SharedStringIndex())
                {
                    text = sharedStrings.Lookup(*index).value_or(std::string());
                }
            }
            else
            {
                text = value->Text();
            }

            if (text.empty())
            {
                continue;
            }

            result.Blocks.push_back(ExtractedTextBlock{sheetName + "!" + address.ToA1(), std::move(text)});
        }
    }

    result.Ok = true;
    return result;
}

ExtractedDocumentText ExtractWord(Word::WordDocumentEditor& editor)
{
    ExtractedDocumentText result;
    result.Family = DocumentFamily::Word;

    const auto extracted = ExtractText(editor);
    result.Diagnostics = extracted.Diagnostics;
    if (!extracted.Ok)
    {
        return result;
    }

    for (const auto& block : extracted.Blocks)
    {
        result.Blocks.push_back(ExtractedTextBlock{std::string(ToString(block.Scope)) + ": " + block.Label,
                                                   block.Text});
    }
    result.Ok = true;
    return result;
}

/// The result a family reports when its editor could not open the file.
ExtractedDocumentText FailedToOpen(DocumentFamily family, const char* message, const std::filesystem::path& path)
{
    ExtractedDocumentText result;
    result.Family = family;
    result.Diagnostics.push_back(ToolDiagnostic{ToolSeverity::Error, message, path.string()});
    return result;
}

} // namespace

ExtractedDocumentText Extract(const std::filesystem::path& path)
{
    OpenXmlPackage package;
    ApplyDefaultPackageLimits(package);
    if (!package.LoadFromFile(path))
    {
        ExtractedDocumentText result;
        result.Diagnostics.push_back(ToolDiagnostic{ToolSeverity::Error, "Failed to open package", path.string()});
        return result;
    }

    const auto info = GetInfo(package);
    switch (info.Family)
    {
        case DocumentFamily::Word:
        {
            auto editor = Word::WordDocumentEditor::Open(path, UntrustedOpenSettings());
            return editor ? Extract(*editor)
                          : FailedToOpen(DocumentFamily::Word, "Failed to open Word document", path);
        }
        case DocumentFamily::Excel:
        {
            auto editor = Excel::ExcelDocumentEditor::Open(path, UntrustedOpenSettings());
            return editor ? Extract(*editor)
                          : FailedToOpen(DocumentFamily::Excel, "Failed to open Excel document", path);
        }
        case DocumentFamily::PowerPoint:
        {
            auto editor = PowerPoint::PowerPointDocumentEditor::Open(path, UntrustedOpenSettings());
            return editor ? Extract(*editor)
                          : FailedToOpen(DocumentFamily::PowerPoint, "Failed to open PowerPoint document", path);
        }
        case DocumentFamily::Unknown:
            break;
    }

    ExtractedDocumentText result;
    result.Diagnostics.push_back(
        ToolDiagnostic{ToolSeverity::Error, DescribeUnknownFamily(info), path.string()});
    return result;
}

ExtractedDocumentText Extract(Word::WordDocumentEditor& editor)
{
    return ExtractWord(editor);
}

ExtractedDocumentText Extract(Excel::ExcelDocumentEditor& editor)
{
    return ExtractExcel(editor);
}

ExtractedDocumentText Extract(PowerPoint::PowerPointDocumentEditor& editor)
{
    return ExtractPowerPoint(editor);
}

} // namespace ExyokiOffice::Tools
