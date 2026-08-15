// Copyright (c) 2026 Jakub Melka and Contributors
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "doctest.h"

#include "ExyokiOffice/DOM/DocumentFormat/OpenXml/Presentation.hpp"
#include "ExyokiOffice/PowerPoint/PowerPointDocument.hpp"

using namespace ExyokiOffice::PowerPoint;
using ExyokiOffice::AngleUnit;
using ExyokiOffice::Color;
using ExyokiOffice::MeasurementUnit;
using ExyokiOffice::MeasuringAngle;
using ExyokiOffice::MeasuringUnits;

namespace Drawing = ExyokiOffice::DocumentFormat::OpenXml::Drawing;

namespace
{
PresentationShape::Ptr FreshShape(PowerPointDocumentEditor::Ptr& editor, const char* name = "Styled")
{
    editor = PowerPointDocumentEditor::CreateNew();
    return editor->AddSlide()->ShapeTree()->AddShape(name);
}
} // namespace

TEST_SUITE("PowerPointShapeStyleTests")
{
    TEST_CASE("a fresh shape reports an inherited fill and no explicit outline [unit] [powerpoint] [shape-style]")
    {
        PowerPointDocumentEditor::Ptr editor;
        auto shape = FreshShape(editor);
        auto fill = shape->GetFill();
        REQUIRE(fill);
        CHECK(fill->Kind == PresentationFillKind::Inherited);
        CHECK_FALSE(shape->GetOutline());
    }

    TEST_CASE("solid fill round-trips through save and reopen [unit] [powerpoint] [shape-style]")
    {
        PowerPointDocumentEditor::Ptr editor;
        auto shape = FreshShape(editor);
        PresentationShapeFill fill;
        fill.Kind = PresentationFillKind::Solid;
        fill.ColorValue = Color(0x1F, 0x4E, 0x79);
        REQUIRE(shape->SetFill(fill));
        CHECK(shape->GetFill() == fill);

        const auto xml = editor->GetSlide(0)->GetPart()->GetXmlString();
        CHECK(xml.find("<a:solidFill>") != std::string::npos);
        CHECK(xml.find("val=\"1F4E79\"") != std::string::npos);

        auto reopened = PowerPointDocumentEditor::Open(editor->SaveToMemory());
        REQUIRE(reopened);
        CHECK(reopened->GetSlide(0)->ShapeTree()->Get(0)->GetFill() == fill);
    }

    TEST_CASE("no-fill fill writes an explicit noFill element [unit] [powerpoint] [shape-style]")
    {
        PowerPointDocumentEditor::Ptr editor;
        auto shape = FreshShape(editor);
        PresentationShapeFill fill;
        fill.Kind = PresentationFillKind::None;
        REQUIRE(shape->SetFill(fill));

        auto read = shape->GetFill();
        REQUIRE(read);
        CHECK(read->Kind == PresentationFillKind::None);

        const auto xml = editor->GetSlide(0)->GetPart()->GetXmlString();
        CHECK(xml.find("<a:noFill") != std::string::npos);
    }

    TEST_CASE("linear gradient fill round-trips stops and angle [unit] [powerpoint] [shape-style]")
    {
        PowerPointDocumentEditor::Ptr editor;
        auto shape = FreshShape(editor);
        PresentationShapeFill fill;
        fill.Kind = PresentationFillKind::Gradient;
        fill.GradientStops = {{Color(0xFF, 0x00, 0x00), 0.0},
                              {Color(0x00, 0x80, 0x00), 50.0},
                              {Color(0x00, 0x00, 0xFF), 100.0}};
        fill.GradientAngle = MeasuringAngle(90.0, AngleUnit::Degree);
        REQUIRE(shape->SetFill(fill));
        CHECK(shape->GetFill() == fill);

        const auto xml = editor->GetSlide(0)->GetPart()->GetXmlString();
        CHECK(xml.find("<a:gradFill>") != std::string::npos);
        CHECK(xml.find("<a:gsLst>") != std::string::npos);
        CHECK(xml.find("pos=\"50000\"") != std::string::npos);
        CHECK(xml.find("ang=\"5400000\"") != std::string::npos);

        auto reopened = PowerPointDocumentEditor::Open(editor->SaveToMemory());
        REQUIRE(reopened);
        CHECK(reopened->GetSlide(0)->ShapeTree()->Get(0)->GetFill() == fill);
    }

    TEST_CASE("setting a fill replaces any previously authored fill [unit] [powerpoint] [shape-style]")
    {
        PowerPointDocumentEditor::Ptr editor;
        auto shape = FreshShape(editor);

        PresentationShapeFill solid;
        solid.Kind = PresentationFillKind::Solid;
        solid.ColorValue = Color(0x10, 0x20, 0x30);
        REQUIRE(shape->SetFill(solid));

        PresentationShapeFill gradient;
        gradient.Kind = PresentationFillKind::Gradient;
        gradient.GradientStops = {{Color(0, 0, 0), 0.0}, {Color(0xFF, 0xFF, 0xFF), 100.0}};
        REQUIRE(shape->SetFill(gradient));

        const auto xml = editor->GetSlide(0)->GetPart()->GetXmlString();
        CHECK(xml.find("<a:solidFill>") == std::string::npos);
        CHECK(xml.find("<a:gradFill>") != std::string::npos);

        // Restoring inherited removes every fill element again.
        PresentationShapeFill inherited;
        REQUIRE(shape->SetFill(inherited));
        auto read = shape->GetFill();
        REQUIRE(read);
        CHECK(read->Kind == PresentationFillKind::Inherited);
        const auto cleared = editor->GetSlide(0)->GetPart()->GetXmlString();
        CHECK(cleared.find("<a:gradFill>") == std::string::npos);
        CHECK(cleared.find("<a:noFill") == std::string::npos);
    }

    TEST_CASE("invalid fill data is rejected without changing the shape [unit] [powerpoint] [shape-style]")
    {
        PowerPointDocumentEditor::Ptr editor;
        auto shape = FreshShape(editor);

        PresentationShapeFill autoSolid;
        autoSolid.Kind = PresentationFillKind::Solid;
        autoSolid.ColorValue = Color(); // automatic
        CHECK_FALSE(shape->SetFill(autoSolid));

        PresentationShapeFill oneStop;
        oneStop.Kind = PresentationFillKind::Gradient;
        oneStop.GradientStops = {{Color(1, 2, 3), 0.0}};
        CHECK_FALSE(shape->SetFill(oneStop));

        PresentationShapeFill badPosition;
        badPosition.Kind = PresentationFillKind::Gradient;
        badPosition.GradientStops = {{Color(1, 2, 3), 0.0}, {Color(4, 5, 6), 150.0}};
        CHECK_FALSE(shape->SetFill(badPosition));

        PresentationShapeFill autoStop;
        autoStop.Kind = PresentationFillKind::Gradient;
        autoStop.GradientStops = {{Color(1, 2, 3), 0.0}, {Color(), 100.0}};
        CHECK_FALSE(shape->SetFill(autoStop));

        // None of the rejected calls wrote a fill element.
        auto read = shape->GetFill();
        REQUIRE(read);
        CHECK(read->Kind == PresentationFillKind::Inherited);
    }

    TEST_CASE("solid outline round-trips width, color, cap, compound, and dash [unit] [powerpoint] [shape-style]")
    {
        PowerPointDocumentEditor::Ptr editor;
        auto shape = FreshShape(editor);
        PresentationShapeOutline outline;
        outline.Fill = PresentationFillKind::Solid;
        outline.ColorValue = Color(0x00, 0x00, 0x00);
        outline.Width = MeasuringUnits(2.0, MeasurementUnit::Point);
        outline.Cap = Drawing::LineCapValues::Round;
        outline.Compound = Drawing::CompoundLineValues::Single;
        outline.Dash = Drawing::PresetLineDashValues::Dash;
        REQUIRE(shape->SetOutline(outline));
        CHECK(shape->GetOutline() == outline);

        const auto xml = editor->GetSlide(0)->GetPart()->GetXmlString();
        CHECK(xml.find("<a:ln ") != std::string::npos);
        CHECK(xml.find("w=\"25400\"") != std::string::npos);
        CHECK(xml.find("<a:prstDash val=\"dash\"") != std::string::npos);

        auto reopened = PowerPointDocumentEditor::Open(editor->SaveToMemory());
        REQUIRE(reopened);
        CHECK(reopened->GetSlide(0)->ShapeTree()->Get(0)->GetOutline() == outline);
    }

    TEST_CASE("no-color outline round-trips only its explicit attributes [unit] [powerpoint] [shape-style]")
    {
        PowerPointDocumentEditor::Ptr editor;
        auto shape = FreshShape(editor);
        PresentationShapeOutline outline;
        outline.Fill = PresentationFillKind::None;
        outline.Width = MeasuringUnits(1.0, MeasurementUnit::Point);
        REQUIRE(shape->SetOutline(outline));
        CHECK(shape->GetOutline() == outline);

        const auto xml = editor->GetSlide(0)->GetPart()->GetXmlString();
        CHECK(xml.find("<a:ln ") != std::string::npos);
        CHECK(xml.find("<a:noFill") != std::string::npos);
    }

    TEST_CASE("an inherited-color outline preserves width without a color element [unit] [powerpoint] [shape-style]")
    {
        PowerPointDocumentEditor::Ptr editor;
        auto shape = FreshShape(editor);
        PresentationShapeOutline outline;
        outline.Fill = PresentationFillKind::Inherited;
        outline.Width = MeasuringUnits(3.0, MeasurementUnit::Point);
        REQUIRE(shape->SetOutline(outline));

        auto read = shape->GetOutline();
        REQUIRE(read);
        CHECK(read->Fill == PresentationFillKind::Inherited);
        REQUIRE(read->Width);
        CHECK(*read->Width == MeasuringUnits(3.0, MeasurementUnit::Point));
    }

    TEST_CASE("outline validation rejects gradients, automatic colors, and negative widths [unit] [powerpoint] [shape-style]")
    {
        PowerPointDocumentEditor::Ptr editor;
        auto shape = FreshShape(editor);

        PresentationShapeOutline gradient;
        gradient.Fill = PresentationFillKind::Gradient;
        CHECK_FALSE(shape->SetOutline(gradient));

        PresentationShapeOutline autoColor;
        autoColor.Fill = PresentationFillKind::Solid;
        autoColor.ColorValue = Color();
        CHECK_FALSE(shape->SetOutline(autoColor));

        PresentationShapeOutline negative;
        negative.Fill = PresentationFillKind::Solid;
        negative.ColorValue = Color(1, 1, 1);
        negative.Width = MeasuringUnits(-1.0, MeasurementUnit::Point);
        CHECK_FALSE(shape->SetOutline(negative));

        CHECK_FALSE(shape->GetOutline());
    }

    TEST_CASE("fill and outline coexist in the correct schema order [unit] [powerpoint] [shape-style]")
    {
        PowerPointDocumentEditor::Ptr editor;
        auto shape = FreshShape(editor);
        PresentationShapeFill fill;
        fill.Kind = PresentationFillKind::Solid;
        fill.ColorValue = Color(0xAB, 0xCD, 0xEF);
        PresentationShapeOutline outline;
        outline.Fill = PresentationFillKind::Solid;
        outline.ColorValue = Color(0x12, 0x34, 0x56);
        outline.Width = MeasuringUnits(1.5, MeasurementUnit::Point);
        REQUIRE(shape->SetFill(fill));
        REQUIRE(shape->SetOutline(outline));

        const auto xml = editor->GetSlide(0)->GetPart()->GetXmlString();
        const auto fillPos = xml.find("val=\"ABCDEF\""); // the shape fill color
        const auto linePos = xml.find("<a:ln ");
        REQUIRE(fillPos != std::string::npos);
        REQUIRE(linePos != std::string::npos);
        CHECK(fillPos < linePos); // fill precedes line in CT_ShapeProperties

        auto reopened = PowerPointDocumentEditor::Open(editor->SaveToMemory());
        REQUIRE(reopened);
        auto roundTripped = reopened->GetSlide(0)->ShapeTree()->Get(0);
        CHECK(roundTripped->GetFill() == fill);
        CHECK(roundTripped->GetOutline() == outline);
    }

    TEST_CASE("groups and graphic frames do not expose shape fill or outline [unit] [powerpoint] [shape-style]")
    {
        auto editor = PowerPointDocumentEditor::CreateNew();
        auto tree = editor->AddSlide()->ShapeTree();
        tree->AddShape("A");
        tree->AddShape("B");
        auto group = tree->Group({0, 1});
        REQUIRE(group);
        REQUIRE(group->IsGroup());

        PresentationShapeFill fill;
        fill.Kind = PresentationFillKind::Solid;
        fill.ColorValue = Color(1, 2, 3);
        CHECK_FALSE(group->SetFill(fill));
        CHECK_FALSE(group->GetFill());

        PresentationShapeOutline outline;
        outline.Fill = PresentationFillKind::Solid;
        outline.ColorValue = Color(1, 2, 3);
        CHECK_FALSE(group->SetOutline(outline));
        CHECK_FALSE(group->GetOutline());
    }

    TEST_CASE("connector shapes support fill and outline styling [unit] [powerpoint] [shape-style]")
    {
        auto editor = PowerPointDocumentEditor::CreateNew();
        auto connector = editor->AddSlide()->ShapeTree()->AddConnector("Line");
        REQUIRE(connector);
        PresentationShapeOutline outline;
        outline.Fill = PresentationFillKind::Solid;
        outline.ColorValue = Color(0x80, 0x00, 0x00);
        outline.Width = MeasuringUnits(1.0, MeasurementUnit::Point);
        REQUIRE(connector->SetOutline(outline));
        CHECK(connector->GetOutline() == outline);
    }

    TEST_CASE("a fresh shape reports empty effects and no effect list [unit] [powerpoint] [shape-style]")
    {
        PowerPointDocumentEditor::Ptr editor;
        auto shape = FreshShape(editor);
        auto effects = shape->GetEffects();
        REQUIRE(effects);
        CHECK_FALSE(effects->Glow);
        CHECK_FALSE(effects->Shadow);
        CHECK_FALSE(effects->Reflection);
        CHECK(editor->GetSlide(0)->GetPart()->GetXmlString().find("<a:effectLst") == std::string::npos);
    }

    TEST_CASE("shape glow, shadow, and reflection round-trip together [unit] [powerpoint] [shape-style]")
    {
        PowerPointDocumentEditor::Ptr editor;
        auto shape = FreshShape(editor);
        PresentationShapeEffects effects;

        PresentationTextGlow glow;
        glow.Radius = MeasuringUnits(5.0, MeasurementUnit::Point);
        glow.ColorValue = Color(0xFF, 0xD7, 0x00);
        effects.Glow = glow;

        PresentationTextShadow shadow;
        shadow.BlurRadius = MeasuringUnits(4.0, MeasurementUnit::Point);
        shadow.Distance = MeasuringUnits(3.0, MeasurementUnit::Point);
        shadow.ColorValue = Color(0x40, 0x40, 0x40);
        shadow.Direction = MeasuringAngle(45.0, AngleUnit::Degree);
        effects.Shadow = shadow;

        PresentationTextReflection reflection;
        reflection.BlurRadius = MeasuringUnits(2.0, MeasurementUnit::Point);
        reflection.Distance = MeasuringUnits(1.0, MeasurementUnit::Point);
        effects.Reflection = reflection;

        REQUIRE(shape->SetEffects(effects));
        CHECK(shape->GetEffects() == effects);

        const auto xml = editor->GetSlide(0)->GetPart()->GetXmlString();
        CHECK(xml.find("<a:effectLst") != std::string::npos);
        CHECK(xml.find("<a:glow") != std::string::npos);
        CHECK(xml.find("<a:outerShdw") != std::string::npos);
        CHECK(xml.find("<a:reflection") != std::string::npos);

        auto reopened = PowerPointDocumentEditor::Open(editor->SaveToMemory());
        REQUIRE(reopened);
        CHECK(reopened->GetSlide(0)->ShapeTree()->Get(0)->GetEffects() == effects);
    }

    TEST_CASE("setting empty effects clears an existing effect list [unit] [powerpoint] [shape-style]")
    {
        PowerPointDocumentEditor::Ptr editor;
        auto shape = FreshShape(editor);
        PresentationShapeEffects effects;
        PresentationTextGlow glow;
        glow.Radius = MeasuringUnits(3.0, MeasurementUnit::Point);
        glow.ColorValue = Color(0x00, 0xFF, 0x00);
        effects.Glow = glow;
        REQUIRE(shape->SetEffects(effects));
        REQUIRE(editor->GetSlide(0)->GetPart()->GetXmlString().find("<a:glow") != std::string::npos);

        REQUIRE(shape->SetEffects({}));
        auto cleared = shape->GetEffects();
        REQUIRE(cleared);
        CHECK_FALSE(cleared->Glow);
        CHECK(editor->GetSlide(0)->GetPart()->GetXmlString().find("<a:effectLst") == std::string::npos);
    }

    TEST_CASE("invalid shape effects are rejected without mutation [unit] [powerpoint] [shape-style]")
    {
        PowerPointDocumentEditor::Ptr editor;
        auto shape = FreshShape(editor);

        PresentationShapeEffects autoGlow;
        PresentationTextGlow glow;
        glow.Radius = MeasuringUnits(3.0, MeasurementUnit::Point);
        glow.ColorValue = Color(); // automatic color is invalid for an effect
        autoGlow.Glow = glow;
        CHECK_FALSE(shape->SetEffects(autoGlow));

        PresentationShapeEffects negativeShadow;
        PresentationTextShadow shadow;
        shadow.BlurRadius = MeasuringUnits(-1.0, MeasurementUnit::Point);
        shadow.Distance = MeasuringUnits(1.0, MeasurementUnit::Point);
        shadow.ColorValue = Color(1, 1, 1);
        negativeShadow.Shadow = shadow;
        CHECK_FALSE(shape->SetEffects(negativeShadow));

        auto effects = shape->GetEffects();
        REQUIRE(effects);
        CHECK_FALSE(effects->Glow);
        CHECK_FALSE(effects->Shadow);
    }

    TEST_CASE("groups and graphic frames do not expose shape effects [unit] [powerpoint] [shape-style]")
    {
        auto editor = PowerPointDocumentEditor::CreateNew();
        auto tree = editor->AddSlide()->ShapeTree();
        tree->AddShape("A");
        tree->AddShape("B");
        auto group = tree->Group({0, 1});
        REQUIRE(group);

        PresentationShapeEffects effects;
        PresentationTextGlow glow;
        glow.Radius = MeasuringUnits(2.0, MeasurementUnit::Point);
        glow.ColorValue = Color(1, 2, 3);
        effects.Glow = glow;
        CHECK_FALSE(group->SetEffects(effects));
        CHECK_FALSE(group->GetEffects());
    }
}
