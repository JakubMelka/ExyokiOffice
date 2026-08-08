// Copyright (c) 2026 Jakub Melka and Collaborators
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "doctest.h"

#include "ExyokiOffice/DOM/DocumentFormat/OpenXml/Wordprocessing.hpp"
#include "ExyokiOffice/Word/WordDocument.hpp"
#include "ExyokiOffice/StandardTypes.hpp"

#include <cmath>
#include <memory>
#include <string_view>

namespace
{
namespace W = ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing;

using ExyokiOffice::Color;
using ExyokiOffice::ColorPreset;
using ExyokiOffice::MeasurementUnit;
using ExyokiOffice::MeasuringUnits;
using ExyokiOffice::Word::ParagraphIndentation;
using ExyokiOffice::Word::ParagraphNumbering;
using ExyokiOffice::Word::ParagraphSpacing;
using ExyokiOffice::Word::ParagraphSpacingLines;
using ExyokiOffice::Word::WordDocumentEditor;

int Twips(const MeasuringUnits& value)
{
    return static_cast<int>(std::lround(value.ToTw().GetValue()));
}

ExyokiOffice::Real Points(const MeasuringUnits& value)
{
    return value.ToPt().GetValue();
}

struct ReopenedParagraph
{
    WordDocumentEditor::Ptr Editor;
    std::shared_ptr<ExyokiOffice::Word::Paragraph> Paragraph;
};

struct ReopenedRun
{
    WordDocumentEditor::Ptr Editor;
    std::shared_ptr<ExyokiOffice::Word::Run> Run;
};

ReopenedParagraph ReopenParagraphByText(const WordDocumentEditor::Ptr& editor, std::string_view text)
{
    ReopenedParagraph result;
    result.Editor = WordDocumentEditor::Open(editor->SaveToMemory());
    REQUIRE(result.Editor != nullptr);
    auto paragraphs = result.Editor->Paragraphs();
    REQUIRE_FALSE(paragraphs.empty());
    for (const auto& paragraph : paragraphs)
    {
        if (paragraph && paragraph->PlainText() == text)
        {
            result.Paragraph = paragraph;
            return result;
        }
    }
    FAIL("Expected paragraph text was not found after round trip");
    return result;
}

ReopenedRun ReopenFirstRunByText(const WordDocumentEditor::Ptr& editor, std::string_view paragraphText)
{
    ReopenedRun result;
    auto paragraph = ReopenParagraphByText(editor, paragraphText);
    result.Editor = paragraph.Editor;
    REQUIRE(paragraph.Paragraph != nullptr);
    auto runs = paragraph.Paragraph->Runs();
    REQUIRE_FALSE(runs.empty());
    result.Run = runs.front();
    return result;
}

} // namespace

