// Copyright (c) 2026 Jakub Melka and Contributors
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#pragma once

#include "FormulaParser.hpp"

#include "ExyokiOffice/Excel/ExcelDocument.hpp"
#include "ExyokiOffice/StandardTypes.hpp"

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <vector>

namespace ExyokiOffice::Excel
{

class FormulaFunctionRegistry;

/** @brief Fully resolved rectangular reference area on a concrete worksheet. */
struct ResolvedReferenceArea
{
    /** Resolved worksheet display name; empty when the sheet is unknown. */
    std::string sheet;
    UInt32 firstRow = 0;
    UInt32 lastRow = 0;
    UInt32 firstColumn = 0;
    UInt32 lastColumn = 0;

    UInt32 RowCount() const noexcept { return lastRow - firstRow + 1; }
    UInt32 ColumnCount() const noexcept { return lastColumn - firstColumn + 1; }
    bool Contains(UInt32 row, UInt32 column) const noexcept
    {
        return row >= firstRow && row <= lastRow && column >= firstColumn && column <= lastColumn;
    }
};

/**
 * @brief Intermediate evaluation value: a @ref FormulaValue or a reference.
 *
 * References keep their area list so reference-aware functions such as ROW,
 * COLUMN, OFFSET, or the aggregate functions can inspect them without
 * materializing values. Most consumers dereference immediately through the
 * session helpers.
 */
struct EvalValue
{
    FormulaValue value;
    bool isReference = false;
    /** Reference areas; more than one entry represents a union. */
    std::vector<ResolvedReferenceArea> areas;

    static EvalValue Scalar(FormulaValue value)
    {
        EvalValue result;
        result.value = std::move(value);
        return result;
    }
    static EvalValue Reference(std::vector<ResolvedReferenceArea> areas)
    {
        EvalValue result;
        result.isReference = true;
        result.areas = std::move(areas);
        return result;
    }
    static EvalValue Error(FormulaErrorCode code)
    {
        return Scalar(FormulaValue::Error(code));
    }
};

/** @brief Centralized Excel-compatible type coercion rules. */
class FormulaCoercion final
{
public:
    FormulaCoercion() = delete;

    /**
     * @brief Coerces a scalar value to a number.
     *
     * Blank becomes 0, booleans become 1 or 0, and text is parsed as a
     * canonical en-US number. Unparsable text yields `#VALUE!` and errors
     * pass through.
     */
    static FormulaValue ToNumber(const FormulaValue& value);
    /**
     * @brief Coerces a scalar value to text using cell display formatting.
     */
    static FormulaValue ToText(const FormulaValue& value);
    /**
     * @brief Coerces a scalar value to a logical value.
     *
     * Numbers map zero to FALSE and non-zero to TRUE. Text `TRUE`/`FALSE`
     * (case-insensitive) converts; other text yields `#VALUE!`.
     */
    static FormulaValue ToBoolean(const FormulaValue& value);
    /**
     * @brief Parses canonical en-US number text.
     *
     * Leading and trailing ASCII whitespace is ignored. When @p acceptRichForms
     * is true, percent suffixes and ISO date/time text are recognized as well,
     * matching the VALUE worksheet function.
     */
    static std::optional<Real> ParseNumberText(std::string_view text, bool acceptRichForms = false);
    /** @brief Formats a number exactly like @ref ExcelCellValue::Number. */
    static std::string FormatNumber(Real value);
    /**
     * @brief Compares two scalar values with Excel ordering.
     *
     * Numbers sort before text and text before logical values; text
     * comparison is case-insensitive. Blank compares equal to 0, the empty
     * string, and FALSE.
     *
     * @return Negative, zero, or positive, or std::nullopt when either value
     * is an error.
     */
    static std::optional<int> Compare(const FormulaValue& left, const FormulaValue& right);
};

/** @brief Serial-number date arithmetic in the 1900 date system. */
class ExcelDateSerial final
{
public:
    ExcelDateSerial() = delete;

    /** @brief Civil calendar date and time-of-day parts. */
    struct DateParts
    {
        Int32 year = 0;
        UInt32 month = 0;
        UInt32 day = 0;
        UInt32 hour = 0;
        UInt32 minute = 0;
        Real second = 0.0;
    };

