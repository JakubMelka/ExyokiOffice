// Copyright (c) 2026 Jakub Melka and Contributors
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#pragma once

#include "ExyokiOffice/DOM/DocumentFormat/OpenXml/Office2010/Excel.hpp"
#include "ExyokiOffice/Excel/ExcelAddress.hpp"
#include "ExyokiOffice/Export.hpp"
#include "ExyokiOffice/StandardTypes.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ExyokiOffice::Packaging
{
class SlicerCachePart;
class SlicersPart;
class ExcelDocument;
} // namespace ExyokiOffice::Packaging

namespace ExyokiOffice::Excel
{

class Worksheet;

/**
 * @brief Kind of object a slicer filters.
 *
 * The two kinds are stored very differently. A pivot slicer keeps its item list
 * and the current selection inside the slicer cache, using the Excel 2010
 * (`x14`) markup. A table slicer cannot do that, because the cache's tabular
 * data requires a pivot cache identifier, so it is described by an Excel 2013
 * (`x15`) extension and its selection lives in the table's auto-filter.
 */
enum class SlicerSourceKind
{
    /** The slicer filters a pivot table field. */
    PivotTable,
    /** The slicer filters a worksheet table column. */
    Table
};

/** @brief Order the slicer buttons are listed in. */
enum class SlicerSortOrder
{
    /** Ascending, which is Excel's default. */
    Ascending,
    /** Descending. */
    Descending
};

/**
 * @brief How a slicer reacts to filters applied by other slicers on the same cache.
 */
enum class SlicerCrossFilter
{
    /** Cross-filtering is disabled and every button stays enabled. */
    None,
    /** Buttons whose items still have data are listed first. */
    ShowItemsWithDataAtTop,
    /** Buttons keep their position but items without data are greyed out. */
    ShowItemsWithNoData
};

/**
 * @brief One selectable value of a slicer.
 *
 * The three members map exactly onto what the slicer cache can express for a
 * single item: its caption, whether it is currently selected, and whether the
 * source no longer contains a matching row.
 */
struct EXYOKIOFFICE_EXPORT ExcelSlicerItem
{
    /** @brief Display caption of the button. */
    std::string Caption;
    /** @brief True when the item is selected, that is, not filtered out. */
    bool Selected = true;
    /** @brief True when no source row matches the item and the button is greyed out. */
    bool HasNoData = false;
};

/**
 * @brief Complete description of a slicer and the object it filters.
 *
 * The definition is consumed by @ref Worksheet::CreateSlicer and by
 * @ref ExcelSlicer::Update, and is reproduced by @ref ExcelSlicer::Definition.
 * @ref SlicerBuilder assembles the same structure through a fluent interface.
 *
 * @code
 * ExcelSlicerDefinition definition;
 * definition.SourceKind     = SlicerSourceKind::PivotTable;
 * definition.PivotTableName = "SalesByRegion";
 * definition.SourceField    = "Region";
 * definition.From           = *CellAddress::ParseA1("F2");
 * definition.To             = *CellAddress::ParseA1("H12");
 * auto slicer = reportSheet->CreateSlicer(definition).Slicer;
 * @endcode
 */
struct EXYOKIOFFICE_EXPORT ExcelSlicerDefinition
{
    /**
     * @brief Slicer name, unique in the workbook (case-insensitive).
     *
     * The name must be non-empty and at most 255 characters. When empty, a
     * unique `SlicerN` name is generated.
     */
    std::string Name;

    /** @brief Caption shown in the slicer header. Empty uses @ref sourceField. */
    std::string Caption;

    /** @brief Whether the slicer filters a pivot table or a worksheet table. */
    SlicerSourceKind SourceKind = SlicerSourceKind::PivotTable;

    /**
     * @brief Pivot table name when @ref sourceKind is SlicerSourceKind::PivotTable.
     *
     * The pivot table must exist somewhere in the same workbook; it does not
     * have to live on the worksheet that hosts the slicer.
     */
    std::string PivotTableName;

    /** @brief Table display name when @ref sourceKind is SlicerSourceKind::Table. */
    std::string TableName;

    /**
     * @brief Source column, matched case-insensitively.
     *
     * For a pivot slicer this is a pivot cache field name, which is the header
     * of the corresponding source column. For a table slicer it is a table
     * column name. Required in both cases.
     */
    std::string SourceField;

    /** @brief Top-left anchor cell of the visible shape. */
    CellAddress From;

    /** @brief Bottom-right anchor cell of the visible shape. */
    CellAddress To;

    /**
     * @brief Drawing object identifier of the visible shape.
     *
     * Zero allocates the next free identifier. Slicers, charts, and images on
     * one worksheet share a single identifier space.
     */
    UInt32 Id = 0;

