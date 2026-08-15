// Copyright (c) 2026 Jakub Melka and Contributors
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "ExyokiOffice/PowerPoint/PowerPointDocument.hpp"

#include "../Charts/ChartXml.hpp"
#include "ExyokiOffice/DOM/DocumentFormat/OpenXml/Drawing.hpp"
#include "ExyokiOffice/DOM/DocumentFormat/OpenXml/Drawing/Charts.hpp"
#include "ExyokiOffice/DOM/DocumentFormat/OpenXml/Presentation.hpp"
#include "ExyokiOffice/Excel/ExcelAddress.hpp"
#include "ExyokiOffice/Excel/ExcelCellValue.hpp"
#include "ExyokiOffice/Excel/ExcelDocument.hpp"
#include "ExyokiOffice/Packaging/GeneratedParts.hpp"
#include "ExyokiOffice/StandardTypes.hpp"

#include <algorithm>
#include <charconv>
#include <string>

namespace ExyokiOffice::PowerPoint
{
namespace Presentation = ExyokiOffice::DocumentFormat::OpenXml::Presentation;
namespace Drawing = ExyokiOffice::DocumentFormat::OpenXml::Drawing;
namespace Charts = ExyokiOffice::DocumentFormat::OpenXml::Drawing::Charts;
namespace SharedCharts = ExyokiOffice::Detail::Charts;

/** Non-public helpers backing PresentationShapeTree::AddChart and the PresentationShape chart accessors. */
class PresentationChartHelpers
{
public:
    static constexpr std::string_view ChartGraphicDataUri = "http://schemas.openxmlformats.org/drawingml/2006/chart";

    static bool IsScatterType(PresentationChartType type)
    {
        return type == PresentationChartType::XyScatter || type == PresentationChartType::Bubble;
    }

    static SharedCharts::ChartPlotKind ToPlotKind(PresentationChartType type)
    {
        switch (type)
        {
            case PresentationChartType::Column:
                return SharedCharts::ChartPlotKind::Column;
            case PresentationChartType::Bar:
                return SharedCharts::ChartPlotKind::Bar;
            case PresentationChartType::Line:
                return SharedCharts::ChartPlotKind::Line;
            case PresentationChartType::Pie:
                return SharedCharts::ChartPlotKind::Pie;
            case PresentationChartType::Area:
                return SharedCharts::ChartPlotKind::Area;
            case PresentationChartType::XyScatter:
                return SharedCharts::ChartPlotKind::XyScatter;
            case PresentationChartType::Bubble:
                return SharedCharts::ChartPlotKind::Bubble;
            case PresentationChartType::Unknown:
                return SharedCharts::ChartPlotKind::Unknown;
        }
        return SharedCharts::ChartPlotKind::Unknown;
    }

    static PresentationChartType ToPublicType(SharedCharts::ChartPlotKind kind)
    {
        switch (kind)
        {
            case SharedCharts::ChartPlotKind::Column:
                return PresentationChartType::Column;
            case SharedCharts::ChartPlotKind::Bar:
                return PresentationChartType::Bar;
            case SharedCharts::ChartPlotKind::Line:
                return PresentationChartType::Line;
            case SharedCharts::ChartPlotKind::Pie:
                return PresentationChartType::Pie;
            case SharedCharts::ChartPlotKind::Area:
                return PresentationChartType::Area;
            case SharedCharts::ChartPlotKind::XyScatter:
                return PresentationChartType::XyScatter;
            case SharedCharts::ChartPlotKind::Bubble:
                return PresentationChartType::Bubble;
            case SharedCharts::ChartPlotKind::Unknown:
                return PresentationChartType::Unknown;
        }
        return PresentationChartType::Unknown;
    }

