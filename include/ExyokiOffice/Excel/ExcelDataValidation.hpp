// Copyright (c) 2026 Jakub Melka and Contributors
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#pragma once

#include "ExyokiOffice/Excel/ExcelAddress.hpp"
#include "ExyokiOffice/Export.hpp"

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace ExyokiOffice::DocumentFormat::OpenXml::Spreadsheet
{
class DataValidation;
}
namespace ExyokiOffice::Packaging
{
class WorksheetPart;
}

namespace ExyokiOffice::Excel
{

/** @brief SpreadsheetML validation rule category. */
enum class DataValidationType
{
    /** No value restriction; useful only for input/error message metadata. */ None,
    /** Whole-number comparison. */ Whole,
    /** Decimal-number comparison. */ Decimal,
    /** Membership in an inline or formula-backed list. */ List,
    /** Date serial comparison. */ Date,
    /** Time serial comparison. */ Time,
    /** Text-length comparison. */ TextLength,
    /** Formula whose result determines validity. */ Custom
};

/** @brief Comparison applied by scalar data validation rules. */
enum class DataValidationOperator
{
    /** Value lies inclusively between two bounds. */ Between,
    /** Value lies outside two inclusive bounds. */ NotBetween,
    /** Value equals the first formula result. */ Equal,
    /** Value differs from the first formula result. */ NotEqual,
    /** Value is lower than the first formula result. */ LessThan,
    /** Value is no greater than the first formula result. */ LessThanOrEqual,
    /** Value is higher than the first formula result. */ GreaterThan,
    /** Value is no lower than the first formula result. */ GreaterThanOrEqual
};

/** @brief User-interface severity used when invalid input is entered. */
enum class DataValidationErrorStyle
{
    /** Reject invalid input until it is corrected or cancelled. */ Stop,
    /** Warn while allowing the user to retain invalid input. */ Warning,
    /** Inform while allowing the user to retain invalid input. */ Information
};

/**
 * @brief Complete editable definition of one worksheet data validation rule.
 *
 * Formula text is stored verbatim, without evaluation and without requiring a
 * leading equals sign. `formula2` is required only by Between and NotBetween.
 * List and Custom rules require `formula1` and do not accept an operator.
 * None rules accept no formulas or operator. All other types require
 * `formula1`; their operator defaults to Between when omitted.
 *
 * Ranges must be valid, non-duplicated worksheet rectangles. Prompt and error
 * titles are limited to 32 characters and message bodies to 255 characters,
 * matching Excel's interoperable limits.
 */
struct EXYOKIOFFICE_EXPORT ExcelDataValidationDefinition
{
    DataValidationType Type = DataValidationType::None;
    std::optional<DataValidationOperator> Operation;
    std::optional<std::string> Formula1;
    std::optional<std::string> Formula2;
    std::vector<CellRange> Ranges;
    bool AllowBlank = false;
    bool ShowDropDown = true;
    bool ShowInputMessage = false;
    bool ShowErrorMessage = false;
    DataValidationErrorStyle ErrorStyle = DataValidationErrorStyle::Stop;
    std::optional<std::string> PromptTitle;
    std::optional<std::string> Prompt;
    std::optional<std::string> ErrorTitle;
    std::optional<std::string> Error;
};

/**
 * @brief High-level non-owning handle to one worksheet validation element.
 *
 * The handle keeps the XML element alive, but mutations should be performed
 * through Worksheet::UpdateDataValidation(). That method verifies worksheet
 * ownership and applies a completely validated definition atomically.
 */
class EXYOKIOFFICE_EXPORT ExcelDataValidation
{
public:
    using Ptr = std::shared_ptr<ExcelDataValidation>;
    /** @brief Wraps an existing SpreadsheetML validation element. */
    explicit ExcelDataValidation(std::shared_ptr<DocumentFormat::OpenXml::Spreadsheet::DataValidation> element);
    /** @brief Reads the complete high-level definition. */
    ExcelDataValidationDefinition Definition() const;
    /** @brief Returns the underlying typed DOM element for advanced access. */
    std::shared_ptr<DocumentFormat::OpenXml::Spreadsheet::DataValidation> GetLowLevelApi() const;

private:
    friend class Worksheet;
    ExcelDataValidation(std::shared_ptr<DocumentFormat::OpenXml::Spreadsheet::DataValidation> element,
                        std::shared_ptr<Packaging::WorksheetPart> owner);
    std::shared_ptr<DocumentFormat::OpenXml::Spreadsheet::DataValidation> m_element;
    std::weak_ptr<Packaging::WorksheetPart> m_owner;
};

/** @brief Validates a definition without modifying a worksheet. */
[[nodiscard]] EXYOKIOFFICE_EXPORT bool IsValidExcelDataValidation(const ExcelDataValidationDefinition& definition);

} // namespace ExyokiOffice::Excel
