// Copyright (c) 2026 Jakub Melka and Collaborators
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "ExyokiOffice/Tools/DocumentModelIO.hpp"

#include "ExyokiOffice/StandardTypes.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <map>
#include <string>
#include <utility>

namespace ExyokiOffice::Tools
{

namespace
{

void Error(std::vector<ToolDiagnostic>& diagnostics, std::string message, std::string context = {})
{
    diagnostics.push_back(ToolDiagnostic{ToolSeverity::Error, std::move(message), std::move(context)});
}

void Warn(std::vector<ToolDiagnostic>& diagnostics, std::string message, std::string context = {})
{
    diagnostics.push_back(ToolDiagnostic{ToolSeverity::Warning, std::move(message), std::move(context)});
}

[[nodiscard]] bool EqualsIgnoreCase(std::string_view left, std::string_view right)
{
    if (left.size() != right.size())
    {
        return false;
    }
    for (Size i = 0; i < left.size(); ++i)
    {
        if (std::tolower(static_cast<unsigned char>(left[i])) !=
            std::tolower(static_cast<unsigned char>(right[i])))
        {
            return false;
        }
    }
    return true;
}

/// Parses an A1 cell address ("B12") into 1-based row and column numbers.
[[nodiscard]] bool ParseA1Address(const std::string& address, Size& row, Size& column)
{
    Size i = 0;
    column = 0;
    while (i < address.size() && std::isalpha(static_cast<unsigned char>(address[i])))
    {
        column = column * 26 +
                 static_cast<Size>(std::toupper(static_cast<unsigned char>(address[i])) - 'A' + 1);
        ++i;
    }
    if (column == 0 || i == address.size())
    {
        return false;
    }
    row = 0;
    for (; i < address.size(); ++i)
    {
        if (!std::isdigit(static_cast<unsigned char>(address[i])))
        {
            return false;
        }
        row = row * 10 + static_cast<Size>(address[i] - '0');
    }
    return row > 0;
}

/// Formats a 1-based column number as A1 letters (1 -> "A", 27 -> "AA").
std::string ColumnLetters(Size column)
{
    std::string letters;
    while (column > 0)
    {
        const Size remainder = (column - 1) % 26;
        letters.insert(letters.begin(), static_cast<char>('A' + remainder));
        column = (column - 1) / 26;
    }
    return letters;
}

/// The text one cell contributes to CSV output.
std::string CsvCellText(const ExcelCellModel& cell)
{
    if (cell.Type == "formula")
    {
        return cell.CachedValue;
    }
    if (cell.Type == "bool")
    {
        return cell.Value == "true" || cell.Value == "1" ? "TRUE" : "FALSE";
    }
    return cell.Value;
}

std::string QuoteCsvField(const std::string& field, const std::string& separator)
{
    const bool needsQuoting = field.find('"') != std::string::npos ||
                              field.find('\n') != std::string::npos ||
                              field.find('\r') != std::string::npos ||
                              (!separator.empty() && field.find(separator) != std::string::npos);
    if (!needsQuoting)
    {
        return field;
    }
    std::string quoted = "\"";
    for (const char c : field)
    {
        if (c == '"')
        {
            quoted += '"';
        }
        quoted += c;
    }
    quoted += '"';
    return quoted;
}

/// True when the whole text is a plain decimal number a spreadsheet would
/// store as such. Leading zeros ("007") stay text so identifiers survive.
[[nodiscard]] bool LooksLikeNumber(const std::string& text)
{
    Size i = 0;
    if (i < text.size() && text[i] == '-')
    {
        ++i;
    }
    const Size integerStart = i;
    while (i < text.size() && std::isdigit(static_cast<unsigned char>(text[i])))
    {
        ++i;
    }
    const Size integerDigits = i - integerStart;
    if (integerDigits > 1 && text[integerStart] == '0')
    {
        return false;
    }
    bool hasFraction = false;
    if (i < text.size() && text[i] == '.')
    {
        ++i;
        const Size fractionStart = i;
        while (i < text.size() && std::isdigit(static_cast<unsigned char>(text[i])))
        {
            ++i;
        }
        hasFraction = i > fractionStart;
        if (!hasFraction)
        {
            return false;
        }
    }
    if (integerDigits == 0 && !hasFraction)
    {
        return false;
    }
    if (i < text.size() && (text[i] == 'e' || text[i] == 'E'))
    {
        ++i;
        if (i < text.size() && (text[i] == '+' || text[i] == '-'))
        {
            ++i;
        }
        const Size exponentStart = i;
        while (i < text.size() && std::isdigit(static_cast<unsigned char>(text[i])))
        {
            ++i;
        }
        if (i == exponentStart)
        {
            return false;
        }
    }
    return i == text.size();
}

/// Splits CSV text into rows of fields, honoring RFC 4180 quoting.
std::vector<std::vector<std::string>> ParseCsvRows(std::string_view csv, const std::string& separator)
{
    std::vector<std::vector<std::string>> rows;
    std::vector<std::string> row;
    std::string field;
    bool inQuotes = false;
    bool fieldWasQuoted = false;
    bool rowHasContent = false;

    // Strip a UTF-8 byte-order mark so the first field never carries it.
    if (csv.size() >= 3 && static_cast<unsigned char>(csv[0]) == 0xEF &&
        static_cast<unsigned char>(csv[1]) == 0xBB && static_cast<unsigned char>(csv[2]) == 0xBF)
    {
        csv.remove_prefix(3);
    }

    const auto finishField = [&]
    {
        row.push_back(std::move(field));
        field.clear();
        fieldWasQuoted = false;
    };
    const auto finishRow = [&]
    {
        finishField();
        rows.push_back(std::move(row));
        row.clear();
        rowHasContent = false;
    };

    Size i = 0;
    while (i < csv.size())
    {
        const char c = csv[i];
        if (inQuotes)
        {
            if (c == '"')
            {
                if (i + 1 < csv.size() && csv[i + 1] == '"')
                {
                    field += '"';
                    ++i;
                }
                else
                {
                    inQuotes = false;
                }
            }
            else
            {
                field += c;
            }
            ++i;
            continue;
        }

        if (c == '"' && field.empty() && !fieldWasQuoted)
        {
            inQuotes = true;
            fieldWasQuoted = true;
            rowHasContent = true;
            ++i;
            continue;
        }
        if (!separator.empty() && csv.compare(i, separator.size(), separator) == 0)
        {
            finishField();
            rowHasContent = true;
            i += separator.size();
            continue;
        }
        if (c == '\r' || c == '\n')
        {
            finishRow();
            if (c == '\r' && i + 1 < csv.size() && csv[i + 1] == '\n')
            {
                ++i;
            }
            ++i;
            continue;
        }
        field += c;
        rowHasContent = true;
        ++i;
    }

    // A trailing line without a newline still counts; a file ending with a
    // newline does not produce a phantom empty row.
    if (rowHasContent || !field.empty() || !row.empty())
    {
        finishRow();
    }
    return rows;
}

} // namespace

std::string SerializeModelCsv(const DocumentModel& model, const CsvOptions& options,
                              std::vector<ToolDiagnostic>& diagnostics)
{
    if (model.Family != DocumentFamily::Excel || !model.Excel)
    {
        Error(diagnostics, "CSV output requires an Excel workbook model");
        return {};
    }
    if (model.Excel->Sheets.empty())
    {
        Error(diagnostics, "Workbook model has no worksheets");
        return {};
    }

    const ExcelSheetModel* sheet = nullptr;
    if (options.SheetName.empty())
    {
        sheet = &model.Excel->Sheets.front();
        if (model.Excel->Sheets.size() > 1)
        {
            Warn(diagnostics,
                 "Workbook has " + std::to_string(model.Excel->Sheets.size()) +
                     " worksheets; exporting the first one",
                 sheet->Name);
        }
    }
    else
    {
        for (const auto& candidate : model.Excel->Sheets)
        {
            if (EqualsIgnoreCase(candidate.Name, options.SheetName))
            {
                sheet = &candidate;
                break;
            }
        }
        if (!sheet)
        {
            Error(diagnostics, "Worksheet not found", options.SheetName);
            return {};
        }
    }

    std::map<std::pair<Size, Size>, std::string> grid;
    Size maxRow = 0;
    Size maxColumn = 0;
    for (const auto& cell : sheet->Cells)
    {
        Size row = 0;
        Size column = 0;
        if (!ParseA1Address(cell.Address, row, column))
        {
            Warn(diagnostics, "Skipping cell with unparsable address", cell.Address);
            continue;
        }
        grid[{row, column}] = CsvCellText(cell);
        maxRow = std::max(maxRow, row);
        maxColumn = std::max(maxColumn, column);
    }

    std::string output;
    for (Size row = 1; row <= maxRow; ++row)
    {
        for (Size column = 1; column <= maxColumn; ++column)
        {
            if (column > 1)
            {
                output += options.Separator;
            }
            const auto found = grid.find({row, column});
            if (found != grid.end())
            {
                output += QuoteCsvField(found->second, options.Separator);
            }
        }
        output += "\r\n";
    }
    return output;
}

DocumentModel ParseModelCsv(std::string_view csv, const CsvOptions& options,
                            std::vector<ToolDiagnostic>& diagnostics)
{
    DocumentModel model;
    model.Family = DocumentFamily::Excel;
    model.Excel.emplace();

    ExcelSheetModel sheet;
    sheet.Name = options.SheetName.empty() ? "Sheet1" : options.SheetName;

    bool formulaTextReported = false;
    const auto rows = ParseCsvRows(csv, options.Separator.empty() ? "," : options.Separator);
    for (Size rowIndex = 0; rowIndex < rows.size(); ++rowIndex)
    {
        const auto& fields = rows[rowIndex];
        for (Size columnIndex = 0; columnIndex < fields.size(); ++columnIndex)
        {
            const auto& text = fields[columnIndex];
            if (text.empty())
            {
                continue;
            }

            ExcelCellModel cell;
            cell.Address = ColumnLetters(columnIndex + 1) + std::to_string(rowIndex + 1);
            if (EqualsIgnoreCase(text, "TRUE") || EqualsIgnoreCase(text, "FALSE"))
            {
                cell.Type = "bool";
                cell.Value = EqualsIgnoreCase(text, "TRUE") ? "true" : "false";
            }
            else if (LooksLikeNumber(text))
            {
                cell.Type = "number";
                cell.Value = text;
            }
            else
            {
                if (text.front() == '=' && !formulaTextReported)
                {
                    Warn(diagnostics,
                         "Values beginning with '=' are imported as text; CSV import never creates formulas",
                         cell.Address);
                    formulaTextReported = true;
                }
                cell.Type = "string";
                cell.Value = text;
            }
            sheet.Cells.push_back(std::move(cell));
        }
    }

    model.Excel->Sheets.push_back(std::move(sheet));
    return model;
}

} // namespace ExyokiOffice::Tools
