// Copyright (c) 2026 Jakub Melka and Contributors
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#pragma once

#include "ExyokiOffice/Excel/ExcelAddress.hpp"
#include "ExyokiOffice/Export.hpp"
#include "ExyokiOffice/StandardTypes.hpp"

#include <optional>

namespace ExyokiOffice::Excel
{

/**
 * @brief Position inside an anchor cell, in English Metric Units.
 *
 * A two-cell drawing anchor names a cell for each corner and, within that
 * cell, how far from its top-left edge the corner sits. Zero offsets put the
 * corner exactly on the cell's top-left edge, which is what an anchor spanning
 * whole cells uses.
 */
struct EXYOKIOFFICE_EXPORT DrawingAnchorOffset
{
    /** @brief Distance from the cell's left edge, in EMU (914400 per inch). */
    Int64 Column = 0;

    /** @brief Distance from the cell's top edge, in EMU. */
    Int64 Row = 0;

    bool operator==(const DrawingAnchorOffset&) const = default;
};

/** @brief Size of a drawing object in English Metric Units. */
struct EXYOKIOFFICE_EXPORT DrawingExtent
{
    /** @brief Width in EMU (914400 per inch, 12700 per point). */
    Int64 Width = 0;

    /** @brief Height in EMU. */
    Int64 Height = 0;

    bool operator==(const DrawingExtent&) const = default;
};

/**
 * @brief Where a drawing object sits and how large it is.
 *
 * SpreadsheetML has two shapes of anchor. A *two-cell* anchor names the cell
 * of each corner plus an offset inside it, so the object's size is whatever
 * those cells add up to on the reader's screen - which depends on the default
 * font and the display DPI, neither of which the file records. A *one-cell*
 * anchor names the top-left cell and carries the size itself (`xdr:ext`), so
 * the object is exactly as large as requested wherever it is opened.
 *
 * @ref Worksheet::DrawingAnchorForSize fills in both forms for a requested
 * size: @ref Extent is the exact size, and @ref To / @ref ToOffset is the
 * two-cell equivalent under the worksheet's stored column widths and row
 * heights (Calibri 11 at 96 DPI metrics for the defaults). Images and charts
 * are written as one-cell anchors when @ref Extent is set; slicers always use
 * the two-cell form.
 */
struct EXYOKIOFFICE_EXPORT DrawingAnchor
{
    /** @brief Top-left anchor cell. */
    CellAddress From;

    /** @brief Offset of the top-left corner inside @ref From. */
    DrawingAnchorOffset FromOffset;

    /** @brief Cell holding the bottom-right corner of the two-cell form. */
    CellAddress To;

    /** @brief Offset of the bottom-right corner inside @ref To. */
    DrawingAnchorOffset ToOffset;

    /** @brief Exact size; when set, writers that support it use a one-cell anchor. */
    std::optional<DrawingExtent> Extent;
};

} // namespace ExyokiOffice::Excel
