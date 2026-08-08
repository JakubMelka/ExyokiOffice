// Copyright (c) 2026 Jakub Melka and Collaborators
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

// End-to-end tour of the high-level PowerPointDocumentEditor API. The deck
// built here uses slide size and document properties, a SlideBuilder title
// slide with a real title placeholder, formatted text frames, a picture, a
// chart, a DrawingML table, transitions and animations, speaker notes,
// comments, a section, and a custom show — and then re-opens the saved package
// to prove it round-trips.
//
// Everything below is public high-level API: no generated DOM element is
// created or edited here, only the enums that API takes as arguments.
#include "Common/SampleImage.hpp"
#include "ExyokiOffice/PowerPoint/PowerPointDocument.hpp"
#include "ExyokiOffice/StandardTypes.hpp"

#include <iostream>
#include <string>
#include <utility>
#include <vector>

using namespace ExyokiOffice::PowerPoint;
namespace Drawing = ExyokiOffice::DocumentFormat::OpenXml::Drawing;
namespace Presentation = ExyokiOffice::DocumentFormat::OpenXml::Presentation;
using ExyokiOffice::Color;
using ExyokiOffice::MeasurementUnit;
using ExyokiOffice::MeasuringUnits;

namespace ExamplePowerPoint
{

// The deck is laid out on the 13.333 x 7.5 inch widescreen slide set below.
constexpr ExyokiOffice::Real SlideWidthInches = 13.333;
constexpr ExyokiOffice::Real ContentLeftInches = 0.75;
constexpr ExyokiOffice::Real ContentWidthInches = SlideWidthInches - 2.0 * ContentLeftInches;

MeasuringUnits Inches(ExyokiOffice::Real value)
{
    return MeasuringUnits(value, MeasurementUnit::Inch);
}

PresentationShapeTransform Frame(ExyokiOffice::Real left, ExyokiOffice::Real top, ExyokiOffice::Real width, ExyokiOffice::Real height)
{
    return PresentationShapeTransform{.Position = {Inches(left), Inches(top)},
                                      .Size = {Inches(width), Inches(height)}};
}

PresentationShapeTransform HeadingFrame()
{
    return Frame(ContentLeftInches, 0.4, ContentWidthInches, 0.9);
}

PresentationShapeTransform BodyFrame()
{
    return Frame(ContentLeftInches, 1.5, ContentWidthInches, 4.9);
}

// A single centered, colored heading run.
PresentationTextFrame HeadingText(std::string text)
{
    PresentationTextRun run;
    run.Text = std::move(text);
    run.Language = "en-US";
    run.Bold = true;
    run.FontSize = MeasuringUnits(28.0, MeasurementUnit::Point);
    run.FontColor = Color(0x1F, 0x4E, 0x79);

    PresentationTextParagraph paragraph;
    paragraph.Runs = {run};

    PresentationTextFrame frame;
    frame.Paragraphs = {paragraph};
    return frame;
}

// Bulleted body text; Level selects the DrawingML list level (0 is the first).
PresentationTextParagraph Bullet(std::string text, ExyokiOffice::Int32 level, ExyokiOffice::Real fontSizePoints)
{
    PresentationTextRun run;
    run.Text = std::move(text);
    run.Language = "en-US";
    run.FontSize = MeasuringUnits(fontSizePoints, MeasurementUnit::Point);

    PresentationTextParagraph paragraph;
    paragraph.Runs = {run};
    paragraph.Level = level;
    paragraph.Bullet = PresentationTextBullet{.Character = std::string("\xE2\x80\xA2")};
    paragraph.SpaceAfter = PresentationTextSpacing{.Points = MeasuringUnits(6.0, MeasurementUnit::Point)};
    return paragraph;
}

// Returns the shape wrapper hosting a placeholder. Placeholder wrappers carry
// the `p:ph` semantics, shape wrappers carry geometry and text, and both are
// views over the same `p:sp` node — so the two are matched by node identity.
PresentationShape::Ptr FindShape(const PresentationSlide::Ptr& slide,
                                 const PresentationPlaceholder::Ptr& placeholder)
{
    if (!slide || !placeholder || !slide->ShapeTree())
    {
        return nullptr;
    }
    const auto element = placeholder->GetElement();
    for (const auto& shape : slide->ShapeTree()->Shapes())
    {
        if (element && shape->GetElement() && shape->GetElement()->IsSameNode(element))
        {
            return shape;
        }
    }
    return nullptr;
}

// Adds a heading text box to a slide and returns it, so animations can target
// the shape by its non-visual ID.
PresentationShape::Ptr AddHeading(const PresentationSlide::Ptr& slide, std::string text)
{
    auto shape = slide->ShapeTree()->AddShape("Heading");
    if (shape)
    {
        shape->SetPresetGeometry(Drawing::ShapeTypeValues::Rectangle);
        shape->SetTransform(HeadingFrame());
        shape->SetTextFrame(HeadingText(std::move(text)));
    }
    return shape;
}

} // namespace ExamplePowerPoint

