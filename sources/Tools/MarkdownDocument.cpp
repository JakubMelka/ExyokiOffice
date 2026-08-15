// Copyright (c) 2026 Jakub Melka and Collaborators
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "ExyokiOffice/Tools/MarkdownDocument.hpp"
#include "ExyokiOffice/StandardTypes.hpp"
#include "AsciiText.hpp"

#include <algorithm>

namespace ExyokiOffice::Tools::Markdown
{

/// File-local parsing helpers behind the Markdown document reader.
class MarkdownDocumentHelper
{
public:
    static bool IsBlankLine(std::string_view line)
    {
        return line.find_first_not_of(" \t") == std::string_view::npos;
    }

    static Size IndentOf(std::string_view line)
    {
        Size indent = 0;
        for (const char c : line)
        {
            if (c == ' ')
            {
                ++indent;
            }
            else if (c == '\t')
            {
                indent += 4;
            }
            else
            {
                break;
            }
        }
        return indent;
    }

    static std::string_view TrimView(std::string_view text)
    {
        const auto first = text.find_first_not_of(" \t");
        if (first == std::string_view::npos)
        {
            return {};
        }
        const auto last = text.find_last_not_of(" \t");
        return text.substr(first, last - first + 1);
    }

    /// Removes up to `count` columns of leading whitespace (tab = 4 columns).
    static std::string_view StripIndent(std::string_view line, Size count)
    {
        Size columns = 0;
        Size offset = 0;
        while (offset < line.size() && columns < count)
        {
            if (line[offset] == ' ')
            {
                ++columns;
                ++offset;
            }
            else if (line[offset] == '\t')
            {
                columns += 4;
                ++offset;
            }
            else
            {
                break;
            }
        }
        return line.substr(offset);
    }

    static bool IsThematicBreakLine(std::string_view line)
    {
        const auto trimmed = TrimView(line);
        if (trimmed.size() < 3)
        {
            return false;
        }
        const char marker = trimmed.front();
        if (marker != '-' && marker != '*' && marker != '_')
        {
            return false;
        }
        for (const char c : trimmed)
        {
            if (c != marker && c != ' ')
            {
                return false;
            }
        }
        return std::count(trimmed.begin(), trimmed.end(), marker) >= 3;
    }

    struct ListMarker
    {
        bool Ordered = false;
        /// Columns from line start to the item content.
        Size ContentIndent = 0;
        /// Columns of indentation before the marker itself.
        Size MarkerIndent = 0;
        std::string_view Content;
    };

    static bool TryParseListMarker(std::string_view line, ListMarker& marker)
    {
        const Size indent = IndentOf(line);
        const auto rest = StripIndent(line, indent);
        if (rest.empty())
        {
            return false;
        }

        Size markerLength = 0;
        bool ordered = false;
        if (rest.front() == '-' || rest.front() == '*' || rest.front() == '+')
        {
            markerLength = 1;
        }
        else if (AsciiText::IsDigit(rest.front()))
        {
            Size digits = 0;
            while (digits < rest.size() && digits < 9 &&
                   AsciiText::IsDigit(rest[digits]))
            {
                ++digits;
            }
            if (digits == 0 || digits >= rest.size() || (rest[digits] != '.' && rest[digits] != ')'))
            {
                return false;
            }
            markerLength = digits + 1;
            ordered = true;
        }
        else
        {
            return false;
        }

        if (markerLength >= rest.size() || rest[markerLength] != ' ')
        {
            return false;
        }
        Size spaces = 0;
        while (markerLength + spaces < rest.size() && rest[markerLength + spaces] == ' ')
        {
            ++spaces;
        }

        marker.Ordered = ordered;
        marker.MarkerIndent = indent;
        marker.ContentIndent = indent + markerLength + spaces;
        marker.Content = rest.substr(markerLength + spaces);
        return true;
    }

    /// True when the line contains an unescaped pipe character.
    static bool HasUnescapedPipe(std::string_view line)
    {
        for (Size i = 0; i < line.size(); ++i)
        {
            if (line[i] == '|' && (i == 0 || line[i - 1] != '\\'))
            {
                return true;
            }
        }
        return false;
    }

