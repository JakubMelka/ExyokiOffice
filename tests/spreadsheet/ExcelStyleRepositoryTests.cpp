// Copyright (c) 2026 Jakub Melka and Contributors
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "doctest.h"

#include "ExyokiOffice/Excel/ExcelDocument.hpp"
#include "ExyokiOffice/StandardTypes.hpp"

using namespace ExyokiOffice::Excel;

class ExcelStyleTestHelpers final
{
public:
    ExcelStyleTestHelpers() = delete;
    static CellAddress Address(std::string_view text)
    {
        const auto value = CellAddress::ParseA1(text);
        REQUIRE(value);
        return *value;
    }
    static CellRange Range(std::string_view text)
    {
        const auto value = CellRange::ParseA1(text);
        REQUIRE(value);
        return *value;
    }
    static ExyokiOffice::Size Count(std::string_view text, std::string_view token)
    {
        ExyokiOffice::Size count = 0;
        for (ExyokiOffice::Size position = 0; (position = text.find(token, position)) != std::string_view::npos;
             position += token.size())
        {
            ++count;
        }
        return count;
    }
    static ExcelStyle RichStyle()
    {
        ExcelStyle style;
        style.NumberFormat = ExcelNumberFormat{std::nullopt, "#,##0.00;[Red]-#,##0.00"};
        ExcelFont font;
        font.Name = "Aptos Display";
        font.Size = 14.5;
        font.Color = ExcelColor::Rgb("FF336699", -0.2);
        font.Bold = true;
        font.Italic = true;
        font.Underline = ExcelUnderlineStyle::DoubleAccounting;
        font.VerticalAlignment = ExcelFontVerticalAlignment::Superscript;
        font.Family = 2;
        font.CharacterSet = 1;
        font.Scheme = ExcelFontScheme::Major;
        style.Font = font;
        ExcelFill fill;
        fill.Pattern = ExcelFillPattern::Solid;
        fill.Foreground = ExcelColor::Theme(4, 0.25);
        fill.Background = ExcelColor::Indexed(64);
        style.Fill = fill;
        ExcelBorder border;
        border.Left = {ExcelBorderStyle::Thick, ExcelColor::Rgb("FFFF0000")};
        border.Right = {ExcelBorderStyle::Double, ExcelColor::Theme(5)};
        border.Top = {ExcelBorderStyle::Dashed, ExcelColor::Indexed(8)};
        border.Bottom = {ExcelBorderStyle::Thin, ExcelColor::Rgb("FF0000FF")};
        border.Diagonal = {ExcelBorderStyle::DashDot, ExcelColor::Rgb("FF00FF00")};
        border.Horizontal = {ExcelBorderStyle::Dotted, ExcelColor::Automatic()};
        border.Vertical = {ExcelBorderStyle::Hair, std::nullopt};
        border.DiagonalUp = true;
        border.Outline = false;
        style.Border = border;
        ExcelAlignment alignment;
        alignment.Horizontal = ExcelHorizontalAlignment::CenterContinuous;
        alignment.Vertical = ExcelVerticalAlignment::Distributed;
        alignment.TextRotation = 45;
        alignment.WrapText = true;
        alignment.Indent = 2;
        alignment.RelativeIndent = -1;
        alignment.ShrinkToFit = true;
        alignment.ReadingOrder = 2;
        style.Alignment = alignment;
        style.Protection = ExcelProtection{false, true};
        style.QuotePrefix = true;
        return style;
    }
};

