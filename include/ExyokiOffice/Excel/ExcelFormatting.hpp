// Copyright (c) 2026 Jakub Melka and Contributors
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#pragma once

#include "ExyokiOffice/Excel/ExcelDocument.hpp"
#include "ExyokiOffice/Excel/ExcelStyle.hpp"
#include "ExyokiOffice/Export.hpp"

#include <optional>

namespace ExyokiOffice::Excel
{

/**
 * @brief Partial modification of a cell style.
 *
 * A delta describes how one existing ExcelStyle should be changed. Each style
 * component is independently tri-state:
 *
 * - the matching optional member is unset and the matching `clear...` flag is
 *   false: the component is left exactly as it is;
 * - the optional member is set: the component is replaced;
 * - the `clear...` flag is true: the component is removed so the cell inherits
 *   the workbook default again.
 *
 * Setting a component and clearing it at the same time is contradictory;
 * ApplyTo() resolves it in favor of the explicit value, and CellFormatter
 * rejects it as an invalid style before touching the document.
 *
 * Deltas replace whole components rather than individual fields. To change one
 * font attribute while preserving the rest, read the current style with
 * StyleRepository::GetCellStyle(), copy its font, adjust it, and assign the
 * result:
 *
 * @code
 * auto style = editor->Styles().GetCellStyle(*sheet, address).value_or(ExcelStyle{});
 * auto font = style.Font.value_or(ExcelFont{});
 * font.Bold = true;
 *
 * ExcelStyleDelta delta;
 * delta.Font = font;
 * formatter.Modify(*sheet, address, delta);
 * @endcode
 */
struct EXYOKIOFFICE_EXPORT ExcelStyleDelta
{
    /** Replacement number format. */
    std::optional<ExcelNumberFormat> NumberFormat;
    /** Replacement font definition. */
    std::optional<ExcelFont> Font;
    /** Replacement fill definition. */
    std::optional<ExcelFill> Fill;
    /** Replacement border definition. */
    std::optional<ExcelBorder> Border;
    /** Replacement alignment properties. */
    std::optional<ExcelAlignment> Alignment;
    /** Replacement locked/hidden protection flags. */
    std::optional<ExcelProtection> Protection;
    /** Replacement quote-prefix flag. */
    std::optional<bool> QuotePrefix;
    /** Replacement pivot-button flag. */
    std::optional<bool> PivotButton;

    /** Remove the number format so the cell falls back to General. */
    bool ClearNumberFormat = false;
    /** Remove the font so the cell falls back to the default font. */
    bool ClearFont = false;
    /** Remove the fill so the cell falls back to no fill. */
    bool ClearFill = false;
    /** Remove the border so the cell falls back to no border. */
    bool ClearBorder = false;
    /** Remove alignment properties so the cell falls back to General/Bottom. */
    bool ClearAlignment = false;
    /** Remove protection flags so the cell falls back to locked and not hidden. */
    bool ClearProtection = false;

    /** @brief Returns true when the delta would not change anything. */
    [[nodiscard]] bool IsEmpty() const noexcept;
    /**
     * @brief Returns the style that results from applying this delta to a base style.
     *
     * The base style is not modified. Components that the delta does not
     * mention are copied unchanged.
     */
    ExcelStyle ApplyTo(const ExcelStyle& base) const;

    bool operator==(const ExcelStyleDelta&) const = default;
};

/**
 * @brief Border decoration described relative to a rectangular range.
 *
 * The four outline edges apply to the cells on the corresponding border of the
 * range; the two inside edges apply to every shared edge between neighboring
 * cells inside the range. A one-cell range therefore only uses the outline
 * edges, and a single-column range never uses `insideVertical`.
 *
 * Unlike ExcelBorder, this model is range-relative: applying it produces a
 * different per-cell ExcelBorder for corner, edge, and interior cells so that
 * the visual result is a single frame rather than a box around every cell.
 */
struct EXYOKIOFFICE_EXPORT ExcelRangeBorder
{
    /** Edge drawn on the left side of the leftmost column. */
    ExcelBorderSide OutlineLeft;
    /** Edge drawn on the right side of the rightmost column. */
    ExcelBorderSide OutlineRight;
    /** Edge drawn on the top side of the first row. */
    ExcelBorderSide OutlineTop;
    /** Edge drawn on the bottom side of the last row. */
    ExcelBorderSide OutlineBottom;
    /** Edge drawn between horizontally adjacent cells inside the range. */
    ExcelBorderSide InsideVertical;
    /** Edge drawn between vertically adjacent cells inside the range. */
    ExcelBorderSide InsideHorizontal;

