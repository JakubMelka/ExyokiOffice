// Copyright (c) 2026 Jakub Melka and Collaborators
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "doctest.h"

#include "ExyokiOffice/DOM/DocumentFormat/OpenXml/Drawing.hpp"
#include "ExyokiOffice/DOM/DocumentFormat/OpenXml/Drawing/Charts.hpp"
#include "ExyokiOffice/DOM/DocumentFormat/OpenXml/Drawing/Diagrams.hpp"
#include "ExyokiOffice/PowerPoint/PowerPointDocument.hpp"

using namespace ExyokiOffice;
using namespace ExyokiOffice::PowerPoint;
namespace Drawing = ExyokiOffice::DocumentFormat::OpenXml::Drawing;
namespace Charts = ExyokiOffice::DocumentFormat::OpenXml::Drawing::Charts;
namespace Diagrams = ExyokiOffice::DocumentFormat::OpenXml::Drawing::Diagrams;

namespace
{
PresentationTableData OneCellTable()
{
    PresentationTableData value;
    value.ColumnWidths = {100};
    value.Rows = {{100, {{"temporary"}}}};
    return value;
}

std::pair<PresentationShape::Ptr, Drawing::GraphicData::Ptr> EmptyGraphicFrame(const PresentationSlide::Ptr& slide)
{
    auto shape = slide->ShapeTree()->AddTable(OneCellTable());
    auto frame = std::dynamic_pointer_cast<DocumentFormat::OpenXml::Presentation::GraphicFrame>(shape->GetElement());
    auto data = frame->GetFirstChildOfType<Drawing::Graphic>()->GetFirstChildOfType<Drawing::GraphicData>();
    for (const auto& child : data->Children())
    {
        data->RemoveChild(child);
    }
    return {shape, data};
}

PresentationShape::Ptr AddChart(const PresentationSlide::Ptr& slide)
{
    auto [shape, data] = EmptyGraphicFrame(slide);
    data->SetUri(StringValue("http://schemas.openxmlformats.org/drawingml/2006/chart"));

    auto part = slide->GetPart()->AddChartPart();
    part->SetXmlString("<c:chartSpace xmlns:c=\"http://schemas.openxmlformats.org/drawingml/2006/chart\"><c:roundedCorners val=\"0\"/></c:chartSpace>");
    auto reference = data->AppendChild<Charts::ChartReference>();
    reference->SetId(StringValue(part->RelationshipId()));
    return shape;
}
} // namespace

