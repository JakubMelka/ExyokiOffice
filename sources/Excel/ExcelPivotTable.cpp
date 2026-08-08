// Copyright (c) 2026 Jakub Melka and Collaborators
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "ExyokiOffice/Excel/ExcelDocument.hpp"

#include "ExyokiOffice/DOM/DocumentFormat/OpenXml/Spreadsheet.hpp"
#include "ExyokiOffice/Packaging/GeneratedParts.hpp"
#include "Excel/ExcelSlicerInternal.hpp"
#include "ExyokiOffice/StandardTypes.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <exception>
#include <map>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace ExyokiOffice::Excel
{
namespace PivotDetail
{

namespace S = ExyokiOffice::DocumentFormat::OpenXml::Spreadsheet;

/** Marker written to `pivotTableDefinition/@tag` when the report cells are suppressed. */
constexpr std::string_view kNoCachedReportTag = "ExyokiOffice:noCachedReport";
/** Caption used for report filters and subtotals that cover every item. */
constexpr std::string_view kAllItemsCaption = "(All)";
/** Default caption of grand total rows and columns. */
constexpr std::string_view kDefaultGrandTotalCaption = "Grand Total";
/** Suffix appended to a group caption on subtotal lines. */
constexpr std::string_view kSubtotalSuffix = " Total";
/** Producer recorded in the pivot cache definition. */
constexpr std::string_view kRefreshedBy = "ExyokiOffice";
/** SpreadsheetML feature versions accepted by Excel 2007 and later. */
constexpr UInt8 kCreatedVersion = 3;
constexpr UInt8 kRefreshedVersion = 3;
constexpr UInt8 kMinRefreshableVersion = 3;
constexpr UInt8 kUpdatedVersion = 3;

// ---------------------------------------------------------------------------
// Small text helpers
// ---------------------------------------------------------------------------

std::string ToLowerAscii(std::string_view text)
{
    std::string result(text);
    std::transform(result.begin(), result.end(), result.begin(), [](unsigned char character)
                   { return static_cast<char>(std::tolower(character)); });
    return result;
}

bool EqualsIgnoreCase(std::string_view left, std::string_view right)
{
    return left.size() == right.size() && ToLowerAscii(left) == ToLowerAscii(right);
}

/** Orders text case-insensitively and breaks ties with a stable byte comparison. */
int CompareText(std::string_view left, std::string_view right)
{
    const auto lowerLeft = ToLowerAscii(left);
    const auto lowerRight = ToLowerAscii(right);
    if (lowerLeft != lowerRight)
    {
        return lowerLeft < lowerRight ? -1 : 1;
    }
    if (left == right)
    {
        return 0;
    }
    return left < right ? -1 : 1;
}

std::string FormatNumber(Real value)
{
    return ExcelCellValue::Number(value).Text();
}

PivotTableResult Failure(PivotTableError error, std::string message)
{
    return PivotTableResult{error, std::move(message)};
}

PivotTableResult Success()
{
    return PivotTableResult{};
}

// ---------------------------------------------------------------------------
// Source value model
// ---------------------------------------------------------------------------

/** Semantic category of a source cell after shared strings and caches are resolved. */
enum class ValueKind
{
    Blank,
    Number,
    Boolean,
    Text,
    Error
};

/**
 * One resolved source cell.
 *
 * `number` carries numeric and boolean payloads, `text` carries text and error
 * literals. `Caption()` produces the label written to the report cells and to
 * the pivot cache shared items.
 */
struct Value
{
    ValueKind kind = ValueKind::Blank;
    Real number = 0.0;
    std::string text;

    bool IsBlank() const noexcept { return kind == ValueKind::Blank; }

    std::string Caption() const
    {
        switch (kind)
        {
            case ValueKind::Blank:
                return {};
            case ValueKind::Number:
                return FormatNumber(number);
            case ValueKind::Boolean:
                return number != 0.0 ? "TRUE" : "FALSE";
            case ValueKind::Text:
            case ValueKind::Error:
                break;
        }
        return text;
    }
};

/** Report order rank: blanks first, then numbers, booleans, text, and errors. */
int OrderRank(ValueKind kind)
{
    switch (kind)
    {
        case ValueKind::Blank:
            return 0;
        case ValueKind::Number:
            return 1;
        case ValueKind::Boolean:
            return 2;
        case ValueKind::Text:
            return 3;
        case ValueKind::Error:
            return 4;
    }
    return 5;
}

int CompareValues(const Value& left, const Value& right)
{
    const auto leftRank = OrderRank(left.kind);
    const auto rightRank = OrderRank(right.kind);
    if (leftRank != rightRank)
    {
        return leftRank < rightRank ? -1 : 1;
    }
    switch (left.kind)
    {
        case ValueKind::Blank:
            return 0;
        case ValueKind::Number:
        case ValueKind::Boolean:
            if (left.number == right.number)
            {
                return 0;
            }
            return left.number < right.number ? -1 : 1;
        case ValueKind::Text:
        case ValueKind::Error:
            break;
    }
    return CompareText(left.text, right.text);
}

/** Distinct pivot items collapse case-insensitively, exactly like Excel item labels. */
bool ValuesAreSameItem(const Value& left, const Value& right)
{
    return CompareValues(left, right) == 0;
}

/** Reads one worksheet cell and resolves shared strings and cached formula results. */
Value ReadValue(const Worksheet& worksheet, const SharedStringTableService& strings, CellAddress address)
{
    const auto cell = worksheet.GetCellValue(address);
    if (!cell)
    {
        return {};
    }

    const auto fromText = [](CellValueKind kind, const std::string& text) -> Value
    {
        Value value;
        if (text.empty())
        {
            return value;
        }
        switch (kind)
        {
            case CellValueKind::Number:
            {
                // Same parser the rest of the library uses for OpenXML numbers:
                // the whole text has to be consumed, otherwise it stays text.
                const DoubleValue parsed = OpenXmlSimpleValueConvertor::GetDoubleValueFromString(text);
                if (parsed.IsDefined())
                {
                    value.kind = ValueKind::Number;
                    value.number = parsed.Value();
                    return value;
                }
                value.kind = ValueKind::Text;
                value.text = text;
                return value;
            }
            case CellValueKind::Error:
                value.kind = ValueKind::Error;
                value.text = text;
                return value;
            default:
                value.kind = ValueKind::Text;
                value.text = text;
                return value;
        }
    };

    switch (cell->Kind())
    {
        case CellValueKind::Blank:
            return {};
        case CellValueKind::Boolean:
        {
            Value value;
            value.kind = ValueKind::Boolean;
            value.number = cell->BooleanValue().value_or(false) ? 1.0 : 0.0;
            return value;
        }
        case CellValueKind::SharedString:
        {
            const auto index = cell->SharedStringIndex();
            const auto text = index ? strings.Lookup(*index) : std::nullopt;
            return fromText(CellValueKind::InlineString, text.value_or(std::string{}));
        }
        case CellValueKind::InlineString:
        case CellValueKind::DateTime:
            return fromText(CellValueKind::InlineString, cell->Text());
        case CellValueKind::Number:
            return fromText(CellValueKind::Number, cell->Text());
        case CellValueKind::Error:
            return fromText(CellValueKind::Error, cell->Text());
        case CellValueKind::Formula:
        {
            const auto& formula = cell->FormulaValue();
            switch (formula.CachedKind)
            {
                case FormulaCachedValueKind::Number:
                    return fromText(CellValueKind::Number, formula.CachedText);
                case FormulaCachedValueKind::Boolean:
                {
                    Value value;
                    value.kind = ValueKind::Boolean;
                    value.number = (formula.CachedText == "1" || EqualsIgnoreCase(formula.CachedText, "true")) ? 1.0 : 0.0;
                    return value;
                }
                case FormulaCachedValueKind::Error:
                    return fromText(CellValueKind::Error, formula.CachedText);
                case FormulaCachedValueKind::SharedString:
                {
                    UInt32 index = 0;
                    try
                    {
                        index = static_cast<UInt32>(std::stoul(formula.CachedText));
                    }
                    catch (const std::exception&)
                    {
                        return {};
                    }
                    return fromText(CellValueKind::InlineString, strings.Lookup(index).value_or(std::string{}));
                }
                case FormulaCachedValueKind::String:
                case FormulaCachedValueKind::DateTime:
                    return fromText(CellValueKind::InlineString, formula.CachedText);
                case FormulaCachedValueKind::None:
                    break;
            }
            return {};
        }
    }
    return {};
}

/** Header captions and body values of the pivot source rectangle. */
struct SourceData
{
    std::vector<std::string> headers;
    std::vector<std::vector<Value>> rows;
};

// ---------------------------------------------------------------------------
// Aggregation
// ---------------------------------------------------------------------------

/**
 * Mergeable accumulator for every supported aggregate.
 *
 * Keeping running sums instead of raw values lets subtotals and grand totals be
 * merged from the leaf cells they cover, which is both correct (unlike summing
 * pre-computed subtotals) and linear in the number of source rows.
 */
struct Accumulator
{
    UInt64 count = 0;
    UInt64 numericCount = 0;
    Real sum = 0.0;
    Real sumSquares = 0.0;
    Real product = 1.0;
    Real minimum = 0.0;
    Real maximum = 0.0;

    void Add(const Value& value)
    {
        if (value.IsBlank())
        {
            return;
        }
        ++count;
        if (value.kind != ValueKind::Number && value.kind != ValueKind::Boolean)
        {
            return;
        }
        const Real number = value.number;
        if (numericCount == 0)
        {
            minimum = number;
            maximum = number;
        }
        else
        {
            minimum = std::min(minimum, number);
            maximum = std::max(maximum, number);
        }
        ++numericCount;
        sum += number;
        sumSquares += number * number;
        product *= number;
    }

    void Merge(const Accumulator& other)
    {
        if (other.numericCount != 0)
        {
            if (numericCount == 0)
            {
                minimum = other.minimum;
                maximum = other.maximum;
            }
            else
            {
                minimum = std::min(minimum, other.minimum);
                maximum = std::max(maximum, other.maximum);
            }
            product *= other.product;
        }
        count += other.count;
        numericCount += other.numericCount;
        sum += other.sum;
        sumSquares += other.sumSquares;
    }

    std::optional<Real> Result(PivotAggregateFunction function) const
    {
        const auto sampleVariance = [this]() -> std::optional<Real>
        {
            if (numericCount < 2)
            {
                return std::nullopt;
            }
            const auto n = static_cast<Real>(numericCount);
            const Real variance = (sumSquares - (sum * sum) / n) / (n - 1.0);
            return std::max(variance, 0.0);
        };
        const auto populationVariance = [this]() -> std::optional<Real>
        {
            if (numericCount == 0)
            {
                return std::nullopt;
            }
            const auto n = static_cast<Real>(numericCount);
            const Real variance = (sumSquares - (sum * sum) / n) / n;
            return std::max(variance, 0.0);
        };

        switch (function)
        {
            case PivotAggregateFunction::Sum:
                return numericCount == 0 ? std::nullopt : std::optional(sum);
            case PivotAggregateFunction::Count:
                return count == 0 ? std::nullopt : std::optional(static_cast<Real>(count));
            case PivotAggregateFunction::CountNumbers:
                return numericCount == 0 ? std::nullopt : std::optional(static_cast<Real>(numericCount));
            case PivotAggregateFunction::Average:
                return numericCount == 0 ? std::nullopt : std::optional(sum / static_cast<Real>(numericCount));
            case PivotAggregateFunction::Maximum:
                return numericCount == 0 ? std::nullopt : std::optional(maximum);
            case PivotAggregateFunction::Minimum:
                return numericCount == 0 ? std::nullopt : std::optional(minimum);
            case PivotAggregateFunction::Product:
                return numericCount == 0 ? std::nullopt : std::optional(product);
            case PivotAggregateFunction::Variance:
                return sampleVariance();
            case PivotAggregateFunction::VarianceP:
                return populationVariance();
            case PivotAggregateFunction::StandardDeviation:
            {
                const auto variance = sampleVariance();
                return variance ? std::optional(std::sqrt(*variance)) : std::nullopt;
            }
            case PivotAggregateFunction::StandardDeviationP:
            {
                const auto variance = populationVariance();
                return variance ? std::optional(std::sqrt(*variance)) : std::nullopt;
            }
        }
        return std::nullopt;
    }
};

// ---------------------------------------------------------------------------
// Built report model
// ---------------------------------------------------------------------------

/** One pivot cache field together with its placement and enumerated items. */
struct BuiltField
{
    std::string name;
    PivotAxis axis = PivotAxis::None;
    bool showSubtotal = false;
    bool showAll = false;
    bool insertBlankRow = false;
    std::optional<UInt32> selectedItem;
    bool isDataField = false;

    /** True when the field's distinct values are written as `sharedItems` children. */
    bool enumerated = false;
    std::vector<Value> items;
    /** Per source row: index into `items`, valid only for enumerated fields. */
    std::vector<UInt32> recordItems;

    bool containsBlank = false;
    bool containsNumber = false;
    bool containsString = false;
    bool containsInteger = true;
    bool containsError = false;
    bool containsBoolean = false;
    Real minimum = 0.0;
    Real maximum = 0.0;
};

/** A data field resolved against the pivot cache fields. */
struct BuiltDataField
{
    Size field = 0;
    std::string name;
    PivotAggregateFunction function = PivotAggregateFunction::Sum;
    PivotShowDataAs showDataAs = PivotShowDataAs::Normal;
    std::optional<Size> baseField;
    std::optional<UInt32> baseItem;
    std::optional<UInt32> numberFormatId;
};

enum class LineKind
{
    Data,
    Subtotal,
    Grand
};

/** One row or column line of the report, identified by its item-index prefix. */
struct Line
{
    LineKind kind = LineKind::Data;
    std::vector<UInt32> items;
};

/** Everything needed to emit the pivot cache, the pivot table, and the report cells. */
struct BuiltReport
{
    std::vector<BuiltField> fields;
    std::vector<Size> rowFields;
    std::vector<Size> columnFields;
    std::vector<Size> pageFields;
    std::vector<BuiltDataField> dataFields;

    std::vector<Line> rowLines;
    std::vector<Line> columnLines;
    std::map<std::tuple<Size, Size, Size>, Accumulator> cells;

    Size recordCount = 0;
    std::string sourceSheet;
    CellRange sourceRange;
    CellAddress targetCell;

    UInt32 labelColumnCount = 1;
    UInt32 headerRowCount = 1;
    UInt32 pageRowCount = 0;

    UInt32 ValueColumnCount() const
    {
        return static_cast<UInt32>(columnLines.size() * dataFields.size());
    }
    UInt32 TotalColumnCount() const { return labelColumnCount + ValueColumnCount(); }
    UInt32 TotalRowCount() const
    {
        return headerRowCount + static_cast<UInt32>(rowLines.size());
    }
};

// ---------------------------------------------------------------------------
// Reading the source range
// ---------------------------------------------------------------------------

std::optional<SourceData> ReadSource(const ExcelDocument::Ptr& document,
                                     std::string_view sheetName,
                                     CellRange range,
                                     PivotTableResult& result)
{
    ExcelDocumentEditor editor(document);
    auto sheet = editor.GetWorksheet(sheetName);
    if (!sheet)
    {
        result = Failure(PivotTableError::InvalidSource,
                         "The pivot table source worksheet '" + std::string(sheetName) + "' does not exist.");
        return std::nullopt;
    }
    if (!range.IsValid() || range.RowCount() < 2)
    {
        result = Failure(PivotTableError::InvalidSource,
                         "The pivot table source range must be valid and contain a header row plus at least one data row.");
        return std::nullopt;
    }

    const SharedStringTableService strings(document);
    SourceData data;
    const auto firstRow = range.First().Row().Value();
    const auto firstColumn = range.First().Column().Value();

    for (UInt32 column = 0; column < range.ColumnCount(); ++column)
    {
        const auto address = CellAddress(RowIndex(firstRow), ColumnIndex(firstColumn + column));
        auto header = ReadValue(*sheet, strings, address).Caption();
        if (header.empty())
        {
            result = Failure(PivotTableError::InvalidSourceHeader,
                             "The pivot table source header cell " + address.ToA1() + " is blank.");
            return std::nullopt;
        }
        for (const auto& existing : data.headers)
        {
            if (EqualsIgnoreCase(existing, header))
            {
                result = Failure(PivotTableError::InvalidSourceHeader,
                                 "The pivot table source header '" + header + "' is used more than once.");
                return std::nullopt;
            }
        }
        data.headers.push_back(std::move(header));
    }

    data.rows.reserve(range.RowCount() - 1);
    for (UInt32 row = 1; row < range.RowCount(); ++row)
    {
        std::vector<Value> values;
        values.reserve(range.ColumnCount());
        for (UInt32 column = 0; column < range.ColumnCount(); ++column)
        {
            values.push_back(
                ReadValue(*sheet, strings, CellAddress(RowIndex(firstRow + row), ColumnIndex(firstColumn + column))));
        }
        data.rows.push_back(std::move(values));
    }
    return data;
}

// ---------------------------------------------------------------------------
// Building the report
// ---------------------------------------------------------------------------

std::string DefaultDataFieldName(PivotAggregateFunction function, std::string_view sourceField)
{
    std::string_view prefix;
    switch (function)
    {
        case PivotAggregateFunction::Sum:
            prefix = "Sum of ";
            break;
        // Excel labels both counting functions the same way.
        case PivotAggregateFunction::Count:
        case PivotAggregateFunction::CountNumbers:
            prefix = "Count of ";
            break;
        case PivotAggregateFunction::Average:
            prefix = "Average of ";
            break;
        case PivotAggregateFunction::Maximum:
            prefix = "Max of ";
            break;
        case PivotAggregateFunction::Minimum:
            prefix = "Min of ";
            break;
        case PivotAggregateFunction::Product:
            prefix = "Product of ";
            break;
        case PivotAggregateFunction::StandardDeviation:
            prefix = "StdDev of ";
            break;
        case PivotAggregateFunction::StandardDeviationP:
            prefix = "StdDevp of ";
            break;
        case PivotAggregateFunction::Variance:
            prefix = "Var of ";
            break;
        case PivotAggregateFunction::VarianceP:
            prefix = "Varp of ";
            break;
    }
    return std::string(prefix) + std::string(sourceField);
}

std::optional<Size> FindField(const std::vector<std::string>& headers, std::string_view name)
{
    for (Size index = 0; index < headers.size(); ++index)
    {
        if (EqualsIgnoreCase(headers[index], name))
        {
            return index;
        }
    }
    return std::nullopt;
}

/** Collects the distinct values of one source column and sorts them into report order. */
void EnumerateItems(BuiltField& field, const SourceData& source, Size column)
{
    std::vector<Value> distinct;
    for (const auto& row : source.rows)
    {
        const auto& value = row[column];
        const auto existing = std::find_if(distinct.begin(), distinct.end(), [&](const Value& candidate)
                                           { return ValuesAreSameItem(candidate, value); });
        if (existing == distinct.end())
        {
            distinct.push_back(value);
        }
    }
    std::sort(distinct.begin(), distinct.end(), [](const Value& left, const Value& right)
              { return CompareValues(left, right) < 0; });
    field.items = std::move(distinct);
    field.enumerated = true;

    field.recordItems.reserve(source.rows.size());
    for (const auto& row : source.rows)
    {
        const auto position = std::find_if(field.items.begin(), field.items.end(), [&](const Value& candidate)
                                           { return ValuesAreSameItem(candidate, row[column]); });
        field.recordItems.push_back(static_cast<UInt32>(std::distance(field.items.begin(), position)));
    }
}

/** Records the type summary that non-enumerated fields store on `sharedItems`. */
void SummarizeTypes(BuiltField& field, const SourceData& source, Size column)
{
    bool first = true;
    for (const auto& row : source.rows)
    {
        const auto& value = row[column];
        switch (value.kind)
        {
            case ValueKind::Blank:
                field.containsBlank = true;
                break;
            case ValueKind::Number:
                field.containsNumber = true;
                if (value.number != std::floor(value.number) || !std::isfinite(value.number))
                {
                    field.containsInteger = false;
                }
                if (first)
                {
                    field.minimum = value.number;
                    field.maximum = value.number;
                    first = false;
                }
                else
                {
                    field.minimum = std::min(field.minimum, value.number);
                    field.maximum = std::max(field.maximum, value.number);
                }
                break;
            case ValueKind::Boolean:
                field.containsBoolean = true;
                break;
            case ValueKind::Text:
                field.containsString = true;
                break;
            case ValueKind::Error:
                field.containsError = true;
                break;
        }
    }
    if (!field.containsNumber)
    {
        field.containsInteger = false;
    }
}

/** True when the source row passes every report filter selection. */
bool RowPassesPageFilters(const BuiltReport& report, Size row)
{
    for (const auto fieldIndex : report.pageFields)
    {
        const auto& field = report.fields[fieldIndex];
        if (!field.selectedItem)
        {
            continue;
        }
        if (row >= field.recordItems.size() || field.recordItems[row] != *field.selectedItem)
        {
            return false;
        }
    }
    return true;
}

/** Collects the distinct axis tuples that actually occur in the filtered source rows. */
std::vector<std::vector<UInt32>> CollectTuples(const BuiltReport& report,
                                               const std::vector<Size>& axisFields,
                                               const std::vector<Size>& includedRows)
{
    std::vector<std::vector<UInt32>> tuples;
    if (axisFields.empty())
    {
        return tuples;
    }
    for (const auto row : includedRows)
    {
        std::vector<UInt32> tuple;
        tuple.reserve(axisFields.size());
        for (const auto fieldIndex : axisFields)
        {
            tuple.push_back(report.fields[fieldIndex].recordItems[row]);
        }
        tuples.push_back(std::move(tuple));
    }
    std::sort(tuples.begin(), tuples.end());
    tuples.erase(std::unique(tuples.begin(), tuples.end()), tuples.end());
    return tuples;
}

/** Expands sorted tuples into report lines, inserting subtotal and grand total lines. */
std::vector<Line> BuildLines(const BuiltReport& report,
                             const std::vector<Size>& axisFields,
                             const std::vector<std::vector<UInt32>>& tuples,
                             bool grandTotals)
{
    std::vector<Line> lines;
    if (axisFields.empty())
    {
        lines.push_back(Line{LineKind::Data, {}});
        return lines;
    }

    const auto depth = axisFields.size();
    for (Size index = 0; index < tuples.size(); ++index)
    {
        lines.push_back(Line{LineKind::Data, tuples[index]});

        // Emit subtotals for every enclosing level whose group ends here.
        const bool last = index + 1 == tuples.size();
        for (Size level = depth - 1; level-- > 0;)
        {
            if (!report.fields[axisFields[level]].showSubtotal)
            {
                continue;
            }
            const bool groupEnds =
                last || !std::equal(tuples[index].begin(), tuples[index].begin() + static_cast<PtrDiff>(level + 1),
                                    tuples[index + 1].begin());
            if (groupEnds)
            {
                lines.push_back(Line{LineKind::Subtotal,
                                     std::vector<UInt32>(tuples[index].begin(),
                                                         tuples[index].begin() +
                                                             static_cast<PtrDiff>(level + 1))});
            }
        }
    }
    if (grandTotals && !tuples.empty())
    {
        lines.push_back(Line{LineKind::Grand, {}});
    }
    return lines;
}

/** Returns the indices of the lines a fully specified tuple contributes to. */
std::vector<Size> ContributingLines(const std::vector<Line>& lines,
                                    const std::vector<UInt32>& tuple)
{
    std::vector<Size> result;
    for (Size index = 0; index < lines.size(); ++index)
    {
        const auto& line = lines[index];
        if (line.kind == LineKind::Grand)
        {
            result.push_back(index);
            continue;
        }
        if (line.items.size() > tuple.size())
        {
            continue;
        }
        if (std::equal(line.items.begin(), line.items.end(), tuple.begin()))
        {
            result.push_back(index);
        }
    }
    return result;
}

PivotTableResult BuildReportModel(const ExcelPivotTableDefinition& definition,
                                  const SourceData& source,
                                  BuiltReport& report)
{
    report.fields.clear();
    report.fields.resize(source.headers.size());
    for (Size index = 0; index < source.headers.size(); ++index)
    {
        report.fields[index].name = source.headers[index];
    }

    report.rowFields.clear();
    report.columnFields.clear();
    report.pageFields.clear();

    // Axis order follows the caller's placement order, not the source column order.
    for (const auto& requested : definition.Fields)
    {
        const auto index = FindField(source.headers, requested.Name);
        if (!index)
        {
            return Failure(PivotTableError::UnknownField,
                           "The pivot field '" + requested.Name + "' is not a column of the source range.");
        }
        auto& field = report.fields[*index];
        if (field.axis != PivotAxis::None)
        {
            return Failure(PivotTableError::InvalidFieldConfiguration,
                           "The pivot field '" + requested.Name + "' is placed more than once.");
        }
        field.axis = requested.Axis;
        field.showSubtotal = requested.ShowSubtotal;
        field.showAll = requested.ShowAll;
        field.insertBlankRow = requested.InsertBlankRow;
        field.selectedItem = requested.Axis == PivotAxis::Page ? requested.SelectedItem : std::nullopt;
        switch (requested.Axis)
        {
            case PivotAxis::Row:
                report.rowFields.push_back(*index);
                break;
            case PivotAxis::Column:
                report.columnFields.push_back(*index);
                break;
            case PivotAxis::Page:
                report.pageFields.push_back(*index);
                break;
            case PivotAxis::None:
                break;
        }
    }

    if (definition.DataFields.empty())
    {
        return Failure(PivotTableError::InvalidFieldConfiguration,
                       "A pivot table requires at least one data field.");
    }
    for (const auto& requested : definition.DataFields)
    {
        const auto index = FindField(source.headers, requested.SourceField);
        if (!index)
        {
            return Failure(PivotTableError::UnknownField,
                           "The pivot data field '" + requested.SourceField + "' is not a column of the source range.");
        }
        BuiltDataField dataField;
        dataField.field = *index;
        dataField.name = requested.Name.empty() ? DefaultDataFieldName(requested.Function, source.headers[*index])
                                                : requested.Name;
        dataField.function = requested.Function;
        dataField.showDataAs = requested.ShowDataAs;
        dataField.numberFormatId = requested.NumberFormatId;
        if (requested.ShowDataAs != PivotShowDataAs::Normal && requested.BaseField)
        {
            const auto baseIndex = FindField(source.headers, *requested.BaseField);
            if (!baseIndex)
            {
                return Failure(PivotTableError::UnknownField,
                               "The pivot data field base column '" + *requested.BaseField +
                                   "' is not a column of the source range.");
            }
            dataField.baseField = *baseIndex;
            dataField.baseItem = requested.BaseItem;
        }
        for (const auto& existing : report.dataFields)
        {
            if (EqualsIgnoreCase(existing.name, dataField.name))
            {
                return Failure(PivotTableError::InvalidFieldConfiguration,
                               "The pivot data field name '" + dataField.name + "' is used more than once.");
            }
        }
        report.fields[*index].isDataField = true;
        report.dataFields.push_back(std::move(dataField));
    }

    // Axis fields need enumerated shared items; everything else only needs a type summary.
    for (Size index = 0; index < report.fields.size(); ++index)
    {
        auto& field = report.fields[index];
        if (field.axis != PivotAxis::None)
        {
            EnumerateItems(field, source, index);
        }
        else
        {
            SummarizeTypes(field, source, index);
        }
    }

    for (const auto fieldIndex : report.pageFields)
    {
        auto& field = report.fields[fieldIndex];
        if (field.selectedItem && *field.selectedItem >= field.items.size())
        {
            field.selectedItem.reset();
        }
    }

    std::vector<Size> includedRows;
    includedRows.reserve(source.rows.size());
    for (Size row = 0; row < source.rows.size(); ++row)
    {
        if (RowPassesPageFilters(report, row))
        {
            includedRows.push_back(row);
        }
    }

    const auto rowTuples = CollectTuples(report, report.rowFields, includedRows);
    const auto columnTuples = CollectTuples(report, report.columnFields, includedRows);
    report.rowLines = BuildLines(report, report.rowFields, rowTuples, definition.RowGrandTotals);
    report.columnLines = BuildLines(report, report.columnFields, columnTuples, definition.ColumnGrandTotals);

    // Accumulate every filtered source row into each line pair it belongs to.
    std::map<std::vector<UInt32>, std::vector<Size>> rowLineCache;
    std::map<std::vector<UInt32>, std::vector<Size>> columnLineCache;
    for (const auto row : includedRows)
    {
        std::vector<UInt32> rowTuple;
        for (const auto fieldIndex : report.rowFields)
        {
            rowTuple.push_back(report.fields[fieldIndex].recordItems[row]);
        }
        std::vector<UInt32> columnTuple;
        for (const auto fieldIndex : report.columnFields)
        {
            columnTuple.push_back(report.fields[fieldIndex].recordItems[row]);
        }

        auto rowLines = rowLineCache.find(rowTuple);
        if (rowLines == rowLineCache.end())
        {
            rowLines = rowLineCache.emplace(rowTuple, ContributingLines(report.rowLines, rowTuple)).first;
        }
        auto columnLines = columnLineCache.find(columnTuple);
        if (columnLines == columnLineCache.end())
        {
            columnLines = columnLineCache.emplace(columnTuple, ContributingLines(report.columnLines, columnTuple)).first;
        }

        for (Size dataIndex = 0; dataIndex < report.dataFields.size(); ++dataIndex)
        {
            const auto& value = source.rows[row][report.dataFields[dataIndex].field];
            if (value.IsBlank())
            {
                continue;
            }
            for (const auto rowLine : rowLines->second)
            {
                for (const auto columnLine : columnLines->second)
                {
                    report.cells[std::make_tuple(rowLine, columnLine, dataIndex)].Add(value);
                }
            }
        }
    }

    report.recordCount = source.rows.size();
    report.labelColumnCount = std::max<UInt32>(static_cast<UInt32>(report.rowFields.size()), 1);
    report.headerRowCount = static_cast<UInt32>(report.columnFields.size()) + 1;
    report.pageRowCount = report.pageFields.empty() ? 0 : static_cast<UInt32>(report.pageFields.size()) + 1;
    return Success();
}

// ---------------------------------------------------------------------------
// Geometry
// ---------------------------------------------------------------------------

/** The rectangle covered by `location/@ref`: header rows plus report body. */
std::optional<CellRange> ReportRectangle(const BuiltReport& report)
{
    const auto firstRow = static_cast<UInt64>(report.targetCell.Row().Value()) + report.pageRowCount;
    const auto lastRow = firstRow + report.TotalRowCount() - 1;
    const auto firstColumn = static_cast<UInt64>(report.targetCell.Column().Value());
    const auto lastColumn = firstColumn + report.TotalColumnCount() - 1;
    if (lastRow > MaxRowIndex || lastColumn > MaxColumnIndex)
    {
        return std::nullopt;
    }
    return CellRange(CellAddress(RowIndex(static_cast<UInt32>(firstRow)),
                                 ColumnIndex(static_cast<UInt32>(firstColumn))),
                     CellAddress(RowIndex(static_cast<UInt32>(lastRow)),
                                 ColumnIndex(static_cast<UInt32>(lastColumn))));
}

/**
 * The complete written rectangle, including the report filter lines above the
 * report. Report filters occupy two columns (label and selection), so a report
 * that is only one column wide still reserves the second column.
 */
std::optional<CellRange> WrittenRectangle(const BuiltReport& report)
{
    const auto rectangle = ReportRectangle(report);
    if (!rectangle)
    {
        return std::nullopt;
    }
    auto lastColumn = rectangle->Last().Column().Value();
    if (report.pageRowCount != 0)
    {
        lastColumn = std::max<UInt32>(lastColumn, report.targetCell.Column().Value() + 1);
    }
    if (lastColumn > MaxColumnIndex)
    {
        return std::nullopt;
    }
    return CellRange(CellAddress(report.targetCell.Row(), report.targetCell.Column()),
                     CellAddress(rectangle->Last().Row(), ColumnIndex(lastColumn)));
}

bool RangesIntersect(const CellRange& left, const CellRange& right)
{
    return left.First().Row().Value() <= right.Last().Row().Value() &&
           right.First().Row().Value() <= left.Last().Row().Value() &&
           left.First().Column().Value() <= right.Last().Column().Value() &&
           right.First().Column().Value() <= left.Last().Column().Value();
}

// ---------------------------------------------------------------------------
// Rendering the report into worksheet cells
// ---------------------------------------------------------------------------

std::string GrandTotalCaption(const ExcelPivotTableDefinition& definition)
{
    return definition.GrandTotalCaption.empty() ? std::string(kDefaultGrandTotalCaption)
                                                : definition.GrandTotalCaption;
}

/** True when the level-@p level caption of @p index starts a new group. */
bool StartsGroup(const std::vector<Line>& lines, Size index, Size level)
{
    const auto& line = lines[index];
    if (line.kind != LineKind::Data || line.items.size() <= level)
    {
        return false;
    }
    if (index == 0)
    {
        return true;
    }
    const auto& previous = lines[index - 1];
    if (previous.kind != LineKind::Data || previous.items.size() <= level)
    {
        return true;
    }
    return !std::equal(line.items.begin(), line.items.begin() + static_cast<PtrDiff>(level + 1),
                       previous.items.begin());
}

bool WriteText(Worksheet& worksheet, UInt32 row, UInt32 column, std::string_view text)
{
    if (text.empty())
    {
        return true;
    }
    return worksheet.SetCellText(row, column, text);
}

bool RenderReport(Worksheet& worksheet, const BuiltReport& report, const ExcelPivotTableDefinition& definition)
{
    const auto rectangle = ReportRectangle(report);
    if (!rectangle)
    {
        return false;
    }
    const auto firstRow = rectangle->First().Row().Value();
    const auto firstColumn = rectangle->First().Column().Value();
    const auto columnFieldCount = static_cast<UInt32>(report.columnFields.size());
    const auto dataFieldCount = static_cast<UInt32>(report.dataFields.size());
    const auto totalCaption = GrandTotalCaption(definition);

    // Report filter lines above the report.
    for (UInt32 index = 0; index < report.pageFields.size(); ++index)
    {
        const auto& field = report.fields[report.pageFields[index]];
        const auto row = report.targetCell.Row().Value() + index;
        if (!WriteText(worksheet, row, report.targetCell.Column().Value(), field.name))
        {
            return false;
        }
        const auto caption = field.selectedItem && *field.selectedItem < field.items.size()
                                 ? field.items[*field.selectedItem].Caption()
                                 : std::string(kAllItemsCaption);
        if (!WriteText(worksheet, row, report.targetCell.Column().Value() + 1, caption))
        {
            return false;
        }
    }

    // Header rows that name the column fields and their items.
    for (UInt32 level = 0; level < columnFieldCount; ++level)
    {
        const auto row = firstRow + level;
        if (!WriteText(worksheet, row, firstColumn, report.fields[report.columnFields[level]].name))
        {
            return false;
        }
        for (Size line = 0; line < report.columnLines.size(); ++line)
        {
            if (!StartsGroup(report.columnLines, line, level))
            {
                continue;
            }
            const auto column = firstColumn + report.labelColumnCount +
                                static_cast<UInt32>(line) * dataFieldCount;
            if (!WriteText(worksheet, row, column, report.fields[report.columnFields[level]].items[report.columnLines[line].items[level]].Caption()))
            {
                return false;
            }
        }
    }

    // Last header row: row field names and data field names.
    const auto headerRow = firstRow + columnFieldCount;
    if (report.rowFields.empty())
    {
        if (!WriteText(worksheet, headerRow, firstColumn, definition.RowHeaderCaption))
        {
            return false;
        }
    }
    else
    {
        for (UInt32 index = 0; index < report.rowFields.size(); ++index)
        {
            if (!WriteText(worksheet, headerRow, firstColumn + index, report.fields[report.rowFields[index]].name))
            {
                return false;
            }
        }
    }
    for (Size line = 0; line < report.columnLines.size(); ++line)
    {
        for (UInt32 dataIndex = 0; dataIndex < dataFieldCount; ++dataIndex)
        {
            const auto column = firstColumn + report.labelColumnCount +
                                static_cast<UInt32>(line) * dataFieldCount + dataIndex;
            const auto& columnLine = report.columnLines[line];
            std::string caption;
            switch (columnLine.kind)
            {
                case LineKind::Data:
                    caption = report.dataFields[dataIndex].name;
                    break;
                case LineKind::Subtotal:
                {
                    const auto level = columnLine.items.size() - 1;
                    caption = report.fields[report.columnFields[level]].items[columnLine.items[level]].Caption() +
                              std::string(kSubtotalSuffix);
                    break;
                }
                case LineKind::Grand:
                    caption = dataFieldCount > 1 ? totalCaption + " " + report.dataFields[dataIndex].name
                                                 : totalCaption;
                    break;
            }
            if (!WriteText(worksheet, headerRow, column, caption))
            {
                return false;
            }
        }
    }

    // Report body.
    for (Size line = 0; line < report.rowLines.size(); ++line)
    {
        const auto row = firstRow + report.headerRowCount + static_cast<UInt32>(line);
        const auto& rowLine = report.rowLines[line];
        switch (rowLine.kind)
        {
            case LineKind::Data:
            {
                if (report.rowFields.empty())
                {
                    if (!WriteText(worksheet, row, firstColumn, totalCaption))
                    {
                        return false;
                    }
                    break;
                }
                for (UInt32 level = 0; level < report.rowFields.size(); ++level)
                {
                    if (!StartsGroup(report.rowLines, line, level))
                    {
                        continue;
                    }
                    if (!WriteText(worksheet, row, firstColumn + level,
                                   report.fields[report.rowFields[level]].items[rowLine.items[level]].Caption()))
                    {
                        return false;
                    }
                }
                break;
            }
            case LineKind::Subtotal:
            {
                const auto level = rowLine.items.size() - 1;
                const auto caption =
                    report.fields[report.rowFields[level]].items[rowLine.items[level]].Caption() +
                    std::string(kSubtotalSuffix);
                if (!WriteText(worksheet, row, firstColumn + static_cast<UInt32>(level), caption))
                {
                    return false;
                }
                break;
            }
            case LineKind::Grand:
                if (!WriteText(worksheet, row, firstColumn, totalCaption))
                {
                    return false;
                }
                break;
        }

        for (Size columnLine = 0; columnLine < report.columnLines.size(); ++columnLine)
        {
            for (UInt32 dataIndex = 0; dataIndex < dataFieldCount; ++dataIndex)
            {
                const auto found = report.cells.find(std::make_tuple(line, columnLine, Size(dataIndex)));
                if (found == report.cells.end())
                {
                    continue;
                }
                const auto value = found->second.Result(report.dataFields[dataIndex].function);
                if (!value)
                {
                    continue;
                }
                const auto column = firstColumn + report.labelColumnCount +
                                    static_cast<UInt32>(columnLine) * dataFieldCount + dataIndex;
                if (!worksheet.SetCellNumber(row, column, *value))
                {
                    return false;
                }
            }
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// SpreadsheetML emission
// ---------------------------------------------------------------------------

template <typename TElement>
std::shared_ptr<TElement> Append(const std::shared_ptr<OpenXMLElement>& parent)
{
    if (!parent)
    {
        return nullptr;
    }
    auto child = parent->AppendChild<TElement>();
    return child ? child : parent->AppendChildRaw<TElement>();
}

void ClearChildren(const std::shared_ptr<OpenXMLElement>& element)
{
    if (!element)
    {
        return;
    }
    for (const auto& child : element->Children())
    {
        element->RemoveChild(child);
    }
}

S::PivotTableAxisValues::Value AxisToXml(PivotAxis axis)
{
    switch (axis)
    {
        case PivotAxis::Row:
            return S::PivotTableAxisValues::AxisRow;
        case PivotAxis::Column:
            return S::PivotTableAxisValues::AxisColumn;
        case PivotAxis::Page:
            return S::PivotTableAxisValues::AxisPage;
        case PivotAxis::None:
            break;
    }
    return S::PivotTableAxisValues::NotDefinedEnumValue;
}

PivotAxis AxisFromXml(S::PivotTableAxisValues::Value axis)
{
    switch (axis)
    {
        case S::PivotTableAxisValues::AxisRow:
            return PivotAxis::Row;
        case S::PivotTableAxisValues::AxisColumn:
            return PivotAxis::Column;
        case S::PivotTableAxisValues::AxisPage:
            return PivotAxis::Page;
        default:
            break;
    }
    return PivotAxis::None;
}

S::DataConsolidateFunctionValues::Value FunctionToXml(PivotAggregateFunction function)
{
    switch (function)
    {
        case PivotAggregateFunction::Sum:
            return S::DataConsolidateFunctionValues::Sum;
        case PivotAggregateFunction::Count:
            return S::DataConsolidateFunctionValues::Count;
        case PivotAggregateFunction::CountNumbers:
            return S::DataConsolidateFunctionValues::CountNumbers;
        case PivotAggregateFunction::Average:
            return S::DataConsolidateFunctionValues::Average;
        case PivotAggregateFunction::Maximum:
            return S::DataConsolidateFunctionValues::Maximum;
        case PivotAggregateFunction::Minimum:
            return S::DataConsolidateFunctionValues::Minimum;
        case PivotAggregateFunction::Product:
            return S::DataConsolidateFunctionValues::Product;
        case PivotAggregateFunction::StandardDeviation:
            return S::DataConsolidateFunctionValues::StandardDeviation;
        case PivotAggregateFunction::StandardDeviationP:
            return S::DataConsolidateFunctionValues::StandardDeviationP;
        case PivotAggregateFunction::Variance:
            return S::DataConsolidateFunctionValues::Variance;
        case PivotAggregateFunction::VarianceP:
            return S::DataConsolidateFunctionValues::VarianceP;
    }
    return S::DataConsolidateFunctionValues::Sum;
}

PivotAggregateFunction FunctionFromXml(S::DataConsolidateFunctionValues::Value function)
{
    switch (function)
    {
        case S::DataConsolidateFunctionValues::Count:
            return PivotAggregateFunction::Count;
        case S::DataConsolidateFunctionValues::CountNumbers:
            return PivotAggregateFunction::CountNumbers;
        case S::DataConsolidateFunctionValues::Average:
            return PivotAggregateFunction::Average;
        case S::DataConsolidateFunctionValues::Maximum:
            return PivotAggregateFunction::Maximum;
        case S::DataConsolidateFunctionValues::Minimum:
            return PivotAggregateFunction::Minimum;
        case S::DataConsolidateFunctionValues::Product:
            return PivotAggregateFunction::Product;
        case S::DataConsolidateFunctionValues::StandardDeviation:
            return PivotAggregateFunction::StandardDeviation;
        case S::DataConsolidateFunctionValues::StandardDeviationP:
            return PivotAggregateFunction::StandardDeviationP;
        case S::DataConsolidateFunctionValues::Variance:
            return PivotAggregateFunction::Variance;
        case S::DataConsolidateFunctionValues::VarianceP:
            return PivotAggregateFunction::VarianceP;
        default:
            break;
    }
    return PivotAggregateFunction::Sum;
}

S::ShowDataAsValues::Value ShowDataAsToXml(PivotShowDataAs value)
{
    switch (value)
    {
        case PivotShowDataAs::Normal:
            return S::ShowDataAsValues::Normal;
        case PivotShowDataAs::Difference:
            return S::ShowDataAsValues::Difference;
        case PivotShowDataAs::PercentOf:
            return S::ShowDataAsValues::Percent;
        case PivotShowDataAs::PercentDifference:
            return S::ShowDataAsValues::PercentageDifference;
        case PivotShowDataAs::RunningTotal:
            return S::ShowDataAsValues::RunTotal;
        case PivotShowDataAs::PercentOfRow:
            return S::ShowDataAsValues::PercentOfRaw;
        case PivotShowDataAs::PercentOfColumn:
            return S::ShowDataAsValues::PercentOfColumn;
        case PivotShowDataAs::PercentOfTotal:
            return S::ShowDataAsValues::PercentOfTotal;
        case PivotShowDataAs::Index:
            return S::ShowDataAsValues::Index;
    }
    return S::ShowDataAsValues::Normal;
}

PivotShowDataAs ShowDataAsFromXml(S::ShowDataAsValues::Value value)
{
    switch (value)
    {
        case S::ShowDataAsValues::Difference:
            return PivotShowDataAs::Difference;
        case S::ShowDataAsValues::Percent:
            return PivotShowDataAs::PercentOf;
        case S::ShowDataAsValues::PercentageDifference:
            return PivotShowDataAs::PercentDifference;
        case S::ShowDataAsValues::RunTotal:
            return PivotShowDataAs::RunningTotal;
        case S::ShowDataAsValues::PercentOfRaw:
            return PivotShowDataAs::PercentOfRow;
        case S::ShowDataAsValues::PercentOfColumn:
            return PivotShowDataAs::PercentOfColumn;
        case S::ShowDataAsValues::PercentOfTotal:
            return PivotShowDataAs::PercentOfTotal;
        case S::ShowDataAsValues::Index:
            return PivotShowDataAs::Index;
        default:
            break;
    }
    return PivotShowDataAs::Normal;
}

/** Emits one `sharedItems` child describing a cached value. */
void WriteSharedItem(const std::shared_ptr<S::SharedItems>& items, const Value& value)
{
    switch (value.kind)
    {
        case ValueKind::Blank:
            Append<S::MissingItem>(items);
            break;
        case ValueKind::Number:
            if (auto item = Append<S::NumberItem>(items))
            {
                item->SetVal(DoubleValue(value.number));
            }
            break;
        case ValueKind::Boolean:
            if (auto item = Append<S::BooleanItem>(items))
            {
                item->SetVal(BooleanValue(value.number != 0.0));
            }
            break;
        case ValueKind::Error:
            if (auto item = Append<S::ErrorItem>(items))
            {
                item->SetVal(StringValue(value.text));
            }
            break;
        case ValueKind::Text:
            if (auto item = Append<S::StringItem>(items))
            {
                item->SetVal(StringValue(value.text));
            }
            break;
    }
}

/** Emits one `pivotCacheRecords/r` child holding a literal, non-enumerated value. */
void WriteRecordValue(const std::shared_ptr<S::PivotCacheRecord>& record, const Value& value)
{
    switch (value.kind)
    {
        case ValueKind::Blank:
            Append<S::MissingItem>(record);
            break;
        case ValueKind::Number:
            if (auto item = Append<S::NumberItem>(record))
            {
                item->SetVal(DoubleValue(value.number));
            }
            break;
        case ValueKind::Boolean:
            if (auto item = Append<S::BooleanItem>(record))
            {
                item->SetVal(BooleanValue(value.number != 0.0));
            }
            break;
        case ValueKind::Error:
            if (auto item = Append<S::ErrorItem>(record))
            {
                item->SetVal(StringValue(value.text));
            }
            break;
        case ValueKind::Text:
            if (auto item = Append<S::StringItem>(record))
            {
                item->SetVal(StringValue(value.text));
            }
            break;
    }
}

bool WriteCache(const std::shared_ptr<Packaging::PivotTableCacheDefinitionPart>& cachePart,
                const std::shared_ptr<Packaging::PivotTableCacheRecordsPart>& recordsPart,
                const BuiltReport& report,
                const SourceData& source,
                bool refreshOnLoad)
{
    auto definition = cachePart ? cachePart->GetPivotCacheDefinition() : nullptr;
    auto records = recordsPart ? recordsPart->GetPivotCacheRecords() : nullptr;
    if (!definition || !records)
    {
        return false;
    }
    ClearChildren(definition);
    ClearChildren(records);

    definition->SetId(StringValue(recordsPart->RelationshipId()));
    definition->SetRefreshOnLoad(BooleanValue(refreshOnLoad));
    definition->SetRefreshedBy(StringValue(std::string(kRefreshedBy)));
    definition->SetCreatedVersion(ByteValue(kCreatedVersion));
    definition->SetRefreshedVersion(ByteValue(kRefreshedVersion));
    definition->SetMinRefreshableVersion(ByteValue(kMinRefreshableVersion));
    definition->SetRecordCount(UInt32Value(static_cast<UInt32>(report.recordCount)));
    definition->SetSaveData(BooleanValue(true));

    auto cacheSource = Append<S::CacheSource>(definition);
    if (!cacheSource)
    {
        return false;
    }
    cacheSource->SetType(EnumValue<S::SourceValues>(S::SourceValues::Worksheet));
    auto worksheetSource = Append<S::WorksheetSource>(cacheSource);
    if (!worksheetSource)
    {
        return false;
    }
    worksheetSource->SetReference(StringValue(report.sourceRange.ToA1()));
    worksheetSource->SetSheet(StringValue(report.sourceSheet));

    auto cacheFields = Append<S::CacheFields>(definition);
    if (!cacheFields)
    {
        return false;
    }
    cacheFields->SetCount(UInt32Value(static_cast<UInt32>(report.fields.size())));
    for (const auto& field : report.fields)
    {
        auto cacheField = Append<S::CacheField>(cacheFields);
        if (!cacheField)
        {
            return false;
        }
        cacheField->SetName(StringValue(field.name));
        cacheField->SetNumberFormatId(UInt32Value(0));
        auto sharedItems = Append<S::SharedItems>(cacheField);
        if (!sharedItems)
        {
            return false;
        }
        if (field.enumerated)
        {
            bool containsBlank = false;
            bool containsNumber = false;
            bool containsString = false;
            for (const auto& item : field.items)
            {
                containsBlank = containsBlank || item.kind == ValueKind::Blank;
                containsNumber = containsNumber || item.kind == ValueKind::Number;
                containsString = containsString || item.kind != ValueKind::Number;
                WriteSharedItem(sharedItems, item);
            }
            sharedItems->SetCount(UInt32Value(static_cast<UInt32>(field.items.size())));
            sharedItems->SetContainsBlank(BooleanValue(containsBlank));
            sharedItems->SetContainsNumber(BooleanValue(containsNumber));
            sharedItems->SetContainsString(BooleanValue(containsString));
            sharedItems->SetContainsSemiMixedTypes(BooleanValue(containsString));
        }
        else
        {
            sharedItems->SetContainsBlank(BooleanValue(field.containsBlank));
            sharedItems->SetContainsNumber(BooleanValue(field.containsNumber));
            sharedItems->SetContainsString(BooleanValue(field.containsString));
            sharedItems->SetContainsSemiMixedTypes(BooleanValue(field.containsString || field.containsError));
            if (field.containsNumber)
            {
                sharedItems->SetContainsInteger(BooleanValue(field.containsInteger));
                sharedItems->SetMinValue(DoubleValue(field.minimum));
                sharedItems->SetMaxValue(DoubleValue(field.maximum));
            }
        }
    }

    records->SetCount(UInt32Value(static_cast<UInt32>(report.recordCount)));
    for (Size row = 0; row < source.rows.size(); ++row)
    {
        auto record = Append<S::PivotCacheRecord>(records);
        if (!record)
        {
            return false;
        }
        for (Size column = 0; column < report.fields.size(); ++column)
        {
            const auto& field = report.fields[column];
            if (field.enumerated)
            {
                auto item = Append<S::FieldItem>(record);
                if (!item)
                {
                    return false;
                }
                item->SetVal(UInt32Value(field.recordItems[row]));
            }
            else
            {
                WriteRecordValue(record, source.rows[row][column]);
            }
        }
    }
    return true;
}

/** Emits `rowItems` or `colItems` from the report lines. */
bool WriteAxisItems(const std::shared_ptr<OpenXMLElement>& container,
                    const std::vector<Line>& lines,
                    Size dataFieldCount,
                    bool expandDataFields)
{
    const auto repetitions = expandDataFields ? dataFieldCount : Size{1};
    for (const auto& line : lines)
    {
        for (Size dataIndex = 0; dataIndex < repetitions; ++dataIndex)
        {
            auto item = Append<S::RowItem>(container);
            if (!item)
            {
                return false;
            }
            if (line.kind == LineKind::Subtotal)
            {
                item->SetItemType(EnumValue<S::ItemValues>(S::ItemValues::Default));
            }
            else if (line.kind == LineKind::Grand)
            {
                item->SetItemType(EnumValue<S::ItemValues>(S::ItemValues::Grand));
            }
            if (dataIndex != 0)
            {
                item->SetIndex(UInt32Value(static_cast<UInt32>(dataIndex)));
            }
            // A grand total line addresses the axis at depth one with the
            // implicit item zero; every other line spells out its own prefix.
            // `v` carries a schema default of zero but is written explicitly so
            // the markup stays unambiguous for readers that do not apply it.
            if (line.kind == LineKind::Grand)
            {
                auto index = Append<S::MemberPropertyIndex>(item);
                if (!index)
                {
                    return false;
                }
                index->SetVal(Int32Value(0));
                continue;
            }
            for (const auto value : line.items)
            {
                auto index = Append<S::MemberPropertyIndex>(item);
                if (!index)
                {
                    return false;
                }
                index->SetVal(Int32Value(static_cast<Int32>(value)));
            }
        }
    }
    return true;
}

bool WriteDefinition(const std::shared_ptr<Packaging::PivotTablePart>& part,
                     const BuiltReport& report,
                     const ExcelPivotTableDefinition& definition,
                     UInt32 cacheId,
                     const std::string& name)
{
    auto root = part ? part->GetPivotTableDefinition() : nullptr;
    const auto rectangle = ReportRectangle(report);
    if (!root || !rectangle)
    {
        return false;
    }
    ClearChildren(root);

    root->SetName(StringValue(name));
    root->SetCacheId(UInt32Value(cacheId));
    root->SetDataOnRows(BooleanValue(false));
    root->SetApplyNumberFormats(BooleanValue(false));
    root->SetApplyBorderFormats(BooleanValue(false));
    root->SetApplyFontFormats(BooleanValue(false));
    root->SetApplyPatternFormats(BooleanValue(false));
    root->SetApplyAlignmentFormats(BooleanValue(false));
    root->SetApplyWidthHeightFormats(BooleanValue(true));
    root->SetDataCaption(StringValue(definition.DataCaption));
    root->SetGrandTotalCaption(StringValue(definition.GrandTotalCaption));
    root->SetRowHeaderCaption(StringValue(definition.RowHeaderCaption));
    root->SetColumnHeaderCaption(StringValue(definition.ColumnHeaderCaption));
    root->SetTag(StringValue(definition.WriteCachedReport ? std::string{} : std::string(kNoCachedReportTag)));
    root->SetUpdatedVersion(ByteValue(kUpdatedVersion));
    root->SetMinRefreshableVersion(ByteValue(kMinRefreshableVersion));
    root->SetCreatedVersion(ByteValue(kCreatedVersion));
    root->SetUseAutoFormatting(BooleanValue(true));
    root->SetItemPrintTitles(BooleanValue(true));
    root->SetIndent(UInt32Value(0));
    root->SetOutline(BooleanValue(false));
    root->SetOutlineData(BooleanValue(false));
    root->SetCompact(BooleanValue(false));
    root->SetCompactData(BooleanValue(false));
    root->SetMultipleFieldFilters(BooleanValue(false));
    root->SetRowGrandTotals(BooleanValue(definition.RowGrandTotals));
    root->SetColumnGrandTotals(BooleanValue(definition.ColumnGrandTotals));

    auto location = Append<S::Location>(root);
    if (!location)
    {
        return false;
    }
    location->SetReference(StringValue(rectangle->ToA1()));
    location->SetFirstHeaderRow(UInt32Value(std::max<UInt32>(
        static_cast<UInt32>(report.columnFields.size()), 1)));
    location->SetFirstDataRow(UInt32Value(report.headerRowCount));
    location->SetFirstDataColumn(UInt32Value(report.labelColumnCount));

    auto pivotFields = Append<S::PivotFields>(root);
    if (!pivotFields)
    {
        return false;
    }
    pivotFields->SetCount(UInt32Value(static_cast<UInt32>(report.fields.size())));
    for (const auto& field : report.fields)
    {
        auto pivotField = Append<S::PivotField>(pivotFields);
        if (!pivotField)
        {
            return false;
        }
        if (field.axis != PivotAxis::None)
        {
            pivotField->SetAxis(EnumValue<S::PivotTableAxisValues>(AxisToXml(field.axis)));
        }
        if (field.isDataField)
        {
            pivotField->SetDataField(BooleanValue(true));
        }
        pivotField->SetShowAll(BooleanValue(field.showAll));
        pivotField->SetCompact(BooleanValue(false));
        pivotField->SetOutline(BooleanValue(false));
        pivotField->SetSubtotalTop(BooleanValue(false));
        pivotField->SetDefaultSubtotal(BooleanValue(field.showSubtotal));
        if (field.insertBlankRow)
        {
            pivotField->SetInsertBlankRow(BooleanValue(true));
        }
        if (!field.enumerated)
        {
            continue;
        }
        auto items = Append<S::Items>(pivotField);
        if (!items)
        {
            return false;
        }
        UInt32 itemCount = 0;
        for (UInt32 index = 0; index < field.items.size(); ++index)
        {
            auto item = Append<S::Item>(items);
            if (!item)
            {
                return false;
            }
            item->SetIndex(UInt32Value(index));
            ++itemCount;
        }
        if (field.showSubtotal)
        {
            auto item = Append<S::Item>(items);
            if (!item)
            {
                return false;
            }
            item->SetItemType(EnumValue<S::ItemValues>(S::ItemValues::Default));
            ++itemCount;
        }
        items->SetCount(UInt32Value(itemCount));
    }

    if (!report.rowFields.empty())
    {
        auto rowFields = Append<S::RowFields>(root);
        if (!rowFields)
        {
            return false;
        }
        rowFields->SetCount(UInt32Value(static_cast<UInt32>(report.rowFields.size())));
        for (const auto fieldIndex : report.rowFields)
        {
            auto field = Append<S::Field>(rowFields);
            if (!field)
            {
                return false;
            }
            field->SetIndex(Int32Value(static_cast<Int32>(fieldIndex)));
        }
    }

    auto rowItems = Append<S::RowItems>(root);
    if (!rowItems || !WriteAxisItems(rowItems, report.rowLines, report.dataFields.size(), false))
    {
        return false;
    }
    rowItems->SetCount(UInt32Value(static_cast<UInt32>(report.rowLines.size())));

    const bool dataFieldsOnColumnAxis = report.dataFields.size() > 1;
    if (!report.columnFields.empty() || dataFieldsOnColumnAxis)
    {
        auto columnFields = Append<S::ColumnFields>(root);
        if (!columnFields)
        {
            return false;
        }
        UInt32 count = 0;
        for (const auto fieldIndex : report.columnFields)
        {
            auto field = Append<S::Field>(columnFields);
            if (!field)
            {
                return false;
            }
            field->SetIndex(Int32Value(static_cast<Int32>(fieldIndex)));
            ++count;
        }
        if (dataFieldsOnColumnAxis)
        {
            auto field = Append<S::Field>(columnFields);
            if (!field)
            {
                return false;
            }
            // -2 marks the position of the data field axis inside the column fields.
            field->SetIndex(Int32Value(-2));
            ++count;
        }
        columnFields->SetCount(UInt32Value(count));
    }

    auto columnItems = Append<S::ColumnItems>(root);
    if (!columnItems ||
        !WriteAxisItems(columnItems, report.columnLines, report.dataFields.size(), dataFieldsOnColumnAxis))
    {
        return false;
    }
    columnItems->SetCount(UInt32Value(report.ValueColumnCount()));

    if (!report.pageFields.empty())
    {
        auto pageFields = Append<S::PageFields>(root);
        if (!pageFields)
        {
            return false;
        }
        pageFields->SetCount(UInt32Value(static_cast<UInt32>(report.pageFields.size())));
        for (const auto fieldIndex : report.pageFields)
        {
            auto pageField = Append<S::PageField>(pageFields);
            if (!pageField)
            {
                return false;
            }
            pageField->SetField(Int32Value(static_cast<Int32>(fieldIndex)));
            pageField->SetHierarchy(Int32Value(-1));
            if (const auto selected = report.fields[fieldIndex].selectedItem)
            {
                pageField->SetItem(UInt32Value(*selected));
            }
        }
    }

    auto dataFields = Append<S::DataFields>(root);
    if (!dataFields)
    {
        return false;
    }
    dataFields->SetCount(UInt32Value(static_cast<UInt32>(report.dataFields.size())));
    for (const auto& dataField : report.dataFields)
    {
        auto element = Append<S::DataField>(dataFields);
        if (!element)
        {
            return false;
        }
        element->SetName(StringValue(dataField.name));
        element->SetField(UInt32Value(static_cast<UInt32>(dataField.field)));
        element->SetSubtotal(EnumValue<S::DataConsolidateFunctionValues>(FunctionToXml(dataField.function)));
        if (dataField.showDataAs != PivotShowDataAs::Normal)
        {
            element->SetShowDataAs(EnumValue<S::ShowDataAsValues>(ShowDataAsToXml(dataField.showDataAs)));
            if (dataField.baseField)
            {
                element->SetBaseField(Int32Value(static_cast<Int32>(*dataField.baseField)));
            }
            if (dataField.baseItem)
            {
                element->SetBaseItem(UInt32Value(*dataField.baseItem));
            }
        }
        if (dataField.numberFormatId)
        {
            element->SetNumberFormatId(UInt32Value(*dataField.numberFormatId));
        }
    }

    if (!definition.Style.Name.empty())
    {
        auto style = Append<S::PivotTableStyle>(root);
        if (!style)
        {
            return false;
        }
        style->SetName(StringValue(definition.Style.Name));
        style->SetShowRowHeaders(BooleanValue(definition.Style.ShowRowHeaders));
        style->SetShowColumnHeaders(BooleanValue(definition.Style.ShowColumnHeaders));
        style->SetShowRowStripes(BooleanValue(definition.Style.ShowRowStripes));
        style->SetShowColumnStripes(BooleanValue(definition.Style.ShowColumnStripes));
        style->SetShowLastColumn(BooleanValue(definition.Style.ShowLastColumn));
    }
    return true;
}

// ---------------------------------------------------------------------------
// Workbook-level pivot cache registry
// ---------------------------------------------------------------------------

std::shared_ptr<S::PivotCaches> GetPivotCaches(const ExcelDocument::Ptr& document, bool create)
{
    auto workbookPart = document ? document->GetWorkbookPart() : nullptr;
    auto workbook = workbookPart ? workbookPart->GetTypedRootElement() : nullptr;
    if (!workbook)
    {
        return nullptr;
    }
    auto caches = workbook->GetFirstChildOfType<S::PivotCaches>();
    if (!caches && create)
    {
        caches = workbook->AppendChild<S::PivotCaches>();
    }
    return caches;
}

UInt32 NextCacheId(const ExcelDocument::Ptr& document)
{
    UInt32 next = 1;
    if (const auto caches = GetPivotCaches(document, false))
    {
        for (const auto& cache : caches->Elements<S::PivotCache>())
        {
            next = std::max(next, cache->GetCacheId().ValueOr(0) + 1);
        }
    }
    return next;
}

/** Returns the relationship identifier that links @p source to @p target. */
std::string RelationshipIdBetween(const OpenXmlPackagePart& source, const OpenXmlPackagePart& target)
{
    for (const auto& incoming : target.IncomingRelationships())
    {
        if (incoming.SourceUri == source.Uri())
        {
            return incoming.Id;
        }
    }
    return {};
}

std::vector<std::shared_ptr<Packaging::PivotTablePart>> AllPivotTableParts(const ExcelDocument::Ptr& document)
{
    std::vector<std::shared_ptr<Packaging::PivotTablePart>> result;
    auto workbookPart = document ? document->GetWorkbookPart() : nullptr;
    if (!workbookPart)
    {
        return result;
    }
    for (const auto& worksheetPart : workbookPart->GetWorksheetParts())
    {
        if (!worksheetPart)
        {
            continue;
        }
        for (const auto& part : worksheetPart->GetPivotTableParts())
        {
            result.push_back(part);
        }
    }
    return result;
}

bool PivotTableNameExists(const ExcelDocument::Ptr& document,
                          std::string_view name,
                          const std::shared_ptr<Packaging::PivotTablePart>& except)
{
    for (const auto& part : AllPivotTableParts(document))
    {
        if (part == except)
        {
            continue;
        }
        const auto root = part->GetPivotTableDefinition();
        if (root && EqualsIgnoreCase(root->GetName().ToString(), name))
        {
            return true;
        }
    }
    return false;
}

std::string MakeUniquePivotTableName(const ExcelDocument::Ptr& document,
                                     const std::shared_ptr<Packaging::PivotTablePart>& except)
{
    for (UInt32 index = 1; index < 100000; ++index)
    {
        auto candidate = "PivotTable" + std::to_string(index);
        if (!PivotTableNameExists(document, candidate, except))
        {
            return candidate;
        }
    }
    return {};
}

std::shared_ptr<Packaging::WorksheetPart> HostWorksheetPart(
    const ExcelDocument::Ptr& document,
    const std::shared_ptr<Packaging::PivotTablePart>& part)
{
    auto workbookPart = document ? document->GetWorkbookPart() : nullptr;
    if (!workbookPart || !part)
    {
        return nullptr;
    }
    for (const auto& worksheetPart : workbookPart->GetWorksheetParts())
    {
        if (!worksheetPart)
        {
            continue;
        }
        const auto parts = worksheetPart->GetPivotTableParts();
        if (std::find(parts.begin(), parts.end(), part) != parts.end())
        {
            return worksheetPart;
        }
    }
    return nullptr;
}

Worksheet::Ptr HostWorksheet(const ExcelDocument::Ptr& document,
                             const std::shared_ptr<Packaging::PivotTablePart>& part)
{
    const auto worksheetPart = HostWorksheetPart(document, part);
    if (!worksheetPart)
    {
        return nullptr;
    }
    ExcelDocumentEditor editor(document);
    for (const auto& sheet : editor.Worksheets())
    {
        if (sheet && sheet->GetPart() == worksheetPart)
        {
            return sheet;
        }
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
// Reading an existing pivot table back into a definition
// ---------------------------------------------------------------------------

UInt32 PageRowCount(const std::shared_ptr<S::PivotTableDefinition>& root)
{
    const auto pageFields = root ? root->GetFirstChildOfType<S::PageFields>() : nullptr;
    if (!pageFields)
    {
        return 0;
    }
    const auto count = pageFields->Elements<S::PageField>().size();
    return count == 0 ? 0 : static_cast<UInt32>(count) + 1;
}

/**
 * Reads the cache field indices of one axis in report order.
 *
 * The `rowFields`, `colFields`, and `pageFields` elements preserve the order in
 * which the fields are nested, which is not the pivot cache field order.
 * The data field placeholder (index -2) is skipped.
 */
template <typename TContainer, typename TEntry>
std::vector<Size> AxisFieldOrder(const std::shared_ptr<S::PivotTableDefinition>& root)
{
    std::vector<Size> result;
    const auto container = root ? root->GetFirstChildOfType<TContainer>() : nullptr;
    if (!container)
    {
        return result;
    }
    for (const auto& entry : container->template Elements<TEntry>())
    {
        const auto index = entry->GetField().ValueOr(-1);
        if (index >= 0)
        {
            result.push_back(static_cast<Size>(index));
        }
    }
    return result;
}

/** `rowFields`/`colFields` store the index in `@x` instead of `@fld`. */
template <typename TContainer>
std::vector<Size> AxisFieldOrderByIndex(const std::shared_ptr<S::PivotTableDefinition>& root)
{
    std::vector<Size> result;
    const auto container = root ? root->GetFirstChildOfType<TContainer>() : nullptr;
    if (!container)
    {
        return result;
    }
    for (const auto& entry : container->template Elements<S::Field>())
    {
        const auto index = entry->GetIndex().ValueOr(-1);
        if (index >= 0)
        {
            result.push_back(static_cast<Size>(index));
        }
    }
    return result;
}

std::vector<std::string> CacheFieldNames(const std::shared_ptr<Packaging::PivotTableCacheDefinitionPart>& cachePart)
{
    std::vector<std::string> names;
    const auto definition = cachePart ? cachePart->GetPivotCacheDefinition() : nullptr;
    const auto fields = definition ? definition->GetFirstChildOfType<S::CacheFields>() : nullptr;
    if (!fields)
    {
        return names;
    }
    for (const auto& field : fields->Elements<S::CacheField>())
    {
        names.push_back(field->GetName().ToString());
    }
    return names;
}

} // namespace PivotDetail

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

bool IsValidPivotTableName(std::string_view name)
{
    if (name.empty() || name.size() > 255)
    {
        return false;
    }
    for (const auto character : name)
    {
        if (static_cast<unsigned char>(character) < 0x20)
        {
            return false;
        }
    }
    return name.front() != ' ' && name.back() != ' ';
}

ExcelPivotTable::ExcelPivotTable(std::shared_ptr<Packaging::PivotTablePart> part,
                                 std::shared_ptr<Packaging::ExcelDocument> document)
    : m_part(std::move(part)), m_document(std::move(document))
{
}

bool ExcelPivotTable::IsValid() const noexcept
{
    return m_part != nullptr && m_document != nullptr;
}

std::shared_ptr<Packaging::PivotTablePart> ExcelPivotTable::GetPart() const
{
    return m_part;
}

std::shared_ptr<Packaging::PivotTableCacheDefinitionPart> ExcelPivotTable::GetCacheDefinitionPart() const
{
    return m_part ? m_part->GetPivotTableCacheDefinitionPart() : nullptr;
}

std::shared_ptr<Packaging::PivotTableCacheRecordsPart> ExcelPivotTable::GetCacheRecordsPart() const
{
    const auto cachePart = GetCacheDefinitionPart();
    return cachePart ? cachePart->GetPivotTableCacheRecordsPart() : nullptr;
}

std::string ExcelPivotTable::Name() const
{
    const auto root = m_part ? m_part->GetPivotTableDefinition() : nullptr;
    return root ? root->GetName().ToString() : std::string{};
}

PivotTableResult ExcelPivotTable::SetName(std::string_view name)
{
    namespace Detail = PivotDetail;
    auto root = m_part ? m_part->GetPivotTableDefinition() : nullptr;
    if (!root || !m_document)
    {
        return Detail::Failure(PivotTableError::InvalidWorksheet, "The pivot table is detached.");
    }
    if (!IsValidPivotTableName(name))
    {
        return Detail::Failure(PivotTableError::InvalidName,
                               "'" + std::string(name) + "' is not a valid pivot table name.");
    }
    if (Detail::PivotTableNameExists(m_document, name, m_part))
    {
        return Detail::Failure(PivotTableError::InvalidName,
                               "The pivot table name '" + std::string(name) + "' is already used in this workbook.");
    }
    const auto previousName = root->GetName().ToString();
    root->SetName(StringValue(std::string(name)));
    // Slicer caches identify their pivot tables by name, so the rename has to
    // reach them or their slicers become orphaned.
    SlicerDetail::RenamePivotTableInSlicers(m_document, previousName, name);
    return Detail::Success();
}

std::optional<CellRange> ExcelPivotTable::ReportRange() const
{
    const auto root = m_part ? m_part->GetPivotTableDefinition() : nullptr;
    const auto location = root ? root->GetFirstChildOfType<DocumentFormat::OpenXml::Spreadsheet::Location>() : nullptr;
    if (!location)
    {
        return std::nullopt;
    }
    return CellRange::ParseA1(location->GetReference().ToString());
}

std::optional<CellRange> ExcelPivotTable::WrittenRange() const
{
    const auto report = ReportRange();
    const auto root = m_part ? m_part->GetPivotTableDefinition() : nullptr;
    if (!report || !root)
    {
        return std::nullopt;
    }
    const auto pageRows = PivotDetail::PageRowCount(root);
    if (pageRows == 0)
    {
        return report;
    }
    const auto firstRow = report->First().Row().Value() - pageRows;
    const auto lastColumn = std::max<UInt32>(report->Last().Column().Value(),
                                             report->First().Column().Value() + 1);
    return CellRange(CellAddress(RowIndex(firstRow), report->First().Column()),
                     CellAddress(report->Last().Row(), ColumnIndex(lastColumn)));
}

std::string ExcelPivotTable::SourceSheet() const
{
    namespace S = DocumentFormat::OpenXml::Spreadsheet;
    const auto cachePart = GetCacheDefinitionPart();
    const auto definition = cachePart ? cachePart->GetPivotCacheDefinition() : nullptr;
    const auto source = definition ? definition->GetFirstChildOfType<S::CacheSource>() : nullptr;
    const auto worksheetSource = source ? source->GetFirstChildOfType<S::WorksheetSource>() : nullptr;
    return worksheetSource ? worksheetSource->GetSheet().ToString() : std::string{};
}

std::optional<CellRange> ExcelPivotTable::SourceRange() const
{
    namespace S = DocumentFormat::OpenXml::Spreadsheet;
    const auto cachePart = GetCacheDefinitionPart();
    const auto definition = cachePart ? cachePart->GetPivotCacheDefinition() : nullptr;
    const auto source = definition ? definition->GetFirstChildOfType<S::CacheSource>() : nullptr;
    const auto worksheetSource = source ? source->GetFirstChildOfType<S::WorksheetSource>() : nullptr;
    if (!worksheetSource)
    {
        return std::nullopt;
    }
    return CellRange::ParseA1(worksheetSource->GetReference().ToString());
}

UInt32 ExcelPivotTable::CacheId() const
{
    const auto root = m_part ? m_part->GetPivotTableDefinition() : nullptr;
    return root ? root->GetCacheId().ValueOr(0) : 0;
}

std::vector<std::string> ExcelPivotTable::SourceFieldNames() const
{
    return PivotDetail::CacheFieldNames(GetCacheDefinitionPart());
}

std::vector<ExcelPivotItem> ExcelPivotTable::FieldItems(std::string_view fieldName) const
{
    namespace S = DocumentFormat::OpenXml::Spreadsheet;
    std::vector<ExcelPivotItem> result;
    const auto cachePart = GetCacheDefinitionPart();
    const auto definition = cachePart ? cachePart->GetPivotCacheDefinition() : nullptr;
    const auto fields = definition ? definition->GetFirstChildOfType<S::CacheFields>() : nullptr;
    if (!fields)
    {
        return result;
    }
    for (const auto& field : fields->Elements<S::CacheField>())
    {
        if (!PivotDetail::EqualsIgnoreCase(field->GetName().ToString(), fieldName))
        {
            continue;
        }
        const auto sharedItems = field->GetFirstChildOfType<S::SharedItems>();
        if (!sharedItems)
        {
            return result;
        }
        // Dispatch on the element name rather than the wrapper class: several
        // SpreadsheetML types share the `x:s` and `x:x` element names, so the
        // DOM factory may materialize a differently typed - but equally named -
        // wrapper. The name reported by the wrapper is always the node's name.
        for (const auto& child : sharedItems->Children())
        {
            const auto localName = child->QualifiedName().localName();
            const auto text = child->GetAttribute(OpenXmlQualifiedName({}, "v"));
            ExcelPivotItem item;
            if (localName == "n")
            {
                DoubleValue number;
                number.AssignFromString(text);
                item.Number = number.ValueOr(0.0);
                item.Caption = PivotDetail::FormatNumber(*item.Number);
            }
            else if (localName == "b")
            {
                BooleanValue boolean;
                boolean.AssignFromString(text);
                item.Caption = boolean.ValueOr(false) ? "TRUE" : "FALSE";
            }
            else if (localName == "s" || localName == "e" || localName == "d")
            {
                item.Caption = std::string(text);
            }
            else
            {
                item.Blank = true;
            }
            result.push_back(std::move(item));
        }
        return result;
    }
    return result;
}

std::vector<ExcelPivotField> ExcelPivotTable::Fields() const
{
    namespace S = DocumentFormat::OpenXml::Spreadsheet;
    std::vector<ExcelPivotField> result;
    const auto root = m_part ? m_part->GetPivotTableDefinition() : nullptr;
    const auto pivotFields = root ? root->GetFirstChildOfType<S::PivotFields>() : nullptr;
    if (!pivotFields)
    {
        return result;
    }
    const auto names = SourceFieldNames();
    const auto elements = pivotFields->Elements<S::PivotField>();

    std::map<Size, std::optional<UInt32>> pageSelections;
    if (const auto pageFields = root->GetFirstChildOfType<S::PageFields>())
    {
        for (const auto& pageField : pageFields->Elements<S::PageField>())
        {
            const auto index = pageField->GetField().ValueOr(-1);
            if (index < 0)
            {
                continue;
            }
            const auto item = pageField->GetItem();
            pageSelections[static_cast<Size>(index)] =
                item.IsDefined() ? std::optional(item.Value()) : std::nullopt;
        }
    }

    for (Size index = 0; index < elements.size(); ++index)
    {
        ExcelPivotField field;
        field.Name = index < names.size() ? names[index] : std::string{};
        field.Axis = PivotDetail::AxisFromXml(elements[index]->GetAxis().ValueOr({}).GetValue());
        field.ShowSubtotal = elements[index]->GetDefaultSubtotal().ValueOr(true);
        field.ShowAll = elements[index]->GetShowAll().ValueOr(false);
        field.InsertBlankRow = elements[index]->GetInsertBlankRow().ValueOr(false);
        if (field.Axis == PivotAxis::Page)
        {
            const auto selection = pageSelections.find(index);
            if (selection != pageSelections.end())
            {
                field.SelectedItem = selection->second;
            }
        }
        result.push_back(std::move(field));
    }
    return result;
}

std::vector<ExcelPivotDataField> ExcelPivotTable::DataFields() const
{
    namespace S = DocumentFormat::OpenXml::Spreadsheet;
    std::vector<ExcelPivotDataField> result;
    const auto root = m_part ? m_part->GetPivotTableDefinition() : nullptr;
    const auto dataFields = root ? root->GetFirstChildOfType<S::DataFields>() : nullptr;
    if (!dataFields)
    {
        return result;
    }
    const auto names = SourceFieldNames();
    for (const auto& element : dataFields->Elements<S::DataField>())
    {
        ExcelPivotDataField dataField;
        const auto index = element->GetField().ValueOr(0);
        dataField.SourceField = index < names.size() ? names[index] : std::string{};
        dataField.Name = element->GetName().ToString();
        dataField.Function = PivotDetail::FunctionFromXml(element->GetSubtotal().ValueOr({}).GetValue());
        dataField.ShowDataAs = PivotDetail::ShowDataAsFromXml(element->GetShowDataAs().ValueOr({}).GetValue());
        if (dataField.ShowDataAs != PivotShowDataAs::Normal)
        {
            const auto baseField = element->GetBaseField();
            if (baseField.IsDefined() && baseField.Value() >= 0 &&
                static_cast<Size>(baseField.Value()) < names.size())
            {
                dataField.BaseField = names[static_cast<Size>(baseField.Value())];
            }
            const auto baseItem = element->GetBaseItem();
            if (baseItem.IsDefined())
            {
                dataField.BaseItem = baseItem.Value();
            }
        }
        const auto numberFormat = element->GetNumberFormatId();
        if (numberFormat.IsDefined())
        {
            dataField.NumberFormatId = numberFormat.Value();
        }
        result.push_back(std::move(dataField));
    }
    return result;
}

ExcelPivotTableStyle ExcelPivotTable::Style() const
{
    namespace S = DocumentFormat::OpenXml::Spreadsheet;
    ExcelPivotTableStyle style;
    const auto root = m_part ? m_part->GetPivotTableDefinition() : nullptr;
    const auto element = root ? root->GetFirstChildOfType<S::PivotTableStyle>() : nullptr;
    if (!element)
    {
        style.Name.clear();
        return style;
    }
    style.Name = element->GetName().ToString();
    style.ShowRowHeaders = element->GetShowRowHeaders().ValueOr(false);
    style.ShowColumnHeaders = element->GetShowColumnHeaders().ValueOr(false);
    style.ShowRowStripes = element->GetShowRowStripes().ValueOr(false);
    style.ShowColumnStripes = element->GetShowColumnStripes().ValueOr(false);
    style.ShowLastColumn = element->GetShowLastColumn().ValueOr(false);
    return style;
}

PivotTableResult ExcelPivotTable::SetStyle(const ExcelPivotTableStyle& style)
{
    namespace S = DocumentFormat::OpenXml::Spreadsheet;
    auto root = m_part ? m_part->GetPivotTableDefinition() : nullptr;
    if (!root)
    {
        return PivotDetail::Failure(PivotTableError::InvalidWorksheet, "The pivot table is detached.");
    }
    auto element = root->GetFirstChildOfType<S::PivotTableStyle>();
    if (style.Name.empty())
    {
        if (element)
        {
            root->RemoveChild(element);
        }
        return PivotDetail::Success();
    }
    if (!element)
    {
        element = PivotDetail::Append<S::PivotTableStyle>(root);
    }
    if (!element)
    {
        return PivotDetail::Failure(PivotTableError::WriteFailed, "The pivot table style could not be written.");
    }
    element->SetName(StringValue(style.Name));
    element->SetShowRowHeaders(BooleanValue(style.ShowRowHeaders));
    element->SetShowColumnHeaders(BooleanValue(style.ShowColumnHeaders));
    element->SetShowRowStripes(BooleanValue(style.ShowRowStripes));
    element->SetShowColumnStripes(BooleanValue(style.ShowColumnStripes));
    element->SetShowLastColumn(BooleanValue(style.ShowLastColumn));
    return PivotDetail::Success();
}

bool ExcelPivotTable::RowGrandTotals() const
{
    const auto root = m_part ? m_part->GetPivotTableDefinition() : nullptr;
    return root ? root->GetRowGrandTotals().ValueOr(true) : false;
}

bool ExcelPivotTable::ColumnGrandTotals() const
{
    const auto root = m_part ? m_part->GetPivotTableDefinition() : nullptr;
    return root ? root->GetColumnGrandTotals().ValueOr(true) : false;
}

std::optional<ExcelPivotTableDefinition> ExcelPivotTable::Definition() const
{
    const auto root = m_part ? m_part->GetPivotTableDefinition() : nullptr;
    const auto written = WrittenRange();
    const auto source = SourceRange();
    if (!root || !written || !source)
    {
        return std::nullopt;
    }

    ExcelPivotTableDefinition definition;
    definition.Name = Name();
    definition.SourceSheet = SourceSheet();
    definition.SourceRange = *source;
    definition.TargetCell = written->First();
    definition.DataFields = DataFields();
    definition.Style = Style();
    definition.RowGrandTotals = RowGrandTotals();
    definition.ColumnGrandTotals = ColumnGrandTotals();
    definition.DataCaption = root->GetDataCaption().ToString();
    definition.GrandTotalCaption = root->GetGrandTotalCaption().ToString();
    definition.RowHeaderCaption = root->GetRowHeaderCaption().ToString();
    definition.ColumnHeaderCaption = root->GetColumnHeaderCaption().ToString();
    definition.WriteCachedReport = root->GetTag().ToString() != std::string(PivotDetail::kNoCachedReportTag);

    const auto cachePart = GetCacheDefinitionPart();
    const auto cacheDefinition = cachePart ? cachePart->GetPivotCacheDefinition() : nullptr;
    definition.RefreshOnLoad = cacheDefinition ? cacheDefinition->GetRefreshOnLoad().ValueOr(false) : false;

    // Restore the placement order stored on the axis elements, which differs
    // from the pivot cache field order returned by Fields().
    auto fields = Fields();
    const auto append = [&](const std::vector<Size>& order)
    {
        for (const auto index : order)
        {
            if (index < fields.size() && fields[index].Axis != PivotAxis::None)
            {
                definition.Fields.push_back(fields[index]);
            }
        }
    };
    append(PivotDetail::AxisFieldOrderByIndex<DocumentFormat::OpenXml::Spreadsheet::RowFields>(root));
    append(PivotDetail::AxisFieldOrderByIndex<DocumentFormat::OpenXml::Spreadsheet::ColumnFields>(root));
    append(PivotDetail::AxisFieldOrder<DocumentFormat::OpenXml::Spreadsheet::PageFields,
                                       DocumentFormat::OpenXml::Spreadsheet::PageField>(root));
    return definition;
}

PivotTableResult ExcelPivotTable::Refresh()
{
    const auto definition = Definition();
    if (!definition)
    {
        return PivotDetail::Failure(PivotTableError::InvalidWorksheet, "The pivot table is detached.");
    }
    return Update(*definition);
}

PivotTableResult ExcelPivotTable::SetSourceRange(CellRange range, std::string_view sheet)
{
    auto definition = Definition();
    if (!definition)
    {
        return PivotDetail::Failure(PivotTableError::InvalidWorksheet, "The pivot table is detached.");
    }
    definition->SourceRange = range;
    if (!sheet.empty())
    {
        definition->SourceSheet = std::string(sheet);
    }
    return Update(*definition);
}

PivotTableResult ExcelPivotTable::MoveTo(CellAddress targetCell)
{
    auto definition = Definition();
    if (!definition)
    {
        return PivotDetail::Failure(PivotTableError::InvalidWorksheet, "The pivot table is detached.");
    }
    definition->TargetCell = targetCell;
    return Update(*definition);
}

PivotTableResult ExcelPivotTable::SetFieldAxis(std::string_view fieldName, PivotAxis axis)
{
    auto definition = Definition();
    if (!definition)
    {
        return PivotDetail::Failure(PivotTableError::InvalidWorksheet, "The pivot table is detached.");
    }
    const auto names = SourceFieldNames();
    const auto known = std::any_of(names.begin(), names.end(), [&](const std::string& candidate)
                                   { return PivotDetail::EqualsIgnoreCase(candidate, fieldName); });
    if (!known)
    {
        return PivotDetail::Failure(PivotTableError::UnknownField,
                                    "The pivot field '" + std::string(fieldName) +
                                        "' is not a column of the source range.");
    }

    auto& fields = definition->Fields;
    const auto existing = std::find_if(fields.begin(), fields.end(), [&](const ExcelPivotField& candidate)
                                       { return PivotDetail::EqualsIgnoreCase(candidate.Name, fieldName); });
    if (axis == PivotAxis::None)
    {
        if (existing != fields.end())
        {
            fields.erase(existing);
        }
    }
    else if (existing != fields.end())
    {
        existing->Axis = axis;
        if (axis != PivotAxis::Page)
        {
            existing->SelectedItem.reset();
        }
    }
    else
    {
        ExcelPivotField field;
        field.Name = std::string(fieldName);
        field.Axis = axis;
        fields.push_back(std::move(field));
    }
    return Update(*definition);
}

PivotTableResult ExcelPivotTable::SetGrandTotals(bool rows, bool columns)
{
    auto definition = Definition();
    if (!definition)
    {
        return PivotDetail::Failure(PivotTableError::InvalidWorksheet, "The pivot table is detached.");
    }
    definition->RowGrandTotals = rows;
    definition->ColumnGrandTotals = columns;
    return Update(*definition);
}

std::optional<Real> ExcelPivotTable::AggregatedValue(const std::vector<std::string>& rowItems,
                                                     const std::vector<std::string>& columnItems,
                                                     std::string_view dataFieldName) const
{
    namespace Detail = PivotDetail;
    const auto definition = Definition();
    if (!definition)
    {
        return std::nullopt;
    }
    PivotTableResult status;
    const auto source = Detail::ReadSource(m_document, definition->SourceSheet, definition->SourceRange, status);
    if (!source)
    {
        return std::nullopt;
    }
    Detail::BuiltReport report;
    report.sourceSheet = definition->SourceSheet;
    report.sourceRange = definition->SourceRange;
    report.targetCell = definition->TargetCell;
    if (!Detail::BuildReportModel(*definition, *source, report))
    {
        return std::nullopt;
    }

    Size dataIndex = 0;
    if (!dataFieldName.empty())
    {
        const auto found = std::find_if(report.dataFields.begin(), report.dataFields.end(),
                                        [&](const Detail::BuiltDataField& candidate)
                                        {
                                            return Detail::EqualsIgnoreCase(candidate.name, dataFieldName);
                                        });
        if (found == report.dataFields.end())
        {
            return std::nullopt;
        }
        dataIndex = static_cast<Size>(std::distance(report.dataFields.begin(), found));
    }

    // Resolves captions to a line, preferring the exact prefix depth requested.
    const auto findLine = [](const std::vector<Detail::Line>& lines,
                             const std::vector<Size>& axisFields,
                             const std::vector<Detail::BuiltField>& fields,
                             const std::vector<std::string>& captions) -> std::optional<Size>
    {
        if (captions.empty())
        {
            for (Size index = 0; index < lines.size(); ++index)
            {
                if (lines[index].kind == Detail::LineKind::Grand ||
                    (axisFields.empty() && lines[index].items.empty()))
                {
                    return index;
                }
            }
            return std::nullopt;
        }
        if (captions.size() > axisFields.size())
        {
            return std::nullopt;
        }
        std::vector<UInt32> prefix;
        for (Size level = 0; level < captions.size(); ++level)
        {
            const auto& items = fields[axisFields[level]].items;
            const auto found = std::find_if(items.begin(), items.end(), [&](const Detail::Value& candidate)
                                            { return Detail::EqualsIgnoreCase(candidate.Caption(), captions[level]); });
            if (found == items.end())
            {
                return std::nullopt;
            }
            prefix.push_back(static_cast<UInt32>(std::distance(items.begin(), found)));
        }
        for (Size index = 0; index < lines.size(); ++index)
        {
            if (lines[index].items.size() == prefix.size() && lines[index].items == prefix)
            {
                return index;
            }
        }
        return std::nullopt;
    };

    const auto rowLine = findLine(report.rowLines, report.rowFields, report.fields, rowItems);
    const auto columnLine = findLine(report.columnLines, report.columnFields, report.fields, columnItems);
    if (!rowLine || !columnLine)
    {
        return std::nullopt;
    }
    const auto cell = report.cells.find(std::make_tuple(*rowLine, *columnLine, dataIndex));
    if (cell == report.cells.end())
    {
        return std::nullopt;
    }
    return cell->second.Result(report.dataFields[dataIndex].function);
}

PivotTableResult ExcelPivotTable::Update(const ExcelPivotTableDefinition& definition)
{
    namespace Detail = PivotDetail;
    if (!m_part || !m_document)
    {
        return Detail::Failure(PivotTableError::InvalidWorksheet, "The pivot table is detached.");
    }
    auto host = Detail::HostWorksheet(m_document, m_part);
    if (!host)
    {
        return Detail::Failure(PivotTableError::InvalidWorksheet,
                               "The worksheet that hosts the pivot table could not be resolved.");
    }
    const auto cachePart = GetCacheDefinitionPart();
    const auto recordsPart = GetCacheRecordsPart();
    if (!cachePart || !recordsPart)
    {
        return Detail::Failure(PivotTableError::InvalidWorksheet, "The pivot cache parts are missing.");
    }

    const auto previousWritten = WrittenRange();
    const auto worksheetXml = host->GetPart()->GetXmlString();
    const auto definitionXml = m_part->GetXmlString();
    const auto cacheXml = cachePart->GetXmlString();
    const auto recordsXml = recordsPart->GetXmlString();
    const auto restore = [&]()
    {
        host->GetPart()->SetXmlString(worksheetXml);
        m_part->SetXmlString(definitionXml);
        cachePart->SetXmlString(cacheXml);
        recordsPart->SetXmlString(recordsXml);
    };

    const auto previousName = Name();
    if (previousWritten)
    {
        host->ClearRange(*previousWritten);
    }
    auto result = Worksheet::WritePivotTable(*host, m_part, cachePart, recordsPart, m_document, definition, CacheId());
    if (!result)
    {
        restore();
        return result;
    }

    // Rebuilding the cache renumbers its shared items, so slicer item indexes
    // that were correct a moment ago would now select the wrong values.
    const auto currentName = Name();
    if (!PivotDetail::EqualsIgnoreCase(previousName, currentName))
    {
        SlicerDetail::RenamePivotTableInSlicers(m_document, previousName, currentName);
    }
    SlicerDetail::RefreshSlicersForPivotTable(m_document, currentName);
    return result;
}

// ---------------------------------------------------------------------------
// PivotTableBuilder
// ---------------------------------------------------------------------------

PivotTableBuilder::PivotTableBuilder(std::shared_ptr<Worksheet> sheet) : m_sheet(std::move(sheet))
{
}

PivotTableBuilder& PivotTableBuilder::SetName(std::string name)
{
    m_definition.Name = std::move(name);
    return *this;
}

PivotTableBuilder& PivotTableBuilder::SetSource(const CellRange& range)
{
    m_definition.SourceSheet.clear();
    m_definition.SourceRange = range;
    return *this;
}

PivotTableBuilder& PivotTableBuilder::SetSource(const Worksheet& sheet, const CellRange& range)
{
    m_definition.SourceSheet = sheet.Name();
    m_definition.SourceRange = range;
    return *this;
}

PivotTableBuilder& PivotTableBuilder::SetSource(std::string sheetName, const CellRange& range)
{
    m_definition.SourceSheet = std::move(sheetName);
    m_definition.SourceRange = range;
    return *this;
}

PivotTableBuilder& PivotTableBuilder::SetTarget(CellAddress targetCell)
{
    m_definition.TargetCell = targetCell;
    return *this;
}

PivotTableBuilder& PivotTableBuilder::AddRowField(std::string fieldName, bool showSubtotal)
{
    ExcelPivotField field;
    field.Name = std::move(fieldName);
    field.Axis = PivotAxis::Row;
    field.ShowSubtotal = showSubtotal;
    m_definition.Fields.push_back(std::move(field));
    return *this;
}

PivotTableBuilder& PivotTableBuilder::AddColumnField(std::string fieldName, bool showSubtotal)
{
    ExcelPivotField field;
    field.Name = std::move(fieldName);
    field.Axis = PivotAxis::Column;
    field.ShowSubtotal = showSubtotal;
    m_definition.Fields.push_back(std::move(field));
    return *this;
}

PivotTableBuilder& PivotTableBuilder::AddPageField(std::string fieldName, std::optional<UInt32> selectedItem)
{
    ExcelPivotField field;
    field.Name = std::move(fieldName);
    field.Axis = PivotAxis::Page;
    field.SelectedItem = selectedItem;
    m_definition.Fields.push_back(std::move(field));
    return *this;
}

PivotTableBuilder& PivotTableBuilder::AddDataField(std::string fieldName,
                                                   PivotAggregateFunction function,
                                                   std::string displayName)
{
    ExcelPivotDataField dataField;
    dataField.SourceField = std::move(fieldName);
    dataField.Name = std::move(displayName);
    dataField.Function = function;
    m_definition.DataFields.push_back(std::move(dataField));
    return *this;
}

PivotTableBuilder& PivotTableBuilder::AddDataField(ExcelPivotDataField dataField)
{
    m_definition.DataFields.push_back(std::move(dataField));
    return *this;
}

PivotTableBuilder& PivotTableBuilder::ShowGrandTotals(bool rows, bool columns)
{
    m_definition.RowGrandTotals = rows;
    m_definition.ColumnGrandTotals = columns;
    return *this;
}

PivotTableBuilder& PivotTableBuilder::SetStyle(ExcelPivotTableStyle style)
{
    m_definition.Style = std::move(style);
    return *this;
}

PivotTableBuilder& PivotTableBuilder::WriteCachedReport(bool write)
{
    m_definition.WriteCachedReport = write;
    return *this;
}

PivotTableBuilder& PivotTableBuilder::RefreshOnLoad(bool refresh)
{
    m_definition.RefreshOnLoad = refresh;
    return *this;
}

ExcelPivotTable::Ptr PivotTableBuilder::Build()
{
    return m_sheet ? m_sheet->CreatePivotTable(m_definition).PivotTable : nullptr;
}

// ---------------------------------------------------------------------------
// Worksheet integration
// ---------------------------------------------------------------------------

PivotTableResult Worksheet::WritePivotTable(Worksheet& host,
                                            const std::shared_ptr<Packaging::PivotTablePart>& part,
                                            const std::shared_ptr<Packaging::PivotTableCacheDefinitionPart>& cachePart,
                                            const std::shared_ptr<Packaging::PivotTableCacheRecordsPart>& recordsPart,
                                            const ExcelDocument::Ptr& document,
                                            const ExcelPivotTableDefinition& definition,
                                            UInt32 cacheId)
{
    namespace Detail = PivotDetail;

    auto name = definition.Name;
    if (name.empty())
    {
        name = Detail::MakeUniquePivotTableName(document, part);
    }
    if (!IsValidPivotTableName(name))
    {
        return Detail::Failure(PivotTableError::InvalidName, "'" + name + "' is not a valid pivot table name.");
    }
    if (Detail::PivotTableNameExists(document, name, part))
    {
        return Detail::Failure(PivotTableError::InvalidName,
                               "The pivot table name '" + name + "' is already used in this workbook.");
    }
    if (!definition.TargetCell.IsValid())
    {
        return Detail::Failure(PivotTableError::InvalidTarget, "The pivot table target cell is not valid.");
    }

    const auto sourceSheet = definition.SourceSheet.empty() ? host.Name() : definition.SourceSheet;
    PivotTableResult status;
    const auto source = Detail::ReadSource(document, sourceSheet, definition.SourceRange, status);
    if (!source)
    {
        return status;
    }

    Detail::BuiltReport report;
    report.sourceSheet = sourceSheet;
    report.sourceRange = definition.SourceRange;
    report.targetCell = definition.TargetCell;
    if (auto built = Detail::BuildReportModel(definition, *source, report); !built)
    {
        return built;
    }

    const auto written = Detail::WrittenRectangle(report);
    if (!written)
    {
        return Detail::Failure(PivotTableError::InvalidTarget,
                               "The pivot table report does not fit on the worksheet grid at " +
                                   definition.TargetCell.ToA1() + ".");
    }

    // Reports on the same worksheet must not overlap each other.
    if (Detail::EqualsIgnoreCase(sourceSheet, host.Name()))
    {
        if (Detail::RangesIntersect(*written, definition.SourceRange))
        {
            return Detail::Failure(PivotTableError::OverlappingReport,
                                   "The pivot table report would overlap its own source range.");
        }
    }
    for (const auto& other : host.PivotTables())
    {
        if (!other || other->GetPart() == part)
        {
            continue;
        }
        const auto otherRange = other->WrittenRange();
        if (otherRange && Detail::RangesIntersect(*written, *otherRange))
        {
            return Detail::Failure(PivotTableError::OverlappingReport,
                                   "The pivot table report would overlap the pivot table '" + other->Name() + "'.");
        }
    }

    if (!Detail::WriteCache(cachePart, recordsPart, report, *source, definition.RefreshOnLoad))
    {
        return Detail::Failure(PivotTableError::WriteFailed, "The pivot cache could not be written.");
    }
    if (!Detail::WriteDefinition(part, report, definition, cacheId, name))
    {
        return Detail::Failure(PivotTableError::WriteFailed, "The pivot table definition could not be written.");
    }
    if (definition.WriteCachedReport && !Detail::RenderReport(host, report, definition))
    {
        return Detail::Failure(PivotTableError::WriteFailed, "The pivot table report cells could not be written.");
    }
    return Detail::Success();
}

PivotTableCreationResult Worksheet::CreatePivotTable(const ExcelPivotTableDefinition& definition)
{
    namespace Detail = PivotDetail;
    const auto report = [](PivotTableResult status) -> PivotTableCreationResult
    {
        return PivotTableCreationResult{std::move(status), nullptr};
    };

    if (!m_part || !m_document)
    {
        return report(Detail::Failure(PivotTableError::InvalidWorksheet, "The worksheet is detached."));
    }
    auto workbookPart = m_document->GetWorkbookPart();
    if (!workbookPart)
    {
        return report(Detail::Failure(PivotTableError::InvalidWorksheet, "The workbook part is missing."));
    }

    const auto cacheId = Detail::NextCacheId(m_document);
    auto cachePart = workbookPart->AddPivotTableCacheDefinitionPart();
    auto recordsPart = cachePart ? cachePart->AddPivotTableCacheRecordsPart() : nullptr;
    auto part = m_part->AddPivotTablePart();
    const auto rollback = [&]()
    {
        if (part)
        {
            m_part->RemovePivotTablePart(part);
        }
        if (cachePart)
        {
            workbookPart->RemovePivotTableCacheDefinitionPart(cachePart);
        }
    };
    if (!cachePart || !recordsPart || !part)
    {
        rollback();
        return report(Detail::Failure(PivotTableError::WriteFailed, "The pivot table parts could not be created."));
    }
    if (part->AddPartReference(cachePart, Packaging::PivotTableCacheDefinitionPart::Descriptor().RelationshipType)
            .empty())
    {
        rollback();
        return report(
            Detail::Failure(PivotTableError::WriteFailed, "The pivot cache relationship could not be created."));
    }

    auto caches = Detail::GetPivotCaches(m_document, true);
    auto cacheEntry = caches ? Detail::Append<DocumentFormat::OpenXml::Spreadsheet::PivotCache>(caches) : nullptr;
    if (!cacheEntry)
    {
        rollback();
        return report(
            Detail::Failure(PivotTableError::WriteFailed, "The workbook pivot cache registry could not be updated."));
    }
    cacheEntry->SetCacheId(UInt32Value(cacheId));
    cacheEntry->SetId(StringValue(Detail::RelationshipIdBetween(*workbookPart, *cachePart)));

    auto status = WritePivotTable(*this, part, cachePart, recordsPart, m_document, definition, cacheId);
    if (!status)
    {
        caches->RemoveChild(cacheEntry);
        rollback();
        return report(std::move(status));
    }
    return PivotTableCreationResult{Detail::Success(), std::make_shared<ExcelPivotTable>(part, m_document)};
}

std::vector<ExcelPivotTable::Ptr> Worksheet::PivotTables() const
{
    std::vector<ExcelPivotTable::Ptr> result;
    if (!m_part || !m_document)
    {
        return result;
    }
    for (const auto& part : m_part->GetPivotTableParts())
    {
        result.push_back(std::make_shared<ExcelPivotTable>(part, m_document));
    }
    return result;
}

ExcelPivotTable::Ptr Worksheet::PivotTableByName(std::string_view name) const
{
    for (const auto& pivotTable : PivotTables())
    {
        if (pivotTable && PivotDetail::EqualsIgnoreCase(pivotTable->Name(), name))
        {
            return pivotTable;
        }
    }
    return nullptr;
}

bool Worksheet::RemovePivotTable(const ExcelPivotTable::Ptr& pivotTable)
{
    namespace Detail = PivotDetail;
    namespace S = DocumentFormat::OpenXml::Spreadsheet;
    if (!pivotTable || !m_part || !m_document)
    {
        return false;
    }
    const auto part = pivotTable->GetPart();
    const auto parts = m_part->GetPivotTableParts();
    if (!part || std::find(parts.begin(), parts.end(), part) == parts.end())
    {
        return false;
    }

    const auto written = pivotTable->WrittenRange();
    const auto cachePart = pivotTable->GetCacheDefinitionPart();
    const auto cacheId = pivotTable->CacheId();
    const auto pivotTableName = pivotTable->Name();

    if (!m_part->RemovePivotTablePart(part))
    {
        return false;
    }

    // Slicer caches list the pivot tables they drive; leaving a dangling entry
    // behind makes spreadsheet applications repair the package.
    SlicerDetail::DetachPivotTableFromSlicers(m_document, pivotTableName);
    if (written)
    {
        ClearRange(*written);
    }

    // Retain a cache that another pivot table still uses.
    bool cacheStillUsed = false;
    for (const auto& other : Detail::AllPivotTableParts(m_document))
    {
        if (other->GetPivotTableCacheDefinitionPart() == cachePart)
        {
            cacheStillUsed = true;
            break;
        }
    }
    if (!cacheStillUsed)
    {
        auto workbookPart = m_document->GetWorkbookPart();
        if (workbookPart && cachePart)
        {
            workbookPart->RemovePivotTableCacheDefinitionPart(cachePart);
        }
        if (auto caches = Detail::GetPivotCaches(m_document, false))
        {
            for (const auto& entry : caches->Elements<S::PivotCache>())
            {
                if (entry->GetCacheId().ValueOr(0) == cacheId)
                {
                    caches->RemoveChild(entry);
                    break;
                }
            }
            auto workbook = workbookPart ? workbookPart->GetTypedRootElement() : nullptr;
            if (workbook && caches->Elements<S::PivotCache>().empty())
            {
                workbook->RemoveChild(caches);
            }
        }
    }
    return true;
}

} // namespace ExyokiOffice::Excel
