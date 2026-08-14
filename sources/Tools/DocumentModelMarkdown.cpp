// Copyright (c) 2026 Jakub Melka and Collaborators
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "ExyokiOffice/Tools/DocumentModelIO.hpp"

#include "ExyokiOffice/Excel/ExcelAddress.hpp"
#include "ExyokiOffice/Tools/MarkdownDocument.hpp"
#include "ExyokiOffice/StandardTypes.hpp"

#include <algorithm>
#include <charconv>
#include <map>

namespace ExyokiOffice::Tools
{

namespace Md = ExyokiOffice::Tools::Markdown;

/// File-local writers behind the Markdown projection of the document model.
class DocumentModelMarkdownHelper
{
public:
    static void Warn(std::vector<ToolDiagnostic>& diagnostics, std::string message, std::string context = {})
    {
        diagnostics.push_back(ToolDiagnostic{ToolSeverity::Warning, std::move(message), std::move(context)});
    }

    static Md::MarkdownInline MakeText(std::string text)
    {
        Md::MarkdownInline node;
        node.Kind = Md::MarkdownInline::Type::Text;
        node.Text = std::move(text);
        return node;
    }

    static Md::MarkdownInline MakeWrapped(Md::MarkdownInline::Type kind, std::vector<Md::MarkdownInline> children)
    {
        Md::MarkdownInline node;
        node.Kind = kind;
        node.Children = std::move(children);
        return node;
    }

    static bool IsMonospaceFont(const std::string& font)
    {
        return font == "Consolas" || font == "Courier New" || font == "Courier";
    }

    static std::string InlinePlainText(const std::vector<Md::MarkdownInline>& inlines)
    {
        std::string text;
        for (const auto& node : inlines)
        {
            switch (node.Kind)
            {
                case Md::MarkdownInline::Type::Text:
                case Md::MarkdownInline::Type::Code:
                    text += node.Text;
                    break;
                case Md::MarkdownInline::Type::HardBreak:
                    text += '\n';
                    break;
                case Md::MarkdownInline::Type::Image:
                case Md::MarkdownInline::Type::FootnoteRef:
                    break;
                default:
                    text += InlinePlainText(node.Children);
                    break;
            }
        }
        return text;
    }

    static const MediaReference* FindMedia(const DocumentModel& model, const std::string& mediaId)
    {
        for (const auto& media : model.Media)
        {
            if (media.Id == mediaId)
            {
                return &media;
            }
        }
        return nullptr;
    }

    // ===========================================================================
    // Word -> Markdown
    // ===========================================================================

    class WordToMarkdown
    {
    public:
        WordToMarkdown(const DocumentModel& model, std::vector<ToolDiagnostic>& diagnostics)
            : m_model(model), m_word(*model.Word), m_diagnostics(diagnostics)
        {
        }

        Md::MarkdownDocumentAst Build()
        {
            Md::MarkdownDocumentAst ast;
            ConvertBlocks(m_word.Body, ast.Blocks);

            for (const auto& note : m_word.Footnotes)
            {
                ast.Blocks.push_back(NoteDefinition(note, std::to_string(note.Id)));
            }
            for (const auto& note : m_word.Endnotes)
            {
                ast.Blocks.push_back(NoteDefinition(note, "e" + std::to_string(note.Id)));
            }

            if (!m_word.Headers.empty() || !m_word.Footers.empty())
            {
                Warn(m_diagnostics, "Headers and footers are not representable in Markdown (kept in JSON)");
            }
            if (!m_word.Comments.empty())
            {
                Warn(m_diagnostics, "Comments are not representable in Markdown (kept in JSON)");
            }
            return ast;
        }

    private:
        void ConvertBlocks(const std::vector<WordBlock>& blocks, std::vector<Md::MarkdownBlock>& out)
        {
            Size i = 0;
            while (i < blocks.size())
            {
                const auto& block = blocks[i];
                if (block.Kind == WordBlock::Type::SectionBreak)
                {
                    Md::MarkdownBlock breakBlock;
                    breakBlock.Kind = Md::MarkdownBlock::Type::ThematicBreak;
                    out.push_back(std::move(breakBlock));
                    ++i;
                    continue;
                }
                if (block.Kind == WordBlock::Type::Table)
                {
                    if (block.Table)
                    {
                        out.push_back(ConvertTable(*block.Table));
                    }
                    ++i;
                    continue;
                }
                if (!block.Paragraph)
                {
                    ++i;
                    continue;
                }
                const auto& paragraph = *block.Paragraph;

                if (paragraph.List)
                {
                    i = ConvertListRun(blocks, i, out);
                    continue;
                }
                if (paragraph.StyleId == "Quote" || paragraph.StyleId == "IntenseQuote")
                {
                    i = ConvertQuoteRun(blocks, i, out);
                    continue;
                }
                if (paragraph.StyleId == "HTMLPreformatted")
                {
                    i = ConvertCodeRun(blocks, i, out);
                    continue;
                }

                Md::MarkdownBlock markdownBlock;
                if (paragraph.HeadingLevel > 0)
                {
                    markdownBlock.Kind = Md::MarkdownBlock::Type::Heading;
                    markdownBlock.HeadingLevel = std::min(paragraph.HeadingLevel, 6);
                    if (paragraph.HeadingLevel > 6)
                    {
                        Warn(m_diagnostics, "Heading level above 6 clamped in Markdown",
                             "Heading" + std::to_string(paragraph.HeadingLevel));
                    }
                }
                else
                {
                    markdownBlock.Kind = Md::MarkdownBlock::Type::Paragraph;
                }
                markdownBlock.Inlines = ConvertInlines(paragraph.Inlines);
                out.push_back(std::move(markdownBlock));
                ++i;
            }
        }

        /// Converts a run of consecutive list paragraphs into one (nested) list block.
        Size ConvertListRun(const std::vector<WordBlock>& blocks, Size start,
                            std::vector<Md::MarkdownBlock>& out)
        {
            struct Item
            {
                int Level = 0;
                std::vector<Md::MarkdownInline> Inlines;
            };
            std::vector<Item> items;
            const WordBlock& first = blocks[start];
            if (!first.Paragraph || !first.Paragraph->List)
            {
                return start + 1;
            }
            const int numberingId = first.Paragraph->List->NumberingId;

            Size i = start;
            for (; i < blocks.size(); ++i)
            {
                const WordBlock& block = blocks[i];
                if (block.Kind != WordBlock::Type::Paragraph || !block.Paragraph)
                {
                    break;
                }
                const WordParagraph& paragraph = *block.Paragraph;
                if (!paragraph.List || paragraph.List->NumberingId != numberingId)
                {
                    break;
                }
                Item item;
                item.Level = paragraph.List->Level;
                item.Inlines = ConvertInlines(paragraph.Inlines);
                items.push_back(std::move(item));
            }

            const bool ordered = IsOrderedList(numberingId);
            Size index = 0;
            while (index < items.size())
            {
                out.push_back(BuildList(items, index, items[index].Level, ordered));
            }
            return i;
        }

