// Copyright (c) 2026 Jakub Melka and Contributors
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "ChartXml.hpp"

#include "ExyokiOffice/DOM/DocumentFormat/OpenXml/Drawing.hpp"
#include "ExyokiOffice/StandardTypes.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <iterator>
#include <string_view>

namespace ExyokiOffice::Detail::Charts
{
namespace C = ExyokiOffice::DocumentFormat::OpenXml::Drawing::Charts;
namespace A = ExyokiOffice::DocumentFormat::OpenXml::Drawing;

/**
 * How an axis is written. The primary pair crosses at zero and is drawn; a
 * secondary pair is written the way Excel writes it, with its value axis
 * crossing at the maximum so it stands on the opposite side and its
 * category axis deleted, because the categories are already labelled once.
 *
 * At namespace scope rather than inside ChartDomInternal, where it is only
 * used: BuildCategoryAxis defaults its parameter to an empty one, and GCC
 * parses that default argument before the default member initializers of a
 * class nested in the same class, so neither `{}` nor `AxisStyle{}` compiles
 * there.
 */
struct AxisStyle
{
    bool deleted = false;
    /// `crosses` value, or null to write none, as Excel does on a deleted category axis.
    const char* crosses = "autoZero";
    bool gridLines = false;
};

/** Non-public helpers backing ChartDom's static methods; not part of the public chart API. */
class ChartDomInternal
{
public:
    template <typename T>
    static std::shared_ptr<T> Child(const ChartDom::Element& parent)
    {
        return parent ? parent->GetFirstChildOfType<T>() : nullptr;
    }

    template <typename T>
    static void SetVal(const ChartDom::Element& parent, std::string_view value)
    {
        if (parent)
        {
            parent->AppendChild<T>()->SetAttribute(OpenXmlQualifiedName({}, "val"), value);
        }
    }

    static std::vector<ChartDom::Element> SeriesOf(const ChartDom::Element& group)
    {
        std::vector<ChartDom::Element> result;
        const auto append = [&]<typename T>()
        {
            for (const auto& item : group->Elements<T>())
            {
                result.push_back(item);
            }
        };
        if (openxmlelement_cast<C::BarChart>(group))
        {
            append.template operator()<C::BarChartSeries>();
        }
        else if (openxmlelement_cast<C::LineChart>(group))
        {
            append.template operator()<C::LineChartSeries>();
        }
        else if (openxmlelement_cast<C::PieChart>(group))
        {
            append.template operator()<C::PieChartSeries>();
        }
        else if (openxmlelement_cast<C::AreaChart>(group))
        {
            append.template operator()<C::AreaChartSeries>();
        }
        else if (openxmlelement_cast<C::ScatterChart>(group))
        {
            append.template operator()<C::ScatterChartSeries>();
        }
        else if (openxmlelement_cast<C::BubbleChart>(group))
        {
            append.template operator()<C::BubbleChartSeries>();
        }
        return result;
    }

    static std::shared_ptr<C::NumberReference> NumberRef(const ChartDom::Element& wrapper)
    {
        return Child<C::NumberReference>(wrapper);
    }

    static std::shared_ptr<C::StringReference> StringRef(const ChartDom::Element& wrapper)
    {
        return Child<C::StringReference>(wrapper);
    }

    static bool IsScatterKind(ChartPlotKind kind)
    {
        return kind == ChartPlotKind::XyScatter || kind == ChartPlotKind::Bubble;
    }

    static const char* LegendPositionCode(ChartLegendPosition value)
    {
        switch (value)
        {
            case ChartLegendPosition::Left:
                return "l";
            case ChartLegendPosition::Top:
                return "t";
            case ChartLegendPosition::Bottom:
                return "b";
            default:
                return "r";
        }
    }

    /**
     * Writes the series @p members of @p data into @p group. A series keeps its
     * position in @p data as its index and order, so a combination chart split
     * across several groups still numbers its series once, as Excel does.
     */
    template <typename TSeries>
    static void EmitSeries(const ChartDom::Element& group, const std::vector<ChartSeriesData>& data,
                           const std::vector<Size>& members, bool marker, bool scatter)
    {
        for (const Size i : members)
        {
            // series has a dependent type, so AppendChild needs the template
            // disambiguator here. MSVC accepts it without, Clang does not.
            auto series = group->AppendChild<TSeries>();
            ChartDom::AppendSeriesPreamble(series, i, data[i].name, marker);
            if (scatter)
            {
                if (data[i].category.present)
                {
                    ChartDom::AppendRef(series->template AppendChild<C::XValues>(), data[i].category);
                }
                ChartDom::AppendRef(series->template AppendChild<C::YValues>(), data[i].values);
                if (data[i].bubble.present)
                {
                    ChartDom::AppendRef(series->template AppendChild<C::BubbleSize>(), data[i].bubble);
                }
            }
            else
            {
                if (data[i].category.present)
                {
                    ChartDom::AppendRef(series->template AppendChild<C::CategoryAxisData>(),
                                        data[i].category);
                }
                ChartDom::AppendRef(series->template AppendChild<C::Values>(), data[i].values);
            }
        }
    }