    /**
     * @brief Creates a frame around the range without inner lines.
     *
     * @param style Line style used for all four outline edges.
     * @param color Optional line color; omitted colors use Excel's automatic color.
     */
    static ExcelRangeBorder Box(ExcelBorderStyle style, std::optional<ExcelColor> color = std::nullopt);
    /**
     * @brief Creates a frame around the range plus a full inner grid.
     *
     * @param outlineStyle Line style used for the four outline edges.
     * @param insideStyle Line style used for both inner edge directions.
     * @param color Optional line color applied to every edge.
     */
    static ExcelRangeBorder Grid(ExcelBorderStyle outlineStyle,
                                 ExcelBorderStyle insideStyle,
                                 std::optional<ExcelColor> color = std::nullopt);
    /**
     * @brief Creates a border definition that erases all edges in the range.
     *
     * Applying the result clears outer and inner lines while leaving each
     * cell's diagonal border settings untouched.
     */
    static ExcelRangeBorder None();

    bool operator==(const ExcelRangeBorder&) const = default;
};

/**
 * @brief Read-modify-write formatting service for worksheet cells and ranges.
 *
 * StyleRepository works with whole style definitions and numeric style
 * indices, which is the right primitive for authoring a workbook from scratch
 * but inconvenient for editing one: changing the fill of an already formatted
 * cell would otherwise discard its font, border, and number format.
 *
 * CellFormatter closes that gap. Every operation reads the current style of
 * each target cell, applies the requested change, registers the resulting
 * definition through the workbook StyleRepository (which deduplicates it), and
 * writes the resulting index back. Cells in one range that start with
 * different styles therefore stay different, each keeping the parts of its
 * formatting that the operation does not mention.
 *
 * The formatter is a lightweight value that references the document it was
 * created from; it holds no cache and stays correct across package round
 * trips. All mutating operations are atomic per call: the worksheet XML is
 * restored when any individual cell write fails.
 *
 * Example:
 * @code
 * auto editor = ExyokiOffice::Excel::ExcelDocumentEditor::CreateNew();
 * auto sheet = editor->FirstWorksheet();
 * ExyokiOffice::Excel::CellFormatter formatter(editor->GetDocument());
 *
 * const auto header = *ExyokiOffice::Excel::CellRange::ParseA1("A1:C1");
 * ExyokiOffice::Excel::ExcelFont bold;
 * bold.Bold = true;
 * formatter.SetFont(*sheet, header, bold);
 * formatter.ApplyRangeBorder(*sheet, header,
 *     ExyokiOffice::Excel::ExcelRangeBorder::Box(ExyokiOffice::Excel::ExcelBorderStyle::Medium));
 * @endcode
 */
class EXYOKIOFFICE_EXPORT CellFormatter
{
public:
    /** @brief Creates a detached, invalid formatter. */
    CellFormatter() = default;
    /**
     * @brief Creates a formatter bound to an Excel document.
     *
     * @param document Workbook whose stylesheet receives the registered styles.
     */
    explicit CellFormatter(ExcelDocument::Ptr document);
    /**
     * @brief Creates a formatter bound to the document of an editor.
     *
     * @param editor Editor whose document is formatted; a null editor produces
     *        an invalid formatter.
     */
    explicit CellFormatter(const ExcelDocumentEditor::Ptr& editor);

    /** @brief Returns true when a workbook document is attached. */
    [[nodiscard]] bool IsValid() const noexcept;
    /** @brief Returns the underlying style repository. */
    StyleRepository Styles() const;

    /**
     * @brief Reads the complete style definition applied to one cell.
     *
     * @param worksheet Worksheet that owns the cell.
     * @param address Validated worksheet cell address.
     * @return Style definition, or std::nullopt when the cell cannot be resolved.
     */
    std::optional<ExcelStyle> GetStyle(const Worksheet& worksheet, CellAddress address) const;

    /**
     * @brief Replaces the complete style of one cell.
     *
     * Unlike Modify(), no part of the previous formatting is preserved. A
     * missing sparse cell is materialized as a blank styled cell.
     *
     * @param worksheet Target worksheet.
     * @param address Validated worksheet cell address.
     * @param style Complete replacement style.
     * @return Result reporting the number of styled cells or the failure reason.
     */
    RangeOperationResult SetStyle(Worksheet& worksheet, CellAddress address, const ExcelStyle& style);
    /**
     * @brief Replaces the complete style of every cell in a rectangular range.
     *
     * @param worksheet Target worksheet.
     * @param range Validated rectangular range.
     * @param style Complete replacement style shared by all cells in the range.
     * @return Result reporting the number of styled cells or the failure reason.
     */
    RangeOperationResult SetStyle(Worksheet& worksheet, CellRange range, const ExcelStyle& style);

    /**
     * @brief Merges a style delta into the current style of one cell.
     *
     * @param worksheet Target worksheet.
     * @param address Validated worksheet cell address.
     * @param delta Components to replace or clear.
     * @return Result reporting the number of styled cells or the failure reason.
     */
    RangeOperationResult Modify(Worksheet& worksheet, CellAddress address, const ExcelStyleDelta& delta);
    /**
     * @brief Merges a style delta into the current style of every cell in a range.
     *
     * Each cell is merged with its own previous style, so a range holding
     * different styles keeps those differences in the components that the
     * delta does not mention.
     *
     * @param worksheet Target worksheet.
     * @param range Validated rectangular range.
     * @param delta Components to replace or clear.
     * @return Result reporting the number of styled cells or the failure reason.
     */
    RangeOperationResult Modify(Worksheet& worksheet, CellRange range, const ExcelStyleDelta& delta);