        template <typename Items>
        Md::MarkdownBlock BuildList(const Items& items, Size& index, int level, bool ordered)
        {
            Md::MarkdownBlock list;
            list.Kind = Md::MarkdownBlock::Type::List;
            list.Ordered = ordered;
            while (index < items.size() && items[index].Level >= level)
            {
                if (items[index].Level > level)
                {
                    if (list.Children.empty())
                    {
                        Md::MarkdownBlock item;
                        item.Kind = Md::MarkdownBlock::Type::ListItem;
                        list.Children.push_back(std::move(item));
                    }
                    list.Children.back().Children.push_back(
                        BuildList(items, index, items[index].Level, ordered));
                    continue;
                }
                Md::MarkdownBlock item;
                item.Kind = Md::MarkdownBlock::Type::ListItem;
                item.Inlines = items[index].Inlines;
                list.Children.push_back(std::move(item));
                ++index;
            }
            return list;
        }

        bool IsOrderedList(int numberingId) const
        {
            for (const auto& definition : m_word.Lists)
            {
                if (definition.NumberingId == numberingId)
                {
                    if (!definition.Levels.empty())
                    {
                        return definition.Levels.front().Format != "bullet";
                    }
                    break;
                }
            }
            return false;
        }

        Size ConvertQuoteRun(const std::vector<WordBlock>& blocks, Size start,
                             std::vector<Md::MarkdownBlock>& out)
        {
            Md::MarkdownBlock quote;
            quote.Kind = Md::MarkdownBlock::Type::Blockquote;
            Size i = start;
            for (; i < blocks.size(); ++i)
            {
                const WordBlock& block = blocks[i];
                if (block.Kind != WordBlock::Type::Paragraph || !block.Paragraph)
                {
                    break;
                }
                const WordParagraph& source = *block.Paragraph;
                if (source.StyleId != "Quote" && source.StyleId != "IntenseQuote")
                {
                    break;
                }
                Md::MarkdownBlock paragraph;
                paragraph.Kind = Md::MarkdownBlock::Type::Paragraph;
                paragraph.Inlines = ConvertInlines(source.Inlines);
                quote.Children.push_back(std::move(paragraph));
            }
            out.push_back(std::move(quote));
            return i;
        }

        Size ConvertCodeRun(const std::vector<WordBlock>& blocks, Size start,
                            std::vector<Md::MarkdownBlock>& out)
        {
            Md::MarkdownBlock fence;
            fence.Kind = Md::MarkdownBlock::Type::CodeFence;
            std::string literal;
            Size i = start;
            for (; i < blocks.size(); ++i)
            {
                const WordBlock& block = blocks[i];
                if (block.Kind != WordBlock::Type::Paragraph || !block.Paragraph)
                {
                    break;
                }
                const WordParagraph& paragraph = *block.Paragraph;
                if (paragraph.StyleId != "HTMLPreformatted")
                {
                    break;
                }
                if (!literal.empty())
                {
                    literal += '\n';
                }
                for (const auto& node : paragraph.Inlines)
                {
                    if (node.Kind == WordInline::Type::Text)
                    {
                        literal += node.Text;
                    }
                    else if (node.Kind == WordInline::Type::Break)
                    {
                        literal += '\n';
                    }
                }
            }
            fence.Literal = std::move(literal);
            out.push_back(std::move(fence));
            return i;
        }

        Md::MarkdownBlock ConvertTable(const WordTable& table)
        {
            Md::MarkdownBlock markdownTable;
            markdownTable.Kind = Md::MarkdownBlock::Type::Table;
            bool spansFlattened = false;
            for (const auto& row : table.Rows)
            {
                std::vector<std::vector<Md::MarkdownInline>> cells;
                for (const auto& cell : row.Cells)
                {
                    if (cell.Covered)
                    {
                        spansFlattened = true;
                        cells.emplace_back();
                        continue;
                    }
                    if (cell.RowSpan > 1 || cell.ColSpan > 1)
                    {
                        spansFlattened = true;
                    }
                    cells.push_back(CellInlines(cell));
                }
                markdownTable.TableRows.push_back(std::move(cells));
            }
            if (spansFlattened)
            {
                Warn(m_diagnostics, "Merged table cells flattened in Markdown (kept in JSON)");
            }
            return markdownTable;
        }

        std::vector<Md::MarkdownInline> CellInlines(const WordTableCell& cell)
        {
            std::vector<Md::MarkdownInline> inlines;
            bool first = true;
            for (const auto& block : cell.Blocks)
            {
                if (block.Kind == WordBlock::Type::Paragraph && block.Paragraph)
                {
                    if (!first)
                    {
                        Md::MarkdownInline hardBreak;
                        hardBreak.Kind = Md::MarkdownInline::Type::HardBreak;
                        inlines.push_back(std::move(hardBreak));
                    }
                    auto converted = ConvertInlines(block.Paragraph->Inlines);
                    inlines.insert(inlines.end(), std::make_move_iterator(converted.begin()),
                                   std::make_move_iterator(converted.end()));
                    first = false;
                }
                else if (block.Kind == WordBlock::Type::Table)
                {
                    Warn(m_diagnostics, "Nested table flattened in Markdown (kept in JSON)");
                }
            }
            return inlines;
        }

        std::vector<Md::MarkdownInline> ConvertInlines(const std::vector<WordInline>& inlines)
        {
            std::vector<Md::MarkdownInline> out;
            for (const auto& node : inlines)
            {
                switch (node.Kind)
                {
                    case WordInline::Type::Text:
                    {
                        out.push_back(FormatText(node));
                        break;
                    }
                    case WordInline::Type::Hyperlink:
                    {
                        Md::MarkdownInline link;
                        link.Kind = Md::MarkdownInline::Type::Link;
                        link.Target = !node.Target.empty() ? node.Target : "#" + node.Anchor;
                        link.Tooltip = node.Tooltip;
                        link.Children = ConvertInlines(node.Children);
                        out.push_back(std::move(link));
                        break;
                    }
                    case WordInline::Type::Image:
                    {
                        Md::MarkdownInline image;
                        image.Kind = Md::MarkdownInline::Type::Image;
                        image.Text = node.AltText;
                        if (const auto* media = FindMedia(m_model, node.MediaId))
                        {
                            image.Target = media->FileName;
                        }
                        if (image.Target.empty())
                        {
                            Warn(m_diagnostics, "Image has no exported media file reference", node.MediaId);
                            image.Target = node.MediaId;
                        }
                        out.push_back(std::move(image));
                        break;
                    }
                    case WordInline::Type::FootnoteRef:
                    {
                        Md::MarkdownInline reference;
                        reference.Kind = Md::MarkdownInline::Type::FootnoteRef;
                        reference.Text = std::to_string(node.NoteId);
                        out.push_back(std::move(reference));
                        break;
                    }
                    case WordInline::Type::EndnoteRef:
                    {
                        Md::MarkdownInline reference;
                        reference.Kind = Md::MarkdownInline::Type::FootnoteRef;
                        reference.Text = "e" + std::to_string(node.NoteId);
                        out.push_back(std::move(reference));
                        break;
                    }
                    case WordInline::Type::CommentRef:
                    {
                        // Comment anchors have no Markdown representation.
                        break;
                    }
                    case WordInline::Type::Field:
                    {
                        if (!node.Text.empty())
                        {
                            out.push_back(MakeText(node.Text));
                        }
                        break;
                    }
                    case WordInline::Type::Break:
                    {
                        Md::MarkdownInline hardBreak;
                        hardBreak.Kind = Md::MarkdownInline::Type::HardBreak;
                        out.push_back(std::move(hardBreak));
                        break;
                    }
                }
            }
            return out;
        }