TEST_SUITE("WordFormattingTests")
{

    TEST_CASE("Paragraph formatting read API round trips common direct properties [unit] [word] [word-formatting]")
    {
        auto editor = WordDocumentEditor::CreateNew();
        REQUIRE(editor != nullptr);

        auto paragraph = editor->AddParagraph("Formatted paragraph");
        REQUIRE(paragraph != nullptr);
        paragraph->SetStyleId("BodyText")
            .SetAlignment(W::JustificationValues::Center)
            .SetSpacing(MeasuringUnits(6.0, MeasurementUnit::Point),
                        MeasuringUnits(12.0, MeasurementUnit::Point),
                        MeasuringUnits(14.0, MeasurementUnit::Point),
                        W::LineSpacingRuleValues::Exact)
            .SetIndentation(MeasuringUnits(0.5, MeasurementUnit::Inch),
                            MeasuringUnits(0.25, MeasurementUnit::Inch),
                            MeasuringUnits(0.1, MeasurementUnit::Inch),
                            std::nullopt)
            .SetNumbering(9, 2)
            .AddTabStop(MeasuringUnits(1.5, MeasurementUnit::Inch))
            .SetBorders(W::BorderValues::Single, 8, Color(ColorPreset::Blue))
            .SetShading(Color(ColorPreset::Yellow), W::ShadingPatternValues::Percent25, Color(ColorPreset::Red))
            .SetKeepWithNext(true)
            .SetKeepLines(false)
            .SetPageBreakBefore(true)
            .SetWidowControl(false);

        auto immediate = paragraph->GetFormatting();
        REQUIRE(immediate.has_value());
        CHECK(immediate->StyleId == "BodyText");
        REQUIRE(immediate->Alignment.has_value());
        CHECK(immediate->Alignment->GetValue() == W::JustificationValues::Center);

        auto reopened = ReopenParagraphByText(editor, "Formatted paragraph");
        auto reopenedParagraph = reopened.Paragraph;
        REQUIRE(reopenedParagraph != nullptr);

        auto formatting = reopenedParagraph->GetFormatting();
        REQUIRE(formatting.has_value());
        CHECK(formatting->StyleId == "BodyText");
        REQUIRE(formatting->Alignment.has_value());
        CHECK(formatting->Alignment->GetValue() == W::JustificationValues::Center);

        REQUIRE(formatting->Spacing.has_value());
        CHECK(Twips(*formatting->Spacing->Before) == 120);
        CHECK(Twips(*formatting->Spacing->After) == 240);
        CHECK(Twips(*formatting->Spacing->Line) == 280);
        CHECK(formatting->Spacing->LineRule.GetValue() == W::LineSpacingRuleValues::Exact);

        REQUIRE(formatting->Indentation.has_value());
        CHECK(Twips(*formatting->Indentation->Left) == 720);
        CHECK(Twips(*formatting->Indentation->Right) == 360);
        CHECK(Twips(*formatting->Indentation->FirstLine) == 144);
        CHECK_FALSE(formatting->Indentation->Hanging.has_value());

        REQUIRE(formatting->Numbering.has_value());
        CHECK(formatting->Numbering->NumberingId == 9);
        CHECK(formatting->Numbering->Level == 2);

        REQUIRE(formatting->Shading.has_value());
        CHECK(formatting->Shading->FillColor == "FFFF00");
        CHECK(formatting->Shading->PatternColor == "FF0000");
        CHECK(formatting->Shading->Pattern.GetValue() == W::ShadingPatternValues::Percent25);
        CHECK(formatting->HasBorders);
        CHECK(formatting->KeepWithNext == true);
        CHECK(formatting->KeepLines == false);
        CHECK(formatting->PageBreakBefore == true);
        CHECK(formatting->WidowControl == false);

        auto alignment = reopenedParagraph->GetAlignment();
        REQUIRE(alignment.has_value());
        CHECK(alignment->GetValue() == W::JustificationValues::Center);

        ParagraphSpacing spacing;
        REQUIRE(reopenedParagraph->TryGetSpacing(spacing));
        CHECK(Twips(*spacing.Before) == 120);

        ParagraphIndentation indentation;
        REQUIRE(reopenedParagraph->TryGetIndentation(indentation));
        CHECK(Twips(*indentation.Left) == 720);

        ParagraphNumbering numbering;
        REQUIRE(reopenedParagraph->TryGetNumbering(numbering));
        CHECK(numbering.NumberingId == 9);

        auto shading = reopenedParagraph->GetShading();
        REQUIRE(shading.has_value());
        CHECK(shading->FillColor == "FFFF00");
    }

    TEST_CASE("Paragraph spacing lines and clear formatting API are explicit and round trip [unit] [word] [word-formatting]")
    {
        auto editor = WordDocumentEditor::CreateNew();
        REQUIRE(editor != nullptr);

        auto paragraph = editor->AddParagraph("Line spacing");
        REQUIRE(paragraph != nullptr);
        paragraph->SetStyleId("LineStyle")
            .SetAlignment(W::JustificationValues::Both)
            .SetSpacingLines(120, 240, 360, W::LineSpacingRuleValues::Auto)
            .SetIndentation(MeasuringUnits(1.0, MeasurementUnit::Centimeter), std::nullopt)
            .SetKeepWithNext(true);

        auto reopened = ReopenParagraphByText(editor, "Line spacing");
        auto reopenedParagraph = reopened.Paragraph;
        REQUIRE(reopenedParagraph != nullptr);

        ParagraphSpacingLines lines;
        REQUIRE(reopenedParagraph->TryGetSpacingLines(lines));
        CHECK(lines.BeforeLines == 120);
        CHECK(lines.AfterLines == 240);
        CHECK(lines.LineLines == 360);
        CHECK(lines.LineRule.GetValue() == W::LineSpacingRuleValues::Auto);

        reopenedParagraph->ClearFormatting();
        CHECK(reopenedParagraph->PlainText() == "Line spacing");
        CHECK(reopenedParagraph->GetStyleId().empty());

        CHECK_FALSE(reopenedParagraph->GetAlignment().has_value());
        ParagraphSpacing spacing;
        CHECK_FALSE(reopenedParagraph->TryGetSpacing(spacing));
        ParagraphIndentation indentation;
        CHECK_FALSE(reopenedParagraph->TryGetIndentation(indentation));
        CHECK_FALSE(reopenedParagraph->GetKeepWithNext().has_value());
    }

    TEST_CASE("Run formatting read API round trips common direct properties [unit] [word] [word-formatting]")
    {
        auto editor = WordDocumentEditor::CreateNew();
        REQUIRE(editor != nullptr);

        auto paragraph = editor->AddParagraph();
        REQUIRE(paragraph != nullptr);
        auto run = paragraph->AddRun();
        REQUIRE(run != nullptr);
        REQUIRE(run->AddText("Formatted run") != nullptr);
        run->SetStyleId("Strong")
            .SetBold(true)
            .SetItalic(false)
            .SetUnderline(W::UnderlineValues{W::UnderlineValues::Double})
            .SetStrike(true)
            .SetDoubleStrike(false)
            .SetCaps(true)
            .SetSmallCaps(false)
            .SetNoProof(true)
            .SetColor(Color(1, 2, 3))
            .SetHighlight(W::HighlightColorValues::Yellow)
            .SetFont("Aptos", "Aptos Display")
            .SetLanguage("cs-CZ", "ja-JP", "ar-SA")
            .SetKerning(MeasuringUnits(10.0, MeasurementUnit::Point))
            .SetPosition(MeasuringUnits(-2.0, MeasurementUnit::Point))
            .SetSpacing(MeasuringUnits(1.0, MeasurementUnit::Point))
            .SetFontSize(MeasuringUnits(14.0, MeasurementUnit::Point))
            .SetTextEffect(W::TextEffectValues::Sparkle);

        auto reopened = ReopenFirstRunByText(editor, "Formatted run");
        auto reopenedRun = reopened.Run;
        REQUIRE(reopenedRun != nullptr);

        auto formatting = reopenedRun->GetFormatting();
        REQUIRE(formatting.has_value());
        CHECK(formatting->StyleId == "Strong");
        CHECK(formatting->Bold == true);
        CHECK(formatting->Italic == false);
        REQUIRE(formatting->Underline.has_value());
        CHECK(formatting->Underline->GetValue() == W::UnderlineValues::Double);
        CHECK(formatting->Strike == true);
        CHECK(formatting->DoubleStrike == false);
        CHECK(formatting->Caps == true);
        CHECK(formatting->SmallCaps == false);
        CHECK(formatting->NoProof == true);
        CHECK(formatting->Color == "010203");
        REQUIRE(formatting->Highlight.has_value());
        CHECK(formatting->Highlight->GetValue() == W::HighlightColorValues::Yellow);

        REQUIRE(formatting->Fonts.has_value());
        CHECK(formatting->Fonts->Ascii == "Aptos");
        CHECK(formatting->Fonts->HighAnsi == "Aptos Display");
        REQUIRE(formatting->Language.has_value());
        CHECK(formatting->Language->Latin == "cs-CZ");
        CHECK(formatting->Language->EastAsia == "ja-JP");
        CHECK(formatting->Language->Bidi == "ar-SA");

        REQUIRE(formatting->Kerning.has_value());
        CHECK(Points(*formatting->Kerning) == doctest::Approx(10.0));
        REQUIRE(formatting->Position.has_value());
        CHECK(Points(*formatting->Position) == doctest::Approx(-2.0));
        REQUIRE(formatting->Spacing.has_value());
        CHECK(Twips(*formatting->Spacing) == 20);
        REQUIRE(formatting->FontSize.has_value());
        CHECK(Points(*formatting->FontSize) == doctest::Approx(14.0));
        REQUIRE(formatting->TextEffect.has_value());
        CHECK(formatting->TextEffect->GetValue() == W::TextEffectValues::Sparkle);

        auto bold = reopenedRun->GetBold();
        REQUIRE(bold.has_value());
        CHECK(*bold);
        CHECK(reopenedRun->GetColor() == "010203");
        auto fonts = reopenedRun->GetFont();
        REQUIRE(fonts.has_value());
        CHECK(fonts->Ascii == "Aptos");
        auto language = reopenedRun->GetLanguage();
        REQUIRE(language.has_value());
        CHECK(language->Latin == "cs-CZ");
    }

    TEST_CASE("Run clear formatting removes direct formatting while preserving content [unit] [word] [word-formatting]")
    {
        auto editor = WordDocumentEditor::CreateNew();
        REQUIRE(editor != nullptr);

        auto paragraph = editor->AddParagraph();
        REQUIRE(paragraph != nullptr);
        auto run = paragraph->AddRun();
        REQUIRE(run != nullptr);
        REQUIRE(run->AddText("Keep text") != nullptr);
        run->SetStyleId("Emphasis")
            .SetBold()
            .SetItalic()
            .SetUnderline(W::UnderlineValues{W::UnderlineValues::Single})
            .SetColor(Color(ColorPreset::Red))
            .SetHighlight(W::HighlightColorValues::Cyan)
            .SetFont("Consolas")
            .SetLanguage("en-US")
            .SetFontSize(MeasuringUnits(11.0, MeasurementUnit::Point))
            .SetTextEffect(W::TextEffectValues::Lights);

        auto reopened = ReopenFirstRunByText(editor, "Keep text");
        auto reopenedRun = reopened.Run;
        REQUIRE(reopenedRun != nullptr);
        reopenedRun->ClearFormatting();
        CHECK(reopenedRun->PlainText() == "Keep text");

        auto formatting = reopenedRun->GetFormatting();
        REQUIRE(formatting.has_value());
        CHECK(formatting->StyleId.empty());
        CHECK_FALSE(formatting->Bold.has_value());
        CHECK_FALSE(formatting->Italic.has_value());
        CHECK_FALSE(formatting->Underline.has_value());
        CHECK(formatting->Color.empty());
        CHECK_FALSE(formatting->Highlight.has_value());
        CHECK_FALSE(formatting->Fonts.has_value());
        CHECK_FALSE(formatting->Language.has_value());
        CHECK_FALSE(formatting->FontSize.has_value());
        CHECK_FALSE(formatting->TextEffect.has_value());

        auto saved = editor->SaveToMemory();
        REQUIRE_FALSE(saved.empty());
    }

} // TEST_SUITE("WordFormattingTests")
