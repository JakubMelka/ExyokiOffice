// Copyright (c) 2026 Jakub Melka and Collaborators
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#pragma once

#include "ExyokiOffice/DOM/DocumentFormat/OpenXml/Drawing/Charts.hpp"
#include "ExyokiOffice/OpenXMLElement.hpp"
#include "ExyokiOffice/StandardTypes.hpp"

#include <cstdint>
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
};

/** Host-neutral chart-space layout consumed by ChartDom::BuildChartSpace. */
struct ChartLayout
{
    ChartPlotKind type = ChartPlotKind::Column;
    std::string title;
    std::string categoryAxisTitle;
    std::string valueAxisTitle;
    bool showLegend = true;
    ChartLegendPosition legendPosition = ChartLegendPosition::Right;
    bool showGridLines = true;
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
     * @return false when @p chart has no recognized plot-type group.
     */
    static bool RewriteSeries(const Element& chart, const std::vector<ChartLiteralSeries>& data,
                              const std::optional<std::string>& title);

    /** Formats a double as compact chart-cache decimal text (shared with cache writers). */
    static std::string FormatNumber(Real value);
};

} // namespace ExyokiOffice::Detail::Charts
