// Copyright (c) 2026 Jakub Melka and Contributors
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "doctest.h"

#include "ExyokiOffice/OpenXmlPackageValidator.hpp"
#include "ExyokiOffice/PowerPoint/PowerPointDocument.hpp"
#include "ExyokiOffice/StandardTypes.hpp"

#include <limits>
#include <numbers>

using namespace ExyokiOffice::PowerPoint;
namespace Drawing = ExyokiOffice::DocumentFormat::OpenXml::Drawing;

TEST_SUITE("PowerPointTextFrameTests")
{
    TEST_CASE("rich run and paragraph formatting round trips through DrawingML [unit] [powerpoint] [text-frame]")
    {
        using ExyokiOffice::Color;
        using ExyokiOffice::MeasurementUnit;
        using ExyokiOffice::MeasuringUnits;
        auto editor = PowerPointDocumentEditor::CreateNew();
        auto shape = editor->AddSlide()->ShapeTree()->AddShape("Rich text");

        PresentationTextRun run;
        run.Text = "Formatted";
        run.Language = "en-US";
        run.Bold = true;
        run.Italic = true;
        run.Underline = Drawing::TextUnderlineValues::WavyDouble;
        run.Strike = Drawing::TextStrikeValues::SingleStrike;
        run.Capitalization = Drawing::TextCapsValues::Small;
        run.Typeface = "Aptos Display";
        run.FontSize = MeasuringUnits(18.0, MeasurementUnit::Point);
        run.FontColor = Color(12, 34, 56);
        run.CharacterSpacing = MeasuringUnits(0.5, MeasurementUnit::Point);

        PresentationTextParagraph paragraph;
        paragraph.Runs = {run};
        paragraph.Alignment = Drawing::TextAlignmentTypeValues::Distributed;
        paragraph.LeftMargin = MeasuringUnits(1.0, MeasurementUnit::Centimeter);
        paragraph.RightMargin = MeasuringUnits(0.5, MeasurementUnit::Inch);
        paragraph.FirstLineIndent = MeasuringUnits(-6.0, MeasurementUnit::Point);
        paragraph.DefaultTabSize = MeasuringUnits(0.25, MeasurementUnit::Inch);
        paragraph.LineSpacing = PresentationTextSpacing{std::nullopt, 125.0};
        paragraph.SpaceBefore = PresentationTextSpacing{MeasuringUnits(6.0, MeasurementUnit::Point), std::nullopt};
        paragraph.SpaceAfter = PresentationTextSpacing{MeasuringUnits(8.5, MeasurementUnit::Point), std::nullopt};
        paragraph.Level = 3;
        paragraph.RightToLeft = true;
        paragraph.FontAlignment = Drawing::TextFontAlignmentValues::Baseline;

        PresentationTextFrame expected;
        expected.Paragraphs = {paragraph};
        REQUIRE(shape->SetTextFrame(expected));
        CHECK(shape->GetTextFrame() == expected);
        const auto xml = editor->GetSlide(0)->GetPart()->GetXmlString();
        CHECK(xml.find("typeface=\"Aptos Display\"") != std::string::npos);
        CHECK(xml.find("val=\"0C2238\"") != std::string::npos);
        CHECK(xml.find("sz=\"1800\"") != std::string::npos);
        CHECK(xml.find("val=\"125000\"") != std::string::npos);

        auto reopened = PowerPointDocumentEditor::Open(editor->SaveToMemory());
        REQUIRE(reopened);
        CHECK(reopened->GetSlide(0)->ShapeTree()->Get(0)->GetTextFrame() == expected);
        CHECK(ExyokiOffice::OpenXmlPackageValidator(ExyokiOffice::OpenXmlDomValidationSettings{}).Validate(*reopened->GetDocument()).IsValid());
    }

    TEST_CASE("text frame formatting and content round trip without DOM access [unit] [powerpoint] [text-frame]")
    {
        auto editor = PowerPointDocumentEditor::CreateNew();
        auto shape = editor->AddSlide()->ShapeTree()->AddShape("Text box");
        PresentationTextFrame expected;
        expected.LeftMargin = {1.0, ExyokiOffice::MeasurementUnit::Millimeter};
        expected.TopMargin = {0.05, ExyokiOffice::MeasurementUnit::Inch};
        expected.RightMargin = {6.0, ExyokiOffice::MeasurementUnit::Point};
        expected.BottomMargin = {100.0, ExyokiOffice::MeasurementUnit::Twip};
        expected.Vertical = Drawing::TextVerticalValues::Vertical270;
        expected.Anchor = Drawing::TextAnchoringTypeValues::Center;
        expected.Paragraphs = {
            {{{"Hello", "en-US", true, false, "https://example.test/report", "Open report"},
              {" world", "en-GB", false, true, std::nullopt, ""}},
             Drawing::TextAlignmentTypeValues::Center,
             PresentationTextBullet{std::string("•"), std::nullopt, 1},
             {{914400, Drawing::TextTabAlignmentValues::Left}, {1828800, Drawing::TextTabAlignmentValues::Decimal}}},
            {{{"Second", "cs-CZ", false, false, std::nullopt, ""}},
             Drawing::TextAlignmentTypeValues::Justified,
             PresentationTextBullet{std::nullopt, Drawing::TextAutoNumberSchemeValues::ArabicPeriod, 3},
             {}}};

        REQUIRE(shape->SetTextFrame(expected));
        CHECK(shape->GetTextFrame() == expected);
        auto reopened = PowerPointDocumentEditor::Open(editor->SaveToMemory());
        REQUIRE(reopened);
        CHECK(reopened->GetSlide(0)->ShapeTree()->Get(0)->GetTextFrame() == expected);
    }

    TEST_CASE("plain paragraphs explicitly disable bullets and support empty frames [unit] [powerpoint] [text-frame]")
    {
        auto editor = PowerPointDocumentEditor::CreateNew();
        auto shape = editor->AddSlide()->ShapeTree()->AddShape();
        PresentationTextFrame frame;
        frame.Paragraphs = {{{{"Plain", "", false, false, std::nullopt, ""}}}};
        REQUIRE(shape->SetTextFrame(frame));
        REQUIRE(shape->GetTextFrame());
        CHECK(shape->GetTextFrame()->Paragraphs == frame.Paragraphs);

        PresentationTextFrame empty;
        REQUIRE(shape->SetTextFrame(empty));
        REQUIRE(shape->GetTextFrame());
        CHECK(shape->GetTextFrame()->Paragraphs.size() == 1);
        CHECK(shape->GetTextFrame()->Paragraphs.front().Runs.empty());
    }

    TEST_CASE("replacing text removes obsolete hyperlink relationships [unit] [powerpoint] [text-frame]")
    {
        auto editor = PowerPointDocumentEditor::CreateNew();
        auto slide = editor->AddSlide();
        auto shape = slide->ShapeTree()->AddShape();
        PresentationTextFrame linked;
        linked.Paragraphs = {{{{"Link", "en-US", false, false, "https://example.test/", ""}}}};
        REQUIRE(shape->SetTextFrame(linked));
        CHECK(slide->GetPart()->Relationships().size() == 1);
        PresentationTextFrame plain;
        plain.Paragraphs = {{{{"Plain", "en-US", false, false, std::nullopt, ""}}}};
        REQUIRE(shape->SetTextFrame(plain));
        CHECK(slide->GetPart()->Relationships().empty());
    }

    TEST_CASE("invalid text frame input is rejected without destroying existing text [unit] [powerpoint] [text-frame]")
    {
        auto editor = PowerPointDocumentEditor::CreateNew();
        auto tree = editor->AddSlide()->ShapeTree();
        auto shape = tree->AddShape();
        PresentationTextFrame valid;
        valid.Paragraphs = {{{{"Kept", "", false, false, std::nullopt, ""}}}};
        REQUIRE(shape->SetTextFrame(valid));
        auto invalid = valid;
        invalid.LeftMargin = -1;
        CHECK_FALSE(shape->SetTextFrame(invalid));
        invalid = valid;
        invalid.Paragraphs.front().Bullet = PresentationTextBullet{std::string("•"),
                                                                   Drawing::TextAutoNumberSchemeValues::ArabicPeriod, 1};
        CHECK_FALSE(shape->SetTextFrame(invalid));
        invalid = valid;
        invalid.Paragraphs.front().Level = 9;
        CHECK_FALSE(shape->SetTextFrame(invalid));
        invalid = valid;
        invalid.Paragraphs.front().LineSpacing = PresentationTextSpacing{};
        CHECK_FALSE(shape->SetTextFrame(invalid));
        invalid = valid;
        invalid.Paragraphs.front().Runs.front().FontColor = ExyokiOffice::Color{};
        CHECK_FALSE(shape->SetTextFrame(invalid));
        invalid = valid;
        invalid.Paragraphs.front().Runs.front().FontSize =
            ExyokiOffice::MeasuringUnits(0.0, ExyokiOffice::MeasurementUnit::Point);
        CHECK_FALSE(shape->SetTextFrame(invalid));
        invalid = valid;
        invalid.Paragraphs.front().LineSpacing = PresentationTextSpacing{std::nullopt, 20116.9};
        CHECK_FALSE(shape->SetTextFrame(invalid));
        invalid = valid;
        invalid.Paragraphs.front().Bullet =
            PresentationTextBullet{std::nullopt, Drawing::TextAutoNumberSchemeValues::ArabicPeriod, 0};
        CHECK_FALSE(shape->SetTextFrame(invalid));
        invalid = valid;
        invalid.Paragraphs.front().Runs.front().CharacterSpacing =
            ExyokiOffice::MeasuringUnits(5000.0, ExyokiOffice::MeasurementUnit::Point);
        CHECK_FALSE(shape->SetTextFrame(invalid));
        CHECK(shape->GetTextFrame() == valid);
        auto connector = tree->AddConnector();
        CHECK_FALSE(connector->SetTextFrame(valid));
        CHECK_FALSE(connector->GetTextFrame().has_value());
    }

    TEST_CASE("authored text frame is accepted by the OpenXML validator [unit] [powerpoint] [text-frame]")
    {
        auto editor = PowerPointDocumentEditor::CreateNew();
        auto shape = editor->AddSlide()->ShapeTree()->AddShape();
        PresentationTextFrame frame;
        frame.Paragraphs = {{{{"Validated", "en-US", true, false, "https://example.test/", "Example"}},
                             Drawing::TextAlignmentTypeValues::Right,
                             PresentationTextBullet{std::string("–"), std::nullopt, 1},
                             {{914400, Drawing::TextTabAlignmentValues::Center}}}};
        REQUIRE(shape->SetTextFrame(frame));
        CHECK(ExyokiOffice::OpenXmlPackageValidator(ExyokiOffice::OpenXmlDomValidationSettings{}).Validate(*editor->GetDocument()).IsValid());
    }

    TEST_CASE("advanced text effects and 3-D text round trip [unit] [powerpoint] [text-frame]")
    {
        using ExyokiOffice::AngleUnit;
        using ExyokiOffice::Color;
        using ExyokiOffice::MeasurementUnit;
        using ExyokiOffice::MeasuringAngle;
        using ExyokiOffice::MeasuringUnits;

        auto editor = PowerPointDocumentEditor::CreateNew();
        auto shape = editor->AddSlide()->ShapeTree()->AddShape("Effects");
        PresentationTextRun run;
        run.Text = "Effects";
        run.Glow = PresentationTextGlow{MeasuringUnits(3.0, MeasurementUnit::Point), Color(10, 20, 30)};
        PresentationTextShadow shadow;
        shadow.BlurRadius = MeasuringUnits(2.0, MeasurementUnit::Point);
        shadow.Distance = MeasuringUnits(0.1, MeasurementUnit::Inch);
        shadow.ColorValue = Color(40, 50, 60);
        shadow.Direction = MeasuringAngle(45.0, AngleUnit::Degree);
        shadow.HorizontalSkew = MeasuringAngle(0.1, AngleUnit::Radian);
        shadow.Alignment = Drawing::RectangleAlignmentValues::TopLeft;
        shadow.RotateWithShape = false;
        run.Shadow = shadow;
        PresentationTextReflection reflection;
        reflection.BlurRadius = MeasuringUnits(1.0, MeasurementUnit::Millimeter);
        reflection.Distance = MeasuringUnits(40.0, MeasurementUnit::Twip);
        reflection.StartOpacity = 65000;
        reflection.EndPosition = 75000;
        reflection.Direction = MeasuringAngle(std::numbers::pi / 2.0, AngleUnit::Radian);
        reflection.FadeDirection = MeasuringAngle(90.0, AngleUnit::Degree);
        reflection.Alignment = Drawing::RectangleAlignmentValues::Bottom;
        run.Reflection = reflection;

        PresentationTextFrame expected;
        expected.Paragraphs = {{{run}}};
        PresentationText3D threeD;
        threeD.Depth = MeasuringUnits(4.0, MeasurementUnit::Point);
        threeD.ExtrusionHeight = MeasuringUnits(0.2, MeasurementUnit::Inch);
        threeD.ContourWidth = MeasuringUnits(1.0, MeasurementUnit::Millimeter);
        threeD.Material = Drawing::PresetMaterialTypeValues::Metal;
        threeD.Camera = Drawing::PresetCameraValues::PerspectiveFront;
        threeD.LightRig = Drawing::LightRigValues::Balanced;
        threeD.LightDirection = Drawing::LightRigDirectionValues::TopRight;
        threeD.TopBevel = PresentationTextBevel{MeasuringUnits(2.0, MeasurementUnit::Point),
                                                MeasuringUnits(1.0, MeasurementUnit::Point),
                                                Drawing::BevelPresetValues::RelaxedInset};
        threeD.BottomBevel = PresentationTextBevel{MeasuringUnits(1.0, MeasurementUnit::Point),
                                                   MeasuringUnits(0.5, MeasurementUnit::Point),
                                                   Drawing::BevelPresetValues::Circle};
        threeD.ExtrusionColor = Color(70, 80, 90);
        threeD.ContourColor = Color(100, 110, 120);
        expected.ThreeD = threeD;

        REQUIRE(shape->SetTextFrame(expected));
        CHECK(shape->GetTextFrame() == expected);
        const auto xml = editor->GetSlide(0)->GetPart()->GetXmlString();
        CHECK(xml.find("<a:effectLst>") != std::string::npos);
        CHECK(xml.find("<a:glow") != std::string::npos);
        CHECK(xml.find("<a:outerShdw") != std::string::npos);
        CHECK(xml.find("<a:reflection") != std::string::npos);
        CHECK(xml.find("<a:scene3d>") != std::string::npos);
        CHECK(xml.find("<a:sp3d") != std::string::npos);
        CHECK(xml.find("<a:bevelT") != std::string::npos);
        CHECK(xml.find("<a:bevelB") != std::string::npos);

        auto reopened = PowerPointDocumentEditor::Open(editor->SaveToMemory());
        REQUIRE(reopened);
        auto actual = reopened->GetSlide(0)->ShapeTree()->Get(0)->GetTextFrame();
        REQUIRE(actual);
        CHECK(*actual == expected);
        CHECK(ExyokiOffice::OpenXmlPackageValidator(ExyokiOffice::OpenXmlDomValidationSettings{}).Validate(*reopened->GetDocument()).IsValid());
    }

    TEST_CASE("invalid advanced text effects preserve existing text [unit] [powerpoint] [text-frame]")
    {
        using ExyokiOffice::AngleUnit;
        using ExyokiOffice::Color;
        using ExyokiOffice::MeasurementUnit;
        using ExyokiOffice::MeasuringAngle;
        using ExyokiOffice::MeasuringUnits;
        auto editor = PowerPointDocumentEditor::CreateNew();
        auto shape = editor->AddSlide()->ShapeTree()->AddShape();
        PresentationTextFrame valid;
        valid.Paragraphs = {{{{"Kept"}}}};
        REQUIRE(shape->SetTextFrame(valid));

        auto invalid = valid;
        invalid.Paragraphs.front().Runs.front().Glow =
            PresentationTextGlow{MeasuringUnits(-1.0, MeasurementUnit::Point), Color(1, 2, 3)};
        CHECK_FALSE(shape->SetTextFrame(invalid));
        invalid = valid;
        invalid.Paragraphs.front().Runs.front().Shadow = PresentationTextShadow{};
        CHECK_FALSE(shape->SetTextFrame(invalid));
        invalid = valid;
        PresentationTextShadow shadow;
        shadow.ColorValue = Color(1, 2, 3);
        shadow.Direction = MeasuringAngle(std::numeric_limits<ExyokiOffice::Real>::infinity(), AngleUnit::Radian);
        invalid.Paragraphs.front().Runs.front().Shadow = shadow;
        CHECK_FALSE(shape->SetTextFrame(invalid));
        invalid = valid;
        PresentationText3D threeD;
        threeD.Depth = MeasuringUnits(-1.0, MeasurementUnit::Point);
        invalid.ThreeD = threeD;
        CHECK_FALSE(shape->SetTextFrame(invalid));
        CHECK(shape->GetTextFrame() == valid);
    }
}
