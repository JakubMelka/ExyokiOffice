// Copyright (c) 2026 Jakub Melka and Contributors
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "ChartXml.hpp"

#include "ExyokiOffice/DOM/DocumentFormat/OpenXml/Drawing.hpp"
#include "ExyokiOffice/StandardTypes.hpp"

#include <array>
#include <charconv>

namespace ExyokiOffice::Detail::Charts
{
namespace C = ExyokiOffice::DocumentFormat::OpenXml::Drawing::Charts;
namespace A = ExyokiOffice::DocumentFormat::OpenXml::Drawing;

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

    template <typename TSeries>
    static void EmitSeries(const ChartDom::Element& group, const std::vector<ChartSeriesData>& data, bool marker,
                           bool scatter)
    {
        for (Size i = 0; i < data.size(); ++i)
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
                                  const char* cross, const std::string& title)
    {
        auto axis = plot->AppendChild<C::CategoryAxis>();
        SetVal<C::AxisId>(axis, id);
        SetVal<C::Orientation>(axis->AppendChild<C::Scaling>(), "minMax");
        SetVal<C::Delete>(axis, "0");
        SetVal<C::AxisPosition>(axis, position);
        if (!title.empty())
        {
            ChartDom::AppendTitle(axis, title);
        }
        SetVal<C::MajorTickMark>(axis, "out");
        SetVal<C::MinorTickMark>(axis, "none");
        SetVal<C::TickLabelPosition>(axis, "nextTo");
        SetVal<C::CrossingAxis>(axis, cross);
        SetVal<C::Crosses>(axis, "autoZero");
        SetVal<C::AutoLabeled>(axis, "1");
        SetVal<C::LabelAlignment>(axis, "ctr");
        SetVal<C::LabelOffset>(axis, "100");
        SetVal<C::NoMultiLevelLabels>(axis, "0");
    }