    /**
     * @brief Converts a civil date to an Excel serial number.
     *
     * Month and day overflow is normalized the way DATE() does, so month 13
     * rolls into the following year. Serial numbers before 1 (dates before
     * 1900) are reported as std::nullopt.
     */
    static std::optional<Real> FromParts(Int32 year,
                                         Int64 month,
                                         Int64 day,
                                         Real hours = 0.0,
                                         Real minutes = 0.0,
                                         Real seconds = 0.0);
    /**
     * @brief Splits a non-negative serial number into date and time parts.
     *
     * The 1900 leap-year bug is preserved for compatibility: serial 60 maps
     * to 1900-02-28 because the fictitious 1900-02-29 does not exist in the
     * civil calendar.
     */
    static std::optional<DateParts> ToParts(Real serial);
    /**
     * @brief Parses ISO 8601 date, date-time, or time-of-day text to a serial.
     */
    static std::optional<Real> ParseIso(std::string_view text);
};

/**
 * @brief One formula evaluation pass over a workbook.
 *
 * The session resolves references, reads cell values (optionally overlaid
 * with freshly recalculated results), applies operator semantics, and invokes
 * registered functions. It is an internal component created by
 * @ref FormulaEngine; it is not part of the public API.
 */
class FormulaEvaluationSession
{
public:
    /** @brief Overlay map key: lower-case sheet name with row and column. */
    struct CellKey
    {
        std::string sheet;
        UInt32 row = 0;
        UInt32 column = 0;

        bool operator<(const CellKey& other) const noexcept
        {
            if (sheet != other.sheet)
            {
                return sheet < other.sheet;
            }
            if (row != other.row)
            {
                return row < other.row;
            }
            return column < other.column;
        }
    };
    using OverlayMap = std::map<CellKey, FormulaValue>;

    FormulaEvaluationSession(ExcelDocument::Ptr document, const FormulaFunctionRegistry& registry);

    /** @brief Selects the worksheet used to resolve unqualified references. */
    void SetCurrentSheet(std::string sheetName);
    const std::string& CurrentSheet() const noexcept { return m_currentSheet; }
    /** @brief Supplies the anchor cell providing ROW() and COLUMN() context. */
    void SetAnchor(CellAddress anchor) { m_anchor = anchor; }
    CellAddress Anchor() const noexcept { return m_anchor; }
    /**
     * @brief Applies a shared-formula expansion offset.
     *
     * Relative reference coordinates are shifted by the offset when a shared
     * group dependent cell evaluates its anchor's expression.
     */
    void SetReferenceOffset(Int64 rowOffset, Int64 columnOffset)
    {
        m_rowOffset = rowOffset;
        m_columnOffset = columnOffset;
    }
    /**
     * @brief Enables array-formula semantics for the outermost expression.
     *
     * In array context, range references used by scalar operators broadcast
     * as matrices instead of applying implicit intersection.
     */
    void SetArrayContext(bool arrayContext) { m_arrayContext = arrayContext; }
    /** @brief Supplies recalculated values that override stored cell values. */
    void SetOverlay(const OverlayMap* overlay) { m_overlay = overlay; }

    /** @brief Returns true when a worksheet with this name exists. */
    bool SheetExists(std::string_view sheetName);
    /** @brief Returns the display name of the first worksheet, or empty. */
    std::string FirstSheetName();

    /** @brief Evaluates a parsed expression tree to a final value. */
    FormulaValue EvaluateToValue(const FormulaExpression& root);

    /** @brief Evaluates one expression node. */
    EvalValue Evaluate(const FormulaExpression& node);

    /**
     * @brief Dereferences an evaluation value to a scalar.
     *
     * Multi-cell references apply implicit intersection against the anchor
     * cell; outside array context a failed intersection yields `#VALUE!`.
     */
    FormulaValue DereferenceScalar(const EvalValue& value);
    /**
     * @brief Dereferences an evaluation value to a scalar or array value.
     *
     * References materialize to arrays clipped to the worksheet's used range;
     * scalars pass through.
     */
    FormulaValue DereferenceToValue(const EvalValue& value);