    static bool IsTableDelimiterLine(std::string_view line)
    {
        const auto trimmed = TrimView(line);
        if (trimmed.empty() || !HasUnescapedPipe(trimmed))
        {
            return false;
        }
        bool hasDash = false;
        for (const char c : trimmed)
        {
            if (c == '-')
            {
                hasDash = true;
            }
            else if (c != '|' && c != ':' && c != ' ')
            {
                return false;
            }
        }
        return hasDash;
    }

    static bool TryParseFootnoteDefinitionStart(std::string_view line, std::string& label, std::string_view& content)
    {
        const auto trimmed = TrimView(line);
        if (!trimmed.starts_with("[^"))
        {
            return false;
        }
        const auto close = trimmed.find("]:");
        if (close == std::string_view::npos || close <= 2)
        {
            return false;
        }
        label = std::string(trimmed.substr(2, close - 2));
        content = TrimView(trimmed.substr(close + 2));
        return true;
    }

    static bool TryParseAtxHeading(std::string_view line, int& level, std::string_view& content)
    {
        const auto trimmed = TrimView(line);
        Size hashes = 0;
        while (hashes < trimmed.size() && trimmed[hashes] == '#')
        {
            ++hashes;
        }
        if (hashes == 0 || hashes > 6)
        {
            return false;
        }
        if (hashes < trimmed.size() && trimmed[hashes] != ' ')
        {
            return false;
        }
        level = static_cast<int>(hashes);
        content = hashes < trimmed.size() ? TrimView(trimmed.substr(hashes + 1)) : std::string_view();
        return true;
    }

    static bool IsFenceLine(std::string_view line, std::string_view& info)
    {
        const auto trimmed = TrimView(line);
        if (!trimmed.starts_with("```"))
        {
            return false;
        }
        info = TrimView(trimmed.substr(3));
        return true;
    }

    /// True when the line begins a construct that interrupts a paragraph.
    static bool StartsNewBlock(std::string_view line)
    {
        if (IsBlankLine(line))
        {
            return true;
        }
        int level = 0;
        std::string_view ignoredText;
        ListMarker marker;
        std::string label;
        std::string_view fenceInfo;
        return IsThematicBreakLine(line) || TryParseAtxHeading(line, level, ignoredText) ||
               TryParseListMarker(line, marker) || IsFenceLine(line, fenceInfo) ||
               TrimView(line).starts_with(">") || TryParseFootnoteDefinitionStart(line, label, ignoredText);
    }

    // ---------------------------------------------------------------------------
    // Inline parsing
    // ---------------------------------------------------------------------------