    static void BuildCategoryAxis(const ChartDom::Element& plot, const char* id, const char* position,
                                  const char* cross, const std::string& title, const AxisStyle& style = {})
    {
        auto axis = plot->AppendChild<C::CategoryAxis>();
        SetVal<C::AxisId>(axis, id);
        SetVal<C::Orientation>(axis->AppendChild<C::Scaling>(), "minMax");
        SetVal<C::Delete>(axis, style.deleted ? "1" : "0");
        SetVal<C::AxisPosition>(axis, position);
        if (!title.empty())
        {
            ChartDom::AppendTitle(axis, title);
        }
        SetVal<C::MajorTickMark>(axis, "out");
        SetVal<C::MinorTickMark>(axis, "none");
        SetVal<C::TickLabelPosition>(axis, "nextTo");
        SetVal<C::CrossingAxis>(axis, cross);
        if (style.crosses != nullptr)
        {
            SetVal<C::Crosses>(axis, style.crosses);
        }
        SetVal<C::AutoLabeled>(axis, "1");
        SetVal<C::LabelAlignment>(axis, "ctr");
        SetVal<C::LabelOffset>(axis, "100");
        SetVal<C::NoMultiLevelLabels>(axis, "0");
    }

    static void BuildValueAxis(const ChartDom::Element& plot, const char* id, const char* position,
                               const char* cross, const std::string& title, const AxisStyle& style)
    {
        auto axis = plot->AppendChild<C::ValueAxis>();
        SetVal<C::AxisId>(axis, id);
        SetVal<C::Orientation>(axis->AppendChild<C::Scaling>(), "minMax");
        SetVal<C::Delete>(axis, style.deleted ? "1" : "0");
        SetVal<C::AxisPosition>(axis, position);
        if (style.gridLines)
        {
            axis->AppendChild<C::MajorGridlines>();
        }
        if (!title.empty())
        {
            ChartDom::AppendTitle(axis, title);
        }
        auto format = axis->AppendChild<C::NumberingFormat>();
        format->SetFormatCode(StringValue("General"));
        format->SetSourceLinked(BooleanValue(true));
        SetVal<C::MajorTickMark>(axis, "out");
        SetVal<C::MinorTickMark>(axis, "none");
        SetVal<C::TickLabelPosition>(axis, "nextTo");
        SetVal<C::CrossingAxis>(axis, cross);
        if (style.crosses != nullptr)
        {
            SetVal<C::Crosses>(axis, style.crosses);
        }
        SetVal<C::CrossBetween>(axis, "between");
    }

    /// The plot type a series is drawn as: its own, or the chart's.
    static ChartPlotKind EffectiveKind(ChartPlotKind chartKind, const ChartSeriesData& series)
    {
        return series.kind == ChartPlotKind::Unknown ? chartKind : series.kind;
    }

    /** The series one plot-type group of a chart being written will hold. */
    struct GroupPlan
    {
        ChartPlotKind kind = ChartPlotKind::Unknown;
        bool secondary = false;
        std::vector<Size> members;
    };

    /**
     * Splits @p series into one group per plot type and axis pair. A plot area
     * holds every group before every axis, and Excel lists the primary groups
     * first, so they are planned first, each in order of first appearance.
     */
    static std::vector<GroupPlan> PlanGroups(ChartPlotKind chartKind, const std::vector<ChartSeriesData>& series)
    {
        std::vector<GroupPlan> plans;
        for (const bool secondary : {false, true})
        {
            for (Size index = 0; index < series.size(); ++index)
            {
                if (series[index].secondaryAxis != secondary)
                {
                    continue;
                }
                const auto kind = EffectiveKind(chartKind, series[index]);
                auto plan = std::find_if(plans.begin(), plans.end(), [&](const GroupPlan& candidate)
                                         { return candidate.kind == kind && candidate.secondary == secondary; });
                if (plan == plans.end())
                {
                    plans.push_back(GroupPlan{kind, secondary, {}});
                    plan = std::prev(plans.end());
                }
                plan->members.push_back(index);
            }
        }
        return plans;
    }

