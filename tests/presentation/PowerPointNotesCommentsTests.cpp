// Copyright (c) 2026 Jakub Melka and Contributors
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "doctest.h"

#include "ExyokiOffice/OpenXmlPackageValidator.hpp"
#include "ExyokiOffice/PowerPoint/PowerPointDocument.hpp"

#include <algorithm>
#include <string_view>

using namespace ExyokiOffice::PowerPoint;

// PowerPoint reads comment, reply and author identifiers as GUIDs, so the API
// accepts only braced GUIDs; the tests use recognisable fixed ones.

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
        CHECK(ExyokiOffice::OpenXmlPackageValidator(ExyokiOffice::OpenXmlDomValidationSettings{}).Validate(*reopened->GetDocument()).IsValid());
        REQUIRE(reopened->RemoveHandoutSettings());
        CHECK_FALSE(reopened->HandoutSettings());
        CHECK_FALSE(reopened->RemoveHandoutSettings());
    }

    TEST_CASE("modern authors comments and replies retain stable ids through round trip [unit] [powerpoint] [comments]")
    {
        auto editor = PowerPointDocumentEditor::CreateNew();
        REQUIRE(editor->AddCommentAuthor({"{A0000000-0000-4000-8000-00000000000A}", "Alice", "AL", "alice@example.test", "test"}));
        REQUIRE(editor->AddCommentAuthor({"{B0000000-0000-4000-8000-00000000000B}", "Bob", "BO", {}, {}}));
        CHECK_FALSE(editor->AddCommentAuthor({"{A0000000-0000-4000-8000-00000000000A}", "Duplicate", {}, {}, {}}));

        PresentationComment comment{"{C0000001-0000-4000-8000-000000000001}", "{A0000000-0000-4000-8000-00000000000A}", "Root\ncomment", {120, 340}, {{"{D0000001-0000-4000-8000-000000000001}", "{B0000000-0000-4000-8000-00000000000B}", "Reply"}}, PresentationCommentStatus::Resolved};
        auto slide = editor->AddSlide();
        REQUIRE(slide->AddComment(comment));
        CHECK_FALSE(slide->AddComment({"{E0000000-0000-4000-8000-00000000000E}", "missing", "Text", {}, {}}));
        CHECK_FALSE(slide->AddComment(comment));
        CHECK_FALSE(editor->RemoveCommentAuthor("{B0000000-0000-4000-8000-00000000000B}"));

        auto reopened = PowerPointDocumentEditor::Open(editor->SaveToMemory());
        REQUIRE(reopened);
        CHECK(reopened->CommentAuthors().size() == 2);
        REQUIRE(reopened->GetSlide(0)->Comments().size() == 1);
        CHECK(reopened->GetSlide(0)->Comments()[0] == comment);

        comment.Text = "Edited";
        comment.Replies.push_back({"{D0000002-0000-4000-8000-000000000002}", "{A0000000-0000-4000-8000-00000000000A}", "Follow-up"});
        REQUIRE(reopened->GetSlide(0)->UpdateComment("{C0000001-0000-4000-8000-000000000001}", comment));
        REQUIRE(reopened->UpdateCommentAuthor("{A0000000-0000-4000-8000-00000000000A}", {"{A0000000-0000-4000-8000-00000000000A}", "Alice Smith", "AS", {}, {}}));
        auto secondRoundTrip = PowerPointDocumentEditor::Open(reopened->SaveToMemory());
        REQUIRE(secondRoundTrip);
        CHECK(secondRoundTrip->GetSlide(0)->Comments()[0] == comment);
        CHECK(secondRoundTrip->CommentAuthors()[0].Name == "Alice Smith");
        CHECK(ExyokiOffice::OpenXmlPackageValidator(ExyokiOffice::OpenXmlDomValidationSettings{}).Validate(*secondRoundTrip->GetDocument()).IsValid());
    }

    TEST_CASE("comment status changes and presentation-wide ids are preserved [unit] [powerpoint] [comments]")
    {
        auto editor = PowerPointDocumentEditor::CreateNew();
        REQUIRE(editor->AddCommentAuthor({"{A0000000-0000-4000-8000-000000000001}", "Author", "A", {}, {}}));
        auto firstSlide = editor->AddSlide();
        auto secondSlide = editor->AddSlide();
        REQUIRE(firstSlide->AddComment(
            {"{C0000001-0000-4000-8000-000000000001}", "{A0000000-0000-4000-8000-000000000001}", "First", {}, {{"{D0000001-0000-4000-8000-000000000001}", "{A0000000-0000-4000-8000-000000000001}", "Reply"}}}));

        CHECK_FALSE(secondSlide->AddComment({"{C0000001-0000-4000-8000-000000000001}", "{A0000000-0000-4000-8000-000000000001}", "Duplicate root", {}, {}}));
        CHECK_FALSE(secondSlide->AddComment({"{C0000002-0000-4000-8000-000000000002}", "{A0000000-0000-4000-8000-000000000001}", "Duplicate reply", {}, {{"{D0000001-0000-4000-8000-000000000001}", "{A0000000-0000-4000-8000-000000000001}", "Reply"}}}));
        CHECK_FALSE(secondSlide->AddComment({"{D0000001-0000-4000-8000-000000000001}", "{A0000000-0000-4000-8000-000000000001}", "Reply id as root", {}, {}}));

        REQUIRE(firstSlide->SetCommentStatus("{C0000001-0000-4000-8000-000000000001}", PresentationCommentStatus::Closed));
        CHECK_FALSE(firstSlide->SetCommentStatus("missing", PresentationCommentStatus::Resolved));
        auto reopened = PowerPointDocumentEditor::Open(editor->SaveToMemory());
        REQUIRE(reopened);
        REQUIRE(reopened->GetSlide(0)->Comments().size() == 1);
        CHECK(reopened->GetSlide(0)->Comments()[0].Status == PresentationCommentStatus::Closed);
    }

    TEST_CASE("comment removal cleans empty part and then permits author removal [unit] [powerpoint] [comments]")
    {
        auto editor = PowerPointDocumentEditor::CreateNew();
        REQUIRE(editor->AddCommentAuthor({"{A0000000-0000-4000-8000-000000000001}", "Author", "A", {}, {}}));
        auto slide = editor->AddSlide();
        REQUIRE(slide->AddComment({"{C0000000-0000-4000-8000-000000000001}", "{A0000000-0000-4000-8000-000000000001}", "Text", {}, {}}));
        CHECK_FALSE(slide->UpdateComment("wrong", {"{C0000000-0000-4000-8000-000000000001}", "{A0000000-0000-4000-8000-000000000001}", "Text", {}, {}}));
        REQUIRE(slide->RemoveComment("{C0000000-0000-4000-8000-000000000001}"));
        CHECK(slide->Comments().empty());
        CHECK(slide->GetPart()->GetcommentParts().empty());
        REQUIRE(editor->RemoveCommentAuthor("{A0000000-0000-4000-8000-000000000001}"));
        CHECK(editor->CommentAuthors().empty());
    }

    TEST_CASE("comment and author identifiers must be braced GUIDs [unit] [powerpoint] [comments]")
    {
        // A file with any other identifier is repaired by PowerPoint, which then
        // replaces the values - so the API refuses them up front.
        auto editor = PowerPointDocumentEditor::CreateNew();
        auto slide = editor->AddSlide();
        REQUIRE(slide != nullptr);
        CHECK_FALSE(editor->AddCommentAuthor({"author-1", "Plain", "P", {}, {}}));
        CHECK_FALSE(editor->AddCommentAuthor({"A0000000-0000-4000-8000-00000000000A", "Unbraced", "U", {}, {}}));
        CHECK_FALSE(editor->AddCommentAuthor({"{A0000000-0000-4000-8000-0000000000ZZ}", "Not hex", "N", {}, {}}));
        CHECK(editor->CommentAuthors().empty());

        const std::string author = "{A0000000-0000-4000-8000-00000000000A}";
        REQUIRE(editor->AddCommentAuthor({author, "Alice", "AL", {}, {}}));
        CHECK_FALSE(slide->AddComment({"comment-1", author, "Plain id", {}, {}}));
        CHECK_FALSE(slide->AddComment({"{C0000001-0000-4000-8000-000000000001}", "author-1", "Plain author", {}, {}}));
        CHECK_FALSE(slide->AddComment({"{C0000001-0000-4000-8000-000000000001}", author, "Plain reply", {},
                                       {{"reply-1", author, "Reply"}}}));
        CHECK_FALSE(slide->AddComment({"{C0000001-0000-4000-8000-000000000001}", author, "Plain reply author", {},
                                       {{"{D0000001-0000-4000-8000-000000000001}", "author-1", "Reply"}}}));
        CHECK(slide->Comments().empty());
        CHECK(slide->GetPart()->GetcommentParts().empty());
        // Lower-case digits are still hexadecimal.
        REQUIRE(slide->AddComment({"{c0000001-0000-4000-8000-000000000001}", author, "Lower case", {}, {}}));
        // Reading back never rewrites what was stored.
        CHECK(slide->Comments()[0].Id == "{c0000001-0000-4000-8000-000000000001}");
    }

    TEST_CASE("a comment carries its slide anchor and the slide links the comment part [unit] [powerpoint] [comments]")
    {
        auto editor = PowerPointDocumentEditor::CreateNew();
        const std::string author = "{A0000000-0000-4000-8000-00000000000A}";
        REQUIRE(editor->AddCommentAuthor({author, "Alice", "AL", {}, {}}));
        auto slide = editor->AddSlide();
        REQUIRE(slide != nullptr);
        REQUIRE(slide->AddComment({"{C0000001-0000-4000-8000-000000000001}", author, "Text", {}, {}}));

        // pc:sldMkLst names the slide id; PowerPoint refuses a comment without
        // an anchor and expects the slide-side p188:commentRel extension.
        const auto commentXml = slide->GetPart()->GetcommentParts().front()->GetXmlString();
        CHECK(commentXml.find("sldMkLst") != std::string::npos);
        CHECK(commentXml.find("sldId=\"" + std::to_string(slide->Id()) + "\"") != std::string::npos);
        const auto slideXml = slide->GetPart()->GetXmlString();
        CHECK(slideXml.find("commentRel") != std::string::npos);
        CHECK(slideXml.find("{6950BFC3-D8DA-4A85-94F7-54DA5524770B}") != std::string::npos);

        // Removing the last comment removes the part and the extension with it.
        REQUIRE(slide->RemoveComment("{C0000001-0000-4000-8000-000000000001}"));
        CHECK(slide->GetPart()->GetXmlString().find("commentRel") == std::string::npos);
        CHECK(ExyokiOffice::OpenXmlPackageValidator(ExyokiOffice::OpenXmlDomValidationSettings{}).Validate(*editor->GetDocument()).IsValid());
    }

    TEST_CASE("editing a comment written without the slide-side link adds it [unit] [powerpoint] [comments]")
    {
        // A presentation saved before the p188:commentRel extension was written
        // has the comment part and its relationship, but no slide extension.
        // Any edit of an existing comment brings the slide up to date.
        const std::string author = "{A0000000-0000-4000-8000-00000000000A}";
        const std::string id = "{C0000001-0000-4000-8000-000000000001}";
        auto editor = PowerPointDocumentEditor::CreateNew();
        REQUIRE(editor->AddCommentAuthor({author, "Alice", "AL", {}, {}}));
        auto slide = editor->AddSlide();
        REQUIRE(slide->AddComment({id, author, "Text", {}, {}}));
        auto slideXml = slide->GetPart()->GetXmlString();
        const auto extensionStart = slideXml.find("<p:extLst>");
        REQUIRE(extensionStart != std::string::npos);
        slideXml.erase(extensionStart, slideXml.find("</p:extLst>") + 11 - extensionStart);
        slide->GetPart()->SetXmlString(slideXml);
        REQUIRE(slide->GetPart()->GetXmlString().find("commentRel") == std::string::npos);

        SUBCASE("through UpdateComment")
        {
            REQUIRE(slide->UpdateComment(id, {id, author, "Edited", {}, {}}));
        }
        SUBCASE("through SetCommentStatus")
        {
            REQUIRE(slide->SetCommentStatus(id, PresentationCommentStatus::Resolved));
        }
        CHECK(slide->GetPart()->GetXmlString().find("commentRel") != std::string::npos);
        auto reopened = PowerPointDocumentEditor::Open(editor->SaveToMemory());
        REQUIRE(reopened);
        CHECK(reopened->GetSlide(0)->GetPart()->GetXmlString().find("commentRel") != std::string::npos);
        CHECK(ExyokiOffice::OpenXmlPackageValidator(ExyokiOffice::OpenXmlDomValidationSettings{}).Validate(*reopened->GetDocument()).IsValid());
    }

    TEST_CASE("a rejected comment leaves the document untouched [unit] [powerpoint] [comments]")
    {
        const std::string author = "{A0000000-0000-4000-8000-00000000000A}";
        auto editor = PowerPointDocumentEditor::CreateNew();
        REQUIRE(editor->AddCommentAuthor({author, "Alice", "AL", {}, {}}));
        auto slide = editor->AddSlide();
        REQUIRE(slide->AddComment({"{C0000001-0000-4000-8000-000000000001}", author, "First", {}, {}}));
        const auto slideXml = slide->GetPart()->GetXmlString();
        const auto commentXml = slide->GetPart()->GetcommentParts().front()->GetXmlString();
        const auto relationships = slide->GetPart()->Relationships().size();

        // Duplicate id, unknown author, malformed reply: each is refused before
        // anything is written, so the part, the slide and its relationships stay.
        CHECK_FALSE(slide->AddComment({"{C0000001-0000-4000-8000-000000000001}", author, "Duplicate", {}, {}}));
        CHECK_FALSE(slide->AddComment({"{C0000002-0000-4000-8000-000000000002}", "{E0000000-0000-4000-8000-00000000000E}", "Unknown", {}, {}}));
        CHECK_FALSE(slide->AddComment({"{C0000002-0000-4000-8000-000000000002}", author, "Bad reply", {}, {{"reply", author, "R"}}}));
        CHECK(slide->GetPart()->GetXmlString() == slideXml);
        CHECK(slide->GetPart()->GetcommentParts().front()->GetXmlString() == commentXml);
        CHECK(slide->GetPart()->Relationships().size() == relationships);
        CHECK(slide->Comments().size() == 1);
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
