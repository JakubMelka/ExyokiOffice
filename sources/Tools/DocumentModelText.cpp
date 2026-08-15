// Copyright (c) 2026 Jakub Melka and Contributors
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "ExyokiOffice/Tools/DocumentModelIO.hpp"

#include "ExyokiOffice/Excel/ExcelAddress.hpp"
#include "ExyokiOffice/StandardTypes.hpp"

#include <algorithm>
#include <map>

namespace ExyokiOffice::Tools
{

/// File-local helpers for the plain-text projection of the document model.
class DocumentModelTextHelper
{
public:
    static std::string InlinesPlainText(const std::vector<WordInline>& inlines)
    {
        std::string text;
        for (const auto& node : inlines)
        {
            switch (node.Kind)
            {
                case WordInline::Type::Text:
                    text += node.Text;
                    break;
                case WordInline::Type::Hyperlink:
                    text += InlinesPlainText(node.Children);
                    break;
                case WordInline::Type::Field:
                    text += node.Text;
                    break;
                case WordInline::Type::Break:
                    text += '\n';
                    break;
                case WordInline::Type::Image:
                case WordInline::Type::FootnoteRef:
                case WordInline::Type::EndnoteRef:
                case WordInline::Type::CommentRef:
                    break;
            }
        }
        return text;
    }

    static std::string BlocksPlainText(const std::vector<WordBlock>& blocks)
    {
        std::string text;
        for (const auto& block : blocks)
        {
            switch (block.Kind)
            {
                case WordBlock::Type::Paragraph:
                {
                    if (block.Paragraph)
                    {
                        text += InlinesPlainText(block.Paragraph->Inlines);
                    }
                    text += '\n';
                    break;
                }
                case WordBlock::Type::Table:
                {
                    if (block.Table)
                    {
                        for (const auto& row : block.Table->Rows)
                        {
                            std::string line;
                            for (const auto& cell : row.Cells)
                            {
                                if (!line.empty())
                                {
                                    line += '\t';
                                }
                                if (!cell.Covered)
                                {
                                    auto cellText = BlocksPlainText(cell.Blocks);
                                    std::replace(cellText.begin(), cellText.end(), '\n', ' ');
                                    while (!cellText.empty() && cellText.back() == ' ')
                                    {
                                        cellText.pop_back();
                                    }
                                    line += cellText;
                                }
                            }
                            text += line + '\n';
                        }
                    }
                    break;
                }
                case WordBlock::Type::SectionBreak:
                    text += '\n';
                    break;
            }
        }
        return text;
    }

    static std::string WordText(const WordDocumentModel& word)
    {
        std::string text = BlocksPlainText(word.Body);

        const auto appendNotes = [&](const std::vector<WordNote>& notes, const char* title)
        {
            if (notes.empty())
            {
                return;
            }
            text += "\n--- " + std::string(title) + " ---\n";
            for (const auto& note : notes)
            {
                text += "[" + std::to_string(note.Id) + "] " + BlocksPlainText(note.Blocks);
            }
        };
        appendNotes(word.Footnotes, "footnotes");
        appendNotes(word.Endnotes, "endnotes");
        return text;
    }

    static std::string ExcelText(const ExcelWorkbookModel& workbook)
    {
        std::string text;
        for (const auto& sheet : workbook.Sheets)
        {
            if (!text.empty())
            {
                text += '\n';
            }
            text += "# " + sheet.Name + '\n';

            std::map<std::pair<UInt32, UInt32>, std::string> cells;
            UInt32 maxRow = 0;
            UInt32 maxColumn = 0;
            for (const auto& cell : sheet.Cells)
            {
                const auto address = Excel::CellAddress::ParseA1(cell.Address);
                if (!address)
                {
                    continue;
                }
                const auto row = address->Row().Value();
                const auto column = address->Column().Value();
                maxRow = std::max(maxRow, row);
                maxColumn = std::max(maxColumn, column);
                cells[{row, column}] = cell.Type == "formula" ? "=" + cell.Formula : cell.Value;
            }

            for (UInt32 row = 1; row <= maxRow; ++row)
            {
                std::string line;
                for (UInt32 column = 1; column <= maxColumn; ++column)
                {
                    if (column > 1)
                    {
                        line += '\t';
                    }
                    if (const auto cell = cells.find({row, column}); cell != cells.end())
                    {
                        line += cell->second;
                    }
                }
                text += line + '\n';
            }
        }
        return text;
    }