        Md::MarkdownInline FormatText(const WordInline& node)
        {
            if (IsMonospaceFont(node.Props.Font))
            {
                Md::MarkdownInline code;
                code.Kind = Md::MarkdownInline::Type::Code;
                code.Text = node.Text;
                return code;
            }

            Md::MarkdownInline current = MakeText(node.Text);
            if (node.Props.Underline)
            {
                current = MakeWrapped(Md::MarkdownInline::Type::Underline, {std::move(current)});
            }
            if (node.Props.Strike)
            {
                current = MakeWrapped(Md::MarkdownInline::Type::Strike, {std::move(current)});
            }
            if (node.Props.Italic)
            {
                current = MakeWrapped(Md::MarkdownInline::Type::Emphasis, {std::move(current)});
            }
            if (node.Props.Bold)
            {
                current = MakeWrapped(Md::MarkdownInline::Type::Strong, {std::move(current)});
            }
            return current;
        }

        Md::MarkdownBlock NoteDefinition(const WordNote& note, std::string label)
        {
            Md::MarkdownBlock definition;
            definition.Kind = Md::MarkdownBlock::Type::FootnoteDefinition;
            definition.Info = std::move(label);
            for (const auto& block : note.Blocks)
            {
                if (block.Kind == WordBlock::Type::Paragraph && block.Paragraph)
                {
                    Md::MarkdownBlock paragraph;
                    paragraph.Kind = Md::MarkdownBlock::Type::Paragraph;
                    paragraph.Inlines = ConvertInlines(block.Paragraph->Inlines);
                    definition.Children.push_back(std::move(paragraph));
                }
            }
            return definition;
        }

        const DocumentModel& m_model;
        const WordDocumentModel& m_word;
        std::vector<ToolDiagnostic>& m_diagnostics;
    };

    // ===========================================================================
    // Markdown -> Word
    // ===========================================================================

    class MarkdownToWord
    {
    public:
        explicit MarkdownToWord(DocumentModel& model)
            : m_model(model), m_word(*model.Word)
        {
        }

        void Build(const Md::MarkdownDocumentAst& ast)
        {
            for (const auto& block : ast.Blocks)
            {
                ConvertBlock(block, m_word.Body, 0, 0);
            }
        }

    private:
        void ConvertBlock(const Md::MarkdownBlock& block, std::vector<WordBlock>& out, int listId, int listLevel)
        {
            switch (block.Kind)
            {
                case Md::MarkdownBlock::Type::Heading:
                {
                    WordParagraph paragraph;
                    paragraph.HeadingLevel = block.HeadingLevel;
                    paragraph.StyleId = "Heading" + std::to_string(block.HeadingLevel);
                    paragraph.Inlines = ConvertInlines(block.Inlines, WordRunProps{});
                    PushParagraph(out, std::move(paragraph));
                    break;
                }
                case Md::MarkdownBlock::Type::Paragraph:
                {
                    WordParagraph paragraph;
                    paragraph.Inlines = ConvertInlines(block.Inlines, WordRunProps{});
                    if (listId != 0)
                    {
                        paragraph.List = WordListRef{listId, listLevel};
                    }
                    PushParagraph(out, std::move(paragraph));
                    break;
                }
                case Md::MarkdownBlock::Type::List:
                {
                    const int id = listId != 0 ? listId : AllocateList(block.Ordered);
                    const int level = listId != 0 ? listLevel + 1 : 0;
                    for (const auto& item : block.Children)
                    {
                        if (item.Kind != Md::MarkdownBlock::Type::ListItem)
                        {
                            continue;
                        }
                        if (!item.Inlines.empty())
                        {
                            WordParagraph paragraph;
                            paragraph.List = WordListRef{id, level};
                            paragraph.Inlines = ConvertInlines(item.Inlines, WordRunProps{});
                            PushParagraph(out, std::move(paragraph));
                        }
                        for (const auto& child : item.Children)
                        {
                            ConvertBlock(child, out, id, level);
                        }
                    }
                    break;
                }
                case Md::MarkdownBlock::Type::ListItem:
                {
                    // Handled by the owning list.
                    break;
                }
                case Md::MarkdownBlock::Type::Table:
                {
                    WordBlock tableBlock;
                    tableBlock.Kind = WordBlock::Type::Table;
                    WordTable table;
                    for (const auto& row : block.TableRows)
                    {
                        WordTableRow tableRow;
                        for (const auto& cell : row)
                        {
                            WordTableCell tableCell;
                            WordParagraph paragraph;
                            paragraph.Inlines = ConvertInlines(cell, WordRunProps{});
                            WordBlock cellBlock;
                            cellBlock.Kind = WordBlock::Type::Paragraph;
                            cellBlock.Paragraph = std::move(paragraph);
                            tableCell.Blocks.push_back(std::move(cellBlock));
                            tableRow.Cells.push_back(std::move(tableCell));
                        }
                        table.Rows.push_back(std::move(tableRow));
                    }
                    tableBlock.Table = std::move(table);
                    out.push_back(std::move(tableBlock));
                    break;
                }
                case Md::MarkdownBlock::Type::CodeFence:
                {
                    Size start = 0;
                    const auto& literal = block.Literal;
                    while (start <= literal.size())
                    {
                        const auto newline = literal.find('\n', start);
                        const auto line = literal.substr(
                            start, newline == std::string::npos ? std::string::npos : newline - start);
                        WordParagraph paragraph;
                        paragraph.StyleId = "HTMLPreformatted";
                        if (!line.empty())
                        {
                            WordInline text;
                            text.Kind = WordInline::Type::Text;
                            text.Text = line;
                            text.Props.Font = "Consolas";
                            paragraph.Inlines.push_back(std::move(text));
                        }
                        PushParagraph(out, std::move(paragraph));
                        if (newline == std::string::npos)
                        {
                            break;
                        }
                        start = newline + 1;
                    }
                    break;
                }
                case Md::MarkdownBlock::Type::Blockquote:
                {
                    std::vector<WordBlock> inner;
                    for (const auto& child : block.Children)
                    {
                        ConvertBlock(child, inner, 0, 0);
                    }
                    for (auto& innerBlock : inner)
                    {
                        if (innerBlock.Kind == WordBlock::Type::Paragraph && innerBlock.Paragraph &&
                            innerBlock.Paragraph->StyleId.empty())
                        {
                            innerBlock.Paragraph->StyleId = "Quote";
                        }
                        out.push_back(std::move(innerBlock));
                    }
                    break;
                }
                case Md::MarkdownBlock::Type::ThematicBreak:
                {
                    WordBlock sectionBreak;
                    sectionBreak.Kind = WordBlock::Type::SectionBreak;
                    out.push_back(std::move(sectionBreak));
                    break;
                }
                case Md::MarkdownBlock::Type::FootnoteDefinition:
                {
                    bool isEndnote = false;
                    const int id = ParseNoteLabel(block.Info, isEndnote);
                    WordNote note;
                    note.Id = id;
                    for (const auto& child : block.Children)
                    {
                        if (child.Kind == Md::MarkdownBlock::Type::Paragraph)
                        {
                            WordParagraph paragraph;
                            paragraph.Inlines = ConvertInlines(child.Inlines, WordRunProps{});
                            WordBlock paragraphBlock;
                            paragraphBlock.Kind = WordBlock::Type::Paragraph;
                            paragraphBlock.Paragraph = std::move(paragraph);
                            note.Blocks.push_back(std::move(paragraphBlock));
                        }
                    }
                    (isEndnote ? m_word.Endnotes : m_word.Footnotes).push_back(std::move(note));
                    break;
                }
            }
        }

