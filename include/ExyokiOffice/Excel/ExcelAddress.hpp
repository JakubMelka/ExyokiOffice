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
 * @brief Maximum one-based row index supported by the Excel worksheet grid.
 */
inline constexpr UInt32 MaxRowIndex = 1048576;
/**
 * @brief Maximum one-based column index supported by the Excel worksheet grid.
 *
 * Column 16,384 is represented as XFD in A1 notation.
 */
inline constexpr UInt32 MaxColumnIndex = 16384;

/**
 * @brief Validated one-based Excel row index.
 *
 * RowIndex accepts only values in the worksheet grid range 1..1,048,576. It is
 * intentionally small and value-like so it can be passed through higher-level
 * cell, range, and sparse worksheet APIs without repeating boundary checks.
 */
class EXYOKIOFFICE_EXPORT RowIndex
{
public:
    /**
     * @brief Creates an invalid row index with value 0.
     */
    constexpr RowIndex() noexcept = default;
    /**
     * @brief Creates a row index without validation.
     *
     * Prefer TryCreate() for public input. This constructor exists for code that
     * has already validated the coordinate.
     */
    explicit constexpr RowIndex(UInt32 value) noexcept : m_value(value) {}

    /**
     * @brief Validates and constructs a row index.
     *
     * @param value One-based row number.
     * @return RowIndex when value is in range, otherwise std::nullopt.
     */
    static std::optional<RowIndex> TryCreate(UInt32 value) noexcept;

    /**
     * @brief Returns true when the index is inside the Excel worksheet grid.
     */
    [[nodiscard]] constexpr bool IsValid() const noexcept { return m_value >= 1 && m_value <= MaxRowIndex; }
    /**
     * @brief Returns the one-based numeric row index.
     */
    constexpr UInt32 Value() const noexcept { return m_value; }

private:
    UInt32 m_value = 0;
};

/**
 * @brief Validated one-based Excel column index.
 *
 * ColumnIndex converts between numeric column coordinates and A1 column letters
 * such as A, Z, AA, and XFD. Values are limited to Excel's worksheet grid range
 * 1..16,384.
 */
class EXYOKIOFFICE_EXPORT ColumnIndex
{
public:
    /**
     * @brief Creates an invalid column index with value 0.
     */
    constexpr ColumnIndex() noexcept = default;
    /**
     * @brief Creates a column index without validation.
     *
     * Prefer TryCreate() or ParseName() for public input.
     */
    explicit constexpr ColumnIndex(UInt32 value) noexcept : m_value(value) {}

    /**
     * @brief Validates and constructs a column index.
     *
     * @param value One-based column number.
     * @return ColumnIndex when value is in range, otherwise std::nullopt.
     */
    static std::optional<ColumnIndex> TryCreate(UInt32 value) noexcept;
    /**
     * @brief Parses an A1 column name.
     *
     * The parser accepts ASCII letters only and is case-insensitive. Empty
     * strings, non-letter characters, and values beyond XFD are rejected.
     */
    static std::optional<ColumnIndex> ParseName(std::string_view name);

    /**
     * @brief Returns true when the index is inside the Excel worksheet grid.
     */
    [[nodiscard]] constexpr bool IsValid() const noexcept { return m_value >= 1 && m_value <= MaxColumnIndex; }
    /**
     * @brief Returns the one-based numeric column index.
     */
    constexpr UInt32 Value() const noexcept { return m_value; }
    /**
     * @brief Formats the index as an A1 column name.
     *
     * @return Column letters such as A or XFD, or an empty string when invalid.
     */
    std::string ToName() const;

private:
    UInt32 m_value = 0;
};

/**
 * @brief Validated cell address in Excel's one-based worksheet grid.
 *
 * CellAddress stores numeric coordinates and can parse or format both absolute
 * A1 references such as B2 and R1C1 references such as R2C2. Absolute markers
 * in A1 text (`$A$1`) are accepted but not stored; later formula-specific APIs
 * will model relative and mixed references separately.
 */
class EXYOKIOFFICE_EXPORT CellAddress
{
public:
    /**
     * @brief Creates an invalid address at row 0, column 0.
     */
    constexpr CellAddress() noexcept = default;
    /**
     * @brief Creates an address from already validated row and column indexes.
     */
    constexpr CellAddress(RowIndex row, ColumnIndex column) noexcept : m_row(row), m_column(column) {}

    /**
     * @brief Validates and constructs a cell address.
     */
    static std::optional<CellAddress> TryCreate(UInt32 row, UInt32 column) noexcept;
    /**
     * @brief Parses a single A1 cell reference.
     *
     * Accepted examples include A1, xfd1048576, $B$2, and C$10. Sheet-qualified
     * references, ranges, formulas, negative indexes, and partial input are
     * rejected.
     */
    static std::optional<CellAddress> ParseA1(std::string_view text);
    /**
     * @brief Parses a single absolute R1C1 cell reference.
     *
     * Accepted examples include R1C1 and r1048576c16384. Relative forms such as
     * R[1]C[1] are formula references and are intentionally rejected here.
     */
    static std::optional<CellAddress> ParseR1C1(std::string_view text);