    static SharedCharts::ChartLegendPosition ToLegendKind(PresentationChartLegendPosition position)
    {
        switch (position)
        {
            case PresentationChartLegendPosition::None:
                return SharedCharts::ChartLegendPosition::None;
            case PresentationChartLegendPosition::Right:
                return SharedCharts::ChartLegendPosition::Right;
            case PresentationChartLegendPosition::Left:
                return SharedCharts::ChartLegendPosition::Left;
            case PresentationChartLegendPosition::Top:
                return SharedCharts::ChartLegendPosition::Top;
            case PresentationChartLegendPosition::Bottom:
                return SharedCharts::ChartLegendPosition::Bottom;
        }
        return SharedCharts::ChartLegendPosition::Right;
    }

    /** First cached-data row; row 1 is left free for a caller-supplied header. */
    static constexpr UInt32 FirstDataRow = 2;

    /**
     * Synthetic data-source formula for a single column of cached points, referencing
     * the workbook layout a caller would produce with SetChartEmbeddedWorkbook.
     * @param columnNumber One-based worksheet column (1 = A, 2 = B, ...).
     */
    static std::string RangeFormula(UInt32 columnNumber, Size count)
    {
        if (count == 0)
        {
            return {};
        }
        auto first = Excel::CellAddress::TryCreate(FirstDataRow, columnNumber);
        auto last = Excel::CellAddress::TryCreate(FirstDataRow + static_cast<UInt32>(count) - 1, columnNumber);
        auto range = (first && last) ? Excel::CellRange::TryCreate(*first, *last) : std::nullopt;
        return range ? Excel::SheetCellRange("Sheet1", *range).ToFormula() : std::string{};
    }

    static SharedCharts::ChartSeriesRef NumberRef(std::string formula, const std::vector<Real>& values)
    {
        SharedCharts::ChartSeriesRef ref;
        ref.present = ref.numeric = ref.hasCache = true;
        ref.formula = std::move(formula);
        ref.count = static_cast<UInt32>(values.size());
        for (Real value : values)
        {
            ref.numbers.emplace_back(SharedCharts::ChartDom::FormatNumber(value));
        }
        return ref;
    }

    static std::vector<SharedCharts::ChartSeriesData> ResolveSeries(const PresentationChartDefinition& definition,
                                                                    bool scatter)
    {
        std::vector<SharedCharts::ChartSeriesData> resolved;
        resolved.reserve(definition.Series.size());
        for (Size i = 0; i < definition.Series.size(); ++i)
        {
            const auto& source = definition.Series[i];
            const auto valuesColumn = static_cast<UInt32>(i) + 2; // column A is reserved for categories
            SharedCharts::ChartSeriesData data;
            data.name = source.Name;
            data.values = NumberRef(RangeFormula(valuesColumn, source.Values.size()), source.Values);
            if (source.Categories)
            {
                if (scatter)
                {
                    std::vector<Real> numbers;
                    numbers.reserve(source.Categories->size());
                    for (const auto& text : *source.Categories)
                    {
                        Real value = 0.0;
                        std::from_chars(text.data(), text.data() + text.size(), value);
                        numbers.push_back(value);
                    }
                    data.category = NumberRef(RangeFormula(1, numbers.size()), numbers);
                }
                else
                {
                    data.category.present = data.category.hasCache = true;
                    data.category.numeric = false;
                    data.category.formula = RangeFormula(1, source.Categories->size());
                    data.category.count = static_cast<UInt32>(source.Categories->size());
                    data.category.strings = *source.Categories;
                }
            }
            if (definition.Type == PresentationChartType::Bubble && source.BubbleSizes)
            {
                const auto bubbleColumn = static_cast<UInt32>(i + definition.Series.size()) + 2;
                data.bubble = NumberRef(RangeFormula(bubbleColumn, source.BubbleSizes->size()), *source.BubbleSizes);
            }
            resolved.push_back(std::move(data));
        }
        return resolved;
    }

    static std::shared_ptr<Charts::ChartReference> ChartReferenceOf(const std::shared_ptr<OpenXMLElement>& element)
    {
        if (!element)
        {
            return nullptr;
        }
        auto references = element->Descendants<Charts::ChartReference>();
        return references.empty() ? nullptr : references.front();
    }

