// Copyright (c) 2026 Jakub Melka and Collaborators
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

// One test case per row of the PowerPoint table in docs/Compatibility.md,
// graded the same way the matrix grades it: Create through
// PowerPointDocumentEditor, Edit after reopening the saved presentation,
// Preserve across an open-save cycle.
//
// PresentationML requires a slide to reference a layout, and a layout to belong
// to a master, so every case starts from the same scaffolding rather than from a
// bare presentation.

#include "doctest.h"

#include "TestSupport.hpp"

#include "ExyokiOffice/DOM/DocumentFormat/OpenXml/Drawing.hpp"
#include "ExyokiOffice/DOM/DocumentFormat/OpenXml/Presentation.hpp"
#include "ExyokiOffice/PowerPoint/PowerPointDocument.hpp"
#include "ExyokiOffice/StandardTypes.hpp"

#include <string>
#include <vector>

namespace
{

using namespace ExyokiOffice::PowerPoint;
using ExyokiOffice::Byte;
using ExyokiOffice::Int64;
using ExyokiOffice::UInt32;
using ExyokiOfficeTests::CheckPreservation;
using ExyokiOfficeTests::RoundTrip;
using ExyokiOfficeTests::ValidatePackage;

namespace Drawing = ExyokiOffice::DocumentFormat::OpenXml::Drawing;
namespace Presentation = ExyokiOffice::DocumentFormat::OpenXml::Presentation;

/// A 1x1 transparent PNG.
std::vector<Byte> MinimalPng()
{
    return {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A, 0x00, 0x00, 0x00, 0x0D, 0x49, 0x48, 0x44, 0x52,
            0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x08, 0x06, 0x00, 0x00, 0x00, 0x1F, 0x15, 0xC4,
            0x89, 0x00, 0x00, 0x00, 0x0D, 0x49, 0x44, 0x41, 0x54, 0x08, 0xD7, 0x63, 0xF8, 0xCF, 0xC0, 0xF0,
            0x1F, 0x00, 0x05, 0x00, 0x01, 0xFF, 0x89, 0x99, 0x3D, 0x1D, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45,
            0x4E, 0x44, 0xAE, 0x42, 0x60, 0x82};
}

/// The scaffolding every PresentationML document needs before it can hold a
/// slide: one master, one layout, and a slide bound to that layout.
struct Deck
{
    PowerPointDocumentEditor::Ptr Editor;
    PresentationSlideMaster::Ptr Master;
    PresentationSlideLayout::Ptr Layout;
    PresentationSlide::Ptr Slide;
};

Deck MakeDeck()
{
    Deck deck;
    deck.Editor = PowerPointDocumentEditor::CreateNew();
    REQUIRE(deck.Editor != nullptr);

    deck.Master = deck.Editor->AddSlideMaster("Compat");
    REQUIRE(deck.Master != nullptr);
    deck.Layout = deck.Editor->AddSlideLayout(deck.Master, "Content", Presentation::SlideLayoutValues::Object);
    REQUIRE(deck.Layout != nullptr);

    deck.Slide = deck.Editor->AddSlide();
    REQUIRE(deck.Slide != nullptr);
    REQUIRE(deck.Editor->SetSlideLayout(0, deck.Layout));

    return deck;
}

PresentationShapeTransform Frame(Int64 x, Int64 y, Int64 width, Int64 height)
{
    return PresentationShapeTransform{.Position = {x, y}, .Size = {width, height}};
}

/// Saves, checks the package validates, and reports that the bytes round-trip
/// unchanged. Every row below ends with this.
void CheckSavesValidatesAndPreserves(const PowerPointDocumentEditor::Ptr& editor)
{
    REQUIRE(editor != nullptr);
    const auto bytes = editor->SaveToMemory();
    REQUIRE_FALSE(bytes.empty());

    const auto validation = ValidatePackage(bytes);
    CAPTURE(validation.FirstError);
    CHECK_FALSE(validation.HasErrors);

    const auto preservation = CheckPreservation(bytes);
    REQUIRE(preservation.Ok);
    for (const auto& difference : preservation.Differences)
    {
        CAPTURE(difference);
        CHECK_MESSAGE(false, "package changed through open-save");
    }
    CHECK(preservation.Preserved);
}

} // namespace

