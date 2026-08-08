// Copyright (c) 2026 Jakub Melka and Collaborators
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "ExyokiOffice/Excel/ExcelDataValidation.hpp"

#include "ExyokiOffice/DOM/DocumentFormat/OpenXml/Spreadsheet.hpp"
#include "ExyokiOffice/StandardTypes.hpp"

#include <unordered_set>

namespace ExyokiOffice::Excel
{
namespace Spreadsheet = ExyokiOffice::DocumentFormat::OpenXml::Spreadsheet;

class DataValidationHelpers final
{
public:
    static Spreadsheet::DataValidationValues Type(DataValidationType value)
    {
        using X = Spreadsheet::DataValidationValues;
        switch (value)
        {
            case DataValidationType::None:
                return X::None;
            case DataValidationType::Whole:
                return X::Whole;
            case DataValidationType::Decimal:
                return X::Decimal;
            case DataValidationType::List:
                return X::List;
            case DataValidationType::Date:
                return X::Date;
            case DataValidationType::Time:
                return X::Time;
            case DataValidationType::TextLength:
                return X::TextLength;
            case DataValidationType::Custom:
                return X::Custom;
        }
        return X::None;
    }
    static DataValidationType Type(Spreadsheet::DataValidationValues::Value value)
    {
        using X = Spreadsheet::DataValidationValues;
        switch (value)
        {
            case X::Whole:
                return DataValidationType::Whole;
            case X::Decimal:
                return DataValidationType::Decimal;
            case X::List:
                return DataValidationType::List;
            case X::Date:
                return DataValidationType::Date;
            case X::Time:
                return DataValidationType::Time;
            case X::TextLength:
                return DataValidationType::TextLength;
            case X::Custom:
                return DataValidationType::Custom;
            default:
                return DataValidationType::None;
        }
    }
    static Spreadsheet::DataValidationOperatorValues Op(DataValidationOperator value)
    {
        using X = Spreadsheet::DataValidationOperatorValues;
        switch (value)
        {
            case DataValidationOperator::Between:
                return X::Between;
            case DataValidationOperator::NotBetween:
                return X::NotBetween;
            case DataValidationOperator::Equal:
                return X::Equal;
            case DataValidationOperator::NotEqual:
                return X::NotEqual;
            case DataValidationOperator::LessThan:
                return X::LessThan;
            case DataValidationOperator::LessThanOrEqual:
                return X::LessThanOrEqual;
            case DataValidationOperator::GreaterThan:
                return X::GreaterThan;
            case DataValidationOperator::GreaterThanOrEqual:
                return X::GreaterThanOrEqual;
        }
        return X::Between;
    }
    static DataValidationOperator Op(Spreadsheet::DataValidationOperatorValues::Value value)
    {
        using X = Spreadsheet::DataValidationOperatorValues;
        switch (value)
        {
            case X::NotBetween:
                return DataValidationOperator::NotBetween;
            case X::Equal:
                return DataValidationOperator::Equal;
            case X::NotEqual:
                return DataValidationOperator::NotEqual;
            case X::LessThan:
                return DataValidationOperator::LessThan;
            case X::LessThanOrEqual:
                return DataValidationOperator::LessThanOrEqual;
            case X::GreaterThan:
                return DataValidationOperator::GreaterThan;
            case X::GreaterThanOrEqual:
                return DataValidationOperator::GreaterThanOrEqual;
            default:
                return DataValidationOperator::Between;
        }
    }
    static Spreadsheet::DataValidationErrorStyleValues Style(DataValidationErrorStyle value)
    {
        using X = Spreadsheet::DataValidationErrorStyleValues;
        return value == DataValidationErrorStyle::Warning ? X::Warning : value == DataValidationErrorStyle::Information ? X::Information
                                                                                                                        : X::Stop;
    }
    static DataValidationErrorStyle Style(Spreadsheet::DataValidationErrorStyleValues::Value value)
    {
        using X = Spreadsheet::DataValidationErrorStyleValues;
        return value == X::Warning ? DataValidationErrorStyle::Warning : value == X::Information ? DataValidationErrorStyle::Information
                                                                                                 : DataValidationErrorStyle::Stop;
    }
};

bool IsValidExcelDataValidation(const ExcelDataValidationDefinition& d)
{
    if (d.Ranges.empty() || (d.PromptTitle && d.PromptTitle->size() > 32) || (d.ErrorTitle && d.ErrorTitle->size() > 32) ||
        (d.Prompt && d.Prompt->size() > 255) || (d.Error && d.Error->size() > 255))
    {
        return false;
    }
    std::unordered_set<std::string> ranges;
    for (const auto& range : d.Ranges)
    {
        if (!range.IsValid() || !ranges.insert(range.ToA1()).second)
        {
            return false;
        }
    }
    if ((d.Formula1 && d.Formula1->empty()) || (d.Formula2 && d.Formula2->empty()))
    {
        return false;
    }
    if (d.Type == DataValidationType::None)
    {
        return !d.Operation && !d.Formula1 && !d.Formula2;
    }
    if (d.Type == DataValidationType::List || d.Type == DataValidationType::Custom)
    {
        return d.Formula1 && !d.Formula2 && !d.Operation;
    }
    if (!d.Formula1)
    {
        return false;
    }
    const auto op = d.Operation.value_or(DataValidationOperator::Between);
    const bool pair = op == DataValidationOperator::Between || op == DataValidationOperator::NotBetween;
    return pair == d.Formula2.has_value();
}

ExcelDataValidation::ExcelDataValidation(std::shared_ptr<Spreadsheet::DataValidation> element) : m_element(std::move(element)) {}
ExcelDataValidation::ExcelDataValidation(std::shared_ptr<Spreadsheet::DataValidation> element,
                                         std::shared_ptr<Packaging::WorksheetPart> owner)
    : m_element(std::move(element)), m_owner(std::move(owner)) {}
std::shared_ptr<Spreadsheet::DataValidation> ExcelDataValidation::GetLowLevelApi() const
{
    return m_element;
}

ExcelDataValidationDefinition ExcelDataValidation::Definition() const
{
    ExcelDataValidationDefinition d;
    if (!m_element)
    {
        return d;
    }
    d.Type = DataValidationHelpers::Type(m_element->GetType().ValueOr(Spreadsheet::DataValidationValues()).GetValue());
    const auto op = m_element->GetOperator();
    if (op.IsDefined())
    {
        d.Operation = DataValidationHelpers::Op(op.Value().GetValue());
    }
    d.AllowBlank = m_element->GetAllowBlank().ValueOr(false);
    d.ShowDropDown = !m_element->GetShowDropDown().ValueOr(false);
    d.ShowInputMessage = m_element->GetShowInputMessage().ValueOr(false);
    d.ShowErrorMessage = m_element->GetShowErrorMessage().ValueOr(false);
    d.ErrorStyle = DataValidationHelpers::Style(m_element->GetErrorStyle().ValueOr(Spreadsheet::DataValidationErrorStyleValues()).GetValue());
    auto read = [&](auto value) -> std::optional<std::string>
    { return value.IsDefined() ? std::optional<std::string>(value.ToString()) : std::nullopt; };
    d.PromptTitle = read(m_element->GetPromptTitle());
    d.Prompt = read(m_element->GetPrompt());
    d.ErrorTitle = read(m_element->GetErrorTitle());
    d.Error = read(m_element->GetError());
    if (auto f = m_element->GetFirstChildOfType<Spreadsheet::Formula1>())
    {
        d.Formula1 = f->GetText();
    }
    if (auto f = m_element->GetFirstChildOfType<Spreadsheet::Formula2>())
    {
        d.Formula2 = f->GetText();
    }
    const auto references = m_element->GetSequenceOfReferences().ToString();
    Size begin = 0;
    while (begin < references.size())
    {
        const auto end = references.find(' ', begin);
        const auto token = std::string_view(references).substr(begin, end == std::string::npos ? end : end - begin);
        if (auto range = CellRange::ParseA1(token))
        {
            d.Ranges.push_back(*range);
        }
        if (end == std::string::npos)
        {
            break;
        }
        begin = end + 1;
    }
    return d;
}

} // namespace ExyokiOffice::Excel
