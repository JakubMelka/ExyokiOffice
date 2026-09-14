// Copyright (c) 2026 Jakub Melka and Contributors
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

// Library-level regressions for the PowerPoint defects the Office COM run
// found (MCP_ERRORS.md, P-1, P-3, P-7, P-8, P-9): what the library writes has
// to be what PowerPoint draws and reads back.

#include "doctest.h"

#include "ExyokiOffice/DOM/DocumentFormat/OpenXml/Drawing.hpp"
#include "ExyokiOffice/DOM/DocumentFormat/OpenXml/Presentation.hpp"
#include "ExyokiOffice/PowerPoint/PowerPointDocument.hpp"
#include "ExyokiOffice/StandardTypes.hpp"
#include "ExyokiOffice/Tools/ValidationRunner.hpp"

#include <string>
#include <vector>

using namespace ExyokiOffice;
using namespace ExyokiOffice::PowerPoint;

namespace Drawing = ExyokiOffice::DocumentFormat::OpenXml::Drawing;
namespace Presentation = ExyokiOffice::DocumentFormat::OpenXml::Presentation;

class PowerPointOfficeFidelityTestHelpers
{
public:
    static Size Occurrences(const std::string& text, const std::string& needle)
    {
        Size count = 0;
        for (auto position = text.find(needle); position != std::string::npos;
             position = text.find(needle, position + needle.size()))
        {
            ++count;
        }
        return count;
    }

    static std::string Xml(const PresentationSlide::Ptr& slide)
    {
        return slide->GetPart()->GetXmlString();
    }

    /// Adds `count` plain shapes and returns their non-visual identifiers.
    static std::vector<UInt32> Shapes(const PresentationSlide::Ptr& slide, Size count)
    {
        std::vector<UInt32> result;
        auto tree = slide->ShapeTree();
        for (Size index = 0; index < count; ++index)
        {
            auto shape = tree->AddShape("Shape " + std::to_string(index));
            result.push_back(shape ? shape->Id() : 0);
        }
        return result;
    }

    static PresentationAnimationEffectData Fade(UInt32 shapeId)
    {
        PresentationAnimationEffectData data;
        data.TargetShapeId = shapeId;
        data.Class = PresentationAnimationEffectClass::Entrance;
        data.Effect = PresentationAnimationEffect::Fade;
        return data;
    }

    /// The shape wrapper of a placeholder, matched by node identity.
    static PresentationShape::Ptr ShapeOf(const PresentationSlide::Ptr& slide,
                                          const PresentationPlaceholder::Ptr& placeholder)
    {
        for (const auto& shape : slide->ShapeTree()->Shapes())
        {
            if (shape->GetElement()->IsSameNode(placeholder->GetElement()))
            {
                return shape;
            }
        }
        return nullptr;
    }

    static Real WidthOf(const std::optional<PresentationShapeTransform>& transform)
    {
        return transform ? transform->Size.Width.ToEmu().GetValue() : -1.0;
    }

    static std::vector<std::string> SectionNames(const PowerPointDocumentEditor& editor)
    {
        std::vector<std::string> names;
        for (const auto& section : editor.Sections())
        {
            names.push_back(section.Name);
        }
        return names;
    }

    static std::vector<Size> SectionSizes(const PowerPointDocumentEditor& editor)
    {
        std::vector<Size> sizes;
        for (const auto& section : editor.Sections())
        {
            sizes.push_back(section.SlideIds.size());
        }
        return sizes;
    }
};

using Helpers = PowerPointOfficeFidelityTestHelpers;