    /** @brief Number of button columns. Must be between 1 and 20000. */
    UInt32 ColumnCount = 1;

    /**
     * @brief Height of one button row, in English Metric Units.
     *
     * The attribute is required by the schema, so the value must stay non-zero.
     * The default matches the row height Excel writes for a default slicer.
     */
    UInt32 RowHeight = 241300;

    /** @brief Slicer style name, for example `SlicerStyleLight1`. */
    std::string Style = "SlicerStyleLight1";

    /** @brief Shows the caption header above the buttons. */
    bool ShowCaption = true;

    /** @brief Prevents the user from moving or resizing the slicer. */
    bool LockedPosition = false;

    /** @brief Zero-based index of the first visible button when the list is scrolled. */
    UInt32 StartItem = 0;

    /** @brief OLAP hierarchy level. Keep zero for worksheet-backed sources. */
    UInt32 Level = 0;

    /** @brief Button order. */
    SlicerSortOrder SortOrder = SlicerSortOrder::Ascending;

    /** @brief Cross-filter behavior against other slicers on the same cache. */
    SlicerCrossFilter CrossFilter = SlicerCrossFilter::ShowItemsWithDataAtTop;

    /**
     * @brief Keeps buttons for items that no longer occur in the source.
     *
     * @note This flag exists only in the Excel 2010 markup and is therefore
     * ignored for a table slicer.
     */
    bool ShowMissing = true;

    /** @brief Honours the workbook's custom sort lists when ordering buttons. */
    bool CustomListSort = true;

    /**
     * @brief Captions of the selected items. An empty vector selects everything.
     *
     * Selection is expressed by caption rather than by index because cache item
     * indexes change whenever the source data is refreshed, while captions
     * survive a refresh.
     */
    std::vector<std::string> SelectedItems;

    /**
     * @brief Writes the visible slicer shape into the worksheet drawing.
     *
     * When disabled, only the slicer part, the cache part, and the registry
     * entries are written. The filter is then fully defined but no shape is
     * shown until a spreadsheet application places one.
     */
    bool WriteDrawing = true;
};

/** @brief Reason a slicer operation was rejected. */
enum class SlicerError
{
    /** The operation completed successfully. */
    None,
    /** The worksheet wrapper, workbook, or slicer part is detached. */
    InvalidWorksheet,
    /** The slicer name is invalid or already used in this workbook. */
    InvalidName,
    /** The named pivot table or table does not exist in this workbook. */
    UnknownSource,
    /** The source column is not a pivot cache field or a table column. */
    UnknownField,
    /** The drawing anchor cells are missing or the rectangle is inverted. */
    InvalidAnchor,
    /** A presentation attribute is outside its permitted range. */
    InvalidPresentation,
    /** A selected item caption does not occur in the source column. */
    UnknownItem,
    /** A package part, relationship, or XML write failed. */
    WriteFailed
};

/** @brief Structured result of a slicer operation. */
struct EXYOKIOFFICE_EXPORT SlicerResult
{
    /** @brief Error code; @ref SlicerError::None on success. */
    SlicerError Error = SlicerError::None;
    /** @brief Human-readable diagnostic message; empty on success. */
    std::string Message;

    /** @brief Returns true when the operation completed successfully. */
    [[nodiscard]] bool Succeeded() const noexcept { return Error == SlicerError::None; }
    /** @brief Provides convenient success testing in conditional statements. */
    [[nodiscard]] explicit operator bool() const noexcept { return Succeeded(); }
};

/**
 * @brief High-level wrapper around one slicer and its slicer cache.
 *
 * A wrapper stays valid while its package parts remain attached to the workbook
 * and while a slicer of the remembered name exists in them. It is obtained from
 * @ref Worksheet::CreateSlicer, @ref Worksheet::Slicers, or
 * @ref Worksheet::SlicerByName; removing a slicer through
 * @ref Worksheet::RemoveSlicer invalidates wrappers retained by callers.
 *
 * Unlike a pivot table, several slicers share one slicers part, so a wrapper is
 * identified by the slicers part **and** the slicer name. Renaming through
 * @ref SetName keeps the wrapper usable; renaming the underlying element by any
 * other means does not.
 *
 * @note Reading properties of a slicer that was authored elsewhere and has no
 * resolvable cache part is supported and degrades to empty results rather than
 * failing, so imported workbooks can be enumerated safely.
 */
class EXYOKIOFFICE_EXPORT ExcelSlicer
{
public:
    using Ptr = std::shared_ptr<ExcelSlicer>;