        static void PushParagraph(std::vector<WordBlock>& out, WordParagraph paragraph)
        {
            WordBlock block;
            block.Kind = WordBlock::Type::Paragraph;
            block.Paragraph = std::move(paragraph);
            out.push_back(std::move(block));
        }

        int AllocateList(bool ordered)
        {
            const int id = m_nextListId++;
            WordListDefinition definition;
            definition.NumberingId = id;
            for (int level = 0; level < 9; ++level)
            {
                WordListLevel levelDefinition;
                levelDefinition.Format = ordered ? "decimal" : "bullet";
                levelDefinition.LevelText = ordered ? "%" + std::to_string(level + 1) + "." : "*";
                definition.Levels.push_back(std::move(levelDefinition));
            }
            m_word.Lists.push_back(std::move(definition));
            return id;
        }

        static int ParseNoteLabel(const std::string& label, bool& isEndnote)
        {
            std::string digits = label;
            isEndnote = false;
            if (!digits.empty() && (digits.front() == 'e' || digits.front() == 'E'))
            {
                isEndnote = true;
                digits.erase(digits.begin());
            }
            // A label that is not a number at all - or one that overflows - falls back
            // to the first note; std::atoi would be undefined behaviour on overflow.
            int id = 0;
            const auto parsed = std::from_chars(digits.data(), digits.data() + digits.size(), id);
            if (parsed.ec != std::errc{})
            {
                return 1;
            }
            return id != 0 ? id : 1;
        }

        std::vector<WordInline> ConvertInlines(const std::vector<Md::MarkdownInline>& inlines,
                                               WordRunProps props)
        {
            std::vector<WordInline> out;
            for (const auto& node : inlines)
            {
                switch (node.Kind)
                {
                    case Md::MarkdownInline::Type::Text:
                    {
                        WordInline text;
                        text.Kind = WordInline::Type::Text;
                        text.Text = node.Text;
                        text.Props = props;
                        out.push_back(std::move(text));
                        break;
                    }
                    case Md::MarkdownInline::Type::Code:
                    {
                        WordInline text;
                        text.Kind = WordInline::Type::Text;
                        text.Text = node.Text;
                        text.Props = props;
                        text.Props.Font = "Consolas";
                        out.push_back(std::move(text));
                        break;
                    }
                    case Md::MarkdownInline::Type::Strong:
                    {
                        auto childProps = props;
                        childProps.Bold = true;
                        AppendAll(out, ConvertInlines(node.Children, childProps));
                        break;
                    }
                    case Md::MarkdownInline::Type::Emphasis:
                    {
                        auto childProps = props;
                        childProps.Italic = true;
                        AppendAll(out, ConvertInlines(node.Children, childProps));
                        break;
                    }
                    case Md::MarkdownInline::Type::Strike:
                    {
                        auto childProps = props;
                        childProps.Strike = true;
                        AppendAll(out, ConvertInlines(node.Children, childProps));
                        break;
                    }
                    case Md::MarkdownInline::Type::Underline:
                    {
                        auto childProps = props;
                        childProps.Underline = true;
                        AppendAll(out, ConvertInlines(node.Children, childProps));
                        break;
                    }
                    case Md::MarkdownInline::Type::Link:
                    {
                        WordInline link;
                        link.Kind = WordInline::Type::Hyperlink;
                        if (!node.Target.empty() && node.Target.front() == '#')
                        {
                            link.Anchor = node.Target.substr(1);
                        }
                        else
                        {
                            link.Target = node.Target;
                        }
                        link.Tooltip = node.Tooltip;
                        link.Children = ConvertInlines(node.Children, props);
                        out.push_back(std::move(link));
                        break;
                    }
                    case Md::MarkdownInline::Type::Image:
                    {
                        WordInline image;
                        image.Kind = WordInline::Type::Image;
                        image.AltText = node.Text;
                        image.MediaId = EnsureMedia(node.Target);
                        out.push_back(std::move(image));
                        break;
                    }
                    case Md::MarkdownInline::Type::FootnoteRef:
                    {
                        bool isEndnote = false;
                        const int id = ParseNoteLabel(node.Text, isEndnote);
                        WordInline reference;
                        reference.Kind =
                            isEndnote ? WordInline::Type::EndnoteRef : WordInline::Type::FootnoteRef;
                        reference.NoteId = id;
                        out.push_back(std::move(reference));
                        break;
                    }
                    case Md::MarkdownInline::Type::HardBreak:
                    {
                        WordInline lineBreak;
                        lineBreak.Kind = WordInline::Type::Break;
                        lineBreak.BreakKind = "line";
                        out.push_back(std::move(lineBreak));
                        break;
                    }
                }
            }
            return out;
        }

