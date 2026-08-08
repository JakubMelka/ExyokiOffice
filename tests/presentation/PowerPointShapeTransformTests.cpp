// Copyright (c) 2026 Jakub Melka and Collaborators
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "doctest.h"

#include "ExyokiOffice/DOM/DocumentFormat/OpenXml/Presentation.hpp"
#include "ExyokiOffice/PowerPoint/PowerPointDocument.hpp"
#include "ExyokiOffice/StandardTypes.hpp"

#include <limits>

using namespace ExyokiOffice::PowerPoint;

TEST_SUITE("PowerPointShapeTransformTests")
{
    TEST_CASE("shape transform accepts physical units and serializes integral EMU [unit] [powerpoint] [transform]")
    {
        using ExyokiOffice::MeasurementUnit;
        using ExyokiOffice::MeasuringUnits;
        auto editor = PowerPointDocumentEditor::CreateNew();
        auto shape = editor->AddSlide()->ShapeTree()->AddShape("Physical transform");
        PresentationShapeTransform expected{
            {MeasuringUnits(25.4, MeasurementUnit::Millimeter), MeasuringUnits(0.5, MeasurementUnit::Inch)},
            {MeasuringUnits(4.0, MeasurementUnit::Inch), MeasuringUnits(72.0, MeasurementUnit::Point)}};

        REQUIRE(shape->SetTransform(expected));
        CHECK(shape->GetTransform() == expected);
        const auto xml = editor->GetSlide(0)->GetPart()->GetXmlString();
        CHECK(xml.find("x=\"914400\"") != std::string::npos);
        CHECK(xml.find("y=\"457200\"") != std::string::npos);
        CHECK(xml.find("cx=\"3657600\"") != std::string::npos);
        CHECK(xml.find("cy=\"914400\"") != std::string::npos);

        auto reopened = PowerPointDocumentEditor::Open(editor->SaveToMemory());
        REQUIRE(reopened);
        CHECK(reopened->GetSlide(0)->ShapeTree()->Get(0)->GetTransform() == expected);
    }

    TEST_CASE("shape transform preserves native values through round trip [unit] [powerpoint] [transform]")
    {
        auto editor = PowerPointDocumentEditor::CreateNew();
        auto slide = editor->AddSlide();
        auto shape = slide->ShapeTree()->AddShape("Exact transform");
        PresentationShapeTransform expected{{-914400, 1828800}, {12192000, 6858000}, -5400000, true, false};
        REQUIRE(shape->SetTransform(expected));
        CHECK(shape->GetTransform() == expected);

        auto reopened = PowerPointDocumentEditor::Open(editor->SaveToMemory());
        REQUIRE(reopened);
        auto actual = reopened->GetSlide(0)->ShapeTree()->Get(0)->GetTransform();
        REQUIRE(actual);
        CHECK(*actual == expected);
    }

    TEST_CASE("group transform has an independent child coordinate system [unit] [powerpoint] [transform]")
    {
        auto editor = PowerPointDocumentEditor::CreateNew();
        auto tree = editor->AddSlide()->ShapeTree();
        tree->AddShape("A");
        tree->AddShape("B");
        auto group = tree->Group({0, 1});
        PresentationShapeTransform expected{{100, 200}, {3000, 4000}, 2700000, false, true, PresentationPoint{-500, -600}, PresentationSize{7000, 8000}};
        REQUIRE(group->SetTransform(expected));
        CHECK(group->GetTransform() == expected);
        auto reopened = PowerPointDocumentEditor::Open(editor->SaveToMemory());
        REQUIRE(reopened);
        CHECK(reopened->GetSlide(0)->ShapeTree()->Get(0)->GetTransform() == expected);
    }

    TEST_CASE("transform validation rejects invalid extents and coordinate models [unit] [powerpoint] [transform]")
    {
        auto editor = PowerPointDocumentEditor::CreateNew();
        auto tree = editor->AddSlide()->ShapeTree();
        auto shape = tree->AddShape();
        PresentationShapeTransform invalid;
        invalid.Size.Width = -1;
        CHECK_FALSE(shape->SetTransform(invalid));
        invalid.Size.Width = 1;
        invalid.GroupChildPosition = PresentationPoint{};
        CHECK_FALSE(shape->SetTransform(invalid));
        invalid = {};
        invalid.Position.X = ExyokiOffice::MeasuringUnits(std::numeric_limits<ExyokiOffice::Real>::quiet_NaN(),
                                                          ExyokiOffice::MeasurementUnit::Inch);
        CHECK_FALSE(shape->SetTransform(invalid));
        invalid.Position.X = ExyokiOffice::MeasuringUnits(std::numeric_limits<ExyokiOffice::Real>::infinity(),
                                                          ExyokiOffice::MeasurementUnit::Emu);
        CHECK_FALSE(shape->SetTransform(invalid));

        tree->AddShape();
        auto group = tree->Group({0, 1});
        PresentationShapeTransform missingChildCoordinates;
        CHECK_FALSE(group->SetTransform(missingChildCoordinates));
    }

    TEST_CASE("all transform-bearing PresentationML shape hosts use the same API [unit] [powerpoint] [transform]")
    {
        namespace Presentation = ExyokiOffice::DocumentFormat::OpenXml::Presentation;
        auto editor = PowerPointDocumentEditor::CreateNew();
        auto tree = editor->AddSlide()->ShapeTree();
        tree->AddShape();
        auto parent = tree->Get(0)->GetElement()->Parent();
        REQUIRE(parent);
        REQUIRE(parent->AppendChild<Presentation::Picture>());
        REQUIRE(parent->AppendChild<Presentation::ConnectionShape>());
        REQUIRE(parent->AppendChild<Presentation::GraphicFrame>());
        REQUIRE(parent->AppendChild<Presentation::ContentPart>());

        PresentationShapeTransform expected{{11, 22}, {33, 44}, 60000, true, true};
        REQUIRE(tree->Count() == 5);
        for (ExyokiOffice::Size index = 0; index < 4; ++index)
        {
            INFO("shape host index: " << index);
            REQUIRE(tree->Get(index)->SetTransform(expected));
            CHECK(tree->Get(index)->GetTransform() == expected);
        }
        CHECK_FALSE(tree->Get(4)->SetTransform(expected));
        CHECK_FALSE(tree->Get(4)->GetTransform().has_value());
    }
}
