// Copyright (c) 2026 Jakub Melka and Collaborators
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "doctest.h"

#include "ExyokiOffice/PowerPoint/PowerPointDocument.hpp"
#include "ExyokiOffice/StandardTypes.hpp"

#include <set>

using ExyokiOffice::PowerPoint::PowerPointDocumentEditor;

TEST_SUITE("PowerPointSlideCollectionTests")
{
    TEST_CASE("slides are added with unique IDs and matching relationships [unit] [powerpoint] [slides]")
    {
        auto editor = PowerPointDocumentEditor::CreateNew();
        REQUIRE(editor != nullptr);
        CHECK(editor->SlideCount() == 0);

        const auto first = editor->AddSlide();
        const auto second = editor->AddSlide();
        const auto third = editor->AddSlide();
        REQUIRE(first != nullptr);
        REQUIRE(second != nullptr);
        REQUIRE(third != nullptr);

        CHECK(editor->SlideCount() == 3);
        CHECK(first->Id() >= 256);
        CHECK(second->Id() == first->Id() + 1);
        CHECK(third->Id() == second->Id() + 1);
        CHECK_FALSE(first->RelationshipId().empty());
        CHECK(first->RelationshipId() != second->RelationshipId());
        CHECK(second->RelationshipId() != third->RelationshipId());

        auto presentationPart = editor->GetDocument()->GetPresentationPart();
        REQUIRE(presentationPart != nullptr);
        CHECK(presentationPart->GetSlideParts().size() == 3);
        CHECK(editor->GetSlide(0)->GetPart() == first->GetPart());
        CHECK(editor->GetSlide(1)->GetPart() == second->GetPart());
        CHECK(editor->GetSlide(2)->GetPart() == third->GetPart());
        CHECK(editor->GetSlide(3) == nullptr);

        std::set<ExyokiOffice::UInt32> ids;
        std::set<std::string> relationshipIds;
        for (const auto& slide : editor->Slides())
        {
            ids.insert(slide->Id());
            relationshipIds.insert(slide->RelationshipId());
            REQUIRE(slide->GetPart() != nullptr);
            CHECK(slide->GetPart()->RelationshipId() == slide->RelationshipId());
        }
        CHECK(ids.size() == 3);
        CHECK(relationshipIds.size() == 3);
    }

    TEST_CASE("slide order follows presentation XML and survives round trip [unit] [powerpoint] [slides]")
    {
        auto editor = PowerPointDocumentEditor::CreateNew();
        REQUIRE(editor != nullptr);
        const auto first = editor->AddSlide();
        const auto second = editor->AddSlide();
        const auto third = editor->AddSlide();
        REQUIRE(first != nullptr);
        REQUIRE(second != nullptr);
        REQUIRE(third != nullptr);

        const auto firstId = first->Id();
        const auto secondId = second->Id();
        const auto thirdId = third->Id();
        CHECK(editor->MoveSlide(2, 0));
        CHECK(editor->MoveSlide(1, 2));
        CHECK(editor->MoveSlide(1, 1));
        CHECK_FALSE(editor->MoveSlide(3, 0));
        CHECK_FALSE(editor->MoveSlide(0, 3));

        auto ordered = editor->Slides();
        REQUIRE(ordered.size() == 3);
        CHECK(ordered[0]->Id() == thirdId);
        CHECK(ordered[1]->Id() == secondId);
        CHECK(ordered[2]->Id() == firstId);

        const auto bytes = editor->SaveToMemory();
        REQUIRE_FALSE(bytes.empty());
        auto reopened = PowerPointDocumentEditor::Open(bytes);
        REQUIRE(reopened != nullptr);
        auto reopenedSlides = reopened->Slides();
        REQUIRE(reopenedSlides.size() == 3);
        CHECK(reopenedSlides[0]->Id() == thirdId);
        CHECK(reopenedSlides[1]->Id() == secondId);
        CHECK(reopenedSlides[2]->Id() == firstId);
        CHECK(reopened->GetDocument()->GetPresentationPart()->GetSlideParts().size() == 3);
    }

    TEST_CASE("hidden state and removal update slide XML and package graph [unit] [powerpoint] [slides]")
    {
        auto editor = PowerPointDocumentEditor::CreateNew();
        REQUIRE(editor != nullptr);
        auto first = editor->AddSlide();
        auto hidden = editor->AddSlide();
        auto last = editor->AddSlide();
        REQUIRE(first != nullptr);
        REQUIRE(hidden != nullptr);
        REQUIRE(last != nullptr);

        CHECK_FALSE(hidden->IsHidden());
        CHECK(hidden->SetHidden(true));
        CHECK(hidden->IsHidden());
        CHECK(hidden->GetPart()->GetXmlString().find("show=\"0\"") != std::string::npos);
        CHECK_FALSE(first->IsHidden());

        const auto removedId = first->Id();
        const auto removedRelationship = first->RelationshipId();
        CHECK(editor->RemoveSlide(0));
        CHECK_FALSE(editor->RemoveSlide(9));
        CHECK(editor->SlideCount() == 2);
        CHECK(editor->GetDocument()->GetPresentationPart()->GetSlideParts().size() == 2);
        for (const auto& slide : editor->Slides())
        {
            CHECK(slide->Id() != removedId);
            CHECK(slide->RelationshipId() != removedRelationship);
        }

        auto reopened = PowerPointDocumentEditor::Open(editor->SaveToMemory());
        REQUIRE(reopened != nullptr);
        REQUIRE(reopened->SlideCount() == 2);
        CHECK(reopened->GetSlide(0)->Id() == hidden->Id());
        CHECK(reopened->GetSlide(0)->IsHidden());
        CHECK(reopened->GetSlide(1)->Id() == last->Id());
        CHECK(reopened->GetDocument()->GetPresentationPart()->GetSlideParts().size() == 2);

        CHECK(reopened->RemoveSlide(1));
        CHECK(reopened->RemoveSlide(0));
        CHECK(reopened->SlideCount() == 0);
        CHECK(reopened->GetDocument()->GetPresentationPart()->GetSlideParts().empty());
    }
} // namespace
