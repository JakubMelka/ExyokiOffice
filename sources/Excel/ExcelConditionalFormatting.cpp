// Copyright (c) 2026 Jakub Melka and Collaborators
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "ExyokiOffice/Excel/ExcelConditionalFormatting.hpp"

#include "ExyokiOffice/DOM/DocumentFormat/OpenXml/Spreadsheet.hpp"
#include "ExyokiOffice/StandardTypes.hpp"

#include <unordered_set>

namespace ExyokiOffice::Excel
{
namespace Spreadsheet = ExyokiOffice::DocumentFormat::OpenXml::Spreadsheet;

class ConditionalFormattingModelHelpers final
{
public:
    static ExcelConditionalFormattingDefinition Rule(ConditionalFormattingType type, std::vector<CellRange> ranges)
    {
        ExcelConditionalFormattingDefinition result;
        result.Type = type;
        result.Ranges = std::move(ranges);
        return result;
    }
    static ExcelConditionalFormattingDefinition TextRule(ConditionalFormattingType type, std::vector<CellRange> ranges,
                                                         std::string text)
    {
        auto result = Rule(type, std::move(ranges));
        result.Text = std::move(text);
        return result;
    }
    static ConditionalFormattingType Type(Spreadsheet::ConditionalFormatValues::Value value)
    {
        using X = Spreadsheet::ConditionalFormatValues;
        switch (value)
        {
            case X::CellIs:
                return ConditionalFormattingType::CellIs;
            case X::UniqueValues:
                return ConditionalFormattingType::UniqueValues;
            case X::DuplicateValues:
                return ConditionalFormattingType::DuplicateValues;
            case X::ContainsText:
                return ConditionalFormattingType::ContainsText;
            case X::NotContainsText:
                return ConditionalFormattingType::NotContainsText;
            case X::BeginsWith:
                return ConditionalFormattingType::BeginsWith;
            case X::EndsWith:
                return ConditionalFormattingType::EndsWith;
            case X::ContainsBlanks:
                return ConditionalFormattingType::ContainsBlanks;
            case X::NotContainsBlanks:
                return ConditionalFormattingType::NotContainsBlanks;
            case X::ContainsErrors:
                return ConditionalFormattingType::ContainsErrors;
            case X::NotContainsErrors:
                return ConditionalFormattingType::NotContainsErrors;
            case X::Top10:
                return ConditionalFormattingType::Top;
            case X::AboveAverage:
                return ConditionalFormattingType::AboveAverage;
            default:
                return ConditionalFormattingType::Expression;
        }
    }
    static ConditionalFormattingOperator Op(Spreadsheet::ConditionalFormattingOperatorValues::Value value)
    {
        using X = Spreadsheet::ConditionalFormattingOperatorValues;
        switch (value)
        {
            case X::LessThan:
                return ConditionalFormattingOperator::LessThan;
            case X::LessThanOrEqual:
                return ConditionalFormattingOperator::LessThanOrEqual;
            case X::NotEqual:
                return ConditionalFormattingOperator::NotEqual;
            case X::GreaterThanOrEqual:
                return ConditionalFormattingOperator::GreaterThanOrEqual;
            case X::GreaterThan:
                return ConditionalFormattingOperator::GreaterThan;
            case X::Between:
                return ConditionalFormattingOperator::Between;
            case X::NotBetween:
                return ConditionalFormattingOperator::NotBetween;
            default:
                return ConditionalFormattingOperator::Equal;
        }
    }
};

ExcelConditionalFormattingDefinition ExcelConditionalFormattingDefinition::Expression(std::vector<CellRange> ranges,
                                                                                      std::string formula)
{
    auto result = ConditionalFormattingModelHelpers::Rule(ConditionalFormattingType::Expression, std::move(ranges));
    result.Formulas.push_back(std::move(formula));
    return result;
}

ExcelConditionalFormattingDefinition ExcelConditionalFormattingDefinition::CellIs(
    std::vector<CellRange> ranges, ConditionalFormattingOperator operation, std::string formula)
{
    auto result = ConditionalFormattingModelHelpers::Rule(ConditionalFormattingType::CellIs, std::move(ranges));
    result.Operation = operation;
    result.Formulas.push_back(std::move(formula));
    return result;
}

ExcelConditionalFormattingDefinition ExcelConditionalFormattingDefinition::Between(std::vector<CellRange> ranges,
                                                                                   std::string lower, std::string upper)
{
    auto result = CellIs(std::move(ranges), ConditionalFormattingOperator::Between, std::move(lower));
    result.Formulas.push_back(std::move(upper));
    return result;
}

ExcelConditionalFormattingDefinition ExcelConditionalFormattingDefinition::NotBetween(std::vector<CellRange> ranges,
                                                                                      std::string lower,
                                                                                      std::string upper)
{
    auto result = CellIs(std::move(ranges), ConditionalFormattingOperator::NotBetween, std::move(lower));
    result.Formulas.push_back(std::move(upper));
    return result;
}

ExcelConditionalFormattingDefinition ExcelConditionalFormattingDefinition::ContainsText(std::vector<CellRange> ranges,
                                                                                        std::string text)
{
    return ConditionalFormattingModelHelpers::TextRule(ConditionalFormattingType::ContainsText, std::move(ranges),
                                                       std::move(text));
}

ExcelConditionalFormattingDefinition ExcelConditionalFormattingDefinition::NotContainsText(
    std::vector<CellRange> ranges, std::string text)
{
    return ConditionalFormattingModelHelpers::TextRule(ConditionalFormattingType::NotContainsText, std::move(ranges),
                                                       std::move(text));
}

ExcelConditionalFormattingDefinition ExcelConditionalFormattingDefinition::BeginsWith(std::vector<CellRange> ranges,
                                                                                      std::string text)
{
    return ConditionalFormattingModelHelpers::TextRule(ConditionalFormattingType::BeginsWith, std::move(ranges),
                                                       std::move(text));
}

ExcelConditionalFormattingDefinition ExcelConditionalFormattingDefinition::EndsWith(std::vector<CellRange> ranges,
                                                                                    std::string text)
{
    return ConditionalFormattingModelHelpers::TextRule(ConditionalFormattingType::EndsWith, std::move(ranges),
                                                       std::move(text));
}

ExcelConditionalFormattingDefinition ExcelConditionalFormattingDefinition::UniqueValues(std::vector<CellRange> ranges)
{
    return ConditionalFormattingModelHelpers::Rule(ConditionalFormattingType::UniqueValues, std::move(ranges));
}

ExcelConditionalFormattingDefinition ExcelConditionalFormattingDefinition::DuplicateValues(
    std::vector<CellRange> ranges)
{
    return ConditionalFormattingModelHelpers::Rule(ConditionalFormattingType::DuplicateValues, std::move(ranges));
}

ExcelConditionalFormattingDefinition ExcelConditionalFormattingDefinition::ContainsBlanks(std::vector<CellRange> ranges)
{
    return ConditionalFormattingModelHelpers::Rule(ConditionalFormattingType::ContainsBlanks, std::move(ranges));
}

ExcelConditionalFormattingDefinition ExcelConditionalFormattingDefinition::NotContainsBlanks(
    std::vector<CellRange> ranges)
{
    return ConditionalFormattingModelHelpers::Rule(ConditionalFormattingType::NotContainsBlanks, std::move(ranges));
}

ExcelConditionalFormattingDefinition ExcelConditionalFormattingDefinition::ContainsErrors(std::vector<CellRange> ranges)
{
    return ConditionalFormattingModelHelpers::Rule(ConditionalFormattingType::ContainsErrors, std::move(ranges));
}

ExcelConditionalFormattingDefinition ExcelConditionalFormattingDefinition::NotContainsErrors(
    std::vector<CellRange> ranges)
{
    return ConditionalFormattingModelHelpers::Rule(ConditionalFormattingType::NotContainsErrors, std::move(ranges));
}

ExcelConditionalFormattingDefinition ExcelConditionalFormattingDefinition::Top(std::vector<CellRange> ranges,
                                                                               UInt32 rank, bool percent)
{
    auto result = ConditionalFormattingModelHelpers::Rule(ConditionalFormattingType::Top, std::move(ranges));
    result.Rank = rank;
    result.Percent = percent;
    return result;
}

ExcelConditionalFormattingDefinition ExcelConditionalFormattingDefinition::Bottom(std::vector<CellRange> ranges,
                                                                                  UInt32 rank, bool percent)
{
    auto result = Top(std::move(ranges), rank, percent);
    result.IsBottom = true;
    return result;
}

ExcelConditionalFormattingDefinition ExcelConditionalFormattingDefinition::AboveAverage(
    std::vector<CellRange> ranges, bool includeEqual, std::optional<Int32> deviation)
{
    auto result = ConditionalFormattingModelHelpers::Rule(ConditionalFormattingType::AboveAverage, std::move(ranges));
    result.EqualAverage = includeEqual;
    result.StandardDeviation = deviation;
    return result;
}

ExcelConditionalFormattingDefinition ExcelConditionalFormattingDefinition::BelowAverage(
    std::vector<CellRange> ranges, bool includeEqual, std::optional<Int32> deviation)
{
    auto result = AboveAverage(std::move(ranges), includeEqual, deviation);
    result.IsAboveAverage = false;
    return result;
}

bool IsValidExcelConditionalFormatting(const ExcelConditionalFormattingDefinition& d)
{
    if (d.Ranges.empty())
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
    for (const auto& formula : d.Formulas)
    {
        if (formula.empty())
        {
            return false;
        }
    }
    if (d.StandardDeviation && *d.StandardDeviation < 0)
    {
        return false;
    }
    const auto textRule =
        d.Type == ConditionalFormattingType::ContainsText || d.Type == ConditionalFormattingType::NotContainsText ||
        d.Type == ConditionalFormattingType::BeginsWith || d.Type == ConditionalFormattingType::EndsWith;
    if (d.Type == ConditionalFormattingType::Expression)
    {
        return d.Formulas.size() == 1 && !d.Operation && !d.Text;
    }
    if (d.Type == ConditionalFormattingType::CellIs)
    {
        if (!d.Operation || d.Text)
        {
            return false;
        }
        const bool pair = *d.Operation == ConditionalFormattingOperator::Between ||
                          *d.Operation == ConditionalFormattingOperator::NotBetween;
        return d.Formulas.size() == (pair ? 2u : 1u);
    }
    if (textRule)
    {
        return d.Text && !d.Text->empty() && d.Formulas.empty() && !d.Operation;
    }
    if (!d.Formulas.empty() || d.Operation || d.Text)
    {
        return false;
    }
    if (d.Type == ConditionalFormattingType::Top)
    {
        return d.Rank > 0 && !d.StandardDeviation;
    }
    if (d.Type == ConditionalFormattingType::AboveAverage)
    {
        return !d.Percent && !d.IsBottom;
    }
    return !d.StandardDeviation;
}

ExcelConditionalFormatting::ExcelConditionalFormatting(std::shared_ptr<Spreadsheet::ConditionalFormatting> container,
                                                       std::shared_ptr<Spreadsheet::ConditionalFormattingRule> rule,
                                                       std::shared_ptr<Packaging::WorksheetPart> owner)
    : m_container(std::move(container)), m_rule(std::move(rule)), m_owner(std::move(owner))
{
}

std::shared_ptr<Spreadsheet::ConditionalFormattingRule> ExcelConditionalFormatting::GetLowLevelApi() const
{
    return m_rule;
}
UInt32 ExcelConditionalFormatting::Priority() const
{
    return m_rule ? static_cast<UInt32>(m_rule->GetPriority().ValueOr(0)) : 0;
}
ExcelConditionalFormattingDefinition ExcelConditionalFormatting::Definition() const
{
    ExcelConditionalFormattingDefinition d;
    if (!m_rule || !m_container)
    {
        return d;
    }
    d.Type = ConditionalFormattingModelHelpers::Type(
        m_rule->GetType().ValueOr(Spreadsheet::ConditionalFormatValues()).GetValue());
    d.StopIfTrue = m_rule->GetStopIfTrue().ValueOr(false);
    if (m_rule->GetFormatId().IsDefined())
    {
        d.DifferentialFormatId = m_rule->GetFormatId().Value();
    }
    if (m_rule->GetOperator().IsDefined())
    {
        d.Operation = ConditionalFormattingModelHelpers::Op(m_rule->GetOperator().Value().GetValue());
    }
    if (m_rule->GetText().IsDefined())
    {
        d.Text = m_rule->GetText().ToString();
    }
    d.Rank = m_rule->GetRank().ValueOr(10);
    d.Percent = m_rule->GetPercent().ValueOr(false);
    d.IsBottom = m_rule->GetBottom().ValueOr(false);
    d.IsAboveAverage = m_rule->GetAboveAverage().ValueOr(true);
    d.EqualAverage = m_rule->GetEqualAverage().ValueOr(false);
    if (m_rule->GetStdDev().IsDefined())
    {
        d.StandardDeviation = m_rule->GetStdDev().Value();
    }
    for (const auto& formula : m_rule->Elements<Spreadsheet::Formula>())
    {
        d.Formulas.emplace_back(formula->GetText());
    }
    const auto refs = m_container->GetSequenceOfReferences().ToString();
    Size begin = 0;
    while (begin < refs.size())
    {
        const auto end = refs.find(' ', begin);
        const auto token = std::string_view(refs).substr(begin, end == std::string::npos ? end : end - begin);
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
