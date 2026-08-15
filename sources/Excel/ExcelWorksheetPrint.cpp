// Copyright (c) 2026 Jakub Melka and Contributors
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "ExyokiOffice/Excel/ExcelDocument.hpp"

#include "ExyokiOffice/DOM/DocumentFormat/OpenXml/Spreadsheet.hpp"
#include "ExyokiOffice/StandardTypes.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <limits>

namespace ExyokiOffice::Excel
{
namespace Spreadsheet = ExyokiOffice::DocumentFormat::OpenXml::Spreadsheet;

class WorksheetPrintHelpers final
{
public:
    static bool ValidMargins(const PageMargins& margins)
    {
        const auto isValid = [](const ExyokiOffice::MeasuringUnits& value)
        {
            const auto inches = value.ToIN().GetValue();
            return std::isfinite(inches) && inches >= 0.0;
        };
        return isValid(margins.Left) && isValid(margins.Right) && isValid(margins.Top) &&
               isValid(margins.Bottom) && isValid(margins.Header) && isValid(margins.Footer);
    }

    static std::string QuoteSheetName(std::string_view name)
    {
        std::string result("'");
        for (const char character : name)
        {
            result += character;
            if (character == '\'')
            {
                result += '\'';
            }
        }
        return result + "'";
    }

    static std::string AbsoluteRange(const CellRange& range)
    {
        const auto first = range.First();
        const auto last = range.Last();
        const auto firstColumn = first.Column().ToName();
        const auto lastColumn = last.Column().ToName();
        return "$" + firstColumn + "$" + std::to_string(first.Row().Value()) + ":$" +
               lastColumn + "$" + std::to_string(last.Row().Value());
    }

    static std::string StripAbsoluteMarkers(std::string value)
    {
        value.erase(std::remove(value.begin(), value.end(), '$'), value.end());
        return value;
    }

    static std::optional<UInt32> SheetIndex(const ExcelDocument::Ptr& document,
                                            std::string_view name)
    {
        const auto part = document ? document->GetWorkbookPart() : nullptr;
        const auto workbook = part ? part->GetWorkbook() : nullptr;
        const auto sheets = workbook ? workbook->GetFirstChildOfType<Spreadsheet::Sheets>() : nullptr;
        if (!sheets)
        {
            return std::nullopt;
        }

        UInt32 index = 0;
        for (const auto& sheet : sheets->Elements<Spreadsheet::Sheet>())
        {
            if (sheet->GetName().ToString() == name)
            {
                return index;
            }
            ++index;
        }
        return std::nullopt;
    }

    static Spreadsheet::DefinedName::Ptr FindDefinedName(const ExcelDocument::Ptr& document,
                                                         std::string_view name,
                                                         UInt32 sheetIndex)
    {
        const auto part = document ? document->GetWorkbookPart() : nullptr;
        const auto workbook = part ? part->GetWorkbook() : nullptr;
        const auto names = workbook ? workbook->GetFirstChildOfType<Spreadsheet::DefinedNames>() : nullptr;
        if (!names)
        {
            return nullptr;
        }

        for (const auto& item : names->Elements<Spreadsheet::DefinedName>())
        {
            if (item->GetName().ToString() == name &&
                item->GetLocalSheetId().ValueOr(std::numeric_limits<UInt32>::max()) == sheetIndex)
            {
                return item;
            }
        }
        return nullptr;
    }

