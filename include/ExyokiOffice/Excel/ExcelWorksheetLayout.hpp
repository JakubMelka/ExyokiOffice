// Copyright (c) 2026 Jakub Melka and Contributors
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#pragma once

#include "ExyokiOffice/Excel/ExcelAddress.hpp"
#include "ExyokiOffice/Export.hpp"
#include "ExyokiOffice/StandardTypes.hpp"

#include <cstdint>
#include <optional>

namespace ExyokiOffice::Excel
{

/**
 * @brief Explicit presentation metadata for one worksheet row.
 *
 * Height is measured in points. An omitted
 * height leaves Excel's default row
 * height in effect. Outline levels are limited by SpreadsheetML to 0 through 7.
 *
 * ExyokiOffice stores these values without performing layout calculations.
 */
struct EXYOKIOFFICE_EXPORT RowDimension
{
    std::optional<Real> Height;
    bool Hidden = false;
    UInt8 OutlineLevel = 0;
    bool Collapsed = false;
};

/**
 * @brief Explicit presentation metadata for one worksheet column.
 *
 * Width uses SpreadsheetML's character-based
 * column-width unit. It is not a
 * pixel value and is never converted or auto-fitted by ExyokiOffice. An
 * omitted
 * width uses the workbook/application default. Outline levels are 0..7.
 */
struct EXYOKIOFFICE_EXPORT ColumnDimension
{
    std::optional<Real> Width;
    bool Hidden = false;
    UInt8 OutlineLevel = 0;
    bool Collapsed = false;
};

/**
 * @brief Editable metadata for the primary normal worksheet view.
 *
 * `frozenRows` and `frozenColumns` count rows
 * and columns frozen at the top and
 * left edge. Zero on both axes removes the pane. `activeCell` is independent
 * of
 * freezing and defaults to A1 when a new view is materialized.
 */
struct EXYOKIOFFICE_EXPORT WorksheetView
{
    std::optional<CellAddress> ActiveCell;
    UInt32 FrozenRows = 0;
    UInt32 FrozenColumns = 0;
};

/** @brief Validates row presentation metadata without modifying a worksheet. */
[[nodiscard]] EXYOKIOFFICE_EXPORT bool IsValidRowDimension(const RowDimension& dimension) noexcept;
/** @brief Validates column presentation metadata without modifying a worksheet. */
[[nodiscard]] EXYOKIOFFICE_EXPORT bool IsValidColumnDimension(const ColumnDimension& dimension) noexcept;
/** @brief Validates view bounds without modifying a worksheet. */
[[nodiscard]] EXYOKIOFFICE_EXPORT bool IsValidWorksheetView(const WorksheetView& view) noexcept;

} // namespace ExyokiOffice::Excel
