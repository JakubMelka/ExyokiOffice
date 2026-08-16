// Copyright (c) 2026 Jakub Melka and Contributors
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "doctest.h"

#include "ExyokiOffice/Word/WordDocument.hpp"

#include "ExyokiOffice/DOM/DocumentFormat/OpenXml/Office2013/Word.hpp"
#include "ExyokiOffice/DOM/DocumentFormat/OpenXml/Office2019/Word/Cid.hpp"
#include "ExyokiOffice/DOM/DocumentFormat/OpenXml/Office2021/Word/CommentsExt.hpp"
#include "ExyokiOffice/DOM/DocumentFormat/OpenXml/Wordprocessing.hpp"
#include "ExyokiOffice/OpenXmlPackage.hpp"
#include "ExyokiOffice/Tools/ValidationRunner.hpp"

#include <algorithm>
#include <cstddef>
#include <string>
#include <vector>

namespace
{
using ExyokiOffice::Word::Comment;
using ExyokiOffice::Word::CommentAuthor;
using ExyokiOffice::Word::ContentControl;
using ExyokiOffice::Word::ContentControlLevel;
using ExyokiOffice::Word::NoteEntryType;
using ExyokiOffice::Word::NoteKind;
using ExyokiOffice::Word::Paragraph;
using ExyokiOffice::Word::Run;
using ExyokiOffice::Word::WordDocumentEditor;

using LockingValues = ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::LockingValues;
using CommentEx = ExyokiOffice::DocumentFormat::OpenXml::Office2013::Word::CommentEx;
using CommentId = ExyokiOffice::DocumentFormat::OpenXml::Office2019::Word::Cid::CommentId;
using CommentExtensible = ExyokiOffice::DocumentFormat::OpenXml::Office2021::Word::CommentsExt::CommentExtensible;
using DomParagraph = ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::Paragraph;

std::shared_ptr<Run> AddPlainRun(const std::shared_ptr<Paragraph>& paragraph, std::string_view text)
{
    auto run = paragraph->AddRun();
    if (run)
    {
        run->AddText(text);
    }
    return run;
}

ExyokiOffice::UInt32 ReadHexId(const ExyokiOffice::HexBinaryValue& value)
{
    if (!value.IsDefined())
    {
        return 0;
    }
    ExyokiOffice::UInt32 result = 0;
    for (const auto byte : value.Value())
    {
        result = (result << 8) | static_cast<ExyokiOffice::UInt32>(byte);
    }
    return result;
}

/// The thread key of a comment: the w14:paraId of its last paragraph.
ExyokiOffice::UInt32 ThreadParaId(const std::shared_ptr<Comment>& comment)
{
    if (!comment || !comment->GetLowLevelApi())
    {
        return 0;
    }
    auto paragraphs = comment->GetLowLevelApi()->Elements<DomParagraph>();
    return paragraphs.empty() ? 0 : ReadHexId(paragraphs.back()->GetParagraphId());
}

std::vector<std::shared_ptr<CommentEx>> CommentExRows(const std::shared_ptr<WordDocumentEditor>& editor)
{
    std::vector<std::shared_ptr<CommentEx>> rows;
    auto mainPart = editor->GetDocument()->GetMainDocumentPart();
    auto part = mainPart ? mainPart->GetWordprocessingCommentsExPart() : nullptr;
    auto root = part ? part->GetTypedRootElement() : nullptr;
    if (!root)
    {
        return rows;
    }
    for (const auto& row : root->Elements<CommentEx>())
    {
        if (row)
        {
            rows.push_back(row);
        }
    }
    return rows;
}

std::vector<std::shared_ptr<CommentId>> CommentIdRows(const std::shared_ptr<WordDocumentEditor>& editor)
{
    std::vector<std::shared_ptr<CommentId>> rows;
    auto mainPart = editor->GetDocument()->GetMainDocumentPart();
    auto part = mainPart ? mainPart->GetWordprocessingCommentsIdsPart() : nullptr;
    auto root = part ? part->GetTypedRootElement() : nullptr;
    if (!root)
    {
        return rows;
    }
    for (const auto& row : root->Elements<CommentId>())
    {
        if (row)
        {
            rows.push_back(row);
        }
    }
    return rows;
}

std::vector<std::shared_ptr<CommentExtensible>> CommentExtensibleRows(const std::shared_ptr<WordDocumentEditor>& editor)
{
    std::vector<std::shared_ptr<CommentExtensible>> rows;
    auto mainPart = editor->GetDocument()->GetMainDocumentPart();
    auto part = mainPart ? mainPart->GetWordCommentsExtensiblePart() : nullptr;
    auto root = part ? part->GetTypedRootElement() : nullptr;
    if (!root)
    {
        return rows;
    }
    for (const auto& row : root->Elements<CommentExtensible>())
    {
        if (row)
        {
            rows.push_back(row);
        }
    }
    return rows;
}

/// Counts `<w:commentRangeStart` occurrences in serialized main-document markup.
std::size_t CountOccurrences(const std::string& haystack, std::string_view needle)
{
    std::size_t count = 0;
    for (std::size_t position = haystack.find(needle, 0); position != std::string::npos;
         position = haystack.find(needle, position + needle.size()))
    {
        ++count;
    }
    return count;
}
} // namespace

// ---------------------------------------------------------------------------
// Footnotes / endnotes
// ---------------------------------------------------------------------------