    /**
     * @brief Creates a wrapper for an existing slicer.
     *
     * Normal callers obtain wrappers from @ref Worksheet instead of using this
     * constructor directly.
     *
     * @param part Slicers part attached to a worksheet.
     * @param name Name of the slicer inside @p part.
     * @param document Workbook that owns @p part.
     */
    ExcelSlicer(std::shared_ptr<Packaging::SlicersPart> part,
                std::string name,
                std::shared_ptr<Packaging::ExcelDocument> document);

    /** @brief Returns true when the wrapper resolves to an existing slicer. */
    [[nodiscard]] bool IsValid() const noexcept;

    /** @brief Returns the slicer name, or an empty string when detached. */
    std::string Name() const;

    /**
     * @brief Renames the slicer.
     *
     * The new name must be non-empty, at most 255 characters, and unique across
     * every slicer in the workbook when compared case-insensitively. The visible
     * shape is renamed together with the slicer, because the drawing refers to
     * the slicer by name rather than by relationship.
     */
    SlicerResult SetName(std::string_view name);

    /** @brief Returns the caption shown in the slicer header. */
    std::string Caption() const;
    /** @brief Replaces the caption shown in the slicer header. */
    SlicerResult SetCaption(std::string_view caption);

    /** @brief Returns whether the slicer filters a pivot table or a table. */
    SlicerSourceKind SourceKind() const;

    /** @brief Returns the pivot table or table name that feeds this slicer. */
    std::string SourceObjectName() const;

    /** @brief Returns the pivot cache field or table column name being filtered. */
    std::string SourceField() const;

    /**
     * @brief Returns every item of the source column with its selection state.
     *
     * For a pivot slicer the items and their selection come from the slicer
     * cache. For a table slicer the items are recomputed from the table's cells
     * and the selection is read from the table's auto-filter.
     *
     * @return Items in button order, or an empty vector when the source cannot
     * be resolved.
     */
    std::vector<ExcelSlicerItem> Items() const;

    /**
     * @brief Selects exactly the listed item captions.
     *
     * @param captions Captions to select, matched case-insensitively. An empty
     * vector selects every item, which is how Excel represents an unfiltered
     * slicer.
     * @return @ref SlicerError::UnknownItem when a caption does not occur in the
     * source column.
     */
    SlicerResult SelectItems(const std::vector<std::string>& captions);

    /** @brief Returns the two-cell anchor of the visible shape, when one exists. */
    std::optional<std::pair<CellAddress, CellAddress>> Anchor() const;

    /**
     * @brief Moves or resizes the visible shape.
     *
     * @param from Top-left anchor cell.
     * @param to Bottom-right anchor cell.
     * @return @ref SlicerError::InvalidAnchor for an invalid or inverted
     * rectangle, or when the slicer was created without a shape.
     */
    SlicerResult SetAnchor(CellAddress from, CellAddress to);

    /** @brief Returns the number of button columns. */
    UInt32 ColumnCount() const;
    /** @brief Returns the slicer style name. */
    std::string Style() const;
    /** @brief Returns true when the caption header is shown. */
    [[nodiscard]] bool ShowCaption() const;
    /** @brief Returns true when the user cannot move or resize the slicer. */
    [[nodiscard]] bool LockedPosition() const;
    /** @brief Returns the button order. */
    SlicerSortOrder SortOrder() const;
    /** @brief Returns the cross-filter behavior. */
    SlicerCrossFilter CrossFilter() const;

    /**
     * @brief Reconstructs the definition that produced the current slicer.
     *
     * The result can be modified and passed back to @ref Update.
     *
     * @return The definition, or `std::nullopt` when the wrapper is detached.
     */
    std::optional<ExcelSlicerDefinition> Definition() const;

    /**
     * @brief Replaces the presentation, behavior, and selection of the slicer.
     *
     * The slicer keeps its package parts, its cache, and its relationships, so
     * anything that already refers to it stays valid.
     *
     * Changing @ref ExcelSlicerDefinition::sourceKind,
     * @ref ExcelSlicerDefinition::pivotTableName,
     * @ref ExcelSlicerDefinition::tableName, or
     * @ref ExcelSlicerDefinition::sourceField is rejected with
     * @ref SlicerError::UnknownSource; remove the slicer and create a new one
     * instead. On failure the workbook is left unchanged.
     */
    SlicerResult Update(const ExcelSlicerDefinition& definition);

    /** @brief Exposes the slicers part for advanced Open XML access. */
    std::shared_ptr<Packaging::SlicersPart> GetPart() const;
    /**
     * @brief Exposes the slicer cache part for advanced Open XML access.
     * @return The cache part, or nullptr when it cannot be resolved.
     */
    std::shared_ptr<Packaging::SlicerCachePart> GetCachePart() const;
    /** @brief Exposes the generated `x14:slicer` element backing this wrapper. */
    std::shared_ptr<DocumentFormat::OpenXml::Office2010::Excel::Slicer> GetLowLevelApi() const;

private:
    friend class Worksheet;

