// Copyright (c) 2026 Jakub Melka and Contributors
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "doctest.h"

#include "ExyokiOffice/OpenXmlPackageValidator.hpp"
#include "ExyokiOffice/PowerPoint/PowerPointDocument.hpp"

#include <algorithm>
#include <string_view>

using namespace ExyokiOffice::PowerPoint;

TEST_SUITE("PowerPointNotesCommentsTests")
{
    TEST_CASE("notes add edit remove and escaped text round trip [unit] [powerpoint] [notes]")
    {
        auto editor = PowerPointDocumentEditor::CreateNew();
        auto slide = editor->AddSlide();
        REQUIRE(slide->SetNotesText("First & <important>\nSecond"));
        CHECK(slide->NotesText() == "First & <important>\nSecond");
        REQUIRE(slide->SetNotesText("Replacement"));

        auto reopened = PowerPointDocumentEditor::Open(editor->SaveToMemory());
        REQUIRE(reopened);
        CHECK(reopened->GetSlide(0)->NotesText() == "Replacement");
        REQUIRE(reopened->GetSlide(0)->RemoveNotes());
        CHECK(reopened->GetSlide(0)->NotesText().empty());
        CHECK_FALSE(reopened->GetSlide(0)->RemoveNotes());
    }

    TEST_CASE("notes page inheritance settings round trip [unit] [powerpoint] [notes]")
    {
        auto editor = PowerPointDocumentEditor::CreateNew();
        auto slide = editor->AddSlide();
        PresentationNotesPage page{"Speaker notes\nSecond paragraph", false, false};
        REQUIRE(slide->SetNotesPage(page));
        CHECK(slide->NotesPage() == page);

        auto reopened = PowerPointDocumentEditor::Open(editor->SaveToMemory());
        REQUIRE(reopened);
        CHECK(reopened->GetSlide(0)->NotesPage() == page);
        REQUIRE(reopened->GetSlide(0)->SetNotesText("Updated"));
        auto updated = reopened->GetSlide(0)->NotesPage();
        REQUIRE(updated);
        CHECK(updated->Text == "Updated");
        CHECK_FALSE(updated->ShowMasterShapes);
        CHECK_FALSE(updated->ShowMasterPlaceholderAnimations);
    }

    TEST_CASE("handout master formatting and page size round trip [unit] [powerpoint] [handouts]")
    {
        auto editor = PowerPointDocumentEditor::CreateNew();
        CHECK_FALSE(editor->HandoutSettings());
        CHECK_FALSE(editor->SetHandoutSettings({}));
        PresentationHandoutSettings settings;
        settings.PageSize = {9144000, 6858000};
        settings.ShowHeader = false;
        settings.ShowFooter = true;
        settings.ShowDateTime = false;
        settings.ShowSlideNumber = true;
        REQUIRE(editor->SetHandoutSettings(settings));
        CHECK(editor->HandoutSettings() == settings);

        auto reopened = PowerPointDocumentEditor::Open(editor->SaveToMemory());
        REQUIRE(reopened);
        CHECK(reopened->HandoutSettings() == settings);
        CHECK(ExyokiOffice::OpenXmlPackageValidator().Validate(*reopened->GetDocument()).IsValid());
        REQUIRE(reopened->RemoveHandoutSettings());
        CHECK_FALSE(reopened->HandoutSettings());
        CHECK_FALSE(reopened->RemoveHandoutSettings());
    }

    TEST_CASE("modern authors comments and replies retain stable ids through round trip [unit] [powerpoint] [comments]")
    {
        auto editor = PowerPointDocumentEditor::CreateNew();
        REQUIRE(editor->AddCommentAuthor({"author-a", "Alice", "AL", "alice@example.test", "test"}));
        REQUIRE(editor->AddCommentAuthor({"author-b", "Bob", "BO", {}, {}}));
        CHECK_FALSE(editor->AddCommentAuthor({"author-a", "Duplicate", {}, {}, {}}));

        PresentationComment comment{"comment-1", "author-a", "Root\ncomment", {120, 340}, {{"reply-1", "author-b", "Reply"}}, PresentationCommentStatus::Resolved};
        auto slide = editor->AddSlide();
        REQUIRE(slide->AddComment(comment));
        CHECK_FALSE(slide->AddComment({"unknown-author", "missing", "Text", {}, {}}));
        CHECK_FALSE(slide->AddComment(comment));
        CHECK_FALSE(editor->RemoveCommentAuthor("author-b"));

        auto reopened = PowerPointDocumentEditor::Open(editor->SaveToMemory());
        REQUIRE(reopened);
        CHECK(reopened->CommentAuthors().size() == 2);
        REQUIRE(reopened->GetSlide(0)->Comments().size() == 1);
        CHECK(reopened->GetSlide(0)->Comments()[0] == comment);

        comment.Text = "Edited";
        comment.Replies.push_back({"reply-2", "author-a", "Follow-up"});
        REQUIRE(reopened->GetSlide(0)->UpdateComment("comment-1", comment));
        REQUIRE(reopened->UpdateCommentAuthor("author-a", {"author-a", "Alice Smith", "AS", {}, {}}));
        auto secondRoundTrip = PowerPointDocumentEditor::Open(reopened->SaveToMemory());
        REQUIRE(secondRoundTrip);
        CHECK(secondRoundTrip->GetSlide(0)->Comments()[0] == comment);
        CHECK(secondRoundTrip->CommentAuthors()[0].Name == "Alice Smith");
        CHECK(ExyokiOffice::OpenXmlPackageValidator().Validate(*secondRoundTrip->GetDocument()).IsValid());
    }

    TEST_CASE("comment status changes and presentation-wide ids are preserved [unit] [powerpoint] [comments]")
    {
        auto editor = PowerPointDocumentEditor::CreateNew();
        REQUIRE(editor->AddCommentAuthor({"author", "Author", "A", {}, {}}));
        auto firstSlide = editor->AddSlide();
        auto secondSlide = editor->AddSlide();
        REQUIRE(firstSlide->AddComment(
            {"comment-1", "author", "First", {}, {{"reply-1", "author", "Reply"}}}));

        CHECK_FALSE(secondSlide->AddComment({"comment-1", "author", "Duplicate root", {}, {}}));
        CHECK_FALSE(secondSlide->AddComment({"comment-2", "author", "Duplicate reply", {}, {{"reply-1", "author", "Reply"}}}));
        CHECK_FALSE(secondSlide->AddComment({"reply-1", "author", "Reply id as root", {}, {}}));

        REQUIRE(firstSlide->SetCommentStatus("comment-1", PresentationCommentStatus::Closed));
        CHECK_FALSE(firstSlide->SetCommentStatus("missing", PresentationCommentStatus::Resolved));
        auto reopened = PowerPointDocumentEditor::Open(editor->SaveToMemory());
        REQUIRE(reopened);
        REQUIRE(reopened->GetSlide(0)->Comments().size() == 1);
        CHECK(reopened->GetSlide(0)->Comments()[0].Status == PresentationCommentStatus::Closed);
    }

    TEST_CASE("comment removal cleans empty part and then permits author removal [unit] [powerpoint] [comments]")
    {
        auto editor = PowerPointDocumentEditor::CreateNew();
        REQUIRE(editor->AddCommentAuthor({"author", "Author", "A", {}, {}}));
        auto slide = editor->AddSlide();
        REQUIRE(slide->AddComment({"comment", "author", "Text", {}, {}}));
        CHECK_FALSE(slide->UpdateComment("wrong", {"comment", "author", "Text", {}, {}}));
        REQUIRE(slide->RemoveComment("comment"));
        CHECK(slide->Comments().empty());
        CHECK(slide->GetPart()->GetcommentParts().empty());
        REQUIRE(editor->RemoveCommentAuthor("author"));
        CHECK(editor->CommentAuthors().empty());
    }

    TEST_CASE("a notes slide references its slide and the notes master [unit] [powerpoint] [notes]")
    {
        // PowerPoint reports a notes slide without those two relationships, or a
        // presentation without the notes master entry, as damaged content.
        auto editor = PowerPointDocumentEditor::CreateNew();
        REQUIRE(editor != nullptr);
        auto slide = editor->AddSlide();
        REQUIRE(slide != nullptr);
        REQUIRE(slide->SetNotesText("Speaker notes"));

        auto notes = slide->GetPart()->GetNotesSlidePart();
        REQUIRE(notes != nullptr);
        const auto types = notes->Relationships();
        const auto hasType = [&types](std::string_view suffix)
        {
            return std::any_of(types.begin(), types.end(), [suffix](const auto& relationship)
                               { return relationship.Type.ends_with(suffix); });
        };
        CHECK(hasType("/slide"));
        CHECK(hasType("/notesMaster"));

        auto presentationPart = editor->GetDocument()->GetPresentationPart();
        REQUIRE(presentationPart != nullptr);
        CHECK(presentationPart->GetNotesMasterPart() != nullptr);
        CHECK(presentationPart->GetXmlString().find("notesMasterIdLst") != std::string::npos);

        // A second notes slide reuses the same notes master.
        auto second = editor->AddSlide();
        REQUIRE(second != nullptr);
        REQUIRE(second->SetNotesText("More notes"));
        CHECK(editor->GetDocument()->GetPresentationPart()->GetNotesMasterPart() != nullptr);

        auto reopened = PowerPointDocumentEditor::Open(editor->SaveToMemory());
        REQUIRE(reopened != nullptr);
        REQUIRE(reopened->SlideCount() == 2);
        CHECK(reopened->GetSlide(0)->NotesText() == "Speaker notes");
        CHECK(reopened->GetSlide(1)->NotesText() == "More notes");
    }

} // TEST_SUITE("PowerPointNotesCommentsTests")