    static std::string PptFrameText(const PptTextFrame& frame)
    {
        std::string text;
        for (const auto& paragraph : frame.Paragraphs)
        {
            for (const auto& run : paragraph.Runs)
            {
                text += run.Text;
            }
            text += '\n';
        }
        return text;
    }

    static void PptShapeText(const PptShape& shape, std::string& text)
    {
        switch (shape.Kind)
        {
            case PptShape::Type::TextBox:
            case PptShape::Type::Placeholder:
            {
                if (shape.Text)
                {
                    text += PptFrameText(*shape.Text);
                }
                break;
            }
            case PptShape::Type::Table:
            {
                if (shape.Table)
                {
                    for (const auto& row : shape.Table->Rows)
                    {
                        std::string line;
                        for (const auto& cell : row)
                        {
                            if (!line.empty())
                            {
                                line += '\t';
                            }
                            if (!cell.Covered)
                            {
                                auto cellText = PptFrameText(cell.Text);
                                std::replace(cellText.begin(), cellText.end(), '\n', ' ');
                                while (!cellText.empty() && cellText.back() == ' ')
                                {
                                    cellText.pop_back();
                                }
                                line += cellText;
                            }
                        }
                        text += line + '\n';
                    }
                }
                break;
            }
            case PptShape::Type::Group:
            {
                for (const auto& child : shape.Children)
                {
                    PptShapeText(child, text);
                }
                break;
            }
            case PptShape::Type::Picture:
            case PptShape::Type::Other:
                break;
        }
    }

    static std::string PowerPointText(const PowerPointDeckModel& deck)
    {
        std::string text;
        Size slideIndex = 1;
        for (const auto& slide : deck.Slides)
        {
            if (!text.empty())
            {
                text += '\n';
            }
            text += "## Slide " + std::to_string(slideIndex) + '\n';
            for (const auto& shape : slide.Shapes)
            {
                PptShapeText(shape, text);
            }
            if (!slide.NotesText.empty())
            {
                text += "Notes: " + slide.NotesText + '\n';
            }
            ++slideIndex;
        }
        return text;
    }
};

std::string SerializeModelText(const DocumentModel& model)
{
    switch (model.Family)
    {
        case DocumentFamily::Word:
            return model.Word ? DocumentModelTextHelper::WordText(*model.Word) : std::string();
        case DocumentFamily::Excel:
            return model.Excel ? DocumentModelTextHelper::ExcelText(*model.Excel) : std::string();
        case DocumentFamily::PowerPoint:
            return model.PowerPoint ? DocumentModelTextHelper::PowerPointText(*model.PowerPoint) : std::string();
        case DocumentFamily::Unknown:
            break;
    }
    return {};
}

DocumentModel ParseModelText(std::string_view text, DocumentFamily targetFamily,
                             std::vector<ToolDiagnostic>& diagnostics)
{
    DocumentModel model;
    if (targetFamily != DocumentFamily::Word)
    {
        diagnostics.push_back(ToolDiagnostic{
            ToolSeverity::Error, "Plain text can only be converted to a Word document"});
        return model;
    }

    model.Family = DocumentFamily::Word;
    auto& word = model.Word.emplace();

    std::string line;
    const auto flushLine = [&]()
    {
        WordBlock block;
        block.Kind = WordBlock::Type::Paragraph;
        WordParagraph paragraph;
        if (!line.empty())
        {
            WordInline node;
            node.Kind = WordInline::Type::Text;
            node.Text = line;
            paragraph.Inlines.push_back(std::move(node));
        }
        block.Paragraph = std::move(paragraph);
        word.Body.push_back(std::move(block));
        line.clear();
    };

    for (const char c : text)
    {
        if (c == '\r')
        {
            continue;
        }
        if (c == '\n')
        {
            flushLine();
            continue;
        }
        line += c;
    }
    if (!line.empty())
    {
        flushLine();
    }
    return model;
}

} // namespace ExyokiOffice::Tools