    std::shared_ptr<Packaging::SlicersPart> m_part;
    std::string m_name;
    std::shared_ptr<Packaging::ExcelDocument> m_document;
};

/**
 * @brief Result of creating a slicer: the attached wrapper and the status.
 *
 * Returned by @ref Worksheet::CreateSlicer so that the failure reason travels
 * with the slicer instead of through an out-parameter.
 */
struct EXYOKIOFFICE_EXPORT SlicerCreationResult
{
    /** @brief Structured status; @ref SlicerError::None on success. */
    SlicerResult Status;
    /** @brief The attached slicer, or nullptr when @ref Status reports a failure. */
    ExcelSlicer::Ptr Slicer;

    /** @brief Returns true when the slicer was created. */
    [[nodiscard]] bool Succeeded() const noexcept { return Status.Succeeded(); }
    /** @brief Provides convenient success testing in conditional statements. */
    [[nodiscard]] explicit operator bool() const noexcept { return Succeeded(); }
};

/**
 * @brief Fluent builder that assembles and inserts a slicer.
 *
 * The builder wraps an @ref ExcelSlicerDefinition and forwards to
 * @ref Worksheet::CreateSlicer on @ref Build:
 *
 * @code
 * auto slicer = SlicerBuilder(reportSheet)
 *                   .SetName("RegionSlicer")
 *                   .SetPivotSource("SalesByRegion", "Region")
 *                   .SetAnchor(*CellAddress::ParseA1("F2"), *CellAddress::ParseA1("H12"))
 *                   .SetColumnCount(2)
 *                   .SelectItems({"East", "West"})
 *                   .Build();
 * @endcode
 */
class EXYOKIOFFICE_EXPORT SlicerBuilder
{
public:
    /**
     * @brief Creates a builder that inserts into @p sheet.
     *
     * @param sheet Worksheet that hosts the slicer. The builder keeps a shared
     * reference, so the worksheet wrapper must be alive when @ref Build is called.
     */
    explicit SlicerBuilder(std::shared_ptr<Worksheet> sheet);

    /** @brief Sets the slicer name. An empty name requests a generated one. */
    SlicerBuilder& SetName(std::string name);
    /** @brief Sets the caption shown in the slicer header. */
    SlicerBuilder& SetCaption(std::string caption);
    /**
     * @brief Filters a pivot table field.
     * @param pivotTableName Pivot table anywhere in the same workbook.
     * @param fieldName Pivot cache field name, that is, a source column header.
     */
    SlicerBuilder& SetPivotSource(std::string pivotTableName, std::string fieldName);
    /**
     * @brief Filters a worksheet table column.
     * @param tableName Table display name anywhere in the same workbook.
     * @param columnName Table column name.
     */
    SlicerBuilder& SetTableSource(std::string tableName, std::string columnName);
    /** @brief Sets the two-cell anchor of the visible shape. */
    SlicerBuilder& SetAnchor(CellAddress from, CellAddress to);
    /** @brief Sets the number of button columns. */
    SlicerBuilder& SetColumnCount(UInt32 columns);
    /** @brief Sets the slicer style name. */
    SlicerBuilder& SetStyle(std::string style);
    /** @brief Shows or hides the caption header. */
    SlicerBuilder& ShowCaption(bool show);
    /** @brief Sets the button order. */
    SlicerBuilder& SetSortOrder(SlicerSortOrder order);
    /** @brief Sets the cross-filter behavior. */
    SlicerBuilder& SetCrossFilter(SlicerCrossFilter crossFilter);
    /** @brief Selects the listed captions. An empty vector selects everything. */
    SlicerBuilder& SelectItems(std::vector<std::string> captions);
    /** @brief Enables or disables writing the visible shape. */
    SlicerBuilder& WriteDrawing(bool write);

    /**
     * @brief Creates the slicer.
     * @return The attached slicer, or nullptr when the worksheet or the
     * accumulated definition is invalid. Use @ref Worksheet::CreateSlicer
     * directly when the structured failure reason is needed.
     */
    ExcelSlicer::Ptr Build();

    /** @brief Returns the accumulated definition without inserting it. */
    const ExcelSlicerDefinition& Definition() const noexcept { return m_definition; }

private:
    std::shared_ptr<Worksheet> m_sheet;
    ExcelSlicerDefinition m_definition;
};

/** @brief Validates a slicer name against Excel's naming rules. */
[[nodiscard]] EXYOKIOFFICE_EXPORT bool IsValidSlicerName(std::string_view name);

} // namespace ExyokiOffice::Excel
