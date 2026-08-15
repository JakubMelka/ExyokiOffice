// Copyright (c) 2026 Jakub Melka and Contributors
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "ExyokiOffice/Excel/ExcelDocument.hpp"

#include "../Charts/ChartXml.hpp"
#include "ExyokiOffice/DOM/DocumentFormat/OpenXml/Drawing.hpp"
#include "ExyokiOffice/DOM/DocumentFormat/OpenXml/Drawing/Charts.hpp"
#include "ExyokiOffice/DOM/DocumentFormat/OpenXml/Drawing/Spreadsheet.hpp"
#include "ExyokiOffice/Packaging/GeneratedParts.hpp"
#include "Excel/WorksheetDrawingHelpers.hpp"
#include "ExyokiOffice/StandardTypes.hpp"

#include <algorithm>
#include <charconv>

namespace ExyokiOffice::Excel
{
namespace Detail
{
namespace C = ExyokiOffice::DocumentFormat::OpenXml::Drawing::Charts;
namespace A = ExyokiOffice::DocumentFormat::OpenXml::Drawing;
namespace X = ExyokiOffice::DocumentFormat::OpenXml::Drawing::Spreadsheet;
using ExyokiOffice::Detail::Charts::ChartDom;
using ExyokiOffice::Detail::Charts::ChartLayout;
using ExyokiOffice::Detail::Charts::ChartLegendPosition;
using ExyokiOffice::Detail::Charts::ChartPlotKind;
using ExyokiOffice::Detail::Charts::ChartSeriesData;
using ExyokiOffice::Detail::Charts::ChartSeriesRef;

// Thin adapter over the shared Excel::SheetCellRange formula parser/formatter.
// Excel's own chart formulas always target the workbook that hosts the chart,
// so only the range is needed back from Parse(); the sheet name is discarded.
class ChartFormulaText
{
public:
    static std::string Qualify(const std::string& sheet, const CellRange& range)
    {
        return SheetCellRange(sheet, range).ToFormula();
    }
    static std::optional<CellRange> Parse(const std::string& formula)
    {
        auto parsed = SheetCellRange::Parse(formula);
        return parsed ? std::optional(parsed->Range()) : std::nullopt;
    }
};

class ExcelChartTypeMap
{
public:
    static bool IsScatterLike(ExcelChartType type)
    {
        return type == ExcelChartType::XyScatter || type == ExcelChartType::Bubble;
    }
};

template <typename T>
std::shared_ptr<T> Child(const std::shared_ptr<OpenXMLElement>& parent)
{
    return parent ? parent->GetFirstChildOfType<T>() : nullptr;
}

template <typename T>
std::shared_ptr<T> AddVal(const std::shared_ptr<OpenXMLElement>& parent, std::string_view value)
{
    auto child = parent->AppendChild<T>();
    child->SetAttribute(OpenXmlQualifiedName({}, "val"), value);
    return child;
}

class ExcelChartDom
{
public:
    static ChartPlotKind PlotKind(ExcelChartType type)
    {
        switch (type)
        {
            case ExcelChartType::Column:
                return ChartPlotKind::Column;
            case ExcelChartType::Bar:
                return ChartPlotKind::Bar;
            case ExcelChartType::Line:
                return ChartPlotKind::Line;
            case ExcelChartType::Pie:
                return ChartPlotKind::Pie;
            case ExcelChartType::Area:
                return ChartPlotKind::Area;
            case ExcelChartType::XyScatter:
                return ChartPlotKind::XyScatter;
            case ExcelChartType::Bubble:
                return ChartPlotKind::Bubble;
        }
        return ChartPlotKind::Column;
    }

    static ChartLegendPosition LegendKind(ExcelLegendPosition position)
    {
        switch (position)
        {
            case ExcelLegendPosition::None:
                return ChartLegendPosition::None;
            case ExcelLegendPosition::Left:
                return ChartLegendPosition::Left;
            case ExcelLegendPosition::Top:
                return ChartLegendPosition::Top;
            case ExcelLegendPosition::Bottom:
                return ChartLegendPosition::Bottom;
            case ExcelLegendPosition::Right:
                return ChartLegendPosition::Right;
        }
        return ChartLegendPosition::Right;
    }

