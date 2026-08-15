// Copyright (c) 2026 Jakub Melka and Contributors
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "doctest.h"

#include "ExyokiOffice/Tools/MarkdownDocument.hpp"
#include "ExyokiOffice/StandardTypes.hpp"

using namespace ExyokiOffice::Tools;
namespace Md = ExyokiOffice::Tools::Markdown;

namespace
{

const Md::MarkdownInline* FindInline(const std::vector<Md::MarkdownInline>& inlines,
                                     Md::MarkdownInline::Type kind)
{
    for (const auto& node : inlines)
    {
        if (node.Kind == kind)
        {
            return &node;
        }
        if (const auto* nested = FindInline(node.Children, kind))
        {
            return nested;
        }
    }
    return nullptr;
}

} // namespace

TEST_CASE("Markdown parser reads headings, paragraphs, and thematic breaks [unit] [tools] [conversion]")
{
    std::vector<ToolDiagnostic> diagnostics;
    const auto ast = Md::ParseMarkdown("# Title\n\nBody text\n\n---\n\n## Sub *emph*\n", diagnostics);

    REQUIRE(ast.Blocks.size() == 4);
    CHECK(ast.Blocks[0].Kind == Md::MarkdownBlock::Type::Heading);
    CHECK(ast.Blocks[0].HeadingLevel == 1);
    CHECK(ast.Blocks[1].Kind == Md::MarkdownBlock::Type::Paragraph);
    CHECK(ast.Blocks[2].Kind == Md::MarkdownBlock::Type::ThematicBreak);
    CHECK(ast.Blocks[3].Kind == Md::MarkdownBlock::Type::Heading);
    CHECK(ast.Blocks[3].HeadingLevel == 2);
    CHECK(FindInline(ast.Blocks[3].Inlines, Md::MarkdownInline::Type::Emphasis) != nullptr);
}

TEST_CASE("Markdown parser reads inline formatting, code, links, and images [unit] [tools] [conversion]")
{
    std::vector<ToolDiagnostic> diagnostics;
    const auto ast = Md::ParseMarkdown(
        "**bold** *italic* ~~strike~~ <u>under</u> `code` [link](https://example.com \"tip\") "
        "![alt](img.png)<br>after\n",
        diagnostics);

    REQUIRE(ast.Blocks.size() == 1);
    const auto& inlines = ast.Blocks[0].Inlines;
    CHECK(FindInline(inlines, Md::MarkdownInline::Type::Strong) != nullptr);
    CHECK(FindInline(inlines, Md::MarkdownInline::Type::Emphasis) != nullptr);
    CHECK(FindInline(inlines, Md::MarkdownInline::Type::Strike) != nullptr);
    CHECK(FindInline(inlines, Md::MarkdownInline::Type::Underline) != nullptr);
    CHECK(FindInline(inlines, Md::MarkdownInline::Type::HardBreak) != nullptr);

    const auto* code = FindInline(inlines, Md::MarkdownInline::Type::Code);
    REQUIRE(code != nullptr);
    CHECK(code->Text == "code");

    const auto* link = FindInline(inlines, Md::MarkdownInline::Type::Link);
    REQUIRE(link != nullptr);
    CHECK(link->Target == "https://example.com");
    CHECK(link->Tooltip == "tip");

    const auto* image = FindInline(inlines, Md::MarkdownInline::Type::Image);
    REQUIRE(image != nullptr);
    CHECK(image->Target == "img.png");
    CHECK(image->Text == "alt");
}

TEST_CASE("Markdown parser reads nested lists [unit] [tools] [conversion]")
{
    std::vector<ToolDiagnostic> diagnostics;
    const auto ast = Md::ParseMarkdown("- one\n- two\n    - nested\n1. first\n2. second\n", diagnostics);

    REQUIRE(ast.Blocks.size() == 2);
    const auto& bullets = ast.Blocks[0];
    CHECK(bullets.Kind == Md::MarkdownBlock::Type::List);
    CHECK(!bullets.Ordered);
    REQUIRE(bullets.Children.size() == 2);
    REQUIRE(bullets.Children[1].Children.size() == 1);
    CHECK(bullets.Children[1].Children[0].Kind == Md::MarkdownBlock::Type::List);

    const auto& ordered = ast.Blocks[1];
    CHECK(ordered.Kind == Md::MarkdownBlock::Type::List);
    CHECK(ordered.Ordered);
    CHECK(ordered.Children.size() == 2);
}