    static void BuildValueAxis(const ChartDom::Element& plot, const char* id, const char* position,
                               const char* cross, const std::string& title, bool grid)
    {
        auto axis = plot->AppendChild<C::ValueAxis>();
        SetVal<C::AxisId>(axis, id);
        SetVal<C::Orientation>(axis->AppendChild<C::Scaling>(), "minMax");
        SetVal<C::Delete>(axis, "0");
        SetVal<C::AxisPosition>(axis, position);
        if (grid)
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
        SetVal<C::Crosses>(axis, "autoZero");
        SetVal<C::CrossBetween>(axis, "between");
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

    static void AppendLiteralData(const ChartDom::Element& series, const ChartLiteralSeries& data, Size index,
                                  const std::vector<OldSeriesFormulas>& formulas, bool scatter, bool marker)
    {
        ChartDom::AppendSeriesPreamble(series, index, data.name, marker);
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
                             const std::vector<OldSeriesFormulas>& formulas, bool scatter, bool marker)
    {
        for (const auto& old : group->Elements<TSeries>())
        {
            group->RemoveChild(old);
        }
        for (Size i = 0; i < data.size(); ++i)
        {
            AppendLiteralData(group->AppendChild<TSeries>(), data[i], i, formulas, scatter, marker);
        }
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
    const bool scatter = ChartDomInternal::IsScatterKind(layout.type);
    switch (layout.type)
    {
        case ChartPlotKind::Column:
        case ChartPlotKind::Bar:
        {
            auto group = plot->AppendChild<C::BarChart>();
            ChartDomInternal::SetVal<C::BarDirection>(group, layout.type == ChartPlotKind::Bar ? "bar" : "col");
            ChartDomInternal::SetVal<C::BarGrouping>(group, "clustered");
            ChartDomInternal::SetVal<C::VaryColors>(group, "0");
            ChartDomInternal::EmitSeries<C::BarChartSeries>(group, series, false, false);
            ChartDomInternal::SetVal<C::AxisId>(group, "1");
            ChartDomInternal::SetVal<C::AxisId>(group, "2");
            break;
        }
        case ChartPlotKind::Line:
        {
            auto group = plot->AppendChild<C::LineChart>();
            ChartDomInternal::SetVal<C::Grouping>(group, "standard");
            ChartDomInternal::SetVal<C::VaryColors>(group, "0");
            ChartDomInternal::EmitSeries<C::LineChartSeries>(group, series, true, false);
            ChartDomInternal::SetVal<C::ShowMarker>(group, "1");
            ChartDomInternal::SetVal<C::AxisId>(group, "1");
            ChartDomInternal::SetVal<C::AxisId>(group, "2");
            break;
        }
        case ChartPlotKind::Pie:
        {
            auto group = plot->AppendChild<C::PieChart>();
            ChartDomInternal::SetVal<C::VaryColors>(group, "1");
            ChartDomInternal::EmitSeries<C::PieChartSeries>(group, series, false, false);
            ChartDomInternal::SetVal<C::FirstSliceAngle>(group, "0");
            break;
        }
        case ChartPlotKind::Area:
        {
            auto group = plot->AppendChild<C::AreaChart>();
            ChartDomInternal::SetVal<C::Grouping>(group, "standard");
            ChartDomInternal::SetVal<C::VaryColors>(group, "0");
            ChartDomInternal::EmitSeries<C::AreaChartSeries>(group, series, false, false);
            ChartDomInternal::SetVal<C::AxisId>(group, "1");
            ChartDomInternal::SetVal<C::AxisId>(group, "2");
            break;
        }
        case ChartPlotKind::XyScatter:
        {
            auto group = plot->AppendChild<C::ScatterChart>();
            ChartDomInternal::SetVal<C::ScatterStyle>(group, "lineMarker");
            ChartDomInternal::SetVal<C::VaryColors>(group, "0");
            ChartDomInternal::EmitSeries<C::ScatterChartSeries>(group, series, false, true);
            ChartDomInternal::SetVal<C::AxisId>(group, "1");
            ChartDomInternal::SetVal<C::AxisId>(group, "2");
            break;
        }
        case ChartPlotKind::Bubble:
        {
            auto group = plot->AppendChild<C::BubbleChart>();
            ChartDomInternal::SetVal<C::VaryColors>(group, "0");
            ChartDomInternal::EmitSeries<C::BubbleChartSeries>(group, series, false, true);
            ChartDomInternal::SetVal<C::AxisId>(group, "1");
            ChartDomInternal::SetVal<C::AxisId>(group, "2");
            break;
        }
        case ChartPlotKind::Unknown:
            break;
    }
    if (layout.type != ChartPlotKind::Pie && layout.type != ChartPlotKind::Unknown)
    {
        if (scatter)
        {
            ChartDomInternal::BuildValueAxis(plot, "1", "b", "2", layout.categoryAxisTitle, layout.showGridLines);
            ChartDomInternal::BuildValueAxis(plot, "2", "l", "1", layout.valueAxisTitle, layout.showGridLines);
        }
        else
        {
            const bool horizontal = layout.type == ChartPlotKind::Bar;
            ChartDomInternal::BuildCategoryAxis(plot, "1", horizontal ? "l" : "b", "2", layout.categoryAxisTitle);
            ChartDomInternal::BuildValueAxis(plot, "2", horizontal ? "b" : "l", "1", layout.valueAxisTitle, layout.showGridLines);
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
    ChartPlotKind kind{};
    bool scatter = false;
    auto group = FindPlotGroup(ChartDomInternal::Child<C::PlotArea>(chart), kind, scatter);
    if (!group)
    {
        return false;
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
    const auto formulas = ChartDomInternal::ReadOldFormulas(group, scatter);
    if (openxmlelement_cast<C::BarChart>(group))
    {
        ChartDomInternal::RebuildTyped<C::BarChartSeries>(group, data, formulas, false, false);
    }
    else if (openxmlelement_cast<C::LineChart>(group))
    {
        ChartDomInternal::RebuildTyped<C::LineChartSeries>(group, data, formulas, false, true);
    }
    else if (openxmlelement_cast<C::PieChart>(group))
    {
        ChartDomInternal::RebuildTyped<C::PieChartSeries>(group, data, formulas, false, false);
    }
    else if (openxmlelement_cast<C::AreaChart>(group))
    {
        ChartDomInternal::RebuildTyped<C::AreaChartSeries>(group, data, formulas, false, false);
    }
    else if (openxmlelement_cast<C::ScatterChart>(group))
    {
        ChartDomInternal::RebuildTyped<C::ScatterChartSeries>(group, data, formulas, true, false);
    }
    else if (openxmlelement_cast<C::BubbleChart>(group))
    {
        ChartDomInternal::RebuildTyped<C::BubbleChartSeries>(group, data, formulas, true, false);
    }
    else
    {
        return false;
    }
    return true;
}

} // namespace ExyokiOffice::Detail::Charts