    static void Build(const std::shared_ptr<Packaging::ChartPart>& part, const ExcelChartDefinition& definition,
                      const std::vector<ChartSeriesData>& series)
    {
        ChartLayout layout;
        layout.type = PlotKind(definition.Type);
        layout.title = definition.Title;
        layout.categoryAxisTitle = definition.CategoryAxisTitle;
        layout.valueAxisTitle = definition.ValueAxisTitle;
        layout.showLegend = definition.ShowLegend;
        layout.legendPosition = LegendKind(definition.LegendPosition);
        layout.showGridLines = definition.ShowGridLines;
        ChartDom::BuildChartSpace(part->GetChartSpace(), layout, series);
    }

    static void Parse(const std::shared_ptr<Packaging::ChartPart>& part, ExcelChartDefinition& definition)
    {
        auto chart = Child<C::Chart>(part->GetChartSpace());
        if (!chart)
        {
            return;
        }
        definition.Title = ChartDom::ReadTitle(chart);
        ChartPlotKind kind{};
        bool scatter = false;
        auto group = ChartDom::FindPlotGroup(Child<C::PlotArea>(chart), kind, scatter);
        switch (kind)
        {
            case ChartPlotKind::Column:
                definition.Type = ExcelChartType::Column;
                break;
            case ChartPlotKind::Bar:
                definition.Type = ExcelChartType::Bar;
                break;
            case ChartPlotKind::Line:
                definition.Type = ExcelChartType::Line;
                break;
            case ChartPlotKind::Pie:
                definition.Type = ExcelChartType::Pie;
                break;
            case ChartPlotKind::Area:
                definition.Type = ExcelChartType::Area;
                break;
            case ChartPlotKind::XyScatter:
                definition.Type = ExcelChartType::XyScatter;
                break;
            case ChartPlotKind::Bubble:
                definition.Type = ExcelChartType::Bubble;
                break;
            default:
                return;
        }
        for (const auto& node : ChartDom::Series(group))
        {
            ExcelChartSeries item;
            item.Name = ChartDom::ReadSeriesName(node);
            auto values = scatter ? std::static_pointer_cast<OpenXMLElement>(Child<C::YValues>(node)) : std::static_pointer_cast<OpenXMLElement>(Child<C::Values>(node));
            auto categories = scatter ? std::static_pointer_cast<OpenXMLElement>(Child<C::XValues>(node)) : std::static_pointer_cast<OpenXMLElement>(Child<C::CategoryAxisData>(node));
            if (auto range = ChartFormulaText::Parse(ChartDom::ReadRefFormula(values)))
            {
                item.Values = *range;
            }
            if (auto range = ChartFormulaText::Parse(ChartDom::ReadRefFormula(categories)))
            {
                if (scatter)
                {
                    item.XValues = *range;
                }
                else
                {
                    item.Categories = *range;
                }
            }
            if (auto range = ChartFormulaText::Parse(ChartDom::ReadRefFormula(Child<C::BubbleSize>(node))))
            {
                item.BubbleSizes = *range;
            }
            definition.Series.push_back(std::move(item));
        }
    }

    static void AppendAnchor(const std::shared_ptr<X::WorksheetDrawing>& root, const ExcelChartDefinition& definition,
                             const std::string& relationshipId)
    {
        auto anchor = root->AppendChild<X::TwoCellAnchor>();
        SetMarker(anchor->AppendChild<X::FromMarker>(), definition.From);
        SetMarker(anchor->AppendChild<X::ToMarker>(), definition.To);
        auto frame = anchor->AppendChild<X::GraphicFrame>();
        auto nv = frame->AppendChild<X::NonVisualGraphicFrameProperties>();
        auto props = nv->AppendChild<X::NonVisualDrawingProperties>();
        props->SetId(UInt32Value(definition.Id));
        props->SetName(StringValue(definition.Name.empty() ? "Chart " + std::to_string(definition.Id) : definition.Name));
        nv->AppendChild<X::NonVisualGraphicFrameDrawingProperties>();
        auto transform = frame->AppendChild<X::Transform>();
        auto offset = transform->AppendChild<A::Offset>();
        offset->SetAttribute({{}, "x"}, "0");
        offset->SetAttribute({{}, "y"}, "0");
        auto extent = transform->AppendChild<A::Extents>();
        extent->SetAttribute({{}, "cx"}, "0");
        extent->SetAttribute({{}, "cy"}, "0");
        auto graphicData = frame->AppendChild<A::Graphic>()->AppendChild<A::GraphicData>();
        graphicData->SetUri(StringValue("http://schemas.openxmlformats.org/drawingml/2006/chart"));
        graphicData->AppendChild<C::ChartReference>()->SetId(StringValue(relationshipId));
        anchor->AppendChild<X::ClientData>();
    }