    static std::shared_ptr<Packaging::ChartPart> ResolveChartPart(
        const std::shared_ptr<OpenXMLElement>& element, const std::shared_ptr<Packaging::SlidePart>& slidePart)
    {
        auto reference = ChartReferenceOf(element);
        if (!reference || !slidePart)
        {
            return nullptr;
        }
        const auto relationshipId = reference->GetId().ToString();
        for (const auto& part : slidePart->GetChartParts())
        {
            if (part && part->RelationshipId() == relationshipId)
            {
                return part;
            }
        }
        return nullptr;
    }

    static UInt32 NextShapeId(const std::shared_ptr<OpenXMLElement>& tree)
    {
        UInt32 id = 2;
        for (const auto& property : tree->Descendants<Presentation::NonVisualDrawingProperties>())
        {
            id = std::max(id, property->GetId().ValueOr(0) + 1);
        }
        return id;
    }

    // Mirrors the numeric-cell extraction used when Excel writes chart caches:
    // only genuinely numeric cells (or formulas cached as numeric) contribute a
    // value; anything else reads as 0.0.
    static Real CellNumber(const Excel::ExcelCellValue& value)
    {
        std::string_view text;
        if (value.Kind() == Excel::CellValueKind::Number || value.Kind() == Excel::CellValueKind::Boolean)
        {
            text = value.Text();
        }
        else if (value.Kind() == Excel::CellValueKind::Formula &&
                 (value.FormulaValue().CachedKind == Excel::FormulaCachedValueKind::Number ||
                  value.FormulaValue().CachedKind == Excel::FormulaCachedValueKind::Boolean))
        {
            text = value.FormulaValue().CachedText;
        }
        else
        {
            return 0.0;
        }
        Real result = 0.0;
        std::from_chars(text.data(), text.data() + text.size(), result);
        return result;
    }

    // Mirrors the label extraction used when Excel writes chart caches.
    static std::string CellLabel(const Excel::ExcelCellValue& value, Excel::SharedStringTableService& strings)
    {
        if (value.Kind() == Excel::CellValueKind::SharedString)
        {
            if (auto index = value.SharedStringIndex())
            {
                if (auto text = strings.Lookup(*index))
                {
                    return *text;
                }
            }
            return {};
        }
        if (value.Kind() == Excel::CellValueKind::Boolean)
        {
            return value.BooleanValue().value_or(false) ? "TRUE" : "FALSE";
        }
        if (value.Kind() == Excel::CellValueKind::Formula)
        {
            return value.FormulaValue().CachedText;
        }
        return value.Kind() == Excel::CellValueKind::Blank ? std::string{} : value.Text();
    }

    static std::optional<std::vector<Real>> ReadLiveNumbers(const std::shared_ptr<Excel::ExcelDocumentEditor>& workbook,
                                                            const std::string& formula)
    {
        auto parsed = Excel::SheetCellRange::Parse(formula);
        if (!parsed)
        {
            return std::nullopt;
        }
        auto sheet = workbook->GetWorksheet(parsed->Sheet());
        if (!sheet)
        {
            return std::nullopt;
        }
        std::vector<Real> result;
        const auto range = parsed->Range();
        for (auto row = range.First().Row().Value(); row <= range.Last().Row().Value(); ++row)
        {
            for (auto column = range.First().Column().Value(); column <= range.Last().Column().Value(); ++column)
            {
                auto address = Excel::CellAddress::TryCreate(row, column);
                auto value = address ? sheet->GetCellValue(*address) : std::nullopt;
                result.push_back(value ? CellNumber(*value) : 0.0);
            }
        }
        return result;
    }