        static void AppendAll(std::vector<WordInline>& out, std::vector<WordInline> nodes)
        {
            out.insert(out.end(), std::make_move_iterator(nodes.begin()), std::make_move_iterator(nodes.end()));
        }

        std::string EnsureMedia(const std::string& fileReference)
        {
            if (fileReference.empty())
            {
                return {};
            }
            for (const auto& media : m_model.Media)
            {
                if (media.FileName == fileReference)
                {
                    return media.Id;
                }
            }
            MediaReference media;
            media.Id = "media" + std::to_string(m_model.Media.size() + 1);
            media.FileName = fileReference;
            m_model.Media.push_back(std::move(media));
            return m_model.Media.back().Id;
        }

        DocumentModel& m_model;
        WordDocumentModel& m_word;
        int m_nextListId = 1;
    };

    // ===========================================================================
    // Excel <-> Markdown
    // ===========================================================================

    static Md::MarkdownDocumentAst ExcelToMarkdown(const ExcelWorkbookModel& workbook,
                                                   std::vector<ToolDiagnostic>& diagnostics)
    {
        Md::MarkdownDocumentAst ast;
        for (const auto& sheet : workbook.Sheets)
        {
            Md::MarkdownBlock heading;
            heading.Kind = Md::MarkdownBlock::Type::Heading;
            heading.HeadingLevel = 2;
            heading.Inlines.push_back(MakeText(sheet.Name));
            ast.Blocks.push_back(std::move(heading));

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
                maxRow = std::max(maxRow, address->Row().Value());
                maxColumn = std::max(maxColumn, address->Column().Value());
                cells[{address->Row().Value(), address->Column().Value()}] =
                    cell.Type == "formula" ? "=" + cell.Formula : cell.Value;
            }
            if (maxRow == 0 || maxColumn == 0)
            {
                continue;
            }

            Md::MarkdownBlock table;
            table.Kind = Md::MarkdownBlock::Type::Table;
            for (UInt32 row = 1; row <= maxRow; ++row)
            {
                std::vector<std::vector<Md::MarkdownInline>> tableRow;
                for (UInt32 column = 1; column <= maxColumn; ++column)
                {
                    std::vector<Md::MarkdownInline> cellInlines;
                    if (const auto cell = cells.find({row, column}); cell != cells.end())
                    {
                        cellInlines.push_back(MakeText(cell->second));
                    }
                    tableRow.push_back(std::move(cellInlines));
                }
                table.TableRows.push_back(std::move(tableRow));
            }
            ast.Blocks.push_back(std::move(table));

            if (!sheet.Merges.empty() || !sheet.Tables.empty() || !sheet.Hyperlinks.empty())
            {
                Warn(diagnostics, "Merges, tables, and hyperlinks are not representable in Markdown (kept in JSON)",
                     sheet.Name);
            }
        }
        return ast;
    }

    static void MarkdownToExcel(const Md::MarkdownDocumentAst& ast, DocumentModel& model,
                                std::vector<ToolDiagnostic>& diagnostics)
    {
        auto& workbook = model.Excel.emplace();
        ExcelSheetModel* currentSheet = nullptr;

        const auto ensureSheet = [&]() -> ExcelSheetModel&
        {
            if (currentSheet == nullptr)
            {
                ExcelSheetModel sheet;
                sheet.Name = "Sheet" + std::to_string(workbook.Sheets.size() + 1);
                workbook.Sheets.push_back(std::move(sheet));
                currentSheet = &workbook.Sheets.back();
            }
            return *currentSheet;
        };

        for (const auto& block : ast.Blocks)
        {
            if (block.Kind == Md::MarkdownBlock::Type::Heading)
            {
                ExcelSheetModel sheet;
                sheet.Name = InlinePlainText(block.Inlines);
                if (sheet.Name.empty())
                {
                    sheet.Name = "Sheet" + std::to_string(workbook.Sheets.size() + 1);
                }
                workbook.Sheets.push_back(std::move(sheet));
                currentSheet = &workbook.Sheets.back();
                continue;
            }
            if (block.Kind != Md::MarkdownBlock::Type::Table)
            {
                if (block.Kind != Md::MarkdownBlock::Type::ThematicBreak)
                {
                    Warn(diagnostics, "Non-table Markdown content ignored for Excel output");
                }
                continue;
            }

            auto& sheet = ensureSheet();
            for (Size rowIndex = 0; rowIndex < block.TableRows.size(); ++rowIndex)
            {
                const auto& row = block.TableRows[rowIndex];
                for (Size columnIndex = 0; columnIndex < row.size(); ++columnIndex)
                {
                    std::string text = InlinePlainText(row[columnIndex]);
                    if (text.empty())
                    {
                        continue;
                    }
                    const auto address = Excel::CellAddress::TryCreate(
                        static_cast<UInt32>(rowIndex + 1), static_cast<UInt32>(columnIndex + 1));
                    if (!address)
                    {
                        continue;
                    }
                    ExcelCellModel cell;
                    cell.Address = address->ToA1();
                    if (text.front() == '=')
                    {
                        cell.Type = "formula";
                        cell.Formula = text.substr(1);
                    }
                    else if (text == "true" || text == "false")
                    {
                        cell.Type = "bool";
                        cell.Value = text;
                    }
                    else
                    {
                        char* end = nullptr;
                        const Real number = std::strtod(text.c_str(), &end);
                        (void)number;
                        if (end != nullptr && *end == '\0' && end != text.c_str())
                        {
                            cell.Type = "number";
                            cell.Value = text;
                        }
                        else
                        {
                            cell.Type = "string";
                            cell.Value = text;
                        }
                    }
                    sheet.Cells.push_back(std::move(cell));
                }
            }
            // Subsequent tables under the same heading extend the same sheet below
            // the previous content; that is rare, so a fresh sheet is started instead.
            currentSheet = nullptr;
        }
    }

    // ===========================================================================
    // PowerPoint <-> Markdown
    // ===========================================================================

    class PowerPointToMarkdown
    {
    public:
        PowerPointToMarkdown(const DocumentModel& model, std::vector<ToolDiagnostic>& diagnostics)
            : m_model(model), m_diagnostics(diagnostics)
        {
        }

        Md::MarkdownDocumentAst Build()
        {
            Md::MarkdownDocumentAst ast;
            if (!m_model.PowerPoint)
            {
                return ast;
            }
            bool first = true;
            Size slideIndex = 1;
            for (const auto& slide : m_model.PowerPoint->Slides)
            {
                if (!first)
                {
                    Md::MarkdownBlock separator;
                    separator.Kind = Md::MarkdownBlock::Type::ThematicBreak;
                    ast.Blocks.push_back(std::move(separator));
                }
                first = false;
                ConvertSlide(slide, "slide " + std::to_string(slideIndex), ast.Blocks);
                ++slideIndex;
            }
            return ast;
        }