TEST_SUITE("WordNotesCommentsContentControlTests")
{

    TEST_CASE("Paragraph::AddFootnote creates the reference marker, the entry, and the separator bookkeeping [unit] [word] [word-notes-comments-content-control]")
    {
        auto editor = WordDocumentEditor::CreateNew();
        REQUIRE(editor != nullptr);

        auto paragraph = editor->AddParagraph("See the note below");
        REQUIRE(paragraph != nullptr);

        auto note = paragraph->AddFootnote("Explanatory text.");
        REQUIRE(note != nullptr);
        CHECK(note->Kind() == NoteKind::Footnote);
        CHECK(note->GetEntryType() == NoteEntryType::Normal);
        CHECK(note->PlainText() == "Explanatory text.");
        CHECK(note->GetId() >= 1);

        // Word expects a Separator (-1) and ContinuationSeparator (0) entry alongside real content.
        auto all = editor->Footnotes();
        REQUIRE(all.size() == 3);

        int separatorCount = 0;
        int continuationCount = 0;
        int normalCount = 0;
        for (const auto& entry : all)
        {
            switch (entry->GetEntryType())
            {
                case NoteEntryType::Separator:
                    ++separatorCount;
                    break;
                case NoteEntryType::ContinuationSeparator:
                    ++continuationCount;
                    break;
                case NoteEntryType::Normal:
                    ++normalCount;
                    break;
                default:
                    break;
            }
        }
        CHECK(separatorCount == 1);
        CHECK(continuationCount == 1);
        CHECK(normalCount == 1);

        auto found = editor->FindFootnote(note->GetId());
        REQUIRE(found != nullptr);
        CHECK(found->PlainText() == "Explanatory text.");
    }

    TEST_CASE("Multiple footnotes receive sequential, unique IDs [unit] [word] [word-notes-comments-content-control]")
    {
        auto editor = WordDocumentEditor::CreateNew();
        REQUIRE(editor != nullptr);
        auto paragraph = editor->AddParagraph("Text");
        REQUIRE(paragraph != nullptr);

        auto first = paragraph->AddFootnote("First note.");
        auto second = paragraph->AddFootnote("Second note.");
        REQUIRE(first != nullptr);
        REQUIRE(second != nullptr);

        CHECK(first->GetId() != second->GetId());
        CHECK(second->GetId() == first->GetId() + 1);
    }

    TEST_CASE("Footnotes and endnotes round-trip through save and open [unit] [word] [word-notes-comments-content-control]")
    {
        auto editor = WordDocumentEditor::CreateNew();
        REQUIRE(editor != nullptr);
        auto paragraph = editor->AddParagraph("Body text");
        REQUIRE(paragraph != nullptr);

        auto footnote = paragraph->AddFootnote("Footnote body.");
        auto endnote = paragraph->AddEndnote("Endnote body.");
        REQUIRE(footnote != nullptr);
        REQUIRE(endnote != nullptr);

        auto reopened = WordDocumentEditor::Open(editor->SaveToMemory());
        REQUIRE(reopened != nullptr);

        auto reopenedFootnotes = reopened->Footnotes();
        auto reopenedEndnotes = reopened->Endnotes();

        // Separator + ContinuationSeparator + one real entry each.
        REQUIRE(reopenedFootnotes.size() == 3);
        REQUIRE(reopenedEndnotes.size() == 3);

        auto reopenedFootnote = reopened->FindFootnote(footnote->GetId());
        auto reopenedEndnote = reopened->FindEndnote(endnote->GetId());
        REQUIRE(reopenedFootnote != nullptr);
        REQUIRE(reopenedEndnote != nullptr);
        CHECK(reopenedFootnote->PlainText() == "Footnote body.");
        CHECK(reopenedEndnote->PlainText() == "Endnote body.");

        auto reopenedParagraphs = reopened->Paragraphs();
        REQUIRE(reopenedParagraphs.size() == 1);
        CHECK(reopenedParagraphs.front()->PlainText() == "Body text");
    }

    TEST_CASE("Note::SetText replaces content while keeping the reference mark [unit] [word] [word-notes-comments-content-control]")
    {
        auto editor = WordDocumentEditor::CreateNew();
        REQUIRE(editor != nullptr);
        auto paragraph = editor->AddParagraph("Body");
        REQUIRE(paragraph != nullptr);

        auto note = paragraph->AddFootnote("Original text.");
        REQUIRE(note != nullptr);
        CHECK(note->PlainText() == "Original text.");

        note->SetText("Replaced text.");
        CHECK(note->PlainText() == "Replaced text.");
        REQUIRE(note->Paragraphs().size() == 1);

        // The reference mark run must still be present as the first run of the note paragraph.
        auto noteParagraph = note->Paragraphs().front();
        auto runs = noteParagraph->Runs();
        REQUIRE(runs.size() >= 1);
    }

    TEST_CASE("Note::Remove deletes the reference marker in the body and the entry in the notes part [unit] [word] [word-notes-comments-content-control]")
    {
        auto editor = WordDocumentEditor::CreateNew();
        REQUIRE(editor != nullptr);
        auto paragraph = editor->AddParagraph("Body text");
        REQUIRE(paragraph != nullptr);

        auto note = paragraph->AddFootnote("Removable note.");
        REQUIRE(note != nullptr);
        const auto id = note->GetId();

        CHECK(editor->FindFootnote(id) != nullptr);
        note->Remove();
        CHECK(editor->FindFootnote(id) == nullptr);

        // The reference run is gone; only the original text remains.
        CHECK(paragraph->PlainText() == "Body text");

        auto reopened = WordDocumentEditor::Open(editor->SaveToMemory());
        REQUIRE(reopened != nullptr);
        CHECK(reopened->FindFootnote(id) == nullptr);
        auto reopenedParagraphs = reopened->Paragraphs();
        REQUIRE(reopenedParagraphs.size() == 1);
        CHECK(reopenedParagraphs.front()->PlainText() == "Body text");
    }

    TEST_CASE("WordDocumentEditor::Footnotes returns an empty vector without a footnotes part [unit] [word] [word-notes-comments-content-control]")
    {
        auto editor = WordDocumentEditor::CreateNew();
        REQUIRE(editor != nullptr);
        CHECK(editor->Footnotes().empty());
        CHECK(editor->Endnotes().empty());
        CHECK(editor->FindFootnote(1) == nullptr);
    }

    // ---------------------------------------------------------------------------
    // Comments
    // ---------------------------------------------------------------------------

    TEST_CASE("Paragraph::AddCommentOnParagraph creates a comment wrapping the whole paragraph [unit] [word] [word-notes-comments-content-control]")
    {
        auto editor = WordDocumentEditor::CreateNew();
        REQUIRE(editor != nullptr);
        auto paragraph = editor->AddParagraph("Please review this section.");
        REQUIRE(paragraph != nullptr);

        CommentAuthor author;
        author.Name = "Reviewer";
        author.Initials = "RV";
        auto comment = paragraph->AddCommentOnParagraph("Needs a citation.", author);
        REQUIRE(comment != nullptr);

        CHECK(comment->GetAuthor() == "Reviewer");
        CHECK(comment->GetInitials() == "RV");
        CHECK(comment->PlainText() == "Needs a citation.");
        CHECK(comment->GetDate().has_value());

        auto all = editor->Comments();
        REQUIRE(all.size() == 1);
        CHECK(all.front()->GetId() == comment->GetId());

        auto found = editor->FindComment(comment->GetId());
        REQUIRE(found != nullptr);
        CHECK(found->GetAuthor() == "Reviewer");

        auto paragraphComments = paragraph->Comments();
        REQUIRE(paragraphComments.size() == 1);
        CHECK(paragraphComments.front()->GetId() == comment->GetId());
    }

    TEST_CASE("Paragraph::AddComment wraps only the selected runs, not the whole paragraph [unit] [word] [word-notes-comments-content-control]")
    {
        auto editor = WordDocumentEditor::CreateNew();
        REQUIRE(editor != nullptr);
        auto paragraph = editor->AddParagraph();
        REQUIRE(paragraph != nullptr);

        AddPlainRun(paragraph, "Before ");
        auto middle = AddPlainRun(paragraph, "commented");
        AddPlainRun(paragraph, " after");
        REQUIRE(paragraph->PlainText() == "Before commented after");

        std::vector<std::shared_ptr<Run>> commentedRuns{middle};
        auto comment = paragraph->AddComment(commentedRuns, "Just this word.");
        REQUIRE(comment != nullptr);

        // Comment insertion must not alter the visible paragraph text.
        CHECK(paragraph->PlainText() == "Before commented after");
        CHECK(comment->PlainText() == "Just this word.");

        auto reopened = WordDocumentEditor::Open(editor->SaveToMemory());
        REQUIRE(reopened != nullptr);
        auto reopenedParagraphs = reopened->Paragraphs();
        REQUIRE(reopenedParagraphs.size() == 1);
        CHECK(reopenedParagraphs.front()->PlainText() == "Before commented after");

        auto reopenedComments = reopened->Comments();
        REQUIRE(reopenedComments.size() == 1);
        CHECK(reopenedComments.front()->PlainText() == "Just this word.");
    }

    TEST_CASE("Comments round-trip through save and open with author metadata intact [unit] [word] [word-notes-comments-content-control]")
    {
        auto editor = WordDocumentEditor::CreateNew();
        REQUIRE(editor != nullptr);
        auto paragraph = editor->AddParagraph("Reviewed text.");
        REQUIRE(paragraph != nullptr);

        CommentAuthor author;
        author.Name = "Jane Doe";
        author.Initials = "JD";
        auto comment = paragraph->AddCommentOnParagraph("Looks good.", author);
        REQUIRE(comment != nullptr);
        const auto id = comment->GetId();

        auto reopened = WordDocumentEditor::Open(editor->SaveToMemory());
        REQUIRE(reopened != nullptr);

        auto found = reopened->FindComment(id);
        REQUIRE(found != nullptr);
        CHECK(found->GetAuthor() == "Jane Doe");
        CHECK(found->GetInitials() == "JD");
        CHECK(found->PlainText() == "Looks good.");
    }

    TEST_CASE("Comment::SetText replaces the comment body [unit] [word] [word-notes-comments-content-control]")
    {
        auto editor = WordDocumentEditor::CreateNew();
        REQUIRE(editor != nullptr);
        auto paragraph = editor->AddParagraph("Text");
        REQUIRE(paragraph != nullptr);

        auto comment = paragraph->AddCommentOnParagraph("Original comment.");
        REQUIRE(comment != nullptr);
        comment->SetText("Updated comment.");
        CHECK(comment->PlainText() == "Updated comment.");
        CHECK(comment->Paragraphs().size() == 1);
    }

    TEST_CASE("A comment without an author still validates [unit] [word] [word-notes-comments-content-control]")
    {
        auto editor = WordDocumentEditor::CreateNew();
        REQUIRE(editor != nullptr);
        auto paragraph = editor->AddParagraph("Reviewed text.");
        REQUIRE(paragraph != nullptr);

        // w:author is a required attribute of w:comment, so leaving it out
        // when no display name was supplied produced an invalid package.
        auto comment = paragraph->AddCommentOnParagraph("Anonymous note.");
        REQUIRE(comment != nullptr);
        CHECK(comment->GetAuthor().empty());

        const auto bytes = editor->SaveToMemory();
        REQUIRE_FALSE(bytes.empty());

        ExyokiOffice::OpenXmlPackage package;
        REQUIRE(package.LoadFromMemory(bytes));

        const auto report = ExyokiOffice::Tools::Run(package);
        std::string firstError;
        for (const auto& issue : report.ValidationIssues)
        {
            if (issue.Severity == ExyokiOffice::ValidationSeverity::Error)
            {
                firstError = issue.Message + " @ " + issue.PartUri;
                break;
            }
        }
        CAPTURE(firstError);
        CHECK(report.ErrorCount == 0);
    }

    TEST_CASE("Comment::Remove deletes range markers, the reference, and the comments-part entry [unit] [word] [word-notes-comments-content-control]")
    {
        auto editor = WordDocumentEditor::CreateNew();
        REQUIRE(editor != nullptr);
        auto paragraph = editor->AddParagraph("Commented text.");
        REQUIRE(paragraph != nullptr);

        auto comment = paragraph->AddCommentOnParagraph("Temporary comment.");
        REQUIRE(comment != nullptr);
        const auto id = comment->GetId();

        CHECK(editor->FindComment(id) != nullptr);
        comment->Remove();
        CHECK(editor->FindComment(id) == nullptr);
        CHECK(paragraph->Comments().empty());
        CHECK(paragraph->PlainText() == "Commented text.");

        auto reopened = WordDocumentEditor::Open(editor->SaveToMemory());
        REQUIRE(reopened != nullptr);
        CHECK(reopened->FindComment(id) == nullptr);
        auto reopenedParagraphs = reopened->Paragraphs();
        REQUIRE(reopenedParagraphs.size() == 1);
        CHECK(reopenedParagraphs.front()->PlainText() == "Commented text.");
    }

    TEST_CASE("A paragraph in a table cell can be commented on [unit] [word] [word-notes-comments-content-control]")
    {
        // Table::Paragraphs() used to drop the part on the way out, so a
        // comment on a table cell was refused for a reason that had nothing to
        // do with the document.
        auto editor = WordDocumentEditor::CreateNew();
        REQUIRE(editor != nullptr);

        auto table = editor->AddTable(1, 1);
        REQUIRE(table != nullptr);
        auto cellParagraphs = table->Paragraphs();
        REQUIRE(cellParagraphs.size() == 1);
        AddPlainRun(cellParagraphs.front(), "Cell text");

        auto comment = cellParagraphs.front()->AddCommentOnParagraph("Looks right.");
        REQUIRE(comment != nullptr);
        CHECK(comment->PlainText() == "Looks right.");
    }

    TEST_CASE("Paragraph::AddComment fails gracefully without a part or with empty runs [unit] [word] [word-notes-comments-content-control]")
    {
        auto editor = WordDocumentEditor::CreateNew();
        REQUIRE(editor != nullptr);

        auto attached = editor->AddParagraph("Body text.");
        REQUIRE(attached != nullptr);

        // Wrapped by hand, so there is no document to add a comments part to.
        Paragraph detached(attached->GetLowLevelApi());
        CHECK(detached.AddCommentOnParagraph("Should fail.") == nullptr);

        std::vector<std::shared_ptr<Run>> empty;
        CHECK(attached->AddComment(empty, "Should also fail.") == nullptr);
    }

    // ---------------------------------------------------------------------------
    // Threaded comments (replies and resolution)
    // ---------------------------------------------------------------------------

    TEST_CASE("Comment::AddReply builds a thread that survives a round trip [unit] [word] [word-notes-comments-content-control]")
    {
        auto editor = WordDocumentEditor::CreateNew();
        REQUIRE(editor != nullptr);
        auto paragraph = editor->AddParagraph("Reviewed text.");
        REQUIRE(paragraph != nullptr);

        CommentAuthor author;
        author.Name = "Jane Doe";
        author.Initials = "JD";
        auto parent = paragraph->AddCommentOnParagraph("Is this the final wording?", author);
        REQUIRE(parent != nullptr);

        CommentAuthor replyAuthor;
        replyAuthor.Name = "John Roe";
        replyAuthor.Initials = "JR";
        auto reply = parent->AddReply("Yes, it is.", replyAuthor);
        REQUIRE(reply != nullptr);
        CHECK(reply->GetId() != parent->GetId());

        REQUIRE(parent->Replies().size() == 1);
        CHECK(parent->Replies().front()->GetId() == reply->GetId());
        REQUIRE(reply->GetParent() != nullptr);
        CHECK(reply->GetParent()->GetId() == parent->GetId());
        CHECK(parent->GetParent() == nullptr);
        CHECK(reply->Replies().empty());

        const auto parentId = parent->GetId();
        const auto replyId = reply->GetId();

        auto reopened = WordDocumentEditor::Open(editor->SaveToMemory());
        REQUIRE(reopened != nullptr);
        CHECK(reopened->Comments().size() == 2);

        auto reopenedParent = reopened->FindComment(parentId);
        auto reopenedReply = reopened->FindComment(replyId);
        REQUIRE(reopenedParent != nullptr);
        REQUIRE(reopenedReply != nullptr);

        auto replies = reopenedParent->Replies();
        REQUIRE(replies.size() == 1);
        CHECK(replies.front()->GetId() == replyId);
        CHECK(replies.front()->PlainText() == "Yes, it is.");

        REQUIRE(reopenedReply->GetParent() != nullptr);
        CHECK(reopenedReply->GetParent()->GetId() == parentId);
        CHECK(reopenedReply->PlainText() == "Yes, it is.");
        CHECK(reopenedReply->GetAuthor() == "John Roe");
        CHECK(reopenedReply->GetInitials() == "JR");
        CHECK(reopenedParent->PlainText() == "Is this the final wording?");
    }

    TEST_CASE("A reply carries its own comment ID and its own body range markers [unit] [word] [word-notes-comments-content-control]")
    {
        auto editor = WordDocumentEditor::CreateNew();
        REQUIRE(editor != nullptr);
        auto paragraph = editor->AddParagraph("Commented sentence.");
        REQUIRE(paragraph != nullptr);

        auto parent = paragraph->AddCommentOnParagraph("Question?");
        REQUIRE(parent != nullptr);
        auto reply = parent->AddReply("Answer.");
        REQUIRE(reply != nullptr);

        const auto parentIdText = std::to_string(parent->GetId());
        const auto replyIdText = std::to_string(reply->GetId());

        const auto xml = editor->GetDocument()->GetMainDocumentPart()->GetXmlString();
        CHECK(CountOccurrences(xml, "<w:commentRangeStart") == 2);
        CHECK(CountOccurrences(xml, "<w:commentRangeEnd") == 2);
        CHECK(CountOccurrences(xml, "<w:commentReference") == 2);
        CHECK(xml.find("<w:commentRangeStart w:id=\"" + parentIdText + "\"") != std::string::npos);
        CHECK(xml.find("<w:commentRangeStart w:id=\"" + replyIdText + "\"") != std::string::npos);
        CHECK(xml.find("<w:commentRangeEnd w:id=\"" + replyIdText + "\"") != std::string::npos);
        CHECK(xml.find("<w:commentReference w:id=\"" + replyIdText + "\"") != std::string::npos);

        // Inserting the reply's markers must not disturb the commented text.
        CHECK(paragraph->PlainText() == "Commented sentence.");

        // The reply covers the same span, so both comments answer for this paragraph.
        CHECK(paragraph->Comments().size() == 2);
    }

    TEST_CASE("commentsExtended links the reply to its parent with positive paragraph IDs [unit] [word] [word-notes-comments-content-control]")
    {
        auto editor = WordDocumentEditor::CreateNew();
        REQUIRE(editor != nullptr);
        auto paragraph = editor->AddParagraph("Reviewed text.");
        REQUIRE(paragraph != nullptr);

        CommentAuthor author;
        author.Name = "Jane Doe";
        auto parent = paragraph->AddCommentOnParagraph("Question?", author);
        REQUIRE(parent != nullptr);
        auto reply = parent->AddReply("Answer.", author);
        REQUIRE(reply != nullptr);

        const auto parentParaId = ThreadParaId(parent);
        const auto replyParaId = ThreadParaId(reply);
        CHECK(parentParaId != 0);
        CHECK(replyParaId != 0);
        CHECK(parentParaId != replyParaId);

        auto rows = CommentExRows(editor);
        REQUIRE(rows.size() == 2);

        std::shared_ptr<CommentEx> parentRow;
        std::shared_ptr<CommentEx> replyRow;
        for (const auto& row : rows)
        {
            // Word ignores a paraId whose high bit is set, so no allocated ID may
            // read as negative and none may be zero.
            const auto paraId = ReadHexId(row->GetParaId());
            CHECK(paraId != 0);
            CHECK((paraId & 0x80000000u) == 0u);

            if (paraId == parentParaId)
            {
                parentRow = row;
            }
            else if (paraId == replyParaId)
            {
                replyRow = row;
            }
        }
        REQUIRE(parentRow != nullptr);
        REQUIRE(replyRow != nullptr);

        // Only the reply is parented, and it points at the parent's thread key.
        CHECK_FALSE(parentRow->GetParaIdParent().IsDefined());
        CHECK(ReadHexId(replyRow->GetParaIdParent()) == parentParaId);
        CHECK_FALSE(parentRow->GetDone().ValueOr(false));
        CHECK_FALSE(replyRow->GetDone().ValueOr(false));

        // commentsIds maps each thread key to a durable ID, and commentsExtensible
        // is keyed by that durable ID rather than by the paraId.
        auto idRows = CommentIdRows(editor);
        REQUIRE(idRows.size() == 2);
        std::vector<ExyokiOffice::UInt32> durableIds;
        for (const auto& row : idRows)
        {
            const auto durableId = ReadHexId(row->GetDurableId());
            CHECK(durableId != 0);
            CHECK((durableId & 0x80000000u) == 0u);
            durableIds.push_back(durableId);
        }
        REQUIRE(durableIds.size() == 2);
        CHECK(durableIds[0] != durableIds[1]);

        auto extensibleRows = CommentExtensibleRows(editor);
        REQUIRE(extensibleRows.size() == 2);
        for (const auto& row : extensibleRows)
        {
            const auto durableId = ReadHexId(row->GetDurableId());
            CHECK(std::find(durableIds.begin(), durableIds.end(), durableId) != durableIds.end());
            CHECK(row->GetDateUtc().IsDefined());
        }

        // The author gets one people entry, shared by both comments.
        auto mainPart = editor->GetDocument()->GetMainDocumentPart();
        REQUIRE(mainPart != nullptr);
        auto peoplePart = mainPart->GetWordprocessingPeoplePart();
        REQUIRE(peoplePart != nullptr);
        auto peopleRoot = peoplePart->GetTypedRootElement();
        REQUIRE(peopleRoot != nullptr);
        auto persons = peopleRoot->Elements<ExyokiOffice::DocumentFormat::OpenXml::Office2013::Word::Person>();
        REQUIRE(persons.size() == 1);
        CHECK(persons.front()->GetAuthor().ToString() == "Jane Doe");
    }

    TEST_CASE("Comment::SetResolved marks the whole thread and round-trips [unit] [word] [word-notes-comments-content-control]")
    {
        auto editor = WordDocumentEditor::CreateNew();
        REQUIRE(editor != nullptr);
        auto paragraph = editor->AddParagraph("Reviewed text.");
        REQUIRE(paragraph != nullptr);

        auto parent = paragraph->AddCommentOnParagraph("Question?");
        REQUIRE(parent != nullptr);
        auto reply = parent->AddReply("Answer.");
        REQUIRE(reply != nullptr);
        auto nested = reply->AddReply("Thanks.");
        REQUIRE(nested != nullptr);

        CHECK_FALSE(parent->IsResolved());
        CHECK_FALSE(reply->IsResolved());
        CHECK_FALSE(nested->IsResolved());

        // Resolving from the middle of the thread still resolves root and leaves.
        reply->SetResolved(true);
        CHECK(parent->IsResolved());
        CHECK(reply->IsResolved());
        CHECK(nested->IsResolved());

        const auto parentId = parent->GetId();
        const auto replyId = reply->GetId();
        const auto nestedId = nested->GetId();

        auto reopened = WordDocumentEditor::Open(editor->SaveToMemory());
        REQUIRE(reopened != nullptr);
        REQUIRE(reopened->FindComment(parentId) != nullptr);
        REQUIRE(reopened->FindComment(replyId) != nullptr);
        REQUIRE(reopened->FindComment(nestedId) != nullptr);
        CHECK(reopened->FindComment(parentId)->IsResolved());
        CHECK(reopened->FindComment(replyId)->IsResolved());
        CHECK(reopened->FindComment(nestedId)->IsResolved());

        // Re-opening the thread clears the whole thread again.
        reopened->FindComment(nestedId)->SetResolved(false);
        CHECK_FALSE(reopened->FindComment(parentId)->IsResolved());
        CHECK_FALSE(reopened->FindComment(replyId)->IsResolved());
        CHECK_FALSE(reopened->FindComment(nestedId)->IsResolved());
    }

    TEST_CASE("Appending a paragraph to a replied-to comment keeps the thread intact [unit] [word] [word-notes-comments-content-control]")
    {
        auto editor = WordDocumentEditor::CreateNew();
        REQUIRE(editor != nullptr);
        auto paragraph = editor->AddParagraph("Reviewed text.");
        REQUIRE(paragraph != nullptr);

        auto parent = paragraph->AddCommentOnParagraph("First line.");
        REQUIRE(parent != nullptr);
        auto reply = parent->AddReply("Reply text.");
        REQUIRE(reply != nullptr);

        const auto originalParaId = ThreadParaId(parent);
        REQUIRE(originalParaId != 0);

        // The thread key travels to the new last paragraph; the previous last
        // paragraph is given a fresh one, so no commentsExtended row goes stale.
        auto added = parent->AddParagraph("Second line.");
        REQUIRE(added != nullptr);
        REQUIRE(parent->Paragraphs().size() == 2);
        CHECK(parent->PlainText() == "First line.Second line.");
        CHECK(ThreadParaId(parent) == originalParaId);

        auto lowLevel = parent->GetLowLevelApi();
        REQUIRE(lowLevel != nullptr);
        auto domParagraphs = lowLevel->Elements<DomParagraph>();
        REQUIRE(domParagraphs.size() == 2);
        const auto firstParaId = ReadHexId(domParagraphs.front()->GetParagraphId());
        CHECK(firstParaId != 0);
        CHECK(firstParaId != originalParaId);
        CHECK((firstParaId & 0x80000000u) == 0u);

        REQUIRE(parent->Replies().size() == 1);
        REQUIRE(reply->GetParent() != nullptr);
        CHECK(reply->GetParent()->GetId() == parent->GetId());
        CHECK(CommentExRows(editor).size() == 2);

        const auto parentId = parent->GetId();
        const auto replyId = reply->GetId();

        auto reopened = WordDocumentEditor::Open(editor->SaveToMemory());
        REQUIRE(reopened != nullptr);
        auto reopenedParent = reopened->FindComment(parentId);
        auto reopenedReply = reopened->FindComment(replyId);
        REQUIRE(reopenedParent != nullptr);
        REQUIRE(reopenedReply != nullptr);
        CHECK(reopenedParent->PlainText() == "First line.Second line.");
        REQUIRE(reopenedParent->Replies().size() == 1);
        CHECK(reopenedParent->Replies().front()->GetId() == replyId);
        REQUIRE(reopenedReply->GetParent() != nullptr);
        CHECK(reopenedReply->GetParent()->GetId() == parentId);
    }

    TEST_CASE("Comment::Clear keeps the thread anchor of a threaded comment [unit] [word] [word-notes-comments-content-control]")
    {
        auto editor = WordDocumentEditor::CreateNew();
        REQUIRE(editor != nullptr);
        auto paragraph = editor->AddParagraph("Reviewed text.");
        REQUIRE(paragraph != nullptr);

        auto parent = paragraph->AddCommentOnParagraph("First wording.");
        REQUIRE(parent != nullptr);
        auto reply = parent->AddReply("Reply text.");
        REQUIRE(reply != nullptr);
        const auto originalParaId = ThreadParaId(parent);

        parent->Clear();
        CHECK(parent->PlainText().empty());
        REQUIRE(parent->Paragraphs().size() == 1);
        CHECK(ThreadParaId(parent) == originalParaId);
        REQUIRE(parent->Replies().size() == 1);

        // SetText stays a single-paragraph operation on a threaded comment.
        parent->SetText("Second wording.");
        CHECK(parent->PlainText() == "Second wording.");
        CHECK(parent->Paragraphs().size() == 1);
        CHECK(ThreadParaId(parent) == originalParaId);
        REQUIRE(parent->Replies().size() == 1);
        CHECK(parent->Replies().front()->GetId() == reply->GetId());
    }

    TEST_CASE("Comment::Remove takes the replies with it and leaves no dangling rows [unit] [word] [word-notes-comments-content-control]")
    {
        auto editor = WordDocumentEditor::CreateNew();
        REQUIRE(editor != nullptr);
        auto paragraph = editor->AddParagraph("Reviewed text.");
        REQUIRE(paragraph != nullptr);

        auto parent = paragraph->AddCommentOnParagraph("Question?");
        REQUIRE(parent != nullptr);
        auto reply = parent->AddReply("Answer.");
        REQUIRE(reply != nullptr);
        auto nested = reply->AddReply("Thanks.");
        REQUIRE(nested != nullptr);

        auto survivor = paragraph->AddCommentOnParagraph("Unrelated comment.");
        REQUIRE(survivor != nullptr);
        const auto survivorId = survivor->GetId();

        REQUIRE(editor->Comments().size() == 4);
        REQUIRE(CommentExRows(editor).size() == 4);

        parent->Remove();

        auto remaining = editor->Comments();
        REQUIRE(remaining.size() == 1);
        CHECK(remaining.front()->GetId() == survivorId);
        CHECK(CommentExRows(editor).size() == 1);
        CHECK(CommentIdRows(editor).size() == 1);
        CHECK(CommentExtensibleRows(editor).size() == 1);
        CHECK(paragraph->PlainText() == "Reviewed text.");

        const auto xml = editor->GetDocument()->GetMainDocumentPart()->GetXmlString();
        CHECK(CountOccurrences(xml, "<w:commentRangeStart") == 1);
        CHECK(CountOccurrences(xml, "<w:commentRangeEnd") == 1);
        CHECK(CountOccurrences(xml, "<w:commentReference") == 1);

        auto reopened = WordDocumentEditor::Open(editor->SaveToMemory());
        REQUIRE(reopened != nullptr);
        REQUIRE(reopened->Comments().size() == 1);
        CHECK(reopened->Comments().front()->GetId() == survivorId);
        CHECK(reopened->Comments().front()->Replies().empty());
        CHECK(reopened->Comments().front()->GetParent() == nullptr);
    }

    TEST_CASE("Merging documents keeps every comment's thread key unique [unit] [word] [word-notes-comments-content-control]")
    {
        // Thread keys are allocated deterministically, so two documents written by
        // this library start from the same value; merging must not let the copied
        // entries shadow the target's own thread.
        auto source = WordDocumentEditor::CreateNew();
        REQUIRE(source != nullptr);
        auto sourceParagraph = source->AddParagraph("Source text.");
        REQUIRE(sourceParagraph != nullptr);
        REQUIRE(sourceParagraph->AddCommentOnParagraph("Source comment.") != nullptr);

        auto target = WordDocumentEditor::CreateNew();
        REQUIRE(target != nullptr);
        auto targetParagraph = target->AddParagraph("Target text.");
        REQUIRE(targetParagraph != nullptr);
        auto targetComment = targetParagraph->AddCommentOnParagraph("Target comment.");
        REQUIRE(targetComment != nullptr);
        auto targetReply = targetComment->AddReply("Target reply.");
        REQUIRE(targetReply != nullptr);
        const auto targetParaId = ThreadParaId(targetComment);

        REQUIRE(target->Body().InsertDocument(*source));

        auto comments = target->Comments();
        REQUIRE(comments.size() == 3);

        std::vector<ExyokiOffice::UInt32> paraIds;
        for (const auto& comment : comments)
        {
            const auto paraId = ThreadParaId(comment);
            CHECK(paraId != 0);
            CHECK(std::find(paraIds.begin(), paraIds.end(), paraId) == paraIds.end());
            paraIds.push_back(paraId);
        }

        // The target's own thread survives the merge untouched.
        CHECK(ThreadParaId(targetComment) == targetParaId);
        REQUIRE(targetComment->Replies().size() == 1);
        CHECK(targetComment->Replies().front()->GetId() == targetReply->GetId());
    }

    TEST_CASE("A document with threaded comments validates without errors [unit] [word] [word-notes-comments-content-control]")
    {
        auto editor = WordDocumentEditor::CreateNew();
        REQUIRE(editor != nullptr);
        auto paragraph = editor->AddParagraph("Reviewed text.");
        REQUIRE(paragraph != nullptr);

        CommentAuthor author;
        author.Name = "Jane Doe";
        author.Initials = "JD";
        auto parent = paragraph->AddCommentOnParagraph("Question?", author);
        REQUIRE(parent != nullptr);
        auto reply = parent->AddReply("Answer.", author);
        REQUIRE(reply != nullptr);
        REQUIRE(reply->AddReply("Thanks.") != nullptr);
        parent->AddParagraph("A second line.");
        parent->SetResolved(true);

        const auto bytes = editor->SaveToMemory();
        REQUIRE_FALSE(bytes.empty());

        ExyokiOffice::OpenXmlPackage package;
        REQUIRE(package.LoadFromMemory(bytes));

        const auto report = ExyokiOffice::Tools::Run(package);
        std::string firstError;
        for (const auto& issue : report.ValidationIssues)
        {
            if (issue.Severity == ExyokiOffice::ValidationSeverity::Error)
            {
                firstError = issue.Message + " @ " + issue.PartUri;
                break;
            }
        }
        CAPTURE(firstError);
        CHECK(report.Loaded);
        CHECK(report.ErrorCount == 0);
    }

    // ---------------------------------------------------------------------------
    // Content controls (structured document tags)
    // ---------------------------------------------------------------------------

    TEST_CASE("BodyCursor::InsertContentControl creates a block-level content control with an ID [unit] [word] [word-notes-comments-content-control]")
    {
        auto editor = WordDocumentEditor::CreateNew();
        REQUIRE(editor != nullptr);

        auto control = editor->Body().InsertContentControl("customerName", "Customer Name");
        REQUIRE(control != nullptr);
        CHECK(control->IsBlock());
        CHECK_FALSE(control->IsInline());
        CHECK(control->GetId() >= 1);
        CHECK(control->GetTag() == "customerName");
        CHECK(control->GetAlias() == "Customer Name");

        control->SetText("Acme Corp.");
        CHECK(control->PlainText() == "Acme Corp.");
        REQUIRE(control->Paragraphs().size() == 1);

        // Inline-only accessors are inert for a block-level control.
        CHECK(control->Runs().empty());
        CHECK(control->AddRun() == nullptr);
    }

    TEST_CASE("Paragraph::AddInlineContentControl creates a run-level content control with an ID [unit] [word] [word-notes-comments-content-control]")
    {
        auto editor = WordDocumentEditor::CreateNew();
        REQUIRE(editor != nullptr);
        auto paragraph = editor->AddParagraph("Status: ");
        REQUIRE(paragraph != nullptr);

        auto control = paragraph->AddInlineContentControl("status", "Status");
        REQUIRE(control != nullptr);
        CHECK(control->IsInline());
        CHECK_FALSE(control->IsBlock());
        CHECK(control->GetTag() == "status");
        CHECK(control->GetAlias() == "Status");

        control->SetText("Draft");
        CHECK(control->PlainText() == "Draft");
        REQUIRE(control->Runs().size() == 1);

        // Block-only accessors are inert for an inline control.
        CHECK(control->Paragraphs().empty());
        CHECK(control->AddParagraph("x") == nullptr);

        CHECK(paragraph->PlainText() == "Status: Draft");
    }

    TEST_CASE("Content control IDs are unique across the document [unit] [word] [word-notes-comments-content-control]")
    {
        auto editor = WordDocumentEditor::CreateNew();
        REQUIRE(editor != nullptr);

        auto blockControl = editor->Body().InsertContentControl();
        REQUIRE(blockControl != nullptr);

        auto paragraph = editor->AddParagraph();
        REQUIRE(paragraph != nullptr);
        auto inlineControl = paragraph->AddInlineContentControl();
        REQUIRE(inlineControl != nullptr);

        CHECK(blockControl->GetId() != inlineControl->GetId());
    }

    TEST_CASE("ContentControl lock and placeholder metadata can be set, read, and cleared [unit] [word] [word-notes-comments-content-control]")
    {
        auto editor = WordDocumentEditor::CreateNew();
        REQUIRE(editor != nullptr);
        auto control = editor->Body().InsertContentControl();
        REQUIRE(control != nullptr);

        CHECK_FALSE(control->GetLock().has_value());
        control->SetLock(LockingValues::SdtContentLocked);
        REQUIRE(control->GetLock().has_value());
        CHECK(control->GetLock().value().GetValue() == LockingValues::SdtContentLocked);
        control->ClearLock();
        CHECK_FALSE(control->GetLock().has_value());

        CHECK_FALSE(control->IsShowingPlaceholder());
        control->SetShowingPlaceholder(true);
        CHECK(control->IsShowingPlaceholder());
        control->SetShowingPlaceholder(false);
        CHECK_FALSE(control->IsShowingPlaceholder());
    }

    TEST_CASE("Content controls round-trip through save and open, preserving level and metadata [unit] [word] [word-notes-comments-content-control]")
    {
        auto editor = WordDocumentEditor::CreateNew();
        REQUIRE(editor != nullptr);

        auto blockControl = editor->Body().InsertContentControl("blockTag", "Block Alias");
        REQUIRE(blockControl != nullptr);
        blockControl->SetText("Block content");

        auto paragraph = editor->AddParagraph("Inline: ");
        REQUIRE(paragraph != nullptr);
        auto inlineControl = paragraph->AddInlineContentControl("inlineTag", "Inline Alias");
        REQUIRE(inlineControl != nullptr);
        inlineControl->SetText("value");

        auto reopened = WordDocumentEditor::Open(editor->SaveToMemory());
        REQUIRE(reopened != nullptr);

        auto controls = reopened->ContentControls();
        REQUIRE(controls.size() == 2);

        auto reopenedBlock = reopened->FindContentControl(blockControl->GetId());
        auto reopenedInline = reopened->FindContentControl(inlineControl->GetId());
        REQUIRE(reopenedBlock != nullptr);
        REQUIRE(reopenedInline != nullptr);

        CHECK(reopenedBlock->IsBlock());
        CHECK(reopenedBlock->GetTag() == "blockTag");
        CHECK(reopenedBlock->GetAlias() == "Block Alias");
        CHECK(reopenedBlock->PlainText() == "Block content");

        CHECK(reopenedInline->IsInline());
        CHECK(reopenedInline->GetTag() == "inlineTag");
        CHECK(reopenedInline->GetAlias() == "Inline Alias");
        CHECK(reopenedInline->PlainText() == "value");
    }

    TEST_CASE("WordDocumentEditor::BodyBlocks reports a block-level content control [unit] [word] [word-notes-comments-content-control]")
    {
        auto editor = WordDocumentEditor::CreateNew();
        REQUIRE(editor != nullptr);
        editor->AddParagraph("Before");
        auto control = editor->Body().InsertContentControl("tag");
        REQUIRE(control != nullptr);
        control->SetText("Contents");
        editor->AddParagraph("After");

        auto blocks = editor->BodyBlocks();
        REQUIRE(blocks.size() == 3);
        CHECK(blocks[0].Type() == ExyokiOffice::Word::BodyBlockType::Paragraph);
        CHECK(blocks[1].Type() == ExyokiOffice::Word::BodyBlockType::ContentControl);
        CHECK(blocks[2].Type() == ExyokiOffice::Word::BodyBlockType::Paragraph);

        auto asControl = blocks[1].AsContentControl();
        REQUIRE(asControl != nullptr);
        CHECK(asControl->GetTag() == "tag");
        CHECK(asControl->PlainText() == "Contents");
    }

    TEST_CASE("Paragraph::ContentControls enumerates only inline content controls in that paragraph [unit] [word] [word-notes-comments-content-control]")
    {
        auto editor = WordDocumentEditor::CreateNew();
        REQUIRE(editor != nullptr);
        auto paragraph = editor->AddParagraph();
        REQUIRE(paragraph != nullptr);

        auto controlA = paragraph->AddInlineContentControl("a");
        auto controlB = paragraph->AddInlineContentControl("b");
        REQUIRE(controlA != nullptr);
        REQUIRE(controlB != nullptr);

        auto blockControl = editor->Body().InsertContentControl("blockOnly");
        REQUIRE(blockControl != nullptr);

        auto inlineControls = paragraph->ContentControls();
        REQUIRE(inlineControls.size() == 2);
        CHECK(inlineControls[0]->GetTag() == "a");
        CHECK(inlineControls[1]->GetTag() == "b");
    }

    TEST_CASE("ContentControl::Remove deletes the whole control including its content [unit] [word] [word-notes-comments-content-control]")
    {
        auto editor = WordDocumentEditor::CreateNew();
        REQUIRE(editor != nullptr);
        auto control = editor->Body().InsertContentControl("removable");
        REQUIRE(control != nullptr);
        control->SetText("Doomed content");
        const auto id = control->GetId();

        REQUIRE(editor->ContentControls().size() == 1);
        control->Remove();
        CHECK(editor->ContentControls().empty());
        CHECK(editor->FindContentControl(id) == nullptr);

        auto reopened = WordDocumentEditor::Open(editor->SaveToMemory());
        REQUIRE(reopened != nullptr);
        CHECK(reopened->ContentControls().empty());
    }

    TEST_CASE("ContentControl::Clear empties content without removing the control [unit] [word] [word-notes-comments-content-control]")
    {
        auto editor = WordDocumentEditor::CreateNew();
        REQUIRE(editor != nullptr);
        auto control = editor->Body().InsertContentControl("clearable");
        REQUIRE(control != nullptr);
        control->SetText("Some text");
        REQUIRE(control->PlainText() == "Some text");

        control->Clear();
        CHECK(control->PlainText().empty());
        CHECK(control->Paragraphs().empty());
        CHECK(editor->FindContentControl(control->GetId()) != nullptr);
    }

} // TEST_SUITE("WordNotesCommentsContentControlTests")