TEST_CASE("Markdown parser reads GFM tables with escapes [unit] [tools] [conversion]")
{
    std::vector<ToolDiagnostic> diagnostics;
    const auto ast =
        Md::ParseMarkdown("| Name | Value |\n| --- | --- |\n| pipe \\| char | a<br>b |\n", diagnostics);

    REQUIRE(ast.Blocks.size() == 1);
    const auto& table = ast.Blocks[0];
    REQUIRE(table.Kind == Md::MarkdownBlock::Type::Table);
    REQUIRE(table.TableRows.size() == 2);
    REQUIRE(table.TableRows[0].size() == 2);

    // The escaped pipe stays inside one cell.
    REQUIRE(table.TableRows[1].size() == 2);
    REQUIRE(!table.TableRows[1][0].empty());
    CHECK(table.TableRows[1][0].front().Text.find("pipe") != std::string::npos);
    CHECK(FindInline(table.TableRows[1][1], Md::MarkdownInline::Type::HardBreak) != nullptr);
}

TEST_CASE("Markdown parser reads code fences, blockquotes, and footnotes [unit] [tools] [conversion]")
{
    std::vector<ToolDiagnostic> diagnostics;
    const auto ast = Md::ParseMarkdown("```cpp\nint x = 1;\n```\n\n> quoted text\n\nText[^1]\n\n[^1]: The note\n",
                                       diagnostics);

    REQUIRE(ast.Blocks.size() == 4);
    CHECK(ast.Blocks[0].Kind == Md::MarkdownBlock::Type::CodeFence);
    CHECK(ast.Blocks[0].Info == "cpp");
    CHECK(ast.Blocks[0].Literal == "int x = 1;");
    CHECK(ast.Blocks[1].Kind == Md::MarkdownBlock::Type::Blockquote);
    CHECK(ast.Blocks[2].Kind == Md::MarkdownBlock::Type::Paragraph);
    CHECK(FindInline(ast.Blocks[2].Inlines, Md::MarkdownInline::Type::FootnoteRef) != nullptr);
    CHECK(ast.Blocks[3].Kind == Md::MarkdownBlock::Type::FootnoteDefinition);
    CHECK(ast.Blocks[3].Info == "1");
}

TEST_CASE("Markdown render-parse round trip is stable [unit] [tools] [conversion]")
{
    const std::string source = "# Title\n\nText with **bold**, *italic*, and `code`.\n\n"
                               "- item one\n- item two\n    - nested\n\n"
                               "| A | B |\n| --- | --- |\n| 1 | 2 |\n\n"
                               "> quote\n\n---\n";

    std::vector<ToolDiagnostic> diagnostics;
    const auto first = Md::ParseMarkdown(source, diagnostics);
    const auto rendered = Md::RenderMarkdown(first);
    const auto second = Md::ParseMarkdown(rendered, diagnostics);
    const auto renderedAgain = Md::RenderMarkdown(second);

    CHECK(rendered == renderedAgain);
    REQUIRE(first.Blocks.size() == second.Blocks.size());
    for (ExyokiOffice::Size i = 0; i < first.Blocks.size(); ++i)
    {
        CHECK(first.Blocks[i].Kind == second.Blocks[i].Kind);
    }
}

TEST_CASE("Markdown escaping survives a render-parse cycle [unit] [tools] [conversion]")
{
    Md::MarkdownDocumentAst ast;
    Md::MarkdownBlock paragraph;
    paragraph.Kind = Md::MarkdownBlock::Type::Paragraph;
    Md::MarkdownInline text;
    text.Kind = Md::MarkdownInline::Type::Text;
    text.Text = "literal *stars*, _underscores_, [brackets], |pipes|, and #hash";
    paragraph.Inlines.push_back(text);
    ast.Blocks.push_back(paragraph);

    const auto rendered = Md::RenderMarkdown(ast);
    std::vector<ToolDiagnostic> diagnostics;
    const auto parsed = Md::ParseMarkdown(rendered, diagnostics);

    REQUIRE(parsed.Blocks.size() == 1);
    REQUIRE(parsed.Blocks[0].Inlines.size() == 1);
    CHECK(parsed.Blocks[0].Inlines[0].Kind == Md::MarkdownInline::Type::Text);
    CHECK(parsed.Blocks[0].Inlines[0].Text == text.Text);
}
