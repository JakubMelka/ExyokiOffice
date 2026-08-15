// Copyright (c) 2026 Jakub Melka and Contributors
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#pragma once

#include "ExyokiOffice/Export.hpp"
#include "ExyokiOffice/StandardTypes.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace ExyokiOffice::Excel
{

/**
 * @brief High-level category of a SpreadsheetML cell payload.
 *
 * The enum describes the semantic value carried by a worksheet cell without
 * exposing the low-level SpreadsheetML `t`, `f`, `v`, and `is` elements. Formula
 * cells are represented separately because they combine a formula expression
 * with an optional cached result.
 */
enum class CellValueKind
{
    Blank,
    SharedString,
    InlineString,
    Number,
    Boolean,
    Error,
    DateTime,
    Formula
};

/**
 * @brief Cached result type stored next to a formula.
 *
 * Spreadsheet applications may keep the last calculated value in the formula
 * cell's `v` element. ExyokiOffice preserves the cached value as text and
 * records its type; it does not evaluate formulas.
 */
enum class FormulaCachedValueKind
{
    None,
    SharedString,
    String,
    Number,
    Boolean,
    Error,
    DateTime
};

/** @brief Workbook-wide syntax used by formula references. */
enum class FormulaReferenceStyle
{
    /** References use column letters followed by row numbers, for example `B4`. */
    A1,
    /** References use row and column coordinates, for example `R4C2`. */
    R1C1
};

/** @brief SpreadsheetML storage category of a cell formula. */
enum class CellFormulaKind
{
    /** An ordinary formula stored independently in one cell. */
    Normal,
    /** A member of a shared-formula group identified by sharedIndex. */
    Shared,
    /** An array formula whose anchor owns a rectangular result range. */
    Array
};

/**
 * @brief Formula text and optional cached result for a cell.
 *
 * Formula text is stored without the leading `=` character, matching
 * SpreadsheetML. The cached result is optional and kept exactly as text so an
 * open-save-open round-trip does not alter number formatting, error literals,
 * date serials, or cached string data.
 */
struct EXYOKIOFFICE_EXPORT CellFormulaValue
{
    /** Formula expression without the leading equals sign. */
    std::string Formula;
    /** Workbook reference syntax in effect for the expression. */
    FormulaReferenceStyle ReferenceStyle = FormulaReferenceStyle::A1;
    /** Formula storage category. */
    CellFormulaKind Kind = CellFormulaKind::Normal;
    /**
     * SpreadsheetML `ref` value.
     *
     * Shared-formula anchors and array formulas normally provide a range.
     * Shared dependent cells leave it empty.
     */
    std::optional<std::string> Reference;
    /** Shared-formula group index; required when kind is Shared. */
    std::optional<UInt32> SharedIndex;
    /** Requests recalculation of an array formula on every calculation pass. */
    bool AlwaysCalculateArray = false;
    /** Type of the optional cached result. */
    FormulaCachedValueKind CachedKind = FormulaCachedValueKind::None;
    /** Cached result exactly as stored in the cell value element. */
    std::string CachedText;

    /** @brief Creates an ordinary formula definition. */
    static CellFormulaValue Normal(std::string formula,
                                   FormulaCachedValueKind cachedKind = FormulaCachedValueKind::None,
                                   std::string cachedText = {},
                                   FormulaReferenceStyle referenceStyle = FormulaReferenceStyle::A1);
    /**
     * @brief Creates the anchor of a shared-formula group.
     * @param reference Rectangular A1 range occupied by the shared group.
     */
    static CellFormulaValue Shared(std::string formula,
                                   UInt32 sharedIndex,
                                   std::string reference,
                                   FormulaCachedValueKind cachedKind = FormulaCachedValueKind::None,
                                   std::string cachedText = {},
                                   FormulaReferenceStyle referenceStyle = FormulaReferenceStyle::A1);
    /** @brief Creates a dependent shared-formula cell whose expression is owned by its anchor. */
    static CellFormulaValue SharedDependent(
        UInt32 sharedIndex,
        FormulaCachedValueKind cachedKind = FormulaCachedValueKind::None,
        std::string cachedText = {},
        FormulaReferenceStyle referenceStyle = FormulaReferenceStyle::A1);
    /**
     * @brief Creates an array formula anchored in a rectangular result range.
     * @param reference Rectangular A1 range controlled by the anchor cell.
     */
    static CellFormulaValue Array(std::string formula,
                                  std::string reference,
                                  FormulaCachedValueKind cachedKind = FormulaCachedValueKind::None,
                                  std::string cachedText = {},
                                  bool alwaysCalculate = false,
                                  FormulaReferenceStyle referenceStyle = FormulaReferenceStyle::A1);
};

/**
 * @brief Typed value model for a SpreadsheetML cell.
 *
 * ExcelCellValue separates the public editing API from raw SpreadsheetML. It
 * covers blank cells, inline strings, shared-string indexes, numbers, booleans,
 * errors, ISO/date serial text, and formulas with cached results. Textual
 * payloads are deliberately preserved as strings where SpreadsheetML stores
 * text (`v`, `is/t`, and `f`) so callers can round-trip existing workbooks
 * without changing lexical representation.
 */
class EXYOKIOFFICE_EXPORT ExcelCellValue
{
public:
    /**
     * @brief Creates a blank cell value.
     */
    ExcelCellValue() = default;

    /**
     * @brief Creates a blank cell value.
     */
    static ExcelCellValue Blank();
    /**
     * @brief Creates an inline string cell.
     */
    static ExcelCellValue InlineString(std::string text);
    /**
     * @brief Creates a shared-string reference.
     *
     * The index refers to the workbook shared string table. Use
     * ExcelDocumentEditor::SharedStrings() to resolve, deduplicate, and clean
     * shared string entries.
     */
    static ExcelCellValue SharedString(UInt32 sharedStringIndex);
    /**
     * @brief Creates a numeric cell from already formatted SpreadsheetML text.
     */
    static ExcelCellValue NumberText(std::string text);
    /**
     * @brief Creates a numeric cell from a double using a stable round-trip format.
     */
    static ExcelCellValue Number(Real value);
    /**
     * @brief Creates a boolean cell.
     */
    static ExcelCellValue Boolean(bool value);
    /**
     * @brief Creates an error cell.
     *
     * The value should be an Excel error literal such as `#DIV/0!`, `#VALUE!`,
     * or `#N/A`.
     */
    static ExcelCellValue Error(std::string errorText);
    /**
     * @brief Creates a date/time cell from SpreadsheetML text.
     *
     * The text is written with cell type `d`. It may be an ISO 8601 value or any
     * lexical value the caller wants to preserve for a date cell.
     */
    static ExcelCellValue DateTimeText(std::string text);
    /**
     * @brief Creates a formula cell with an optional cached result.
     *
     * @param formula Formula text with or without a leading `=`.
     * @param cachedKind Type of the cached result.
     * @param cachedText Cached result text stored in the cell's `v` element.
     */
    static ExcelCellValue Formula(std::string formula,
                                  FormulaCachedValueKind cachedKind = FormulaCachedValueKind::None,
                                  std::string cachedText = {});
    /**
     * @brief Creates a formula cell from a complete SpreadsheetML formula model.
     *
     * This overload preserves shared and array metadata in addition to the
     * expression and cached result. Validation is performed when the value is
     * assigned to a worksheet because array ranges must contain their anchor.
     */
    static ExcelCellValue Formula(CellFormulaValue formula);

    /**
     * @brief Returns the semantic kind of the cell value.
     */
    CellValueKind Kind() const noexcept;
    /**
     * @brief Returns true when the value is blank.
     */
    [[nodiscard]] bool IsBlank() const noexcept;
    /**
     * @brief Returns the stored text payload.
     *
     * For inline strings this is the string text. For number, error, date/time,
     * and formula cached values this is the exact cell value text. For boolean
     * values this returns `1` or `0`.
     */
    const std::string& Text() const noexcept;
    /**
     * @brief Returns the shared string table index when Kind() is SharedString.
     */
    std::optional<UInt32> SharedStringIndex() const noexcept;
    /**
     * @brief Returns the boolean value when Kind() is Boolean.
     */
    std::optional<bool> BooleanValue() const noexcept;
    /**
     * @brief Returns the formula payload when Kind() is Formula.
     */
    const CellFormulaValue& FormulaValue() const noexcept;

private:
    CellValueKind m_kind = CellValueKind::Blank;
    std::string m_text;
    std::optional<UInt32> m_sharedStringIndex;
    std::optional<bool> m_booleanValue;
    CellFormulaValue m_formula;
};

} // namespace ExyokiOffice::Excel
