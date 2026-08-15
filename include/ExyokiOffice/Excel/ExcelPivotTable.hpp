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
class PivotTablePart;
class PivotTableCacheDefinitionPart;
class PivotTableCacheRecordsPart;
class ExcelDocument;
} // namespace ExyokiOffice::Packaging

namespace ExyokiOffice::Excel
{

class Worksheet;

/**
 * @brief Placement of a source column inside a pivot table report.
 *
 * Every column of the pivot cache is represented by a pivot field. A field that
 * is not placed on an axis and is not used by a data field stays available in
 * the spreadsheet application's field list but does not affect the report.
 */
enum class PivotAxis
{
    /** The field is not placed on any axis. */
    None,
    /** The field groups report rows (`axis="axisRow"`). */
    Row,
    /** The field groups report columns (`axis="axisCol"`). */
    Column,
    /** The field acts as a report filter above the report (`axis="axisPage"`). */
    Page
};

/**
 * @brief Aggregate applied by a pivot data field.
 *
 * The values map one-to-one onto SpreadsheetML's `ST_DataConsolidateFunction`.
 * ExyokiOffice evaluates every function itself when it writes the cached
 * report cells, so the results are available without a spreadsheet application.
 *
 * Blank source cells never take part in an aggregate. Text and error cells take
 * part in @ref Count only.
 */
enum class PivotAggregateFunction
{
    /** Sum of the numeric values in the group. */
    Sum,
    /** Number of non-blank values in the group (Excel's `COUNTA`). */
    Count,
    /** Number of numeric values in the group (Excel's `COUNT`). */
    CountNumbers,
    /** Arithmetic mean of the numeric values in the group. */
    Average,
    /** Largest numeric value in the group. */
    Maximum,
    /** Smallest numeric value in the group. */
    Minimum,
    /** Product of the numeric values in the group. */
    Product,
    /** Sample standard deviation; blank for groups with fewer than two values. */
    StandardDeviation,
    /** Population standard deviation. */
    StandardDeviationP,
    /** Sample variance; blank for groups with fewer than two values. */
    Variance,
    /** Population variance. */
    VarianceP
};

/**
 * @brief Optional "show values as" transformation stored on a data field.
 *
 * The value is written to `dataField/@showDataAs` for spreadsheet applications
 * that recalculate the report. ExyokiOffice always writes raw aggregates into
 * the cached report cells, because the derived presentations depend on a base
 * field and base item that only a recalculating application resolves.
 */
enum class PivotShowDataAs
{
    /** Plain aggregate. */
    Normal,
    /** Difference from the base item. */
    Difference,
    /** Percentage of the base item. */
    PercentOf,
    /** Percentage difference from the base item. */
    PercentDifference,
    /** Running total along the base field. */
    RunningTotal,
    /** Percentage of the row total. */
    PercentOfRow,
    /** Percentage of the column total. */
    PercentOfColumn,
    /** Percentage of the grand total. */
    PercentOfTotal,
    /** Index (weighted relative importance). */
    Index
};

/**
 * @brief Placement and behavior of one source column in a pivot table.
 *
 * @ref name must match a header cell of the source range, compared
 * case-insensitively. Columns of the source range that are not listed in
 * @ref ExcelPivotTableDefinition::fields become unplaced pivot fields
 * automatically, so callers only describe the columns they actually use.
 */
struct EXYOKIOFFICE_EXPORT ExcelPivotField
{
    /** @brief Source column header. Required and matched case-insensitively. */
    std::string Name;

    /** @brief Axis the field is placed on. */
    PivotAxis Axis = PivotAxis::None;

    /**
     * @brief Emits a subtotal row or column for each group of this field.
     *
     * Subtotals are only meaningful for a row or column field that has at least
     * one further field nested inside it; the innermost field of an axis never
     * produces subtotals because each of its groups is already a single line.
     */
    bool ShowSubtotal = false;

    /**
     * @brief Keeps items without matching source rows visible in the field list.
     *
     * This maps to `pivotField/@showAll`. It does not change the cached report,
     * because ExyokiOffice only enumerates items that occur in the source data.
     */
    bool ShowAll = false;

    /** @brief Requests a blank separator line after each group (`@insertBlankRow`). */
    bool InsertBlankRow = false;

