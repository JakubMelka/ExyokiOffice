// Copyright (c) 2026 Jakub Melka and Collaborators
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#pragma once

#include "ExyokiOffice/Export.hpp"
#include "ExyokiOffice/Tools/PackageModel.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace ExyokiOffice::Tools::Markdown
{

/**
 * @brief One inline node of the Markdown AST.
 *
 * The parser and writer support a pragmatic CommonMark/GFM subset documented
 * in docs/tools/conversion-formats.md: emphasis, strong, strikethrough, `<u>`
 * underline, code spans, links, images, footnote references, and hard breaks.
 */
struct EXYOKIOFFICE_EXPORT MarkdownInline
{
    enum class Type
    {
        Text,        ///< Literal text (unescaped).
        Strong,      ///< **children**
        Emphasis,    ///< *children*
        Strike,      ///< ~~children~~
        Underline,   ///< <u>children</u>
        Code,        ///< `text`
        Link,        ///< [children](target "tooltip")
        Image,       ///< ![text](target "tooltip")
        FootnoteRef, ///< [^label] (label in Text)
        HardBreak    ///< <br>
    };

    Type Kind = Type::Text;
    /// Literal text (Text/Code), alt text (Image), or footnote label (FootnoteRef).
    std::string Text;
    /// Link/image destination.
    std::string Target;
    /// Optional link/image title.
    std::string Tooltip;
    std::vector<MarkdownInline> Children;
};

/**
 * @brief One block node of the Markdown AST.
 *
 * Supported blocks: ATX headings, paragraphs, ordered/unordered lists nested
 * by indentation, GFM pipe tables, fenced code blocks, blockquotes, thematic
 * breaks, and footnote definitions.
 */
struct EXYOKIOFFICE_EXPORT MarkdownBlock
{
    enum class Type
    {
        Heading,
        Paragraph,
        List,
        ListItem,
        Table,
        CodeFence,
        Blockquote,
        ThematicBreak,
        FootnoteDefinition
    };

    Type Kind = Type::Paragraph;

    /// Heading level 1-6.
    int HeadingLevel = 0;
    /// True for ordered lists.
    bool Ordered = false;
    /// Fence info string ("cpp" in ```cpp), or footnote label for FootnoteDefinition.
    std::string Info;
    /// Raw text of a code fence (lines joined with '\n').
    std::string Literal;

    /// Inline content (Heading, Paragraph, and the leading text of ListItem).
    std::vector<MarkdownInline> Inlines;
    /// Child blocks (List items, ListItem sub-blocks, Blockquote and FootnoteDefinition content).
    std::vector<MarkdownBlock> Children;

    /// Table cells: TableRows[row][column] holds the cell's inline content.
    std::vector<std::vector<std::vector<MarkdownInline>>> TableRows;
};

/// A parsed Markdown document.
struct EXYOKIOFFICE_EXPORT MarkdownDocumentAst
{
    std::vector<MarkdownBlock> Blocks;
};

/// Parses Markdown text into the AST. Unsupported constructs degrade to
/// literal text; diagnostics report notable degradations.
EXYOKIOFFICE_EXPORT MarkdownDocumentAst ParseMarkdown(std::string_view text,
                                                      std::vector<ToolDiagnostic>& diagnostics);

/// Renders the AST back to Markdown text with canonical formatting.
EXYOKIOFFICE_EXPORT std::string RenderMarkdown(const MarkdownDocumentAst& document);

/// Escapes Markdown-significant characters in literal text.
EXYOKIOFFICE_EXPORT std::string EscapeMarkdownText(std::string_view text);

} // namespace ExyokiOffice::Tools::Markdown