    static std::optional<std::vector<std::string>> ReadLiveLabels(
        const std::shared_ptr<Excel::ExcelDocumentEditor>& workbook, const std::string& formula)
    {
        auto parsed = Excel::SheetCellRange::Parse(formula);
        if (!parsed)
        {
            return std::nullopt;
        }
        auto sheet = workbook->GetWorksheet(parsed->Sheet());
        if (!sheet)
        {
            return std::nullopt;
        }
        Excel::SharedStringTableService strings(workbook->GetDocument());
        std::vector<std::string> result;
        const auto range = parsed->Range();
        for (auto row = range.First().Row().Value(); row <= range.Last().Row().Value(); ++row)
        {
            for (auto column = range.First().Column().Value(); column <= range.Last().Column().Value(); ++column)
            {
                auto address = Excel::CellAddress::TryCreate(row, column);
                auto value = address ? sheet->GetCellValue(*address) : std::nullopt;
                result.push_back(value ? CellLabel(*value, strings) : std::string{});
            }
        }
        return result;
    }
};

PresentationShape::Ptr PresentationShapeTree::AddChart(const PresentationChartDefinition& chart)
{
    auto tree = std::dynamic_pointer_cast<Presentation::GroupShapeType>(m_tree);
    if (!tree || !m_slidePart || chart.Type == PresentationChartType::Unknown || chart.Series.empty())
    {
        return nullptr;
    }
    for (const auto& series : chart.Series)
    {
        if (series.Values.empty())
        {
            return nullptr;
        }
    }

    const UInt32 id = PresentationChartHelpers::NextShapeId(tree);
    auto frame = tree->AppendChild<Presentation::GraphicFrame>();
    auto nonVisual = frame ? frame->AppendChild<Presentation::NonVisualGraphicFrameProperties>() : nullptr;
    auto properties = nonVisual ? nonVisual->AppendChild<Presentation::NonVisualDrawingProperties>() : nullptr;
    auto drawing = nonVisual ? nonVisual->AppendChild<Presentation::NonVisualGraphicFrameDrawingProperties>() : nullptr;
    auto app = nonVisual ? nonVisual->AppendChild<Presentation::ApplicationNonVisualDrawingProperties>() : nullptr;
    auto transform = frame ? frame->AppendChild<Presentation::Transform>() : nullptr;
    auto graphic = frame ? frame->AppendChild<Drawing::Graphic>() : nullptr;
    auto data = graphic ? graphic->AppendChild<Drawing::GraphicData>() : nullptr;
    auto reference = data ? data->AppendChild<Charts::ChartReference>() : nullptr;
    if (!frame || !nonVisual || !properties || !drawing || !app || !transform || !graphic || !data || !reference)
    {
        if (frame)
        {
            tree->RemoveChild(frame);
        }
        return nullptr;
    }
    properties->SetId(UInt32Value(id));
    properties->SetName(StringValue(chart.Name.empty() ? "Chart " + std::to_string(id) : chart.Name));
    data->SetUri(StringValue(std::string(PresentationChartHelpers::ChartGraphicDataUri)));

    auto part = m_slidePart->AddChartPart();
    if (!part)
    {
        tree->RemoveChild(frame);
        return nullptr;
    }
    reference->SetId(StringValue(part->RelationshipId()));

    SharedCharts::ChartLayout layout;
    layout.type = PresentationChartHelpers::ToPlotKind(chart.Type);
    layout.title = chart.Title;
    layout.categoryAxisTitle = chart.CategoryAxisTitle;
    layout.valueAxisTitle = chart.ValueAxisTitle;
    layout.showLegend = chart.ShowLegend;
    layout.legendPosition = PresentationChartHelpers::ToLegendKind(chart.LegendPosition);
    layout.showGridLines = chart.ShowGridLines;
    SharedCharts::ChartDom::BuildChartSpace(
        part->GetChartSpace(), layout,
        PresentationChartHelpers::ResolveSeries(chart, PresentationChartHelpers::IsScatterType(chart.Type)));

    auto wrapper = PresentationShape::Ptr(new PresentationShape(frame, m_slidePart));
    if (!wrapper->SetTransform(chart.Transform))
    {
        m_slidePart->RemoveChartPart(part);
        tree->RemoveChild(frame);
        return nullptr;
    }
    return wrapper;
}

std::optional<PresentationChartInfo> PresentationShape::GetChart() const
{
    auto part = PresentationChartHelpers::ResolveChartPart(m_element, m_slidePart);
    if (!part)
    {
        return std::nullopt;
    }
    auto chartSpace = part->GetChartSpace();
    auto chart = chartSpace ? chartSpace->GetFirstChildOfType<Charts::Chart>() : nullptr;
    if (!chart)
    {
        return std::nullopt;
    }
    PresentationChartInfo info;
    info.HasEmbeddedWorkbook = part->GetEmbeddedPackagePart() != nullptr;
    info.Title = SharedCharts::ChartDom::ReadTitle(chart);
    SharedCharts::ChartPlotKind kind{};
    bool scatter = false;
    auto group = SharedCharts::ChartDom::FindPlotGroup(chart->GetFirstChildOfType<Charts::PlotArea>(), kind, scatter);
    info.Type = PresentationChartHelpers::ToPublicType(kind);
    for (const auto& series : SharedCharts::ChartDom::Series(group))
    {
        PresentationChartSeries entry;
        entry.Name = SharedCharts::ChartDom::ReadSeriesName(series);
        auto values = scatter ? std::static_pointer_cast<OpenXMLElement>(series->GetFirstChildOfType<Charts::YValues>())
                              : std::static_pointer_cast<OpenXMLElement>(series->GetFirstChildOfType<Charts::Values>());
        auto categories =
            scatter ? std::static_pointer_cast<OpenXMLElement>(series->GetFirstChildOfType<Charts::XValues>())
                    : std::static_pointer_cast<OpenXMLElement>(series->GetFirstChildOfType<Charts::CategoryAxisData>());
        entry.Values = SharedCharts::ChartDom::ReadNumericCache(values);
        if (categories)
        {
            entry.Categories = SharedCharts::ChartDom::ReadCategoryCache(categories);
        }
        info.Series.push_back(std::move(entry));
    }
    return info;
}

bool PresentationShape::UpdateChartData(const std::vector<PresentationChartSeries>& series,
                                        std::optional<std::string> title)
{
    if (series.empty())
    {
        return false;
    }
    auto part = PresentationChartHelpers::ResolveChartPart(m_element, m_slidePart);
    auto chartSpace = part ? part->GetChartSpace() : nullptr;
    auto chart = chartSpace ? chartSpace->GetFirstChildOfType<Charts::Chart>() : nullptr;
    if (!chart)
    {
        return false;
    }
    std::vector<SharedCharts::ChartLiteralSeries> literal;
    literal.reserve(series.size());
    for (const auto& entry : series)
    {
        literal.push_back({entry.Name, entry.Values, entry.Categories});
    }
    return SharedCharts::ChartDom::RewriteSeries(chart, literal, title);
}

std::optional<std::vector<Byte>> PresentationShape::GetChartEmbeddedWorkbook() const
{
    auto part = PresentationChartHelpers::ResolveChartPart(m_element, m_slidePart);
    auto embedded = part ? part->GetEmbeddedPackagePart() : nullptr;
    return embedded ? std::optional(embedded->GetBinaryData()) : std::nullopt;
}

bool PresentationShape::SetChartEmbeddedWorkbook(std::span<const Byte> bytes)
{
    auto part = PresentationChartHelpers::ResolveChartPart(m_element, m_slidePart);
    if (!part)
    {
        return false;
    }
    auto embedded = part->GetEmbeddedPackagePart();
    if (!embedded)
    {
        embedded = part->AddEmbeddedPackagePart();
    }
    if (!embedded)
    {
        return false;
    }
    embedded->SetBinaryData(std::vector<Byte>(bytes.begin(), bytes.end()));
    return true;
}

bool PresentationShape::RefreshChartDataFromEmbeddedWorkbook()
{
    auto part = PresentationChartHelpers::ResolveChartPart(m_element, m_slidePart);
    auto embeddedPart = part ? part->GetEmbeddedPackagePart() : nullptr;
    if (!embeddedPart)
    {
        return false;
    }
    auto workbook = Excel::ExcelDocumentEditor::Open(embeddedPart->GetBinaryData());
    if (!workbook)
    {
        return false;
    }
    auto chartSpace = part->GetChartSpace();
    auto chart = chartSpace ? chartSpace->GetFirstChildOfType<Charts::Chart>() : nullptr;
    SharedCharts::ChartPlotKind kind{};
    bool scatter = false;
    auto group = chart ? SharedCharts::ChartDom::FindPlotGroup(chart->GetFirstChildOfType<Charts::PlotArea>(), kind, scatter)
                       : nullptr;
    if (!group)
    {
        return false;
    }

    std::vector<PresentationChartSeries> refreshed;
    for (const auto& seriesNode : SharedCharts::ChartDom::Series(group))
    {
        auto valuesWrapper =
            scatter ? std::static_pointer_cast<OpenXMLElement>(seriesNode->GetFirstChildOfType<Charts::YValues>())
                    : std::static_pointer_cast<OpenXMLElement>(seriesNode->GetFirstChildOfType<Charts::Values>());
        auto values =
            PresentationChartHelpers::ReadLiveNumbers(workbook, SharedCharts::ChartDom::ReadRefFormula(valuesWrapper));
        if (!values)
        {
            return false;
        }

        PresentationChartSeries entry;
        entry.Name = SharedCharts::ChartDom::ReadSeriesName(seriesNode);
        entry.Values = std::move(*values);

        auto categoriesWrapper =
            scatter ? std::static_pointer_cast<OpenXMLElement>(seriesNode->GetFirstChildOfType<Charts::XValues>())
                    : std::static_pointer_cast<OpenXMLElement>(seriesNode->GetFirstChildOfType<Charts::CategoryAxisData>());
        if (categoriesWrapper)
        {
            const auto formula = SharedCharts::ChartDom::ReadRefFormula(categoriesWrapper);
            if (!formula.empty())
            {
                if (scatter)
                {
                    auto xValues = PresentationChartHelpers::ReadLiveNumbers(workbook, formula);
                    if (!xValues)
                    {
                        return false;
                    }
                    std::vector<std::string> asText;
                    asText.reserve(xValues->size());
                    for (Real value : *xValues)
                    {
                        asText.push_back(SharedCharts::ChartDom::FormatNumber(value));
                    }
                    entry.Categories = std::move(asText);
                }
                else
                {
                    auto labels = PresentationChartHelpers::ReadLiveLabels(workbook, formula);
                    if (!labels)
                    {
                        return false;
                    }
                    entry.Categories = std::move(*labels);
                }
            }
        }
        refreshed.push_back(std::move(entry));
    }
    if (refreshed.empty())
    {
        return false;
    }
    return UpdateChartData(refreshed);
}

std::optional<std::string> PresentationShape::GetChartStyleXml() const
{
    auto part = PresentationChartHelpers::ResolveChartPart(m_element, m_slidePart);
    if (!part)
    {
        return std::nullopt;
    }
    auto styles = part->GetChartStyleParts();
    return styles.empty() ? std::nullopt : std::optional(styles.front()->GetXmlString());
}

bool PresentationShape::SetChartStyleXml(const std::string& xml)
{
    auto part = PresentationChartHelpers::ResolveChartPart(m_element, m_slidePart);
    if (!part)
    {
        return false;
    }
    auto styles = part->GetChartStyleParts();
    auto style = styles.empty() ? part->AddChartStylePart() : styles.front();
    if (!style)
    {
        return false;
    }
    style->SetXmlString(xml);
    return true;
}

std::optional<std::string> PresentationShape::GetChartColorStyleXml() const
{
    auto part = PresentationChartHelpers::ResolveChartPart(m_element, m_slidePart);
    if (!part)
    {
        return std::nullopt;
    }
    auto styles = part->GetChartColorStyleParts();
    return styles.empty() ? std::nullopt : std::optional(styles.front()->GetXmlString());
}

bool PresentationShape::SetChartColorStyleXml(const std::string& xml)
{
    auto part = PresentationChartHelpers::ResolveChartPart(m_element, m_slidePart);
    if (!part)
    {
        return false;
    }
    auto styles = part->GetChartColorStyleParts();
    auto style = styles.empty() ? part->AddChartColorStylePart() : styles.front();
    if (!style)
    {
        return false;
    }
    style->SetXmlString(xml);
    return true;
}

} // namespace ExyokiOffice::PowerPoint
