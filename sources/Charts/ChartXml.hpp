// Copyright (c) 2026 Jakub Melka and Contributors
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#pragma once

#include "ExyokiOffice/DOM/DocumentFormat/OpenXml/Drawing/Charts.hpp"
#include "ExyokiOffice/OpenXMLElement.hpp"
#include "ExyokiOffice/StandardTypes.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace ExyokiOffice::Detail::Charts
{

enum class ChartPlotKind
{
    Unknown,
    Column,
    Bar,
    Line,
    Pie,
    Area,
    XyScatter,
    Bubble
};

/** Host-neutral legend placement mirrored by Excel and PowerPoint chart APIs. */
enum class ChartLegendPosition
{
    None,
    Right,
    Left,
    Top,
    Bottom
};

struct ChartSeriesRef
{
    bool present = false;
    bool numeric = true;
    bool hasCache = false;
    std::string formula;
    UInt32 count = 0;
    std::vector<std::optional<std::string>> numbers;
    std::vector<std::string> strings;
};

/** One fully resolved series (name plus value/category/bubble references) ready to serialize. */
struct ChartSeriesData
{
    std::string name;
    ChartSeriesRef values;
    ChartSeriesRef category;
    ChartSeriesRef bubble;
    /** Plot type of this series; Unknown means the chart's own type. */
    ChartPlotKind kind = ChartPlotKind::Unknown;
    /** Whether the series is plotted against the secondary axis pair. */
    bool secondaryAxis = false;
};

/** Host-neutral chart-space layout consumed by ChartDom::BuildChartSpace. */
struct ChartLayout
{
    ChartPlotKind type = ChartPlotKind::Column;
    std::string title;
    std::string categoryAxisTitle;
    std::string valueAxisTitle;
    /** Title of the secondary value axis; used only when a series is on it. */
    std::string secondaryValueAxisTitle;
    bool showLegend = true;
    ChartLegendPosition legendPosition = ChartLegendPosition::Right;
    bool showGridLines = true;
};

/** One series of an existing chart, with the group it sits in, as ChartDom::AllSeries reads it. */
struct ChartSeriesNode
{
    std::shared_ptr<OpenXMLElement> series;
    std::shared_ptr<OpenXMLElement> group;
    /**
     * Position of @ref group among ChartDom::PlotGroups. Element wrappers are
     * not guaranteed to be the same objects across two reads of the tree, so a
     * series is matched to its group by position rather than by pointer.
     */
    Size groupIndex = 0;
    ChartPlotKind kind = ChartPlotKind::Unknown;
    bool scatterLike = false;
    bool secondaryAxis = false;
    /** The series' `c:order`: its position across every group of the chart. */
    UInt32 order = 0;
};

/** One plot-type group of an existing chart, as ChartDom::PlotGroups reads it. */
struct ChartPlotGroup
{
    std::shared_ptr<OpenXMLElement> group;
    ChartPlotKind kind = ChartPlotKind::Unknown;
    bool scatterLike = false;
    /** True when the group names a different axis pair than the first group. */
    bool secondaryAxis = false;
};

/**
 * One series expressed as literal cached values, used when rewriting the cached
 * data of an existing chart (embedded-workbook charts in Word and PowerPoint).
 * For scatter/bubble charts @ref categories carries the decimal text of the X
 * values; otherwise it carries category labels.
 */
struct ChartLiteralSeries
{
    std::string name;
    std::vector<Real> values;
    std::optional<std::vector<std::string>> categories;
};

/** Shared chart operations implemented exclusively on the generated Open XML DOM. */
class ChartDom
{
public:
    using Element = std::shared_ptr<OpenXMLElement>;

    static Element FindPlotGroup(const Element& plotArea, ChartPlotKind& kind, bool& scatterLike);

    /**
     * Every plot-type group of @p plotArea in document order. A combination
     * chart has one group per plot type and axis pair; the first group's axes
     * are taken as the primary pair.
     */
    static std::vector<ChartPlotGroup> PlotGroups(const Element& plotArea);

    /**
     * Every series of @p plotArea across all of its groups, in `c:order`. A
     * reader that walks only the first group of a combination chart sees part
     * of it, and a writer that rewrites that part with the whole list
     * duplicates the rest.
     */
    static std::vector<ChartSeriesNode> AllSeries(const Element& plotArea);

    /**
     * Whether @p series can share one plot area under a chart of type
     * @p chartKind. Kinds that need different axes cannot be combined: a pie
     * has none, scatter and bubble plot two value axes, and a horizontal bar
     * swaps the axes of every other category kind. Bubble does not combine
     * with scatter, and at least one series must stay on the primary axis.
     */
    static bool IsValidCombination(ChartPlotKind chartKind, const std::vector<ChartSeriesData>& series);

    static std::vector<Element> Series(const Element& group);
    static std::string ReadTitle(const Element& chart);
    static std::string ReadSeriesName(const Element& series);
    static std::string ReadRefFormula(const Element& wrapper);
    static std::vector<Real> ReadNumericCache(const Element& wrapper);
    static std::vector<std::string> ReadCategoryCache(const Element& wrapper);

    static void AppendTitle(const Element& parent, const std::string& text);
    static void SetAutoTitleDeleted(const Element& chart, bool deleted);
    static void AppendSeriesPreamble(const Element& series, Size index, const std::string& name,
                                     bool withMarker);
    static void AppendRef(const Element& wrapper, const ChartSeriesRef& ref);

    /**
     * Rebuilds a whole `c:chartSpace` from a host-neutral layout and resolved
     * series. Existing children of @p chartSpace are cleared first, so this both
     * creates a new chart and fully re-authors an existing one.
     */
    static void BuildChartSpace(const Element& chartSpace, const ChartLayout& layout,
                                const std::vector<ChartSeriesData>& series);

    /**
     * Replaces the cached values (and optionally the title) of an existing chart
     * while preserving its plot type, formatting, and series source formulas.
     *
     * A chart with one plot-type group takes any number of series. A combination
     * chart keeps each series in the group it is in, matched by `c:order`, so it
     * takes exactly as many series as it has; changing the count would leave no
     * way to say which group a new series belongs to.
     *
     * @return false when @p chart has no recognized plot-type group, or when a
     * combination chart is given a different number of series than it has. The
     * chart is left untouched in both cases.
     */
    static bool RewriteSeries(const Element& chart, const std::vector<ChartLiteralSeries>& data,
                              const std::optional<std::string>& title);

    /** Formats a double as compact chart-cache decimal text (shared with cache writers). */
    static std::string FormatNumber(Real value);
};

} // namespace ExyokiOffice::Detail::Charts