    /// Writes one plot-type group, bound to the primary (1, 2) or secondary (3, 4) axis pair.
    static void EmitGroup(const ChartDom::Element& plot, const GroupPlan& plan,
                          const std::vector<ChartSeriesData>& series)
    {
        const char* const categoryAxis = plan.secondary ? "3" : "1";
        const char* const valueAxis = plan.secondary ? "4" : "2";
        const auto bindAxes = [&](const ChartDom::Element& group)
        {
            SetVal<C::AxisId>(group, categoryAxis);
            SetVal<C::AxisId>(group, valueAxis);
        };
        switch (plan.kind)
        {
            case ChartPlotKind::Column:
            case ChartPlotKind::Bar:
            {
                auto group = plot->AppendChild<C::BarChart>();
                SetVal<C::BarDirection>(group, plan.kind == ChartPlotKind::Bar ? "bar" : "col");
                SetVal<C::BarGrouping>(group, "clustered");
                SetVal<C::VaryColors>(group, "0");
                EmitSeries<C::BarChartSeries>(group, series, plan.members, false, false);
                bindAxes(group);
                break;
            }
            case ChartPlotKind::Line:
            {
                auto group = plot->AppendChild<C::LineChart>();
                SetVal<C::Grouping>(group, "standard");
                SetVal<C::VaryColors>(group, "0");
                EmitSeries<C::LineChartSeries>(group, series, plan.members, true, false);
                SetVal<C::ShowMarker>(group, "1");
                bindAxes(group);
                break;
            }
            case ChartPlotKind::Pie:
            {
                auto group = plot->AppendChild<C::PieChart>();
                SetVal<C::VaryColors>(group, "1");
                EmitSeries<C::PieChartSeries>(group, series, plan.members, false, false);
                SetVal<C::FirstSliceAngle>(group, "0");
                break;
            }
            case ChartPlotKind::Area:
            {
                auto group = plot->AppendChild<C::AreaChart>();
                SetVal<C::Grouping>(group, "standard");
                SetVal<C::VaryColors>(group, "0");
                EmitSeries<C::AreaChartSeries>(group, series, plan.members, false, false);
                bindAxes(group);
                break;
            }
            case ChartPlotKind::XyScatter:
            {
                auto group = plot->AppendChild<C::ScatterChart>();
                SetVal<C::ScatterStyle>(group, "lineMarker");
                SetVal<C::VaryColors>(group, "0");
                EmitSeries<C::ScatterChartSeries>(group, series, plan.members, false, true);
                bindAxes(group);
                break;
            }
            case ChartPlotKind::Bubble:
            {
                auto group = plot->AppendChild<C::BubbleChart>();
                SetVal<C::VaryColors>(group, "0");
                EmitSeries<C::BubbleChartSeries>(group, series, plan.members, false, true);
                bindAxes(group);
                break;
            }
            case ChartPlotKind::Unknown:
                break;
        }
    }

    /// Classifies a plot-area child; Unknown for axes and anything else.
    static ChartPlotKind GroupKind(const ChartDom::Element& child, bool& scatterLike)
    {
        scatterLike = false;
        if (auto bar = openxmlelement_cast<C::BarChart>(child))
        {
            auto direction = Child<C::BarDirection>(bar);
            return direction && direction->GetAttribute(OpenXmlQualifiedName({}, "val")) == "bar"
                       ? ChartPlotKind::Bar
                       : ChartPlotKind::Column;
        }
        if (openxmlelement_cast<C::LineChart>(child))
        {
            return ChartPlotKind::Line;
        }
        if (openxmlelement_cast<C::PieChart>(child))
        {
            return ChartPlotKind::Pie;
        }
        if (openxmlelement_cast<C::AreaChart>(child))
        {
            return ChartPlotKind::Area;
        }
        if (openxmlelement_cast<C::ScatterChart>(child))
        {
            scatterLike = true;
            return ChartPlotKind::XyScatter;
        }
        if (openxmlelement_cast<C::BubbleChart>(child))
        {
            scatterLike = true;
            return ChartPlotKind::Bubble;
        }
        return ChartPlotKind::Unknown;
    }

    /// The first axis a plot-type group names, or empty for a pie.
    static std::string FirstAxisId(const ChartDom::Element& group)
    {
        auto axis = Child<C::AxisId>(group);
        return axis ? std::string(axis->GetAttribute(OpenXmlQualifiedName({}, "val"))) : std::string{};
    }

