// Copyright (c) 2026 Jakub Melka and Contributors
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#pragma once

#include "ExyokiOffice/Excel/ExcelAddress.hpp"
#include "ExyokiOffice/Export.hpp"
#include "ExyokiOffice/StandardTypes.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace ExyokiOffice::DocumentFormat::OpenXml::Spreadsheet
{
class ConditionalFormatting;
class ConditionalFormattingRule;
} // namespace ExyokiOffice::DocumentFormat::OpenXml::Spreadsheet
namespace ExyokiOffice::Packaging
{
class WorksheetPart;
}

namespace ExyokiOffice::Excel
{

/** @brief Non-visual SpreadsheetML conditional-format rule category. */
enum class ConditionalFormattingType
{
    Expression,
    CellIs,
    UniqueValues,
    DuplicateValues,
    ContainsText,
    NotContainsText,
    BeginsWith,
    EndsWith,
    ContainsBlanks,
    NotContainsBlanks,
    ContainsErrors,
    NotContainsErrors,
    Top,
    AboveAverage
};

/** @brief Comparison used by a CellIs conditional-format rule. */
enum class ConditionalFormattingOperator
{
    LessThan,
    LessThanOrEqual,
    Equal,
    NotEqual,
    GreaterThanOrEqual,
    GreaterThan,
    Between,
    NotBetween
};

/**
 * @brief Complete editable definition of a common conditional-format rule.
 *
 * Ranges are non-empty, valid,
 * distinct worksheet rectangles. Formula strings
 * are stored verbatim and are never evaluated. Expression requires
 * one
 * formula. CellIs requires an operator and one formula, or two for Between and
 * NotBetween. Text rules require
 * non-empty `text`; the remaining rule types do
 * not accept formulas, an operator, or text.
 *
 *
 * `differentialFormatId` refers to an existing workbook dxfs entry; this API
 * preserves the reference but does not
 * create or visually evaluate styles.
 * Top uses `rank`, with `percent` and `bottom` selecting its variants.
 * AboveAverage
 * uses `aboveAverage`, `equalAverage`, and an optional non-negative `standardDeviation`.
 * Priority is
 * managed by Worksheet and is therefore intentionally absent.
 */
struct EXYOKIOFFICE_EXPORT ExcelConditionalFormattingDefinition
{
    /** @brief SpreadsheetML rule category controlling interpretation of all other fields. */
    ConditionalFormattingType Type = ConditionalFormattingType::Expression;
    /** @brief Non-empty set of distinct worksheet rectangles to which the rule applies. */
    std::vector<CellRange> Ranges;
    /** @brief Verbatim formula operands, without calculation or automatic `=` insertion. */
    std::vector<std::string> Formulas;
    /** @brief Comparison operator; defined only for CellIs rules. */
    std::optional<ConditionalFormattingOperator> Operation;
    /** @brief Search text required by contains, not-contains, begins-with, and ends-with rules. */
    std::optional<std::string> Text;
    /** @brief Optional zero-based reference into the workbook differential-formats collection. */
    std::optional<UInt32> DifferentialFormatId;
    /** @brief Whether evaluation stops after this rule evaluates to true. */
    bool StopIfTrue = false;
    /** @brief Positive item count or percentage used only by Top rules. */
    UInt32 Rank = 10;
    /** @brief Whether a Top rule's rank is a percentage rather than an item count. */
    bool Percent = false;
    /** @brief Whether a Top rule selects bottom rather than top values. */
    bool IsBottom = false;
    /** @brief Whether an AboveAverage rule selects above rather than below average values. */
    bool IsAboveAverage = true;
    /** @brief Whether values equal to the average or boundary are included. */
    bool EqualAverage = false;
    /** @brief Optional non-negative standard-deviation offset for AboveAverage rules. */
    std::optional<Int32> StandardDeviation;