TEST_SUITE("ExcelStyleRepositoryTests")
{
    TEST_CASE("Style repository initializes a valid default stylesheet [unit] [excel] [excel-style]")
    {
        auto editor = ExcelDocumentEditor::CreateNew();
        auto styles = editor->Styles();
        CHECK(styles.IsValid());
        CHECK(styles.Count() == 0);
        const auto result = styles.GetOrAdd(ExcelStyle{});
        REQUIRE(result);
        CHECK(result.StyleIndex == 0);
        CHECK(styles.Count() == 1);
        const auto part = editor->GetDocument()->GetWorkbookPart()->GetWorkbookStylesPart();
        REQUIRE(part);
        const auto xml = part->GetXmlString();
        CHECK(xml.find("<x:fonts count=\"1\"") != std::string::npos);
        CHECK(xml.find("<x:fills count=\"2\"") != std::string::npos);
        CHECK(xml.find("<x:borders count=\"1\"") != std::string::npos);
        CHECK(xml.find("<x:cellStyleXfs count=\"1\"") != std::string::npos);
        CHECK(xml.find("<x:cellXfs count=\"1\"") != std::string::npos);
        CHECK(xml.find("name=\"Normal\"") != std::string::npos);
    }

    TEST_CASE("Rich styles deduplicate every component and final XF across service instances [unit] [excel] [excel-style]")
    {
        auto editor = ExcelDocumentEditor::CreateNew();
        auto style = ExcelStyleTestHelpers::RichStyle();
        const auto first = editor->Styles().GetOrAdd(style);
        REQUIRE(first);
        CHECK(first.StyleIndex == 1);
        const auto xmlAfterFirst = editor->GetDocument()->GetWorkbookPart()->GetWorkbookStylesPart()->GetXmlString();
        const auto second = editor->Styles().GetOrAdd(style);
        REQUIRE(second);
        CHECK(second.StyleIndex == first.StyleIndex);
        CHECK(editor->Styles().Count() == 2);
        const auto xmlAfterSecond = editor->GetDocument()->GetWorkbookPart()->GetWorkbookStylesPart()->GetXmlString();
        CHECK(xmlAfterSecond == xmlAfterFirst);
        CHECK(xmlAfterSecond.find("numFmtId=\"164\"") != std::string::npos);
        CHECK(xmlAfterSecond.find("applyFont=\"1\"") != std::string::npos);
        CHECK(xmlAfterSecond.find("applyAlignment=\"1\"") != std::string::npos);
        CHECK(xmlAfterSecond.find("horizontal=\"centerContinuous\"") != std::string::npos);
        CHECK(xmlAfterSecond.find("diagonalUp=\"1\"") != std::string::npos);
    }

    TEST_CASE("Pattern and gradient fills expose rich color and stop options [unit] [excel] [excel-style]")
    {
        auto editor = ExcelDocumentEditor::CreateNew();
        ExcelStyle linearStyle;
        ExcelFill linear;
        linear.Kind = ExcelFillKind::LinearGradient;
        linear.Degree = 37.5;
        linear.GradientStops = {{0.0, ExcelColor::Rgb("FFFF0000")},
                                {0.4, ExcelColor::Theme(3, 0.1)},
                                {1.0, ExcelColor::Indexed(12)}};
        linearStyle.Fill = linear;
        const auto linearResult = editor->Styles().GetOrAdd(linearStyle);
        REQUIRE(linearResult);

        ExcelStyle pathStyle;
        ExcelFill path;
        path.Kind = ExcelFillKind::PathGradient;
        path.Left = 0.1;
        path.Right = 0.2;
        path.Top = 0.3;
        path.Bottom = 0.4;
        path.GradientStops = {{0.0, ExcelColor::Automatic()}, {1.0, ExcelColor::Rgb("FFFFFFFF")}};
        pathStyle.Fill = path;
        const auto pathResult = editor->Styles().GetOrAdd(pathStyle);
        REQUIRE(pathResult);
        CHECK(pathResult.StyleIndex != linearResult.StyleIndex);

        const auto xml = editor->GetDocument()->GetWorkbookPart()->GetWorkbookStylesPart()->GetXmlString();
        CHECK(xml.find("type=\"linear\"") != std::string::npos);
        CHECK(xml.find("degree=\"37.5\"") != std::string::npos);
        CHECK(xml.find("type=\"path\"") != std::string::npos);
        CHECK(ExcelStyleTestHelpers::Count(xml, "<x:stop") == 5);
    }

    TEST_CASE("Styles apply to existing and missing cells without changing values [unit] [excel] [excel-style]")
    {
        auto editor = ExcelDocumentEditor::CreateNew();
        auto sheet = editor->FirstWorksheet();
        REQUIRE(sheet);
        CHECK(sheet->SetCellFormula(ExcelStyleTestHelpers::Address("A1"), "1+1", FormulaCachedValueKind::Number, "2"));
        const auto style = editor->Styles().GetOrAdd(ExcelStyleTestHelpers::RichStyle());
        REQUIRE(style);
        REQUIRE(editor->Styles().ApplyToCell(*sheet, ExcelStyleTestHelpers::Address("A1"), style.StyleIndex));
        REQUIRE(editor->Styles().ApplyToCell(*sheet, ExcelStyleTestHelpers::Address("B2"), style.StyleIndex));
        CHECK(editor->Styles().CellStyleIndex(*sheet, ExcelStyleTestHelpers::Address("A1")) == style.StyleIndex);
        CHECK(editor->Styles().CellStyleIndex(*sheet, ExcelStyleTestHelpers::Address("B2")) == style.StyleIndex);
        CHECK(editor->Styles().CellStyleIndex(*sheet, ExcelStyleTestHelpers::Address("C3")) == 0);
        CHECK(sheet->GetCellValue(ExcelStyleTestHelpers::Address("A1"))->Kind() == CellValueKind::Formula);
        CHECK(sheet->GetCellValue(ExcelStyleTestHelpers::Address("A1"))->FormulaValue().Formula == "1+1");
        CHECK(sheet->ContainsCell(ExcelStyleTestHelpers::Address("B2")));
        CHECK(sheet->GetCellValue(ExcelStyleTestHelpers::Address("B2"))->Kind() == CellValueKind::Blank);
    }

    TEST_CASE("Range style application is complete and invalid indexes are atomic [unit] [excel] [excel-style]")
    {
        auto editor = ExcelDocumentEditor::CreateNew();
        auto sheet = editor->FirstWorksheet();
        REQUIRE(sheet);
        ExcelStyle style;
        style.Alignment = ExcelAlignment{ExcelHorizontalAlignment::Center, ExcelVerticalAlignment::Center};
        const auto registered = editor->Styles().GetOrAdd(style);
        REQUIRE(registered);
        auto result = editor->Styles().ApplyToRange(*sheet, ExcelStyleTestHelpers::Range("B2:D4"), registered.StyleIndex);
        REQUIRE(result);
        CHECK(result.AffectedCellCount == 9);
        CHECK(sheet->StoredCellCount() == 9);
        CHECK(editor->Styles().CellStyleIndex(*sheet, ExcelStyleTestHelpers::Address("D4")) == registered.StyleIndex);
        const auto originalXml = sheet->GetPart()->GetXmlString();
        result = editor->Styles().ApplyToRange(*sheet, ExcelStyleTestHelpers::Range("A1:B2"), 9999);
        CHECK_FALSE(result);
        CHECK(result.Error == RangeOperationError::StyleNotFound);
        CHECK(sheet->GetPart()->GetXmlString() == originalXml);
    }

    TEST_CASE("Invalid rich style values are rejected before package mutation [unit] [excel] [excel-style]")
    {
        auto editor = ExcelDocumentEditor::CreateNew();
        ExcelStyle style;
        ExcelFont font;
        font.Size = -1.0;
        font.Color = ExcelColor::Rgb("BAD");
        style.Font = font;
        auto result = editor->Styles().GetOrAdd(style);
        CHECK_FALSE(result);
        CHECK(result.Status.Error == RangeOperationError::InvalidStyle);
        CHECK_FALSE(editor->GetDocument()->GetWorkbookPart()->GetWorkbookStylesPart());

        style = {};
        ExcelFill gradient;
        gradient.Kind = ExcelFillKind::LinearGradient;
        gradient.GradientStops = {{0.8, ExcelColor::Rgb("FFFFFFFF")}, {0.2, ExcelColor::Rgb("FF000000")}};
        style.Fill = gradient;
        result = editor->Styles().GetOrAdd(style);
        CHECK_FALSE(result);
        CHECK(result.Status.Error == RangeOperationError::InvalidStyle);

        style = {};
        ExcelAlignment alignment;
        alignment.TextRotation = 181;
        style.Alignment = alignment;
        result = editor->Styles().GetOrAdd(style);
        CHECK_FALSE(result);
        CHECK(result.Status.Error == RangeOperationError::InvalidStyle);
    }

    TEST_CASE("Built-in and custom number formats are independently reused [unit] [excel] [excel-style]")
    {
        auto editor = ExcelDocumentEditor::CreateNew();
        ExcelStyle builtin;
        builtin.NumberFormat = ExcelNumberFormat{14, {}};
        const auto builtinResult = editor->Styles().GetOrAdd(builtin);
        REQUIRE(builtinResult);
        ExcelStyle custom;
        custom.NumberFormat = ExcelNumberFormat{std::nullopt, "yyyy-mm-dd hh:mm"};
        const auto customFirst = editor->Styles().GetOrAdd(custom);
        const auto customSecond = editor->Styles().GetOrAdd(custom);
        REQUIRE(customFirst);
        REQUIRE(customSecond);
        CHECK(customFirst.StyleIndex == customSecond.StyleIndex);
        CHECK(customFirst.StyleIndex != builtinResult.StyleIndex);
        const auto xml = editor->GetDocument()->GetWorkbookPart()->GetWorkbookStylesPart()->GetXmlString();
        CHECK(ExcelStyleTestHelpers::Count(xml, "<x:numFmt ") == 1);
        CHECK(xml.find("formatCode=\"yyyy-mm-dd hh:mm\"") != std::string::npos);
    }

    TEST_CASE("Style repository deduplication and assignments survive package round trip [unit] [excel] [excel-style]")
    {
        auto editor = ExcelDocumentEditor::CreateNew();
        auto styleDefinition = ExcelStyleTestHelpers::RichStyle();
        const auto style = editor->Styles().GetOrAdd(styleDefinition);
        REQUIRE(style);
        REQUIRE(editor->Styles().ApplyToRange(*editor->FirstWorksheet(), ExcelStyleTestHelpers::Range("A1:C2"), style.StyleIndex));
        const auto bytes = editor->SaveToMemory();
        REQUIRE_FALSE(bytes.empty());
        auto reopened = ExcelDocumentEditor::Open(bytes);
        REQUIRE(reopened);
        CHECK(reopened->Styles().Count() == 2);
        CHECK(reopened->Styles().CellStyleIndex(*reopened->FirstWorksheet(), ExcelStyleTestHelpers::Address("C2")) == style.StyleIndex);
        const auto duplicate = reopened->Styles().GetOrAdd(styleDefinition);
        REQUIRE(duplicate);
        CHECK(duplicate.StyleIndex == style.StyleIndex);
        CHECK(reopened->Styles().Count() == 2);
    }
}
