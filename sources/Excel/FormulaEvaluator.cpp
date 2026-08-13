// Copyright (c) 2026 Jakub Melka and Collaborators
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "FormulaEvaluator.hpp"

#include "FormulaFunctions.hpp"

#include "ExyokiOffice/Excel/ExcelNamedRange.hpp"
#include "ExyokiOffice/StandardTypes.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <limits>
#include <utility>

namespace ExyokiOffice::Excel
{

namespace FormulaEvaluatorHelpers
{

constexpr int MaxEvaluationDepth = 128;
/** Guard against materializing enormous matrices (16 million elements). */
constexpr Size MaxMaterializedElements = Size(1) << 24;

char AsciiUpper(char c)
{
    return (c >= 'a' && c <= 'z') ? static_cast<char>(c - 'a' + 'A') : c;
}

char AsciiLower(char c)
{
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

std::string ToLower(std::string_view text)
{
    std::string result(text);
    for (char& c : result)
    {
        c = AsciiLower(c);
    }
    return result;
}

int CompareTextIgnoreCase(std::string_view left, std::string_view right)
{
    const Size common = std::min(left.size(), right.size());
    for (Size i = 0; i < common; ++i)
    {
        const char l = AsciiUpper(left[i]);
        const char r = AsciiUpper(right[i]);
        if (l != r)
        {
            return l < r ? -1 : 1;
        }
    }
    if (left.size() == right.size())
    {
        return 0;
    }
    return left.size() < right.size() ? -1 : 1;
}

/** Days from civil algorithm (proleptic Gregorian), Howard Hinnant style. */
constexpr Int64 DaysFromCivil(Int64 year, Int64 month, Int64 day)
{
    year -= month <= 2;
    const Int64 era = (year >= 0 ? year : year - 399) / 400;
    const Int64 yearOfEra = year - era * 400;
    const Int64 dayOfYear = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
    const Int64 dayOfEra = yearOfEra * 365 + yearOfEra / 4 - yearOfEra / 100 + dayOfYear;
    return era * 146097 + dayOfEra - 719468;
}

void CivilFromDays(Int64 days, Int32& year, UInt32& month, UInt32& day)
{
    days += 719468;
    const Int64 era = (days >= 0 ? days : days - 146096) / 146097;
    const Int64 dayOfEra = days - era * 146097;
    const Int64 yearOfEra = (dayOfEra - dayOfEra / 1460 + dayOfEra / 36524 - dayOfEra / 146096) / 365;
    const Int64 y = yearOfEra + era * 400;
    const Int64 dayOfYear = dayOfEra - (365 * yearOfEra + yearOfEra / 4 - yearOfEra / 100);
    const Int64 mp = (5 * dayOfYear + 2) / 153;
    day = static_cast<UInt32>(dayOfYear - (153 * mp + 2) / 5 + 1);
    month = static_cast<UInt32>(mp + (mp < 10 ? 3 : -9));
    year = static_cast<Int32>(y + (month <= 2 ? 1 : 0));
}

constexpr Int64 EpochDays1899_12_31 = DaysFromCivil(1899, 12, 31);
constexpr Int64 EpochDays1899_12_30 = DaysFromCivil(1899, 12, 30);

} // namespace FormulaEvaluatorHelpers

// ---------------------------------------------------------------------------
// FormulaCoercion
// ---------------------------------------------------------------------------

std::optional<Real> FormulaCoercion::ParseNumberText(std::string_view text, bool acceptRichForms)
{
    Size begin = 0;
    Size end = text.size();
    while (begin < end && (text[begin] == ' ' || text[begin] == '\t'))
    {
        ++begin;
    }
    while (end > begin && (text[end - 1] == ' ' || text[end - 1] == '\t'))
    {
        --end;
    }
    if (begin >= end)
    {
        return std::nullopt;
    }
    std::string_view trimmed = text.substr(begin, end - begin);

    Real percentDivisor = 1.0;
    if (acceptRichForms)
    {
        while (!trimmed.empty() && trimmed.back() == '%')
        {
            percentDivisor *= 100.0;
            trimmed.remove_suffix(1);
        }
        if (const auto isoSerial = ExcelDateSerial::ParseIso(trimmed); isoSerial && percentDivisor == 1.0)
        {
            return *isoSerial;
        }
    }
    if (trimmed.empty())
    {
        return std::nullopt;
    }

    Real value = 0.0;
    const char* first = trimmed.data();
    const char* last = trimmed.data() + trimmed.size();
    // std::from_chars rejects a leading '+', which Excel accepts.
    if (*first == '+')
    {
        ++first;
        if (first == last)
        {
            return std::nullopt;
        }
    }
    const auto result = std::from_chars(first, last, value);
    if (result.ec != std::errc() || result.ptr != last || !std::isfinite(value))
    {
        return std::nullopt;
    }
    return value / percentDivisor;
}

std::string FormulaCoercion::FormatNumber(Real value)
{
    char buffer[64]{};
    auto [ptr, ec] = std::to_chars(buffer, buffer + sizeof(buffer), value);
    if (ec == std::errc())
    {
        return std::string(buffer, ptr);
    }
    std::snprintf(buffer, sizeof(buffer), "%.17g", value);
    return std::string(buffer);
}

FormulaValue FormulaCoercion::ToNumber(const FormulaValue& value)
{
    switch (value.Kind())
    {
        case FormulaValueKind::Blank:
            return FormulaValue::Number(0.0);
        case FormulaValueKind::Number:
            return value;
        case FormulaValueKind::Boolean:
            return FormulaValue::Number(*value.BooleanValue() ? 1.0 : 0.0);
        case FormulaValueKind::Text:
        {
            const auto parsed = ParseNumberText(value.TextValue());
            if (!parsed)
            {
                return FormulaValue::Error(FormulaErrorCode::Value);
            }
            return FormulaValue::Number(*parsed);
        }
        case FormulaValueKind::Error:
            return value;
        case FormulaValueKind::Array:
            return ToNumber(value.At(0, 0));
    }
    return FormulaValue::Error(FormulaErrorCode::Value);
}

FormulaValue FormulaCoercion::ToText(const FormulaValue& value)
{
    switch (value.Kind())
    {
        case FormulaValueKind::Blank:
            return FormulaValue::Text({});
        case FormulaValueKind::Number:
            return FormulaValue::Text(FormatNumber(*value.NumberValue()));
        case FormulaValueKind::Boolean:
            return FormulaValue::Text(*value.BooleanValue() ? "TRUE" : "FALSE");
        case FormulaValueKind::Text:
        case FormulaValueKind::Error:
            return value;
        case FormulaValueKind::Array:
            return ToText(value.At(0, 0));
    }
    return FormulaValue::Error(FormulaErrorCode::Value);
}

FormulaValue FormulaCoercion::ToBoolean(const FormulaValue& value)
{
    switch (value.Kind())
    {
        case FormulaValueKind::Blank:
            return FormulaValue::Boolean(false);
        case FormulaValueKind::Number:
            return FormulaValue::Boolean(*value.NumberValue() != 0.0);
        case FormulaValueKind::Boolean:
            return value;
        case FormulaValueKind::Text:
        {
            const std::string& text = value.TextValue();
            if (FormulaEvaluatorHelpers::CompareTextIgnoreCase(text, "TRUE") == 0)
            {
                return FormulaValue::Boolean(true);
            }
            if (FormulaEvaluatorHelpers::CompareTextIgnoreCase(text, "FALSE") == 0)
            {
                return FormulaValue::Boolean(false);
            }
            return FormulaValue::Error(FormulaErrorCode::Value);
        }
        case FormulaValueKind::Error:
            return value;
        case FormulaValueKind::Array:
            return ToBoolean(value.At(0, 0));
    }
    return FormulaValue::Error(FormulaErrorCode::Value);
}

std::optional<int> FormulaCoercion::Compare(const FormulaValue& left, const FormulaValue& right)
{
    if (left.IsError() || right.IsError())
    {
        return std::nullopt;
    }

    const auto typeRank = [](const FormulaValue& value) -> int
    {
        switch (value.Kind())
        {
            case FormulaValueKind::Number:
                return 0;
            case FormulaValueKind::Text:
                return 1;
            case FormulaValueKind::Boolean:
                return 2;
            default:
                return 0;
        }
    };

    // Blank adapts to the other operand: it equals 0, the empty string, and FALSE.
    FormulaValue effectiveLeft = left;
    FormulaValue effectiveRight = right;
    if (left.Kind() == FormulaValueKind::Blank && right.Kind() == FormulaValueKind::Blank)
    {
        return 0;
    }
    if (left.Kind() == FormulaValueKind::Blank)
    {
        switch (right.Kind())
        {
            case FormulaValueKind::Text:
                effectiveLeft = FormulaValue::Text({});
                break;
            case FormulaValueKind::Boolean:
                effectiveLeft = FormulaValue::Boolean(false);
                break;
            default:
                effectiveLeft = FormulaValue::Number(0.0);
                break;
        }
    }
    if (right.Kind() == FormulaValueKind::Blank)
    {
        switch (left.Kind())
        {
            case FormulaValueKind::Text:
                effectiveRight = FormulaValue::Text({});
                break;
            case FormulaValueKind::Boolean:
                effectiveRight = FormulaValue::Boolean(false);
                break;
            default:
                effectiveRight = FormulaValue::Number(0.0);
                break;
        }
    }

    const int leftRank = typeRank(effectiveLeft);
    const int rightRank = typeRank(effectiveRight);
    if (leftRank != rightRank)
    {
        return leftRank < rightRank ? -1 : 1;
    }

    switch (effectiveLeft.Kind())
    {
        case FormulaValueKind::Number:
        {
            const Real l = *effectiveLeft.NumberValue();
            const Real r = *effectiveRight.NumberValue();
            if (l < r)
            {
                return -1;
            }
            if (l > r)
            {
                return 1;
            }
            return 0;
        }
        case FormulaValueKind::Text:
            return FormulaEvaluatorHelpers::CompareTextIgnoreCase(effectiveLeft.TextValue(),
                                                                  effectiveRight.TextValue());
        case FormulaValueKind::Boolean:
        {
            const int l = *effectiveLeft.BooleanValue() ? 1 : 0;
            const int r = *effectiveRight.BooleanValue() ? 1 : 0;
            return l - r;
        }
        default:
            return 0;
    }
}

// ---------------------------------------------------------------------------
// ExcelDateSerial
// ---------------------------------------------------------------------------

std::optional<Real> ExcelDateSerial::FromParts(Int32 year,
                                               Int64 month,
                                               Int64 day,
                                               Real hours,
                                               Real minutes,
                                               Real seconds)
{
    // Normalize month overflow the way DATE() does: month 13 rolls over.
    Int64 totalMonths = static_cast<Int64>(year) * 12 + (month - 1);
    Int64 normalizedYear = totalMonths / 12;
    Int64 normalizedMonth = totalMonths % 12;
    if (normalizedMonth < 0)
    {
        normalizedMonth += 12;
        --normalizedYear;
    }

    const Int64 days =
        FormulaEvaluatorHelpers::DaysFromCivil(normalizedYear, normalizedMonth + 1, 1) + (day - 1);

    Real serial;
    const Int64 relative = days - FormulaEvaluatorHelpers::EpochDays1899_12_31;
    const Int64 firstOfMarch1900 = FormulaEvaluatorHelpers::DaysFromCivil(1900, 3, 1);
    if (days >= firstOfMarch1900)
    {
        serial = static_cast<Real>(relative + 1);
    }
    else
    {
        serial = static_cast<Real>(relative);
    }

    serial += hours / 24.0 + minutes / 1440.0 + seconds / 86400.0;
    if (serial < 0.0)
    {
        return std::nullopt;
    }
    return serial;
}

std::optional<ExcelDateSerial::DateParts> ExcelDateSerial::ToParts(Real serial)
{
    if (!(serial >= 0.0) || serial >= 2958466.0) // 9999-12-31 is serial 2958465
    {
        return std::nullopt;
    }

    Real dayPart = std::floor(serial);
    Real fraction = serial - dayPart;
    // Round the time to the nearest millisecond to absorb representation noise.
    Real secondsOfDay = fraction * 86400.0;
    secondsOfDay = std::round(secondsOfDay * 1000.0) / 1000.0;
    if (secondsOfDay >= 86400.0)
    {
        secondsOfDay -= 86400.0;
        dayPart += 1.0;
    }

    DateParts parts;
    const auto days = static_cast<Int64>(dayPart);
    Int64 civilDays;
    if (days >= 61)
    {
        civilDays = FormulaEvaluatorHelpers::EpochDays1899_12_30 + days;
    }
    else if (days == 60)
    {
        // The fictitious 1900-02-29 of the 1900 date system.
        civilDays = FormulaEvaluatorHelpers::DaysFromCivil(1900, 2, 28);
    }
    else
    {
        civilDays = FormulaEvaluatorHelpers::EpochDays1899_12_31 + days;
    }
    FormulaEvaluatorHelpers::CivilFromDays(civilDays, parts.year, parts.month, parts.day);

    parts.hour = static_cast<UInt32>(secondsOfDay / 3600.0);
    secondsOfDay -= parts.hour * 3600.0;
    parts.minute = static_cast<UInt32>(secondsOfDay / 60.0);
    parts.second = secondsOfDay - parts.minute * 60.0;
    return parts;
}

std::optional<Real> ExcelDateSerial::ParseIso(std::string_view text)
{
    const auto parseUnsigned = [](std::string_view part, UInt32& value) -> bool
    {
        if (part.empty())
        {
            return false;
        }
        value = 0;
        const auto result = std::from_chars(part.data(), part.data() + part.size(), value);
        return result.ec == std::errc() && result.ptr == part.data() + part.size();
    };

    if (!text.empty() && text.back() == 'Z')
    {
        text.remove_suffix(1);
    }

    std::string_view datePart = text;
    std::string_view timePart;
    const Size separator = text.find_first_of("T ");
    if (separator != std::string_view::npos)
    {
        datePart = text.substr(0, separator);
        timePart = text.substr(separator + 1);
    }

    Real dateSerial = 0.0;
    bool hasDate = false;
    if (datePart.find('-') != std::string_view::npos)
    {
        // YYYY-MM-DD
        if (datePart.size() < 8 || datePart[4] != '-' || datePart[7] != '-')
        {
            if (datePart.size() != 10 || datePart[4] != '-' || datePart[7] != '-')
            {
                return std::nullopt;
            }
        }
        UInt32 year = 0;
        UInt32 month = 0;
        UInt32 day = 0;
        if (datePart.size() != 10 || !parseUnsigned(datePart.substr(0, 4), year) ||
            !parseUnsigned(datePart.substr(5, 2), month) || !parseUnsigned(datePart.substr(8, 2), day) ||
            month < 1 || month > 12 || day < 1 || day > 31)
        {
            return std::nullopt;
        }
        const auto serial = FromParts(static_cast<Int32>(year), month, day);
        if (!serial)
        {
            return std::nullopt;
        }
        dateSerial = *serial;
        hasDate = true;
    }
    else if (separator == std::string_view::npos)
    {
        // No date separator found: the whole text may be a time value.
        timePart = text;
        datePart = {};
    }
    else if (!datePart.empty())
    {
        return std::nullopt;
    }

    Real timeSerial = 0.0;
    bool hasTime = false;
    if (!timePart.empty())
    {
        // HH:MM or HH:MM:SS with optional fractional seconds.
        const Size firstColon = timePart.find(':');
        if (firstColon == std::string_view::npos)
        {
            return std::nullopt;
        }
        UInt32 hour = 0;
        UInt32 minute = 0;
        Real seconds = 0.0;
        const Size secondColon = timePart.find(':', firstColon + 1);
        if (!parseUnsigned(timePart.substr(0, firstColon), hour))
        {
            return std::nullopt;
        }
        std::string_view minutePart = secondColon == std::string_view::npos
                                          ? timePart.substr(firstColon + 1)
                                          : timePart.substr(firstColon + 1, secondColon - firstColon - 1);
        if (!parseUnsigned(minutePart, minute) || hour > 23 || minute > 59)
        {
            return std::nullopt;
        }
        if (secondColon != std::string_view::npos)
        {
            const std::string_view secondsPart = timePart.substr(secondColon + 1);
            if (secondsPart.empty())
            {
                return std::nullopt;
            }
            const char* first = secondsPart.data();
            const char* last = secondsPart.data() + secondsPart.size();
            const auto result = std::from_chars(first, last, seconds);
            if (result.ec != std::errc() || result.ptr != last || seconds < 0.0 || seconds >= 60.0)
            {
                return std::nullopt;
            }
        }
        timeSerial = hour / 24.0 + minute / 1440.0 + seconds / 86400.0;
        hasTime = true;
    }

    if (!hasDate && !hasTime)
    {
        return std::nullopt;
    }
    return dateSerial + timeSerial;
}

// ---------------------------------------------------------------------------
// FormulaEvaluationSession
// ---------------------------------------------------------------------------

FormulaEvaluationSession::FormulaEvaluationSession(ExcelDocument::Ptr document,
                                                   const FormulaFunctionRegistry& registry)
    : m_document(std::move(document)), m_editor(m_document), m_registry(registry)
{
}

void FormulaEvaluationSession::SetCurrentSheet(std::string sheetName)
{
    m_currentSheet = std::move(sheetName);
}

std::mt19937& FormulaEvaluationSession::RandomEngine()
{
    if (!m_randomEngine)
    {
        m_randomEngine.emplace(std::random_device{}());
    }
    return *m_randomEngine;
}

FormulaEvaluationSession::SheetCache* FormulaEvaluationSession::FindSheet(std::string_view sheetName)
{
    if (!m_sheetsLoaded)
    {
        m_sheetsLoaded = true;
        for (const auto& worksheet : m_editor.Worksheets())
        {
            if (!worksheet)
            {
                continue;
            }
            SheetCache cache;
            cache.worksheet = worksheet;
            cache.displayName = worksheet->Name();
            m_sheets.emplace(FormulaEvaluatorHelpers::ToLower(cache.displayName), std::move(cache));
        }
    }
    const std::string key =
        FormulaEvaluatorHelpers::ToLower(sheetName.empty() ? std::string_view(m_currentSheet) : sheetName);
    const auto it = m_sheets.find(key);
    return it != m_sheets.end() ? &it->second : nullptr;
}

bool FormulaEvaluationSession::SheetExists(std::string_view sheetName)
{
    return FindSheet(sheetName) != nullptr;
}

std::string FormulaEvaluationSession::FirstSheetName()
{
    const auto first = m_editor.FirstWorksheet();
    return first ? first->Name() : std::string();
}

const std::vector<CellAddress>& FormulaEvaluationSession::StoredAddresses(SheetCache& cache)
{
    if (!cache.storedAddresses)
    {
        auto addresses = cache.worksheet->StoredCellAddresses();
        std::sort(addresses.begin(), addresses.end(), [](const CellAddress& a, const CellAddress& b)
                  {
            if (a.Row().Value() != b.Row().Value())
            {
                return a.Row().Value() < b.Row().Value();
            }
            return a.Column().Value() < b.Column().Value(); });
        cache.storedAddresses = std::move(addresses);
    }
    return *cache.storedAddresses;
}

FormulaValue FormulaEvaluationSession::CellValueToFormulaValue(const ExcelCellValue& cellValue)
{
    switch (cellValue.Kind())
    {
        case CellValueKind::Blank:
            return FormulaValue();
        case CellValueKind::Number:
        {
            const auto parsed = FormulaCoercion::ParseNumberText(cellValue.Text());
            return FormulaValue::Number(parsed.value_or(0.0));
        }
        case CellValueKind::InlineString:
            return FormulaValue::Text(cellValue.Text());
        case CellValueKind::SharedString:
        {
            SharedStringTableService sharedStrings(m_document);
            const auto index = cellValue.SharedStringIndex();
            if (index)
            {
                if (auto text = sharedStrings.Lookup(*index))
                {
                    return FormulaValue::Text(std::move(*text));
                }
            }
            return FormulaValue::Text({});
        }
        case CellValueKind::Boolean:
            return FormulaValue::Boolean(cellValue.BooleanValue().value_or(false));
        case CellValueKind::Error:
        {
            const auto code = ParseFormulaErrorText(cellValue.Text());
            return FormulaValue::Error(code.value_or(FormulaErrorCode::Value));
        }
        case CellValueKind::DateTime:
        {
            if (const auto serial = ExcelDateSerial::ParseIso(cellValue.Text()))
            {
                return FormulaValue::Number(*serial);
            }
            return FormulaValue::Text(cellValue.Text());
        }
        case CellValueKind::Formula:
        {
            const CellFormulaValue& formula = cellValue.FormulaValue();
            switch (formula.CachedKind)
            {
                case FormulaCachedValueKind::None:
                    return FormulaValue();
                case FormulaCachedValueKind::SharedString:
                {
                    UInt32 index = 0;
                    const auto result = std::from_chars(
                        formula.CachedText.data(), formula.CachedText.data() + formula.CachedText.size(), index);
                    if (result.ec == std::errc())
                    {
                        SharedStringTableService sharedStrings(m_document);
                        if (auto text = sharedStrings.Lookup(index))
                        {
                            return FormulaValue::Text(std::move(*text));
                        }
                    }
                    return FormulaValue::Text({});
                }
                case FormulaCachedValueKind::String:
                    return FormulaValue::Text(formula.CachedText);
                case FormulaCachedValueKind::Number:
                {
                    const auto parsed = FormulaCoercion::ParseNumberText(formula.CachedText);
                    return FormulaValue::Number(parsed.value_or(0.0));
                }
                case FormulaCachedValueKind::Boolean:
                    return FormulaValue::Boolean(formula.CachedText == "1" ||
                                                 FormulaEvaluatorHelpers::CompareTextIgnoreCase(
                                                     formula.CachedText, "TRUE") == 0);
                case FormulaCachedValueKind::Error:
                {
                    const auto code = ParseFormulaErrorText(formula.CachedText);
                    return FormulaValue::Error(code.value_or(FormulaErrorCode::Value));
                }
                case FormulaCachedValueKind::DateTime:
                {
                    if (const auto serial = ExcelDateSerial::ParseIso(formula.CachedText))
                    {
                        return FormulaValue::Number(*serial);
                    }
                    return FormulaValue::Text(formula.CachedText);
                }
            }
            return FormulaValue();
        }
    }
    return FormulaValue();
}

FormulaValue FormulaEvaluationSession::ReadCell(std::string_view sheetName,
                                                UInt32 row,
                                                UInt32 column)
{
    SheetCache* sheet = FindSheet(sheetName);
    if (!sheet)
    {
        return FormulaValue::Error(FormulaErrorCode::Ref);
    }
    if (m_overlay)
    {
        CellKey key;
        key.sheet = FormulaEvaluatorHelpers::ToLower(sheet->displayName);
        key.row = row;
        key.column = column;
        const auto it = m_overlay->find(key);
        if (it != m_overlay->end())
        {
            return it->second;
        }
    }
    const auto address = CellAddress::TryCreate(row, column);
    if (!address)
    {
        return FormulaValue::Error(FormulaErrorCode::Ref);
    }
    const auto cellValue = sheet->worksheet->GetCellValue(*address);
    if (!cellValue)
    {
        return FormulaValue();
    }
    return CellValueToFormulaValue(*cellValue);
}

bool FormulaEvaluationSession::ForEachStoredCell(
    const ResolvedReferenceArea& area,
    const std::function<void(UInt32, UInt32, const FormulaValue&)>& callback)
{
    SheetCache* sheet = FindSheet(area.sheet);
    if (!sheet)
    {
        return false;
    }
    const auto& addresses = StoredAddresses(*sheet);
    // Addresses are sorted by row; binary-search the first candidate row.
    const auto begin = std::lower_bound(addresses.begin(), addresses.end(), area.firstRow,
                                        [](const CellAddress& address, UInt32 row)
                                        {
                                            return address.Row().Value() < row;
                                        });
    for (auto it = begin; it != addresses.end(); ++it)
    {
        const UInt32 row = it->Row().Value();
        if (row > area.lastRow)
        {
            break;
        }
        const UInt32 column = it->Column().Value();
        if (column < area.firstColumn || column > area.lastColumn)
        {
            continue;
        }
        callback(row, column, ReadCell(area.sheet, row, column));
    }

    // During recalculation the overlay may hold values for cells that are not
    // physically stored yet (array-formula results distributed over their
    // range). Visit those as well.
    if (m_overlay && !m_overlay->empty())
    {
        const std::string sheetKey = FormulaEvaluatorHelpers::ToLower(sheet->displayName);
        CellKey lowKey{sheetKey, area.firstRow, 0};
        for (auto it = m_overlay->lower_bound(lowKey); it != m_overlay->end(); ++it)
        {
            const CellKey& key = it->first;
            if (key.sheet != sheetKey || key.row > area.lastRow)
            {
                break;
            }
            if (key.column < area.firstColumn || key.column > area.lastColumn)
            {
                continue;
            }
            const auto address = CellAddress::TryCreate(key.row, key.column);
            if (address && sheet->worksheet->ContainsCell(*address))
            {
                continue; // already visited in the stored pass
            }
            callback(key.row, key.column, it->second);
        }
    }
    return true;
}

std::pair<UInt32, UInt32> FormulaEvaluationSession::SheetExtent(std::string_view sheetName)
{
    SheetCache* sheet = FindSheet(sheetName);
    if (!sheet)
    {
        return {0, 0};
    }
    UInt32 maxRow = 0;
    UInt32 maxColumn = 0;
    for (const auto& address : StoredAddresses(*sheet))
    {
        maxRow = std::max(maxRow, address.Row().Value());
        maxColumn = std::max(maxColumn, address.Column().Value());
    }
    return {maxRow, maxColumn};
}

FormulaValue FormulaEvaluationSession::EvaluateToValue(const FormulaExpression& root)
{
    return DereferenceToValue(Evaluate(root));
}

EvalValue FormulaEvaluationSession::Evaluate(const FormulaExpression& node)
{
    if (m_depth >= FormulaEvaluatorHelpers::MaxEvaluationDepth)
    {
        return EvalValue::Error(FormulaErrorCode::Value);
    }
    ++m_depth;
    EvalValue result;
    switch (node.kind)
    {
        case FormulaExpressionKind::NumberLiteral:
            result = EvalValue::Scalar(FormulaValue::Number(node.number));
            break;
        case FormulaExpressionKind::StringLiteral:
            result = EvalValue::Scalar(FormulaValue::Text(node.text));
            break;
        case FormulaExpressionKind::BooleanLiteral:
            result = EvalValue::Scalar(FormulaValue::Boolean(node.boolean));
            break;
        case FormulaExpressionKind::ErrorLiteral:
            result = EvalValue::Error(node.error);
            break;
        case FormulaExpressionKind::EmptyArgument:
            result = EvalValue::Scalar(FormulaValue());
            break;
        case FormulaExpressionKind::ArrayLiteral:
            result = EvaluateArrayLiteral(node);
            break;
        case FormulaExpressionKind::Reference:
            result = EvaluateReference(node);
            break;
        case FormulaExpressionKind::NameReference:
            result = EvaluateNameReference(node);
            break;
        case FormulaExpressionKind::Unary:
            result = EvaluateUnary(node);
            break;
        case FormulaExpressionKind::Binary:
            result = EvaluateBinary(node);
            break;
        case FormulaExpressionKind::Union:
        {
            std::vector<ResolvedReferenceArea> areas;
            bool failed = false;
            for (const auto& child : node.children)
            {
                EvalValue operand = Evaluate(*child);
                if (!operand.isReference)
                {
                    result = operand.value.IsError() ? operand : EvalValue::Error(FormulaErrorCode::Value);
                    failed = true;
                    break;
                }
                areas.insert(areas.end(), operand.areas.begin(), operand.areas.end());
            }
            if (!failed)
            {
                result = EvalValue::Reference(std::move(areas));
            }
            break;
        }
        case FormulaExpressionKind::FunctionCall:
            result = EvaluateFunction(node);
            break;
    }
    --m_depth;
    return result;
}

EvalValue FormulaEvaluationSession::EvaluateArrayLiteral(const FormulaExpression& node)
{
    std::vector<FormulaValue> elements;
    elements.reserve(node.children.size());
    for (const auto& child : node.children)
    {
        switch (child->kind)
        {
            case FormulaExpressionKind::NumberLiteral:
                elements.push_back(FormulaValue::Number(child->number));
                break;
            case FormulaExpressionKind::StringLiteral:
                elements.push_back(FormulaValue::Text(child->text));
                break;
            case FormulaExpressionKind::BooleanLiteral:
                elements.push_back(FormulaValue::Boolean(child->boolean));
                break;
            case FormulaExpressionKind::ErrorLiteral:
                elements.push_back(FormulaValue::Error(child->error));
                break;
            default:
                return EvalValue::Error(FormulaErrorCode::Value);
        }
    }
    return EvalValue::Scalar(
        FormulaValue::Array(node.arrayRowCount, node.arrayColumnCount, std::move(elements)));
}

void FormulaEvaluationSession::LoadNames()
{
    if (m_namesLoaded)
    {
        return;
    }
    m_namesLoaded = true;
    NamedRangeManager manager(m_document);
    for (NamedRange& entry : manager.List())
    {
        // Sheet-scoped entries whose sheet index is unresolvable cannot be
        // referenced and are skipped.
        if (entry.Scope == NamedRangeScope::Sheet && entry.ScopeSheet.empty())
        {
            continue;
        }
        std::pair<std::string, std::string> key{
            entry.Scope == NamedRangeScope::Sheet ? FormulaEvaluatorHelpers::ToLower(entry.ScopeSheet)
                                                  : std::string(),
            FormulaEvaluatorHelpers::ToLower(entry.Name)};
        NameDefinition definition;
        definition.formula = std::move(entry.Formula);
        m_names.emplace(std::move(key), std::move(definition));
    }
}

const FormulaExpression* FormulaEvaluationSession::ResolveName(std::string_view name,
                                                               std::string_view sheetQualifier,
                                                               bool qualified)
{
    LoadNames();
    const std::string nameLower = FormulaEvaluatorHelpers::ToLower(name);
    const std::string scopeLower =
        FormulaEvaluatorHelpers::ToLower(qualified ? sheetQualifier : std::string_view(m_currentSheet));

    auto it = m_names.find(std::pair<std::string, std::string>{scopeLower, nameLower});
    if (it == m_names.end())
    {
        it = m_names.find(std::pair<std::string, std::string>{std::string(), nameLower});
    }
    if (it == m_names.end())
    {
        return nullptr;
    }
    NameDefinition& definition = it->second;
    if (!definition.parsed)
    {
        definition.parsed = std::make_unique<FormulaParseResult>(FormulaParser::Parse(definition.formula));
    }
    return definition.parsed->Succeeded() ? definition.parsed->root.get() : nullptr;
}

EvalValue FormulaEvaluationSession::EvaluateNameReference(const FormulaExpression& node)
{
    const FormulaExpression* definition =
        ResolveName(node.text, node.area.sheet, node.area.hasSheet);
    if (!definition)
    {
        return EvalValue::Error(FormulaErrorCode::Name);
    }
    // A name whose definition leads back to itself would recurse forever;
    // cyclic definitions terminate as an error value instead.
    const std::string nameLower = FormulaEvaluatorHelpers::ToLower(node.text);
    if (std::find(m_nameStack.begin(), m_nameStack.end(), nameLower) != m_nameStack.end())
    {
        return EvalValue::Error(FormulaErrorCode::Value);
    }
    m_nameStack.push_back(nameLower);
    // Name definitions are self-contained: shared-formula offsets never apply
    // inside them. The anchor is kept for implicit intersection.
    const Int64 savedRowOffset = m_rowOffset;
    const Int64 savedColumnOffset = m_columnOffset;
    m_rowOffset = 0;
    m_columnOffset = 0;
    EvalValue result = Evaluate(*definition);
    m_rowOffset = savedRowOffset;
    m_columnOffset = savedColumnOffset;
    m_nameStack.pop_back();
    return result;
}

EvalValue FormulaEvaluationSession::EvaluateReference(const FormulaExpression& node)
{
    const FormulaReferenceArea& area = node.area;
    if (area.external)
    {
        return EvalValue::Error(FormulaErrorCode::Ref);
    }

    const auto applyOffset = [](const FormulaCoordinate& coordinate, Int64 offset,
                                UInt32 maximum) -> std::optional<UInt32>
    {
        Int64 value = coordinate.value;
        if (!coordinate.absolute)
        {
            value += offset;
        }
        if (value < 1 || value > maximum)
        {
            return std::nullopt;
        }
        return static_cast<UInt32>(value);
    };

    const auto firstRow = applyOffset(area.firstRow, m_rowOffset, MaxRowIndex);
    const auto lastRow = applyOffset(area.lastRow, m_rowOffset, MaxRowIndex);
    const auto firstColumn = applyOffset(area.firstColumn, m_columnOffset, MaxColumnIndex);
    const auto lastColumn = applyOffset(area.lastColumn, m_columnOffset, MaxColumnIndex);
    if (!firstRow || !lastRow || !firstColumn || !lastColumn)
    {
        return EvalValue::Error(FormulaErrorCode::Ref);
    }

    SheetCache* sheet = FindSheet(area.hasSheet ? std::string_view(area.sheet) : std::string_view());
    if (!sheet)
    {
        return EvalValue::Error(FormulaErrorCode::Ref);
    }

    ResolvedReferenceArea resolved;
    resolved.sheet = sheet->displayName;
    resolved.firstRow = std::min(*firstRow, *lastRow);
    resolved.lastRow = std::max(*firstRow, *lastRow);
    resolved.firstColumn = std::min(*firstColumn, *lastColumn);
    resolved.lastColumn = std::max(*firstColumn, *lastColumn);
    return EvalValue::Reference({resolved});
}

EvalValue FormulaEvaluationSession::EvaluateUnary(const FormulaExpression& node)
{
    EvalValue operand = Evaluate(*node.children.front());
    const FormulaValue value =
        m_arrayContext ? DereferenceToValue(operand) : DereferenceScalar(operand);

    const auto apply = [&](const FormulaValue& scalar) -> FormulaValue
    {
        if (scalar.IsError())
        {
            return scalar;
        }
        switch (node.unaryOperator)
        {
            case FormulaUnaryOperator::Plus:
                // Unary plus preserves the operand, including text, like Excel.
                return scalar;
            case FormulaUnaryOperator::Minus:
            {
                const FormulaValue number = FormulaCoercion::ToNumber(scalar);
                if (number.IsError())
                {
                    return number;
                }
                return FormulaValue::Number(-*number.NumberValue());
            }
            case FormulaUnaryOperator::Percent:
            {
                const FormulaValue number = FormulaCoercion::ToNumber(scalar);
                if (number.IsError())
                {
                    return number;
                }
                return FormulaValue::Number(*number.NumberValue() / 100.0);
            }
        }
        return FormulaValue::Error(FormulaErrorCode::Value);
    };

    if (value.Kind() == FormulaValueKind::Array)
    {
        std::vector<FormulaValue> elements;
        elements.reserve(value.RowCount() * value.ColumnCount());
        for (Size row = 0; row < value.RowCount(); ++row)
        {
            for (Size column = 0; column < value.ColumnCount(); ++column)
            {
                elements.push_back(apply(value.At(row, column)));
            }
        }
        return EvalValue::Scalar(FormulaValue::Array(value.RowCount(), value.ColumnCount(), std::move(elements)));
    }
    return EvalValue::Scalar(apply(value));
}

EvalValue FormulaEvaluationSession::EvaluateBinary(const FormulaExpression& node)
{
    if (node.binaryOperator == FormulaBinaryOperator::Intersect)
    {
        EvalValue left = Evaluate(*node.children[0]);
        EvalValue right = Evaluate(*node.children[1]);
        return EvaluateIntersection(left, right);
    }

    EvalValue leftOperand = Evaluate(*node.children[0]);
    EvalValue rightOperand = Evaluate(*node.children[1]);
    const FormulaValue left =
        m_arrayContext ? DereferenceToValue(leftOperand) : DereferenceScalar(leftOperand);
    const FormulaValue right =
        m_arrayContext ? DereferenceToValue(rightOperand) : DereferenceScalar(rightOperand);

    if (left.Kind() == FormulaValueKind::Array || right.Kind() == FormulaValueKind::Array)
    {
        return EvalValue::Scalar(ApplyBinaryBroadcast(node.binaryOperator, left, right));
    }
    return EvalValue::Scalar(ApplyBinaryScalar(node.binaryOperator, left, right));
}

EvalValue FormulaEvaluationSession::EvaluateIntersection(const EvalValue& left, const EvalValue& right)
{
    if (!left.isReference)
    {
        return left.value.IsError() ? left : EvalValue::Error(FormulaErrorCode::Value);
    }
    if (!right.isReference)
    {
        return right.value.IsError() ? right : EvalValue::Error(FormulaErrorCode::Value);
    }

    std::vector<ResolvedReferenceArea> result;
    for (const auto& a : left.areas)
    {
        for (const auto& b : right.areas)
        {
            if (FormulaEvaluatorHelpers::CompareTextIgnoreCase(a.sheet, b.sheet) != 0)
            {
                continue;
            }
            ResolvedReferenceArea intersection;
            intersection.sheet = a.sheet;
            intersection.firstRow = std::max(a.firstRow, b.firstRow);
            intersection.lastRow = std::min(a.lastRow, b.lastRow);
            intersection.firstColumn = std::max(a.firstColumn, b.firstColumn);
            intersection.lastColumn = std::min(a.lastColumn, b.lastColumn);
            if (intersection.firstRow <= intersection.lastRow &&
                intersection.firstColumn <= intersection.lastColumn)
            {
                result.push_back(std::move(intersection));
            }
        }
    }
    if (result.empty())
    {
        return EvalValue::Error(FormulaErrorCode::Null);
    }
    return EvalValue::Reference(std::move(result));
}

FormulaValue FormulaEvaluationSession::ApplyBinaryScalar(FormulaBinaryOperator op,
                                                         const FormulaValue& left,
                                                         const FormulaValue& right)
{
    if (left.IsError())
    {
        return left;
    }
    if (right.IsError())
    {
        return right;
    }

    switch (op)
    {
        case FormulaBinaryOperator::Add:
        case FormulaBinaryOperator::Subtract:
        case FormulaBinaryOperator::Multiply:
        case FormulaBinaryOperator::Divide:
        case FormulaBinaryOperator::Power:
        {
            const FormulaValue leftNumber = FormulaCoercion::ToNumber(left);
            if (leftNumber.IsError())
            {
                return leftNumber;
            }
            const FormulaValue rightNumber = FormulaCoercion::ToNumber(right);
            if (rightNumber.IsError())
            {
                return rightNumber;
            }
            const Real l = *leftNumber.NumberValue();
            const Real r = *rightNumber.NumberValue();
            Real result = 0.0;
            switch (op)
            {
                case FormulaBinaryOperator::Add:
                    result = l + r;
                    break;
                case FormulaBinaryOperator::Subtract:
                    result = l - r;
                    break;
                case FormulaBinaryOperator::Multiply:
                    result = l * r;
                    break;
                case FormulaBinaryOperator::Divide:
                    if (r == 0.0)
                    {
                        return FormulaValue::Error(FormulaErrorCode::Div0);
                    }
                    result = l / r;
                    break;
                case FormulaBinaryOperator::Power:
                    if (l == 0.0 && r == 0.0)
                    {
                        return FormulaValue::Error(FormulaErrorCode::Num);
                    }
                    if (l == 0.0 && r < 0.0)
                    {
                        return FormulaValue::Error(FormulaErrorCode::Div0);
                    }
                    if (l < 0.0 && r != std::floor(r))
                    {
                        return FormulaValue::Error(FormulaErrorCode::Num);
                    }
                    result = std::pow(l, r);
                    break;
                default:
                    break;
            }
            if (!std::isfinite(result))
            {
                return FormulaValue::Error(FormulaErrorCode::Num);
            }
            return FormulaValue::Number(result);
        }
        case FormulaBinaryOperator::Concatenate:
        {
            const FormulaValue leftText = FormulaCoercion::ToText(left);
            if (leftText.IsError())
            {
                return leftText;
            }
            const FormulaValue rightText = FormulaCoercion::ToText(right);
            if (rightText.IsError())
            {
                return rightText;
            }
            return FormulaValue::Text(leftText.TextValue() + rightText.TextValue());
        }
        case FormulaBinaryOperator::Equal:
        case FormulaBinaryOperator::NotEqual:
        case FormulaBinaryOperator::Less:
        case FormulaBinaryOperator::LessEqual:
        case FormulaBinaryOperator::Greater:
        case FormulaBinaryOperator::GreaterEqual:
        {
            const auto comparison = FormulaCoercion::Compare(left, right);
            if (!comparison)
            {
                return left.IsError() ? left : right;
            }
            const int c = *comparison;
            switch (op)
            {
                case FormulaBinaryOperator::Equal:
                    return FormulaValue::Boolean(c == 0);
                case FormulaBinaryOperator::NotEqual:
                    return FormulaValue::Boolean(c != 0);
                case FormulaBinaryOperator::Less:
                    return FormulaValue::Boolean(c < 0);
                case FormulaBinaryOperator::LessEqual:
                    return FormulaValue::Boolean(c <= 0);
                case FormulaBinaryOperator::Greater:
                    return FormulaValue::Boolean(c > 0);
                case FormulaBinaryOperator::GreaterEqual:
                    return FormulaValue::Boolean(c >= 0);
                default:
                    break;
            }
            break;
        }
        case FormulaBinaryOperator::Intersect:
            break;
    }
    return FormulaValue::Error(FormulaErrorCode::Value);
}

FormulaValue FormulaEvaluationSession::ApplyBinaryBroadcast(FormulaBinaryOperator op,
                                                            const FormulaValue& left,
                                                            const FormulaValue& right)
{
    const Size leftRows = left.RowCount();
    const Size leftColumns = left.ColumnCount();
    const Size rightRows = right.RowCount();
    const Size rightColumns = right.ColumnCount();
    const Size rows = std::max(leftRows, rightRows);
    const Size columns = std::max(leftColumns, rightColumns);

    const auto elementAt = [](const FormulaValue& value, Size row, Size column,
                              Size valueRows, Size valueColumns) -> const FormulaValue*
    {
        const Size effectiveRow = valueRows == 1 ? 0 : row;
        const Size effectiveColumn = valueColumns == 1 ? 0 : column;
        if (effectiveRow >= valueRows || effectiveColumn >= valueColumns)
        {
            return nullptr;
        }
        return &value.At(effectiveRow, effectiveColumn);
    };

    std::vector<FormulaValue> elements;
    elements.reserve(rows * columns);
    for (Size row = 0; row < rows; ++row)
    {
        for (Size column = 0; column < columns; ++column)
        {
            const FormulaValue* l = elementAt(left, row, column, leftRows, leftColumns);
            const FormulaValue* r = elementAt(right, row, column, rightRows, rightColumns);
            if (!l || !r)
            {
                elements.push_back(FormulaValue::Error(FormulaErrorCode::NA));
                continue;
            }
            elements.push_back(ApplyBinaryScalar(op, *l, *r));
        }
    }
    if (rows == 1 && columns == 1)
    {
        return elements.front();
    }
    return FormulaValue::Array(rows, columns, std::move(elements));
}

FormulaValue FormulaEvaluationSession::DereferenceScalar(const EvalValue& value)
{
    if (!value.isReference)
    {
        if (value.value.Kind() == FormulaValueKind::Array)
        {
            return value.value.At(0, 0);
        }
        return value.value;
    }

    if (value.areas.size() != 1)
    {
        return FormulaValue::Error(FormulaErrorCode::Value);
    }
    const ResolvedReferenceArea& area = value.areas.front();
    if (area.RowCount() == 1 && area.ColumnCount() == 1)
    {
        return ReadCell(area.sheet, area.firstRow, area.firstColumn);
    }

    // Implicit intersection against the anchor cell.
    if (m_anchor.IsValid())
    {
        const UInt32 anchorRow = m_anchor.Row().Value();
        const UInt32 anchorColumn = m_anchor.Column().Value();
        if (area.ColumnCount() == 1 && anchorRow >= area.firstRow && anchorRow <= area.lastRow)
        {
            return ReadCell(area.sheet, anchorRow, area.firstColumn);
        }
        if (area.RowCount() == 1 && anchorColumn >= area.firstColumn && anchorColumn <= area.lastColumn)
        {
            return ReadCell(area.sheet, area.firstRow, anchorColumn);
        }
    }
    return FormulaValue::Error(FormulaErrorCode::Value);
}

FormulaValue FormulaEvaluationSession::DereferenceToValue(const EvalValue& value)
{
    if (!value.isReference)
    {
        return value.value;
    }
    if (value.areas.size() != 1)
    {
        return FormulaValue::Error(FormulaErrorCode::Value);
    }
    const ResolvedReferenceArea& area = value.areas.front();
    if (area.RowCount() == 1 && area.ColumnCount() == 1)
    {
        return ReadCell(area.sheet, area.firstRow, area.firstColumn);
    }

    // Clip unbounded whole-row/column areas to the stored worksheet extent.
    ResolvedReferenceArea clipped = area;
    const auto [maxRow, maxColumn] = SheetExtent(area.sheet);
    if (clipped.lastRow == MaxRowIndex)
    {
        clipped.lastRow = std::max(clipped.firstRow, maxRow);
    }
    if (clipped.lastColumn == MaxColumnIndex)
    {
        clipped.lastColumn = std::max(clipped.firstColumn, maxColumn);
    }

    const Size rows = clipped.RowCount();
    const Size columns = clipped.ColumnCount();
    if (rows * columns > FormulaEvaluatorHelpers::MaxMaterializedElements)
    {
        return FormulaValue::Error(FormulaErrorCode::Num);
    }

    std::vector<FormulaValue> elements;
    elements.reserve(rows * columns);
    for (Size row = 0; row < rows; ++row)
    {
        for (Size column = 0; column < columns; ++column)
        {
            elements.push_back(ReadCell(clipped.sheet,
                                        clipped.firstRow + static_cast<UInt32>(row),
                                        clipped.firstColumn + static_cast<UInt32>(column)));
        }
    }
    return FormulaValue::Array(rows, columns, std::move(elements));
}

EvalValue FormulaEvaluationSession::EvaluateFunction(const FormulaExpression& node)
{
    const RegisteredFormulaFunction* function = m_registry.Find(node.text);
    if (!function)
    {
        return EvalValue::Error(FormulaErrorCode::Name);
    }

    const Size argumentCount = node.children.size();
    if (argumentCount < function->spec.MinimumArgumentCount ||
        argumentCount > function->spec.MaximumArgumentCount)
    {
        return EvalValue::Error(FormulaErrorCode::Value);
    }

    // Special forms evaluate their arguments lazily.
    if (function->specialForm != FormulaSpecialForm::None)
    {
        const auto evaluateArgument = [&](Size index) -> EvalValue
        {
            return Evaluate(*node.children[index]);
        };
        const auto scalarArgument = [&](Size index) -> FormulaValue
        {
            return DereferenceScalar(evaluateArgument(index));
        };
        const auto valueArgument = [&](Size index) -> FormulaValue
        {
            if (node.children[index]->kind == FormulaExpressionKind::EmptyArgument)
            {
                return FormulaValue::Number(0.0);
            }
            return DereferenceToValue(evaluateArgument(index));
        };

        switch (function->specialForm)
        {
            case FormulaSpecialForm::If:
            {
                const FormulaValue condition = FormulaCoercion::ToBoolean(scalarArgument(0));
                if (condition.IsError())
                {
                    return EvalValue::Scalar(condition);
                }
                if (*condition.BooleanValue())
                {
                    return EvalValue::Scalar(argumentCount >= 2 ? valueArgument(1)
                                                                : FormulaValue::Boolean(true));
                }
                return EvalValue::Scalar(argumentCount >= 3 ? valueArgument(2)
                                                            : FormulaValue::Boolean(false));
            }
            case FormulaSpecialForm::IfError:
            {
                const FormulaValue value = valueArgument(0);
                if (value.IsError())
                {
                    return EvalValue::Scalar(valueArgument(1));
                }
                return EvalValue::Scalar(value);
            }
            case FormulaSpecialForm::IfNa:
            {
                const FormulaValue value = valueArgument(0);
                if (value.IsError() && value.ErrorCode() == FormulaErrorCode::NA)
                {
                    return EvalValue::Scalar(valueArgument(1));
                }
                return EvalValue::Scalar(value);
            }
            case FormulaSpecialForm::Choose:
            {
                const FormulaValue index = FormulaCoercion::ToNumber(scalarArgument(0));
                if (index.IsError())
                {
                    return EvalValue::Scalar(index);
                }
                const Real indexValue = std::floor(*index.NumberValue());
                if (indexValue < 1.0 || indexValue >= static_cast<Real>(argumentCount))
                {
                    return EvalValue::Error(FormulaErrorCode::Value);
                }
                // CHOOSE may select a reference operand, so it stays a raw value.
                return evaluateArgument(static_cast<Size>(indexValue));
            }
            case FormulaSpecialForm::Ifs:
            {
                if (argumentCount % 2 != 0)
                {
                    return EvalValue::Error(FormulaErrorCode::Value);
                }
                for (Size i = 0; i + 1 < argumentCount; i += 2)
                {
                    const FormulaValue condition = FormulaCoercion::ToBoolean(scalarArgument(i));
                    if (condition.IsError())
                    {
                        return EvalValue::Scalar(condition);
                    }
                    if (*condition.BooleanValue())
                    {
                        return EvalValue::Scalar(valueArgument(i + 1));
                    }
                }
                return EvalValue::Error(FormulaErrorCode::NA);
            }
            case FormulaSpecialForm::Switch:
            {
                const FormulaValue subject = scalarArgument(0);
                if (subject.IsError())
                {
                    return EvalValue::Scalar(subject);
                }
                Size index = 1;
                while (index + 1 < argumentCount)
                {
                    const FormulaValue candidate = scalarArgument(index);
                    if (candidate.IsError())
                    {
                        return EvalValue::Scalar(candidate);
                    }
                    const auto comparison = FormulaCoercion::Compare(subject, candidate);
                    if (comparison && *comparison == 0)
                    {
                        return EvalValue::Scalar(valueArgument(index + 1));
                    }
                    index += 2;
                }
                if (index < argumentCount)
                {
                    return EvalValue::Scalar(valueArgument(index)); // default branch
                }
                return EvalValue::Error(FormulaErrorCode::NA);
            }
            default:
                break;
        }
        return EvalValue::Error(FormulaErrorCode::Value);
    }

    // Eager evaluation for regular functions.
    std::vector<EvalValue> arguments;
    arguments.reserve(argumentCount);
    for (const auto& child : node.children)
    {
        arguments.push_back(Evaluate(*child));
    }

    if (function->internalFunction)
    {
        return EvalValue::Scalar(function->internalFunction(*this, arguments));
    }
    if (function->customFunction)
    {
        std::vector<FormulaValue> values;
        values.reserve(arguments.size());
        for (const auto& argument : arguments)
        {
            values.push_back(DereferenceToValue(argument));
        }
        FormulaFunctionContext context;
        context.m_sheetName = m_currentSheet;
        context.m_anchor = m_anchor;
        context.m_session = this;
        return EvalValue::Scalar(function->customFunction(context, values));
    }
    return EvalValue::Error(FormulaErrorCode::Name);
}

} // namespace ExyokiOffice::Excel