    /** @brief Creates a formula-based rule. Formula text is stored verbatim. */
    static ExcelConditionalFormattingDefinition Expression(std::vector<CellRange> ranges, std::string formula);
    /** @brief Creates a one-operand cell comparison rule. Between operators are rejected by validation. */
    static ExcelConditionalFormattingDefinition CellIs(std::vector<CellRange> ranges,
                                                       ConditionalFormattingOperator operation, std::string formula);
    /** @brief Creates an inclusive between comparison with lower and upper formula operands. */
    static ExcelConditionalFormattingDefinition Between(std::vector<CellRange> ranges, std::string lowerFormula,
                                                        std::string upperFormula);
    /** @brief Creates a not-between comparison with lower and upper formula operands. */
    static ExcelConditionalFormattingDefinition NotBetween(std::vector<CellRange> ranges, std::string lowerFormula,
                                                           std::string upperFormula);
    /** @brief Creates a rule matching cells that contain the supplied non-empty text. */
    static ExcelConditionalFormattingDefinition ContainsText(std::vector<CellRange> ranges, std::string text);
    /** @brief Creates a rule matching cells that do not contain the supplied text. */
    static ExcelConditionalFormattingDefinition NotContainsText(std::vector<CellRange> ranges, std::string text);
    /** @brief Creates a rule matching cells whose text begins with the supplied text. */
    static ExcelConditionalFormattingDefinition BeginsWith(std::vector<CellRange> ranges, std::string text);
    /** @brief Creates a rule matching cells whose text ends with the supplied text. */
    static ExcelConditionalFormattingDefinition EndsWith(std::vector<CellRange> ranges, std::string text);
    /** @brief Creates a rule matching values that occur exactly once in the target ranges. */
    static ExcelConditionalFormattingDefinition UniqueValues(std::vector<CellRange> ranges);
    /** @brief Creates a rule matching values that occur more than once in the target ranges. */
    static ExcelConditionalFormattingDefinition DuplicateValues(std::vector<CellRange> ranges);
    /** @brief Creates a rule matching blank cells. */
    static ExcelConditionalFormattingDefinition ContainsBlanks(std::vector<CellRange> ranges);
    /** @brief Creates a rule matching non-blank cells. */
    static ExcelConditionalFormattingDefinition NotContainsBlanks(std::vector<CellRange> ranges);
    /** @brief Creates a rule matching cells containing errors. */
    static ExcelConditionalFormattingDefinition ContainsErrors(std::vector<CellRange> ranges);
    /** @brief Creates a rule matching cells that do not contain errors. */
    static ExcelConditionalFormattingDefinition NotContainsErrors(std::vector<CellRange> ranges);
    /** @brief Creates a top item-count or percentage rule. Rank must be positive. */
    static ExcelConditionalFormattingDefinition Top(std::vector<CellRange> ranges, UInt32 rank,
                                                    bool percent = false);
    /** @brief Creates a bottom item-count or percentage rule. Rank must be positive. */
    static ExcelConditionalFormattingDefinition Bottom(std::vector<CellRange> ranges, UInt32 rank,
                                                       bool percent = false);
    /** @brief Creates an above-average rule with an optional standard-deviation boundary. */
    static ExcelConditionalFormattingDefinition AboveAverage(
        std::vector<CellRange> ranges, bool includeEqual = false,
        std::optional<Int32> standardDeviation = std::nullopt);
    /** @brief Creates a below-average rule with an optional standard-deviation boundary. */
    static ExcelConditionalFormattingDefinition BelowAverage(
        std::vector<CellRange> ranges, bool includeEqual = false,
        std::optional<Int32> standardDeviation = std::nullopt);
};

/** @brief Handle to one conditional-format rule owned by a worksheet. */
class EXYOKIOFFICE_EXPORT ExcelConditionalFormatting
{
public:
    using Ptr = std::shared_ptr<ExcelConditionalFormatting>;
    /** @brief Reads the complete high-level definition. */
    ExcelConditionalFormattingDefinition Definition() const;
    /** @brief Returns the current one-based evaluation priority. */
    UInt32 Priority() const;
    /** @brief Returns the generated rule element for advanced access. */
    std::shared_ptr<DocumentFormat::OpenXml::Spreadsheet::ConditionalFormattingRule> GetLowLevelApi() const;

private:
    friend class Worksheet;
    ExcelConditionalFormatting(std::shared_ptr<DocumentFormat::OpenXml::Spreadsheet::ConditionalFormatting> container,
                               std::shared_ptr<DocumentFormat::OpenXml::Spreadsheet::ConditionalFormattingRule> rule,
                               std::shared_ptr<Packaging::WorksheetPart> owner);
    std::shared_ptr<DocumentFormat::OpenXml::Spreadsheet::ConditionalFormatting> m_container;
    std::shared_ptr<DocumentFormat::OpenXml::Spreadsheet::ConditionalFormattingRule> m_rule;
    std::weak_ptr<Packaging::WorksheetPart> m_owner;
};

/** @brief Validates a definition without changing a worksheet. */
[[nodiscard]] EXYOKIOFFICE_EXPORT bool IsValidExcelConditionalFormatting(const ExcelConditionalFormattingDefinition& definition);

} // namespace ExyokiOffice::Excel
