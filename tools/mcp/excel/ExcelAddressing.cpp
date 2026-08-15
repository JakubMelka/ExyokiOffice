// Copyright (c) 2026 Jakub Melka and Contributors
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "ExcelAddressing.hpp"

#include "ToolRegistry.hpp"

#include "AsciiText.hpp"

#include <algorithm>
#include <charconv>

namespace ExyokiOffice::Mcp
{

/**
 * @brief Strict numeric parsing for the band syntaxes.
 *
 * The band tokens come straight from an agent, so a token is accepted only
 * when it is consumed whole: "2x" is a mistake worth reporting, not the row 2
 * a lenient parser would silently read out of it.
 */
class ExcelAddressingParsing
{
public:
    /// Parses a 1-based row number; the whole token must be digits in range.
    [[nodiscard]] static bool ParseRowNumber(const std::string& text, UInt32& result)
    {
        const auto* begin = text.data();
        const auto* end = begin + text.size();

        UInt32 value = 0;
        const auto parsed = std::from_chars(begin, end, value);
        if (parsed.ec != std::errc() || parsed.ptr != end || value < 1 || value > Excel::MaxRowIndex)
        {
            return false;
        }

        result = value;
        return true;
    }
};

Excel::Worksheet::Ptr ExcelAddressing::FindSheet(Excel::ExcelDocumentEditor& editor, const nlohmann::json& arguments,
                                                 ToolOutcome& failure)
{
    return FindSheetMember(editor, arguments, "sheet", failure);
}

Excel::Worksheet::Ptr ExcelAddressing::FindSheetMember(Excel::ExcelDocumentEditor& editor,
                                                       const nlohmann::json& arguments, const std::string& member,
                                                       ToolOutcome& failure)
{
    const auto value = arguments.find(member);
    if (value == arguments.end() || value->is_null())
    {
        auto first = editor.FirstWorksheet();
        if (first == nullptr)
        {
            failure = MakeError(ErrorCode::SheetNotFound, "The workbook has no worksheet.", {},
                                "Call add_sheet to create one.");
        }

        return first;
    }

    auto sheet = FindSheetValue(editor, *value);
    if (sheet == nullptr)
    {
        const auto token = SheetToken(arguments, member);
        failure = MakeError(ErrorCode::SheetNotFound, "The workbook has no worksheet '" + token + "'.", token,
                            "Call list_sheets to see the available worksheets.");
    }

    return sheet;
}

Excel::Worksheet::Ptr ExcelAddressing::FindSheetValue(Excel::ExcelDocumentEditor& editor,
                                                      const nlohmann::json& value)
{
    if (value.is_number_integer())
    {
        // An integer is always a position. Only a string can name a worksheet,
        // which is what keeps a sheet literally called "2" reachable while the
        // integer 2 still means the second sheet.
        const auto index = value.get<Int64>();
        return index >= 1 ? FindSheetByIndex(editor, static_cast<Size>(index)) : nullptr;
    }

    if (value.is_string())
    {
        const auto token = value.get<std::string>();
        return token.empty() ? nullptr : FindSheetByToken(editor, token);
    }

    return nullptr;
}

Excel::Worksheet::Ptr ExcelAddressing::FindSheetByToken(Excel::ExcelDocumentEditor& editor, const std::string& token)
{
    // A name always wins over an index, so a worksheet literally called "2"
    // stays reachable in a workbook that also has a second sheet.
    auto named = FindSheetByName(editor, token);
    if (named != nullptr)
    {
        return named;
    }

    UInt32 index = 0;
    const auto* begin = token.data();
    const auto* end = begin + token.size();
    const auto parsed = std::from_chars(begin, end, index);
    if (parsed.ec != std::errc() || parsed.ptr != end)
    {
        return nullptr;
    }

    return FindSheetByIndex(editor, static_cast<Size>(index));
}

Excel::Worksheet::Ptr ExcelAddressing::FindSheetByIndex(Excel::ExcelDocumentEditor& editor, Size index)
{
    const auto sheets = editor.Worksheets();
    if (index < 1 || index > sheets.size())
    {
        return nullptr;
    }

    return sheets[index - 1];
}

Excel::Worksheet::Ptr ExcelAddressing::FindSheetByName(Excel::ExcelDocumentEditor& editor, const std::string& name)
{
    for (const auto& sheet : editor.Worksheets())
    {
        if (sheet != nullptr && AsciiText::EqualsIgnoreCase(sheet->Name(), name))
        {
            return sheet;
        }
    }

    return nullptr;
}

std::string ExcelAddressing::SheetToken(const nlohmann::json& arguments, const std::string& name)
{
    const auto member = arguments.find(name);
    if (member == arguments.end())
    {
        return {};
    }

    if (member->is_number_integer())
    {
        return std::to_string(member->get<Int64>());
    }

    if (member->is_string())
    {
        return member->get<std::string>();
    }

    return {};
}

std::optional<Excel::CellAddress> ExcelAddressing::ParseCell(const std::string& text, ToolOutcome& failure)
{
    auto address = Excel::CellAddress::ParseA1(text);
    if (!address.has_value() || !address->IsValid())
    {
        failure = MakeError(ErrorCode::RangeInvalid, "'" + text + "' is not a valid A1 cell address.", text,
                            "Use A1 notation such as \"B2\".");
        return std::nullopt;
    }

    return address;
}

std::optional<Excel::CellRange> ExcelAddressing::ParseRange(const std::string& text, ToolOutcome& failure)
{
    auto range = Excel::CellRange::ParseA1(text);
    if (range.has_value() && range->IsValid())
    {
        return range;
    }

    auto single = Excel::CellAddress::ParseA1(text);
    if (single.has_value() && single->IsValid())
    {
        return Excel::CellRange(*single, *single);
    }

    failure = MakeError(ErrorCode::RangeInvalid, "'" + text + "' is not a valid A1 range.", text,
                        "Use A1 notation such as \"A1:C10\" or a single cell such as \"B2\".");
    return std::nullopt;
}

std::optional<Excel::CellRange> ExcelAddressing::UsedRange(const Excel::Worksheet& sheet)
{
    const auto addresses = sheet.StoredCellAddresses();
    if (addresses.empty())
    {
        return std::nullopt;
    }

    UInt32 firstRow = Excel::MaxRowIndex;
    UInt32 lastRow = 1;
    UInt32 firstColumn = Excel::MaxColumnIndex;
    UInt32 lastColumn = 1;
    for (const auto& address : addresses)
    {
        firstRow = std::min(firstRow, address.Row().Value());
        lastRow = std::max(lastRow, address.Row().Value());
        firstColumn = std::min(firstColumn, address.Column().Value());
        lastColumn = std::max(lastColumn, address.Column().Value());
    }

    const auto first = Excel::CellAddress::TryCreate(firstRow, firstColumn);
    const auto last = Excel::CellAddress::TryCreate(lastRow, lastColumn);
    if (!first.has_value() || !last.has_value())
    {
        return std::nullopt;
    }

    return Excel::CellRange(*first, *last);
}

bool ExcelAddressing::ParseCellValue(const nlohmann::json& value, Excel::ExcelCellValue& result,
                                     ToolOutcome& failure)
{
    if (value.is_null())
    {
        // null means "leave the cell untouched" everywhere the tools accept a
        // cell value, so the writing tools skip such entries before they reach
        // this function. Turning null into a blank here would silently erase
        // the cell instead, which is what clear_range is for.
        failure = MakeError(ErrorCode::InputInvalid,
                            "A null cell value leaves the cell untouched and is never written.", {},
                            "Omit the cell, or call clear_range to erase its contents.");
        return false;
    }

    if (value.is_string())
    {
        result = Excel::ExcelCellValue::InlineString(value.get<std::string>());
        return true;
    }

    if (value.is_boolean())
    {
        result = Excel::ExcelCellValue::Boolean(value.get<bool>());
        return true;
    }

    if (value.is_number())
    {
        result = Excel::ExcelCellValue::Number(value.get<Real>());
        return true;
    }

    if (value.is_object())
    {
        const auto formula = value.find("formula");
        if (formula != value.end() && formula->is_string())
        {
            result = Excel::ExcelCellValue::Formula(formula->get<std::string>());
            return true;
        }

        const auto type = value.value("type", std::string());
        const auto text = value.value("value", std::string());
        if (type == "datetime")
        {
            result = Excel::ExcelCellValue::DateTimeText(text);
            return true;
        }

        if (type == "error")
        {
            result = Excel::ExcelCellValue::Error(text);
            return true;
        }

        if (type == "number")
        {
            result = Excel::ExcelCellValue::NumberText(text);
            return true;
        }

        if (type == "text" || type.empty())
        {
            result = Excel::ExcelCellValue::InlineString(text);
            return true;
        }

        failure = MakeError(ErrorCode::InputInvalid, "Unknown cell value type '" + type + "'.", type,
                            "Use text, number, datetime, error, or {\"formula\": \"...\"}.");
        return false;
    }

    failure = MakeError(ErrorCode::InputInvalid, "A cell value must be a string, number, boolean, object, or null.");
    return false;
}

std::string ExcelAddressing::CellValueToText(const Excel::ExcelCellValue& value,
                                             const Excel::SharedStringTableService& sharedStrings)
{
    if (value.Kind() == Excel::CellValueKind::SharedString)
    {
        const auto index = value.SharedStringIndex();
        if (index.has_value())
        {
            return sharedStrings.Lookup(*index).value_or(std::string());
        }

        return {};
    }

    if (value.Kind() == Excel::CellValueKind::Boolean)
    {
        return value.BooleanValue().value_or(false) ? "TRUE" : "FALSE";
    }

    if (value.Kind() == Excel::CellValueKind::Formula)
    {
        return value.FormulaValue().CachedText;
    }

    return value.Text();
}

nlohmann::json ExcelAddressing::CellValueToJson(const Excel::ExcelCellValue& value,
                                                const Excel::SharedStringTableService& sharedStrings)
{
    switch (value.Kind())
    {
        case Excel::CellValueKind::Blank:
            return nullptr;
        case Excel::CellValueKind::Boolean:
            return value.BooleanValue().value_or(false);
        case Excel::CellValueKind::Number:
        {
            // The stored text is authoritative; converting it back keeps the
            // exact value the workbook holds instead of a reformatted one.
            try
            {
                return std::stod(value.Text());
            }
            catch (const std::exception&)
            {
                return value.Text();
            }
        }
        default:
            break;
    }

    return CellValueToText(value, sharedStrings);
}

std::string ExcelAddressing::CellKindToken(Excel::CellValueKind kind)
{
    switch (kind)
    {
        case Excel::CellValueKind::Blank:
            return "blank";
        case Excel::CellValueKind::SharedString:
        case Excel::CellValueKind::InlineString:
            return "text";
        case Excel::CellValueKind::Number:
            return "number";
        case Excel::CellValueKind::Boolean:
            return "boolean";
        case Excel::CellValueKind::Error:
            return "error";
        case Excel::CellValueKind::DateTime:
            return "datetime";
        case Excel::CellValueKind::Formula:
            return "formula";
    }

    return "blank";
}

nlohmann::json ExcelAddressing::CellValueSchema()
{
    nlohmann::json schema = nlohmann::json::object();
    schema["description"] =
        "A cell value: a string, a number, a boolean, {\"formula\": \"SUM(A1:A9)\"}, or "
        "{\"value\": \"2026-08-02T00:00:00\", \"type\": \"datetime\"}. null skips the cell and leaves whatever it "
        "already holds; it never clears it, which is what clear_range does.";
    return schema;
}

bool ExcelAddressing::ParseColumnBand(const std::string& text, UInt32& first, UInt32& last)
{
    const auto separator = text.find(':');
    const auto firstText = separator == std::string::npos ? text : text.substr(0, separator);
    const auto lastText = separator == std::string::npos ? text : text.substr(separator + 1);

    const auto firstColumn = Excel::ColumnIndex::ParseName(firstText);
    const auto lastColumn = Excel::ColumnIndex::ParseName(lastText);
    if (!firstColumn.has_value() || !lastColumn.has_value())
    {
        return false;
    }

    first = std::min(firstColumn->Value(), lastColumn->Value());
    last = std::max(firstColumn->Value(), lastColumn->Value());
    return true;
}

bool ExcelAddressing::ParseRowBand(const std::string& text, UInt32& first, UInt32& last)
{
    const auto separator = text.find(':');
    const auto firstText = separator == std::string::npos ? text : text.substr(0, separator);
    const auto lastText = separator == std::string::npos ? text : text.substr(separator + 1);

    UInt32 firstRow = 0;
    UInt32 lastRow = 0;
    if (!ExcelAddressingParsing::ParseRowNumber(firstText, firstRow) ||
        !ExcelAddressingParsing::ParseRowNumber(lastText, lastRow))
    {
        return false;
    }

    first = std::min(firstRow, lastRow);
    last = std::max(firstRow, lastRow);
    return true;
}
} // namespace ExyokiOffice::Mcp