    /**
     * @brief Item selected by a page (report filter) field.
     *
     * The index refers to the field's shared items in report order, which is the
     * ascending order returned by @ref ExcelPivotTable::FieldItems. `std::nullopt`
     * selects every item, shown as `(All)`. The setting is ignored for fields
     * whose @ref axis is not @ref PivotAxis::Page.
     *
     * A page field selection filters the cached report that ExyokiOffice
     * writes, exactly like a spreadsheet application would.
     */
    std::optional<UInt32> SelectedItem;
};

/**
 * @brief One aggregated value column of a pivot table.
 *
 * Data fields are always placed on the column axis, as the innermost level.
 * A pivot table requires at least one data field.
 */
struct EXYOKIOFFICE_EXPORT ExcelPivotDataField
{
    /** @brief Source column header to aggregate. Matched case-insensitively. */
    std::string SourceField;

    /**
     * @brief Display name of the value column.
     *
     * When empty, a name is generated from the function and the source column,
     * for example `Sum of Amount`. Names must be unique within the pivot table,
     * compared case-insensitively.
     */
    std::string Name;

    /** @brief Aggregate applied to the source column. */
    PivotAggregateFunction Function = PivotAggregateFunction::Sum;

    /** @brief Optional "show values as" transformation. */
    PivotShowDataAs ShowDataAs = PivotShowDataAs::Normal;

    /**
     * @brief Base field used by @ref showDataAs, given as a source column header.
     *
     * Ignored when @ref showDataAs is @ref PivotShowDataAs::Normal.
     */
    std::optional<std::string> BaseField;

    /** @brief Base item index within @ref baseField used by @ref showDataAs. */
    std::optional<UInt32> BaseItem;

    /**
     * @brief Number format applied to the value column (`dataField/@numFmtId`).
     *
     * The identifier refers to a built-in or workbook-defined number format;
     * see @ref ExyokiOffice::Excel::StyleRepository for registering formats.
     * The identifier is stored on the data field only. It is not applied to the
     * cached report cells, which keep the worksheet's default cell format.
     */
    std::optional<UInt32> NumberFormatId;
};

/**
 * @brief Table-style presentation flags stored on `pivotTableStyleInfo`.
 *
 * The style name refers to a built-in or workbook-defined table style such as
 * `PivotStyleLight16`. ExyokiOffice does not validate the name against the
 * workbook style catalog, because built-in style names are not stored in the
 * package.
 */
struct EXYOKIOFFICE_EXPORT ExcelPivotTableStyle
{
    /** @brief Table style name; no style element is written when empty. */
    std::string Name = "PivotStyleLight16";
    /** @brief Applies the style's row-header formatting. */
    bool ShowRowHeaders = true;
    /** @brief Applies the style's column-header formatting. */
    bool ShowColumnHeaders = true;
    /** @brief Applies banded row formatting. */
    bool ShowRowStripes = false;
    /** @brief Applies banded column formatting. */
    bool ShowColumnStripes = false;
    /** @brief Applies the style's last-column formatting. */
    bool ShowLastColumn = true;
};

/**
 * @brief Complete description of a pivot table and its worksheet data source.
 *
 * The definition is consumed by @ref Worksheet::CreatePivotTable and by
 * @ref ExcelPivotTable::Update. @ref PivotTableBuilder assembles the same
 * structure through a fluent interface.
 *
 * @code
 * ExcelPivotTableDefinition definition;
 * definition.Name        = "SalesByRegion";
 * definition.SourceSheet = "Data";
 * definition.SourceRange = *CellRange::ParseA1("A1:D101");
 * definition.TargetCell  = *CellAddress::ParseA1("A3");
 * definition.Fields      = {{"Region", PivotAxis::Row}, {"Quarter", PivotAxis::Column}};
 * definition.DataFields  = {{"Amount", "", PivotAggregateFunction::Sum}};
 * auto pivot = reportSheet->CreatePivotTable(definition).PivotTable;
 * @endcode
 */
struct EXYOKIOFFICE_EXPORT ExcelPivotTableDefinition
{
    /**
     * @brief Pivot table name, unique in the workbook (case-insensitive).
     *
     * The name must be non-empty and at most 255 characters. When empty, a
     * unique `PivotTableN` name is generated.
     */
    std::string Name;

    /**
     * @brief Worksheet that owns @ref sourceRange.
     *
     * When empty, the worksheet that hosts the pivot table is used. The sheet
     * must exist in the same workbook.
     */
    std::string SourceSheet;

    /**
     * @brief Source rectangle including the header row.
     *
     * The first row supplies the field names and must contain a non-empty,
     * unique caption in every column. At least one further row is required.
     */
    CellRange SourceRange;

