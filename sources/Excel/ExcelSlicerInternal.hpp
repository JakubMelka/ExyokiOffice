// Copyright (c) 2026 Jakub Melka and Contributors
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#pragma once

#include "ExyokiOffice/Excel/ExcelSlicer.hpp"
#include "ExyokiOffice/StandardTypes.hpp"

#include <cstdint>
#include <memory>
#include <string_view>

namespace ExyokiOffice::Packaging
{
class ExcelDocument;
} // namespace ExyokiOffice::Packaging

namespace ExyokiOffice::Excel::SlicerDetail
{

/**
 * @brief Detaches every slicer that filtered a pivot table that is being removed.
 *
 * A slicer cache lists the pivot tables it drives in `x14:pivotTables`. Removing
 * a pivot table without updating that list leaves a dangling reference that makes
 * Excel repair the file, so this drops the matching `x14:pivotTable` entry from
 * every cache and, when a cache loses its last pivot table, removes the whole
 * slicer chain built on it: the slicer elements referencing it by `@cache`, their
 * drawing anchors, the workbook registry entry, and the cache part.
 *
 * @param document Workbook that owns the removed pivot table.
 * @param pivotTableName Name of the pivot table that has just been removed.
 */
void DetachPivotTableFromSlicers(const std::shared_ptr<Packaging::ExcelDocument>& document,
                                 std::string_view pivotTableName);

/**
 * @brief Detaches every slicer that filtered a worksheet table that is being removed.
 *
 * A table slicer's cache carries `x15:tableSlicerCache/@tableId`. Removing the
 * table orphans it, so the whole slicer chain for that cache is removed.
 *
 * @param document Workbook that owns the removed table.
 * @param tableId Identifier of the table that has just been removed.
 */
void DetachTableFromSlicers(const std::shared_ptr<Packaging::ExcelDocument>& document, UInt32 tableId);

/**
 * @brief Follows a pivot table rename in every slicer cache that refers to it.
 *
 * Slicer caches identify their pivot tables by name, so a rename that is not
 * mirrored here silently orphans the slicers.
 *
 * @param document Workbook that owns the pivot table.
 * @param oldName Name the pivot table had before the rename.
 * @param newName Name the pivot table has now.
 */
void RenamePivotTableInSlicers(const std::shared_ptr<Packaging::ExcelDocument>& document,
                               std::string_view oldName,
                               std::string_view newName);

/**
 * @brief Re-derives the cached items of every slicer built on a pivot table.
 *
 * Rebuilding a pivot cache renumbers the shared items, so slicer item indexes
 * that were correct before the rebuild would silently select the wrong values
 * afterwards. This re-reads the cache field and rewrites the slicer items,
 * preserving the selection by caption rather than by index.
 *
 * @param document Workbook that owns the pivot table.
 * @param pivotTableName Name of the pivot table whose cache was rebuilt.
 */
void RefreshSlicersForPivotTable(const std::shared_ptr<Packaging::ExcelDocument>& document,
                                 std::string_view pivotTableName);

} // namespace ExyokiOffice::Excel::SlicerDetail
