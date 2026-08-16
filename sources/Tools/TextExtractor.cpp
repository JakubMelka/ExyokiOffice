// Copyright (c) 2026 Jakub Melka and Contributors
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

/// File-local helpers behind text extraction.
class TextExtractorHelper
{
public:
    static std::string JoinShapeText(const PowerPoint::PresentationTextFrame& frame)
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

    /// The cells of a presentation table, row by row, tab separated.
    static std::string JoinTableText(const PowerPoint::PresentationTableData& table)
    {
        std::string text;
        for (const auto& row : table.Rows)
        {
            // The separator goes before every cell but the first, counted by
            // position rather than by what has been written so far. Deciding it
            // on "is the line still empty" drops the tabs of leading empty
            // cells, and {"", "Q2"} then extracts as `Q2`, indistinguishable
            // from a one-column table - the column a value sits in is exactly
            // what a tab-separated extract is for.
            std::string line;
            bool anyText = false;
            for (Size index = 0; index < row.Cells.size(); ++index)
            {
                if (index != 0)
                {
                    line += '\t';
                }
                line += row.Cells[index].Text;
                anyText = anyText || !row.Cells[index].Text.empty();
            }
            // A row of nothing but empty cells is still skipped; it carries no
            // text, and its tabs would only be noise in the extract.
            if (anyText)
            {
                text += line;
                text += '\n';
            }
        }
        while (!text.empty() && text.back() == '\n')
        {
            text.pop_back();
        }
        return text;
    }

    /**
     * @brief Appends the text of @p shape and, for a group, of everything inside it.
     *
     * A `p:grpSp` holds no text of its own; grouping three labelled boxes used
     * to make all three vanish from the extract, and grouping is what a deck
     * author does to move them together. A table is a `p:graphicFrame` rather
     * than a shape with a text frame, so its text is reached through the table
     * data instead of through GetTextFrame().
     *
     * The label carries the path through the groups - `shape 3.1` - so a caller
     * can tell nesting apart from a flat slide.
     */
    static void CollectShapeText(const PowerPoint::PresentationShape::Ptr& shape,
                                 const std::string& slideLabel,
                                 const std::string& shapeLabel,
                                 ExtractedDocumentText& result)
    {
        if (!shape)
        {
            return;
        }

        if (shape->IsGroup())
        {
            Size childIndex = 1;
            for (const auto& child : shape->Children())
            {
                CollectShapeText(child, slideLabel, shapeLabel + "." + std::to_string(childIndex), result);
                ++childIndex;
            }
            return;
        }

        if (auto frame = shape->GetTextFrame())
        {
            if (auto text = JoinShapeText(*frame); !text.empty())
            {
                result.Blocks.push_back(ExtractedTextBlock{slideLabel + " shape " + shapeLabel, std::move(text)});
            }
        }

        if (auto table = shape->GetTable())
        {
            if (auto text = JoinTableText(*table); !text.empty())
            {
                result.Blocks.push_back(ExtractedTextBlock{slideLabel + " table " + shapeLabel, std::move(text)});
            }
        }
    }

    static ExtractedDocumentText ExtractPowerPoint(PowerPoint::PowerPointDocumentEditor& editor)
    {
        ExtractedDocumentText result;
        result.Family = DocumentFamily::PowerPoint;

        Size slideIndex = 1;
        for (const auto& slide : editor.Slides())
        {
            const std::string slideLabel = "slide " + std::to_string(slideIndex);
            Size shapeIndex = 1;
            if (auto tree = slide->ShapeTree())
            {
                for (const auto& shape : tree->Shapes())
                {
                    CollectShapeText(shape, slideLabel, std::to_string(shapeIndex), result);
                    ++shapeIndex;
                }
            }

            if (auto notes = slide->NotesText(); !notes.empty())
            {
                result.Blocks.push_back(ExtractedTextBlock{slideLabel + " notes", notes});
            }
            ++slideIndex;
        }

        result.Ok = true;
        return result;
    }

    static ExtractedDocumentText ExtractExcel(Excel::ExcelDocumentEditor& editor)
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

    static ExtractedDocumentText ExtractWord(Word::WordDocumentEditor& editor)
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
    static ExtractedDocumentText FailedToOpen(DocumentFamily family, const char* message, const std::filesystem::path& path)
    {
        ExtractedDocumentText result;
        result.Family = family;
        result.Diagnostics.push_back(ToolDiagnostic{ToolSeverity::Error, message, path.string()});
        return result;
    }
};

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
                          : TextExtractorHelper::FailedToOpen(DocumentFamily::Word, "Failed to open Word document", path);
        }
        case DocumentFamily::Excel:
        {
            auto editor = Excel::ExcelDocumentEditor::Open(path, UntrustedOpenSettings());
            return editor ? Extract(*editor)
                          : TextExtractorHelper::FailedToOpen(DocumentFamily::Excel, "Failed to open Excel document", path);
        }
        case DocumentFamily::PowerPoint:
        {
            auto editor = PowerPoint::PowerPointDocumentEditor::Open(path, UntrustedOpenSettings());
            return editor ? Extract(*editor)
                          : TextExtractorHelper::FailedToOpen(DocumentFamily::PowerPoint, "Failed to open PowerPoint document", path);
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
    return TextExtractorHelper::ExtractWord(editor);
}

ExtractedDocumentText Extract(Excel::ExcelDocumentEditor& editor)
{
    return TextExtractorHelper::ExtractExcel(editor);
}

ExtractedDocumentText Extract(PowerPoint::PowerPointDocumentEditor& editor)
{
    return TextExtractorHelper::ExtractPowerPoint(editor);
}

} // namespace ExyokiOffice::Tools
