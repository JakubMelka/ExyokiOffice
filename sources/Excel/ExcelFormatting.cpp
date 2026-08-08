// Copyright (c) 2026 Jakub Melka and Collaborators
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "ExyokiOffice/Excel/ExcelFormatting.hpp"
#include "ExyokiOffice/StandardTypes.hpp"

#include <utility>

namespace ExyokiOffice::Excel
{

class CellFormatterHelpers final
{
public:
    CellFormatterHelpers() = delete;

    static RangeOperationResult Error(RangeOperationError error, std::string message)
    {
        return RangeOperationResult{error, std::move(message), 0};
    }

    /**
     * Reports the contradictory "set and clear the same component" combination.
     *
     * The check runs before any document mutation so a rejected delta cannot
     * leave a partially formatted range behind.
     */
    static bool IsContradictory(const ExcelStyleDelta& delta)
    {
        return (delta.NumberFormat && delta.ClearNumberFormat) || (delta.Font && delta.ClearFont) ||
               (delta.Fill && delta.ClearFill) || (delta.Border && delta.ClearBorder) ||
               (delta.Alignment && delta.ClearAlignment) || (delta.Protection && delta.ClearProtection);
    }

    /** Selects the edge that a cell at the given range position must use. */
    static ExcelBorderSide EdgeAt(bool isOutline, const ExcelBorderSide& outline, const ExcelBorderSide& inside)
    {
        return isOutline ? outline : inside;
    }
};

bool ExcelStyleDelta::IsEmpty() const noexcept
{
    return !NumberFormat && !Font && !Fill && !Border && !Alignment && !Protection && !QuotePrefix &&
           !PivotButton && !ClearNumberFormat && !ClearFont && !ClearFill && !ClearBorder && !ClearAlignment &&
           !ClearProtection;
}

ExcelStyle ExcelStyleDelta::ApplyTo(const ExcelStyle& base) const
{
    ExcelStyle result = base;
    if (NumberFormat)
    {
        result.NumberFormat = NumberFormat;
    }
    else if (ClearNumberFormat)
    {
        result.NumberFormat.reset();
    }
    if (Font)
    {
        result.Font = Font;
    }
    else if (ClearFont)
    {
        result.Font.reset();
    }
    if (Fill)
    {
        result.Fill = Fill;
    }
    else if (ClearFill)
    {
        result.Fill.reset();
    }
    if (Border)
    {
        result.Border = Border;
    }
    else if (ClearBorder)
    {
        result.Border.reset();
    }
    if (Alignment)
    {
        result.Alignment = Alignment;
    }
    else if (ClearAlignment)
    {
        result.Alignment.reset();
    }
    if (Protection)
    {
        result.Protection = Protection;
    }
    else if (ClearProtection)
    {
        result.Protection.reset();
    }
    if (QuotePrefix)
    {
        result.QuotePrefix = *QuotePrefix;
    }
    if (PivotButton)
    {
        result.PivotButton = *PivotButton;
    }
    return result;
}

ExcelRangeBorder ExcelRangeBorder::Box(ExcelBorderStyle style, std::optional<ExcelColor> color)
{
    const ExcelBorderSide side{style, std::move(color)};
    ExcelRangeBorder border;
    border.OutlineLeft = side;
    border.OutlineRight = side;
    border.OutlineTop = side;
    border.OutlineBottom = side;
    return border;
}

ExcelRangeBorder ExcelRangeBorder::Grid(ExcelBorderStyle outlineStyle,
                                        ExcelBorderStyle insideStyle,
                                        std::optional<ExcelColor> color)
{
    ExcelRangeBorder border = Box(outlineStyle, color);
    const ExcelBorderSide inside{insideStyle, std::move(color)};
    border.InsideVertical = inside;
    border.InsideHorizontal = inside;
    return border;
}

ExcelRangeBorder ExcelRangeBorder::None()
{
    return ExcelRangeBorder{};
}

CellFormatter::CellFormatter(ExcelDocument::Ptr document) : m_document(std::move(document)) {}

CellFormatter::CellFormatter(const ExcelDocumentEditor::Ptr& editor)
    : m_document(editor ? editor->GetDocument() : nullptr)
{
}

bool CellFormatter::IsValid() const noexcept
{
    return m_document != nullptr;
}

StyleRepository CellFormatter::Styles() const
{
    return StyleRepository(m_document);
}

std::optional<ExcelStyle> CellFormatter::GetStyle(const Worksheet& worksheet, CellAddress address) const
{
    return Styles().GetCellStyle(worksheet, address);
}

RangeOperationResult CellFormatter::SetStyle(Worksheet& worksheet, CellAddress address, const ExcelStyle& style)
{
    if (!IsValid())
    {
        return CellFormatterHelpers::Error(RangeOperationError::InvalidWorksheet,
                                           "The formatter is not attached to a workbook.");
    }
    if (!address.IsValid())
    {
        return CellFormatterHelpers::Error(RangeOperationError::InvalidAddress,
                                           "The target cell address is invalid.");
    }

    auto styles = Styles();
    const auto registered = styles.GetOrAdd(style);
    if (!registered)
    {
        return registered.Status;
    }
    return styles.ApplyToCell(worksheet, address, registered.StyleIndex);
}

RangeOperationResult CellFormatter::SetStyle(Worksheet& worksheet, CellRange range, const ExcelStyle& style)
{
    if (!IsValid())
    {
        return CellFormatterHelpers::Error(RangeOperationError::InvalidWorksheet,
                                           "The formatter is not attached to a workbook.");
    }
    if (!range.IsValid())
    {
        return CellFormatterHelpers::Error(RangeOperationError::InvalidAddress, "The target range is invalid.");
    }

    auto styles = Styles();
    const auto registered = styles.GetOrAdd(style);
    if (!registered)
    {
        return registered.Status;
    }
    return styles.ApplyToRange(worksheet, range, registered.StyleIndex);
}

RangeOperationResult CellFormatter::Modify(Worksheet& worksheet, CellAddress address, const ExcelStyleDelta& delta)
{
    return Modify(worksheet, CellRange(address, address), delta);
}

RangeOperationResult CellFormatter::Modify(Worksheet& worksheet, CellRange range, const ExcelStyleDelta& delta)
{
    if (!IsValid())
    {
        return CellFormatterHelpers::Error(RangeOperationError::InvalidWorksheet,
                                           "The formatter is not attached to a workbook.");
    }
    if (!range.IsValid())
    {
        return CellFormatterHelpers::Error(RangeOperationError::InvalidAddress, "The target range is invalid.");
    }
    if (CellFormatterHelpers::IsContradictory(delta))
    {
        return CellFormatterHelpers::Error(
            RangeOperationError::InvalidStyle,
            "A style component cannot be replaced and cleared by the same delta.");
    }

    const auto worksheetPart = worksheet.GetPart();
    const auto workbookPart = m_document->GetWorkbookPart();
    if (!worksheetPart || !workbookPart)
    {
        return CellFormatterHelpers::Error(RangeOperationError::InvalidWorksheet,
                                           "The worksheet is detached from its workbook.");
    }
    if (delta.IsEmpty())
    {
        return RangeOperationResult{RangeOperationError::None, {}, 0};
    }

    // Both parts are restored together so a failure in the middle of a range
    // leaves neither orphaned styles nor partially reformatted cells behind.
    const auto originalWorksheetXml = worksheetPart->GetXmlString();
    const auto stylesPart = workbookPart->GetWorkbookStylesPart();
    const auto originalStylesXml = stylesPart ? stylesPart->GetXmlString() : std::string{};

    const auto restore = [&]()
    {
        worksheetPart->SetXmlString(originalWorksheetXml);
        if (stylesPart)
        {
            stylesPart->SetXmlString(originalStylesXml);
        }
    };

    auto styles = Styles();
    Size affected = 0;
    for (UInt32 row = range.First().Row().Value(); row <= range.Last().Row().Value(); ++row)
    {
        for (UInt32 column = range.First().Column().Value(); column <= range.Last().Column().Value(); ++column)
        {
            const auto address = CellAddress::TryCreate(row, column);
            if (!address)
            {
                restore();
                return CellFormatterHelpers::Error(RangeOperationError::InvalidAddress,
                                                   "The range extends beyond the worksheet grid.");
            }
            const auto current = styles.GetCellStyle(worksheet, *address);
            if (!current)
            {
                restore();
                return CellFormatterHelpers::Error(RangeOperationError::StyleNotFound,
                                                   "A cell references a style index that does not exist.");
            }
            const auto registered = styles.GetOrAdd(delta.ApplyTo(*current));
            if (!registered)
            {
                restore();
                return registered.Status;
            }
            if (const auto applied = styles.ApplyToCell(worksheet, *address, registered.StyleIndex); !applied)
            {
                restore();
                return applied;
            }
            ++affected;
        }
    }
    return RangeOperationResult{RangeOperationError::None, {}, affected};
}

RangeOperationResult CellFormatter::SetNumberFormat(Worksheet& worksheet,
                                                    CellRange range,
                                                    const ExcelNumberFormat& format)
{
    ExcelStyleDelta delta;
    delta.NumberFormat = format;
    return Modify(worksheet, range, delta);
}

RangeOperationResult CellFormatter::SetFont(Worksheet& worksheet, CellRange range, const ExcelFont& font)
{
    ExcelStyleDelta delta;
    delta.Font = font;
    return Modify(worksheet, range, delta);
}

RangeOperationResult CellFormatter::SetFill(Worksheet& worksheet, CellRange range, const ExcelFill& fill)
{
    ExcelStyleDelta delta;
    delta.Fill = fill;
    return Modify(worksheet, range, delta);
}

RangeOperationResult CellFormatter::SetFillPattern(Worksheet& worksheet,
                                                   CellRange range,
                                                   ExcelFillPattern pattern,
                                                   const ExcelColor& foreground,
                                                   std::optional<ExcelColor> background)
{
    ExcelFill fill;
    fill.Kind = ExcelFillKind::Pattern;
    fill.Pattern = pattern;
    fill.Foreground = foreground;
    fill.Background = std::move(background);
    return SetFill(worksheet, range, fill);
}

RangeOperationResult CellFormatter::SetBorder(Worksheet& worksheet, CellRange range, const ExcelBorder& border)
{
    ExcelStyleDelta delta;
    delta.Border = border;
    return Modify(worksheet, range, delta);
}

RangeOperationResult CellFormatter::SetAlignment(Worksheet& worksheet,
                                                 CellRange range,
                                                 const ExcelAlignment& alignment)
{
    ExcelStyleDelta delta;
    delta.Alignment = alignment;
    return Modify(worksheet, range, delta);
}

RangeOperationResult CellFormatter::SetProtection(Worksheet& worksheet,
                                                  CellRange range,
                                                  const ExcelProtection& protection)
{
    ExcelStyleDelta delta;
    delta.Protection = protection;
    return Modify(worksheet, range, delta);
}

RangeOperationResult CellFormatter::SetLocked(Worksheet& worksheet, CellRange range, bool locked)
{
    ExcelProtection protection;
    protection.Locked = locked;
    return SetProtection(worksheet, range, protection);
}

RangeOperationResult CellFormatter::ApplyRangeBorder(Worksheet& worksheet,
                                                     CellRange range,
                                                     const ExcelRangeBorder& border)
{
    if (!IsValid())
    {
        return CellFormatterHelpers::Error(RangeOperationError::InvalidWorksheet,
                                           "The formatter is not attached to a workbook.");
    }
    if (!range.IsValid())
    {
        return CellFormatterHelpers::Error(RangeOperationError::InvalidAddress, "The target range is invalid.");
    }

    const auto worksheetPart = worksheet.GetPart();
    const auto workbookPart = m_document->GetWorkbookPart();
    if (!worksheetPart || !workbookPart)
    {
        return CellFormatterHelpers::Error(RangeOperationError::InvalidWorksheet,
                                           "The worksheet is detached from its workbook.");
    }

    const auto originalWorksheetXml = worksheetPart->GetXmlString();
    const auto stylesPart = workbookPart->GetWorkbookStylesPart();
    const auto originalStylesXml = stylesPart ? stylesPart->GetXmlString() : std::string{};

    const auto restore = [&]()
    {
        worksheetPart->SetXmlString(originalWorksheetXml);
        if (stylesPart)
        {
            stylesPart->SetXmlString(originalStylesXml);
        }
    };

    const auto firstRow = range.First().Row().Value();
    const auto lastRow = range.Last().Row().Value();
    const auto firstColumn = range.First().Column().Value();
    const auto lastColumn = range.Last().Column().Value();

    auto styles = Styles();
    Size affected = 0;
    for (UInt32 row = firstRow; row <= lastRow; ++row)
    {
        for (UInt32 column = firstColumn; column <= lastColumn; ++column)
        {
            const auto address = CellAddress::TryCreate(row, column);
            if (!address)
            {
                restore();
                return CellFormatterHelpers::Error(RangeOperationError::InvalidAddress,
                                                   "The range extends beyond the worksheet grid.");
            }
            const auto current = styles.GetCellStyle(worksheet, *address);
            if (!current)
            {
                restore();
                return CellFormatterHelpers::Error(RangeOperationError::StyleNotFound,
                                                   "A cell references a style index that does not exist.");
            }

            // Diagonal settings are cell-private and have no range-relative
            // meaning, so they survive the frame that is drawn around them.
            ExcelBorder target = current->Border.value_or(ExcelBorder{});
            target.Left = CellFormatterHelpers::EdgeAt(column == firstColumn, border.OutlineLeft, border.InsideVertical);
            target.Right = CellFormatterHelpers::EdgeAt(column == lastColumn, border.OutlineRight, border.InsideVertical);
            target.Top = CellFormatterHelpers::EdgeAt(row == firstRow, border.OutlineTop, border.InsideHorizontal);
            target.Bottom = CellFormatterHelpers::EdgeAt(row == lastRow, border.OutlineBottom, border.InsideHorizontal);
            target.Horizontal = ExcelBorderSide{};
            target.Vertical = ExcelBorderSide{};

            ExcelStyle style = *current;
            style.Border = target;
            const auto registered = styles.GetOrAdd(style);
            if (!registered)
            {
                restore();
                return registered.Status;
            }
            if (const auto applied = styles.ApplyToCell(worksheet, *address, registered.StyleIndex); !applied)
            {
                restore();
                return applied;
            }
            ++affected;
        }
    }
    return RangeOperationResult{RangeOperationError::None, {}, affected};
}

} // namespace ExyokiOffice::Excel