    struct OldSeriesFormulas
    {
        std::string value;
        std::string category;
    };

    static std::vector<OldSeriesFormulas> ReadOldFormulas(const ChartDom::Element& group, bool scatter)
    {
        std::vector<OldSeriesFormulas> result;
        for (const auto& series : ChartDom::Series(group))
        {
            auto values = scatter ? std::static_pointer_cast<OpenXMLElement>(series->GetFirstChildOfType<C::YValues>())
                                  : std::static_pointer_cast<OpenXMLElement>(series->GetFirstChildOfType<C::Values>());
            auto categories =
                scatter ? std::static_pointer_cast<OpenXMLElement>(series->GetFirstChildOfType<C::XValues>())
                        : std::static_pointer_cast<OpenXMLElement>(series->GetFirstChildOfType<C::CategoryAxisData>());
            result.push_back({ChartDom::ReadRefFormula(values), ChartDom::ReadRefFormula(categories)});
        }
        return result;
    }

    /// @p index is the series' position within its group, which is how @p formulas
    /// is indexed; @p ordinal is its position across the whole chart.
    static void AppendLiteralData(const ChartDom::Element& series, const ChartLiteralSeries& data, Size index,
                                  Size ordinal, const std::vector<OldSeriesFormulas>& formulas, bool scatter,
                                  bool marker)
    {
        ChartDom::AppendSeriesPreamble(series, ordinal, data.name, marker);
        if (data.categories)
        {
            ChartSeriesRef ref;
            ref.present = ref.hasCache = true;
            ref.formula = index < formulas.size() ? formulas[index].category : std::string{};
            ref.count = static_cast<UInt32>(data.categories->size());
            ref.numeric = scatter;
            if (scatter)
            {
                for (const auto& value : *data.categories)
                {
                    Real parsed = 0.0;
                    std::from_chars(value.data(), value.data() + value.size(), parsed);
                    ref.numbers.emplace_back(ChartDom::FormatNumber(parsed));
                }
            }
            else
            {
                ref.strings = *data.categories;
            }
            ChartDom::Element wrapper =
                scatter ? std::static_pointer_cast<OpenXMLElement>(series->AppendChild<C::XValues>())
                        : std::static_pointer_cast<OpenXMLElement>(series->AppendChild<C::CategoryAxisData>());
            ChartDom::AppendRef(wrapper, ref);
        }
        ChartSeriesRef ref;
        ref.present = ref.numeric = ref.hasCache = true;
        ref.formula = index < formulas.size() ? formulas[index].value : std::string{};
        ref.count = static_cast<UInt32>(data.values.size());
        for (Real value : data.values)
        {
            ref.numbers.emplace_back(ChartDom::FormatNumber(value));
        }
        ChartDom::Element wrapper = scatter ? std::static_pointer_cast<OpenXMLElement>(series->AppendChild<C::YValues>())
                                            : std::static_pointer_cast<OpenXMLElement>(series->AppendChild<C::Values>());
        ChartDom::AppendRef(wrapper, ref);
    }

    template <typename TSeries>
    static void RebuildTyped(const ChartDom::Element& group, const std::vector<ChartLiteralSeries>& data,
                             const std::vector<Size>& ordinals, const std::vector<OldSeriesFormulas>& formulas,
                             bool scatter, bool marker)
    {
        for (const auto& old : group->Elements<TSeries>())
        {
            group->RemoveChild(old);
        }
        for (Size i = 0; i < data.size(); ++i)
        {
            AppendLiteralData(group->AppendChild<TSeries>(), data[i], i, ordinals[i], formulas, scatter, marker);
        }
    }

    /// Rewrites the series of one plot-type group; false for a group kind it does not know.
    static bool RebuildGroup(const ChartPlotGroup& plot, const std::vector<ChartLiteralSeries>& data,
                             const std::vector<Size>& ordinals)
    {
        const auto formulas = ReadOldFormulas(plot.group, plot.scatterLike);
        switch (plot.kind)
        {
            case ChartPlotKind::Column:
            case ChartPlotKind::Bar:
                RebuildTyped<C::BarChartSeries>(plot.group, data, ordinals, formulas, false, false);
                return true;
            case ChartPlotKind::Line:
                RebuildTyped<C::LineChartSeries>(plot.group, data, ordinals, formulas, false, true);
                return true;
            case ChartPlotKind::Pie:
                RebuildTyped<C::PieChartSeries>(plot.group, data, ordinals, formulas, false, false);
                return true;
            case ChartPlotKind::Area:
                RebuildTyped<C::AreaChartSeries>(plot.group, data, ordinals, formulas, false, false);
                return true;
            case ChartPlotKind::XyScatter:
                RebuildTyped<C::ScatterChartSeries>(plot.group, data, ordinals, formulas, true, false);
                return true;
            case ChartPlotKind::Bubble:
                RebuildTyped<C::BubbleChartSeries>(plot.group, data, ordinals, formulas, true, false);
                return true;
            case ChartPlotKind::Unknown:
                break;
        }
        return false;
    }

