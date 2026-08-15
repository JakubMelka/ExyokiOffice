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
#include <string_view>
#include <vector>

namespace ExyokiOffice::Packaging
{
class TableDefinitionPart;
}

namespace ExyokiOffice::Excel
{

/** @brief Aggregate function displayed in a table totals row. */
enum class TableTotalsFunction
{
    None,
    Sum,
    Minimum,
    Maximum,
    Average,
    Count,
    CountNumbers,
    StandardDeviation,
    Variance,
    Custom
};

/**
 * @brief Complete editable definition of one SpreadsheetML table column.
 *
 * Column IDs are stable positive identifiers within a table. A zero ID passed
 * to SetColumns() requests automatic allocation. Names must be non-empty and
 * unique within the table using case-insensitive comparison. Formula text is
 * stored verbatim without a leading equals-sign requirement or evaluation.
 */
struct EXYOKIOFFICE_EXPORT ExcelTableColumn
{
    UInt32 Id = 0;
    std::string Name;
    std::optional<std::string> CalculatedColumnFormula;
    std::optional<std::string> TotalsRowFormula;
    std::optional<std::string> TotalsRowLabel;
    TableTotalsFunction TotalsFunction = TableTotalsFunction::None;
};

/**
 * @brief Discrete-value filter for one zero-based table column.
 *
 * Values are SpreadsheetML lexical values and are not converted according to
 * cell type. `includeBlank` additionally selects blank cells. At least one
 * value or `includeBlank` must be supplied.
 */
struct EXYOKIOFFICE_EXPORT ExcelTableValueFilter
{
    UInt32 ColumnIndex = 0;
    std::vector<std::string> Values;
    bool IncludeBlank = false;
};

/**
 * @brief High-level wrapper around one worksheet table definition part.
 *
 * The wrapper remains valid while its underlying package part is attached.
 * Mutations update the table definition XML atomically. They do not calculate
 * formulas or populate worksheet cells. Removing a table through Worksheet
 * detaches its package part and invalidates wrappers retained by callers.
 */
class EXYOKIOFFICE_EXPORT ExcelTable
{
public:
    using Ptr = std::shared_ptr<ExcelTable>;

    /** @brief Creates a wrapper for an existing table definition part. */
    explicit ExcelTable(std::shared_ptr<Packaging::TableDefinitionPart> part);

    /** @brief Returns the stable workbook table identifier, or zero when
     * detached. */
    UInt32 Id() const;
    /** @brief Returns the display name used by structured references. */
    std::string Name() const;
    /** @brief Returns the table rectangle, including header and totals rows. */
    std::optional<CellRange> Range() const;
    /** @brief Returns all table columns in their XML and worksheet order. */
    std::vector<ExcelTableColumn> Columns() const;
    /** @brief Returns true when the table owns an auto-filter definition. */
    [[nodiscard]] bool AutoFilterEnabled() const;
    /** @brief Returns true when the table has a visible totals row. */
    [[nodiscard]] bool TotalsRowShown() const;

    /**
     * @brief Renames the table within its definition part.
     *
     * This low-level wrapper validates Excel table-name syntax but cannot check
     * workbook-wide uniqueness. Prefer Worksheet::RenameTable() when the table
     * is attached to a worksheet editor.
     */
    bool SetName(std::string_view name);
    /**
     * @brief Replaces all column definitions atomically.
     * @param columns Ordered columns; count must equal Range().ColumnCount().
     * @return true when IDs, names, formulas, and totals metadata were valid.
     */
    bool SetColumns(const std::vector<ExcelTableColumn>& columns);
    /**
     * @brief Resizes the table while preserving column metadata by position.
     *
     * The new range must have the same width as the existing range and contain
     * at least one row. The table reference and enabled auto-filter reference
     * are changed together or not at all.
     */
    bool Resize(CellRange range);
    /**
     * @brief Resizes the table and atomically replaces its column definitions.
     *
     * Use this overload when the table width changes. `columns.size()` must
     * equal the new width. Existing value filters whose zero-based column index
     * no longer fits are removed; remaining filters and auto-filter range are
     * preserved.
     */
    bool Resize(CellRange range, const std::vector<ExcelTableColumn>& columns);
    /** @brief Adds or removes the table auto-filter and synchronizes its range.
     */
    bool SetAutoFilterEnabled(bool enabled);
    /** @brief Returns all discrete-value filters in column order. */
    std::vector<ExcelTableValueFilter> ValueFilters() const;
    /**
     * @brief Creates or replaces the discrete-value filter for one column.
     * @return false for an out-of-range column, duplicate values, an empty
     * criterion, or a detached table.
     */
    bool SetValueFilter(const ExcelTableValueFilter& filter);
    /** @brief Removes the value filter for a zero-based table column. */
    bool RemoveValueFilter(UInt32 columnIndex);
    /** @brief Removes every value filter while retaining the auto-filter. */
    bool ClearValueFilters();
    /**
     * @brief Shows or hides the totals row.
     *
     * Showing totals requires at least two table rows. Column totals formulas,
     * labels, and functions are retained when the row is hidden.
     */
    bool SetTotalsRowShown(bool shown);

    /** @brief Exposes the underlying package part for advanced Open XML access.
     */
    std::shared_ptr<Packaging::TableDefinitionPart> GetPart() const;

private:
    std::shared_ptr<Packaging::TableDefinitionPart> m_part;
};

/** @brief Validates Excel table/display names used by structured references. */
[[nodiscard]] EXYOKIOFFICE_EXPORT bool IsValidExcelTableName(std::string_view name);

} // namespace ExyokiOffice::Excel
