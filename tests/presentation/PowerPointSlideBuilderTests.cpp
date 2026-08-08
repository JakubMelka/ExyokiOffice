// Copyright (c) 2026 Jakub Melka and Collaborators
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "doctest.h"

#include "ExyokiOffice/OpenXmlPackageValidator.hpp"
#include "ExyokiOffice/PowerPoint/PowerPointDocument.hpp"

using namespace ExyokiOffice::PowerPoint;
namespace Drawing = ExyokiOffice::DocumentFormat::OpenXml::Drawing;
namespace Presentation = ExyokiOffice::DocumentFormat::OpenXml::Presentation;

TEST_SUITE("PowerPointSlideBuilderTests")
{
    TEST_CASE("builder accepts a complete rich text frame [unit] [powerpoint] [slide-builder]")
    {
        PresentationTextRun run;
        run.Text = "Styled";
        run.Bold = true;
        run.FontSize = ExyokiOffice::MeasuringUnits(20.0, ExyokiOffice::MeasurementUnit::Point);
        run.FontColor = ExyokiOffice::Color(ExyokiOffice::ColorPreset::Blue);
        PresentationTextFrame frame;
        frame.Paragraphs = {{std::vector<PresentationTextRun>{run}}};

        auto editor = PowerPointDocumentEditor::CreateNew();
        auto slide = editor->CreateSlideBuilder().AddTextBox(frame, {}, "Rich text").Build();
        REQUIRE(slide);
        REQUIRE(slide->ShapeTree()->Count() == 1);
        REQUIRE(slide->ShapeTree()->Get(0)->GetTextFrame());
        CHECK(slide->ShapeTree()->Get(0)->GetTextFrame() == frame);
    }

    TEST_CASE("builder authors layout title text boxes shapes and hidden state [unit] [powerpoint] [slide-builder]")
    {
        auto editor = PowerPointDocumentEditor::CreateNew();
        REQUIRE(editor);
        auto master = editor->AddSlideMaster("Corporate");
        auto layout = editor->AddSlideLayout(master, "Title and content", Presentation::SlideLayoutValues::Object);
        REQUIRE(layout);

        const PresentationShapeTransform titleTransform{{100000, 200000}, {7000000, 600000}};
        const PresentationShapeTransform bodyTransform{{100000, 1000000}, {7000000, 3000000}};
        const PresentationShapeTransform shapeTransform{{7500000, 1000000}, {1000000, 1000000}};
        auto builder = editor->CreateSlideBuilder();
        builder.SetLayout(layout)
            .SetHidden()
            .SetTitle("Status", titleTransform)
            .AddTextBox("First line\nSecond line", bodyTransform, "Body")
            .AddShape(Drawing::ShapeTypeValues::Ellipse, shapeTransform, "Indicator");

        auto slide = builder.Build();
        REQUIRE(slide);
        CHECK(slide->IsHidden());
        REQUIRE(slide->Layout());
        CHECK(slide->Layout()->Name() == "Title and content");
        auto shapes = slide->ShapeTree()->Shapes();
        REQUIRE(shapes.size() == 3);
        REQUIRE(shapes[0]->GetTextFrame());
        CHECK(shapes[0]->GetTextFrame()->Paragraphs[0].Runs[0].Text == "Status");
        CHECK(shapes[0]->GetTransform() == titleTransform);
        REQUIRE(shapes[1]->GetTextFrame());
        REQUIRE(shapes[1]->GetTextFrame()->Paragraphs.size() == 2);
        CHECK(shapes[1]->GetTextFrame()->Paragraphs[1].Runs[0].Text == "Second line");
        CHECK(shapes[2]->GetPresetGeometry() == Drawing::ShapeTypeValues::Ellipse);
        CHECK(shapes[2]->GetTransform() == shapeTransform);
        CHECK(ExyokiOffice::OpenXmlPackageValidator().Validate(*editor->GetDocument()).IsValid());

        auto reopened = PowerPointDocumentEditor::Open(editor->SaveToMemory());
        REQUIRE(reopened);
        REQUIRE(reopened->SlideCount() == 1);
        CHECK(reopened->GetSlide(0)->IsHidden());
        CHECK(reopened->GetSlide(0)->Layout()->Name() == "Title and content");
        CHECK(reopened->GetSlide(0)->ShapeTree()->Count() == 3);
    }

    TEST_CASE("builder is reusable and content clearing retains title configuration [unit] [powerpoint] [slide-builder]")
    {
        auto editor = PowerPointDocumentEditor::CreateNew();
        auto builder = editor->CreateSlideBuilder();
        builder.SetTitle("Reusable").AddTextBox("Removed", {}).AddShape(Drawing::ShapeTypeValues::Rectangle, {});
        builder.ClearContent();
        REQUIRE(builder.Build());
        REQUIRE(builder.Build());
        CHECK(editor->SlideCount() == 2);
        CHECK(editor->GetSlide(0)->ShapeTree()->Count() == 1);
        CHECK(editor->GetSlide(1)->ShapeTree()->Count() == 1);
        builder.ClearTitle();
        REQUIRE(builder.Build());
        CHECK(editor->GetSlide(2)->ShapeTree()->Count() == 0);
    }

    TEST_CASE("builder validation rolls back the allocated slide [unit] [powerpoint] [slide-builder]")
    {
        auto editor = PowerPointDocumentEditor::CreateNew();
        auto foreign = PowerPointDocumentEditor::CreateNew();
        auto foreignMaster = foreign->AddSlideMaster("Foreign");
        auto foreignLayout = foreign->AddSlideLayout(foreignMaster, "Foreign");
        REQUIRE(foreignLayout);

        auto foreignBuilder = editor->CreateSlideBuilder();
        foreignBuilder.SetLayout(foreignLayout);
        CHECK(editor->AddSlide(foreignBuilder) == nullptr);
        CHECK(editor->SlideCount() == 0);

        auto invalidShape = editor->CreateSlideBuilder();
        invalidShape.AddShape(Drawing::ShapeTypeValues::InvalidEnumValue, {});
        CHECK(invalidShape.Build() == nullptr);
        CHECK(editor->SlideCount() == 0);
        CHECK(editor->GetDocument()->GetPresentationPart()->GetSlideParts().empty());
    }

    TEST_CASE("complete reorder accepts only permutations and survives round trip [unit] [powerpoint] [slides]")
    {
        auto editor = PowerPointDocumentEditor::CreateNew();
        const auto first = editor->AddSlide()->Id();
        const auto second = editor->AddSlide()->Id();
        const auto third = editor->AddSlide()->Id();
        CHECK_FALSE(editor->ReorderSlides({0, 0, 2}));
        CHECK_FALSE(editor->ReorderSlides({0, 1}));
        CHECK(editor->ReorderSlides({2, 0, 1}));
        CHECK(editor->GetSlide(0)->Id() == third);
        CHECK(editor->GetSlide(1)->Id() == first);
        CHECK(editor->GetSlide(2)->Id() == second);

        auto reopened = PowerPointDocumentEditor::Open(editor->SaveToMemory());
        REQUIRE(reopened);
        CHECK(reopened->GetSlide(0)->Id() == third);
        CHECK(reopened->GetSlide(1)->Id() == first);
        CHECK(reopened->GetSlide(2)->Id() == second);
    }
} // namespace