    class InlineParser
    {
    public:
        static std::vector<MarkdownInline> Parse(std::string_view text)
        {
            std::vector<MarkdownInline> result;
            std::string literal;

            const auto flushLiteral = [&]()
            {
                if (!literal.empty())
                {
                    MarkdownInline node;
                    node.Kind = MarkdownInline::Type::Text;
                    node.Text = std::move(literal);
                    literal.clear();
                    result.push_back(std::move(node));
                }
            };

            Size i = 0;
            while (i < text.size())
            {
                const char c = text[i];

                if (c == '\\' && i + 1 < text.size() && IsEscapablePunctuation(text[i + 1]))
                {
                    literal += text[i + 1];
                    i += 2;
                    continue;
                }

                if (c == '`')
                {
                    Size open = 0;
                    while (i + open < text.size() && text[i + open] == '`')
                    {
                        ++open;
                    }
                    const auto close = FindBacktickRun(text, i + open, open);
                    if (close != std::string_view::npos)
                    {
                        flushLiteral();
                        MarkdownInline node;
                        node.Kind = MarkdownInline::Type::Code;
                        node.Text = std::string(TrimView(text.substr(i + open, close - i - open)));
                        result.push_back(std::move(node));
                        i = close + open;
                        continue;
                    }
                }

                if (text.substr(i).starts_with("<br>") || text.substr(i).starts_with("<br/>") ||
                    text.substr(i).starts_with("<br />"))
                {
                    flushLiteral();
                    MarkdownInline node;
                    node.Kind = MarkdownInline::Type::HardBreak;
                    result.push_back(std::move(node));
                    i += text.substr(i).starts_with("<br>")
                             ? Size{4}
                             : (text.substr(i).starts_with("<br/>") ? Size{5} : Size{6});
                    continue;
                }

                if (text.substr(i).starts_with("<u>"))
                {
                    const auto close = text.find("</u>", i + 3);
                    if (close != std::string_view::npos)
                    {
                        flushLiteral();
                        MarkdownInline node;
                        node.Kind = MarkdownInline::Type::Underline;
                        node.Children = Parse(text.substr(i + 3, close - i - 3));
                        result.push_back(std::move(node));
                        i = close + 4;
                        continue;
                    }
                }

                if (TryParseDelimited(text, i, "**", MarkdownInline::Type::Strong, result, flushLiteral) ||
                    TryParseDelimited(text, i, "__", MarkdownInline::Type::Strong, result, flushLiteral) ||
                    TryParseDelimited(text, i, "~~", MarkdownInline::Type::Strike, result, flushLiteral) ||
                    TryParseDelimited(text, i, "*", MarkdownInline::Type::Emphasis, result, flushLiteral) ||
                    TryParseDelimited(text, i, "_", MarkdownInline::Type::Emphasis, result, flushLiteral))
                {
                    continue;
                }

                if (c == '!' && i + 1 < text.size() && text[i + 1] == '[')
                {
                    MarkdownInline node;
                    Size next = i;
                    if (TryParseLinkLike(text, next, true, node))
                    {
                        flushLiteral();
                        result.push_back(std::move(node));
                        i = next;
                        continue;
                    }
                }

                if (c == '[')
                {
                    if (text.substr(i).starts_with("[^"))
                    {
                        const auto close = text.find(']', i + 2);
                        if (close != std::string_view::npos && close > i + 2)
                        {
                            flushLiteral();
                            MarkdownInline node;
                            node.Kind = MarkdownInline::Type::FootnoteRef;
                            node.Text = std::string(text.substr(i + 2, close - i - 2));
                            result.push_back(std::move(node));
                            i = close + 1;
                            continue;
                        }
                    }

                    MarkdownInline node;
                    Size next = i;
                    if (TryParseLinkLike(text, next, false, node))
                    {
                        flushLiteral();
                        result.push_back(std::move(node));
                        i = next;
                        continue;
                    }
                }

                literal += c;
                ++i;
            }

            flushLiteral();
            return result;
        }

    private:
        static bool IsEscapablePunctuation(char c)
        {
            return AsciiText::IsPunct(c);
        }

        static Size FindBacktickRun(std::string_view text, Size from, Size length)
        {
            for (Size i = from; i + length <= text.size(); ++i)
            {
                if (text[i] != '`')
                {
                    continue;
                }
                Size run = 0;
                while (i + run < text.size() && text[i + run] == '`')
                {
                    ++run;
                }
                if (run == length)
                {
                    return i;
                }
                i += run;
            }
            return std::string_view::npos;
        }

        template <typename FlushLiteral>
        static bool TryParseDelimited(std::string_view text, Size& i, std::string_view delimiter,
                                      MarkdownInline::Type kind, std::vector<MarkdownInline>& result,
                                      const FlushLiteral& flushLiteral)
        {
            if (!text.substr(i).starts_with(delimiter))
            {
                return false;
            }
            const Size contentStart = i + delimiter.size();
            if (contentStart >= text.size() || text[contentStart] == ' ')
            {
                return false;
            }

            // Find the closing delimiter, skipping escaped characters and code spans.
            Size j = contentStart;
            while (j < text.size())
            {
                if (text[j] == '\\' && j + 1 < text.size())
                {
                    j += 2;
                    continue;
                }
                if (text.substr(j).starts_with(delimiter) && j > contentStart && text[j - 1] != ' ')
                {
                    // "**" must not close a single "*" opener mid-run and vice versa: for
                    // single-character delimiters require the closer not be part of a longer run.
                    if (delimiter.size() == 1 && j + 1 < text.size() && text[j + 1] == delimiter[0])
                    {
                        j += 2;
                        continue;
                    }
                    flushLiteral();
                    MarkdownInline node;
                    node.Kind = kind;
                    node.Children = Parse(text.substr(contentStart, j - contentStart));
                    result.push_back(std::move(node));
                    i = j + delimiter.size();
                    return true;
                }
                ++j;
            }
            return false;
        }