int main()
{
    using namespace ExamplePowerPoint;

    auto editor = PowerPointDocumentEditor::CreateNew();
    if (!editor)
    {
        std::cerr << "Failed to create the presentation." << std::endl;
        return 1;
    }

    // --- Presentation-level metadata. ---------------------------------------
    // A new presentation has no explicit slide size; setting one pins the deck
    // to a known aspect ratio instead of PowerPoint's own fallback.
    if (!editor->SetSlideSize(PresentationSlideSize::Widescreen16x9()))
    {
        std::cerr << "Failed to set the slide size." << std::endl;
        return 1;
    }

    auto properties = editor->Properties();
    properties.SetTitle("ExyokiOffice Editor Example");
    properties.SetCreator("ExyokiOffice Library");
    properties.SetSubject("High-level PowerPoint API demonstration");
    properties.SetKeywords("ExyokiOffice, OpenXML, PowerPoint, example");
    properties.SetCompany("ExyokiOffice");

    // --- The design: one slide master with two layouts. ----------------------
    // PresentationML requires every slide to reference a layout, and every
    // layout to belong to a master, so a deck needs this scaffolding before its
    // first slide. The master carries the default Office theme, which
    // SetThemeSettings() can replace.
    auto master = editor->AddSlideMaster("ExyokiOffice");
    auto titleLayout = editor->AddSlideLayout(master, "Title Slide", Presentation::SlideLayoutValues::Title);
    auto contentLayout =
        editor->AddSlideLayout(master, "Title and Content", Presentation::SlideLayoutValues::Object);
    if (!titleLayout || !contentLayout)
    {
        std::cerr << "Failed to create the slide master and its layouts." << std::endl;
        return 1;
    }

    // --- Slide 1: title, built with the fluent SlideBuilder. -----------------
    auto titleSlide = editor->CreateSlideBuilder()
                          .SetLayout(titleLayout)
                          .AddTextBox("Creating PowerPoint presentations with modern C++",
                                      Frame(ContentLeftInches, 3.9, ContentWidthInches, 0.9), "Subtitle")
                          .AddTextBox("No Microsoft Office installation. No COM. Just a ZIP of XML parts.",
                                      Frame(ContentLeftInches, 4.8, ContentWidthInches, 0.6), "Tagline")
                          .Build();
    if (!titleSlide)
    {
        std::cerr << "Failed to build the title slide." << std::endl;
        return 1;
    }

    // SlideBuilder::SetTitle() would be shorter, but it authors an ordinary text
    // box named "Title". PowerPoint's outline pane, "Reset Slide", and every
    // tool that reads slide titles look for a `p:ph` placeholder instead, so the
    // title is declared as a real one. AddPlaceholder returns a placeholder
    // wrapper rather than a shape wrapper, so the shape is matched back out of
    // the tree by node identity. Placeholders can also be declared on the master
    // and the layout and then overridden per slide; see docs/PowerPoint.md.
    auto titlePlaceholder = titleSlide->AddPlaceholder(Presentation::PlaceholderValues::Title, 1);
    auto titleShape = FindShape(titleSlide, titlePlaceholder);
    if (!titlePlaceholder || !titleShape)
    {
        std::cerr << "Failed to add the title placeholder." << std::endl;
        return 1;
    }
    titleShape->SetTransform(Frame(ContentLeftInches, 2.5, ContentWidthInches, 1.3));
    titleShape->SetTextFrame(HeadingText("ExyokiOffice"));
    titleSlide->SetNotesText("Welcome slide.\n"
                             "Introduce the library, then move on to the feature overview.");

    // Content slides are appended and assigned the shared content layout.
    const auto addContentSlide = [&]() -> PresentationSlide::Ptr
    {
        auto slide = editor->AddSlide();
        if (!slide || !editor->SetSlideLayout(editor->SlideCount() - 1, contentLayout))
        {
            return nullptr;
        }
        return slide;
    };

    // --- Slide 2: multi-level bulleted text with direct formatting. ----------
    auto featuresSlide = addContentSlide();
    if (!featuresSlide)
    {
        std::cerr << "Failed to add a content slide." << std::endl;
        return 1;
    }
    auto featuresHeading = AddHeading(featuresSlide, "What the library does");

    PresentationTextFrame bullets;
    bullets.Paragraphs = {
        Bullet("Direct ZIP/XML package writing, no Office dependency", 0, 20.0),
        Bullet("Word, Excel, and PowerPoint share one packaging layer", 1, 16.0),
        Bullet("Typed DrawingML shapes, text, pictures, tables, and charts", 0, 20.0),
        Bullet("Fills, outlines, glow, shadow, and reflection effects", 1, 16.0),
        Bullet("Slide masters, layouts, and placeholder inheritance", 0, 20.0),
        Bullet("Transitions, animation effects, speaker notes, and comments", 0, 20.0),
        Bullet("Lossless round-trip: unknown parts are preserved, not dropped", 0, 20.0),
    };
    auto featuresBody = featuresSlide->ShapeTree()->AddShape("Body");

    // A tinted panel behind the list, sent to the back so the text stays on top.
    auto panel = featuresSlide->ShapeTree()->AddShape("Panel");
    if (!featuresHeading || !featuresBody || !panel)
    {
        std::cerr << "Failed to add the shapes of the feature slide." << std::endl;
        return 1;
    }

    featuresBody->SetPresetGeometry(Drawing::ShapeTypeValues::Rectangle);
    featuresBody->SetTransform(BodyFrame());
    featuresBody->SetTextFrame(bullets);

    panel->SetPresetGeometry(Drawing::ShapeTypeValues::RoundRectangle, {{"adj", "val 6000"}});
    panel->SetTransform(Frame(ContentLeftInches - 0.2, 1.3, ContentWidthInches + 0.4, 5.3));
    panel->SetFill(PresentationShapeFill{.Kind = PresentationFillKind::Solid,
                                         .ColorValue = Color(0xF2, 0xF6, 0xFB)});
    panel->SetOutline(PresentationShapeOutline{.Fill = PresentationFillKind::Solid,
                                               .ColorValue = Color(0x1F, 0x4E, 0x79),
                                               .Width = MeasuringUnits(1.0, MeasurementUnit::Point)});

    // Reordering rebuilds the shape XML, which invalidates the wrappers created
    // above, so the animation targets are captured as plain IDs beforehand.
    const auto headingShapeId = featuresHeading->Id();
    const auto bodyShapeId = featuresBody->Id();
    featuresSlide->ShapeTree()->SendToBack(featuresSlide->ShapeTree()->Count() - 1);

    featuresSlide->SetTransition(PresentationTransitionData{.Kind = PresentationTransitionKind::Fade,
                                                            .Speed = PresentationTransitionSpeed::Medium,
                                                            .AdvanceOnClick = true});
    const auto headingAnimation =
        featuresSlide->AddAnimationEffect({.TargetShapeId = headingShapeId,
                                           .Class = PresentationAnimationEffectClass::Entrance,
                                           .Effect = PresentationAnimationEffect::Fly,
                                           .Direction = PresentationAnimationDirection::Left});
    const auto bodyAnimation =
        featuresSlide->AddAnimationEffect({.TargetShapeId = bodyShapeId,
                                           .Class = PresentationAnimationEffectClass::Entrance,
                                           .Effect = PresentationAnimationEffect::Fade,
                                           .Trigger = PresentationAnimationTrigger::AfterPrevious,
                                           .Timing = {.Delay = 200, .Duration = 600}});
    if (!headingAnimation || !bodyAnimation)
    {
        std::cerr << "Failed to add the slide animations." << std::endl;
        return 1;
    }
    featuresSlide->SetNotesText("Mention that every bullet maps to a documented API guide in docs/.");

    // --- Slide 3: a picture with alt text, a caption, and a hyperlink. -------
    auto pictureSlide = addContentSlide();
    if (!pictureSlide)
    {
        std::cerr << "Failed to add a content slide." << std::endl;
        return 1;
    }
    AddHeading(pictureSlide, "Pictures");

    PresentationPictureData picture;
    picture.Embedded = PresentationEmbeddedPicture{
        .Data = ExyokiOfficeExamples::BuildGradientBmp(480, 270), .ContentType = "image/bmp"};
    picture.Name = "Gradient";
    picture.AltText = "A generated blue-to-red gradient used as sample artwork.";
    picture.Transform = Frame(ContentLeftInches, 1.6, 6.0, 3.375);
    auto pictureShape = pictureSlide->ShapeTree()->AddPicture(picture);
    if (!pictureShape)
    {
        std::cerr << "Failed to insert the picture." << std::endl;
        return 1;
    }
    pictureShape->SetEffects(PresentationShapeEffects{
        .Shadow = PresentationTextShadow{.BlurRadius = MeasuringUnits(6.0, MeasurementUnit::Point),
                                         .Distance = MeasuringUnits(3.0, MeasurementUnit::Point),
                                         .ColorValue = Color(0x80, 0x80, 0x80)}});

    PresentationTextRun captionRun;
    captionRun.Text = "Image bytes are embedded as a package part; the format, pixel size, and DPI "
                      "are detected from the bytes themselves.";
    captionRun.Language = "en-US";
    captionRun.FontSize = MeasuringUnits(14.0, MeasurementUnit::Point);
    PresentationTextRun linkRun;
    linkRun.Text = " Read the guide.";
    linkRun.Language = "en-US";
    linkRun.FontSize = MeasuringUnits(14.0, MeasurementUnit::Point);
    linkRun.Hyperlink = "https://en.wikipedia.org/wiki/Office_Open_XML";
    linkRun.HyperlinkTooltip = "Office Open XML on Wikipedia";

    PresentationTextFrame caption;
    caption.Paragraphs = {PresentationTextParagraph{.Runs = {captionRun, linkRun}}};
    auto captionShape = pictureSlide->ShapeTree()->AddShape("Caption");
    if (!captionShape)
    {
        std::cerr << "Failed to add the caption shape." << std::endl;
        return 1;
    }
    captionShape->SetPresetGeometry(Drawing::ShapeTypeValues::Rectangle);
    captionShape->SetTransform(Frame(7.0, 1.6, 5.583, 3.375));
    captionShape->SetTextFrame(caption);

    // --- Slide 4: a chart rendered from literal cached values. ---------------
    auto chartSlide = addContentSlide();
    if (!chartSlide)
    {
        std::cerr << "Failed to add a content slide." << std::endl;
        return 1;
    }
    AddHeading(chartSlide, "Charts");

    PresentationChartDefinition chart;
    chart.Type = PresentationChartType::Column;
    chart.Title = "Documents produced per quarter";
    chart.CategoryAxisTitle = "Quarter";
    chart.ValueAxisTitle = "Thousands";
    chart.LegendPosition = PresentationChartLegendPosition::Bottom;
    chart.Transform = Frame(ContentLeftInches, 1.5, ContentWidthInches, 4.8);

    PresentationChartSeries wordSeries;
    wordSeries.Name = "Word";
    wordSeries.Values = {12.0, 18.5, 22.0, 27.5};
    wordSeries.Categories = std::vector<std::string>{"Q1", "Q2", "Q3", "Q4"};
    PresentationChartSeries excelSeries;
    excelSeries.Name = "Excel";
    excelSeries.Values = {9.0, 11.0, 16.5, 19.0};
    excelSeries.Categories = wordSeries.Categories;
    PresentationChartSeries presentationSeries;
    presentationSeries.Name = "PowerPoint";
    presentationSeries.Values = {4.0, 6.5, 8.0, 13.5};
    presentationSeries.Categories = wordSeries.Categories;
    chart.Series = {wordSeries, excelSeries, presentationSeries};

    if (!chartSlide->ShapeTree()->AddChart(chart))
    {
        std::cerr << "Failed to insert the chart." << std::endl;
        return 1;
    }
    chartSlide->SetTransition(PresentationTransitionData{
        .Kind = PresentationTransitionKind::Push,
        .Duration = 700,
        .Options = {.Direction = PresentationTransitionDirection::Left}});

    // --- Slide 5: a DrawingML table with a merged header cell. ---------------
    auto tableSlide = addContentSlide();
    if (!tableSlide)
    {
        std::cerr << "Failed to add a content slide." << std::endl;
        return 1;
    }
    AddHeading(tableSlide, "Roadmap");

    PresentationTableData table;
    table.ColumnWidths = {Inches(2.6), Inches(2.2), Inches(7.033)};
    table.Rows = {
        PresentationTableRow{.Height = Inches(0.45), .Cells = {{"Status by area"}, {""}, {""}}},
        PresentationTableRow{.Height = Inches(0.45), .Cells = {{"Area"}, {"Status"}, {"Notes"}}},
        PresentationTableRow{.Height = Inches(0.45),
                             .Cells = {{"Word"}, {"Stable"}, {"Paragraphs, tables, fields, notes, revisions"}}},
        PresentationTableRow{.Height = Inches(0.45),
                             .Cells = {{"Excel"}, {"Stable"}, {"Cells, formulas, tables, pivots, charts"}}},
        PresentationTableRow{.Height = Inches(0.45),
                             .Cells = {{"PowerPoint"}, {"Stable"}, {"Slides, shapes, media, tables, animations"}}},
    };
    table.Transform = Frame(ContentLeftInches, 1.5, ContentWidthInches, 2.25);
    auto tableShape = tableSlide->ShapeTree()->AddTable(table);
    if (!tableShape || !tableShape->MergeTableCells(0, 0, 1, 3))
    {
        std::cerr << "Failed to insert the roadmap table." << std::endl;
        return 1;
    }

    // --- Review metadata: an author, a comment, a section, a custom show. ----
    editor->AddCommentAuthor({.Id = "author-1", .Name = "Reviewer", .Initials = "RV"});
    tableSlide->AddComment({.Id = "comment-1",
                            .AuthorId = "author-1",
                            .Text = "Confirm the roadmap wording before the release.",
                            .Position = {Inches(1.0), Inches(1.0)}});

    editor->AddSection({.Id = "{1D9B4D3F-4C4E-4B2C-9E77-9E5F0C1B8A01}",
                        .Name = "Introduction",
                        .SlideIds = {titleSlide->Id(), featuresSlide->Id()}});
    editor->AddSection({.Id = "{1D9B4D3F-4C4E-4B2C-9E77-9E5F0C1B8A02}",
                        .Name = "Feature tour",
                        .SlideIds = {pictureSlide->Id(), chartSlide->Id(), tableSlide->Id()}});
    editor->AddCustomShow(
        {.Id = 1, .Name = "Executive summary", .SlideIds = {titleSlide->Id(), chartSlide->Id()}});

    // --- Save. ---------------------------------------------------------------
    const char* fileName = "Presentation.pptx";
    if (!editor->SaveToFile(fileName))
    {
        std::cerr << "Failed to save " << fileName << "." << std::endl;
        return 1;
    }

    // --- Re-open what was just written, so the example also proves the
    //     presentation round-trips through the library. ----------------------
    auto reopened = PowerPointDocumentEditor::Open(fileName);
    if (!reopened)
    {
        std::cerr << "Failed to re-open " << fileName << "." << std::endl;
        return 1;
    }

    const auto reopenedSize = reopened->GetSlideSize();
    const auto reopenedSlides = reopened->Slides();
    if (!reopenedSize || reopenedSlides.size() != 5)
    {
        std::cerr << "The presentation did not survive the round trip." << std::endl;
        return 1;
    }

    ExyokiOffice::Size shapeCount = 0;
    for (const auto& slide : reopenedSlides)
    {
        shapeCount += slide->ShapeTree()->Count();
    }

    std::cout << "Wrote " << fileName << ": " << reopenedSlides.size() << " slides, " << shapeCount
              << " shapes, " << reopened->Sections().size() << " sections, "
              << reopened->CustomShows().size() << " custom show(s), "
              << reopenedSlides[1]->AnimationEffects().size() << " animation effects on slide 2, "
              << reopenedSlides[4]->Comments().size() << " comment(s) on slide 5, "
              << reopenedSlides[0]->Placeholders(false).size() << " placeholder(s) on slide 1.\n"
              << "Slide size: " << reopenedSize->Size.Width.ToUnit(MeasurementUnit::Inch).GetValue() << " x "
              << reopenedSize->Size.Height.ToUnit(MeasurementUnit::Inch).GetValue() << " inches."
              << std::endl;
    return 0;
}
