// Copyright (c) 2026 Jakub Melka and Contributors
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "ExyokiOffice/Word/WordDocument.hpp"

#include "../Charts/ChartXml.hpp"
#include "ExyokiOffice/DOM/DocumentFormat/OpenXml/Drawing.hpp"
#include "ExyokiOffice/DOM/DocumentFormat/OpenXml/Drawing/Charts.hpp"
#include "ExyokiOffice/Packaging/GeneratedParts.hpp"
#include "ExyokiOffice/StandardTypes.hpp"

#include <array>
#include <charconv>

namespace ExyokiOffice::Word::Detail
{
namespace C = ExyokiOffice::DocumentFormat::OpenXml::Drawing::Charts;
namespace A = ExyokiOffice::DocumentFormat::OpenXml::Drawing;
using ExyokiOffice::Detail::Charts::ChartDom;
using ExyokiOffice::Detail::Charts::ChartPlotKind;
using ExyokiOffice::Detail::Charts::ChartSeriesRef;

class WordChartTypeMap
{
public:
    static WordChartType ToPublic(ChartPlotKind kind)
    {
        switch (kind)
        {
            case ChartPlotKind::Column:
                return WordChartType::Column;
            case ChartPlotKind::Bar:
                return WordChartType::Bar;
            case ChartPlotKind::Line:
                return WordChartType::Line;
            case ChartPlotKind::Pie:
                return WordChartType::Pie;
            case ChartPlotKind::Area:
                return WordChartType::Area;
            case ChartPlotKind::XyScatter:
                return WordChartType::XyScatter;
            case ChartPlotKind::Bubble:
                return WordChartType::Bubble;
            case ChartPlotKind::Unknown:
                return WordChartType::Unknown;
        }
        return WordChartType::Unknown;
    }
};

class WordChartReader
{
public:
    static void ParseChartPart(const std::shared_ptr<Packaging::ChartPart>& part, WordChartInfo& info)
    {
        info.HasEmbeddedWorkbook = part->GetEmbeddedPackagePart() != nullptr;
        auto chart = part->GetChartSpace()->GetFirstChildOfType<C::Chart>();
        if (!chart)
        {
            return;
        }
        info.Title = ChartDom::ReadTitle(chart);
        ChartPlotKind kind{};
        bool scatter = false;
        auto group = ChartDom::FindPlotGroup(chart->GetFirstChildOfType<C::PlotArea>(), kind, scatter);
        info.Type = WordChartTypeMap::ToPublic(kind);
        for (const auto& series : ChartDom::Series(group))
        {
            WordChartSeries entry;
            entry.Name = ChartDom::ReadSeriesName(series);
            auto values = scatter ? std::static_pointer_cast<OpenXMLElement>(series->GetFirstChildOfType<C::YValues>())
                                  : std::static_pointer_cast<OpenXMLElement>(series->GetFirstChildOfType<C::Values>());
            auto categories = scatter ? std::static_pointer_cast<OpenXMLElement>(series->GetFirstChildOfType<C::XValues>())
                                      : std::static_pointer_cast<OpenXMLElement>(series->GetFirstChildOfType<C::CategoryAxisData>());
            entry.Values = ChartDom::ReadNumericCache(values);
            if (categories)
            {
                entry.Categories = ChartDom::ReadCategoryCache(categories);
            }
            info.Series.push_back(std::move(entry));
        }
    }
};

class WordChartWriter
{
public:
    static bool Rewrite(const std::shared_ptr<Packaging::ChartPart>& part,
                        const std::vector<WordChartSeries>& data, const std::optional<std::string>& title)
    {
        auto chart = part->GetChartSpace()->GetFirstChildOfType<C::Chart>();
        if (!chart)
        {
            return false;
        }
        std::vector<ExyokiOffice::Detail::Charts::ChartLiteralSeries> literal;
        literal.reserve(data.size());
        for (const auto& series : data)
        {
            literal.push_back({series.Name, series.Values, series.Categories});
        }
        return ChartDom::RewriteSeries(chart, literal, title);
    }
};
} // namespace ExyokiOffice::Word::Detail

namespace ExyokiOffice::Word
{
namespace C = ExyokiOffice::DocumentFormat::OpenXml::Drawing::Charts;
namespace A = ExyokiOffice::DocumentFormat::OpenXml::Drawing;

std::vector<WordChartInfo> WordDocumentEditor::Charts() const
{
    std::vector<WordChartInfo> result;
    if (!m_document)
    {
        return result;
    }
    auto mainPart = m_document->GetMainDocumentPart();
    if (!mainPart || !mainPart->GetRootElement())
    {
        return result;
    }
    const auto parts = mainPart->GetChartParts();
    for (const auto& reference : mainPart->GetRootElement()->Descendants<C::ChartReference>())
    {
        auto graphicData = reference->Parent();
        if (!graphicData || graphicData->GetAttribute(OpenXmlQualifiedName({}, "uri")) !=
                                "http://schemas.openxmlformats.org/drawingml/2006/chart")
        {
            continue;
        }
        const std::string relationshipId = reference->GetId().ToString();
        for (const auto& part : parts)
        {
            if (part->RelationshipId() == relationshipId)
            {
                WordChartInfo info;
                info.RelationshipId = relationshipId;
                Detail::WordChartReader::ParseChartPart(part, info);
                result.push_back(std::move(info));
                break;
            }
        }
    }
    return result;
}

bool WordDocumentEditor::UpdateChartData(const std::string& id, const std::vector<WordChartSeries>& data,
                                         std::optional<std::string> title)
{
    if (!m_document || id.empty() || data.empty())
    {
        return false;
    }
    auto mainPart = m_document->GetMainDocumentPart();
    if (!mainPart)
    {
        return false;
    }
    for (const auto& part : mainPart->GetChartParts())
    {
        if (part->RelationshipId() == id)
        {
            return Detail::WordChartWriter::Rewrite(part, data, title);
        }
    }
    return false;
}

std::optional<std::vector<Byte>> WordDocumentEditor::GetChartEmbeddedWorkbook(const std::string& id) const
{
    if (!m_document)
    {
        return std::nullopt;
    }
    auto mainPart = m_document->GetMainDocumentPart();
    if (!mainPart)
    {
        return std::nullopt;
    }
    for (const auto& part : mainPart->GetChartParts())
    {
        if (part->RelationshipId() == id)
        {
            auto embedded = part->GetEmbeddedPackagePart();
            return embedded ? std::optional(embedded->GetBinaryData()) : std::nullopt;
        }
    }
    return std::nullopt;
}

bool WordDocumentEditor::SetChartEmbeddedWorkbook(const std::string& id, std::span<const Byte> bytes)
{
    if (!m_document)
    {
        return false;
    }
    auto mainPart = m_document->GetMainDocumentPart();
    if (!mainPart)
    {
        return false;
    }
    for (const auto& part : mainPart->GetChartParts())
    {
        if (part->RelationshipId() == id)
        {
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
    }
    return false;
}
} // namespace ExyokiOffice::Word