        static bool TryParseLinkLike(std::string_view text, Size& i, bool isImage, MarkdownInline& node)
        {
            const Size bracketStart = isImage ? i + 1 : i;
            Size depth = 0;
            Size j = bracketStart;
            Size closeBracket = std::string_view::npos;
            while (j < text.size())
            {
                if (text[j] == '\\')
                {
                    j += 2;
                    continue;
                }
                if (text[j] == '[')
                {
                    ++depth;
                }
                else if (text[j] == ']')
                {
                    --depth;
                    if (depth == 0)
                    {
                        closeBracket = j;
                        break;
                    }
                }
                ++j;
            }
            if (closeBracket == std::string_view::npos || closeBracket + 1 >= text.size() ||
                text[closeBracket + 1] != '(')
            {
                return false;
            }

            Size parenDepth = 1;
            Size k = closeBracket + 2;
            while (k < text.size() && parenDepth > 0)
            {
                if (text[k] == '\\')
                {
                    k += 2;
                    continue;
                }
                if (text[k] == '(')
                {
                    ++parenDepth;
                }
                else if (text[k] == ')')
                {
                    --parenDepth;
                }
                ++k;
            }
            if (parenDepth != 0)
            {
                return false;
            }

            const auto label = text.substr(bracketStart + 1, closeBracket - bracketStart - 1);
            auto destination = TrimView(text.substr(closeBracket + 2, k - closeBracket - 3));

            std::string tooltip;
            if (destination.size() >= 2 && destination.back() == '"')
            {
                const auto quote = destination.rfind('"', destination.size() - 2);
                if (quote != std::string_view::npos && quote > 0 && destination[quote - 1] == ' ')
                {
                    tooltip = std::string(destination.substr(quote + 1, destination.size() - quote - 2));
                    destination = TrimView(destination.substr(0, quote));
                }
            }

            node = MarkdownInline();
            node.Kind = isImage ? MarkdownInline::Type::Image : MarkdownInline::Type::Link;
            node.Target = std::string(destination);
            node.Tooltip = std::move(tooltip);
            if (isImage)
            {
                // Image alt text stays literal; unescape backslash escapes.
                std::string alt;
                for (Size p = 0; p < label.size(); ++p)
                {
                    if (label[p] == '\\' && p + 1 < label.size())
                    {
                        alt += label[p + 1];
                        ++p;
                    }
                    else
                    {
                        alt += label[p];
                    }
                }
                node.Text = std::move(alt);
            }
            else
            {
                node.Children = Parse(label);
            }
            i = k;
            return true;
        }
    };

    // ---------------------------------------------------------------------------
    // Block parsing
    // ---------------------------------------------------------------------------

    class BlockParser
    {
    public:
        static std::vector<MarkdownBlock> ParseLines(const std::vector<std::string>& lines, Size begin,
                                                     Size end, std::vector<ToolDiagnostic>& diagnostics)
        {
            std::vector<MarkdownBlock> blocks;
            Size i = begin;
            while (i < end)
            {
                const std::string_view line = lines[i];
                if (IsBlankLine(line))
                {
                    ++i;
                    continue;
                }

                std::string_view fenceInfo;
                int headingLevel = 0;
                std::string_view headingText;
                ListMarker marker;
                std::string footnoteLabel;
                std::string_view footnoteContent;

                if (IsThematicBreakLine(line))
                {
                    MarkdownBlock block;
                    block.Kind = MarkdownBlock::Type::ThematicBreak;
                    blocks.push_back(std::move(block));
                    ++i;
                }
                else if (TryParseAtxHeading(line, headingLevel, headingText))
                {
                    MarkdownBlock block;
                    block.Kind = MarkdownBlock::Type::Heading;
                    block.HeadingLevel = headingLevel;
                    block.Inlines = InlineParser::Parse(headingText);
                    blocks.push_back(std::move(block));
                    ++i;
                }
                else if (IsFenceLine(line, fenceInfo))
                {
                    i = ParseCodeFence(lines, i, end, fenceInfo, blocks);
                }
                else if (TrimView(line).starts_with(">"))
                {
                    i = ParseBlockquote(lines, i, end, blocks, diagnostics);
                }
                else if (TryParseFootnoteDefinitionStart(line, footnoteLabel, footnoteContent))
                {
                    i = ParseFootnoteDefinition(lines, i, end, footnoteLabel, footnoteContent, blocks, diagnostics);
                }
                else if (TryParseListMarker(line, marker))
                {
                    i = ParseList(lines, i, end, blocks, diagnostics);
                }
                else if (HasUnescapedPipe(line) && i + 1 < end && IsTableDelimiterLine(lines[i + 1]))
                {
                    i = ParseTable(lines, i, end, blocks);
                }
                else
                {
                    i = ParseParagraph(lines, i, end, blocks);
                }
            }
            return blocks;
        }