    private:
        void ConvertSlide(const PptSlide& slide, const std::string& context,
                          std::vector<Md::MarkdownBlock>& out)
        {
            if (slide.Hidden)
            {
                Warn(m_diagnostics, "Hidden flag is not representable in Markdown (kept in JSON)", context);
            }
            if (!slide.Comments.empty())
            {
                Warn(m_diagnostics, "Slide comments are not representable in Markdown (kept in JSON)", context);
            }

            for (const auto& shape : slide.Shapes)
            {
                ConvertShape(shape, context, out);
            }

            if (!slide.NotesText.empty())
            {
                Md::MarkdownBlock quote;
                quote.Kind = Md::MarkdownBlock::Type::Blockquote;
                Size start = 0;
                bool firstLine = true;
                while (start <= slide.NotesText.size())
                {
                    const auto newline = slide.NotesText.find('\n', start);
                    const auto line = slide.NotesText.substr(
                        start, newline == std::string::npos ? std::string::npos : newline - start);
                    Md::MarkdownBlock paragraph;
                    paragraph.Kind = Md::MarkdownBlock::Type::Paragraph;
                    paragraph.Inlines.push_back(MakeText(firstLine ? "Notes: " + line : line));
                    quote.Children.push_back(std::move(paragraph));
                    firstLine = false;
                    if (newline == std::string::npos)
                    {
                        break;
                    }
                    start = newline + 1;
                }
                out.push_back(std::move(quote));
            }
        }

        void ConvertShape(const PptShape& shape, const std::string& context,
                          std::vector<Md::MarkdownBlock>& out)
        {
            switch (shape.Kind)
            {
                case PptShape::Type::Placeholder:
                {
                    if ((shape.PlaceholderType == "title" || shape.PlaceholderType == "ctrTitle") && shape.Text)
                    {
                        Md::MarkdownBlock heading;
                        heading.Kind = Md::MarkdownBlock::Type::Heading;
                        heading.HeadingLevel = 1;
                        heading.Inlines = FrameFirstParagraphInlines(*shape.Text);
                        out.push_back(std::move(heading));
                        break;
                    }
                    if (shape.Text)
                    {
                        ConvertTextFrame(*shape.Text, out);
                    }
                    break;
                }
                case PptShape::Type::TextBox:
                {
                    if (shape.Text)
                    {
                        ConvertTextFrame(*shape.Text, out);
                    }
                    break;
                }
                case PptShape::Type::Picture:
                {
                    Md::MarkdownBlock paragraph;
                    paragraph.Kind = Md::MarkdownBlock::Type::Paragraph;
                    Md::MarkdownInline image;
                    image.Kind = Md::MarkdownInline::Type::Image;
                    image.Text = shape.AltText;
                    if (const auto* media = FindMedia(m_model, shape.MediaId))
                    {
                        image.Target = media->FileName;
                    }
                    if (image.Target.empty())
                    {
                        image.Target = shape.MediaId;
                    }
                    paragraph.Inlines.push_back(std::move(image));
                    out.push_back(std::move(paragraph));
                    break;
                }
                case PptShape::Type::Table:
                {
                    if (!shape.Table)
                    {
                        break;
                    }
                    Md::MarkdownBlock table;
                    table.Kind = Md::MarkdownBlock::Type::Table;
                    for (const auto& row : shape.Table->Rows)
                    {
                        std::vector<std::vector<Md::MarkdownInline>> tableRow;
                        for (const auto& cell : row)
                        {
                            if (cell.Covered)
                            {
                                tableRow.emplace_back();
                            }
                            else
                            {
                                tableRow.push_back(FrameFirstParagraphInlines(cell.Text));
                            }
                        }
                        table.TableRows.push_back(std::move(tableRow));
                    }
                    out.push_back(std::move(table));
                    break;
                }
                case PptShape::Type::Group:
                {
                    for (const auto& child : shape.Children)
                    {
                        ConvertShape(child, context, out);
                    }
                    break;
                }
                case PptShape::Type::Other:
                {
                    Warn(m_diagnostics, "Unsupported shape dropped in Markdown", context);
                    break;
                }
            }
        }

        void ConvertTextFrame(const PptTextFrame& frame, std::vector<Md::MarkdownBlock>& out)
        {
            bool hasLevels = false;
            for (const auto& paragraph : frame.Paragraphs)
            {
                if (paragraph.Level > 0)
                {
                    hasLevels = true;
                    break;
                }
            }

            if (!hasLevels)
            {
                for (const auto& paragraph : frame.Paragraphs)
                {
                    Md::MarkdownBlock markdownParagraph;
                    markdownParagraph.Kind = Md::MarkdownBlock::Type::Paragraph;
                    markdownParagraph.Inlines = RunsToInlines(paragraph.Runs);
                    out.push_back(std::move(markdownParagraph));
                }
                return;
            }

            Size index = 0;
            out.push_back(BuildList(frame.Paragraphs, index, frame.Paragraphs.front().Level));
        }

        Md::MarkdownBlock BuildList(const std::vector<PptParagraph>& paragraphs, Size& index, int level)
        {
            Md::MarkdownBlock list;
            list.Kind = Md::MarkdownBlock::Type::List;
            list.Ordered = false;
            while (index < paragraphs.size() && paragraphs[index].Level >= level)
            {
                if (paragraphs[index].Level > level)
                {
                    if (list.Children.empty())
                    {
                        Md::MarkdownBlock item;
                        item.Kind = Md::MarkdownBlock::Type::ListItem;
                        list.Children.push_back(std::move(item));
                    }
                    list.Children.back().Children.push_back(
                        BuildList(paragraphs, index, paragraphs[index].Level));
                    continue;
                }
                Md::MarkdownBlock item;
                item.Kind = Md::MarkdownBlock::Type::ListItem;
                item.Inlines = RunsToInlines(paragraphs[index].Runs);
                list.Children.push_back(std::move(item));
                ++index;
            }
            return list;
        }

        std::vector<Md::MarkdownInline> FrameFirstParagraphInlines(const PptTextFrame& frame)
        {
            std::vector<Md::MarkdownInline> inlines;
            bool first = true;
            for (const auto& paragraph : frame.Paragraphs)
            {
                if (!first)
                {
                    Md::MarkdownInline hardBreak;
                    hardBreak.Kind = Md::MarkdownInline::Type::HardBreak;
                    inlines.push_back(std::move(hardBreak));
                }
                auto converted = RunsToInlines(paragraph.Runs);
                inlines.insert(inlines.end(), std::make_move_iterator(converted.begin()),
                               std::make_move_iterator(converted.end()));
                first = false;
            }
            return inlines;
        }