    /**
     * @brief Returns true when both row and column are valid.
     */
    [[nodiscard]] constexpr bool IsValid() const noexcept { return m_row.IsValid() && m_column.IsValid(); }
    /**
     * @brief Returns the validated one-based row index.
     */
    constexpr RowIndex Row() const noexcept { return m_row; }
    /**
     * @brief Returns the validated one-based column index.
     */
    constexpr ColumnIndex Column() const noexcept { return m_column; }
    /**
     * @brief Formats the address in A1 notation.
     */
    std::string ToA1() const;
    /**
     * @brief Formats the address in absolute R1C1 notation.
     */
    std::string ToR1C1() const;

private:
    RowIndex m_row;
    ColumnIndex m_column;
};

/**
 * @brief Rectangular cell range bounded by two validated addresses.
 *
 * CellRange represents only normalized rectangular ranges. The first address is
 * always the top-left cell and the last address is always the bottom-right cell.
 * Single-cell ranges are allowed and format as a single A1 address.
 */
class EXYOKIOFFICE_EXPORT CellRange
{
public:
    /**
     * @brief Creates an invalid range.
     */
    constexpr CellRange() noexcept = default;
    /**
     * @brief Creates a range from already normalized endpoints.
     */
    constexpr CellRange(CellAddress first, CellAddress last) noexcept : m_first(first), m_last(last) {}

    /**
     * @brief Validates and constructs a normalized range.
     */
    static std::optional<CellRange> TryCreate(CellAddress first, CellAddress last) noexcept;
    /**
     * @brief Parses an A1 range.
     *
     * Both `A1:B2` and a single-cell `A1` are accepted. The parser rejects
     * reversed ranges such as B2:A1, whole-row/whole-column shorthand, sheet
     * qualifiers, empty endpoints, and coordinates outside Excel's grid.
     */
    static std::optional<CellRange> ParseA1(std::string_view text);
    /**
     * @brief Parses an absolute R1C1 range.
     *
     * Both `R1C1:R2C2` and a single-cell `R1C1` are accepted. Relative R1C1
     * formulas are intentionally rejected.
     */
    static std::optional<CellRange> ParseR1C1(std::string_view text);

    /**
     * @brief Returns true when both endpoints are valid and normalized.
     */
    [[nodiscard]] bool IsValid() const noexcept;
    /**
     * @brief Returns the top-left address.
     */
    constexpr CellAddress First() const noexcept { return m_first; }
    /**
     * @brief Returns the bottom-right address.
     */
    constexpr CellAddress Last() const noexcept { return m_last; }
    /**
     * @brief Returns the number of rows covered by the range.
     */
    UInt32 RowCount() const noexcept;
    /**
     * @brief Returns the number of columns covered by the range.
     */
    UInt32 ColumnCount() const noexcept;
    /**
     * @brief Formats the range in A1 notation.
     */
    std::string ToA1() const;
    /**
     * @brief Formats the range in absolute R1C1 notation.
     */
    std::string ToR1C1() const;

private:
    CellAddress m_first;
    CellAddress m_last;
};

/**
 * @brief A worksheet-qualified cell range, as used by chart data-source formulas
 * and other cross-sheet A1 references (for example `Sheet1!$A$2:$A$5` or
 * `'My Data'!$B$2`).
 *
 * Parse() accepts the same syntax ToFormula() produces: an unquoted sheet name,
 * or a single-quoted one with `''` representing a literal embedded quote,
 * followed by `!` and an A1 range. ToFormula() always emits an absolute range
 * (`$A$1` style) and quotes the sheet name only when required.
 */
class EXYOKIOFFICE_EXPORT SheetCellRange
{
public:
    /**
     * @brief Creates an empty-sheet, invalid-range instance.
     */
    SheetCellRange() = default;
    /**
     * @brief Creates an instance from an already resolved sheet name and range.
     */
    SheetCellRange(std::string sheet, CellRange range) noexcept : m_sheet(std::move(sheet)), m_range(range) {}

    /**
     * @brief Parses a sheet-qualified A1 formula.
     * @return The sheet name and range, or `std::nullopt` when the formula has
     *         no `!` qualifier or its range is not valid A1 syntax.
     */
    static std::optional<SheetCellRange> Parse(std::string_view formula);

    /**
     * @brief Returns the worksheet name, exactly as parsed or supplied (never quoted).
     */
    const std::string& Sheet() const noexcept { return m_sheet; }
    /**
     * @brief Returns the cell range on that worksheet.
     */
    constexpr CellRange Range() const noexcept { return m_range; }

    /**
     * @brief Formats as an absolute, sheet-qualified A1 formula.
     * @return For example `Sheet1!$A$2:$A$5`, or `'My Data'!$B$2` when the sheet
     *         name requires quoting; an empty string when Range() is invalid.
     */
    std::string ToFormula() const;

private:
    std::string m_sheet;
    CellRange m_range;
};

} // namespace ExyokiOffice::Excel