    static std::shared_ptr<X::TwoCellAnchor> FindAnchor(const std::shared_ptr<X::WorksheetDrawing>& root, UInt32 id)
    {
        for (const auto& anchor : root->Elements<X::TwoCellAnchor>())
        {
            auto frame = Child<X::GraphicFrame>(anchor);
            auto nonVisual = Child<X::NonVisualGraphicFrameProperties>(frame);
            auto properties = Child<X::NonVisualDrawingProperties>(nonVisual);
            if (properties && properties->GetId().ValueOr(0) == id)
            {
                return anchor;
            }
        }
        return nullptr;
    }

    static std::string RelationshipId(const std::shared_ptr<X::TwoCellAnchor>& anchor)
    {
        auto frame = Child<X::GraphicFrame>(anchor);
        auto graphic = Child<A::Graphic>(frame);
        auto data = Child<A::GraphicData>(graphic);
        auto ref = Child<C::ChartReference>(data);
        return ref ? ref->GetId().ToString() : std::string{};
    }

    static void UpdateAnchor(const std::shared_ptr<X::TwoCellAnchor>& anchor, const ExcelChartDefinition& definition)
    {
        SetMarker(Child<X::FromMarker>(anchor), definition.From);
        SetMarker(Child<X::ToMarker>(anchor), definition.To);
        if (!definition.Name.empty())
        {
            Child<X::NonVisualDrawingProperties>(Child<X::NonVisualGraphicFrameProperties>(Child<X::GraphicFrame>(anchor)))
                ->SetName(StringValue(definition.Name));
        }
    }

private:
    static void SetMarker(const std::shared_ptr<X::MarkerType>& marker, const CellAddress& address)
    {
        auto set = [&]<typename T>(std::string value)
        {
            auto node = Child<T>(marker);
            if (!node)
            {
                node = marker->AppendChild<T>();
            }
            node->SetText(value);
        };
        set.template operator()<X::ColumnId>(std::to_string(address.Column().Value() - 1));
        set.template operator()<X::ColumnOffset>("0");
        set.template operator()<X::RowId>(std::to_string(address.Row().Value() - 1));
        set.template operator()<X::RowOffset>("0");
    }
};

static UInt32 MaxDrawingId(const std::shared_ptr<X::WorksheetDrawing>& root)
{
    UInt32 result = 0;
    for (const auto& props : root->Descendants<X::NonVisualDrawingProperties>())
    {
        result = std::max(result, props->GetId().ValueOr(0));
    }
    return result;
}
static bool DrawingIdExists(const std::shared_ptr<X::WorksheetDrawing>& root, UInt32 id)
{
    return std::ranges::any_of(root->Descendants<X::NonVisualDrawingProperties>(), [id](const auto& p)
                               { return p->GetId().ValueOr(0) == id; });
}
} // namespace Detail

std::optional<UInt32> Worksheet::AddChart(ExcelChartDefinition chart)
{
    if (!m_part || chart.Series.empty() || !chart.From.IsValid() || !chart.To.IsValid() ||
        chart.To.Row().Value() < chart.From.Row().Value() || chart.To.Column().Value() < chart.From.Column().Value())
    {
        return std::nullopt;
    }
    auto drawing = m_part->GetDrawingsPart();
    if (!drawing)
    {
        drawing = m_part->AddDrawingsPart();
        if (!drawing || !Detail::LinkWorksheetDrawing(m_part, drawing->RelationshipId()))
        {
            return std::nullopt;
        }
    }
    auto root = drawing->GetWorksheetDrawing();
    if (!root)
    {
        return std::nullopt;
    }
    if (chart.Id == 0)
    {
        chart.Id = Detail::MaxDrawingId(root) + 1;
    }
    else if (Detail::DrawingIdExists(root, chart.Id))
    {
        return std::nullopt;
    }

    const bool scatter = Detail::ExcelChartTypeMap::IsScatterLike(chart.Type);
    const std::string host = Name();
    SharedStringTableService strings(m_document);
    const auto number = [](const ExcelCellValue& v) -> std::optional<std::string>
    {
        if (v.Kind() == CellValueKind::Number || v.Kind() == CellValueKind::Boolean)
        {
            return v.Text();
        }
        if (v.Kind() == CellValueKind::Formula && (v.FormulaValue().CachedKind == FormulaCachedValueKind::Number || v.FormulaValue().CachedKind == FormulaCachedValueKind::Boolean))
        {
            return v.FormulaValue().CachedText;
        }
        return std::nullopt;
    };
    const auto label = [&](const ExcelCellValue& v)
    {
        if (v.Kind() == CellValueKind::SharedString)
        {
            if (auto i = v.SharedStringIndex())
            {
                if (auto s = strings.Lookup(*i))
                {
                    return *s;
                }
            }
            return std::string{};
        }
        if (v.Kind() == CellValueKind::Boolean)
        {
            return std::string(v.BooleanValue().value_or(false) ? "TRUE" : "FALSE");
        }
        return v.Kind() == CellValueKind::Formula ? v.FormulaValue().CachedText : (v.Kind() == CellValueKind::Blank ? std::string{} : v.Text());
    };
    const auto makeRef = [&](const std::string& sheet, const CellRange& range, bool same, bool numeric)
    {
        Detail::ChartSeriesRef ref;
        ref.present = true;
        ref.numeric = numeric;
        ref.formula = Detail::ChartFormulaText::Qualify(sheet, range);
        ref.count = range.RowCount() * range.ColumnCount();
        if (same)
        {
            ref.hasCache = true;
            for (UInt32 r = range.First().Row().Value(); r <= range.Last().Row().Value(); ++r)
            {
                for (UInt32 c = range.First().Column().Value(); c <= range.Last().Column().Value(); ++c)
                {
                    auto a = CellAddress::TryCreate(r, c);
                    auto v = a ? GetCellValue(*a) : std::nullopt;
                    if (numeric)
                    {
                        ref.numbers.push_back(v ? number(*v) : std::nullopt);
                    }
                    else
                    {
                        ref.strings.push_back(v ? label(*v) : std::string{});
                    }
                }
            }
        }
        return ref;
    };
    std::vector<Detail::ChartSeriesData> resolved;
    for (const auto& series : chart.Series)
    {
        if (!series.Values.IsValid())
        {
            return std::nullopt;
        }
        const std::string sheet = series.SourceSheet && !series.SourceSheet->empty() ? *series.SourceSheet : host;
        const bool same = sheet == host;
        Detail::ChartSeriesData data;
        data.name = series.Name;
        data.values = makeRef(sheet, series.Values, same, true);
        auto categories = scatter ? (series.XValues ? series.XValues : series.Categories) : (series.Categories ? series.Categories : series.XValues);
        if (categories && categories->IsValid())
        {
            data.category = makeRef(sheet, *categories, same, scatter);
        }
        if (chart.Type == ExcelChartType::Bubble && series.BubbleSizes && series.BubbleSizes->IsValid())
        {
            data.bubble = makeRef(sheet, *series.BubbleSizes, same, true);
        }
        resolved.push_back(std::move(data));
    }
    auto part = drawing->AddChartPart();
    if (!part)
    {
        return std::nullopt;
    }
    Detail::ExcelChartDom::Build(part, chart, resolved);
    Detail::ExcelChartDom::AppendAnchor(root, chart, part->RelationshipId());
    return chart.Id;
}

bool Worksheet::UpdateChart(const ExcelChartDefinition& chart)
{
    if (!m_part || chart.Series.empty() || chart.Id == 0 || !chart.From.IsValid() || !chart.To.IsValid() ||
        chart.To.Row().Value() < chart.From.Row().Value() ||
        chart.To.Column().Value() < chart.From.Column().Value())
    {
        return false;
    }
    auto drawing = m_part->GetDrawingsPart();
    auto root = drawing ? drawing->GetWorksheetDrawing() : nullptr;
    auto anchor = root ? Detail::ExcelChartDom::FindAnchor(root, chart.Id) : nullptr;
    if (!anchor)
    {
        return false;
    }
    const auto relationshipId = Detail::ExcelChartDom::RelationshipId(anchor);
    std::shared_ptr<Packaging::ChartPart> part;
    for (const auto& candidate : drawing->GetChartParts())
    {
        if (candidate->RelationshipId() == relationshipId)
        {
            part = candidate;
            break;
        }
    }
    if (!part)
    {
        return false;
    }
    // Reuse AddChart's range resolution without creating another part by building the same cache data here.
    const bool scatter = Detail::ExcelChartTypeMap::IsScatterLike(chart.Type);
    const std::string host = Name();
    SharedStringTableService strings(m_document);
    const auto number = [](const ExcelCellValue& v) -> std::optional<std::string>
    {
        if (v.Kind() == CellValueKind::Number || v.Kind() == CellValueKind::Boolean)
        {
            return v.Text();
        }
        if (v.Kind() == CellValueKind::Formula &&
            (v.FormulaValue().CachedKind == FormulaCachedValueKind::Number ||
             v.FormulaValue().CachedKind == FormulaCachedValueKind::Boolean))
        {
            return v.FormulaValue().CachedText;
        }
        return std::nullopt;
    };
    const auto label = [&](const ExcelCellValue& v)
    {
        if (v.Kind() == CellValueKind::SharedString)
        {
            if (const auto index = v.SharedStringIndex())
            {
                if (const auto text = strings.Lookup(*index))
                {
                    return *text;
                }
            }
            return std::string{};
        }
        if (v.Kind() == CellValueKind::Boolean)
        {
            return std::string(v.BooleanValue().value_or(false) ? "TRUE" : "FALSE");
        }
        if (v.Kind() == CellValueKind::Formula)
        {
            return v.FormulaValue().CachedText;
        }
        return v.Kind() == CellValueKind::Blank ? std::string{} : v.Text();
    };
    const auto makeRef = [&](const std::string& sheet, const CellRange& range, bool same, bool numeric)
    {
        Detail::ChartSeriesRef ref;
        ref.present = true;
        ref.numeric = numeric;
        ref.formula = Detail::ChartFormulaText::Qualify(sheet, range);
        ref.count = range.RowCount() * range.ColumnCount();
        if (same)
        {
            ref.hasCache = true;
            for (UInt32 row = range.First().Row().Value(); row <= range.Last().Row().Value(); ++row)
            {
                for (UInt32 column = range.First().Column().Value(); column <= range.Last().Column().Value(); ++column)
                {
                    const auto address = CellAddress::TryCreate(row, column);
                    const auto value = address ? GetCellValue(*address) : std::nullopt;
                    if (numeric)
                    {
                        ref.numbers.push_back(value ? number(*value) : std::nullopt);
                    }
                    else
                    {
                        ref.strings.push_back(value ? label(*value) : std::string{});
                    }
                }
            }
        }
        return ref;
    };
    std::vector<Detail::ChartSeriesData> resolved;
    for (const auto& series : chart.Series)
    {
        if (!series.Values.IsValid())
        {
            return false;
        }
        const std::string sheet = series.SourceSheet && !series.SourceSheet->empty() ? *series.SourceSheet : host;
        const bool same = sheet == host;
        Detail::ChartSeriesData data;
        data.name = series.Name;
        data.values = makeRef(sheet, series.Values, same, true);
        auto categories = scatter ? (series.XValues ? series.XValues : series.Categories) : (series.Categories ? series.Categories : series.XValues);
        if (categories && categories->IsValid())
        {
            data.category = makeRef(sheet, *categories, same, scatter);
        }
        if (chart.Type == ExcelChartType::Bubble && series.BubbleSizes && series.BubbleSizes->IsValid())
        {
            data.bubble = makeRef(sheet, *series.BubbleSizes, same, true);
        }
        resolved.push_back(std::move(data));
    }
    Detail::ExcelChartDom::Build(part, chart, resolved);
    Detail::ExcelChartDom::UpdateAnchor(anchor, chart);
    return true;
}

std::vector<ExcelChartDefinition> Worksheet::Charts() const
{
    std::vector<ExcelChartDefinition> result;
    auto drawing = m_part ? m_part->GetDrawingsPart() : nullptr;
    auto root = drawing ? drawing->GetWorksheetDrawing() : nullptr;
    if (!root)
    {
        return result;
    }
    for (const auto& anchor : root->Elements<Detail::X::TwoCellAnchor>())
    {
        const auto relationshipId = Detail::ExcelChartDom::RelationshipId(anchor);
        if (relationshipId.empty())
        {
            continue;
        }
        ExcelChartDefinition definition;
        auto frame = Detail::Child<Detail::X::GraphicFrame>(anchor);
        auto props = Detail::Child<Detail::X::NonVisualDrawingProperties>(Detail::Child<Detail::X::NonVisualGraphicFrameProperties>(frame));
        if (props)
        {
            definition.Id = props->GetId().ValueOr(0);
            definition.Name = props->GetName().ToString();
        }
        auto readMarker = [](const std::shared_ptr<Detail::X::MarkerType>& marker)
        {
            UInt32 row = 0;
            UInt32 column = 0;
            if (auto node = Detail::Child<Detail::X::RowId>(marker))
            {
                std::from_chars(node->GetText().data(), node->GetText().data() + node->GetText().size(), row);
            }
            if (auto node = Detail::Child<Detail::X::ColumnId>(marker))
            {
                std::from_chars(node->GetText().data(), node->GetText().data() + node->GetText().size(), column);
            }
            return CellAddress::TryCreate(row + 1, column + 1);
        };
        if (auto address = readMarker(Detail::Child<Detail::X::FromMarker>(anchor)))
        {
            definition.From = *address;
        }
        if (auto address = readMarker(Detail::Child<Detail::X::ToMarker>(anchor)))
        {
            definition.To = *address;
        }
        for (const auto& part : drawing->GetChartParts())
        {
            if (part->RelationshipId() == relationshipId)
            {
                Detail::ExcelChartDom::Parse(part, definition);
                break;
            }
        }
        result.push_back(std::move(definition));
    }
    return result;
}

bool Worksheet::RemoveChart(UInt32 id)
{
    auto drawing = m_part ? m_part->GetDrawingsPart() : nullptr;
    auto root = drawing ? drawing->GetWorksheetDrawing() : nullptr;
    if (!root || id == 0)
    {
        return false;
    }
    auto anchor = Detail::ExcelChartDom::FindAnchor(root, id);
    if (!anchor)
    {
        return false;
    }
    const auto relationshipId = Detail::ExcelChartDom::RelationshipId(anchor);
    root->RemoveChild(anchor);
    for (const auto& part : drawing->GetChartParts())
    {
        if (part->RelationshipId() == relationshipId)
        {
            drawing->RemoveChartPart(part);
            break;
        }
    }
    if (root->Children().empty())
    {
        Detail::UnlinkWorksheetDrawing(m_part);
        return m_part->RemoveDrawingsPart();
    }
    return true;
}

ChartBuilder::ChartBuilder(std::shared_ptr<Worksheet> sheet)
    : m_sheet(std::move(sheet))
{
}
ChartBuilder& ChartBuilder::SetType(ExcelChartType value)
{
    m_definition.Type = value;
    return *this;
}
ChartBuilder& ChartBuilder::SetTitle(const std::string& value)
{
    m_definition.Title = value;
    return *this;
}
ChartBuilder& ChartBuilder::SetAnchor(CellAddress from, CellAddress to)
{
    m_definition.From = from;
    m_definition.To = to;
    return *this;
}
ChartBuilder& ChartBuilder::SetCategoryAxisTitle(const std::string& value)
{
    m_definition.CategoryAxisTitle = value;
    return *this;
}
ChartBuilder& ChartBuilder::SetValueAxisTitle(const std::string& value)
{
    m_definition.ValueAxisTitle = value;
    return *this;
}
ChartBuilder& ChartBuilder::ShowLegend(bool show, ExcelLegendPosition value)
{
    m_definition.ShowLegend = show;
    m_definition.LegendPosition = value;
    return *this;
}
ChartBuilder& ChartBuilder::ShowGridLines(bool show)
{
    m_definition.ShowGridLines = show;
    return *this;
}
ChartBuilder& ChartBuilder::AddSeries(const std::string& name, const CellRange& values)
{
    ExcelChartSeries s;
    s.Name = name;
    s.Values = values;
    s.SourceSheet = m_sourceSheet;
    m_definition.Series.push_back(std::move(s));
    return *this;
}
ChartBuilder& ChartBuilder::SetXAxisLabels(const CellRange& labels)
{
    for (auto& s : m_definition.Series)
    {
        s.Categories = labels;
    }
    return *this;
}
ChartBuilder& ChartBuilder::SetSourceSheet(const std::string& sheet)
{
    // Kept as a builder-level default so the call is order-independent: it
    // reaches the series added before it as well as the ones added after.
    m_sourceSheet = sheet.empty() ? std::optional<std::string>{} : std::optional<std::string>{sheet};
    for (auto& s : m_definition.Series)
    {
        s.SourceSheet = m_sourceSheet;
    }
    return *this;
}
std::optional<UInt32> ChartBuilder::Build()
{
    return m_sheet ? m_sheet->AddChart(m_definition) : std::nullopt;
}
} // namespace ExyokiOffice::Excel
