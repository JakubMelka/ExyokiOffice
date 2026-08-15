// Copyright (c) 2026 Jakub Melka and Contributors
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "doctest.h"

#include "ExyokiOffice/PowerPoint/PowerPointDocument.hpp"
#include "ExyokiOffice/StandardTypes.hpp"

using namespace ExyokiOffice::PowerPoint;

TEST_SUITE("PowerPointSectionCustomShowTests")
{
    TEST_CASE("section and custom-show CRUD survive a package round trip [unit] [powerpoint] [section-custom-show]")
    {
        auto editor = PowerPointDocumentEditor::CreateNew();
        auto first = editor->AddSlide();
        auto second = editor->AddSlide();
        auto third = editor->AddSlide();
        REQUIRE(first);
        REQUIRE(second);
        REQUIRE(third);

        PresentationSection opening{"{SECTION-1}", "Opening", {first->Id(), second->Id()}};
        PresentationSection appendix{"{SECTION-2}", "Appendix", {third->Id()}};
        REQUIRE(editor->AddSection(opening));
        REQUIRE(editor->AddSection(appendix));
        PresentationCustomShow show{7, "Executive", {third->Id(), first->Id(), third->Id()}};
        REQUIRE(editor->AddCustomShow(show));
        CHECK(editor->Sections() == std::vector<PresentationSection>{opening, appendix});
        CHECK(editor->CustomShows() == std::vector<PresentationCustomShow>{show});

        opening.Name = "Introduction";
        opening.SlideIds = {second->Id(), first->Id()};
        REQUIRE(editor->UpdateSection(opening.Id, opening));
        opening.SlideIds = {first->Id(), second->Id()};
        show.Name = "Board";
        show.SlideIds = {second->Id(), third->Id()};
        REQUIRE(editor->UpdateCustomShow(show.Id, show));

        auto reopened = PowerPointDocumentEditor::Open(editor->SaveToMemory());
        REQUIRE(reopened);
        CHECK(reopened->Sections() == std::vector<PresentationSection>{opening, appendix});
        CHECK(reopened->CustomShows() == std::vector<PresentationCustomShow>{show});
        REQUIRE(reopened->RemoveSection(appendix.Id));
        REQUIRE(reopened->RemoveCustomShow(show.Id));
        CHECK(reopened->Sections() == std::vector<PresentationSection>{opening});
        CHECK(reopened->CustomShows().empty());
    }

    TEST_CASE("section and custom-show validation is transactional [unit] [powerpoint] [section-custom-show]")
    {
        auto editor = PowerPointDocumentEditor::CreateNew();
        auto first = editor->AddSlide();
        auto second = editor->AddSlide();
        REQUIRE(first);
        REQUIRE(second);
        PresentationSection section{"section", "One", {first->Id()}};
        PresentationCustomShow show{1, "Show", {first->Id()}};
        REQUIRE(editor->AddSection(section));
        REQUIRE(editor->AddCustomShow(show));

        CHECK_FALSE(editor->AddSection(section));
        CHECK_FALSE(editor->AddSection({"other", "Overlap", {first->Id()}}));
        CHECK_FALSE(editor->AddSection({"missing", "Missing", {999999}}));
        CHECK_FALSE(editor->UpdateSection("section", {"changed", "One", {second->Id()}}));
        CHECK_FALSE(editor->AddCustomShow(show));
        CHECK_FALSE(editor->AddCustomShow({2, "Show", {second->Id()}}));
        CHECK_FALSE(editor->AddCustomShow({2, "Missing", {999999}}));
        CHECK_FALSE(editor->UpdateCustomShow(1, {2, "Changed", {second->Id()}}));
        CHECK(editor->Sections() == std::vector<PresentationSection>{section});
        CHECK(editor->CustomShows() == std::vector<PresentationCustomShow>{show});
    }

    TEST_CASE("moving a slide reorders section membership and preserves custom-show playback order [unit] [powerpoint] [section-custom-show]")
    {
        auto editor = PowerPointDocumentEditor::CreateNew();
        auto first = editor->AddSlide();
        auto second = editor->AddSlide();
        auto third = editor->AddSlide();
        REQUIRE(first);
        REQUIRE(second);
        REQUIRE(third);
        REQUIRE(editor->AddSection({"section", "All", {first->Id(), second->Id(), third->Id()}}));
        PresentationCustomShow show{5, "Sequence", {second->Id(), first->Id(), third->Id()}};
        REQUIRE(editor->AddCustomShow(show));

        REQUIRE(editor->MoveSlide(2, 0));
        REQUIRE(editor->Sections().size() == 1);
        CHECK(editor->Sections()[0].SlideIds ==
              std::vector<ExyokiOffice::UInt32>{third->Id(), first->Id(), second->Id()});
        CHECK(editor->CustomShows() == std::vector<PresentationCustomShow>{show});

        auto reopened = PowerPointDocumentEditor::Open(editor->SaveToMemory());
        REQUIRE(reopened);
        CHECK(reopened->Sections()[0].SlideIds ==
              std::vector<ExyokiOffice::UInt32>{third->Id(), first->Id(), second->Id()});
        CHECK(reopened->CustomShows() == std::vector<PresentationCustomShow>{show});
    }

    TEST_CASE("removing slides updates both collections and removes empty entries [unit] [powerpoint] [section-custom-show]")
    {
        auto editor = PowerPointDocumentEditor::CreateNew();
        auto first = editor->AddSlide();
        auto second = editor->AddSlide();
        auto third = editor->AddSlide();
        REQUIRE(first);
        REQUIRE(second);
        REQUIRE(third);
        REQUIRE(editor->AddSection({"main", "Main", {first->Id(), second->Id()}}));
        REQUIRE(editor->AddSection({"last", "Last", {third->Id()}}));
        REQUIRE(editor->AddCustomShow({1, "Mixed", {third->Id(), second->Id(), first->Id()}}));
        REQUIRE(editor->AddCustomShow({2, "Only third", {third->Id()}}));

        REQUIRE(editor->RemoveSlide(2));
        REQUIRE(editor->Sections().size() == 1);
        CHECK(editor->Sections()[0].SlideIds == std::vector<ExyokiOffice::UInt32>{first->Id(), second->Id()});
        REQUIRE(editor->CustomShows().size() == 1);
        CHECK(editor->CustomShows()[0].SlideIds == std::vector<ExyokiOffice::UInt32>{second->Id(), first->Id()});

        REQUIRE(editor->RemoveSlide(0));
        CHECK(editor->Sections()[0].SlideIds == std::vector<ExyokiOffice::UInt32>{second->Id()});
        CHECK(editor->CustomShows()[0].SlideIds == std::vector<ExyokiOffice::UInt32>{second->Id()});
    }
} // TEST_SUITE("PowerPointSectionCustomShowTests")
