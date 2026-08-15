// Copyright (c) 2026 Jakub Melka and Contributors
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "doctest.h"

#include "ExyokiOffice/DOM/DocumentFormat/OpenXml/Presentation.hpp"
#include "ExyokiOffice/OpenXmlPackageValidator.hpp"
#include "ExyokiOffice/PowerPoint/PowerPointDocument.hpp"

using ExyokiOffice::DocumentFormat::OpenXml::Presentation::PlaceholderValues;
using ExyokiOffice::DocumentFormat::OpenXml::Presentation::SlideLayoutValues;
using ExyokiOffice::PowerPoint::PlaceholderOrigin;
using ExyokiOffice::PowerPoint::PowerPointDocumentEditor;

TEST_SUITE("PowerPointMasterLayoutTests")
{
    TEST_CASE("masters and layouts preserve XML order IDs metadata and relationships [unit] [powerpoint] [masters-layouts]")
    {
        auto editor = PowerPointDocumentEditor::CreateNew();
        REQUIRE(editor != nullptr);
        auto corporate = editor->AddSlideMaster("Corporate");
        auto alternate = editor->AddSlideMaster("Alternate");
        REQUIRE(corporate != nullptr);
        REQUIRE(alternate != nullptr);
        CHECK(corporate->Id() >= 0x80000000u);
        CHECK(alternate->Id() == corporate->Id() + 1u);
        CHECK(corporate->Name() == "Corporate");

        auto title = editor->AddSlideLayout(corporate, "Title", SlideLayoutValues::Title);
        auto content = editor->AddSlideLayout(corporate, "Title and content", SlideLayoutValues::Object);
        auto blank = editor->AddSlideLayout(alternate, "Blank", SlideLayoutValues::Blank);
        REQUIRE(title != nullptr);
        REQUIRE(content != nullptr);
        REQUIRE(blank != nullptr);
        // PresentationML rejects a layout id below 2 147 483 648, and ids are
        // allocated per master.
        CHECK(title->Id() == 0x80000000u);
        CHECK(content->Id() == 0x80000001u);
        CHECK(blank->Id() == 0x80000000u);
        CHECK(title->Name() == "Title");
        CHECK(title->Type() == SlideLayoutValues::Title);
        CHECK(title->Master()->Id() == corporate->Id());

        auto masters = editor->SlideMasters();
        auto layouts = editor->SlideLayouts();
        REQUIRE(masters.size() == 2);
        REQUIRE(layouts.size() == 3);
        CHECK(masters[0]->Name() == "Corporate");
        CHECK(masters[1]->Name() == "Alternate");
        CHECK(layouts[0]->Name() == "Title");
        CHECK(layouts[1]->Name() == "Title and content");
        CHECK(layouts[2]->Name() == "Blank");
        CHECK(corporate->Layouts().size() == 2);
        CHECK(alternate->Layouts().size() == 1);
        CHECK(title->GetPart()->GetSlideMasterPart() == corporate->GetPart());
        const auto validation = ExyokiOffice::OpenXmlPackageValidator().Validate(*editor->GetDocument());
        CHECK(validation.IsValid());
        CHECK(validation.Issues().empty());

        auto reopened = PowerPointDocumentEditor::Open(editor->SaveToMemory());
        REQUIRE(reopened != nullptr);
        REQUIRE(reopened->SlideMasters().size() == 2);
        REQUIRE(reopened->SlideLayouts().size() == 3);
        CHECK(reopened->GetSlideMaster(0)->Name() == "Corporate");
        CHECK(reopened->SlideLayouts()[1]->Name() == "Title and content");
        CHECK(reopened->SlideLayouts()[1]->Type() == SlideLayoutValues::Object);
        CHECK(reopened->SlideLayouts()[1]->Master()->Id() == reopened->GetSlideMaster(0)->Id());
    }

    TEST_CASE("placeholder inheritance uses index then type and supports removal [unit] [powerpoint] [masters-layouts]")
    {
        auto editor = PowerPointDocumentEditor::CreateNew();
        REQUIRE(editor != nullptr);
        auto master = editor->AddSlideMaster("Master");
        auto layout = editor->AddSlideLayout(master, "Content", SlideLayoutValues::Object);
        auto slide = editor->AddSlide();
        REQUIRE(master != nullptr);
        REQUIRE(layout != nullptr);
        REQUIRE(slide != nullptr);
        REQUIRE(editor->SetSlideLayout(0, layout));

        REQUIRE(master->AddPlaceholder(PlaceholderValues::Title, 1) != nullptr);
        REQUIRE(master->AddPlaceholder(PlaceholderValues::Footer) != nullptr);
        REQUIRE(layout->AddPlaceholder(PlaceholderValues::CenteredTitle, 1) != nullptr);
        REQUIRE(layout->AddPlaceholder(PlaceholderValues::Body, 2) != nullptr);
        auto slideBody = slide->AddPlaceholder(PlaceholderValues::Object, 2);
        REQUIRE(slideBody != nullptr);

        auto effective = slide->Placeholders();
        REQUIRE(effective.size() == 3);
        CHECK(effective[0]->Origin() == PlaceholderOrigin::Slide);
        CHECK(effective[0]->Index() == 2u);
        CHECK(effective[1]->Origin() == PlaceholderOrigin::Layout);
        CHECK(effective[1]->Index() == 1u);
        CHECK(effective[1]->Type() == PlaceholderValues::CenteredTitle);
        CHECK(effective[2]->Origin() == PlaceholderOrigin::Master);
        CHECK(effective[2]->Type() == PlaceholderValues::Footer);

        REQUIRE(slideBody->Remove());
        effective = slide->Placeholders();
        REQUIRE(effective.size() == 3);
        CHECK(effective[0]->Origin() == PlaceholderOrigin::Layout);
        CHECK(effective[0]->Index() == 1u);
        CHECK(effective[1]->Origin() == PlaceholderOrigin::Layout);
        CHECK(effective[1]->Index() == 2u);
        CHECK_FALSE(slideBody->Remove());
        CHECK(slide->Placeholders(false).empty());

        auto reopened = PowerPointDocumentEditor::Open(editor->SaveToMemory());
        REQUIRE(reopened != nullptr);
        auto reopenedPlaceholders = reopened->GetSlide(0)->Placeholders();
        REQUIRE(reopenedPlaceholders.size() == 3);
        CHECK(reopenedPlaceholders[0]->Origin() == PlaceholderOrigin::Layout);
        CHECK(reopenedPlaceholders[0]->Index() == 1u);
        CHECK(reopenedPlaceholders[1]->Origin() == PlaceholderOrigin::Layout);
        CHECK(reopenedPlaceholders[1]->Index() == 2u);
        CHECK(reopenedPlaceholders[2]->Origin() == PlaceholderOrigin::Master);
    }

    TEST_CASE("changing a slide layout preserves user placeholder content [unit] [powerpoint] [masters-layouts]")
    {
        auto editor = PowerPointDocumentEditor::CreateNew();
        REQUIRE(editor != nullptr);
        auto master = editor->AddSlideMaster("Master");
        auto firstLayout = editor->AddSlideLayout(master, "First", SlideLayoutValues::Title);
        auto secondLayout = editor->AddSlideLayout(master, "Second", SlideLayoutValues::Object);
        auto slide = editor->AddSlide();
        REQUIRE(firstLayout != nullptr);
        REQUIRE(secondLayout != nullptr);
        REQUIRE(slide != nullptr);
        slide->GetPart()->SetXmlString(
            "<p:sld xmlns:a=\"http://schemas.openxmlformats.org/drawingml/2006/main\" "
            "xmlns:p=\"http://schemas.openxmlformats.org/presentationml/2006/main\"><p:cSld><p:spTree>"
            "<p:sp><p:nvSpPr><p:cNvPr id=\"2\" name=\"User title\"/><p:cNvSpPr/><p:nvPr>"
            "<p:ph type=\"title\" idx=\"1\"/></p:nvPr></p:nvSpPr><p:spPr/><p:txBody><a:bodyPr/>"
            "<a:lstStyle/><a:p><a:r><a:t>User-authored title</a:t></a:r></a:p></p:txBody></p:sp>"
            "</p:spTree></p:cSld></p:sld>");

        REQUIRE(editor->SetSlideLayout(0, firstLayout));
        const auto beforeChange = slide->GetPart()->GetXmlString();
        REQUIRE(editor->SetSlideLayout(0, secondLayout));
        CHECK(slide->GetPart()->GetXmlString() == beforeChange);
        CHECK(slide->GetPart()->GetXmlString().find("User-authored title") != std::string::npos);
        REQUIRE(slide->Layout() != nullptr);
        CHECK(slide->Layout()->Name() == "Second");
        CHECK(slide->Placeholders(false).size() == 1);

        auto copy = editor->CopySlide(0);
        REQUIRE(copy != nullptr);
        REQUIRE(copy->Layout() != nullptr);
        CHECK(copy->Layout()->GetPart() == secondLayout->GetPart());
        CHECK(copy->GetPart()->GetXmlString().find("User-authored title") != std::string::npos);

        auto reopened = PowerPointDocumentEditor::Open(editor->SaveToMemory());
        REQUIRE(reopened != nullptr);
        REQUIRE(reopened->GetSlide(0)->Layout() != nullptr);
        REQUIRE(reopened->GetSlide(1)->Layout() != nullptr);
        CHECK(reopened->GetSlide(0)->Layout()->Name() == "Second");
        CHECK(reopened->GetSlide(1)->Layout()->Name() == "Second");
        CHECK(reopened->GetSlide(0)->GetPart()->GetXmlString().find("User-authored title") != std::string::npos);
    }

    TEST_CASE("master and layout APIs reject foreign wrappers and invalid indices [unit] [powerpoint] [masters-layouts]")
    {
        auto first = PowerPointDocumentEditor::CreateNew();
        auto second = PowerPointDocumentEditor::CreateNew();
        REQUIRE(first != nullptr);
        REQUIRE(second != nullptr);
        auto foreignMaster = second->AddSlideMaster("Foreign");
        auto foreignLayout = second->AddSlideLayout(foreignMaster, "Foreign layout", SlideLayoutValues::Blank);
        REQUIRE(foreignLayout != nullptr);
        REQUIRE(first->AddSlide() != nullptr);

        CHECK(first->AddSlideLayout(foreignMaster, "Invalid") == nullptr);
        CHECK_FALSE(first->SetSlideLayout(0, foreignLayout));
        CHECK_FALSE(first->SetSlideLayout(1, foreignLayout));
        CHECK(first->GetSlideMaster(0) == nullptr);
    }

    TEST_CASE("type based inheritance handles defaults duplicate keys and non-shape hosts [unit] [powerpoint] [masters-layouts]")
    {
        auto editor = PowerPointDocumentEditor::CreateNew();
        REQUIRE(editor != nullptr);
        auto master = editor->AddSlideMaster("Master");
        auto layout = editor->AddSlideLayout(master, "Mixed hosts", SlideLayoutValues::Custom);
        auto slide = editor->AddSlide();
        REQUIRE(master != nullptr);
        REQUIRE(layout != nullptr);
        REQUIRE(slide != nullptr);
        REQUIRE(editor->SetSlideLayout(0, layout));
        REQUIRE(master->AddPlaceholder(PlaceholderValues::Object) != nullptr);
        REQUIRE(master->AddPlaceholder(PlaceholderValues::Body) != nullptr);
        REQUIRE(master->AddPlaceholder(PlaceholderValues::Title, 7) != nullptr);

        layout->GetPart()->SetXmlString(
            "<p:sldLayout xmlns:a=\"http://schemas.openxmlformats.org/drawingml/2006/main\" "
            "xmlns:p=\"http://schemas.openxmlformats.org/presentationml/2006/main\" type=\"bogus\">"
            "<p:cSld name=\"Mixed hosts\">"
            "<p:spTree><p:pic><p:nvPicPr><p:cNvPr id=\"2\" name=\"Picture placeholder\"/>"
            "<p:cNvPicPr/><p:nvPr><p:ph/></p:nvPr></p:nvPicPr><p:blipFill/><p:spPr/></p:pic>"
            "<p:graphicFrame><p:nvGraphicFramePr><p:cNvPr id=\"3\" name=\"Table placeholder\"/>"
            "<p:cNvGraphicFramePr/><p:nvPr><p:ph type=\"body\"/></p:nvPr></p:nvGraphicFramePr>"
            "<p:xfrm/><a:graphic><a:graphicData uri=\"urn:test\"/></a:graphic></p:graphicFrame>"
            "<p:sp><p:nvSpPr><p:cNvPr id=\"4\" name=\"Duplicate A\"/><p:cNvSpPr/><p:nvPr>"
            "<p:ph type=\"body\" idx=\"7\"/></p:nvPr></p:nvSpPr><p:spPr/></p:sp>"
            "<p:sp><p:nvSpPr><p:cNvPr id=\"5\" name=\"Duplicate B\"/><p:cNvSpPr/><p:nvPr>"
            "<p:ph type=\"object\" idx=\"7\"/></p:nvPr></p:nvSpPr><p:spPr/></p:sp>"
            "</p:spTree></p:cSld></p:sldLayout>");

        auto direct = layout->Placeholders(false);
        REQUIRE(direct.size() == 4);
        CHECK(layout->Type() == SlideLayoutValues::Custom);
        CHECK(direct[0]->Type() == PlaceholderValues::Object);
        CHECK(direct[0]->GetShape() == nullptr);
        CHECK(std::dynamic_pointer_cast<ExyokiOffice::DocumentFormat::OpenXml::Presentation::Picture>(
                  direct[0]->GetElement()) != nullptr);
        CHECK(direct[1]->Type() == PlaceholderValues::Body);
        CHECK(std::dynamic_pointer_cast<ExyokiOffice::DocumentFormat::OpenXml::Presentation::GraphicFrame>(
                  direct[1]->GetElement()) != nullptr);
        CHECK(direct[2]->Index() == 7u);
        CHECK(direct[3]->Index() == 7u);

        auto effective = slide->Placeholders();
        REQUIRE(effective.size() == 3);
        CHECK(effective[0]->Origin() == PlaceholderOrigin::Layout);
        CHECK(effective[1]->Origin() == PlaceholderOrigin::Layout);
        CHECK(effective[2]->Origin() == PlaceholderOrigin::Layout);
        CHECK(direct[0]->Remove());
        CHECK(layout->Placeholders(false).size() == 3);
    }

    TEST_CASE("layout assignment is idempotent and leaves no stale relationship edges [unit] [powerpoint] [masters-layouts]")
    {
        auto editor = PowerPointDocumentEditor::CreateNew();
        REQUIRE(editor != nullptr);
        auto master = editor->AddSlideMaster("Master");
        auto firstLayout = editor->AddSlideLayout(master, "First", SlideLayoutValues::Title);
        auto secondLayout = editor->AddSlideLayout(master, "Second", SlideLayoutValues::Blank);
        auto slide = editor->AddSlide();
        REQUIRE(firstLayout != nullptr);
        REQUIRE(secondLayout != nullptr);
        REQUIRE(slide != nullptr);

        REQUIRE(editor->SetSlideLayout(0, firstLayout));
        const auto firstRelationship = slide->GetPart()->RelationshipsByType(
            "http://schemas.openxmlformats.org/officeDocument/2006/relationships/slideLayout");
        REQUIRE(firstRelationship.size() == 1);
        REQUIRE(editor->SetSlideLayout(0, firstLayout));
        CHECK(slide->GetPart()->RelationshipsByType(firstRelationship[0].Type).size() == 1);
        REQUIRE(editor->SetSlideLayout(0, secondLayout));
        REQUIRE(editor->SetSlideLayout(0, firstLayout));
        auto finalRelationships = slide->GetPart()->RelationshipsByType(firstRelationship[0].Type);
        REQUIRE(finalRelationships.size() == 1);
        CHECK(slide->Layout()->GetPart() == firstLayout->GetPart());
        CHECK(firstLayout->GetPart()->IncomingRelationships().size() >= 2);
        for (const auto& incoming : secondLayout->GetPart()->IncomingRelationships())
        {
            CHECK(incoming.SourceUri != slide->GetPart()->Uri());
        }

        auto reopened = PowerPointDocumentEditor::Open(editor->SaveToMemory());
        REQUIRE(reopened != nullptr);
        REQUIRE(reopened->GetSlide(0)->Layout() != nullptr);
        CHECK(reopened->GetSlide(0)->Layout()->Name() == "First");
    }

    TEST_CASE("malformed hierarchy fails safely and creation rolls back [unit] [powerpoint] [masters-layouts]")
    {
        auto editor = PowerPointDocumentEditor::CreateNew();
        REQUIRE(editor != nullptr);
        auto master = editor->AddSlideMaster("Malformed");
        REQUIRE(master != nullptr);
        master->GetPart()->SetXmlString(
            "<p:sldMaster xmlns:p=\"http://schemas.openxmlformats.org/presentationml/2006/main\">"
            "<p:cSld name=\"Malformed\"/></p:sldMaster>");
        const auto partsBefore = master->GetPart()->Parts().size();
        CHECK(editor->AddSlideLayout(master, "Must fail") == nullptr);
        CHECK(master->GetPart()->Parts().size() == partsBefore);
        CHECK(master->Layouts().empty());

        auto presentationPart = editor->GetDocument()->GetPresentationPart();
        REQUIRE(presentationPart != nullptr);
        presentationPart->SetXmlString(
            "<p:presentation xmlns:p=\"http://schemas.openxmlformats.org/presentationml/2006/main\" "
            "xmlns:r=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships\">"
            "<p:sldMasterIdLst><p:sldMasterId id=\"2147483648\" r:id=\"rId999\"/></p:sldMasterIdLst>"
            "</p:presentation>");
        CHECK(editor->SlideMasters().empty());
        CHECK(editor->GetSlideMaster(0) == nullptr);
    }

    TEST_CASE("unknown master and layout XML survives enumeration assignment and round trip [unit] [powerpoint] [masters-layouts]")
    {
        auto editor = PowerPointDocumentEditor::CreateNew();
        REQUIRE(editor != nullptr);
        auto master = editor->AddSlideMaster("Extensions");
        auto layout = editor->AddSlideLayout(master, "Extensions", SlideLayoutValues::Custom);
        auto slide = editor->AddSlide();
        REQUIRE(layout != nullptr);
        REQUIRE(slide != nullptr);
        auto masterXml = master->GetPart()->GetXmlString();
        auto layoutXml = layout->GetPart()->GetXmlString();
        const auto masterEnd = masterXml.rfind("</p:sldMaster>");
        const auto layoutEnd = layoutXml.rfind("</p:sldLayout>");
        REQUIRE(masterEnd != std::string::npos);
        REQUIRE(layoutEnd != std::string::npos);
        masterXml.insert(masterEnd, "<p:extLst><p:ext uri=\"urn:vendor:master\"><v:data xmlns:v=\"urn:vendor\" value=\"M\"/></p:ext></p:extLst>");
        layoutXml.insert(layoutEnd, "<p:extLst><p:ext uri=\"urn:vendor:layout\"><v:data xmlns:v=\"urn:vendor\" value=\"L\"/></p:ext></p:extLst>");
        master->GetPart()->SetXmlString(masterXml);
        layout->GetPart()->SetXmlString(layoutXml);
        REQUIRE(editor->SetSlideLayout(0, layout));
        CHECK(editor->SlideMasters()[0]->GetPart()->GetXmlString().find("urn:vendor:master") != std::string::npos);
        CHECK(editor->SlideLayouts()[0]->GetPart()->GetXmlString().find("urn:vendor:layout") != std::string::npos);

        auto reopened = PowerPointDocumentEditor::Open(editor->SaveToMemory());
        REQUIRE(reopened != nullptr);
        CHECK(reopened->SlideMasters()[0]->GetPart()->GetXmlString().find("urn:vendor:master") != std::string::npos);
        CHECK(reopened->SlideLayouts()[0]->GetPart()->GetXmlString().find("urn:vendor:layout") != std::string::npos);
        CHECK(reopened->GetSlide(0)->Layout()->Name() == "Extensions");
    }

    TEST_CASE("shared part reference primitive deduplicates removes and preserves the target [unit] [powerpoint] [masters-layouts]")
    {
        auto editor = PowerPointDocumentEditor::CreateNew();
        REQUIRE(editor != nullptr);
        auto master = editor->AddSlideMaster("Master");
        auto layout = editor->AddSlideLayout(master, "Layout", SlideLayoutValues::Blank);
        auto slide = editor->AddSlide();
        REQUIRE(layout != nullptr);
        REQUIRE(slide != nullptr);
        constexpr std::string_view relationshipType =
            "http://schemas.openxmlformats.org/officeDocument/2006/relationships/slideLayout";
        const auto firstId = slide->GetPart()->AddPartReference(layout->GetPart(), relationshipType);
        const auto repeatedId = slide->GetPart()->AddPartReference(layout->GetPart(), relationshipType);
        REQUIRE_FALSE(firstId.empty());
        CHECK(repeatedId == firstId);
        CHECK(slide->GetPart()->RelationshipsByType(relationshipType).size() == 1);
        REQUIRE(slide->GetPart()->RemovePartReference(layout->GetPart()));
        CHECK_FALSE(slide->GetPart()->RemovePartReference(layout->GetPart()));
        CHECK(slide->GetPart()->RelationshipsByType(relationshipType).empty());
        CHECK(editor->GetDocument()->GetPartByUri(layout->GetPart()->Uri()) == layout->GetPart());
    }

    TEST_CASE("themes can be applied replaced removed and round tripped [unit] [powerpoint] [masters-layouts]")
    {
        auto editor = PowerPointDocumentEditor::CreateNew();
        REQUIRE(editor != nullptr);
        auto master = editor->AddSlideMaster("Themed");
        REQUIRE(master != nullptr);
        // A new master carries the default Office theme; PowerPoint treats a
        // themeless master as a damaged presentation.
        REQUIRE(master->ThemeXml().has_value());
        CHECK(master->ThemeXml()->find("Office Theme") != std::string::npos);

        const std::string firstTheme =
            "<a:theme xmlns:a=\"http://schemas.openxmlformats.org/drawingml/2006/main\" name=\"Corporate Blue\"/>";
        REQUIRE(master->SetThemeXml(firstTheme));
        REQUIRE(master->ThemeXml().has_value());
        CHECK(master->ThemeXml()->find("Corporate Blue") != std::string::npos);
        CHECK_FALSE(master->SetThemeXml("<not-a-theme/>"));
        CHECK(master->ThemeXml()->find("Corporate Blue") != std::string::npos);

        auto reopened = PowerPointDocumentEditor::Open(editor->SaveToMemory());
        REQUIRE(reopened != nullptr);
        REQUIRE(reopened->GetSlideMaster(0) != nullptr);
        REQUIRE(reopened->GetSlideMaster(0)->ThemeXml().has_value());
        CHECK(reopened->GetSlideMaster(0)->ThemeXml()->find("Corporate Blue") != std::string::npos);
        CHECK(reopened->GetSlideMaster(0)->RemoveTheme());
        CHECK_FALSE(reopened->GetSlideMaster(0)->RemoveTheme());
        CHECK_FALSE(reopened->GetSlideMaster(0)->ThemeXml().has_value());
    }

    TEST_CASE("design import deep copies master layouts theme and extensions [unit] [powerpoint] [masters-layouts]")
    {
        auto source = PowerPointDocumentEditor::CreateNew();
        auto destination = PowerPointDocumentEditor::CreateNew();
        REQUIRE(source != nullptr);
        REQUIRE(destination != nullptr);
        auto sourceMaster = source->AddSlideMaster("Imported design");
        auto sourceTitle = source->AddSlideLayout(sourceMaster, "Imported title", SlideLayoutValues::Title);
        auto sourceBlank = source->AddSlideLayout(sourceMaster, "Imported blank", SlideLayoutValues::Blank);
        REQUIRE(sourceMaster != nullptr);
        REQUIRE(sourceTitle != nullptr);
        REQUIRE(sourceBlank != nullptr);
        REQUIRE(sourceMaster->SetThemeXml(
            "<a:theme xmlns:a=\"http://schemas.openxmlformats.org/drawingml/2006/main\" name=\"Imported theme\"/>"));
        REQUIRE(sourceTitle->AddPlaceholder(PlaceholderValues::Title, 1) != nullptr);

        auto imported = destination->ImportSlideMaster(sourceMaster);
        REQUIRE(imported != nullptr);
        CHECK(imported->Name() == "Imported design");
        REQUIRE(imported->Layouts().size() == 2);
        CHECK(imported->Layouts()[0]->Name() == "Imported title");
        CHECK(imported->Layouts()[1]->Type() == SlideLayoutValues::Blank);
        CHECK(imported->Layouts()[0]->Placeholders(false).size() == 1);
        REQUIRE(imported->ThemeXml().has_value());
        CHECK(imported->ThemeXml()->find("Imported theme") != std::string::npos);
        CHECK(imported->GetPart() != sourceMaster->GetPart());
        CHECK(imported->Layouts()[0]->GetPart() != sourceTitle->GetPart());
        CHECK(imported->GetPart()->Package() == destination->GetDocument().get());

        REQUIRE(destination->AddSlide() != nullptr);
        REQUIRE(destination->SetSlideLayout(0, imported->Layouts()[0]));
        CHECK(ExyokiOffice::OpenXmlPackageValidator().Validate(*destination->GetDocument()).IsValid());
        auto reopened = PowerPointDocumentEditor::Open(destination->SaveToMemory());
        REQUIRE(reopened != nullptr);
        REQUIRE(reopened->SlideMasters().size() == 1);
        REQUIRE(reopened->SlideLayouts().size() == 2);
        REQUIRE(reopened->GetSlide(0)->Layout() != nullptr);
        CHECK(reopened->GetSlide(0)->Layout()->Name() == "Imported title");
        CHECK(sourceMaster->Name() == "Imported design");
    }

    TEST_CASE("layout removal rejects unsafe deletion and can redirect slides [unit] [powerpoint] [masters-layouts]")
    {
        auto editor = PowerPointDocumentEditor::CreateNew();
        REQUIRE(editor != nullptr);
        auto master = editor->AddSlideMaster("Master");
        auto used = editor->AddSlideLayout(master, "Used", SlideLayoutValues::Object);
        auto replacement = editor->AddSlideLayout(master, "Replacement", SlideLayoutValues::Blank);
        auto slide = editor->AddSlide();
        REQUIRE(used != nullptr);
        REQUIRE(replacement != nullptr);
        REQUIRE(slide != nullptr);
        REQUIRE(editor->SetSlideLayout(0, used));
        slide->GetPart()->SetXmlString(
            "<p:sld xmlns:p=\"http://schemas.openxmlformats.org/presentationml/2006/main\">"
            "<p:cSld name=\"User content\"/></p:sld>");
        const auto slideXml = slide->GetPart()->GetXmlString();

        CHECK_FALSE(editor->RemoveSlideLayout(used));
        CHECK_FALSE(editor->RemoveSlideLayout(used, used));
        REQUIRE(editor->RemoveSlideLayout(used, replacement));
        CHECK(editor->SlideLayouts().size() == 1);
        REQUIRE(slide->Layout() != nullptr);
        CHECK(slide->Layout()->Name() == "Replacement");
        CHECK(slide->GetPart()->GetXmlString() == slideXml);
        CHECK_FALSE(editor->RemoveSlideLayout(used, replacement));
        CHECK(ExyokiOffice::OpenXmlPackageValidator().Validate(*editor->GetDocument()).IsValid());

        auto reopened = PowerPointDocumentEditor::Open(editor->SaveToMemory());
        REQUIRE(reopened != nullptr);
        REQUIRE(reopened->SlideLayouts().size() == 1);
        CHECK(reopened->GetSlide(0)->Layout()->Name() == "Replacement");
    }

    TEST_CASE("master removal redirects slides and rejects invalid replacements [unit] [powerpoint] [masters-layouts]")
    {
        auto editor = PowerPointDocumentEditor::CreateNew();
        REQUIRE(editor != nullptr);
        auto obsolete = editor->AddSlideMaster("Obsolete");
        auto retained = editor->AddSlideMaster("Retained");
        auto obsoleteLayout = editor->AddSlideLayout(obsolete, "Old", SlideLayoutValues::Title);
        auto retainedLayout = editor->AddSlideLayout(retained, "New", SlideLayoutValues::Title);
        REQUIRE(obsoleteLayout != nullptr);
        REQUIRE(retainedLayout != nullptr);
        REQUIRE(editor->AddSlide() != nullptr);
        REQUIRE(editor->SetSlideLayout(0, obsoleteLayout));

        CHECK_FALSE(editor->RemoveSlideMaster(obsolete));
        CHECK_FALSE(editor->RemoveSlideMaster(obsolete, obsoleteLayout));
        REQUIRE(editor->RemoveSlideMaster(obsolete, retainedLayout));
        REQUIRE(editor->SlideMasters().size() == 1);
        CHECK(editor->SlideMasters()[0]->Name() == "Retained");
        REQUIRE(editor->GetSlide(0)->Layout() != nullptr);
        CHECK(editor->GetSlide(0)->Layout()->Name() == "New");
        CHECK_FALSE(editor->RemoveSlideMaster(obsolete, retainedLayout));
        CHECK(ExyokiOffice::OpenXmlPackageValidator().Validate(*editor->GetDocument()).IsValid());

        auto reopened = PowerPointDocumentEditor::Open(editor->SaveToMemory());
        REQUIRE(reopened != nullptr);
        REQUIRE(reopened->SlideMasters().size() == 1);
        REQUIRE(reopened->SlideLayouts().size() == 1);
        CHECK(reopened->GetSlide(0)->Layout()->Name() == "New");
    }

    TEST_CASE("master ID allocation reuses the first legal gap without unsigned overflow [unit] [powerpoint] [masters-layouts]")
    {
        auto editor = PowerPointDocumentEditor::CreateNew();
        REQUIRE(editor != nullptr);
        auto first = editor->AddSlideMaster("Near maximum");
        auto second = editor->AddSlideMaster("Maximum");
        REQUIRE(first != nullptr);
        REQUIRE(second != nullptr);

        auto presentation = editor->GetDocument()->GetPresentationPart()->GetTypedRootElement();
        auto list = presentation->GetFirstChildOfType<
            ExyokiOffice::DocumentFormat::OpenXml::Presentation::SlideMasterIdList>();
        REQUIRE(list != nullptr);
        auto entries = list->Elements<ExyokiOffice::DocumentFormat::OpenXml::Presentation::SlideMasterId>();
        REQUIRE(entries.size() == 2);
        entries[0]->SetId(ExyokiOffice::UInt32Value(0xfffffffeu));
        entries[1]->SetId(ExyokiOffice::UInt32Value(0xffffffffu));

        auto allocated = editor->AddSlideMaster("First legal gap");
        REQUIRE(allocated != nullptr);
        CHECK(allocated->Id() == 0x80000000u);
        CHECK(allocated->Id() != first->Id());
        CHECK(allocated->Id() != second->Id());
        CHECK(ExyokiOffice::OpenXmlPackageValidator().Validate(*editor->GetDocument()).IsValid());
    }

    TEST_CASE("typed theme settings update colors and fonts while preserving format effects [unit] [powerpoint] [masters-layouts]")
    {
        auto editor = PowerPointDocumentEditor::CreateNew();
        auto master = editor->AddSlideMaster("Theme model");
        REQUIRE(master != nullptr);
        const std::string xml =
            "<a:theme xmlns:a=\"http://schemas.openxmlformats.org/drawingml/2006/main\" name=\"Original\">"
            "<a:themeElements><a:clrScheme name=\"Colors\">"
            "<a:dk1><a:sysClr val=\"windowText\" lastClr=\"000000\"/></a:dk1><a:lt1><a:srgbClr val=\"FFFFFF\"/></a:lt1>"
            "<a:dk2><a:srgbClr val=\"111111\"/></a:dk2><a:lt2><a:srgbClr val=\"EEEEEE\"/></a:lt2>"
            "<a:accent1><a:srgbClr val=\"100001\"/></a:accent1><a:accent2><a:srgbClr val=\"200002\"/></a:accent2>"
            "<a:accent3><a:srgbClr val=\"300003\"/></a:accent3><a:accent4><a:srgbClr val=\"400004\"/></a:accent4>"
            "<a:accent5><a:srgbClr val=\"500005\"/></a:accent5><a:accent6><a:srgbClr val=\"600006\"/></a:accent6>"
            "<a:hlink><a:srgbClr val=\"0000FF\"/></a:hlink><a:folHlink><a:srgbClr val=\"800080\"/></a:folHlink>"
            "</a:clrScheme><a:fontScheme name=\"Fonts\"><a:majorFont><a:latin typeface=\"Aptos Display\"/>"
            "<a:ea typeface=\"\"/><a:cs typeface=\"\"/><a:font script=\"Jpan\" typeface=\"Yu Gothic\"/></a:majorFont>"
            "<a:minorFont><a:latin typeface=\"Aptos\"/><a:ea typeface=\"\"/><a:cs typeface=\"\"/></a:minorFont>"
            "</a:fontScheme><a:fmtScheme name=\"Effects\"><a:fillStyleLst/><a:lnStyleLst/>"
            "<a:effectStyleLst><a:effectStyle><a:effectLst><a:glow rad=\"1000\"><a:srgbClr val=\"FF0000\"/>"
            "</a:glow></a:effectLst></a:effectStyle></a:effectStyleLst><a:bgFillStyleLst/>"
            "<a:extLst><a:ext uri=\"urn:preserve\"><x:data xmlns:x=\"urn:test\"/></a:ext></a:extLst>"
            "</a:fmtScheme></a:themeElements></a:theme>";
        REQUIRE(master->SetThemeXml(xml));
        auto settings = master->ThemeSettings();
        REQUIRE(settings.has_value());
        CHECK(settings->Name == "Original");
        CHECK(settings->MajorFonts.Latin == "Aptos Display");
        REQUIRE(settings->MajorFonts.SupplementalFonts.size() == 1);
        CHECK(settings->Colors[0].ToHexString() == "000000");

        settings->Name = "Corporate";
        settings->ColorSchemeName = "Corporate colors";
        settings->Colors[4] = ExyokiOffice::Color(10, 20, 30);
        settings->MajorFonts.Latin = "Contoso Display";
        settings->MinorFonts.Latin = "Contoso Text";
        settings->MinorFonts.SupplementalFonts = {{"Arab", "Contoso Arabic"}};
        REQUIRE(master->SetThemeSettings(*settings));
        auto updated = master->ThemeSettings();
        REQUIRE(updated.has_value());
        CHECK(updated->Name == "Corporate");
        CHECK(updated->Colors[4].ToHexString() == "0A141E");
        CHECK(updated->MajorFonts.Latin == "Contoso Display");
        CHECK(updated->MinorFonts.SupplementalFonts[0].first == "Arab");
        REQUIRE(master->ThemeXml().has_value());
        CHECK(master->ThemeXml()->find("urn:preserve") != std::string::npos);
        CHECK(master->ThemeXml()->find("<a:glow") != std::string::npos);
        const auto beforeInvalidUpdate = *master->ThemeXml();
        auto invalid = *updated;
        invalid.Colors[4] = ExyokiOffice::Color();
        CHECK_FALSE(master->SetThemeSettings(invalid));
        CHECK(master->ThemeXml() == beforeInvalidUpdate);

        auto reopened = PowerPointDocumentEditor::Open(editor->SaveToMemory());
        REQUIRE(reopened != nullptr);
        auto roundTrip = reopened->GetSlideMaster(0)->ThemeSettings();
        REQUIRE(roundTrip.has_value());
        CHECK(roundTrip->Colors[4].ToHexString() == "0A141E");
        CHECK(roundTrip->MinorFonts.SupplementalFonts[0].second == "Contoso Arabic");
    }
} // namespace