TEST_SUITE("PowerPointOfficeFidelityTests")
{
    TEST_CASE("P-1: removing the last effect removes p:timing and spares the transition [unit] [powerpoint] [animation-effect]")
    {
        auto editor = PowerPointDocumentEditor::CreateNew();
        auto slide = editor->AddSlide();
        REQUIRE(slide);
        const auto shapes = Helpers::Shapes(slide, 2);
        PresentationTransitionData transition;
        transition.Kind = PresentationTransitionKind::Fade;
        REQUIRE(slide->SetTransition(transition));

        const auto first = slide->AddAnimationEffect(Helpers::Fade(shapes[0]));
        const auto second = slide->AddAnimationEffect(Helpers::Fade(shapes[1]));
        REQUIRE(first);
        REQUIRE(second);
        REQUIRE(Helpers::Occurrences(Helpers::Xml(slide), "<p:timing") == 1);

        // Removing one of two keeps the tree; removing the last one drops it,
        // because an empty p:tnLst is what PowerPoint refuses to open.
        REQUIRE(slide->RemoveAnimationEffect(*first));
        CHECK(Helpers::Occurrences(Helpers::Xml(slide), "<p:timing") == 1);
        REQUIRE(slide->RemoveAnimationEffect(*second));
        CHECK(Helpers::Occurrences(Helpers::Xml(slide), "<p:timing") == 0);
        CHECK(Helpers::Occurrences(Helpers::Xml(slide), "<p:tnLst") == 0);
        CHECK(Helpers::Occurrences(Helpers::Xml(slide), "<p:transition") == 1);
        CHECK(slide->AnimationEffects().empty());

        // ClearAnimationEffects on a slide with effects behaves the same way.
        REQUIRE(slide->AddAnimationEffect(Helpers::Fade(shapes[0])));
        REQUIRE(slide->ClearAnimationEffects());
        CHECK(Helpers::Occurrences(Helpers::Xml(slide), "<p:timing") == 0);
        CHECK(Helpers::Occurrences(Helpers::Xml(slide), "<p:transition") == 1);

        // And an effect added afterwards rebuilds the tree from scratch.
        REQUIRE(slide->AddAnimationEffect(Helpers::Fade(shapes[1])));
        CHECK(Helpers::Occurrences(Helpers::Xml(slide), "<p:timing") == 1);
        CHECK(slide->AnimationEffects().size() == 1);
    }

    TEST_CASE("P-3: the default design positions master, layout, slide and notes placeholders [unit] [powerpoint] [master-layout]")
    {
        auto editor = PowerPointDocumentEditor::CreateNew();
        auto layout = editor->EnsureDefaultLayout();
        REQUIRE(layout);
        // A second call reuses the design instead of adding another master.
        REQUIRE(editor->EnsureDefaultLayout());
        CHECK(editor->SlideMasters().size() == 1);
        CHECK(editor->SlideLayouts().size() == 1);

        // The master carries the five Office placeholders, each with geometry,
        // and the text styles a slide inherits its formatting from.
        auto master = layout->Master();
        REQUIRE(master);
        CHECK(master->Placeholders().size() == 5);
        const auto masterXml = master->GetPart()->GetXmlString();
        CHECK(Helpers::Occurrences(masterXml, "<a:prstGeom prst=\"rect\">") == 5);
        CHECK(Helpers::Occurrences(masterXml, "<p:txStyles>") == 1);
        CHECK(Helpers::Occurrences(masterXml, "<p:bg>") == 1);
        CHECK(Helpers::Occurrences(masterXml, "<a:lvl9pPr") == 2);

        // The layout placeholders inherit the master geometry explicitly.
        CHECK(layout->Placeholders(false).size() == 5);
        CHECK(Helpers::Occurrences(layout->GetPart()->GetXmlString(), "<a:prstGeom prst=\"rect\">") == 5);
        REQUIRE(layout->FindPlaceholder(Presentation::PlaceholderValues::Title));
        REQUIRE(layout->FindPlaceholder(Presentation::PlaceholderValues::Object));
        CHECK(layout->FindPlaceholder(Presentation::PlaceholderValues::Header) == nullptr);

        // A slide placeholder without its own a:xfrm resolves through the layout.
        auto slide = editor->AddSlide(editor->CreateSlideBuilder().SetLayout(layout));
        REQUIRE(slide);
        auto title = slide->AddPlaceholder(Presentation::PlaceholderValues::Title);
        auto body = slide->AddPlaceholder(Presentation::PlaceholderValues::Body);
        REQUIRE(title);
        REQUIRE(body);
        auto titleShape = Helpers::ShapeOf(slide, title);
        auto bodyShape = Helpers::ShapeOf(slide, body);
        REQUIRE(titleShape);
        REQUIRE(bodyShape);
        CHECK(Helpers::WidthOf(titleShape->GetTransform()) == 0.0);
        CHECK(Helpers::WidthOf(titleShape->GetEffectiveTransform()) == 8229600.0);
        CHECK(titleShape->GetEffectiveTransform()->Position.Y.ToEmu().GetValue() == 274638.0);
        CHECK(Helpers::WidthOf(bodyShape->GetEffectiveTransform()) == 8229600.0);
        CHECK(bodyShape->GetEffectiveTransform()->Size.Height.ToEmu().GetValue() == 4525963.0);

        // A content placeholder on a layout that only has a body draws in the body area.
        auto quote = editor->AddSlideLayout(master, "Quote", Presentation::SlideLayoutValues::TitleOnly);
        REQUIRE(quote);
        auto quoteTitle = quote->AddPlaceholder(Presentation::PlaceholderValues::CenteredTitle);
        REQUIRE(quoteTitle);
        auto quoteProperties = quoteTitle->GetShape()->GetFirstChildOfType<Presentation::ShapeProperties>();
        REQUIRE(quoteProperties);
        CHECK(quoteProperties->GetFirstChildOfType<Drawing::Transform2D>() != nullptr);
        auto second = editor->AddSlide(editor->CreateSlideBuilder().SetLayout(quote));
        REQUIRE(second);
        auto chart = second->AddPlaceholder(Presentation::PlaceholderValues::Chart);
        REQUIRE(chart);
        CHECK(Helpers::WidthOf(Helpers::ShapeOf(second, chart)->GetEffectiveTransform()) == 8229600.0);

        // An explicit transform always wins over inheritance.
        PresentationShapeTransform own;
        own.Position = PresentationPoint(Int64{100}, Int64{200});
        own.Size = PresentationSize(Int64{300}, Int64{400});
        REQUIRE(titleShape->SetTransform(own));
        CHECK(Helpers::WidthOf(titleShape->GetEffectiveTransform()) == 300.0);

        // The notes page and its master both position their placeholders.
        REQUIRE(slide->SetNotesText("Visible notes"));
        auto notesRoot = slide->GetPart()->GetNotesSlidePart()->GetTypedRootElement();
        REQUIRE(notesRoot);
        CHECK(notesRoot->Descendants<Presentation::PlaceholderShape>().size() == 2);
        for (const auto& shape : notesRoot->Descendants<Presentation::Shape>())
        {
            const auto extents = shape->Descendants<Drawing::Extents>();
            REQUIRE(extents.size() == 1);
            CHECK(extents.front()->GetCx().ValueOr(0) > 0);
            CHECK(extents.front()->GetCy().ValueOr(0) > 0);
        }
        CHECK(slide->NotesText() == "Visible notes");
        auto notesMaster = editor->GetDocument()->GetPresentationPart()->GetNotesMasterPart();
        REQUIRE(notesMaster);
        CHECK(notesMaster->GetTypedRootElement()->Descendants<Presentation::PlaceholderShape>().size() == 6);

        // A widescreen deck uses PowerPoint's 16:9 table instead of scaling.
        auto wide = PowerPointDocumentEditor::CreateNew();
        REQUIRE(wide->SetSlideSize(PresentationSlideSize::Widescreen16x9()));
        auto wideMaster = wide->AddSlideMaster("Wide");
        REQUIRE(wideMaster);
        CHECK(Helpers::Occurrences(wideMaster->GetPart()->GetXmlString(), "cx=\"10515600\"") == 2);

        auto reopened = PowerPointDocumentEditor::Open(editor->SaveToMemory());
        REQUIRE(reopened);
        CHECK(reopened->SlideMasters().front()->Placeholders().size() == 5);
        auto package = reopened->GetDocument();
        REQUIRE(package);
        const auto report = Tools::Run(*package);
        CHECK(report.ErrorCount == 0);
    }

    TEST_CASE("P-7: AddSectionAt splits the containing section and starts at slide one [unit] [powerpoint] [section-custom-show]")
    {
        auto editor = PowerPointDocumentEditor::CreateNew();
        std::vector<UInt32> ids;
        for (int index = 0; index < 4; ++index)
        {
            auto slide = editor->AddSlide();
            REQUIRE(slide);
            ids.push_back(slide->Id());
        }

        CHECK_FALSE(editor->AddSectionAt(4, "Beyond"));
        CHECK_FALSE(editor->AddSectionAt(0, ""));

        const auto x = editor->AddSectionAt(0, "X", "{X}");
        REQUIRE(x);
        CHECK(x->SlideIds == ids);
        const auto y = editor->AddSectionAt(2, "Y");
        REQUIRE(y);
        CHECK(y->SlideIds == std::vector<UInt32>{ids[2], ids[3]});
        CHECK_FALSE(y->Id.empty());
        CHECK(Helpers::SectionNames(*editor) == std::vector<std::string>{"X", "Y"});
        CHECK(Helpers::SectionSizes(*editor) == std::vector<Size>{2, 2});
        CHECK_FALSE(editor->AddSectionAt(1, "Duplicate", "{X}"));

        // Splitting again inside X leaves X with its first slide.
        const auto z = editor->AddSectionAt(1, "Z");
        REQUIRE(z);
        CHECK(Helpers::SectionNames(*editor) == std::vector<std::string>{"X", "Z", "Y"});
        CHECK(Helpers::SectionSizes(*editor) == std::vector<Size>{1, 1, 2});

        // Starting the first section later than slide 1 creates the default section PowerPoint shows.
        auto fresh = PowerPointDocumentEditor::CreateNew();
        for (int index = 0; index < 4; ++index)
        {
            REQUIRE(fresh->AddSlide());
        }
        const auto late = fresh->AddSectionAt(2, "Y");
        REQUIRE(late);
        CHECK(late->SlideIds.size() == 2);
        CHECK(Helpers::SectionNames(*fresh) == std::vector<std::string>{"Default Section", "Y"});
        CHECK(Helpers::SectionSizes(*fresh) == std::vector<Size>{2, 2});

        auto reopened = PowerPointDocumentEditor::Open(fresh->SaveToMemory());
        REQUIRE(reopened);
        CHECK(Helpers::SectionNames(*reopened) == std::vector<std::string>{"Default Section", "Y"});
    }

    TEST_CASE("P-8: ChangeFillColor uses PowerPoint's Fill Color preset and still reads old files [unit] [powerpoint] [animation-effect]")
    {
        auto editor = PowerPointDocumentEditor::CreateNew();
        auto slide = editor->AddSlide();
        REQUIRE(slide);
        const auto shapes = Helpers::Shapes(slide, 1);
        PresentationAnimationEffectData recolor;
        recolor.TargetShapeId = shapes[0];
        recolor.Class = PresentationAnimationEffectClass::Emphasis;
        recolor.Effect = PresentationAnimationEffect::ChangeFillColor;
        recolor.Color = "FF8800";
        REQUIRE(slide->AddAnimationEffect(recolor));

        auto xml = Helpers::Xml(slide);
        CHECK(Helpers::Occurrences(xml, "presetID=\"1\" presetClass=\"emph\" presetSubtype=\"2\"") == 1);
        CHECK(Helpers::Occurrences(xml, "presetID=\"2\"") == 0);
        CHECK(Helpers::Occurrences(xml, "<p:attrName>fill.type</p:attrName>") == 1);
        CHECK(Helpers::Occurrences(xml, "<p:attrName>fill.on</p:attrName>") == 1);
        auto effects = slide->AnimationEffects();
        REQUIRE(effects.size() == 1);
        CHECK(effects[0].Effect == PresentationAnimationEffect::ChangeFillColor);
        CHECK(effects[0].Color == "FF8800");

        // A file written by an earlier version carries preset 2; the fillcolor
        // behavior makes it unambiguous, so it is still read as this effect.
        const std::string current = "presetID=\"1\" presetClass=\"emph\" presetSubtype=\"2\"";
        const std::string legacy = "presetID=\"2\" presetClass=\"emph\" presetSubtype=\"0\"";
        xml.replace(xml.find(current), current.size(), legacy);
        slide->GetPart()->SetXmlString(xml);
        effects = slide->AnimationEffects();
        REQUIRE(effects.size() == 1);
        CHECK(effects[0].Effect == PresentationAnimationEffect::ChangeFillColor);

        // Without a fill-colour behavior preset 2 is PowerPoint's Change Font,
        // which this version does not model.
        const std::string attribute = "<p:attrName>fillcolor</p:attrName>";
        xml.replace(xml.find(attribute), attribute.size(), "<p:attrName>style.color</p:attrName>");
        slide->GetPart()->SetXmlString(xml);
        effects = slide->AnimationEffects();
        REQUIRE(effects.size() == 1);
        CHECK(effects[0].Effect == PresentationAnimationEffect::Unsupported);
    }

    TEST_CASE("P-9: an interactive sequence carries the trigger conditions PowerPoint reads [unit] [powerpoint] [animation-effect]")
    {
        auto editor = PowerPointDocumentEditor::CreateNew();
        auto slide = editor->AddSlide();
        REQUIRE(slide);
        const auto shapes = Helpers::Shapes(slide, 2);
        PresentationAnimationEffectData fly;
        fly.TargetShapeId = shapes[0];
        fly.TriggerShapeId = shapes[1];
        fly.Class = PresentationAnimationEffectClass::Entrance;
        fly.Effect = PresentationAnimationEffect::Fly;
        fly.Direction = PresentationAnimationDirection::Left;
        REQUIRE(slide->AddAnimationEffect(fly));

        // The serialized XML is indented, so every token is checked on its own.
        const auto xml = Helpers::Xml(slide);
        const std::string button = std::to_string(shapes[1]);
        CHECK(Helpers::Occurrences(xml, "nodeType=\"interactiveSeq\"") == 1);
        CHECK(Helpers::Occurrences(xml, "<p:nextCondLst>") == 1);
        // Once in the start condition and once in the trailing next condition.
        CHECK(Helpers::Occurrences(xml, "<p:cond evt=\"onClick\" delay=\"0\">") == 2);
        CHECK(Helpers::Occurrences(xml, "<p:spTgt spid=\"" + button + "\"") == 2);
        CHECK(Helpers::Occurrences(xml, "<p:endSync evt=\"end\" delay=\"0\">") == 1);
        CHECK(Helpers::Occurrences(xml, "<p:rtn val=\"all\"") == 1);
        CHECK(Helpers::Occurrences(xml, "delay=\"indefinite\"") == 0);

        const auto effects = slide->AnimationEffects();
        REQUIRE(effects.size() == 1);
        CHECK(effects[0].TriggerShapeId == shapes[1]);
        CHECK(effects[0].TargetShapeId == shapes[0]);

        // The main sequence keeps its click groups waiting for the click.
        REQUIRE(slide->AddAnimationEffect(Helpers::Fade(shapes[1])));
        const auto both = Helpers::Xml(slide);
        CHECK(Helpers::Occurrences(both, "delay=\"indefinite\"") == 1);
        CHECK(Helpers::Occurrences(both, "<p:endSync") == 1);
        CHECK(Helpers::Occurrences(both, "<p:prevCondLst>") == 1);
        CHECK(slide->AnimationEffects().size() == 2);
    }
}
