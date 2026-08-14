// Copyright (c) 2026 Jakub Melka and Collaborators
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "ExyokiOffice/Excel/ExcelAddress.hpp"
#include "ExyokiOffice/StandardTypes.hpp"

#include "AsciiText.hpp"

#include <algorithm>

namespace ExyokiOffice::Excel
{

/** Non-public parsing helpers shared by the address, range, and sheet-range types below. */
class ExcelAddressParsing
{
public:
    static bool IsAsciiLetter(char ch) noexcept
    {
        return (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z');
    }

    static bool IsDigit(char ch) noexcept
    {
        return ch >= '0' && ch <= '9';
    }

    static std::optional<UInt32> ParsePositiveUInt(std::string_view text, UInt32 maxValue)
    {
        if (text.empty())
        {
            return std::nullopt;
        }

        UInt32 value = 0;
        for (char ch : text)
        {
            if (!IsDigit(ch))
            {
                return std::nullopt;
            }
            const auto digit = static_cast<UInt32>(ch - '0');
            if (value > (maxValue - digit) / 10)
            {
                return std::nullopt;
            }
            value = value * 10 + digit;
        }
        return value == 0 ? std::nullopt : std::optional<UInt32>(value);
    }

    static std::optional<CellRange> ParseRange(std::string_view text,
                                               std::optional<CellAddress> (*parseCell)(std::string_view))
    {
        const auto separator = text.find(':');
        if (separator == std::string_view::npos)
        {
            auto cell = parseCell(text);
            return cell ? CellRange::TryCreate(*cell, *cell) : std::nullopt;
        }
        if (text.find(':', separator + 1) != std::string_view::npos)
        {
            return std::nullopt;
        }

        auto first = parseCell(text.substr(0, separator));
        auto last = parseCell(text.substr(separator + 1));
        if (!first || !last)
        {
            return std::nullopt;
        }
        return CellRange::TryCreate(*first, *last);
    }

    /**
     * Returns true when a worksheet name must be single-quoted in a formula:
     * empty, starting with a digit, or containing any character other than an
     * ASCII letter, digit, or underscore.
     */
    static bool SheetNameRequiresQuoting(const std::string& name)
    {
        if (name.empty() || IsDigit(name.front()))
        {
            return true;
        }
        return std::ranges::any_of(name, [](char ch)
                                   { return !(IsAsciiLetter(ch) || IsDigit(ch) || ch == '_'); });
    }

    /** Quotes a worksheet name for use in a formula, doubling any embedded single quotes. */
    static std::string QuoteSheetName(const std::string& name)
    {
        if (!SheetNameRequiresQuoting(name))
        {
            return name;
        }
        std::string result = "'";
        for (char ch : name)
        {
            result += ch;
            if (ch == '\'')
            {
                result += '\'';
            }
        }
        return result + "'";
    }

    /** Reverses QuoteSheetName: strips surrounding quotes and un-doubles embedded ones. */
    static std::string UnquoteSheetName(const std::string& name)
    {
        if (name.size() < 2 || name.front() != '\'' || name.back() != '\'')
        {
            return name;
        }
        std::string result;
        for (Size i = 1; i + 1 < name.size(); ++i)
        {
            result += name[i];
            if (name[i] == '\'' && i + 2 < name.size() && name[i + 1] == '\'')
            {
                ++i;
            }
        }
        return result;
    }

    static std::string AbsoluteA1(const CellAddress& value)
    {
        return "$" + value.Column().ToName() + "$" + std::to_string(value.Row().Value());
    }

    static std::string AbsoluteA1(const CellRange& range)
    {
        const auto first = AbsoluteA1(range.First());
        return range.First().Row().Value() == range.Last().Row().Value() &&
                       range.First().Column().Value() == range.Last().Column().Value()
                   ? first
                   : first + ":" + AbsoluteA1(range.Last());
    }
};

std::optional<RowIndex> RowIndex::TryCreate(UInt32 value) noexcept
{
    RowIndex row(value);
    return row.IsValid() ? std::optional<RowIndex>(row) : std::nullopt;
}

std::optional<ColumnIndex> ColumnIndex::TryCreate(UInt32 value) noexcept
{
    ColumnIndex column(value);
    return column.IsValid() ? std::optional<ColumnIndex>(column) : std::nullopt;
}

std::optional<ColumnIndex> ColumnIndex::ParseName(std::string_view name)
{
    if (name.empty())
    {
        return std::nullopt;
    }

    UInt32 value = 0;
    for (char ch : name)
    {
        if (!ExcelAddressParsing::IsAsciiLetter(ch))
        {
            return std::nullopt;
        }
        const auto digit = static_cast<UInt32>(AsciiText::ToUpper(ch) - 'A' + 1);
        if (value > (MaxColumnIndex - digit) / 26)
        {
            return std::nullopt;
        }
        value = value * 26 + digit;
    }
    return TryCreate(value);
}

std::string ColumnIndex::ToName() const
{
    if (!IsValid())
    {
        return {};
    }

    auto column = m_value;
    std::string name;
    while (column > 0)
    {
        --column;
        name.push_back(static_cast<char>('A' + (column % 26)));
        column /= 26;
    }
    std::reverse(name.begin(), name.end());
    return name;
}

std::optional<CellAddress> CellAddress::TryCreate(UInt32 row, UInt32 column) noexcept
{
    auto rowIndex = RowIndex::TryCreate(row);
    auto columnIndex = ColumnIndex::TryCreate(column);
    if (!rowIndex || !columnIndex)
    {
        return std::nullopt;
    }
    return CellAddress(*rowIndex, *columnIndex);
}

std::optional<CellAddress> CellAddress::ParseA1(std::string_view text)
{
    if (text.empty())
    {
        return std::nullopt;
    }

    Size pos = 0;
    if (text[pos] == '$')
    {
        ++pos;
    }

    const auto columnStart = pos;
    while (pos < text.size() && ExcelAddressParsing::IsAsciiLetter(text[pos]))
    {
        ++pos;
    }
    if (columnStart == pos)
    {
        return std::nullopt;
    }
    const auto columnEnd = pos;

    if (pos < text.size() && text[pos] == '$')
    {
        ++pos;
    }

    const auto rowStart = pos;
    while (pos < text.size() && ExcelAddressParsing::IsDigit(text[pos]))
    {
        ++pos;
    }
    if (rowStart == pos || pos != text.size())
    {
        return std::nullopt;
    }

    auto column = ColumnIndex::ParseName(text.substr(columnStart, columnEnd - columnStart));
    if (!column)
    {
        return std::nullopt;
    }
    auto row = ExcelAddressParsing::ParsePositiveUInt(text.substr(rowStart), MaxRowIndex);
    if (!row)
    {
        return std::nullopt;
    }
    return TryCreate(*row, column->Value());
}

std::optional<CellAddress> CellAddress::ParseR1C1(std::string_view text)
{
    if (text.size() < 4 || AsciiText::ToUpper(text[0]) != 'R')
    {
        return std::nullopt;
    }

    Size pos = 1;
    const auto rowStart = pos;
    while (pos < text.size() && ExcelAddressParsing::IsDigit(text[pos]))
    {
        ++pos;
    }
    if (rowStart == pos || pos >= text.size() || AsciiText::ToUpper(text[pos]) != 'C')
    {
        return std::nullopt;
    }

    auto row = ExcelAddressParsing::ParsePositiveUInt(text.substr(rowStart, pos - rowStart), MaxRowIndex);
    ++pos;
    const auto columnStart = pos;
    while (pos < text.size() && ExcelAddressParsing::IsDigit(text[pos]))
    {
        ++pos;
    }
    if (columnStart == pos || pos != text.size())
    {
        return std::nullopt;
    }

    auto column = ExcelAddressParsing::ParsePositiveUInt(text.substr(columnStart), MaxColumnIndex);
    if (!row || !column)
    {
        return std::nullopt;
    }
    return TryCreate(*row, *column);
}

std::string CellAddress::ToA1() const
{
    return IsValid() ? m_column.ToName() + std::to_string(m_row.Value()) : std::string{};
}

std::string CellAddress::ToR1C1() const
{
    return IsValid() ? "R" + std::to_string(m_row.Value()) + "C" + std::to_string(m_column.Value()) : std::string{};
}

std::optional<CellRange> CellRange::TryCreate(CellAddress first, CellAddress last) noexcept
{
    if (!first.IsValid() || !last.IsValid())
    {
        return std::nullopt;
    }
    if (first.Row().Value() > last.Row().Value() || first.Column().Value() > last.Column().Value())
    {
        return std::nullopt;
    }
    return CellRange(first, last);
}

std::optional<CellRange> CellRange::ParseA1(std::string_view text)
{
    return ExcelAddressParsing::ParseRange(text, &CellAddress::ParseA1);
}

std::optional<CellRange> CellRange::ParseR1C1(std::string_view text)
{
    return ExcelAddressParsing::ParseRange(text, &CellAddress::ParseR1C1);
}

bool CellRange::IsValid() const noexcept
{
    return TryCreate(m_first, m_last).has_value();
}

UInt32 CellRange::RowCount() const noexcept
{
    return IsValid() ? (m_last.Row().Value() - m_first.Row().Value() + 1) : 0;
}

UInt32 CellRange::ColumnCount() const noexcept
{
    return IsValid() ? (m_last.Column().Value() - m_first.Column().Value() + 1) : 0;
}

std::string CellRange::ToA1() const
{
    if (!IsValid())
    {
        return {};
    }
    auto first = m_first.ToA1();
    auto last = m_last.ToA1();
    return first == last ? first : first + ":" + last;
}

std::string CellRange::ToR1C1() const
{
    if (!IsValid())
    {
        return {};
    }
    auto first = m_first.ToR1C1();
    auto last = m_last.ToR1C1();
    return first == last ? first : first + ":" + last;
}

std::optional<SheetCellRange> SheetCellRange::Parse(std::string_view formula)
{
    if (formula.empty())
    {
        return std::nullopt;
    }
    const auto bang = formula.find_last_of('!');
    if (bang == std::string_view::npos)
    {
        return std::nullopt;
    }
    std::string sheet = ExcelAddressParsing::UnquoteSheetName(std::string(formula.substr(0, bang)));

    std::string rangeText(formula.substr(bang + 1));
    rangeText.erase(std::remove(rangeText.begin(), rangeText.end(), '$'), rangeText.end());
    auto range = CellRange::ParseA1(rangeText);
    if (!range)
    {
        return std::nullopt;
    }
    return SheetCellRange(std::move(sheet), *range);
}

std::string SheetCellRange::ToFormula() const
{
    if (!m_range.IsValid())
    {
        return {};
    }
    return ExcelAddressParsing::QuoteSheetName(m_sheet) + "!" + ExcelAddressParsing::AbsoluteA1(m_range);
}

} // namespace ExyokiOffice::Excel