    /** @brief Top-left cell of the report on the hosting worksheet. */
    CellAddress TargetCell;

    /** @brief Placement of the source columns. Unlisted columns stay unplaced. */
    std::vector<ExcelPivotField> Fields;

    /** @brief Aggregated value columns. At least one is required. */
    std::vector<ExcelPivotDataField> DataFields;

    /** @brief Table-style presentation flags. */
    ExcelPivotTableStyle Style;

    /** @brief Emits a grand total line below the report rows. */
    bool RowGrandTotals = true;

    /** @brief Emits a grand total column at the right edge of the report. */
    bool ColumnGrandTotals = true;

    /**
     * @brief Asks spreadsheet applications to refresh the cache when opening.
     *
     * Leaving this enabled keeps the report consistent with source data that a
     * user edits later. Disable it when the written cache must be shown exactly
     * as produced by ExyokiOffice.
     */
    bool RefreshOnLoad = true;

    /**
     * @brief Writes the computed report into the hosting worksheet's cells.
     *
     * When enabled, the report is fully rendered as ordinary cell values, so it
     * is readable without a spreadsheet application and survives viewers that
     * do not recalculate pivot tables. When disabled, only the pivot table
     * definition, the cache, and the cache records are written and the report
     * area is left blank until the report is refreshed.
     */
    bool WriteCachedReport = true;

    /** @brief Caption of the data-field header (`@dataCaption`). */
    std::string DataCaption = "Values";

    /** @brief Caption of the grand total lines. Defaults to `Grand Total`. */
    std::string GrandTotalCaption;

    /** @brief Optional caption written to `@rowHeaderCaption`. */
    std::string RowHeaderCaption;

    /** @brief Optional caption written to `@colHeaderCaption`. */
    std::string ColumnHeaderCaption;
};

/** @brief Reason a pivot table operation was rejected. */
enum class PivotTableError
{
    /** The operation completed successfully. */
    None,
    /** The worksheet wrapper, workbook, or pivot table part is detached. */
    InvalidWorksheet,
    /** The pivot table name is invalid or already used in this workbook. */
    InvalidName,
    /** The source range is invalid, has no data rows, or its sheet is missing. */
    InvalidSource,
    /** A header cell of the source range is blank or duplicated. */
    InvalidSourceHeader,
    /** A field or data field references a column that is not in the source. */
    UnknownField,
    /** The field placement or data field configuration is not valid. */
    InvalidFieldConfiguration,
    /** The report does not fit on the worksheet grid at the target cell. */
    InvalidTarget,
    /** The report would overlap another pivot table on the same worksheet. */
    OverlappingReport,
    /** A package part, relationship, or cell write failed. */
    WriteFailed
};

/** @brief Structured result of a pivot table operation. */
struct EXYOKIOFFICE_EXPORT PivotTableResult
{
    /** @brief Error code; @ref PivotTableError::None on success. */
    PivotTableError Error = PivotTableError::None;
    /** @brief Human-readable diagnostic message; empty on success. */
    std::string Message;

    /** @brief Returns true when the operation completed successfully. */
    [[nodiscard]] bool Succeeded() const noexcept { return Error == PivotTableError::None; }
    /** @brief Provides convenient success testing in conditional statements. */
    [[nodiscard]] explicit operator bool() const noexcept { return Succeeded(); }
};

/**
 * @brief One distinct value of a pivot field, as stored in the pivot cache.
 *
 * Item order is the report order: values are sorted ascending, numbers before
 * text, with text compared case-insensitively and ties broken by byte order.
 * The zero-based position in @ref ExcelPivotTable::FieldItems is the index used
 * by @ref ExcelPivotField::selectedItem and @ref ExcelPivotDataField::baseItem.
 */
struct EXYOKIOFFICE_EXPORT ExcelPivotItem
{
    /** @brief Display caption of the item, as written to the report cells. */
    std::string Caption;
    /** @brief Numeric value when the item came from a numeric source cell. */
    std::optional<Real> Number;
    /** @brief True when the item represents blank source cells. */
    bool Blank = false;
};

/**
 * @brief High-level wrapper around one pivot table and its pivot cache.
 *
 * A wrapper stays valid while its package parts remain attached to the
 * workbook. It is obtained from @ref Worksheet::CreatePivotTable,
 * @ref Worksheet::PivotTables, or @ref Worksheet::PivotTableByName; removing a
 * pivot table through @ref Worksheet::RemovePivotTable invalidates wrappers
 * retained by callers.
 *
 * The wrapper owns three package parts: the pivot table definition on the
 * hosting worksheet, the pivot cache definition on the workbook, and the pivot
 * cache records below the cache definition. All three are kept consistent by
 * every mutating method.
 */
class EXYOKIOFFICE_EXPORT ExcelPivotTable
{
public:
    using Ptr = std::shared_ptr<ExcelPivotTable>;