    /**
     * @brief Applies a number format while preserving all other formatting.
     *
     * @param worksheet Target worksheet.
     * @param range Validated rectangular range.
     * @param format Built-in or custom number format to apply.
     * @return Result reporting the number of styled cells or the failure reason.
     */
    RangeOperationResult SetNumberFormat(Worksheet& worksheet, CellRange range, const ExcelNumberFormat& format);
    /**
     * @brief Applies a font while preserving all other formatting.
     *
     * @param worksheet Target worksheet.
     * @param range Validated rectangular range.
     * @param font Complete replacement font.
     * @return Result reporting the number of styled cells or the failure reason.
     */
    RangeOperationResult SetFont(Worksheet& worksheet, CellRange range, const ExcelFont& font);
    /**
     * @brief Applies a fill while preserving all other formatting.
     *
     * @param worksheet Target worksheet.
     * @param range Validated rectangular range.
     * @param fill Complete replacement pattern or gradient fill.
     * @return Result reporting the number of styled cells or the failure reason.
     */
    RangeOperationResult SetFill(Worksheet& worksheet, CellRange range, const ExcelFill& fill);
    /**
     * @brief Applies a solid or hatched pattern fill while preserving all other formatting.
     *
     * This is a shorthand for the common pattern-fill case. Excel draws the
     * pattern itself in the foreground color; for ExcelFillPattern::Solid only
     * the foreground color is visible.
     *
     * @param worksheet Target worksheet.
     * @param range Validated rectangular range.
     * @param pattern Fill pattern to apply.
     * @param foreground Color of the pattern lines, or of the whole cell for solid fills.
     * @param background Optional color drawn behind hatched and dotted patterns.
     * @return Result reporting the number of styled cells or the failure reason.
     */
    RangeOperationResult SetFillPattern(Worksheet& worksheet,
                                        CellRange range,
                                        ExcelFillPattern pattern,
                                        const ExcelColor& foreground,
                                        std::optional<ExcelColor> background = std::nullopt);
    /**
     * @brief Applies a per-cell border definition while preserving all other formatting.
     *
     * Every cell in the range receives the same border, so an outlined range
     * requires ApplyRangeBorder() instead.
     *
     * @param worksheet Target worksheet.
     * @param range Validated rectangular range.
     * @param border Complete replacement border.
     * @return Result reporting the number of styled cells or the failure reason.
     */
    RangeOperationResult SetBorder(Worksheet& worksheet, CellRange range, const ExcelBorder& border);
    /**
     * @brief Applies alignment properties while preserving all other formatting.
     *
     * @param worksheet Target worksheet.
     * @param range Validated rectangular range.
     * @param alignment Complete replacement alignment properties.
     * @return Result reporting the number of styled cells or the failure reason.
     */
    RangeOperationResult SetAlignment(Worksheet& worksheet, CellRange range, const ExcelAlignment& alignment);
    /**
     * @brief Applies cell protection flags while preserving all other formatting.
     *
     * Cell protection only takes effect once the worksheet itself is protected
     * through Worksheet::Protect().
     *
     * @param worksheet Target worksheet.
     * @param range Validated rectangular range.
     * @param protection Locked and hidden flags to apply.
     * @return Result reporting the number of styled cells or the failure reason.
     */
    RangeOperationResult SetProtection(Worksheet& worksheet, CellRange range, const ExcelProtection& protection);
    /**
     * @brief Marks cells as locked or unlocked while preserving all other formatting.
     *
     * Excel treats cells as locked by default, so unlocking the input cells of
     * a protected worksheet is the usual way to build a fill-in form.
     *
     * @param worksheet Target worksheet.
     * @param range Validated rectangular range.
     * @param locked True to lock the cells, false to leave them editable while
     *        the worksheet is protected.
     * @return Result reporting the number of styled cells or the failure reason.
     */
    RangeOperationResult SetLocked(Worksheet& worksheet, CellRange range, bool locked);

    /**
     * @brief Draws a range-relative frame and optional inner grid.
     *
     * Each cell receives only the edges that its position in the range
     * justifies, so the range is framed once instead of every cell being boxed.
     * Existing diagonal borders, `diagonalUp`, `diagonalDown`, and `outline`
     * settings of each cell are preserved; all other edges are replaced.
     *
     * @param worksheet Target worksheet.
     * @param range Validated rectangular range.
     * @param border Range-relative border description.
     * @return Result reporting the number of styled cells or the failure reason.
     */
    RangeOperationResult ApplyRangeBorder(Worksheet& worksheet, CellRange range, const ExcelRangeBorder& border);

private:
    ExcelDocument::Ptr m_document;
};

} // namespace ExyokiOffice::Excel
