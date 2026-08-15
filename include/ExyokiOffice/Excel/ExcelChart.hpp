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
#include <vector>

namespace ExyokiOffice::Excel
{

class Worksheet;

/** @brief Chart plot type produced by the chart builder. */
enum class ExcelChartType
{
    /** Vertical clustered column chart. */
    Column,
    /** Horizontal clustered bar chart. */
    Bar,
    /** Line chart with category axis. */
    Line,
    /** Pie chart from a single series. */
    Pie,
    /** Area chart with category axis. */
    Area,
    /** XY scatter chart from paired numeric ranges. */
    XyScatter,
    /** Bubble chart from paired numeric ranges plus bubble sizes. */
    Bubble
};

/** @brief Placement of the chart legend. */
enum class ExcelLegendPosition
{
    /** No legend is drawn. */
    None,
    /** Legend to the right of the plot area. */
    Right,
    /** Legend to the left of the plot area. */
    Left,
    /** Legend above the plot area. */
    Top,
    /** Legend below the plot area. */
    Bottom
};

/**
 * @brief One data series in a chart.
 *
 * A series always carries a value range. Category charts (column, bar, line,
 * pie, area) take their axis labels from the per-series @ref Categories range,
 * which @ref ChartBuilder::SetXAxisLabels fills in for every series at once.
 * Scatter and bubble charts pair @ref XValues with @ref Values, and bubble
 * charts add @ref BubbleSizes.
 */
struct EXYOKIOFFICE_EXPORT ExcelChartSeries
{
    /** @brief Series display name shown in the legend. */
    std::string Name;

    /** @brief Value (Y) range. Required. */
    CellRange Values;

    /** @brief Optional per-series category axis labels. */
    std::optional<CellRange> Categories;

    /** @brief X value range for scatter and bubble charts. */
    std::optional<CellRange> XValues;

    /** @brief Bubble size range for bubble charts. */
    std::optional<CellRange> BubbleSizes;

    /**
     * @brief Worksheet the ranges live on.
     *
     * When empty the ranges are resolved against the worksheet that hosts the
     * chart. Cached values are only embedded when the source worksheet is the
     * host worksheet; cross-sheet references still emit a valid formula.
     */
    std::optional<std::string> SourceSheet;
};

/**
 * @brief Complete description of a chart anchored into a worksheet.
 *
 * The definition is consumed by @ref Worksheet::AddChart. The @ref ChartBuilder
 * fluent helper assembles the same structure for callers that prefer a
 * chained API.
 */
struct EXYOKIOFFICE_EXPORT ExcelChartDefinition
{
    /** @brief Stable drawing object identifier. Zero requests automatic allocation. */
    UInt32 Id = 0;

    /** @brief Non-visual graphic-frame name. A default is generated when empty. */
    std::string Name;

    /** @brief Top-left anchor cell. */
    CellAddress From;

    /** @brief Bottom-right anchor cell, exclusive in drawing coordinates. */
    CellAddress To;

    /** @brief Chart plot type. */
    ExcelChartType Type = ExcelChartType::Column;

    /** @brief Chart title. No title element is written when empty. */
    std::string Title;

    /** @brief Category (X) axis title. */
    std::string CategoryAxisTitle;

    /** @brief Value (Y) axis title. */
    std::string ValueAxisTitle;

    /** @brief Whether the legend is drawn. */
    bool ShowLegend = true;

    /** @brief Legend placement when @ref ShowLegend is true. */
    ExcelLegendPosition LegendPosition = ExcelLegendPosition::Right;

    /** @brief Whether major value-axis gridlines are drawn. */
    bool ShowGridLines = true;

    /** @brief Data series. At least one is required. */
    std::vector<ExcelChartSeries> Series;
};

/**
 * @brief Fluent builder that assembles and inserts a chart.
 *
 * The builder wraps an @ref ExcelChartDefinition and forwards to
 * @ref Worksheet::AddChart on @ref Build. Example:
 *
 * @code
 * ChartBuilder(sheet)
 *     .SetType(ExcelChartType::Column)
 *     .SetTitle("Sales")
 *     .SetAnchor(from, to)
 *     .AddSeries("Q1", valuesRange)
 *     .SetXAxisLabels(categoryRange)
 *     .Build();
 * @endcode
 *
 * Ranges resolve against the worksheet the chart is anchored into. Add
 * @ref SetSourceSheet when the data lives on a different worksheet.
 */
class EXYOKIOFFICE_EXPORT ChartBuilder
{
public:
    /**
     * @brief Creates a builder that will insert into @p sheet.
     *
     * @param sheet Worksheet the chart is anchored into on @ref Build. The
     * builder keeps a shared reference, so the worksheet wrapper must outlive
     * the call to @ref Build but not necessarily the builder object itself.
     */
    explicit ChartBuilder(std::shared_ptr<Worksheet> sheet);