    /**
     * @brief Creates a wrapper for an existing pivot table part.
     *
     * Normal callers obtain wrappers from @ref Worksheet instead of using this
     * constructor directly.
     *
     * @param part Pivot table definition part attached to a worksheet.
     * @param document Workbook that owns @p part.
     */
    ExcelPivotTable(std::shared_ptr<Packaging::PivotTablePart> part,
                    std::shared_ptr<Packaging::ExcelDocument> document);

    /** @brief Returns true when the wrapper is attached to a usable pivot table part. */
    [[nodiscard]] bool IsValid() const noexcept;

    /** @brief Returns the pivot table name, or an empty string when detached. */
    std::string Name() const;

    /**
     * @brief Renames the pivot table.
     *
     * The new name must be non-empty, at most 255 characters, and unique across
     * every pivot table in the workbook when compared case-insensitively.
     */
    PivotTableResult SetName(std::string_view name);

    /**
     * @brief Returns the report rectangle stored in `location/@ref`.
     *
     * The rectangle covers the header rows and the report body. Report filter
     * (page field) lines are written above it and are not part of the rectangle.
     */
    std::optional<CellRange> ReportRange() const;

    /** @brief Returns the complete area written to the worksheet, including report filters. */
    std::optional<CellRange> WrittenRange() const;

    /** @brief Returns the worksheet name that owns the source range. */
    std::string SourceSheet() const;

    /** @brief Returns the source rectangle including its header row. */
    std::optional<CellRange> SourceRange() const;

    /** @brief Returns the workbook-wide cache identifier shared with `workbook.xml`. */
    UInt32 CacheId() const;

    /** @brief Returns the source column headers in pivot cache field order. */
    std::vector<std::string> SourceFieldNames() const;

    /**
     * @brief Returns the distinct cached items of one source column.
     *
     * @param fieldName Source column header, matched case-insensitively.
     * @return Items in report order, or an empty vector for an unknown column
     * or a column whose values were not enumerated because it is unplaced.
     */
    std::vector<ExcelPivotItem> FieldItems(std::string_view fieldName) const;

    /** @brief Returns the placement of every source column, in cache field order. */
    std::vector<ExcelPivotField> Fields() const;

    /** @brief Returns the aggregated value columns in report order. */
    std::vector<ExcelPivotDataField> DataFields() const;

    /** @brief Returns the table-style presentation flags. */
    ExcelPivotTableStyle Style() const;

    /** @brief Replaces the table-style presentation flags. */
    PivotTableResult SetStyle(const ExcelPivotTableStyle& style);

    /** @brief Returns true when a grand total line is emitted below the report rows. */
    [[nodiscard]] bool RowGrandTotals() const;

    /** @brief Returns true when a grand total column is emitted at the report's right edge. */
    [[nodiscard]] bool ColumnGrandTotals() const;

    /**
     * @brief Reconstructs the definition that produced the current pivot table.
     *
     * The result can be modified and passed back to @ref Update, which is the
     * supported way to change field placement, aggregates, or the source range
     * of an existing pivot table.
     *
     * @return The definition, or `std::nullopt` when the wrapper is detached.
     */
    std::optional<ExcelPivotTableDefinition> Definition() const;

    /**
     * @brief Replaces the complete pivot table configuration and rebuilds it.
     *
     * The pivot table keeps its package parts, its cache identifier, and its
     * relationships, so anything that already refers to the pivot table stays
     * valid. Everything else — source range, fields, data fields, style,
     * totals, target cell, and the cached report cells — is replaced.
     *
     * The previously written report cells are cleared before the new report is
     * written. On failure the workbook is left unchanged.
     *
     * @param definition Replacement configuration. @ref
     * ExcelPivotTableDefinition::name may keep the current name or change it.
     */
    PivotTableResult Update(const ExcelPivotTableDefinition& definition);