        static std::vector<std::vector<MarkdownInline>> ParseTableRow(std::string_view line)
        {
            std::vector<std::vector<MarkdownInline>> cells;
            auto trimmed = TrimView(line);
            if (trimmed.starts_with("|"))
            {
                trimmed.remove_prefix(1);
            }
            if (trimmed.ends_with("|") && (trimmed.size() < 2 || trimmed[trimmed.size() - 2] != '\\'))
            {
                trimmed.remove_suffix(1);
            }

            std::string cell;
            for (Size i = 0; i < trimmed.size(); ++i)
            {
                if (trimmed[i] == '\\' && i + 1 < trimmed.size() && trimmed[i + 1] == '|')
                {
                    cell += "\\|";
                    ++i;
                    continue;
                }
                if (trimmed[i] == '|')
                {
                    cells.push_back(InlineParser::Parse(TrimView(cell)));
                    cell.clear();
                    continue;
                }
                cell += trimmed[i];
            }
            cells.push_back(InlineParser::Parse(TrimView(cell)));
            return cells;
        }

    private:
        static Size ParseCodeFence(const std::vector<std::string>& lines, Size i, Size end,
                                   std::string_view info, std::vector<MarkdownBlock>& blocks)
        {
            MarkdownBlock block;
            block.Kind = MarkdownBlock::Type::CodeFence;
            block.Info = std::string(info);

            std::string literal;
            Size j = i + 1;
            for (; j < end; ++j)
            {
                std::string_view ignored;
                if (IsFenceLine(lines[j], ignored))
                {
                    ++j;
                    break;
                }
                if (!literal.empty())
                {
                    literal += '\n';
                }
                literal += lines[j];
            }
            block.Literal = std::move(literal);
            blocks.push_back(std::move(block));
            return j;
        }

        static Size ParseBlockquote(const std::vector<std::string>& lines, Size i, Size end,
                                    std::vector<MarkdownBlock>& blocks,
                                    std::vector<ToolDiagnostic>& diagnostics)
        {
            std::vector<std::string> inner;
            Size j = i;
            for (; j < end; ++j)
            {
                const auto trimmed = TrimView(lines[j]);
                if (trimmed.starts_with(">"))
                {
                    auto content = trimmed.substr(1);
                    if (content.starts_with(" "))
                    {
                        content.remove_prefix(1);
                    }
                    inner.emplace_back(content);
                }
                else if (!IsBlankLine(lines[j]) && !inner.empty() && !StartsNewBlock(lines[j]))
                {
                    inner.emplace_back(TrimView(lines[j]));
                }
                else
                {
                    break;
                }
            }

            MarkdownBlock block;
            block.Kind = MarkdownBlock::Type::Blockquote;
            block.Children = ParseLines(inner, 0, inner.size(), diagnostics);
            blocks.push_back(std::move(block));
            return j;
        }

        static Size ParseFootnoteDefinition(const std::vector<std::string>& lines, Size i,
                                            Size end, const std::string& label,
                                            std::string_view firstContent,
                                            std::vector<MarkdownBlock>& blocks,
                                            std::vector<ToolDiagnostic>& diagnostics)
        {
            std::vector<std::string> inner;
            inner.emplace_back(firstContent);
            Size j = i + 1;
            for (; j < end; ++j)
            {
                if (IsBlankLine(lines[j]))
                {
                    break;
                }
                if (IndentOf(lines[j]) >= 2)
                {
                    inner.emplace_back(StripIndent(lines[j], 4));
                }
                else if (!StartsNewBlock(lines[j]))
                {
                    inner.emplace_back(TrimView(lines[j]));
                }
                else
                {
                    break;
                }
            }

            MarkdownBlock block;
            block.Kind = MarkdownBlock::Type::FootnoteDefinition;
            block.Info = label;
            block.Children = ParseLines(inner, 0, inner.size(), diagnostics);
            blocks.push_back(std::move(block));
            return j;
        }

