// Copyright (c) 2026 Jakub Melka and Collaborators
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "ExyokiOffice/Excel/ExcelTable.hpp"

#include "ExyokiOffice/DOM/DocumentFormat/OpenXml/Spreadsheet.hpp"
#include "ExyokiOffice/Packaging/GeneratedParts.hpp"
#include "ExyokiOffice/StandardTypes.hpp"

#include <algorithm>
#include <cctype>
#include <unordered_set>

namespace ExyokiOffice::Excel
{
namespace Spreadsheet = ExyokiOffice::DocumentFormat::OpenXml::Spreadsheet;

class ExcelTableImplementation final
{
public:
    ExcelTableImplementation() = delete;

    static Spreadsheet::TotalsRowFunctionValues::Value
    ToXml(TableTotalsFunction value)
    {
        using Xml = Spreadsheet::TotalsRowFunctionValues;
        switch (value)
        {
            case TableTotalsFunction::Sum:
                return Xml::Sum;
            case TableTotalsFunction::Minimum:
                return Xml::Minimum;
            case TableTotalsFunction::Maximum:
                return Xml::Maximum;
            case TableTotalsFunction::Average:
                return Xml::Average;
            case TableTotalsFunction::Count:
                return Xml::Count;
            case TableTotalsFunction::CountNumbers:
                return Xml::CountNumbers;
            case TableTotalsFunction::StandardDeviation:
                return Xml::StandardDeviation;
            case TableTotalsFunction::Variance:
                return Xml::Variance;
            case TableTotalsFunction::Custom:
                return Xml::Custom;
            case TableTotalsFunction::None:
                return Xml::None;
        }
        return Xml::None;
    }

    static TableTotalsFunction
    FromXml(Spreadsheet::TotalsRowFunctionValues::Value value)
    {
        using Xml = Spreadsheet::TotalsRowFunctionValues;
        switch (value)
        {
            case Xml::Sum:
                return TableTotalsFunction::Sum;
            case Xml::Minimum:
                return TableTotalsFunction::Minimum;
            case Xml::Maximum:
                return TableTotalsFunction::Maximum;
            case Xml::Average:
                return TableTotalsFunction::Average;
            case Xml::Count:
                return TableTotalsFunction::Count;
            case Xml::CountNumbers:
                return TableTotalsFunction::CountNumbers;
            case Xml::StandardDeviation:
                return TableTotalsFunction::StandardDeviation;
            case Xml::Variance:
                return TableTotalsFunction::Variance;
            case Xml::Custom:
                return TableTotalsFunction::Custom;
            default:
                return TableTotalsFunction::None;
        }
    }