    /** @brief Reads one cell as a formula value, honoring the overlay. */
    FormulaValue ReadCell(std::string_view sheetName, UInt32 row, UInt32 column);
    /**
     * @brief Invokes a callback for every stored cell inside an area.
     *
     * Iteration touches only physically stored cells, so whole-column areas
     * stay proportional to worksheet content. Overlay values replace stored
     * values; overlay-only cells inside the area are visited as well.
     *
     * @return False when the sheet does not exist.
     */
    bool ForEachStoredCell(const ResolvedReferenceArea& area,
                           const std::function<void(UInt32 row, UInt32 column,
                                                    const FormulaValue& value)>& callback);
    /**
     * @brief Returns the stored extent of a worksheet as maximum row/column.
     *
     * Used to clip whole-row and whole-column references. Returns (0, 0) for
     * an unknown sheet or an empty worksheet.
     */
    std::pair<UInt32, UInt32> SheetExtent(std::string_view sheetName);

    const FormulaFunctionRegistry& Registry() const noexcept { return m_registry; }

    /**
     * @brief The random engine the volatile RAND family draws from.
     *
     * The session owns it, so there is no global or thread-local state: a
     * session belongs to one evaluation on one thread by construction, and
     * the engine lives exactly as long as the evaluation that may use it.
     * Seeded on first use - most sessions never draw a random number.
     */
    std::mt19937& RandomEngine();

private:
    struct SheetCache
    {
        Worksheet::Ptr worksheet;
        std::string displayName;
        /** Stored cell addresses sorted by (row, column); lazily filled. */
        std::optional<std::vector<CellAddress>> storedAddresses;
    };

    /** Lazily parsed defined-name definition. */
    struct NameDefinition
    {
        std::string formula;
        std::unique_ptr<FormulaParseResult> parsed;
    };

    SheetCache* FindSheet(std::string_view sheetName);
    const std::vector<CellAddress>& StoredAddresses(SheetCache& cache);
    FormulaValue CellValueToFormulaValue(const ExcelCellValue& cellValue);
    void LoadNames();
    /**
     * Resolves a defined name to its parsed definition. Qualified names look
     * in the qualifier sheet's scope first; unqualified names look in the
     * current sheet's scope first; both fall back to the workbook scope.
     * Returns nullptr for unknown names and unparsable definitions.
     */
    const FormulaExpression* ResolveName(std::string_view name,
                                         std::string_view sheetQualifier,
                                         bool qualified);

    EvalValue EvaluateReference(const FormulaExpression& node);
    EvalValue EvaluateNameReference(const FormulaExpression& node);
    EvalValue EvaluateUnary(const FormulaExpression& node);
    EvalValue EvaluateBinary(const FormulaExpression& node);
    EvalValue EvaluateFunction(const FormulaExpression& node);
    EvalValue EvaluateArrayLiteral(const FormulaExpression& node);
    FormulaValue ApplyBinaryScalar(FormulaBinaryOperator op,
                                   const FormulaValue& left,
                                   const FormulaValue& right);
    FormulaValue ApplyBinaryBroadcast(FormulaBinaryOperator op,
                                      const FormulaValue& left,
                                      const FormulaValue& right);
    EvalValue EvaluateIntersection(const EvalValue& left, const EvalValue& right);

    ExcelDocument::Ptr m_document;
    ExcelDocumentEditor m_editor;
    const FormulaFunctionRegistry& m_registry;
    std::string m_currentSheet;
    CellAddress m_anchor;
    Int64 m_rowOffset = 0;
    Int64 m_columnOffset = 0;
    bool m_arrayContext = false;
    const OverlayMap* m_overlay = nullptr;
    /** Lower-case sheet name to cached worksheet lookup. */
    std::map<std::string, SheetCache, std::less<>> m_sheets;
    bool m_sheetsLoaded = false;
    /** (lower-case scope sheet or empty, lower-case name) to definition. */
    std::map<std::pair<std::string, std::string>, NameDefinition> m_names;
    bool m_namesLoaded = false;
    /** Names currently being evaluated; guards against definition cycles. */
    std::vector<std::string> m_nameStack;
    /** Recursion guard for deeply nested expressions. */
    int m_depth = 0;
    /** Lazily seeded; see RandomEngine(). */
    std::optional<std::mt19937> m_randomEngine;

    friend class FormulaFunctionHelpers;
};

} // namespace ExyokiOffice::Excel