        static Size ParseTable(const std::vector<std::string>& lines, Size i, Size end,
                               std::vector<MarkdownBlock>& blocks)
        {
            MarkdownBlock block;
            block.Kind = MarkdownBlock::Type::Table;
            block.TableRows.push_back(ParseTableRow(lines[i]));

            Size j = i + 2; // Skip the delimiter row.
            for (; j < end; ++j)
            {
                if (IsBlankLine(lines[j]) || !HasUnescapedPipe(lines[j]))
                {
                    break;
                }
                block.TableRows.push_back(ParseTableRow(lines[j]));
            }
            blocks.push_back(std::move(block));
            return j;
        }

        static Size ParseParagraph(const std::vector<std::string>& lines, Size i, Size end,
                                   std::vector<MarkdownBlock>& blocks)
        {
            std::string text;
            Size j = i;
            for (; j < end; ++j)
            {
                if (j > i && StartsNewBlock(lines[j]))
                {
                    break;
                }
                if (j > i && HasUnescapedPipe(lines[j]) && j + 1 < end && IsTableDelimiterLine(lines[j + 1]))
                {
                    break;
                }
                if (!text.empty())
                {
                    text += '\n';
                }
                text += TrimView(lines[j]);
            }

            MarkdownBlock block;
            block.Kind = MarkdownBlock::Type::Paragraph;
            // Soft line breaks inside one paragraph become hard breaks so the
            // Word/PowerPoint model can preserve them as explicit line breaks.
            Size start = 0;
            bool first = true;
            while (start <= text.size())
            {
                const auto newline = text.find('\n', start);
                const auto lineText = text.substr(start, newline == std::string::npos ? std::string::npos
                                                                                      : newline - start);
                if (!first)
                {
                    MarkdownInline hardBreak;
                    hardBreak.Kind = MarkdownInline::Type::HardBreak;
                    block.Inlines.push_back(std::move(hardBreak));
                }
                auto parsed = InlineParser::Parse(lineText);
                block.Inlines.insert(block.Inlines.end(), std::make_move_iterator(parsed.begin()),
                                     std::make_move_iterator(parsed.end()));
                first = false;
                if (newline == std::string::npos)
                {
                    break;
                }
                start = newline + 1;
            }
            blocks.push_back(std::move(block));
            return j;
        }

        static Size ParseList(const std::vector<std::string>& lines, Size i, Size end,
                              std::vector<MarkdownBlock>& blocks, std::vector<ToolDiagnostic>& diagnostics)
        {
            ListMarker firstMarker;
            TryParseListMarker(lines[i], firstMarker);

            MarkdownBlock list;
            list.Kind = MarkdownBlock::Type::List;
            list.Ordered = firstMarker.Ordered;

            Size j = i;
            while (j < end)
            {
                ListMarker marker;
                if (!TryParseListMarker(lines[j], marker) || marker.MarkerIndent != firstMarker.MarkerIndent ||
                    marker.Ordered != firstMarker.Ordered)
                {
                    break;
                }

                // Collect this item's lines: the marker line plus any following lines
                // indented to the item content (or lazy paragraph continuations).
                std::vector<std::string> itemLines;
                itemLines.emplace_back(marker.Content);
                ++j;
                while (j < end)
                {
                    const std::string_view next = lines[j];
                    if (IsBlankLine(next))
                    {
                        // A blank line ends the item unless deeper-indented content follows.
                        if (j + 1 < end && !IsBlankLine(lines[j + 1]) &&
                            IndentOf(lines[j + 1]) >= marker.ContentIndent)
                        {
                            itemLines.emplace_back("");
                            ++j;
                            continue;
                        }
                        break;
                    }
                    ListMarker nextMarker;
                    const bool isSiblingItem = TryParseListMarker(next, nextMarker) &&
                                               nextMarker.MarkerIndent <= firstMarker.MarkerIndent;
                    if (isSiblingItem)
                    {
                        break;
                    }
                    if (IndentOf(next) >= marker.ContentIndent)
                    {
                        itemLines.emplace_back(StripIndent(next, marker.ContentIndent));
                        ++j;
                        continue;
                    }
                    if (!StartsNewBlock(next))
                    {
                        itemLines.emplace_back(TrimView(next));
                        ++j;
                        continue;
                    }
                    break;
                }

                MarkdownBlock item;
                item.Kind = MarkdownBlock::Type::ListItem;
                auto itemBlocks = ParseLines(itemLines, 0, itemLines.size(), diagnostics);
                if (!itemBlocks.empty() && itemBlocks.front().Kind == MarkdownBlock::Type::Paragraph)
                {
                    item.Inlines = std::move(itemBlocks.front().Inlines);
                    itemBlocks.erase(itemBlocks.begin());
                }
                item.Children = std::move(itemBlocks);
                list.Children.push_back(std::move(item));
            }

            blocks.push_back(std::move(list));
            return j;
        }
    };