    /// The `c:order` of a series, or @p fallback when it carries none.
    static UInt32 ReadOrder(const ChartDom::Element& series, UInt32 fallback)
    {
        auto order = Child<C::Order>(series);
        const auto text = order ? order->GetAttribute(OpenXmlQualifiedName({}, "val")) : std::string_view{};
        UInt32 value = fallback;
        std::from_chars(text.data(), text.data() + text.size(), value);
        return value;
    }
};

ChartDom::Element ChartDom::FindPlotGroup(const Element& plotArea, ChartPlotKind& kind, bool& scatterLike)
{
    kind = ChartPlotKind::Unknown;
    scatterLike = false;
    if (!plotArea)
    {
        return nullptr;
    }
    if (auto group = ChartDomInternal::Child<C::BarChart>(plotArea))
    {
        auto direction = ChartDomInternal::Child<C::BarDirection>(group);
        kind = direction && direction->GetAttribute(OpenXmlQualifiedName({}, "val")) == "bar" ? ChartPlotKind::Bar : ChartPlotKind::Column;
        return group;
    }
    if (auto group = ChartDomInternal::Child<C::LineChart>(plotArea))
    {
        kind = ChartPlotKind::Line;
        return group;
    }
    if (auto group = ChartDomInternal::Child<C::PieChart>(plotArea))
    {
        kind = ChartPlotKind::Pie;
        return group;
    }
    if (auto group = ChartDomInternal::Child<C::AreaChart>(plotArea))
    {
        kind = ChartPlotKind::Area;
        return group;
    }
    if (auto group = ChartDomInternal::Child<C::ScatterChart>(plotArea))
    {
        kind = ChartPlotKind::XyScatter;
        scatterLike = true;
        return group;
    }
    if (auto group = ChartDomInternal::Child<C::BubbleChart>(plotArea))
    {
        kind = ChartPlotKind::Bubble;
        scatterLike = true;
        return group;
    }
    return nullptr;
}

std::vector<ChartPlotGroup> ChartDom::PlotGroups(const Element& plotArea)
{
    std::vector<ChartPlotGroup> groups;
    if (!plotArea)
    {
        return groups;
    }
    std::string primaryAxis;
    for (const auto& child : plotArea->Children())
    {
        ChartPlotGroup entry;
        entry.kind = ChartDomInternal::GroupKind(child, entry.scatterLike);
        if (entry.kind == ChartPlotKind::Unknown)
        {
            continue;
        }
        entry.group = child;
        const auto axis = ChartDomInternal::FirstAxisId(child);
        if (!axis.empty())
        {
            if (primaryAxis.empty())
            {
                primaryAxis = axis;
            }
            entry.secondaryAxis = axis != primaryAxis;
        }
        groups.push_back(std::move(entry));
    }
    return groups;
}

std::vector<ChartSeriesNode> ChartDom::AllSeries(const Element& plotArea)
{
    std::vector<ChartSeriesNode> nodes;
    const auto groups = PlotGroups(plotArea);
    for (Size groupIndex = 0; groupIndex < groups.size(); ++groupIndex)
    {
        const auto& plot = groups[groupIndex];
        for (const auto& series : Series(plot.group))
        {
            const auto fallback = static_cast<UInt32>(nodes.size());
            nodes.push_back(ChartSeriesNode{series, plot.group, groupIndex, plot.kind, plot.scatterLike,
                                            plot.secondaryAxis, ChartDomInternal::ReadOrder(series, fallback)});
        }
    }
    std::ranges::stable_sort(nodes, {}, &ChartSeriesNode::order);
    return nodes;
}

bool ChartDom::IsValidCombination(ChartPlotKind chartKind, const std::vector<ChartSeriesData>& series)
{
    if (chartKind == ChartPlotKind::Unknown)
    {
        return false;
    }
    bool primary = series.empty();
    for (const auto& item : series)
    {
        const auto kind = ChartDomInternal::EffectiveKind(chartKind, item);
        const bool pieMismatch = (kind == ChartPlotKind::Pie) != (chartKind == ChartPlotKind::Pie);
        const bool scatterMismatch = ChartDomInternal::IsScatterKind(kind) != ChartDomInternal::IsScatterKind(chartKind);
        const bool bubbleMismatch = (kind == ChartPlotKind::Bubble) != (chartKind == ChartPlotKind::Bubble);
        const bool barMismatch = (kind == ChartPlotKind::Bar) != (chartKind == ChartPlotKind::Bar);
        if (kind == ChartPlotKind::Unknown || pieMismatch || scatterMismatch || bubbleMismatch || barMismatch ||
            (kind == ChartPlotKind::Pie && item.secondaryAxis))
        {
            return false;
        }
        primary = primary || !item.secondaryAxis;
    }
    return primary;
}

std::vector<ChartDom::Element> ChartDom::Series(const Element& group)
{
    return group ? ChartDomInternal::SeriesOf(group) : std::vector<Element>{};
}

std::string ChartDom::ReadTitle(const Element& chart)
{
    auto title = ChartDomInternal::Child<C::Title>(chart);
    if (!title)
    {
        return {};
    }
    std::string result;
    for (const auto& text : title->Descendants<A::Text>())
    {
        result += text->GetText();
    }
    return result;
}

std::string ChartDom::ReadSeriesName(const Element& series)
{
    auto tx = ChartDomInternal::Child<C::SeriesText>(series);
    if (!tx)
    {
        return {};
    }
    if (auto value = ChartDomInternal::Child<C::NumericValue>(tx))
    {
        return std::string(value->GetText());
    }
    if (auto ref = ChartDomInternal::Child<C::StringReference>(tx))
    {
        if (auto cache = ChartDomInternal::Child<C::StringCache>(ref))
        {
            if (auto point = ChartDomInternal::Child<C::StringPoint>(cache))
            {
                if (auto value = ChartDomInternal::Child<C::NumericValue>(point))
                {
                    return std::string(value->GetText());
                }
            }
        }
    }
    return {};
}

std::string ChartDom::ReadRefFormula(const Element& wrapper)
{
    Element ref = ChartDomInternal::NumberRef(wrapper);
    if (!ref)
    {
        ref = ChartDomInternal::StringRef(wrapper);
    }
    auto formula = ChartDomInternal::Child<C::Formula>(ref);
    return formula ? std::string(formula->GetText()) : std::string{};
}

std::vector<Real> ChartDom::ReadNumericCache(const Element& wrapper)
{
    auto ref = ChartDomInternal::NumberRef(wrapper);
    auto cache = ChartDomInternal::Child<C::NumberingCache>(ref);
    if (!cache)
    {
        return {};
    }
    auto count = ChartDomInternal::Child<C::PointCount>(cache);
    const auto size = count ? count->GetAttribute(OpenXmlQualifiedName({}, "val")) : std::string_view{};
    UInt32 n = 0;
    std::from_chars(size.data(), size.data() + size.size(), n);
    std::vector<Real> result(n, 0.0);
    for (const auto& point : cache->Elements<C::NumericPoint>())
    {
        const auto index = point->GetIndex().ValueOr(0);
        if (index < result.size())
        {
            if (auto value = ChartDomInternal::Child<C::NumericValue>(point))
            {
                std::from_chars(value->GetText().data(), value->GetText().data() + value->GetText().size(), result[index]);
            }
        }
    }
    return result;
}

std::vector<std::string> ChartDom::ReadCategoryCache(const Element& wrapper)
{
    Element cache;
    if (auto ref = ChartDomInternal::NumberRef(wrapper))
    {
        cache = ChartDomInternal::Child<C::NumberingCache>(ref);
    }
    if (!cache)
    {
        if (auto ref = ChartDomInternal::StringRef(wrapper))
        {
            cache = ChartDomInternal::Child<C::StringCache>(ref);
        }
    }
    if (!cache)
    {
        return {};
    }
    auto count = ChartDomInternal::Child<C::PointCount>(cache);
    UInt32 n = 0;
    const auto size = count ? count->GetAttribute(OpenXmlQualifiedName({}, "val")) : std::string_view{};
    std::from_chars(size.data(), size.data() + size.size(), n);
    std::vector<std::string> result(n);
    for (const auto& child : cache->Children())
    {
        UInt32 index = 0;
        if (auto point = openxmlelement_cast<C::NumericPoint>(child))
        {
            index = point->GetIndex().ValueOr(0);
        }
        else if (auto stringPoint = openxmlelement_cast<C::StringPoint>(child))
        {
            index = stringPoint->GetIndex().ValueOr(0);
        }
        else
        {
            continue;
        }
        if (index < result.size())
        {
            if (auto value = ChartDomInternal::Child<C::NumericValue>(child))
            {
                result[index] = value->GetText();
            }
        }
    }
    return result;
}

void ChartDom::AppendTitle(const Element& parent, const std::string& text)
{
    auto title = parent->AppendChild<C::Title>();
    auto tx = title->AppendChild<C::ChartText>();
    auto rich = tx->AppendChild<C::RichText>();
    rich->AppendChild<A::BodyProperties>();
    rich->AppendChild<A::ListStyle>();
    auto paragraph = rich->AppendChild<A::Paragraph>();
    auto run = paragraph->AppendChild<A::Run>();
    run->AppendChild<A::Text>()->SetText(text);
    ChartDomInternal::SetVal<C::Overlay>(title, "0");
}

void ChartDom::SetAutoTitleDeleted(const Element& chart, bool deleted)
{
    auto value = ChartDomInternal::Child<C::AutoTitleDeleted>(chart);
    if (!value)
    {
        value = chart->AppendChild<C::AutoTitleDeleted>();
    }
    value->SetAttribute(OpenXmlQualifiedName({}, "val"), deleted ? "1" : "0");
}

void ChartDom::AppendSeriesPreamble(const Element& series, Size index, const std::string& name, bool withMarker)
{
    ChartDomInternal::SetVal<C::Index>(series, std::to_string(index));
    ChartDomInternal::SetVal<C::Order>(series, std::to_string(index));
    auto tx = series->AppendChild<C::SeriesText>();
    tx->AppendChild<C::NumericValue>()->SetText(name);
    if (withMarker)
    {
        ChartDomInternal::SetVal<C::ShowMarker>(series, "1");
    }
}

void ChartDom::AppendRef(const Element& wrapper, const ChartSeriesRef& ref)
{
    if (!wrapper || !ref.present)
    {
        return;
    }
    Element reference;
    Element cache;
    if (ref.numeric)
    {
        auto typed = wrapper->AppendChild<C::NumberReference>();
        reference = typed;
        typed->AppendChild<C::Formula>()->SetText(ref.formula);
        if (ref.hasCache)
        {
            auto typedCache = typed->AppendChild<C::NumberingCache>();
            cache = typedCache;
            typedCache->AppendChild<C::FormatCode>()->SetText("General");
        }
    }
    else
    {
        auto typed = wrapper->AppendChild<C::StringReference>();
        reference = typed;
        typed->AppendChild<C::Formula>()->SetText(ref.formula);
        if (ref.hasCache)
        {
            cache = typed->AppendChild<C::StringCache>();
        }
    }
    if (!cache)
    {
        return;
    }
    ChartDomInternal::SetVal<C::PointCount>(cache, std::to_string(ref.count));
    if (ref.numeric)
    {
        for (Size i = 0; i < ref.numbers.size(); ++i)
        {
            if (ref.numbers[i])
            {
                auto point = cache->AppendChild<C::NumericPoint>();
                point->SetIndex(UInt32Value(static_cast<UInt32>(i)));
                point->AppendChild<C::NumericValue>()->SetText(*ref.numbers[i]);
            }
        }
    }
    else
    {
        for (Size i = 0; i < ref.strings.size(); ++i)
        {
            auto point = cache->AppendChild<C::StringPoint>();
            point->SetIndex(UInt32Value(static_cast<UInt32>(i)));
            point->AppendChild<C::NumericValue>()->SetText(ref.strings[i]);
        }
    }
}

std::string ChartDom::FormatNumber(Real value)
{
    std::array<char, 64> buffer{};
    const auto result = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
    return result.ec == std::errc() ? std::string(buffer.data(), result.ptr) : std::to_string(value);
}

void ChartDom::BuildChartSpace(const Element& chartSpace, const ChartLayout& layout,
                               const std::vector<ChartSeriesData>& series)
{
    if (!chartSpace)
    {
        return;
    }
    for (const auto& child : chartSpace->Children())
    {
        chartSpace->RemoveChild(child);
    }
    auto chart = chartSpace->AppendChild<C::Chart>();
    if (!layout.title.empty())
    {
        AppendTitle(chart, layout.title);
        SetAutoTitleDeleted(chart, false);
    }
    auto plot = chart->AppendChild<C::PlotArea>();
    plot->AppendChild<C::Layout>();
    const auto plans = ChartDomInternal::PlanGroups(layout.type, series);
    for (const auto& plan : plans)
    {
        ChartDomInternal::EmitGroup(plot, plan, series);
    }

    // Every group shares the axis kinds of the first one, which is what
    // IsValidCombination guarantees, so the first group decides the axes.
    const auto axisKind = plans.empty() ? layout.type : plans.front().kind;
    const bool secondary = std::ranges::any_of(plans, [](const auto& plan)
                                               { return plan.secondary; });
    if (axisKind != ChartPlotKind::Pie && axisKind != ChartPlotKind::Unknown)
    {
        const AxisStyle primary{false, "autoZero", layout.showGridLines};
        const AxisStyle secondaryValue{false, "max", false};
        const AxisStyle secondaryCategory{true, nullptr, false};
        const std::string noTitle;
        if (ChartDomInternal::IsScatterKind(axisKind))
        {
            ChartDomInternal::BuildValueAxis(plot, "1", "b", "2", layout.categoryAxisTitle, primary);
            ChartDomInternal::BuildValueAxis(plot, "2", "l", "1", layout.valueAxisTitle, primary);
            if (secondary)
            {
                ChartDomInternal::BuildValueAxis(plot, "4", "r", "3", layout.secondaryValueAxisTitle, secondaryValue);
                ChartDomInternal::BuildValueAxis(plot, "3", "b", "4", noTitle, AxisStyle{true, "autoZero", false});
            }
        }
        else
        {
            const bool horizontal = axisKind == ChartPlotKind::Bar;
            ChartDomInternal::BuildCategoryAxis(plot, "1", horizontal ? "l" : "b", "2", layout.categoryAxisTitle);
            ChartDomInternal::BuildValueAxis(plot, "2", horizontal ? "b" : "l", "1", layout.valueAxisTitle, primary);
            if (secondary)
            {
                ChartDomInternal::BuildValueAxis(plot, "4", horizontal ? "t" : "r", "3",
                                                 layout.secondaryValueAxisTitle, secondaryValue);
                ChartDomInternal::BuildCategoryAxis(plot, "3", horizontal ? "r" : "b", "4", noTitle,
                                                    secondaryCategory);
            }
        }
    }
    if (layout.showLegend && layout.legendPosition != ChartLegendPosition::None)
    {
        auto legend = chart->AppendChild<C::Legend>();
        ChartDomInternal::SetVal<C::LegendPosition>(legend, ChartDomInternal::LegendPositionCode(layout.legendPosition));
        ChartDomInternal::SetVal<C::Overlay>(legend, "0");
    }
    ChartDomInternal::SetVal<C::PlotVisibleOnly>(chart, "1");
    ChartDomInternal::SetVal<C::DisplayBlanksAs>(chart, "gap");
}

bool ChartDom::RewriteSeries(const Element& chart, const std::vector<ChartLiteralSeries>& data,
                             const std::optional<std::string>& title)
{
    if (!chart)
    {
        return false;
    }
    const auto plotArea = ChartDomInternal::Child<C::PlotArea>(chart);
    const auto groups = PlotGroups(plotArea);
    if (groups.empty())
    {
        return false;
    }

    // Which group each new series goes into. One group takes them all; a
    // combination chart hands the k-th series in `c:order` to the group the
    // k-th existing series is in. Decided before anything is written, so a
    // refused rewrite leaves the chart as it was.
    std::vector<std::vector<Size>> members(groups.size());
    if (groups.size() == 1)
    {
        for (Size index = 0; index < data.size(); ++index)
        {
            members.front().push_back(index);
        }
    }
    else
    {
        const auto nodes = AllSeries(plotArea);
        if (nodes.size() != data.size())
        {
            return false;
        }
        for (Size index = 0; index < nodes.size(); ++index)
        {
            if (nodes[index].groupIndex >= members.size())
            {
                return false;
            }
            members[nodes[index].groupIndex].push_back(index);
        }
    }

    if (title)
    {
        if (auto old = ChartDomInternal::Child<C::Title>(chart))
        {
            chart->RemoveChild(old);
        }
        if (title->empty())
        {
            SetAutoTitleDeleted(chart, true);
        }
        else
        {
            AppendTitle(chart, *title);
            SetAutoTitleDeleted(chart, false);
        }
    }
    for (Size index = 0; index < groups.size(); ++index)
    {
        std::vector<ChartLiteralSeries> slice;
        slice.reserve(members[index].size());
        for (const Size member : members[index])
        {
            slice.push_back(data[member]);
        }
        if (!ChartDomInternal::RebuildGroup(groups[index], slice, members[index]))
        {
            return false;
        }
    }
    return true;
}

} // namespace ExyokiOffice::Detail::Charts