    static void SetDefinedName(const ExcelDocument::Ptr& document, std::string_view name,
                               UInt32 sheetIndex, std::string_view formula)
    {
        const auto part = document ? document->GetWorkbookPart() : nullptr;
        const auto workbook = part ? part->GetWorkbook() : nullptr;
        if (!workbook)
        {
            return;
        }

        auto names = workbook->GetFirstChildOfType<Spreadsheet::DefinedNames>();
        auto item = FindDefinedName(document, name, sheetIndex);
        if (formula.empty())
        {
            if (item && names)
            {
                names->RemoveChild(item);
                if (names->Elements<Spreadsheet::DefinedName>().empty())
                {
                    workbook->RemoveChild(names);
                }
            }
            return;
        }

        if (!names)
        {
            names = workbook->AppendChild<Spreadsheet::DefinedNames>();
        }
        if (!item)
        {
            item = names->AppendChild<Spreadsheet::DefinedName>();
        }
        item->SetName(StringValue(std::string(name)));
        item->SetLocalSheetId(UInt32Value(sheetIndex));
        item->SetText(formula);
    }
};

PageSetup Worksheet::GetPageSetup() const
{
    PageSetup result;
    const auto root = GetLowLevelApi();
    const auto setup = root ? root->GetFirstChildOfType<Spreadsheet::PageSetup>() : nullptr;
    if (!setup)
    {
        return result;
    }

    const auto orientation = setup->GetOrientation().ValueOr(Spreadsheet::OrientationValues());
    result.Orientation = orientation.GetValue() == Spreadsheet::OrientationValues::Landscape
                             ? PageOrientation::Landscape
                             : PageOrientation::Portrait;
    if (setup->GetPaperSize().IsDefined())
    {
        result.PaperSize = static_cast<PaperSize>(setup->GetPaperSize().Value());
    }
    if (setup->GetScale().IsDefined())
    {
        result.Scale = setup->GetScale().Value();
    }
    if (setup->GetFitToWidth().IsDefined())
    {
        result.FitToWidth = setup->GetFitToWidth().Value();
    }
    if (setup->GetFitToHeight().IsDefined())
    {
        result.FitToHeight = setup->GetFitToHeight().Value();
    }
    return result;
}

bool Worksheet::SetPageSetup(const PageSetup& value)
{
    const auto root = GetLowLevelApi();
    if (!root || (value.Scale && (*value.Scale < 10 || *value.Scale > 400)))
    {
        return false;
    }

    auto setup = root->GetFirstChildOfType<Spreadsheet::PageSetup>();
    if (!setup)
    {
        const auto before = root->GetFirstChildOfType<Spreadsheet::HeaderFooter>();
        setup = before ? root->InsertChild<Spreadsheet::PageSetup>(before)
                       : root->AppendChild<Spreadsheet::PageSetup>();
    }

    const auto orientation = value.Orientation == PageOrientation::Landscape
                                 ? Spreadsheet::OrientationValues::Landscape
                                 : Spreadsheet::OrientationValues::Portrait;
    setup->SetOrientation(EnumValue<Spreadsheet::OrientationValues>(Spreadsheet::OrientationValues(orientation)));
    setup->SetPaperSize(value.PaperSize ? UInt32Value(static_cast<UInt32>(*value.PaperSize))
                                        : UInt32Value());
    setup->SetScale(value.Scale ? UInt32Value(*value.Scale) : UInt32Value());
    setup->SetFitToWidth(value.FitToWidth ? UInt32Value(*value.FitToWidth) : UInt32Value());
    setup->SetFitToHeight(value.FitToHeight ? UInt32Value(*value.FitToHeight) : UInt32Value());
    return true;
}

PageMargins Worksheet::GetPageMargins() const
{
    PageMargins result;
    const auto root = GetLowLevelApi();
    const auto margins = root ? root->GetFirstChildOfType<Spreadsheet::PageMargins>() : nullptr;
    if (!margins)
    {
        return result;
    }

    const auto inches = [](Real value)
    {
        return ExyokiOffice::MeasuringUnits(value, ExyokiOffice::MeasurementUnit::Inch);
    };
    result.Left = inches(margins->GetLeft().ValueOr(result.Left.ToIN().GetValue()));
    result.Right = inches(margins->GetRight().ValueOr(result.Right.ToIN().GetValue()));
    result.Top = inches(margins->GetTop().ValueOr(result.Top.ToIN().GetValue()));
    result.Bottom = inches(margins->GetBottom().ValueOr(result.Bottom.ToIN().GetValue()));
    result.Header = inches(margins->GetHeader().ValueOr(result.Header.ToIN().GetValue()));
    result.Footer = inches(margins->GetFooter().ValueOr(result.Footer.ToIN().GetValue()));
    return result;
}

bool Worksheet::SetPageMargins(const PageMargins& value)
{
    const auto root = GetLowLevelApi();
    if (!root || !WorksheetPrintHelpers::ValidMargins(value))
    {
        return false;
    }

    auto margins = root->GetFirstChildOfType<Spreadsheet::PageMargins>();
    if (!margins)
    {
        const auto before = root->GetFirstChildOfType<Spreadsheet::PageSetup>();
        margins = before ? root->InsertChild<Spreadsheet::PageMargins>(before)
                         : root->AppendChild<Spreadsheet::PageMargins>();
    }
    margins->SetLeft(DoubleValue(value.Left.ToIN().GetValue()));
    margins->SetRight(DoubleValue(value.Right.ToIN().GetValue()));
    margins->SetTop(DoubleValue(value.Top.ToIN().GetValue()));
    margins->SetBottom(DoubleValue(value.Bottom.ToIN().GetValue()));
    margins->SetHeader(DoubleValue(value.Header.ToIN().GetValue()));
    margins->SetFooter(DoubleValue(value.Footer.ToIN().GetValue()));
    return true;
}

PrintOptions Worksheet::GetPrintOptions() const
{
    PrintOptions result;
    const auto root = GetLowLevelApi();
    const auto options = root ? root->GetFirstChildOfType<Spreadsheet::PrintOptions>() : nullptr;
    if (!options)
    {
        return result;
    }
    result.HorizontalCentered = options->GetHorizontalCentered().ValueOr(false);
    result.VerticalCentered = options->GetVerticalCentered().ValueOr(false);
    result.Headings = options->GetHeadings().ValueOr(false);
    result.GridLines = options->GetGridLines().ValueOr(false);
    return result;
}

bool Worksheet::SetPrintOptions(const PrintOptions& value)
{
    const auto root = GetLowLevelApi();
    if (!root)
    {
        return false;
    }
    auto options = root->GetFirstChildOfType<Spreadsheet::PrintOptions>();
    if (!options)
    {
        const auto before = root->GetFirstChildOfType<Spreadsheet::PageMargins>();
        options = before ? root->InsertChild<Spreadsheet::PrintOptions>(before)
                         : root->AppendChild<Spreadsheet::PrintOptions>();
    }
    options->SetHorizontalCentered(BooleanValue(value.HorizontalCentered));
    options->SetVerticalCentered(BooleanValue(value.VerticalCentered));
    options->SetHeadings(BooleanValue(value.Headings));
    options->SetGridLines(BooleanValue(value.GridLines));
    options->SetGridLinesSet(BooleanValue(value.GridLines));
    return true;
}

HeaderFooter Worksheet::GetHeaderFooter() const
{
    HeaderFooter result;
    const auto root = GetLowLevelApi();
    const auto headerFooter = root ? root->GetFirstChildOfType<Spreadsheet::HeaderFooter>() : nullptr;
    if (!headerFooter)
    {
        return result;
    }
    const auto text = [&headerFooter]<typename T>()
    {
        const auto element = headerFooter->GetFirstChildOfType<T>();
        return element ? std::string(element->GetText()) : std::string{};
    };
    result.OddHeader = text.template operator()<Spreadsheet::OddHeader>();
    result.OddFooter = text.template operator()<Spreadsheet::OddFooter>();
    result.EvenHeader = text.template operator()<Spreadsheet::EvenHeader>();
    result.EvenFooter = text.template operator()<Spreadsheet::EvenFooter>();
    result.FirstHeader = text.template operator()<Spreadsheet::FirstHeader>();
    result.FirstFooter = text.template operator()<Spreadsheet::FirstFooter>();
    result.DifferentOddEven = headerFooter->GetDifferentOddEven().ValueOr(false);
    result.DifferentFirst = headerFooter->GetDifferentFirst().ValueOr(false);
    return result;
}

bool Worksheet::SetHeaderFooter(const HeaderFooter& value)
{
    const auto root = GetLowLevelApi();
    if (!root)
    {
        return false;
    }
    auto headerFooter = root->GetFirstChildOfType<Spreadsheet::HeaderFooter>();
    if (!headerFooter)
    {
        headerFooter = root->AppendChild<Spreadsheet::HeaderFooter>();
    }
    const auto setText = [&headerFooter]<typename T>(const std::string& text)
    {
        auto element = headerFooter->GetFirstChildOfType<T>();
        if (!element)
        {
            element = headerFooter->AppendChild<T>();
        }
        element->SetText(text);
    };
    setText.template operator()<Spreadsheet::OddHeader>(value.OddHeader);
    setText.template operator()<Spreadsheet::OddFooter>(value.OddFooter);
    setText.template operator()<Spreadsheet::EvenHeader>(value.EvenHeader);
    setText.template operator()<Spreadsheet::EvenFooter>(value.EvenFooter);
    setText.template operator()<Spreadsheet::FirstHeader>(value.FirstHeader);
    setText.template operator()<Spreadsheet::FirstFooter>(value.FirstFooter);
    headerFooter->SetDifferentOddEven(BooleanValue(value.DifferentOddEven));
    headerFooter->SetDifferentFirst(BooleanValue(value.DifferentFirst));
    return true;
}

std::vector<CellRange> Worksheet::GetPrintArea() const
{
    std::vector<CellRange> result;
    const auto index = WorksheetPrintHelpers::SheetIndex(m_document, m_name);
    const auto item = index ? WorksheetPrintHelpers::FindDefinedName(m_document, "_xlnm.Print_Area", *index) : nullptr;
    if (!item)
    {
        return result;
    }
    const std::string formula(item->GetText());
    Size start = 0;
    while (start < formula.size())
    {
        const auto comma = formula.find(',', start);
        const auto token = formula.substr(start, comma - start);
        const auto separator = token.find('!');
        if (separator != std::string::npos)
        {
            const auto range = CellRange::ParseA1(WorksheetPrintHelpers::StripAbsoluteMarkers(token.substr(separator + 1)));
            if (range)
            {
                result.push_back(*range);
            }
        }
        if (comma == std::string::npos)
        {
            break;
        }
        start = comma + 1;
    }
    return result;
}

bool Worksheet::SetPrintArea(const std::vector<CellRange>& areas)
{
    const auto index = WorksheetPrintHelpers::SheetIndex(m_document, m_name);
    if (!index)
    {
        return false;
    }
    std::string formula;
    for (const auto& area : areas)
    {
        if (!area.IsValid())
        {
            return false;
        }
        if (!formula.empty())
        {
            formula += ",";
        }
        formula += WorksheetPrintHelpers::QuoteSheetName(m_name) + "!" +
                   WorksheetPrintHelpers::AbsoluteRange(area);
    }
    WorksheetPrintHelpers::SetDefinedName(m_document, "_xlnm.Print_Area", *index, formula);
    return true;
}

PrintTitles Worksheet::GetPrintTitles() const
{
    PrintTitles result;
    const auto index = WorksheetPrintHelpers::SheetIndex(m_document, m_name);
    const auto item = index ? WorksheetPrintHelpers::FindDefinedName(m_document, "_xlnm.Print_Titles", *index) : nullptr;
    if (!item)
    {
        return result;
    }
    const std::string formula(item->GetText());
    Size start = 0;
    while (start < formula.size())
    {
        const auto comma = formula.find(',', start);
        const auto token = std::string_view(formula).substr(start, comma - start);
        const auto separator = token.find('!');
        if (separator != std::string_view::npos)
        {
            const auto reference = token.substr(separator + 1);
            const auto colon = reference.find(':');
            if (colon != std::string_view::npos)
            {
                const auto first = WorksheetPrintHelpers::StripAbsoluteMarkers(std::string(reference.substr(0, colon)));
                const auto last = WorksheetPrintHelpers::StripAbsoluteMarkers(std::string(reference.substr(colon + 1)));
                const auto firstRow = RowIndex::TryCreate(static_cast<UInt32>(std::strtoul(first.c_str(), nullptr, 10)));
                const auto lastRow = RowIndex::TryCreate(static_cast<UInt32>(std::strtoul(last.c_str(), nullptr, 10)));
                if (firstRow && lastRow)
                {
                    result.Rows = {{firstRow->Value(), lastRow->Value()}};
                }
                else
                {
                    const auto firstColumn = ColumnIndex::ParseName(first);
                    const auto lastColumn = ColumnIndex::ParseName(last);
                    if (firstColumn && lastColumn)
                    {
                        result.Columns = {{firstColumn->Value(), lastColumn->Value()}};
                    }
                }
            }
        }
        if (comma == std::string::npos)
        {
            break;
        }
        start = comma + 1;
    }
    return result;
}

bool Worksheet::SetPrintTitles(const PrintTitles& value)
{
    const auto valid = [](const auto& range, UInt32 maximum)
    {
        return !range || (range->first >= 1 && range->first <= range->second && range->second <= maximum);
    };
    if (!valid(value.Rows, MaxRowIndex) || !valid(value.Columns, MaxColumnIndex))
    {
        return false;
    }
    const auto index = WorksheetPrintHelpers::SheetIndex(m_document, m_name);
    if (!index)
    {
        return false;
    }
    const auto prefix = WorksheetPrintHelpers::QuoteSheetName(m_name) + "!";
    std::string formula;
    if (value.Columns)
    {
        // Both endpoints were bounds-checked by valid() above.
        const auto first = ColumnIndex(value.Columns->first).ToName();
        const auto last = ColumnIndex(value.Columns->second).ToName();
        formula = prefix + "$" + first + ":$" + last;
    }
    if (value.Rows)
    {
        if (!formula.empty())
        {
            formula += ",";
        }
        formula += prefix + "$" + std::to_string(value.Rows->first) + ":$" +
                   std::to_string(value.Rows->second);
    }
    WorksheetPrintHelpers::SetDefinedName(m_document, "_xlnm.Print_Titles", *index, formula);
    return true;
}
} // namespace ExyokiOffice::Excel
