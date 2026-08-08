// Copyright (c) 2026 Jakub Melka and Collaborators
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "doctest.h"

#include "ExyokiOffice/Excel/ExcelDocument.hpp"
#include "ExyokiOffice/Excel/ExcelFormatting.hpp"

using namespace ExyokiOffice::Excel;

class ExcelAdvancedFormattingTestHelpers final
{
public:
    ExcelAdvancedFormattingTestHelpers() = delete;

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
};

TEST_SUITE("ExcelAdvancedFormattingTests")
{
    TEST_CASE("Number format factories cover built-in scientific date time and percentage formats "
              "[unit] [excel] [excel-advanced-formatting]")
    {
        CHECK(ExcelNumberFormat::General() == ExcelNumberFormat{0, {}});
        CHECK(ExcelNumberFormat::Integer() == ExcelNumberFormat{1, {}});
        CHECK(ExcelNumberFormat::Decimal() == ExcelNumberFormat{2, {}});
        CHECK(ExcelNumberFormat::ThousandsInteger() == ExcelNumberFormat{3, {}});
        CHECK(ExcelNumberFormat::ThousandsDecimal() == ExcelNumberFormat{4, {}});
        CHECK(ExcelNumberFormat::Percent() == ExcelNumberFormat{9, {}});
        CHECK(ExcelNumberFormat::PercentDecimal() == ExcelNumberFormat{10, {}});
        CHECK(ExcelNumberFormat::Scientific() == ExcelNumberFormat{11, {}});
        CHECK(ExcelNumberFormat::ShortDate() == ExcelNumberFormat{14, {}});
        CHECK(ExcelNumberFormat::TimeWithSeconds() == ExcelNumberFormat{21, {}});

        const auto custom = ExcelNumberFormat::Custom("[Blue]0.0000");
        REQUIRE(custom);
        CHECK(custom->FormatCode == "[Blue]0.0000");
        CHECK_FALSE(custom->BuiltInId);
        CHECK_FALSE(ExcelNumberFormat::Custom(""));
    }

    TEST_CASE("Accounting format factory validates input and escapes embedded quotes "
              "[unit] [excel] [excel-advanced-formatting]")
    {
        const auto accounting = ExcelNumberFormat::Accounting("Kč", 2);
        REQUIRE(accounting);
        CHECK_FALSE(accounting->BuiltInId);
        CHECK(accounting->FormatCode.find("\"Kč\"") != std::string::npos);
        CHECK(accounting->FormatCode.find("#,##0.00") != std::string::npos);

        const auto quoted = ExcelNumberFormat::Accounting("A\"B", 0);
        REQUIRE(quoted);
        CHECK(quoted->FormatCode.find("\"A\"\"B\"") != std::string::npos);
        CHECK_FALSE(ExcelNumberFormat::Accounting("", 2));
        CHECK_FALSE(ExcelNumberFormat::Accounting("$", 31));
    }

    TEST_CASE("Advanced cell format combines themed font patterned fill colored borders number and protection "
              "[unit] [excel] [excel-advanced-formatting]")
    {
        auto editor = ExcelDocumentEditor::CreateNew();
        auto sheet = editor->FirstWorksheet();
        REQUIRE(sheet);

        ExcelStyle definition;
        definition.NumberFormat = *ExcelNumberFormat::Accounting("EUR", 2);
        ExcelFont font;
        font.Scheme = ExcelFontScheme::Minor;
        font.Color = ExcelColor::Theme(1, -0.15);
        font.Bold = true;
        definition.Font = font;
        ExcelFill fill;
        fill.Pattern = ExcelFillPattern::DarkTrellis;
        fill.Foreground = ExcelColor::Rgb("FF336699");
        fill.Background = ExcelColor::Indexed(9);
        definition.Fill = fill;
        ExcelBorder border;
        border.Left = {ExcelBorderStyle::MediumDashDot, ExcelColor::Theme(4)};
        border.Right = {ExcelBorderStyle::Double, ExcelColor::Rgb("FFFF0000")};
        border.Top = {ExcelBorderStyle::Hair, ExcelColor::Indexed(8)};
        border.Bottom = {ExcelBorderStyle::Thick, ExcelColor::Automatic()};
        definition.Border = border;
        definition.Protection = ExcelProtection{false, true};

        const auto registered = editor->Styles().GetOrAdd(definition);
        REQUIRE(registered);
        REQUIRE(editor->Styles().ApplyToRange(
            *sheet, ExcelAdvancedFormattingTestHelpers::Range("B2:C3"), registered.StyleIndex));
        REQUIRE(sheet->MergeRange(ExcelAdvancedFormattingTestHelpers::Range("B2:C2")));

        const auto styleXml = editor->GetDocument()->GetWorkbookPart()->GetWorkbookStylesPart()->GetXmlString();
        CHECK(styleXml.find("<x:scheme val=\"minor\"") != std::string::npos);
        CHECK(styleXml.find("patternType=\"darkTrellis\"") != std::string::npos);
        CHECK(styleXml.find("style=\"mediumDashDot\"") != std::string::npos);
        CHECK(styleXml.find("locked=\"0\"") != std::string::npos);
        CHECK(styleXml.find("hidden=\"1\"") != std::string::npos);
        CHECK(styleXml.find("formatCode=") != std::string::npos);
        const auto merged = sheet->MergedRangeAt(ExcelAdvancedFormattingTestHelpers::Address("C2"));
        REQUIRE(merged);
        CHECK(merged->ToA1() == "B2:C2");
    }

    TEST_CASE("Worksheet protection exposes positive permissions and interoperable password verification "
              "[unit] [excel] [excel-protection]")
    {
        auto editor = ExcelDocumentEditor::CreateNew();
        auto sheet = editor->FirstWorksheet();
        REQUIRE(sheet);
        CHECK_FALSE(sheet->GetProtection());

        SheetProtectionOptions options;
        options.AllowFormatCells = true;
        options.AllowInsertRows = true;
        options.AllowSelectLockedCells = false;
        options.AllowSort = true;
        options.AllowAutoFilter = true;
        options.ProtectObjects = false;
        REQUIRE(sheet->Protect(options, "secret"));

        const auto info = sheet->GetProtection();
        REQUIRE(info);
        CHECK(info->HasPassword);
        CHECK(info->Options == options);

        const auto xml = sheet->GetPart()->GetXmlString();
        CHECK(xml.find("password=\"DAA7\"") != std::string::npos);
        CHECK(xml.find("formatCells=\"0\"") != std::string::npos);
        CHECK(xml.find("insertRows=\"0\"") != std::string::npos);
        CHECK(xml.find("selectLockedCells=\"1\"") != std::string::npos);
        CHECK(xml.find("sort=\"0\"") != std::string::npos);
        CHECK(xml.find("autoFilter=\"0\"") != std::string::npos);
        CHECK(xml.find("objects=\"0\"") != std::string::npos);

        const auto wrong = sheet->Unprotect("wrong");
        CHECK_FALSE(wrong);
        CHECK(wrong.Error == SheetProtectionError::PasswordMismatch);
        CHECK(sheet->GetProtection());
        REQUIRE(sheet->Unprotect("secret"));
        CHECK_FALSE(sheet->GetProtection());
    }

    TEST_CASE("Worksheet protection replacement and password-free removal round trip "
              "[unit] [excel] [excel-protection]")
    {
        auto editor = ExcelDocumentEditor::CreateNew();
        auto sheet = editor->FirstWorksheet();
        REQUIRE(sheet);
        REQUIRE(sheet->Protect({}, "first"));

        SheetProtectionOptions replacement;
        replacement.AllowDeleteRows = true;
        replacement.AllowPivotTables = true;
        REQUIRE(sheet->Protect(replacement));
        REQUIRE(sheet->GetProtection());
        CHECK_FALSE(sheet->GetProtection()->HasPassword);
        CHECK(sheet->GetProtection()->Options == replacement);

        const auto bytes = editor->SaveToMemory();
        REQUIRE_FALSE(bytes.empty());
        auto reopened = ExcelDocumentEditor::Open(bytes);
        REQUIRE(reopened);
        auto reopenedSheet = reopened->FirstWorksheet();
        REQUIRE(reopenedSheet);
        REQUIRE(reopenedSheet->GetProtection());
        CHECK(reopenedSheet->GetProtection()->Options == replacement);

        const auto unexpected = reopenedSheet->Unprotect("not-needed");
        CHECK_FALSE(unexpected);
        CHECK(unexpected.Error == SheetProtectionError::PasswordMismatch);
        REQUIRE(reopenedSheet->Unprotect());
        CHECK_FALSE(reopenedSheet->GetProtection());
        REQUIRE(reopenedSheet->Unprotect());
    }

    TEST_CASE("Worksheet protection rejects invalid wrappers and passwords without mutation "
              "[unit] [excel] [excel-protection]")
    {
        Worksheet detached;
        auto result = detached.Protect();
        CHECK_FALSE(result);
        CHECK(result.Error == SheetProtectionError::InvalidWorksheet);
        result = detached.Unprotect();
        CHECK_FALSE(result);
        CHECK(result.Error == SheetProtectionError::InvalidWorksheet);

        auto sheet = ExcelDocumentEditor::CreateNew()->FirstWorksheet();
        REQUIRE(sheet);
        const auto originalXml = sheet->GetPart()->GetXmlString();
        result = sheet->Protect({}, "1234567890123456");
        CHECK_FALSE(result);
        CHECK(result.Error == SheetProtectionError::InvalidPassword);
        CHECK(sheet->GetPart()->GetXmlString() == originalXml);
        result = sheet->Unprotect("1234567890123456");
        CHECK_FALSE(result);
        CHECK(result.Error == SheetProtectionError::InvalidPassword);
    }

    TEST_CASE("Style read-back reconstructs every component of a registered cell format "
              "[unit] [excel] [excel-advanced-formatting] [excel-style-readback]")
    {
        auto editor = ExcelDocumentEditor::CreateNew();
        auto sheet = editor->FirstWorksheet();
        REQUIRE(sheet);
        auto styles = editor->Styles();

        ExcelStyle definition;
        definition.NumberFormat = *ExcelNumberFormat::Custom("[Red]#,##0.000;-#,##0.000");
        ExcelFont font;
        font.Name = "Cambria";
        font.Size = 13.5;
        font.Color = ExcelColor::Rgb("FF204060", 0.25);
        font.Bold = true;
        font.Italic = true;
        font.Strike = true;
        font.Outline = true;
        font.Shadow = true;
        font.Condense = true;
        font.Extend = true;
        font.Underline = ExcelUnderlineStyle::DoubleAccounting;
        font.VerticalAlignment = ExcelFontVerticalAlignment::Superscript;
        font.Family = 1;
        font.CharacterSet = 238;
        font.Scheme = ExcelFontScheme::Major;
        definition.Font = font;
        ExcelFill fill;
        fill.Pattern = ExcelFillPattern::LightTrellis;
        fill.Foreground = ExcelColor::Theme(5, -0.5);
        fill.Background = ExcelColor::Indexed(64);
        definition.Fill = fill;
        ExcelBorder border;
        border.Left = {ExcelBorderStyle::SlantDashDot, ExcelColor::Rgb("FF00FF00")};
        border.Right = {ExcelBorderStyle::MediumDashDotDot, ExcelColor::Automatic()};
        border.Top = {ExcelBorderStyle::Dotted, ExcelColor::Indexed(10)};
        border.Bottom = {ExcelBorderStyle::Hair, ExcelColor::Theme(2)};
        border.Diagonal = {ExcelBorderStyle::Thin, ExcelColor::Rgb("FF123456")};
        border.Horizontal = {ExcelBorderStyle::Dashed, std::nullopt};
        border.Vertical = {ExcelBorderStyle::Thick, std::nullopt};
        border.DiagonalUp = true;
        border.DiagonalDown = true;
        border.Outline = false;
        definition.Border = border;
        ExcelAlignment alignment;
        alignment.Horizontal = ExcelHorizontalAlignment::CenterContinuous;
        alignment.Vertical = ExcelVerticalAlignment::Distributed;
        alignment.TextRotation = 135;
        alignment.WrapText = true;
        alignment.Indent = 3;
        alignment.RelativeIndent = -2;
        alignment.JustifyLastLine = true;
        alignment.ShrinkToFit = false;
        alignment.ReadingOrder = 2;
        definition.Alignment = alignment;
        definition.Protection = ExcelProtection{false, true};
        definition.QuotePrefix = true;
        definition.PivotButton = true;

        const auto registered = styles.GetOrAdd(definition);
        REQUIRE(registered);

        const auto readBack = styles.GetStyle(registered.StyleIndex);
        REQUIRE(readBack);
        CHECK(*readBack == definition);

        // Re-registering a read-back definition must not grow the stylesheet.
        const auto styleCount = styles.Count();
        const auto reregistered = styles.GetOrAdd(*readBack);
        REQUIRE(reregistered);
        CHECK(reregistered.StyleIndex == registered.StyleIndex);
        CHECK(styles.Count() == styleCount);

        CHECK_FALSE(styles.GetStyle(styleCount));
    }

    TEST_CASE("Style read-back resolves gradient fills built-in number formats and default cells "
              "[unit] [excel] [excel-advanced-formatting] [excel-style-readback]")
    {
        auto editor = ExcelDocumentEditor::CreateNew();
        auto sheet = editor->FirstWorksheet();
        REQUIRE(sheet);
        auto styles = editor->Styles();

        // An untouched cell resolves to the workbook default cell XF at index zero.
        const auto blank = styles.GetCellStyle(*sheet, ExcelAdvancedFormattingTestHelpers::Address("Z99"));
        REQUIRE(blank);
        CHECK(*blank == ExcelStyle{});

        ExcelStyle gradient;
        gradient.NumberFormat = ExcelNumberFormat::Scientific();
        ExcelFill fill;
        fill.Kind = ExcelFillKind::LinearGradient;
        fill.Degree = 45.0;
        fill.GradientStops = {{0.0, ExcelColor::Rgb("FFFFFFFF")},
                              {0.5, ExcelColor::Theme(3)},
                              {1.0, ExcelColor::Rgb("FF000000", -0.75)}};
        gradient.Fill = fill;

        const auto registered = styles.GetOrAdd(gradient);
        REQUIRE(registered);
        const auto address = ExcelAdvancedFormattingTestHelpers::Address("D4");
        REQUIRE(styles.ApplyToCell(*sheet, address, registered.StyleIndex));

        const auto readBack = styles.GetCellStyle(*sheet, address);
        REQUIRE(readBack);
        CHECK(*readBack == gradient);
        REQUIRE(readBack->NumberFormat);
        CHECK(readBack->NumberFormat->BuiltInId == 11);
        CHECK(readBack->NumberFormat->FormatCode.empty());

        // Path gradients keep their convergence offsets rather than the degree.
        ExcelStyle path = gradient;
        path.Fill->Kind = ExcelFillKind::PathGradient;
        path.Fill->Degree = 0.0;
        path.Fill->Left = 0.25;
        path.Fill->Right = 0.75;
        path.Fill->Top = 0.1;
        path.Fill->Bottom = 0.9;
        const auto pathRegistered = styles.GetOrAdd(path);
        REQUIRE(pathRegistered);
        const auto pathReadBack = styles.GetStyle(pathRegistered.StyleIndex);
        REQUIRE(pathReadBack);
        CHECK(*pathReadBack == path);
    }

    TEST_CASE("Style delta keeps replaces and clears individual components "
              "[unit] [excel] [excel-advanced-formatting] [excel-style-delta]")
    {
        ExcelStyle base;
        base.NumberFormat = ExcelNumberFormat::Percent();
        ExcelFont font;
        font.Bold = true;
        base.Font = font;
        ExcelFill fill;
        fill.Pattern = ExcelFillPattern::Solid;
        fill.Foreground = ExcelColor::Rgb("FFFFFF00");
        base.Fill = fill;
        base.Border = ExcelBorder{};
        base.Alignment = ExcelAlignment{};
        base.Protection = ExcelProtection{true, false};

        CHECK(ExcelStyleDelta{}.IsEmpty());
        CHECK(ExcelStyleDelta{}.ApplyTo(base) == base);

        ExcelStyleDelta replace;
        ExcelFont italic;
        italic.Italic = true;
        replace.Font = italic;
        replace.QuotePrefix = true;
        CHECK_FALSE(replace.IsEmpty());
        const auto replaced = replace.ApplyTo(base);
        CHECK(replaced.Font == italic);
        CHECK(replaced.QuotePrefix);
        CHECK(replaced.NumberFormat == base.NumberFormat);
        CHECK(replaced.Fill == base.Fill);
        CHECK(replaced.Protection == base.Protection);

        ExcelStyleDelta clear;
        clear.ClearNumberFormat = true;
        clear.ClearFill = true;
        clear.ClearBorder = true;
        clear.ClearAlignment = true;
        clear.ClearProtection = true;
        const auto cleared = clear.ApplyTo(base);
        CHECK_FALSE(cleared.NumberFormat);
        CHECK_FALSE(cleared.Fill);
        CHECK_FALSE(cleared.Border);
        CHECK_FALSE(cleared.Alignment);
        CHECK_FALSE(cleared.Protection);
        CHECK(cleared.Font == base.Font);

        // An explicit value wins over a contradictory clear flag.
        ExcelStyleDelta contradictory;
        contradictory.Font = italic;
        contradictory.ClearFont = true;
        CHECK(contradictory.ApplyTo(base).Font == italic);
    }

    TEST_CASE("Cell formatter preserves untouched components while changing one at a time "
              "[unit] [excel] [excel-advanced-formatting] [excel-cell-formatter]")
    {
        auto editor = ExcelDocumentEditor::CreateNew();
        auto sheet = editor->FirstWorksheet();
        REQUIRE(sheet);
        CellFormatter formatter(editor);
        REQUIRE(formatter.IsValid());

        const auto cell = ExcelAdvancedFormattingTestHelpers::Address("B2");
        const auto single = CellRange(cell, cell);

        ExcelFont font;
        font.Name = "Consolas";
        font.Bold = true;
        REQUIRE(formatter.SetFont(*sheet, single, font));
        REQUIRE(formatter.SetNumberFormat(*sheet, single, *ExcelNumberFormat::Accounting("$", 2)));
        REQUIRE(formatter.SetFillPattern(*sheet, single, ExcelFillPattern::LightGrid,
                                         ExcelColor::Rgb("FFAABBCC"), ExcelColor::Rgb("FF102030")));
        ExcelAlignment alignment;
        alignment.Horizontal = ExcelHorizontalAlignment::Right;
        alignment.WrapText = true;
        REQUIRE(formatter.SetAlignment(*sheet, single, alignment));
        REQUIRE(formatter.SetLocked(*sheet, single, false));

        const auto style = formatter.GetStyle(*sheet, cell);
        REQUIRE(style);
        REQUIRE(style->Font);
        CHECK(style->Font->Name == "Consolas");
        CHECK(style->Font->Bold);
        REQUIRE(style->NumberFormat);
        CHECK(style->NumberFormat->FormatCode.find("\"$\"") != std::string::npos);
        REQUIRE(style->Fill);
        CHECK(style->Fill->Pattern == ExcelFillPattern::LightGrid);
        CHECK(style->Fill->Foreground == ExcelColor::Rgb("FFAABBCC"));
        CHECK(style->Fill->Background == ExcelColor::Rgb("FF102030"));
        REQUIRE(style->Alignment);
        CHECK(style->Alignment->Horizontal == ExcelHorizontalAlignment::Right);
        REQUIRE(style->Protection);
        CHECK(style->Protection->Locked == false);

        // Clearing one component leaves the others intact.
        ExcelStyleDelta clearFill;
        clearFill.ClearFill = true;
        REQUIRE(formatter.Modify(*sheet, cell, clearFill));
        const auto reduced = formatter.GetStyle(*sheet, cell);
        REQUIRE(reduced);
        CHECK_FALSE(reduced->Fill);
        CHECK(reduced->Font == style->Font);
        CHECK(reduced->NumberFormat == style->NumberFormat);
        CHECK(reduced->Alignment == style->Alignment);
    }

    TEST_CASE("Cell formatter merges a range delta into each cell's own previous style "
              "[unit] [excel] [excel-advanced-formatting] [excel-cell-formatter]")
    {
        auto editor = ExcelDocumentEditor::CreateNew();
        auto sheet = editor->FirstWorksheet();
        REQUIRE(sheet);
        CellFormatter formatter(editor->GetDocument());

        const auto first = ExcelAdvancedFormattingTestHelpers::Address("A1");
        const auto second = ExcelAdvancedFormattingTestHelpers::Address("A2");
        ExcelFont bold;
        bold.Bold = true;
        REQUIRE(formatter.SetFont(*sheet, CellRange(first, first), bold));
        ExcelFont italic;
        italic.Italic = true;
        REQUIRE(formatter.SetFont(*sheet, CellRange(second, second), italic));

        ExcelStyleDelta shaded;
        ExcelFill fill;
        fill.Pattern = ExcelFillPattern::Solid;
        fill.Foreground = ExcelColor::Rgb("FFEEEEEE");
        shaded.Fill = fill;
        const auto applied = formatter.Modify(*sheet, ExcelAdvancedFormattingTestHelpers::Range("A1:A2"), shaded);
        REQUIRE(applied);
        CHECK(applied.AffectedCellCount == 2);

        const auto firstStyle = formatter.GetStyle(*sheet, first);
        const auto secondStyle = formatter.GetStyle(*sheet, second);
        REQUIRE(firstStyle);
        REQUIRE(secondStyle);
        CHECK(firstStyle->Font == bold);
        CHECK(secondStyle->Font == italic);
        CHECK(firstStyle->Fill == fill);
        CHECK(secondStyle->Fill == fill);
    }

    TEST_CASE("Cell formatter rejects invalid input without mutating the workbook "
              "[unit] [excel] [excel-advanced-formatting] [excel-cell-formatter]")
    {
        CellFormatter detached;
        CHECK_FALSE(detached.IsValid());
        Worksheet detachedSheet;
        auto result = detached.SetFont(detachedSheet, ExcelAdvancedFormattingTestHelpers::Range("A1:A1"), ExcelFont{});
        CHECK_FALSE(result);
        CHECK(result.Error == RangeOperationError::InvalidWorksheet);

        auto editor = ExcelDocumentEditor::CreateNew();
        auto sheet = editor->FirstWorksheet();
        REQUIRE(sheet);
        CellFormatter formatter(editor);
        const auto originalXml = sheet->GetPart()->GetXmlString();

        result = formatter.Modify(*sheet, CellRange{}, ExcelStyleDelta{});
        CHECK_FALSE(result);
        CHECK(result.Error == RangeOperationError::InvalidAddress);

        ExcelStyleDelta contradictory;
        contradictory.Font = ExcelFont{};
        contradictory.ClearFont = true;
        result = formatter.Modify(*sheet, ExcelAdvancedFormattingTestHelpers::Range("A1:B2"), contradictory);
        CHECK_FALSE(result);
        CHECK(result.Error == RangeOperationError::InvalidStyle);

        // Invalid style content is rejected by the repository before anything is written.
        ExcelStyleDelta invalidFont;
        ExcelFont broken;
        broken.Size = -1.0;
        invalidFont.Font = broken;
        result = formatter.Modify(*sheet, ExcelAdvancedFormattingTestHelpers::Range("A1:B2"), invalidFont);
        CHECK_FALSE(result);
        CHECK(result.Error == RangeOperationError::InvalidStyle);

        // An empty delta is a successful no-op.
        const auto empty = formatter.Modify(*sheet, ExcelAdvancedFormattingTestHelpers::Range("A1:B2"), ExcelStyleDelta{});
        CHECK(empty);
        CHECK(empty.AffectedCellCount == 0);
        CHECK(sheet->GetPart()->GetXmlString() == originalXml);
    }

    TEST_CASE("Range border frames the range instead of boxing every cell "
              "[unit] [excel] [excel-advanced-formatting] [excel-range-border]")
    {
        auto editor = ExcelDocumentEditor::CreateNew();
        auto sheet = editor->FirstWorksheet();
        REQUIRE(sheet);
        CellFormatter formatter(editor);

        const auto range = ExcelAdvancedFormattingTestHelpers::Range("B2:D4");
        const auto applied = formatter.ApplyRangeBorder(
            *sheet, range, ExcelRangeBorder::Grid(ExcelBorderStyle::Medium, ExcelBorderStyle::Hair, ExcelColor::Rgb("FF445566")));
        REQUIRE(applied);
        CHECK(applied.AffectedCellCount == 9);

        const auto borderAt = [&](std::string_view text)
        {
            const auto style = formatter.GetStyle(*sheet, ExcelAdvancedFormattingTestHelpers::Address(text));
            REQUIRE(style);
            REQUIRE(style->Border);
            return *style->Border;
        };

        const auto topLeft = borderAt("B2");
        CHECK(topLeft.Left.Style == ExcelBorderStyle::Medium);
        CHECK(topLeft.Top.Style == ExcelBorderStyle::Medium);
        CHECK(topLeft.Right.Style == ExcelBorderStyle::Hair);
        CHECK(topLeft.Bottom.Style == ExcelBorderStyle::Hair);
        CHECK(topLeft.Left.Color == ExcelColor::Rgb("FF445566"));

        const auto center = borderAt("C3");
        CHECK(center.Left.Style == ExcelBorderStyle::Hair);
        CHECK(center.Right.Style == ExcelBorderStyle::Hair);
        CHECK(center.Top.Style == ExcelBorderStyle::Hair);
        CHECK(center.Bottom.Style == ExcelBorderStyle::Hair);

        const auto bottomRight = borderAt("D4");
        CHECK(bottomRight.Right.Style == ExcelBorderStyle::Medium);
        CHECK(bottomRight.Bottom.Style == ExcelBorderStyle::Medium);
        CHECK(bottomRight.Left.Style == ExcelBorderStyle::Hair);
        CHECK(bottomRight.Top.Style == ExcelBorderStyle::Hair);

        // A cell outside the range keeps the workbook default style.
        const auto outside = formatter.GetStyle(*sheet, ExcelAdvancedFormattingTestHelpers::Address("E5"));
        REQUIRE(outside);
        CHECK_FALSE(outside->Border);
    }

    TEST_CASE("Range border boxes a single cell preserves diagonals and can be erased "
              "[unit] [excel] [excel-advanced-formatting] [excel-range-border]")
    {
        auto editor = ExcelDocumentEditor::CreateNew();
        auto sheet = editor->FirstWorksheet();
        REQUIRE(sheet);
        CellFormatter formatter(editor);

        const auto cell = ExcelAdvancedFormattingTestHelpers::Address("C3");
        const auto single = CellRange(cell, cell);

        ExcelBorder diagonalOnly;
        diagonalOnly.Diagonal = {ExcelBorderStyle::Thin, ExcelColor::Rgb("FFFF0000")};
        diagonalOnly.DiagonalUp = true;
        REQUIRE(formatter.SetBorder(*sheet, single, diagonalOnly));

        REQUIRE(formatter.ApplyRangeBorder(*sheet, single, ExcelRangeBorder::Box(ExcelBorderStyle::Double)));
        auto style = formatter.GetStyle(*sheet, cell);
        REQUIRE(style);
        REQUIRE(style->Border);
        CHECK(style->Border->Left.Style == ExcelBorderStyle::Double);
        CHECK(style->Border->Right.Style == ExcelBorderStyle::Double);
        CHECK(style->Border->Top.Style == ExcelBorderStyle::Double);
        CHECK(style->Border->Bottom.Style == ExcelBorderStyle::Double);
        CHECK(style->Border->Diagonal.Style == ExcelBorderStyle::Thin);
        CHECK(style->Border->DiagonalUp);

        REQUIRE(formatter.ApplyRangeBorder(*sheet, single, ExcelRangeBorder::None()));
        style = formatter.GetStyle(*sheet, cell);
        REQUIRE(style);
        REQUIRE(style->Border);
        CHECK(style->Border->Left.Style == ExcelBorderStyle::None);
        CHECK(style->Border->Bottom.Style == ExcelBorderStyle::None);
        CHECK(style->Border->Diagonal.Style == ExcelBorderStyle::Thin);
    }

    TEST_CASE("Cell formatting survives a package round trip "
              "[unit] [excel] [excel-advanced-formatting] [excel-cell-formatter]")
    {
        auto editor = ExcelDocumentEditor::CreateNew();
        auto sheet = editor->FirstWorksheet();
        REQUIRE(sheet);
        CellFormatter formatter(editor);

        const auto range = ExcelAdvancedFormattingTestHelpers::Range("A1:B2");
        ExcelFont font;
        font.Name = "Georgia";
        font.Size = 9.0;
        font.Underline = ExcelUnderlineStyle::SingleAccounting;
        REQUIRE(formatter.SetFont(*sheet, range, font));
        REQUIRE(formatter.SetNumberFormat(*sheet, range, ExcelNumberFormat::ThousandsDecimal()));
        REQUIRE(formatter.ApplyRangeBorder(*sheet, range, ExcelRangeBorder::Box(ExcelBorderStyle::Thick)));
        const auto expected = formatter.GetStyle(*sheet, ExcelAdvancedFormattingTestHelpers::Address("A1"));
        REQUIRE(expected);

        const auto bytes = editor->SaveToMemory();
        REQUIRE_FALSE(bytes.empty());
        auto reopened = ExcelDocumentEditor::Open(bytes);
        REQUIRE(reopened);
        auto reopenedSheet = reopened->FirstWorksheet();
        REQUIRE(reopenedSheet);

        CellFormatter reopenedFormatter(reopened);
        const auto actual = reopenedFormatter.GetStyle(*reopenedSheet,
                                                       ExcelAdvancedFormattingTestHelpers::Address("A1"));
        REQUIRE(actual);
        CHECK(*actual == *expected);
    }

    TEST_CASE("Workbook protection locks the structure and verifies its password "
              "[unit] [excel] [excel-protection]")
    {
        auto editor = ExcelDocumentEditor::CreateNew();
        REQUIRE(editor);
        CHECK_FALSE(editor->GetWorkbookProtection());

        WorkbookProtectionOptions options;
        options.LockStructure = true;
        options.LockWindows = true;
        REQUIRE(editor->ProtectWorkbook(options, "secret"));

        const auto info = editor->GetWorkbookProtection();
        REQUIRE(info);
        CHECK(info->Options == options);
        CHECK(info->HasPassword);

        const auto xml = editor->GetDocument()->GetWorkbookPart()->GetXmlString();
        CHECK(xml.find("lockStructure=\"1\"") != std::string::npos);
        CHECK(xml.find("lockWindows=\"1\"") != std::string::npos);
        CHECK(xml.find("DAA7") != std::string::npos);

        const auto wrong = editor->UnprotectWorkbook("wrong");
        CHECK_FALSE(wrong);
        CHECK(wrong.Error == WorkbookProtectionError::PasswordMismatch);
        CHECK(editor->GetWorkbookProtection());
        REQUIRE(editor->UnprotectWorkbook("secret"));
        CHECK_FALSE(editor->GetWorkbookProtection());
        // Removing protection from an unprotected workbook is a successful no-op.
        REQUIRE(editor->UnprotectWorkbook());
    }

    TEST_CASE("Workbook protection round trips and rejects invalid requests "
              "[unit] [excel] [excel-protection]")
    {
        auto editor = ExcelDocumentEditor::CreateNew();
        REQUIRE(editor);

        auto rejected = editor->ProtectWorkbook({}, "1234567890123456");
        CHECK_FALSE(rejected);
        CHECK(rejected.Error == WorkbookProtectionError::InvalidPassword);
        rejected = editor->ProtectWorkbook(WorkbookProtectionOptions{false, false});
        CHECK_FALSE(rejected);
        CHECK(rejected.Error == WorkbookProtectionError::InvalidWorkbook);
        CHECK_FALSE(editor->GetWorkbookProtection());

        ExcelDocumentEditor detached;
        CHECK_FALSE(detached.GetWorkbookProtection());
        CHECK(detached.ProtectWorkbook().Error == WorkbookProtectionError::InvalidWorkbook);
        CHECK(detached.UnprotectWorkbook().Error == WorkbookProtectionError::InvalidWorkbook);

        WorkbookProtectionOptions structureOnly;
        structureOnly.LockStructure = true;
        structureOnly.LockWindows = false;
        REQUIRE(editor->ProtectWorkbook(structureOnly));
        REQUIRE(editor->GetWorkbookProtection());
        CHECK_FALSE(editor->GetWorkbookProtection()->HasPassword);

        const auto bytes = editor->SaveToMemory();
        REQUIRE_FALSE(bytes.empty());
        auto reopened = ExcelDocumentEditor::Open(bytes);
        REQUIRE(reopened);
        const auto info = reopened->GetWorkbookProtection();
        REQUIRE(info);
        CHECK(info->Options == structureOnly);
        CHECK_FALSE(info->HasPassword);

        const auto unexpected = reopened->UnprotectWorkbook("not-needed");
        CHECK_FALSE(unexpected);
        CHECK(unexpected.Error == WorkbookProtectionError::PasswordMismatch);
        REQUIRE(reopened->UnprotectWorkbook());
        CHECK_FALSE(reopened->GetWorkbookProtection());
    }
}