TEST_SUITE("PowerPointEmbeddedObjectTests")
{
    TEST_CASE("chart payload is discovered and replaced without changing its relationship [unit] [powerpoint] [embedded-object]")
    {
        auto editor = PowerPointDocumentEditor::CreateNew();
        auto shape = AddChart(editor->AddSlide());
        auto object = shape->GetEmbeddedObject();
        REQUIRE(object);
        CHECK(object->Kind == PresentationEmbeddedObjectKind::Chart);
        REQUIRE(object->Payloads.size() == 1);
        const auto relationshipId = object->Payloads[0].RelationshipId;
        auto replacement = object->Payloads[0];
        replacement.Xml = "<c:chartSpace xmlns:c=\"http://schemas.openxmlformats.org/drawingml/2006/chart\"><c:roundedCorners val=\"1\"/></c:chartSpace>";
        REQUIRE(shape->ReplaceEmbeddedObjectPayload(replacement));
        CHECK(shape->GetEmbeddedObject()->Payloads[0].RelationshipId == relationshipId);
        REQUIRE(shape->GetEmbeddedObject()->Payloads[0].Xml);
        CHECK(shape->GetEmbeddedObject()->Payloads[0].Xml->find("val=\"1\"") != std::string::npos);

        auto reopened = PowerPointDocumentEditor::Open(editor->SaveToMemory());
        REQUIRE(reopened);
        auto persisted = reopened->GetSlide(0)->ShapeTree()->Get(0)->GetEmbeddedObject();
        REQUIRE(persisted);
        CHECK(persisted->Payloads[0].RelationshipId == relationshipId);
        REQUIRE(persisted->Payloads[0].Xml);
        CHECK(persisted->Payloads[0].Xml->find("val=\"1\"") != std::string::npos);
    }

    TEST_CASE("payload replacement rejects unrelated ids and representation mismatches transactionally [unit] [powerpoint] [embedded-object]")
    {
        auto editor = PowerPointDocumentEditor::CreateNew();
        auto shape = AddChart(editor->AddSlide());
        const auto original = shape->GetEmbeddedObject()->Payloads[0];
        auto invalid = original;
        invalid.RelationshipId = "rId999";
        CHECK_FALSE(shape->ReplaceEmbeddedObjectPayload(invalid));
        invalid = original;
        invalid.BinaryData = {1, 2, 3};
        CHECK_FALSE(shape->ReplaceEmbeddedObjectPayload(invalid));
        CHECK(shape->GetEmbeddedObject()->Payloads[0] == original);
        CHECK_FALSE(editor->GetSlide(0)->ShapeTree()->AddShape()->GetEmbeddedObject());
    }

    TEST_CASE("SmartArt exposes every referenced definition payload in markup order [unit] [powerpoint] [embedded-object]")
    {
        auto editor = PowerPointDocumentEditor::CreateNew();
        auto slide = editor->AddSlide();
        auto [shape, data] = EmptyGraphicFrame(slide);
        data->SetUri(StringValue("http://schemas.openxmlformats.org/drawingml/2006/diagram"));
        auto diagramData = slide->GetPart()->AddDiagramDataPart();
        diagramData->SetXmlString("<dgm:dataModel xmlns:dgm=\"http://schemas.openxmlformats.org/drawingml/2006/diagram\"/>");
        auto diagramStyle = slide->GetPart()->AddDiagramStylePart();
        diagramStyle->SetXmlString("<dgm:styleDef xmlns:dgm=\"http://schemas.openxmlformats.org/drawingml/2006/diagram\" uniqueId=\"test\"/>");
        auto ids = data->AppendChild<Diagrams::RelationshipIds>();
        ids->SetDataPart(StringValue(diagramData->RelationshipId()));
        ids->SetStylePart(StringValue(diagramStyle->RelationshipId()));

        auto object = shape->GetEmbeddedObject();
        REQUIRE(object);
        CHECK(object->Kind == PresentationEmbeddedObjectKind::SmartArt);
        REQUIRE(object->Payloads.size() == 2);
        CHECK(object->Payloads[0].RelationshipId == diagramData->RelationshipId());
        CHECK(object->Payloads[1].RelationshipId == diagramStyle->RelationshipId());
    }

    TEST_CASE("OLE binary payload replacement preserves the relationship and content type [unit] [powerpoint] [embedded-object]")
    {
        auto editor = PowerPointDocumentEditor::CreateNew();
        auto slide = editor->AddSlide();
        auto [shape, data] = EmptyGraphicFrame(slide);
        data->SetUri(StringValue("http://schemas.openxmlformats.org/presentationml/2006/ole"));
        auto part = slide->GetPart()->AddEmbeddedObjectPart();
        part->SetContentType("application/vnd.ms-office.oleObject");
        part->SetBinaryData({1, 2, 3});
        auto objectMarkup = data->AppendChild<DocumentFormat::OpenXml::Presentation::OleObject>();
        objectMarkup->SetId(StringValue(part->RelationshipId()));

        auto object = shape->GetEmbeddedObject();
        REQUIRE(object);
        CHECK(object->Kind == PresentationEmbeddedObjectKind::Ole);
        REQUIRE(object->Payloads.size() == 1);
        auto replacement = object->Payloads[0];
        replacement.BinaryData = {9, 8, 7, 6};
        REQUIRE(shape->ReplaceEmbeddedObjectPayload(replacement));
        CHECK(shape->GetEmbeddedObject()->Payloads[0].BinaryData == replacement.BinaryData);
        CHECK(shape->GetEmbeddedObject()->Payloads[0].ContentType == "application/vnd.ms-office.oleObject");
    }
} // TEST_SUITE("PowerPointEmbeddedObjectTests")
