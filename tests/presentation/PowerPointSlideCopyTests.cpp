// Copyright (c) 2026 Jakub Melka and Contributors
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "doctest.h"

#include "ExyokiOffice/PowerPoint/PowerPointDocument.hpp"
#include "ExyokiOffice/StandardTypes.hpp"

#include <algorithm>
#include <unordered_set>

using ExyokiOffice::PowerPoint::PowerPointDocumentEditor;

TEST_SUITE("PowerPointSlideCopyTests")
{
    TEST_CASE("slide copy has independent XML and a new collection identity [unit] [powerpoint] [slide-copy]")
    {
        auto editor = PowerPointDocumentEditor::CreateNew();
        REQUIRE(editor != nullptr);
        auto source = editor->AddSlide();
        REQUIRE(source != nullptr);
        source->GetPart()->SetXmlString(
            "<p:sld xmlns:p=\"http://schemas.openxmlformats.org/presentationml/2006/main\">"
            "<p:cSld name=\"Source\"/></p:sld>");
        REQUIRE(source->SetHidden(true));

        auto copy = editor->CopySlide(0);
        REQUIRE(copy != nullptr);
        CHECK(editor->SlideCount() == 2);
        CHECK(copy->Id() != source->Id());
        CHECK(copy->RelationshipId() != source->RelationshipId());
        CHECK(copy->GetPart() != source->GetPart());
        CHECK(copy->GetPart()->Uri() != source->GetPart()->Uri());
        CHECK(copy->IsHidden());
        CHECK(editor->GetSlide(1)->Id() == copy->Id());

        copy->GetPart()->SetXmlString(
            "<p:sld xmlns:p=\"http://schemas.openxmlformats.org/presentationml/2006/main\">"
            "<p:cSld name=\"Copy\"/></p:sld>");
        CHECK(source->GetPart()->GetXmlString().find("Source") != std::string::npos);
        CHECK(source->GetPart()->GetXmlString().find("Copy") == std::string::npos);
        CHECK(copy->GetPart()->GetXmlString().find("Copy") != std::string::npos);
        CHECK(editor->CopySlide(2) == nullptr);
    }

    TEST_CASE("slide copy deep-copies notes and mutable descendants while sharing media [unit] [powerpoint] [slide-copy]")
    {
        auto editor = PowerPointDocumentEditor::CreateNew();
        REQUIRE(editor != nullptr);
        auto source = editor->AddSlide();
        REQUIRE(source != nullptr);

        auto image = source->GetPart()->AddImagePart();
        REQUIRE(image != nullptr);
        image->SetContentType("image/png");
        image->SetBinaryData({0x89, 0x50, 0x4e, 0x47});
        auto chart = source->GetPart()->AddChartPart();
        REQUIRE(chart != nullptr);
        chart->SetXmlString("<c:chartSpace xmlns:c=\"http://schemas.openxmlformats.org/drawingml/2006/chart\"/>");
        auto notes = source->GetPart()->AddNotesSlidePart();
        REQUIRE(notes != nullptr);
        notes->SetXmlString(
            "<p:notes xmlns:p=\"http://schemas.openxmlformats.org/presentationml/2006/main\">"
            "<p:cSld name=\"Original notes\"/></p:notes>");
        auto notesImage = notes->AddImagePart();
        REQUIRE(notesImage != nullptr);
        notesImage->SetContentType("image/png");
        notesImage->SetBinaryData({1, 2, 3, 4});
        const auto externalId = source->GetPart()->AddExternalRelationship("urn:test:hyperlink", "https://example.test/");
        REQUIRE_FALSE(externalId.empty());

        auto copy = editor->CopySlide(0);
        REQUIRE(copy != nullptr);
        auto copiedNotes = copy->GetPart()->GetNotesSlidePart();
        auto copiedChart = copy->GetPart()->GetChartParts();
        REQUIRE(copiedNotes != nullptr);
        REQUIRE(copiedChart.size() == 1);
        CHECK(copiedNotes != notes);
        CHECK(copiedChart[0] != chart);
        REQUIRE(copy->GetPart()->GetImageParts().size() == 1);
        REQUIRE(copiedNotes->GetImageParts().size() == 1);
        CHECK(copy->GetPart()->GetImageParts()[0] == image);
        CHECK(copiedNotes->GetImageParts()[0] == notesImage);
        CHECK(copy->GetPart()->Relationships().size() == source->GetPart()->Relationships().size());

        copiedNotes->SetXmlString(
            "<p:notes xmlns:p=\"http://schemas.openxmlformats.org/presentationml/2006/main\">"
            "<p:cSld name=\"Copied notes\"/></p:notes>");
        copiedChart[0]->SetXmlString("<c:chartSpace xmlns:c=\"http://schemas.openxmlformats.org/drawingml/2006/chart\"><c:roundedCorners val=\"1\"/></c:chartSpace>");
        CHECK(notes->GetXmlString().find("Original notes") != std::string::npos);
        CHECK(chart->GetXmlString().find("roundedCorners") == std::string::npos);

        auto reopened = PowerPointDocumentEditor::Open(editor->SaveToMemory());
        REQUIRE(reopened != nullptr);
        REQUIRE(reopened->SlideCount() == 2);
        auto reopenedSource = reopened->GetSlide(0)->GetPart();
        auto reopenedCopy = reopened->GetSlide(1)->GetPart();
        REQUIRE(reopenedSource->GetNotesSlidePart() != nullptr);
        REQUIRE(reopenedCopy->GetNotesSlidePart() != nullptr);
        CHECK(reopenedSource->GetNotesSlidePart()->GetXmlString().find("Original notes") != std::string::npos);
        CHECK(reopenedCopy->GetNotesSlidePart()->GetXmlString().find("Copied notes") != std::string::npos);
        REQUIRE(reopenedSource->GetImageParts().size() == 1);
        REQUIRE(reopenedCopy->GetImageParts().size() == 1);
        CHECK(reopenedSource->GetImageParts()[0]->Uri() == reopenedCopy->GetImageParts()[0]->Uri());
    }

    TEST_CASE("repeated copies preserve insertion order and allocate unique IDs [unit] [powerpoint] [slide-copy]")
    {
        auto editor = PowerPointDocumentEditor::CreateNew();
        REQUIRE(editor != nullptr);
        auto first = editor->AddSlide();
        auto last = editor->AddSlide();
        REQUIRE(first != nullptr);
        REQUIRE(last != nullptr);
        auto firstCopy = editor->CopySlide(0);
        auto secondCopy = editor->CopySlide(0);
        REQUIRE(firstCopy != nullptr);
        REQUIRE(secondCopy != nullptr);

        auto slides = editor->Slides();
        REQUIRE(slides.size() == 4);
        CHECK(slides[0]->Id() == first->Id());
        CHECK(slides[1]->Id() == secondCopy->Id());
        CHECK(slides[2]->Id() == firstCopy->Id());
        CHECK(slides[3]->Id() == last->Id());
        CHECK(firstCopy->Id() != secondCopy->Id());
        CHECK(firstCopy->RelationshipId() != secondCopy->RelationshipId());
    }

    TEST_CASE("cross-presentation copy imports hierarchy and mutable payload graph [unit] [powerpoint] [slide-copy]")
    {
        auto source = PowerPointDocumentEditor::CreateNew();
        auto destination = PowerPointDocumentEditor::CreateNew();
        REQUIRE(source != nullptr);
        REQUIRE(destination != nullptr);

        auto destinationMaster = destination->AddSlideMaster("Existing master");
        auto destinationLayout = destination->AddSlideLayout(destinationMaster, "Existing layout");
        REQUIRE(destinationLayout != nullptr);
        REQUIRE(destination->AddSlide() != nullptr);
        REQUIRE(destination->SetSlideLayout(0, destinationLayout));

        auto master = source->AddSlideMaster("Imported master");
        auto layout = source->AddSlideLayout(master, "Imported layout");
        auto slide = source->AddSlide();
        REQUIRE(master != nullptr);
        REQUIRE(layout != nullptr);
        REQUIRE(slide != nullptr);
        REQUIRE(source->SetSlideLayout(0, layout));
        REQUIRE(slide->SetHidden(true));

        auto theme = master->GetPart()->AddThemePart();
        REQUIRE(theme != nullptr);
        theme->SetXmlString("<a:theme xmlns:a=\"http://schemas.openxmlformats.org/drawingml/2006/main\" name=\"Imported theme\"/>");
        auto image = slide->GetPart()->AddImagePart();
        REQUIRE(image != nullptr);
        image->SetContentType("image/png");
        image->SetBinaryData({0x89, 0x50, 0x4e, 0x47, 0x01});
        auto chart = slide->GetPart()->AddChartPart();
        REQUIRE(chart != nullptr);
        chart->SetXmlString("<c:chartSpace xmlns:c=\"http://schemas.openxmlformats.org/drawingml/2006/chart\"><c:date1904 val=\"1\"/></c:chartSpace>");
        auto workbook = chart->AddEmbeddedPackagePart();
        REQUIRE(workbook != nullptr);
        workbook->SetContentType("application/vnd.openxmlformats-officedocument.spreadsheetml.sheet");
        workbook->SetBinaryData({0x50, 0x4b, 0x03, 0x04, 0x55});
        const auto hyperlinkId = slide->GetPart()->AddExternalRelationship(
            "http://schemas.openxmlformats.org/officeDocument/2006/relationships/hyperlink",
            "https://example.test/imported");
        REQUIRE_FALSE(hyperlinkId.empty());

        auto imported = destination->CopySlideFrom(*source, 0);
        REQUIRE(imported != nullptr);
        CHECK(destination->SlideCount() == 2);
        CHECK(imported->Id() != slide->Id());
        CHECK(imported->IsHidden());
        REQUIRE(imported->Layout() != nullptr);
        CHECK(imported->Layout()->Name() == "Imported layout");
        REQUIRE(imported->Layout()->Master() != nullptr);
        CHECK(imported->Layout()->Master()->Name() == "Imported master");
        CHECK(imported->GetPart()->Package() == destination->GetDocument().get());
        CHECK(imported->GetPart()->Uri() != slide->GetPart()->Uri());

        auto importedPart = imported->GetPart();
        REQUIRE(importedPart->GetImageParts().size() == 1);
        REQUIRE(importedPart->GetChartParts().size() == 1);
        auto importedChart = importedPart->GetChartParts().front();
        auto importedWorkbook = importedChart->GetEmbeddedPackagePart();
        REQUIRE(importedWorkbook != nullptr);
        CHECK(importedPart->GetImageParts().front() != image);
        CHECK(importedChart != chart);
        CHECK(importedWorkbook != workbook);
        CHECK(importedPart->GetImageParts().front()->GetBinaryData() == image->GetBinaryData());
        CHECK(importedWorkbook->GetBinaryData() == workbook->GetBinaryData());
        REQUIRE(importedPart->Relationships().size() == slide->GetPart()->Relationships().size());
        const auto importedHyperlink = std::find_if(importedPart->Relationships().begin(),
                                                    importedPart->Relationships().end(), [&](const auto& relationship)
                                                    { return relationship.Id == hyperlinkId; });
        REQUIRE(importedHyperlink != importedPart->Relationships().end());
        CHECK(importedHyperlink->Target == "https://example.test/imported");
        CHECK(importedHyperlink->IsExternal);
        REQUIRE(imported->Layout()->Master()->GetPart()->GetThemePart() != nullptr);
        CHECK(imported->Layout()->Master()->GetPart()->GetThemePart() != theme);
        CHECK(imported->Layout()->Master()->GetPart()->GetThemePart()->GetXmlString().find("Imported theme") != std::string::npos);

        std::unordered_set<ExyokiOffice::UInt32> masterIds;
        std::unordered_set<ExyokiOffice::UInt32> layoutIds;
        for (const auto& currentMaster : destination->SlideMasters())
        {
            CHECK(masterIds.insert(currentMaster->Id()).second);
            for (const auto& currentLayout : currentMaster->Layouts())
            {
                CHECK(layoutIds.insert(currentLayout->Id()).second);
            }
        }

        importedChart->SetXmlString("<c:chartSpace xmlns:c=\"http://schemas.openxmlformats.org/drawingml/2006/chart\"/>");
        importedWorkbook->SetBinaryData({9, 8, 7});
        CHECK(chart->GetXmlString().find("date1904") != std::string::npos);
        CHECK(workbook->GetBinaryData() == std::vector<ExyokiOffice::Byte>({0x50, 0x4b, 0x03, 0x04, 0x55}));

        auto reopened = PowerPointDocumentEditor::Open(destination->SaveToMemory());
        REQUIRE(reopened != nullptr);
        REQUIRE(reopened->SlideCount() == 2);
        REQUIRE(reopened->SlideMasters().size() == 2);
        auto reopenedImport = reopened->GetSlide(1);
        REQUIRE(reopenedImport != nullptr);
        REQUIRE(reopenedImport->Layout() != nullptr);
        REQUIRE(reopenedImport->Layout()->Master() != nullptr);
        CHECK(reopenedImport->Layout()->Master()->Name() == "Imported master");
        REQUIRE(reopenedImport->GetPart()->GetChartParts().size() == 1);
        REQUIRE(reopenedImport->GetPart()->GetChartParts().front()->GetEmbeddedPackagePart() != nullptr);
        CHECK(reopenedImport->GetPart()->GetChartParts().front()->GetEmbeddedPackagePart()->GetBinaryData() ==
              std::vector<ExyokiOffice::Byte>({9, 8, 7}));
        const auto& reopenedRelationships = reopenedImport->GetPart()->Relationships();
        const auto reopenedHyperlink = std::find_if(reopenedRelationships.begin(), reopenedRelationships.end(),
                                                    [&](const auto& relationship)
                                                    { return relationship.Id == hyperlinkId; });
        REQUIRE(reopenedHyperlink != reopenedRelationships.end());
        CHECK(reopenedHyperlink->Target == "https://example.test/imported");
        CHECK(reopenedHyperlink->IsExternal);
    }

    TEST_CASE("cross-presentation copy rejects invalid ownership and indices without mutation [unit] [powerpoint] [slide-copy]")
    {
        auto source = PowerPointDocumentEditor::CreateNew();
        auto destination = PowerPointDocumentEditor::CreateNew();
        REQUIRE(source != nullptr);
        REQUIRE(destination != nullptr);
        REQUIRE(source->AddSlide() != nullptr);

        CHECK(destination->CopySlideFrom(*source, 1) == nullptr);
        CHECK(destination->SlideCount() == 0);
        CHECK(source->CopySlideFrom(*source, 0) == nullptr);
        CHECK(source->SlideCount() == 1);

        PowerPointDocumentEditor empty;
        CHECK(destination->CopySlideFrom(empty, 0) == nullptr);
        CHECK(empty.CopySlideFrom(*source, 0) == nullptr);
        CHECK(destination->SlideCount() == 0);
    }
} // namespace