TEST_SUITE("PowerPointMatrixTests")
{

    TEST_CASE("Presentations: create, edit, preserve [compat] [powerpoint] [ppt-presentations]")
    {
        auto deck = MakeDeck();
        deck.Editor->Properties().SetTitle("Presentation matrix");
        REQUIRE(deck.Editor->SetSlideSize(PresentationSlideSize::Widescreen16x9()));
        REQUIRE(deck.Editor->ProtectFromModification("secret").Succeeded());

        auto reopened = RoundTrip(deck.Editor);
        REQUIRE(reopened != nullptr);
        CHECK(reopened->Properties().GetTitle() == "Presentation matrix");
        const auto size = reopened->GetSlideSize();
        REQUIRE(size.has_value());
        CHECK(size->Size.Width == PresentationSlideSize::Widescreen16x9().Size.Width);
        CHECK(reopened->GetModifyProtection().has_value());

        // Edit: change the slide size and lift the modify protection.
        REQUIRE(reopened->SetSlideSize(PresentationSlideSize::Standard4x3()));
        CHECK_FALSE(reopened->UnprotectFromModification("wrong").Succeeded());
        CHECK(reopened->UnprotectFromModification("secret").Succeeded());

        auto edited = RoundTrip(reopened);
        REQUIRE(edited != nullptr);
        REQUIRE(edited->GetSlideSize().has_value());
        CHECK(edited->GetSlideSize()->Size.Width == PresentationSlideSize::Standard4x3().Size.Width);
        CHECK_FALSE(edited->GetModifyProtection().has_value());

        CheckSavesValidatesAndPreserves(edited);
    }

    TEST_CASE("Slides: create, edit, preserve [compat] [powerpoint] [ppt-slides]")
    {
        auto deck = MakeDeck();

        auto built = deck.Editor->CreateSlideBuilder()
                         .SetLayout(deck.Layout)
                         .AddTextBox("Built by the slide builder", Frame(914400, 914400, 5486400, 1143000), "Body")
                         .Build();
        REQUIRE(built != nullptr);
        REQUIRE(deck.Editor->AddSlide() != nullptr);
        CHECK(deck.Editor->SlideCount() == 3);

        auto reopened = RoundTrip(deck.Editor);
        REQUIRE(reopened != nullptr);
        CHECK(reopened->SlideCount() == 3);

        // Edit: copy, reorder and remove.
        REQUIRE(reopened->CopySlide(1) != nullptr);
        CHECK(reopened->SlideCount() == 4);
        CHECK(reopened->MoveSlide(3, 0));
        CHECK(reopened->RemoveSlide(0));

        auto edited = RoundTrip(reopened);
        REQUIRE(edited != nullptr);
        CHECK(edited->SlideCount() == 3);

        CheckSavesValidatesAndPreserves(edited);
    }

    TEST_CASE("Sections and custom shows: create, edit, preserve [compat] [powerpoint] [ppt-sections]")
    {
        auto deck = MakeDeck();
        REQUIRE(deck.Editor->AddSlide() != nullptr);
        REQUIRE(deck.Editor->SetSlideLayout(1, deck.Layout));

        const auto firstId = deck.Editor->GetSlide(0)->Id();
        const auto secondId = deck.Editor->GetSlide(1)->Id();

        REQUIRE(deck.Editor->AddSection(
            PresentationSection{.Id = "{11111111-1111-1111-1111-111111111111}", .Name = "Opening", .SlideIds = {firstId}}));
        REQUIRE(deck.Editor->AddCustomShow(
            PresentationCustomShow{.Id = 1, .Name = "Short", .SlideIds = {firstId, secondId}}));

        auto reopened = RoundTrip(deck.Editor);
        REQUIRE(reopened != nullptr);
        REQUIRE(reopened->Sections().size() == 1);
        CHECK(reopened->Sections().front().Name == "Opening");
        REQUIRE(reopened->CustomShows().size() == 1);
        CHECK(reopened->CustomShows().front().SlideIds.size() == 2);

        // Edit: rename the section, shorten the custom show.
        auto section = reopened->Sections().front();
        section.Name = "Renamed";
        CHECK(reopened->UpdateSection(section.Id, section));

        auto show = reopened->CustomShows().front();
        show.SlideIds = {firstId};
        CHECK(reopened->UpdateCustomShow(show.Id, show));

        auto edited = RoundTrip(reopened);
        REQUIRE(edited != nullptr);
        REQUIRE(edited->Sections().size() == 1);
        CHECK(edited->Sections().front().Name == "Renamed");
        REQUIRE(edited->CustomShows().size() == 1);
        CHECK(edited->CustomShows().front().SlideIds.size() == 1);

        CheckSavesValidatesAndPreserves(edited);
    }

    TEST_CASE("Masters and layouts: created and assigned, but they expose no shape tree "
              "[compat] [powerpoint] [ppt-masters]")
    {
        auto deck = MakeDeck();
        REQUIRE(deck.Master->AddPlaceholder(Presentation::PlaceholderValues::Title, 1) != nullptr);
        REQUIRE(deck.Layout->AddPlaceholder(Presentation::PlaceholderValues::Body, 2) != nullptr);

        auto second = deck.Editor->AddSlideLayout(deck.Master, "Blank", Presentation::SlideLayoutValues::Blank);
        REQUIRE(second != nullptr);

        auto reopened = RoundTrip(deck.Editor);
        REQUIRE(reopened != nullptr);
        REQUIRE_FALSE(reopened->SlideMasters().empty());

        auto master = reopened->SlideMasters().front();
        REQUIRE(master != nullptr);
        CHECK(master->Name() == "Compat");
        CHECK(master->Layouts().size() == 2);
        CHECK_FALSE(master->Placeholders().empty());

        // Edit is graded Partial for exactly one reason: the master and the
        // layout model placeholders but not their own shape trees, so anything
        // else about them has to go through the DOM.
        CHECK(master->GetPart() != nullptr);
        CHECK(master->Layouts().front()->GetPart() != nullptr);

        // Assigning a different layout to a slide is modelled.
        CHECK(reopened->SetSlideLayout(0, master->Layouts().back()));

        auto edited = RoundTrip(reopened);
        REQUIRE(edited != nullptr);
        REQUIRE_FALSE(edited->SlideMasters().empty());
        CHECK(edited->SlideMasters().front()->Layouts().size() == 2);

        CheckSavesValidatesAndPreserves(edited);
    }

    TEST_CASE("Shapes: create, edit, preserve [compat] [powerpoint] [ppt-shapes]")
    {
        auto deck = MakeDeck();
        auto tree = deck.Slide->ShapeTree();
        REQUIRE(tree != nullptr);

        auto shape = tree->AddShape("Rectangle");
        REQUIRE(shape != nullptr);
        REQUIRE(shape->SetPresetGeometry(Drawing::ShapeTypeValues::Rectangle));
        REQUIRE(shape->SetTransform(Frame(914400, 914400, 2743200, 1371600)));

        PresentationShapeFill fill;
        fill.Kind = PresentationFillKind::Solid;
        fill.ColorValue = ExyokiOffice::Color(0x44, 0x72, 0xC4);
        REQUIRE(shape->SetFill(fill));

        REQUIRE(tree->AddConnector("Connector") != nullptr);

        auto reopened = RoundTrip(deck.Editor);
        REQUIRE(reopened != nullptr);
        auto readBack = reopened->GetSlide(0)->ShapeTree();
        REQUIRE(readBack != nullptr);
        CHECK(readBack->Count() == 2);

        auto readShape = readBack->Get(0);
        REQUIRE(readShape != nullptr);
        const auto transform = readShape->GetTransform();
        REQUIRE(transform.has_value());
        CHECK(transform->Size.Width == 2743200);
        const auto readFill = readShape->GetFill();
        REQUIRE(readFill.has_value());
        CHECK(readFill->ColorValue == ExyokiOffice::Color(0x44, 0x72, 0xC4));

        // Edit: move the shape and reorder the tree.
        CHECK(readShape->SetTransform(Frame(0, 0, 1828800, 914400)));
        CHECK(readBack->SendToBack(0));

        auto edited = RoundTrip(reopened);
        REQUIRE(edited != nullptr);
        auto editedTree = edited->GetSlide(0)->ShapeTree();
        REQUIRE(editedTree != nullptr);
        CHECK(editedTree->Count() == 2);

        CheckSavesValidatesAndPreserves(edited);
    }

    TEST_CASE("Text: create, edit, preserve [compat] [powerpoint] [ppt-text]")
    {
        auto deck = MakeDeck();
        auto tree = deck.Slide->ShapeTree();
        REQUIRE(tree != nullptr);

        auto shape = tree->AddShape("TextBox");
        REQUIRE(shape != nullptr);
        REQUIRE(shape->SetPresetGeometry(Drawing::ShapeTypeValues::Rectangle));
        REQUIRE(shape->SetTransform(Frame(914400, 914400, 5486400, 1828800)));

        PresentationTextFrame frame;
        PresentationTextParagraph paragraph;
        paragraph.Runs.push_back(PresentationTextRun{.Text = "Bold heading", .Bold = true});
        PresentationTextParagraph bullet;
        bullet.Level = 1;
        bullet.Runs.push_back(PresentationTextRun{.Text = "A bullet"});
        frame.Paragraphs = {paragraph, bullet};
        REQUIRE(shape->SetTextFrame(frame));

        auto reopened = RoundTrip(deck.Editor);
        REQUIRE(reopened != nullptr);
        auto readShape = reopened->GetSlide(0)->ShapeTree()->Get(0);
        REQUIRE(readShape != nullptr);

        auto readFrame = readShape->GetTextFrame();
        REQUIRE(readFrame.has_value());
        REQUIRE(readFrame->Paragraphs.size() == 2);
        REQUIRE_FALSE(readFrame->Paragraphs.front().Runs.empty());
        CHECK(readFrame->Paragraphs.front().Runs.front().Text == "Bold heading");
        // The Notes column names the exact run formatting that is modelled.
        CHECK(readFrame->Paragraphs.front().Runs.front().Bold);
        CHECK(readFrame->Paragraphs.back().Level == 1);

        // Edit: rewrite the text frame.
        auto editedFrame = *readFrame;
        editedFrame.Paragraphs.front().Runs.front().Text = "Edited heading";
        CHECK(readShape->SetTextFrame(editedFrame));

        auto edited = RoundTrip(reopened);
        REQUIRE(edited != nullptr);
        auto editedRead = edited->GetSlide(0)->ShapeTree()->Get(0)->GetTextFrame();
        REQUIRE(editedRead.has_value());
        REQUIRE_FALSE(editedRead->Paragraphs.empty());
        REQUIRE_FALSE(editedRead->Paragraphs.front().Runs.empty());
        CHECK(editedRead->Paragraphs.front().Runs.front().Text == "Edited heading");

        CheckSavesValidatesAndPreserves(edited);
    }

    TEST_CASE("Pictures and media: create, edit, preserve; linked media stays unresolved "
              "[compat] [powerpoint] [ppt-media]")
    {
        auto deck = MakeDeck();
        auto tree = deck.Slide->ShapeTree();
        REQUIRE(tree != nullptr);

        PresentationPictureData picture;
        picture.Embedded = PresentationEmbeddedPicture{.Data = MinimalPng(), .ContentType = "image/png"};
        picture.Name = "Logo";
        picture.AltText = "A one pixel image";
        picture.Transform = Frame(914400, 914400, 914400, 914400);
        REQUIRE(tree->AddPicture(picture) != nullptr);

        PresentationMediaData media;
        media.Kind = PresentationMediaKind::Audio;
        media.Embedded = PresentationEmbeddedMedia{.Data = {0x01, 0x02, 0x03, 0x04}, .ContentType = "audio/mpeg"};
        media.Name = "Narration";
        media.Transform = Frame(2743200, 914400, 914400, 914400);
        REQUIRE(tree->AddMedia(media) != nullptr);

        // Linked media is preserved but never resolved: the matrix says the
        // library neither downloads nor plays it.
        PresentationMediaData linked;
        linked.Kind = PresentationMediaKind::Video;
        linked.LinkedUri = "https://example.com/clip.mp4";
        linked.Name = "Linked clip";
        linked.Transform = Frame(4572000, 914400, 914400, 914400);
        REQUIRE(tree->AddMedia(linked) != nullptr);

        auto reopened = RoundTrip(deck.Editor);
        REQUIRE(reopened != nullptr);
        auto readBack = reopened->GetSlide(0)->ShapeTree();
        REQUIRE(readBack != nullptr);
        CHECK(readBack->Count() == 3);

        auto readPicture = readBack->Get(0)->GetPicture();
        REQUIRE(readPicture.has_value());
        REQUIRE(readPicture->Embedded.has_value());
        CHECK(readPicture->Embedded->Data == MinimalPng());
        CHECK(readPicture->AltText == "A one pixel image");

        auto readLinked = readBack->Get(2)->GetMedia();
        REQUIRE(readLinked.has_value());
        REQUIRE(readLinked->LinkedUri.has_value());
        CHECK(*readLinked->LinkedUri == "https://example.com/clip.mp4");
        CHECK_FALSE(readLinked->Embedded.has_value());

        // Edit: replace the picture payload.
        CHECK(readBack->Get(0)->ReplacePictureFromData(MinimalPng()));

        auto edited = RoundTrip(reopened);
        REQUIRE(edited != nullptr);
        CHECK(edited->GetSlide(0)->ShapeTree()->Count() == 3);

        CheckSavesValidatesAndPreserves(edited);
    }

    TEST_CASE("DrawingML tables: create, edit, preserve [compat] [powerpoint] [ppt-tables]")
    {
        auto deck = MakeDeck();
        auto tree = deck.Slide->ShapeTree();
        REQUIRE(tree != nullptr);

        PresentationTableData table;
        table.ColumnWidths = {1000000, 2000000, 3000000};
        table.Rows = {{500000, {{"Header"}, {"Q1"}, {"Q2"}}},
                      {600000, {{"Revenue"}, {"10"}, {"20"}}},
                      {700000, {{"Profit"}, {"3"}, {"7"}}}};
        table.Transform = Frame(914400, 914400, 6000000, 1800000);

        auto shape = tree->AddTable(table);
        REQUIRE(shape != nullptr);
        CHECK(shape->MergeTableCells(0, 1, 1, 2));

        auto reopened = RoundTrip(deck.Editor);
        REQUIRE(reopened != nullptr);
        auto readShape = reopened->GetSlide(0)->ShapeTree()->Get(0);
        REQUIRE(readShape != nullptr);

        auto readTable = readShape->GetTable();
        REQUIRE(readTable.has_value());
        CHECK(readTable->ColumnWidths.size() == 3);
        CHECK(readTable->Rows.size() == 3);

        // Edit: rows and columns can be inserted and removed.
        CHECK(readShape->InsertTableRow(1));
        CHECK(readShape->RemoveTableColumn(2));

        auto edited = RoundTrip(reopened);
        REQUIRE(edited != nullptr);
        auto editedTable = edited->GetSlide(0)->ShapeTree()->Get(0)->GetTable();
        REQUIRE(editedTable.has_value());
        CHECK(editedTable->Rows.size() == 4);
        CHECK(editedTable->ColumnWidths.size() == 2);

        CheckSavesValidatesAndPreserves(edited);
    }

    TEST_CASE("Charts: create, edit, preserve [compat] [powerpoint] [ppt-charts]")
    {
        auto deck = MakeDeck();
        auto tree = deck.Slide->ShapeTree();
        REQUIRE(tree != nullptr);

        PresentationChartDefinition chart;
        chart.Type = PresentationChartType::Column;
        chart.Title = "Quarterly revenue";
        chart.Transform = Frame(914400, 914400, 5486400, 3200400);
        PresentationChartSeries series;
        series.Name = "2026";
        series.Values = {12.0, 18.5, 9.0, 21.0};
        series.Categories = std::vector<std::string>{"Q1", "Q2", "Q3", "Q4"};
        chart.Series = {series};

        auto shape = tree->AddChart(chart);
        REQUIRE(shape != nullptr);

        auto reopened = RoundTrip(deck.Editor);
        REQUIRE(reopened != nullptr);
        auto readShape = reopened->GetSlide(0)->ShapeTree()->Get(0);
        REQUIRE(readShape != nullptr);

        auto info = readShape->GetChart();
        REQUIRE(info.has_value());
        CHECK(info->Title == "Quarterly revenue");
        REQUIRE(info->Series.size() == 1);
        CHECK(info->Series.front().Name == "2026");

        // Edit: replace the cached series data.
        PresentationChartSeries replacement;
        replacement.Name = "2027";
        replacement.Values = {1.0, 2.0, 3.0, 4.0};
        replacement.Categories = std::vector<std::string>{"Q1", "Q2", "Q3", "Q4"};
        CHECK(readShape->UpdateChartData({replacement}, std::string("Edited title")));

        auto edited = RoundTrip(reopened);
        REQUIRE(edited != nullptr);
        auto editedInfo = edited->GetSlide(0)->ShapeTree()->Get(0)->GetChart();
        REQUIRE(editedInfo.has_value());
        CHECK(editedInfo->Title == "Edited title");
        REQUIRE(editedInfo->Series.size() == 1);
        CHECK(editedInfo->Series.front().Name == "2027");

        CheckSavesValidatesAndPreserves(edited);
    }

    TEST_CASE("Transitions: create, edit, preserve [compat] [powerpoint] [ppt-transitions]")
    {
        auto deck = MakeDeck();

        PresentationTransitionData transition;
        transition.Kind = PresentationTransitionKind::Wipe;
        transition.Speed = PresentationTransitionSpeed::Fast;
        transition.AdvanceOnClick = true;
        REQUIRE(deck.Slide->SetTransition(transition));

        auto reopened = RoundTrip(deck.Editor);
        REQUIRE(reopened != nullptr);
        auto readBack = reopened->GetSlide(0)->GetTransition();
        REQUIRE(readBack.has_value());
        CHECK(readBack->Kind == PresentationTransitionKind::Wipe);
        CHECK(readBack->Speed == PresentationTransitionSpeed::Fast);

        // Edit: change the effect, then remove it entirely.
        auto changed = *readBack;
        changed.Kind = PresentationTransitionKind::Fade;
        CHECK(reopened->GetSlide(0)->SetTransition(changed));

        auto edited = RoundTrip(reopened);
        REQUIRE(edited != nullptr);
        REQUIRE(edited->GetSlide(0)->GetTransition().has_value());
        CHECK(edited->GetSlide(0)->GetTransition()->Kind == PresentationTransitionKind::Fade);
        CHECK(edited->GetSlide(0)->RemoveTransition());
        CHECK_FALSE(edited->GetSlide(0)->GetTransition().has_value());

        CheckSavesValidatesAndPreserves(edited);
    }

    TEST_CASE("Animations: create, edit, preserve [compat] [powerpoint] [ppt-animations]")
    {
        auto deck = MakeDeck();
        auto tree = deck.Slide->ShapeTree();
        REQUIRE(tree != nullptr);

        auto first = tree->AddShape("First");
        REQUIRE(first != nullptr);
        REQUIRE(first->SetPresetGeometry(Drawing::ShapeTypeValues::Rectangle));
        REQUIRE(first->SetTransform(Frame(0, 0, 914400, 914400)));
        auto second = tree->AddShape("Second");
        REQUIRE(second != nullptr);
        REQUIRE(second->SetPresetGeometry(Drawing::ShapeTypeValues::Ellipse));
        REQUIRE(second->SetTransform(Frame(1828800, 0, 914400, 914400)));

        // Shape wrappers are invalidated by tree reordering, so the animation
        // targets are captured as ids up front.
        const auto firstId = first->Id();
        const auto secondId = second->Id();

        REQUIRE(deck.Slide->AddAnimationEffect(PresentationAnimationEffectData{
            .TargetShapeId = firstId, .Effect = PresentationAnimationEffect::Fade}));
        // Fly is one of the effects that requires a direction.
        REQUIRE(deck.Slide->AddAnimationEffect(
            PresentationAnimationEffectData{.TargetShapeId = secondId,
                                            .Effect = PresentationAnimationEffect::Fly,
                                            .Direction = PresentationAnimationDirection::Left}));

        auto reopened = RoundTrip(deck.Editor);
        REQUIRE(reopened != nullptr);
        auto effects = reopened->GetSlide(0)->AnimationEffects();
        REQUIRE(effects.size() == 2);
        CHECK(effects.front().Effect == PresentationAnimationEffect::Fade);

        // Edit: reorder and remove.
        CHECK(reopened->GetSlide(0)->MoveAnimationEffect(effects.back().Id, 0));
        CHECK(reopened->GetSlide(0)->RemoveAnimationEffect(effects.front().Id));

        auto edited = RoundTrip(reopened);
        REQUIRE(edited != nullptr);
        auto editedEffects = edited->GetSlide(0)->AnimationEffects();
        REQUIRE(editedEffects.size() == 1);
        CHECK(editedEffects.front().Effect == PresentationAnimationEffect::Fly);

        CheckSavesValidatesAndPreserves(edited);
    }

    TEST_CASE("Notes and comments: create, edit, preserve [compat] [powerpoint] [ppt-notes]")
    {
        auto deck = MakeDeck();
        REQUIRE(deck.Slide->SetNotesText("Speaker notes"));

        REQUIRE(deck.Editor->AddCommentAuthor(
            PresentationCommentAuthor{.Id = "1", .Name = "Reviewer", .Initials = "R"}));
        REQUIRE(deck.Slide->AddComment(
            PresentationComment{.Id = "c1", .AuthorId = "1", .Text = "Please revisit this slide"}));

        auto reopened = RoundTrip(deck.Editor);
        REQUIRE(reopened != nullptr);
        CHECK(reopened->GetSlide(0)->NotesText() == "Speaker notes");
        REQUIRE(reopened->CommentAuthors().size() == 1);
        CHECK(reopened->CommentAuthors().front().Name == "Reviewer");
        REQUIRE(reopened->GetSlide(0)->Comments().size() == 1);
        CHECK(reopened->GetSlide(0)->Comments().front().Text == "Please revisit this slide");

        // Edit: change the notes, then resolve the comment thread.
        CHECK(reopened->GetSlide(0)->SetNotesText("Edited notes"));
        CHECK(reopened->GetSlide(0)->SetCommentStatus("c1", PresentationCommentStatus::Resolved));

        auto edited = RoundTrip(reopened);
        REQUIRE(edited != nullptr);
        CHECK(edited->GetSlide(0)->NotesText() == "Edited notes");
        REQUIRE(edited->GetSlide(0)->Comments().size() == 1);
        CHECK(edited->GetSlide(0)->Comments().front().Status == PresentationCommentStatus::Resolved);

        CheckSavesValidatesAndPreserves(edited);
    }

    TEST_CASE("SmartArt and OLE objects are round-tripped untouched "
              "[compat] [powerpoint] [ppt-preserved]")
    {
        auto deck = MakeDeck();
        auto tree = deck.Slide->ShapeTree();
        REQUIRE(tree != nullptr);
        REQUIRE(tree->AddShape("Carrier") != nullptr);

        // Neither construct has a typed API at any level above the DOM, so the
        // row is exercised as content outside the typed model that another
        // producer wrote. BLD-004 will replace this with a file fixture.
        auto slidePart = deck.Slide->GetPart();
        REQUIRE(slidePart != nullptr);

        auto xml = slidePart->GetXmlString();
        const auto treeEnd = xml.find("</p:spTree>");
        REQUIRE(treeEnd != std::string::npos);
        xml.insert(treeEnd, R"(<vnd:oleStandIn xmlns:vnd="http://example.com/vendor" progId="Package"/>)");
        slidePart->SetXmlString(xml);

        const auto bytes = deck.Editor->SaveToMemory();
        REQUIRE_FALSE(bytes.empty());

        auto reopened = PowerPointDocumentEditor::Open(bytes);
        REQUIRE(reopened != nullptr);
        const auto reopenedXml = reopened->GetSlide(0)->GetPart()->GetXmlString();
        CHECK(reopenedXml.find("oleStandIn") != std::string::npos);
        CHECK(reopenedXml.find("http://example.com/vendor") != std::string::npos);

        const auto preservation = CheckPreservation(bytes);
        REQUIRE(preservation.Ok);
        for (const auto& difference : preservation.Differences)
        {
            CAPTURE(difference);
            CHECK_MESSAGE(false, "package changed through open-save");
        }
        CHECK(preservation.Preserved);
    }

} // TEST_SUITE("PowerPointMatrixTests")