    // ---------------------------------------------------------------------------
    // Rendering
    // ---------------------------------------------------------------------------

    class MarkdownWriter
    {
    public:
        static std::string Render(const MarkdownDocumentAst& document)
        {
            std::string out;
            for (const auto& block : document.Blocks)
            {
                RenderBlock(block, 0, out);
            }
            // Normalize the trailing blank line.
            while (out.ends_with("\n\n"))
            {
                out.pop_back();
            }
            return out;
        }

        static std::string RenderInlines(const std::vector<MarkdownInline>& inlines)
        {
            std::string out;
            for (const auto& node : inlines)
            {
                RenderInline(node, out);
            }
            return out;
        }

    private:
        static void RenderBlock(const MarkdownBlock& block, int indent, std::string& out)
        {
            const std::string prefix(static_cast<Size>(indent) * 4, ' ');
            switch (block.Kind)
            {
                case MarkdownBlock::Type::Heading:
                {
                    out += prefix + std::string(static_cast<Size>(std::clamp(block.HeadingLevel, 1, 6)), '#') +
                           " " + RenderInlines(block.Inlines) + "\n\n";
                    break;
                }
                case MarkdownBlock::Type::Paragraph:
                {
                    out += prefix + RenderInlines(block.Inlines) + "\n\n";
                    break;
                }
                case MarkdownBlock::Type::List:
                {
                    int ordinal = 1;
                    for (const auto& item : block.Children)
                    {
                        const std::string marker = block.Ordered ? std::to_string(ordinal) + ". " : "- ";
                        out += prefix + marker + RenderInlines(item.Inlines) + "\n";
                        for (const auto& child : item.Children)
                        {
                            RenderBlock(child, indent + 1, out);
                        }
                        ++ordinal;
                    }
                    if (indent == 0)
                    {
                        out += "\n";
                    }
                    break;
                }
                case MarkdownBlock::Type::ListItem:
                {
                    // List items are rendered by their owning list.
                    break;
                }
                case MarkdownBlock::Type::Table:
                {
                    Size columns = 0;
                    for (const auto& row : block.TableRows)
                    {
                        columns = std::max(columns, row.size());
                    }
                    bool first = true;
                    for (const auto& row : block.TableRows)
                    {
                        out += prefix + "|";
                        for (Size c = 0; c < columns; ++c)
                        {
                            std::string cell = c < row.size() ? RenderInlines(row[c]) : std::string();
                            out += " " + cell + " |";
                        }
                        out += "\n";
                        if (first)
                        {
                            out += prefix + "|";
                            for (Size c = 0; c < columns; ++c)
                            {
                                out += " --- |";
                            }
                            out += "\n";
                            first = false;
                        }
                    }
                    out += "\n";
                    break;
                }
                case MarkdownBlock::Type::CodeFence:
                {
                    out += prefix + "```" + block.Info + "\n";
                    Size start = 0;
                    while (start <= block.Literal.size())
                    {
                        const auto newline = block.Literal.find('\n', start);
                        out += prefix +
                               block.Literal.substr(start, newline == std::string::npos ? std::string::npos
                                                                                        : newline - start) +
                               "\n";
                        if (newline == std::string::npos)
                        {
                            break;
                        }
                        start = newline + 1;
                    }
                    if (block.Literal.empty())
                    {
                        // The loop above emitted one empty line for empty literals; keep it.
                    }
                    out += prefix + "```\n\n";
                    break;
                }
                case MarkdownBlock::Type::Blockquote:
                {
                    std::string inner;
                    for (const auto& child : block.Children)
                    {
                        RenderBlock(child, 0, inner);
                    }
                    while (inner.ends_with("\n"))
                    {
                        inner.pop_back();
                    }
                    Size start = 0;
                    while (start <= inner.size())
                    {
                        const auto newline = inner.find('\n', start);
                        const auto lineText = inner.substr(start, newline == std::string::npos ? std::string::npos
                                                                                               : newline - start);
                        out += prefix + (lineText.empty() ? ">" : "> " + lineText) + "\n";
                        if (newline == std::string::npos)
                        {
                            break;
                        }
                        start = newline + 1;
                    }
                    out += "\n";
                    break;
                }
                case MarkdownBlock::Type::ThematicBreak:
                {
                    out += prefix + "---\n\n";
                    break;
                }
                case MarkdownBlock::Type::FootnoteDefinition:
                {
                    out += prefix + "[^" + block.Info + "]: ";
                    std::string inner;
                    for (const auto& child : block.Children)
                    {
                        RenderBlock(child, 0, inner);
                    }
                    while (inner.ends_with("\n"))
                    {
                        inner.pop_back();
                    }
                    // Continuation lines of a footnote definition are indented.
                    std::string indented;
                    Size start = 0;
                    bool first = true;
                    while (start <= inner.size())
                    {
                        const auto newline = inner.find('\n', start);
                        const auto lineText = inner.substr(start, newline == std::string::npos ? std::string::npos
                                                                                               : newline - start);
                        if (!first)
                        {
                            indented += "\n    ";
                        }
                        indented += lineText;
                        first = false;
                        if (newline == std::string::npos)
                        {
                            break;
                        }
                        start = newline + 1;
                    }
                    out += indented + "\n\n";
                    break;
                }
            }
        }