    /**
     * @brief Re-reads the source range and rebuilds cache, layout, and cells.
     *
     * Use this after changing the source worksheet's data. Field placement,
     * aggregates, and style are preserved. Items that disappeared from the
     * source are dropped and new items are added, so page field selections that
     * no longer resolve fall back to `(All)`.
     */
    PivotTableResult Refresh();

    /**
     * @brief Points the pivot table at another source rectangle and refreshes it.
     *
     * @param range New source rectangle including its header row.
     * @param sheet Worksheet that owns @p range. Empty keeps the current sheet.
     * @note Field placements whose column disappeared from the new source are
     * dropped; data fields must still resolve, otherwise the call fails and the
     * workbook is left unchanged.
     */
    PivotTableResult SetSourceRange(CellRange range, std::string_view sheet = {});

    /**
     * @brief Moves the report to another top-left cell and rewrites its cells.
     *
     * @param targetCell New top-left cell on the hosting worksheet.
     */
    PivotTableResult MoveTo(CellAddress targetCell);

    /**
     * @brief Changes the axis of one source column and rebuilds the report.
     *
     * @param fieldName Source column header, matched case-insensitively.
     * @param axis Requested placement.
     */
    PivotTableResult SetFieldAxis(std::string_view fieldName, PivotAxis axis);

    /** @brief Enables or disables the row and column grand totals. */
    PivotTableResult SetGrandTotals(bool rows, bool columns);

    /**
     * @brief Reads one aggregated report value without touching the worksheet.
     *
     * This resolves the same aggregate that the cached report cell holds and is
     * useful for verification and for callers that disabled
     * @ref ExcelPivotTableDefinition::writeCachedReport.
     *
     * @param rowItems Captions of the row field items, outermost first. Fewer
     * captions than row fields select a subtotal group, which requires that
     * field's @ref ExcelPivotField::showSubtotal. An empty vector selects the
     * grand total across all rows, which requires
     * @ref ExcelPivotTableDefinition::rowGrandTotals unless the report has no
     * row fields at all.
     * @param columnItems Captions of the column field items, outermost first,
     * with the same prefix semantics.
     * @param dataFieldName Data field display name. Empty selects the first one.
     * @return The aggregate, or `std::nullopt` when a caption does not resolve,
     * the requested total line is not part of the report, or the selected group
     * contains no contributing source row.
     */
    std::optional<Real> AggregatedValue(const std::vector<std::string>& rowItems,
                                        const std::vector<std::string>& columnItems,
                                        std::string_view dataFieldName = {}) const;

    /** @brief Exposes the pivot table definition part for advanced Open XML access. */
    std::shared_ptr<Packaging::PivotTablePart> GetPart() const;
    /** @brief Exposes the pivot cache definition part for advanced Open XML access. */
    std::shared_ptr<Packaging::PivotTableCacheDefinitionPart> GetCacheDefinitionPart() const;
    /** @brief Exposes the pivot cache records part for advanced Open XML access. */
    std::shared_ptr<Packaging::PivotTableCacheRecordsPart> GetCacheRecordsPart() const;

private:
    friend class Worksheet;

    std::shared_ptr<Packaging::PivotTablePart> m_part;
    std::shared_ptr<Packaging::ExcelDocument> m_document;
};

/**
 * @brief Result of creating a pivot table: the attached wrapper and the status.
 *
 * Returned by @ref Worksheet::CreatePivotTable so that the failure reason
 * travels with the pivot table instead of through an out-parameter.
 */
struct EXYOKIOFFICE_EXPORT PivotTableCreationResult
{
    /** @brief Structured status; @ref PivotTableError::None on success. */
    PivotTableResult Status;
    /** @brief The attached pivot table, or nullptr when @ref Status reports a failure. */
    ExcelPivotTable::Ptr PivotTable;