        std::vector<Md::MarkdownInline> RunsToInlines(const std::vector<PptRun>& runs)
        {
            std::vector<Md::MarkdownInline> inlines;
            for (const auto& run : runs)
            {
                Md::MarkdownInline current = MakeText(run.Text);
                if (run.Underline)
                {
                    current = MakeWrapped(Md::MarkdownInline::Type::Underline, {std::move(current)});
                }
                if (run.Italic)
                {
                    current = MakeWrapped(Md::MarkdownInline::Type::Emphasis, {std::move(current)});
                }
                if (run.Bold)
                {
                    current = MakeWrapped(Md::MarkdownInline::Type::Strong, {std::move(current)});
                }
                if (!run.Hyperlink.empty())
                {
                    Md::MarkdownInline link;
                    link.Kind = Md::MarkdownInline::Type::Link;
                    link.Target = run.Hyperlink;
                    link.Children.push_back(std::move(current));
                    current = std::move(link);
                }
                inlines.push_back(std::move(current));
            }
            return inlines;
        }

        const DocumentModel& m_model;
        std::vector<ToolDiagnostic>& m_diagnostics;
    };

    class MarkdownToPowerPoint
    {
    public:
        MarkdownToPowerPoint(DocumentModel& model, std::vector<ToolDiagnostic>& diagnostics)
            : m_model(model), m_diagnostics(diagnostics)
        {
        }

        void Build(const Md::MarkdownDocumentAst& ast)
        {
            auto& deck = m_model.PowerPoint.emplace();

            std::vector<std::vector<const Md::MarkdownBlock*>> slides;
            slides.emplace_back();
            for (const auto& block : ast.Blocks)
            {
                if (block.Kind == Md::MarkdownBlock::Type::ThematicBreak)
                {
                    slides.emplace_back();
                    continue;
                }
                slides.back().push_back(&block);
            }

            for (const auto& slideBlocks : slides)
            {
                if (slideBlocks.empty())
                {
                    continue;
                }
                deck.Slides.push_back(BuildSlide(slideBlocks));
            }
        }

    private:
        PptSlide BuildSlide(const std::vector<const Md::MarkdownBlock*>& blocks)
        {
            PptSlide slide;
            PptShape* contentShape = nullptr;

            const auto ensureContent = [&]() -> PptShape&
            {
                if (contentShape == nullptr)
                {
                    PptShape shape;
                    shape.Kind = PptShape::Type::TextBox;
                    shape.Text = PptTextFrame{};
                    slide.Shapes.push_back(std::move(shape));
                    contentShape = &slide.Shapes.back();
                }
                return *contentShape;
            };

            // Hands out the text frame of the current content shape, so callers never
            // have to reach through the optional themselves.
            const auto ensureContentText = [&]() -> PptTextFrame&
            {
                PptShape& shape = ensureContent();
                if (!shape.Text)
                {
                    shape.Text = PptTextFrame{};
                }
                return *shape.Text;
            };

            bool haveTitle = false;
            for (const auto* block : blocks)
            {
                switch (block->Kind)
                {
                    case Md::MarkdownBlock::Type::Heading:
                    {
                        if (!haveTitle)
                        {
                            PptShape title;
                            title.Kind = PptShape::Type::Placeholder;
                            title.PlaceholderType = "title";
                            PptTextFrame frame;
                            frame.Paragraphs.push_back(InlinesToParagraph(block->Inlines, 0));
                            title.Text = std::move(frame);
                            slide.Shapes.insert(slide.Shapes.begin(), std::move(title));
                            contentShape = nullptr; // vector reallocation invalidates the pointer
                            haveTitle = true;
                            break;
                        }
                        PptTextFrame& frame = ensureContentText();
                        auto paragraph = InlinesToParagraph(block->Inlines, 0);
                        for (auto& run : paragraph.Runs)
                        {
                            run.Bold = true;
                        }
                        frame.Paragraphs.push_back(std::move(paragraph));
                        break;
                    }
                    case Md::MarkdownBlock::Type::Paragraph:
                    {
                        // A paragraph holding a single image becomes a picture shape.
                        if (block->Inlines.size() == 1 &&
                            block->Inlines.front().Kind == Md::MarkdownInline::Type::Image)
                        {
                            PptShape picture;
                            picture.Kind = PptShape::Type::Picture;
                            picture.AltText = block->Inlines.front().Text;
                            picture.MediaId = EnsureMedia(block->Inlines.front().Target);
                            slide.Shapes.push_back(std::move(picture));
                            contentShape = nullptr;
                            break;
                        }
                        if (IsNotesBlock(*block))
                        {
                            AppendNotes(slide, *block);
                            break;
                        }
                        ensureContentText().Paragraphs.push_back(InlinesToParagraph(block->Inlines, 0));
                        break;
                    }
                    case Md::MarkdownBlock::Type::List:
                    {
                        AppendList(*block, 0, ensureContentText().Paragraphs);
                        break;
                    }
                    case Md::MarkdownBlock::Type::Table:
                    {
                        PptShape tableShape;
                        tableShape.Kind = PptShape::Type::Table;
                        PptTableModel table;
                        for (const auto& row : block->TableRows)
                        {
                            std::vector<PptTableCell> tableRow;
                            for (const auto& cell : row)
                            {
                                PptTableCell tableCell;
                                tableCell.Text.Paragraphs.push_back(InlinesToParagraph(cell, 0));
                                tableRow.push_back(std::move(tableCell));
                            }
                            table.Rows.push_back(std::move(tableRow));
                        }
                        tableShape.Table = std::move(table);
                        slide.Shapes.push_back(std::move(tableShape));
                        contentShape = nullptr;
                        break;
                    }
                    case Md::MarkdownBlock::Type::Blockquote:
                    {
                        if (!block->Children.empty() && IsNotesBlock(block->Children.front()))
                        {
                            for (const auto& child : block->Children)
                            {
                                AppendNotes(slide, child);
                            }
                            break;
                        }
                        for (const auto& child : block->Children)
                        {
                            if (child.Kind == Md::MarkdownBlock::Type::Paragraph)
                            {
                                ensureContentText().Paragraphs.push_back(InlinesToParagraph(child.Inlines, 0));
                            }
                        }
                        break;
                    }
                    case Md::MarkdownBlock::Type::CodeFence:
                    {
                        PptTextFrame& frame = ensureContentText();
                        Size start = 0;
                        while (start <= block->Literal.size())
                        {
                            const auto newline = block->Literal.find('\n', start);
                            const auto line = block->Literal.substr(
                                start, newline == std::string::npos ? std::string::npos : newline - start);
                            PptParagraph paragraph;
                            PptRun run;
                            run.Text = line;
                            paragraph.Runs.push_back(std::move(run));
                            frame.Paragraphs.push_back(std::move(paragraph));
                            if (newline == std::string::npos)
                            {
                                break;
                            }
                            start = newline + 1;
                        }
                        break;
                    }
                    default:
                    {
                        Warn(m_diagnostics, "Markdown block not representable on a slide");
                        break;
                    }
                }
            }
            return slide;
        }