        static void RenderInline(const MarkdownInline& node, std::string& out)
        {
            switch (node.Kind)
            {
                case MarkdownInline::Type::Text:
                    out += EscapeMarkdownText(node.Text);
                    break;
                case MarkdownInline::Type::Strong:
                    out += "**" + RenderInlines(node.Children) + "**";
                    break;
                case MarkdownInline::Type::Emphasis:
                    out += "*" + RenderInlines(node.Children) + "*";
                    break;
                case MarkdownInline::Type::Strike:
                    out += "~~" + RenderInlines(node.Children) + "~~";
                    break;
                case MarkdownInline::Type::Underline:
                    out += "<u>" + RenderInlines(node.Children) + "</u>";
                    break;
                case MarkdownInline::Type::Code:
                    out += "`" + node.Text + "`";
                    break;
                case MarkdownInline::Type::Link:
                    out += "[" + RenderInlines(node.Children) + "](" + node.Target +
                           (node.Tooltip.empty() ? "" : " \"" + node.Tooltip + "\"") + ")";
                    break;
                case MarkdownInline::Type::Image:
                    out += "![" + EscapeMarkdownText(node.Text) + "](" + node.Target +
                           (node.Tooltip.empty() ? "" : " \"" + node.Tooltip + "\"") + ")";
                    break;
                case MarkdownInline::Type::FootnoteRef:
                    out += "[^" + node.Text + "]";
                    break;
                case MarkdownInline::Type::HardBreak:
                    out += "<br>";
                    break;
            }
        }
    };
};

MarkdownDocumentAst ParseMarkdown(std::string_view text, std::vector<ToolDiagnostic>& diagnostics)
{
    // Normalize line endings and split into lines.
    std::vector<std::string> lines;
    std::string current;
    for (const char c : text)
    {
        if (c == '\r')
        {
            continue;
        }
        if (c == '\n')
        {
            lines.push_back(std::move(current));
            current.clear();
            continue;
        }
        current += c;
    }
    if (!current.empty())
    {
        lines.push_back(std::move(current));
    }

    MarkdownDocumentAst document;
    document.Blocks = MarkdownDocumentHelper::BlockParser::ParseLines(lines, 0, lines.size(), diagnostics);
    return document;
}

std::string RenderMarkdown(const MarkdownDocumentAst& document)
{
    return MarkdownDocumentHelper::MarkdownWriter::Render(document);
}

std::string EscapeMarkdownText(std::string_view text)
{
    std::string out;
    out.reserve(text.size());
    for (const char c : text)
    {
        switch (c)
        {
            case '\\':
            case '`':
            case '*':
            case '_':
            case '[':
            case ']':
            case '|':
            case '~':
            case '<':
            case '#':
                out += '\\';
                out += c;
                break;
            default:
                out += c;
                break;
        }
    }
    return out;
}

} // namespace ExyokiOffice::Tools::Markdown