    /** @brief Returns true when the pivot table was created. */
    [[nodiscard]] bool Succeeded() const noexcept { return Status.Succeeded(); }
    /** @brief Provides convenient success testing in conditional statements. */
    [[nodiscard]] explicit operator bool() const noexcept { return Succeeded(); }
};

/**
 * @brief Fluent builder that assembles and inserts a pivot table.
 *
 * The builder wraps an @ref ExcelPivotTableDefinition and forwards to
 * @ref Worksheet::CreatePivotTable on @ref Build. It is the shortest path from
 * a worksheet range to a finished report:
 *
 * @code
 * auto pivot = PivotTableBuilder(reportSheet)
 *                  .SetName("SalesByRegion")
 *                  .SetSource(*dataSheet, *CellRange::ParseA1("A1:D101"))
 *                  .SetTarget(*CellAddress::ParseA1("A3"))
 *                  .AddRowField("Region")
 *                  .AddColumnField("Quarter")
 *                  .AddDataField("Amount", PivotAggregateFunction::Sum)
 *                  .Build();
 * @endcode
 */
class EXYOKIOFFICE_EXPORT PivotTableBuilder
{
public:
    /**
     * @brief Creates a builder that inserts into @p sheet.
     *
     * @param sheet Worksheet that hosts the report. The builder keeps a shared
     * reference, so the worksheet wrapper must be alive when @ref Build is called.
     */
    explicit PivotTableBuilder(std::shared_ptr<Worksheet> sheet);

    /** @brief Sets the pivot table name. An empty name requests a generated one. */
    PivotTableBuilder& SetName(std::string name);

    /**
     * @brief Sets the source rectangle on the worksheet that hosts the report.
     * @param range Source rectangle including its header row.
     */
    PivotTableBuilder& SetSource(const CellRange& range);

    /**
     * @brief Sets the source rectangle on another worksheet of the same workbook.
     * @param sheet Worksheet that owns @p range.
     * @param range Source rectangle including its header row.
     */
    PivotTableBuilder& SetSource(const Worksheet& sheet, const CellRange& range);

    /**
     * @brief Sets the source rectangle by worksheet name.
     * @param sheetName Worksheet name; empty selects the hosting worksheet.
     * @param range Source rectangle including its header row.
     */
    PivotTableBuilder& SetSource(std::string sheetName, const CellRange& range);

    /** @brief Sets the top-left cell of the report on the hosting worksheet. */
    PivotTableBuilder& SetTarget(CellAddress targetCell);

    /**
     * @brief Appends a row field.
     * @param fieldName Source column header.
     * @param showSubtotal Emits a subtotal line for each group of this field.
     */
    PivotTableBuilder& AddRowField(std::string fieldName, bool showSubtotal = false);

    /**
     * @brief Appends a column field.
     * @param fieldName Source column header.
     * @param showSubtotal Emits a subtotal column for each group of this field.
     */
    PivotTableBuilder& AddColumnField(std::string fieldName, bool showSubtotal = false);

    /**
     * @brief Appends a page (report filter) field.
     * @param fieldName Source column header.
     * @param selectedItem Index of the selected item; `std::nullopt` selects `(All)`.
     */
    PivotTableBuilder& AddPageField(std::string fieldName,
                                    std::optional<UInt32> selectedItem = std::nullopt);

    /**
     * @brief Appends an aggregated value column.
     * @param fieldName Source column header to aggregate.
     * @param function Aggregate to apply.
     * @param displayName Display name; empty generates one such as `Sum of Amount`.
     */
    PivotTableBuilder& AddDataField(std::string fieldName,
                                    PivotAggregateFunction function = PivotAggregateFunction::Sum,
                                    std::string displayName = {});

    /** @brief Appends a fully configured data field. */
    PivotTableBuilder& AddDataField(ExcelPivotDataField dataField);

    /** @brief Enables or disables the row and column grand totals. */
    PivotTableBuilder& ShowGrandTotals(bool rows, bool columns);

    /** @brief Replaces the table-style presentation flags. */
    PivotTableBuilder& SetStyle(ExcelPivotTableStyle style);

    /** @brief Enables or disables writing the computed report into worksheet cells. */
    PivotTableBuilder& WriteCachedReport(bool write);

    /** @brief Enables or disables the refresh-on-load request. */
    PivotTableBuilder& RefreshOnLoad(bool refresh);

    /**
     * @brief Creates the pivot table.
     * @return The attached pivot table, or `nullptr` when the worksheet or the
     * accumulated definition is invalid. Use @ref Worksheet::CreatePivotTable
     * directly when the structured failure reason is needed.
     */
    ExcelPivotTable::Ptr Build();

    /** @brief Returns the accumulated definition without inserting it. */
    const ExcelPivotTableDefinition& Definition() const noexcept { return m_definition; }

private:
    std::shared_ptr<Worksheet> m_sheet;
    ExcelPivotTableDefinition m_definition;
};

/** @brief Validates a pivot table name against Excel's naming rules. */
[[nodiscard]] EXYOKIOFFICE_EXPORT bool IsValidPivotTableName(std::string_view name);

} // namespace ExyokiOffice::Excel