        static bool IsNotesBlock(const Md::MarkdownBlock& block)
        {
            if (block.Kind != Md::MarkdownBlock::Type::Paragraph)
            {
                return false;
            }
            const auto text = InlinePlainText(block.Inlines);
            return text.starts_with("Notes:");
        }

        static void AppendNotes(PptSlide& slide, const Md::MarkdownBlock& block)
        {
            auto text = InlinePlainText(block.Inlines);
            if (text.starts_with("Notes:"))
            {
                text.erase(0, 6);
                if (!text.empty() && text.front() == ' ')
                {
                    text.erase(text.begin());
                }
            }
            if (!slide.NotesText.empty())
            {
                slide.NotesText += '\n';
            }
            slide.NotesText += text;
        }

        void AppendList(const Md::MarkdownBlock& list, int level, std::vector<PptParagraph>& out)
        {
            for (const auto& item : list.Children)
            {
                if (item.Kind != Md::MarkdownBlock::Type::ListItem)
                {
                    continue;
                }
                if (!item.Inlines.empty())
                {
                    out.push_back(InlinesToParagraph(item.Inlines, level == 0 ? 1 : level + 1));
                }
                for (const auto& child : item.Children)
                {
                    if (child.Kind == Md::MarkdownBlock::Type::List)
                    {
                        AppendList(child, level == 0 ? 1 : level + 1, out);
                    }
                }
            }
        }

        PptParagraph InlinesToParagraph(const std::vector<Md::MarkdownInline>& inlines, int level)
        {
            PptParagraph paragraph;
            paragraph.Level = level;
            AppendRuns(inlines, PptRun{}, paragraph.Runs);
            return paragraph;
        }

        void AppendRuns(const std::vector<Md::MarkdownInline>& inlines, PptRun props,
                        std::vector<PptRun>& out)
        {
            for (const auto& node : inlines)
            {
                switch (node.Kind)
                {
                    case Md::MarkdownInline::Type::Text:
                    case Md::MarkdownInline::Type::Code:
                    {
                        PptRun run = props;
                        run.Text = node.Text;
                        out.push_back(std::move(run));
                        break;
                    }
                    case Md::MarkdownInline::Type::Strong:
                    {
                        auto childProps = props;
                        childProps.Bold = true;
                        AppendRuns(node.Children, childProps, out);
                        break;
                    }
                    case Md::MarkdownInline::Type::Emphasis:
                    {
                        auto childProps = props;
                        childProps.Italic = true;
                        AppendRuns(node.Children, childProps, out);
                        break;
                    }
                    case Md::MarkdownInline::Type::Underline:
                    {
                        auto childProps = props;
                        childProps.Underline = true;
                        AppendRuns(node.Children, childProps, out);
                        break;
                    }
                    case Md::MarkdownInline::Type::Strike:
                    {
                        AppendRuns(node.Children, props, out);
                        break;
                    }
                    case Md::MarkdownInline::Type::Link:
                    {
                        auto childProps = props;
                        childProps.Hyperlink = node.Target;
                        AppendRuns(node.Children, childProps, out);
                        break;
                    }
                    case Md::MarkdownInline::Type::Image:
                    {
                        Warn(m_diagnostics, "Inline image moved out of text is not supported; image dropped",
                             node.Target);
                        break;
                    }
                    case Md::MarkdownInline::Type::FootnoteRef:
                    {
                        break;
                    }
                    case Md::MarkdownInline::Type::HardBreak:
                    {
                        PptRun run = props;
                        run.Text = "\n";
                        out.push_back(std::move(run));
                        break;
                    }
                }
            }
        }

        std::string EnsureMedia(const std::string& fileReference)
        {
            if (fileReference.empty())
            {
                return {};
            }
            for (const auto& media : m_model.Media)
            {
                if (media.FileName == fileReference)
                {
                    return media.Id;
                }
            }
            MediaReference media;
            media.Id = "media" + std::to_string(m_model.Media.size() + 1);
            media.FileName = fileReference;
            m_model.Media.push_back(std::move(media));
            return m_model.Media.back().Id;
        }

        DocumentModel& m_model;
        std::vector<ToolDiagnostic>& m_diagnostics;
    };
};

std::string SerializeModelMarkdown(const DocumentModel& model, std::vector<ToolDiagnostic>& diagnostics)
{
    Md::MarkdownDocumentAst ast;
    switch (model.Family)
    {
        case DocumentFamily::Word:
        {
            if (model.Word)
            {
                DocumentModelMarkdownHelper::WordToMarkdown converter(model, diagnostics);
                ast = converter.Build();
            }
            break;
        }
        case DocumentFamily::Excel:
        {
            if (model.Excel)
            {
                ast = DocumentModelMarkdownHelper::ExcelToMarkdown(*model.Excel, diagnostics);
            }
            break;
        }
        case DocumentFamily::PowerPoint:
        {
            if (model.PowerPoint)
            {
                DocumentModelMarkdownHelper::PowerPointToMarkdown converter(model, diagnostics);
                ast = converter.Build();
            }
            break;
        }
        case DocumentFamily::Unknown:
            break;
    }
    return Md::RenderMarkdown(ast);
}

DocumentModel ParseModelMarkdown(std::string_view markdown, DocumentFamily targetFamily,
                                 std::vector<ToolDiagnostic>& diagnostics)
{
    DocumentModel model;
    const auto ast = Md::ParseMarkdown(markdown, diagnostics);

    switch (targetFamily)
    {
        case DocumentFamily::Word:
        {
            model.Family = DocumentFamily::Word;
            model.Word.emplace();
            DocumentModelMarkdownHelper::MarkdownToWord converter(model);
            converter.Build(ast);
            break;
        }
        case DocumentFamily::Excel:
        {
            model.Family = DocumentFamily::Excel;
            DocumentModelMarkdownHelper::MarkdownToExcel(ast, model, diagnostics);
            break;
        }
        case DocumentFamily::PowerPoint:
        {
            model.Family = DocumentFamily::PowerPoint;
            DocumentModelMarkdownHelper::MarkdownToPowerPoint converter(model, diagnostics);
            converter.Build(ast);
            break;
        }
        case DocumentFamily::Unknown:
        {
            diagnostics.push_back(ToolDiagnostic{
                ToolSeverity::Error, "Markdown conversion requires a target Office format"});
            break;
        }
    }
    return model;
}

} // namespace ExyokiOffice::Tools