    /**
     * @brief Sets the chart plot type.
     * @param type Plot type; see @ref ExcelChartType. Defaults to Column.
     */
    ChartBuilder& SetType(ExcelChartType type);

    /**
     * @brief Sets the chart title.
     * @param title Title text. No title element is written when left empty.
     */
    ChartBuilder& SetTitle(const std::string& title);

    /**
     * @brief Sets the two-cell drawing anchor that positions the chart on the sheet.
     * @param from Top-left anchor cell.
     * @param to Bottom-right anchor cell, exclusive in drawing coordinates.
     */
    ChartBuilder& SetAnchor(CellAddress from, CellAddress to);

    /**
     * @brief Sets the category (X) axis title.
     * @param title Axis title text. Has no effect on Pie charts, which have no axes.
     */
    ChartBuilder& SetCategoryAxisTitle(const std::string& title);

    /**
     * @brief Sets the value (Y) axis title.
     * @param title Axis title text. Has no effect on Pie charts, which have no axes.
     */
    ChartBuilder& SetValueAxisTitle(const std::string& title);

    /**
     * @brief Enables or disables the legend and sets its placement.
     * @param show Whether the legend is drawn.
     * @param position Legend placement when @p show is true. Defaults to Right.
     */
    ChartBuilder& ShowLegend(bool show, ExcelLegendPosition position = ExcelLegendPosition::Right);

    /**
     * @brief Enables or disables major value-axis gridlines.
     * @param show Whether gridlines are drawn.
     */
    ChartBuilder& ShowGridLines(bool show);

    /**
     * @brief Appends a value series with a display name.
     * @param name Series display name shown in the legend.
     * @param values Value (Y) range for the series.
     */
    ChartBuilder& AddSeries(const std::string& name, const CellRange& values);

    /**
     * @brief Sets the category axis labels for every series added so far.
     *
     * For scatter and bubble charts the labels are applied as the X value
     * range of every series instead, since those chart types plot two numeric
     * axes rather than one axis of discrete category labels.
     *
     * @param labels Category (or X value, for scatter/bubble) range applied to
     * every series currently in the builder. Series added afterward are
     * unaffected; call this again if more series follow.
     */
    ChartBuilder& SetXAxisLabels(const CellRange& labels);

    /**
     * @brief Reads every series range from @p sheet instead of the host worksheet.
     *
     * Chart ranges carry no sheet name of their own, so without this call they
     * resolve against the worksheet the chart is anchored into — a chart placed
     * on a summary sheet would plot that summary sheet's empty cells. The name
     * applies to series already added and to every series added afterward, so
     * the call may appear anywhere in the chain.
     *
     * Cross-sheet series emit a valid reference but no cached values, because
     * @ref ExcelChartSeries::SourceSheet caching is limited to the host
     * worksheet; Excel fills them in on open.
     *
     * @param sheet Worksheet name the value, category, X value, and bubble size
     * ranges live on. An empty name restores host-worksheet resolution.
     * @see ExcelChartSeries::SourceSheet
     */
    ChartBuilder& SetSourceSheet(const std::string& sheet);

    /**
     * @brief Inserts the chart into the worksheet.
     *
     * @return The allocated drawing object identifier, or std::nullopt when the
     * worksheet or definition is invalid.
     */
    std::optional<UInt32> Build();

    /** @brief Returns the accumulated definition without inserting it. */
    const ExcelChartDefinition& Definition() const noexcept { return m_definition; }

private:
    std::shared_ptr<Worksheet> m_sheet;
    ExcelChartDefinition m_definition;
    std::optional<std::string> m_sourceSheet;
};

} // namespace ExyokiOffice::Excel
