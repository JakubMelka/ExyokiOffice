// Copyright (c) 2026 Jakub Melka and Contributors
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "doctest.h"

#include "ExyokiOffice/OpenXmlPackageValidator.hpp"
#include "ExyokiOffice/PowerPoint/PowerPointDocument.hpp"

using namespace ExyokiOffice::PowerPoint;
namespace Drawing = ExyokiOffice::DocumentFormat::OpenXml::Drawing;

TEST_SUITE("PowerPointGeometryTests")
{
    TEST_CASE("preset geometry and adjustments round trip losslessly [unit] [powerpoint] [geometry]")
    {
        auto editor = PowerPointDocumentEditor::CreateNew();
        auto shape = editor->AddSlide()->ShapeTree()->AddShape("Adjusted round rectangle");
        const std::vector<PresentationGeometryAdjustment> adjustments{{"adj", "val 25000"},
                                                                      {"corner", "+- 50000 adj 0"}};
        REQUIRE(shape->SetPresetGeometry(Drawing::ShapeTypeValues::RoundRectangle, adjustments));
        CHECK(shape->GetPresetGeometry() == Drawing::ShapeTypeValues::RoundRectangle);
        CHECK(shape->GeometryAdjustments() == adjustments);

        auto reopened = PowerPointDocumentEditor::Open(editor->SaveToMemory());
        REQUIRE(reopened);
        auto actual = reopened->GetSlide(0)->ShapeTree()->Get(0);
        CHECK(actual->GetPresetGeometry() == Drawing::ShapeTypeValues::RoundRectangle);
        CHECK(actual->GeometryAdjustments() == adjustments);
    }

    TEST_CASE("freeform paths and connection sites preserve formulas and command order [unit] [powerpoint] [geometry]")
    {
        auto editor = PowerPointDocumentEditor::CreateNew();
        auto shape = editor->AddSlide()->ShapeTree()->AddShape("Freeform");
        const PresentationFreeformPath path{100000, 80000, true, {{PresentationPathCommandType::MoveTo, {{"0", "0"}}}, {PresentationPathCommandType::LineTo, {{"100000", "0"}}}, {PresentationPathCommandType::QuadraticBezierTo, {{"wd2", "hd2"}, {"100000", "80000"}}}, {PresentationPathCommandType::CubicBezierTo, {{"75000", "80000"}, {"25000", "80000"}, {"0", "0"}}}, {PresentationPathCommandType::Close, {}}}};
        const std::vector<PresentationConnectionSite> sites{{"0", {"r", "vc"}}, {"cd2", {"l", "vc"}}};
        REQUIRE(shape->SetFreeformGeometry({path}, sites));
        CHECK(shape->FreeformPaths() == std::vector<PresentationFreeformPath>{path});
        CHECK(shape->ConnectionSites() == sites);
        CHECK_FALSE(shape->GetPresetGeometry().has_value());

        auto reopened = PowerPointDocumentEditor::Open(editor->SaveToMemory());
        REQUIRE(reopened);
        auto actual = reopened->GetSlide(0)->ShapeTree()->Get(0);
        CHECK(actual->FreeformPaths() == std::vector<PresentationFreeformPath>{path});
        CHECK(actual->ConnectionSites() == sites);
    }

    TEST_CASE("connector endpoints can be assigned cleared and round tripped [unit] [powerpoint] [geometry]")
    {
        auto editor = PowerPointDocumentEditor::CreateNew();
        auto tree = editor->AddSlide()->ShapeTree();
        tree->AddShape("Source");
        tree->AddShape("Target");
        auto connector = tree->AddConnector("Link");
        REQUIRE(connector);
        const PresentationConnectorEndpoint start{2, 1};
        const PresentationConnectorEndpoint end{3, 4};
        REQUIRE(connector->SetConnectorEndpoints(start, end));
        CHECK(connector->StartEndpoint() == start);
        CHECK(connector->EndEndpoint() == end);
        CHECK(connector->GetPresetGeometry() == Drawing::ShapeTypeValues::Line);

        auto reopened = PowerPointDocumentEditor::Open(editor->SaveToMemory());
        REQUIRE(reopened);
        auto actual = reopened->GetSlide(0)->ShapeTree()->Get(2);
        CHECK(actual->StartEndpoint() == start);
        CHECK(actual->EndEndpoint() == end);
        REQUIRE(actual->SetConnectorEndpoints(std::nullopt, end));
        CHECK_FALSE(actual->StartEndpoint().has_value());
        CHECK(actual->EndEndpoint() == end);
    }

    TEST_CASE("geometry API rejects structurally invalid values without replacing geometry [unit] [powerpoint] [geometry]")
    {
        auto editor = PowerPointDocumentEditor::CreateNew();
        auto shape = editor->AddSlide()->ShapeTree()->AddShape();
        REQUIRE(shape->SetPresetGeometry(Drawing::ShapeTypeValues::Rectangle));
        CHECK_FALSE(shape->SetPresetGeometry(Drawing::ShapeTypeValues::InvalidEnumValue));
        CHECK_FALSE(shape->SetPresetGeometry(Drawing::ShapeTypeValues::Ellipse, {{"", "val 1"}}));
        CHECK_FALSE(shape->SetFreeformGeometry({}));
        PresentationFreeformPath bad{-1, 10, true, {{PresentationPathCommandType::MoveTo, {{"0", "0"}}}}};
        CHECK_FALSE(shape->SetFreeformGeometry({bad}));
        bad.Width = 10;
        bad.Commands.front().Points.push_back({"1", "1"});
        CHECK_FALSE(shape->SetFreeformGeometry({bad}));
        CHECK(shape->GetPresetGeometry() == Drawing::ShapeTypeValues::Rectangle);
        CHECK_FALSE(shape->SetConnectorEndpoints({PresentationConnectorEndpoint{2, 0}}, std::nullopt));
    }

    TEST_CASE("generated geometry package is accepted by the OpenXML validator [unit] [powerpoint] [geometry]")
    {
        auto editor = PowerPointDocumentEditor::CreateNew();
        auto tree = editor->AddSlide()->ShapeTree();
        auto shape = tree->AddShape();
        REQUIRE(shape->SetPresetGeometry(Drawing::ShapeTypeValues::Rectangle, {{"adj", "val 10000"}}));
        auto connector = tree->AddConnector();
        REQUIRE(connector);
        REQUIRE(connector->SetConnectorEndpoints({PresentationConnectorEndpoint{2, 0}},
                                                 {PresentationConnectorEndpoint{2, 1}}));
        const auto validation = ExyokiOffice::OpenXmlPackageValidator(ExyokiOffice::OpenXmlDomValidationSettings{}).Validate(*editor->GetDocument());
        CHECK(validation.IsValid());
    }
}