    static bool ValidColumns(const std::vector<ExcelTableColumn>& columns,
                             Size expectedCount)
    {
        if (columns.size() != expectedCount || columns.empty())
        {
            return false;
        }
        std::unordered_set<std::string> names;
        std::unordered_set<UInt32> ids;
        for (const auto& column : columns)
        {
            if (column.Name.empty() || column.Name.size() > 255)
            {
                return false;
            }
            std::string folded;
            folded.reserve(column.Name.size());
            for (const auto ch : column.Name)
            {
                folded.push_back(
                    static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
            }
            if (!names.insert(std::move(folded)).second ||
                (column.Id && !ids.insert(column.Id).second))
            {
                return false;
            }
            if (column.TotalsFunction == TableTotalsFunction::Custom &&
                !column.TotalsRowFormula)
            {
                return false;
            }
            if (column.TotalsRowFormula &&
                column.TotalsFunction != TableTotalsFunction::Custom &&
                column.TotalsFunction != TableTotalsFunction::None)
            {
                return false;
            }
        }
        return true;
    }
};

bool IsValidExcelTableName(std::string_view name)
{
    if (name.empty() || name.size() > 255)
    {
        return false;
    }
    const auto first = static_cast<unsigned char>(name.front());
    if (!(std::isalpha(first) || name.front() == '_' || name.front() == '\\'))
    {
        return false;
    }
    for (const auto ch : name)
    {
        const auto value = static_cast<unsigned char>(ch);
        if (!(std::isalnum(value) || ch == '_' || ch == '.' || ch == '\\'))
        {
            return false;
        }
    }
    return !CellAddress::ParseA1(name) && !CellAddress::ParseR1C1(name);
}

ExcelTable::ExcelTable(std::shared_ptr<Packaging::TableDefinitionPart> part)
    : m_part(std::move(part)) {}

UInt32 ExcelTable::Id() const
{
    const auto table = m_part ? m_part->GetTable() : nullptr;
    return table ? table->GetId().ValueOr(0) : 0;
}

std::string ExcelTable::Name() const
{
    const auto table = m_part ? m_part->GetTable() : nullptr;
    return table ? table->GetDisplayName().ToString() : std::string{};
}

std::optional<CellRange> ExcelTable::Range() const
{
    const auto table = m_part ? m_part->GetTable() : nullptr;
    return table ? CellRange::ParseA1(table->GetReference().ToString())
                 : std::nullopt;
}

std::vector<ExcelTableColumn> ExcelTable::Columns() const
{
    std::vector<ExcelTableColumn> result;
    const auto table = m_part ? m_part->GetTable() : nullptr;
    const auto columns =
        table ? table->GetFirstChildOfType<Spreadsheet::TableColumns>() : nullptr;
    for (const auto& column :
         columns ? columns->Elements<Spreadsheet::TableColumn>()
                 : std::vector<std::shared_ptr<Spreadsheet::TableColumn>>{})
    {
        ExcelTableColumn model;
        model.Id = column->GetId().ValueOr(0);
        model.Name = column->GetName().ToString();
        model.TotalsRowLabel =
            column->GetTotalsRowLabel().IsDefined()
                ? std::optional<std::string>(column->GetTotalsRowLabel().ToString())
                : std::nullopt;
        model.TotalsFunction = ExcelTableImplementation::FromXml(
            column->GetTotalsRowFunction()
                .ValueOr(Spreadsheet::TotalsRowFunctionValues())
                .GetValue());
        if (const auto formula =
                column->GetFirstChildOfType<Spreadsheet::CalculatedColumnFormula>())
        {
            model.CalculatedColumnFormula = std::string(formula->GetText());
        }
        if (const auto formula =
                column->GetFirstChildOfType<Spreadsheet::TotalsRowFormula>())
        {
            model.TotalsRowFormula = std::string(formula->GetText());
        }
        result.push_back(std::move(model));
    }
    return result;
}

bool ExcelTable::AutoFilterEnabled() const
{
    const auto table = m_part ? m_part->GetTable() : nullptr;
    return table && table->GetFirstChildOfType<Spreadsheet::AutoFilter>();
}

bool ExcelTable::TotalsRowShown() const
{
    const auto table = m_part ? m_part->GetTable() : nullptr;
    return table && table->GetTotalsRowShown().ValueOr(false);
}

bool ExcelTable::SetName(std::string_view name)
{
    const auto table = m_part ? m_part->GetTable() : nullptr;
    if (!table || !IsValidExcelTableName(name))
    {
        return false;
    }
    table->SetName(StringValue(std::string(name)));
    table->SetDisplayName(StringValue(std::string(name)));
    return true;
}

bool ExcelTable::SetColumns(const std::vector<ExcelTableColumn>& columns)
{
    const auto table = m_part ? m_part->GetTable() : nullptr;
    const auto range = Range();
    if (!table || !range ||
        !ExcelTableImplementation::ValidColumns(columns, range->ColumnCount()))
    {
        return false;
    }
    const auto originalXml = m_part->GetXmlString();
    if (const auto current =
            table->GetFirstChildOfType<Spreadsheet::TableColumns>())
    {
        table->RemoveChild(current);
    }
    const auto xmlColumns = table->AppendChild<Spreadsheet::TableColumns>();
    if (!xmlColumns)
    {
        return m_part->SetXmlString(originalXml), false;
    }
    xmlColumns->SetCount(UInt32Value(static_cast<UInt32>(columns.size())));
    std::unordered_set<UInt32> usedIds;
    for (const auto& value : columns)
    {
        auto id = value.Id;
        if (!id)
        {
            id = 1;
            while (usedIds.contains(id))
            {
                ++id;
            }
        }
        usedIds.insert(id);
        const auto column = xmlColumns->AppendChild<Spreadsheet::TableColumn>();
        if (!column)
        {
            return m_part->SetXmlString(originalXml), false;
        }
        column->SetId(UInt32Value(id));
        column->SetName(StringValue(value.Name));
        if (value.TotalsRowLabel)
        {
            column->SetTotalsRowLabel(StringValue(*value.TotalsRowLabel));
        }
        if (value.TotalsFunction != TableTotalsFunction::None)
        {
            column->SetTotalsRowFunction(
                EnumValue<Spreadsheet::TotalsRowFunctionValues>(
                    ExcelTableImplementation::ToXml(value.TotalsFunction)));
        }
        if (value.CalculatedColumnFormula)
        {
            const auto formula =
                column->AppendChild<Spreadsheet::CalculatedColumnFormula>();
            if (!formula)
            {
                return m_part->SetXmlString(originalXml), false;
            }
            formula->SetText(*value.CalculatedColumnFormula);
        }
        if (value.TotalsRowFormula)
        {
            const auto formula = column->AppendChild<Spreadsheet::TotalsRowFormula>();
            if (!formula)
            {
                return m_part->SetXmlString(originalXml), false;
            }
            formula->SetText(*value.TotalsRowFormula);
        }
    }
    return true;
}

bool ExcelTable::Resize(CellRange range)
{
    const auto table = m_part ? m_part->GetTable() : nullptr;
    const auto current = Range();
    if (!table || !range.IsValid() || !current ||
        range.ColumnCount() != current->ColumnCount())
    {
        return false;
    }
    const auto originalXml = m_part->GetXmlString();
    table->SetReference(StringValue(range.ToA1()));
    if (const auto filter = table->GetFirstChildOfType<Spreadsheet::AutoFilter>())
    {
        filter->SetReference(StringValue(range.ToA1()));
    }
    if (TotalsRowShown() && range.RowCount() < 2)
    {
        return m_part->SetXmlString(originalXml), false;
    }
    return true;
}

bool ExcelTable::Resize(CellRange range,
                        const std::vector<ExcelTableColumn>& columns)
{
    const auto table = m_part ? m_part->GetTable() : nullptr;
    if (!table || !range.IsValid() ||
        !ExcelTableImplementation::ValidColumns(columns, range.ColumnCount()) ||
        (TotalsRowShown() && range.RowCount() < 2))
    {
        return false;
    }
    const auto originalXml = m_part->GetXmlString();
    table->SetReference(StringValue(range.ToA1()));
    if (!SetColumns(columns))
    {
        return m_part->SetXmlString(originalXml), false;
    }
    const auto autoFilter = table->GetFirstChildOfType<Spreadsheet::AutoFilter>();
    if (autoFilter)
    {
        autoFilter->SetReference(StringValue(range.ToA1()));
        for (const auto& filter : autoFilter->Elements<Spreadsheet::FilterColumn>())
        {
            if (filter->GetColumnId().ValueOr(MaxColumnIndex) >= range.ColumnCount())
            {
                if (!autoFilter->RemoveChild(filter))
                {
                    return m_part->SetXmlString(originalXml), false;
                }
            }
        }
    }
    return true;
}

bool ExcelTable::SetAutoFilterEnabled(bool enabled)
{
    const auto table = m_part ? m_part->GetTable() : nullptr;
    const auto range = Range();
    if (!table || !range)
    {
        return false;
    }
    const auto filter = table->GetFirstChildOfType<Spreadsheet::AutoFilter>();
    if (!enabled)
    {
        return !filter || table->RemoveChild(filter);
    }
    const auto target =
        filter ? filter
               : table->InsertChild<Spreadsheet::AutoFilter>(
                     table->GetFirstChildOfType<Spreadsheet::TableColumns>());
    if (!target)
    {
        return false;
    }
    target->SetReference(StringValue(range->ToA1()));
    return true;
}

std::vector<ExcelTableValueFilter> ExcelTable::ValueFilters() const
{
    std::vector<ExcelTableValueFilter> result;
    const auto table = m_part ? m_part->GetTable() : nullptr;
    const auto autoFilter =
        table ? table->GetFirstChildOfType<Spreadsheet::AutoFilter>() : nullptr;
    for (const auto& filterColumn :
         autoFilter ? autoFilter->Elements<Spreadsheet::FilterColumn>()
                    : std::vector<std::shared_ptr<Spreadsheet::FilterColumn>>{})
    {
        const auto filters =
            filterColumn->GetFirstChildOfType<Spreadsheet::Filters>();
        if (!filters)
        {
            continue;
        }
        ExcelTableValueFilter model;
        model.ColumnIndex = filterColumn->GetColumnId().ValueOr(0);
        model.IncludeBlank = filters->GetBlank().ValueOr(false);
        for (const auto& value : filters->Elements<Spreadsheet::Filter>())
        {
            model.Values.push_back(value->GetVal().ToString());
        }
        result.push_back(std::move(model));
    }
    std::sort(result.begin(), result.end(),
              [](const auto& left, const auto& right)
              {
                  return left.ColumnIndex < right.ColumnIndex;
              });
    return result;
}

bool ExcelTable::SetValueFilter(const ExcelTableValueFilter& filter)
{
    const auto table = m_part ? m_part->GetTable() : nullptr;
    const auto range = Range();
    if (!table || !range || filter.ColumnIndex >= range->ColumnCount() ||
        (filter.Values.empty() && !filter.IncludeBlank))
    {
        return false;
    }
    std::unordered_set<std::string> uniqueValues;
    for (const auto& value : filter.Values)
    {
        if (!uniqueValues.insert(value).second)
        {
            return false;
        }
    }
    if (!SetAutoFilterEnabled(true))
    {
        return false;
    }
    const auto autoFilter = table->GetFirstChildOfType<Spreadsheet::AutoFilter>();
    const auto originalXml = m_part->GetXmlString();
    std::shared_ptr<Spreadsheet::FilterColumn> target;
    for (const auto& candidate :
         autoFilter->Elements<Spreadsheet::FilterColumn>())
    {
        if (candidate->GetColumnId().ValueOr(MaxColumnIndex) == filter.ColumnIndex)
        {
            target = candidate;
        }
    }
    if (target)
    {
        autoFilter->RemoveChild(target);
    }
    target = autoFilter->AppendChild<Spreadsheet::FilterColumn>();
    const auto filters =
        target ? target->AppendChild<Spreadsheet::Filters>() : nullptr;
    if (!target || !filters)
    {
        return m_part->SetXmlString(originalXml), false;
    }
    target->SetColumnId(UInt32Value(filter.ColumnIndex));
    if (filter.IncludeBlank)
    {
        filters->SetBlank(BooleanValue(true));
    }
    for (const auto& value : filter.Values)
    {
        const auto xmlValue = filters->AppendChild<Spreadsheet::Filter>();
        if (!xmlValue)
        {
            return m_part->SetXmlString(originalXml), false;
        }
        xmlValue->SetVal(StringValue(value));
    }
    return true;
}

bool ExcelTable::RemoveValueFilter(UInt32 columnIndex)
{
    const auto table = m_part ? m_part->GetTable() : nullptr;
    const auto autoFilter =
        table ? table->GetFirstChildOfType<Spreadsheet::AutoFilter>() : nullptr;
    if (!autoFilter)
    {
        return false;
    }
    for (const auto& candidate :
         autoFilter->Elements<Spreadsheet::FilterColumn>())
    {
        if (candidate->GetColumnId().ValueOr(MaxColumnIndex) == columnIndex)
        {
            return autoFilter->RemoveChild(candidate);
        }
    }
    return false;
}

bool ExcelTable::ClearValueFilters()
{
    const auto table = m_part ? m_part->GetTable() : nullptr;
    const auto autoFilter =
        table ? table->GetFirstChildOfType<Spreadsheet::AutoFilter>() : nullptr;
    if (!autoFilter)
    {
        return false;
    }
    for (const auto& filter : autoFilter->Elements<Spreadsheet::FilterColumn>())
    {
        if (!autoFilter->RemoveChild(filter))
        {
            return false;
        }
    }
    return true;
}

bool ExcelTable::SetTotalsRowShown(bool shown)
{
    const auto table = m_part ? m_part->GetTable() : nullptr;
    const auto range = Range();
    if (!table || !range || (shown && range->RowCount() < 2))
    {
        return false;
    }
    table->SetTotalsRowShown(BooleanValue(shown));
    table->SetTotalsRowCount(UInt32Value(shown ? 1 : 0));
    return true;
}

std::shared_ptr<Packaging::TableDefinitionPart> ExcelTable::GetPart() const
{
    return m_part;
}

} // namespace ExyokiOffice::Excel
